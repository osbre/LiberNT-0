/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the CPU share MMCSS leaves to threads outside its tasks
 * COPYRIGHT:   Copyright 2026 Ostap Brehin <osbre@protonmail.com>
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H
#include <windef.h>
#include <winbase.h>
#include <ndk/ntndk.h>
#include <avrt.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#define MAX_SPINNERS 64

typedef struct _SPINNER
{
    PCWSTR Task;
    ULONG DurationMs;
    ULONG BusyUs;
    ULONG PeriodUs;
    BOOL Yield;
    DWORD_PTR Affinity;
    INT ThreadPriority;
    HANDLE Start;
    LONGLONG HighUs;
    LONGLONG LowUs;
    LONGLONG FirstLowUs;
    LONG LowestPriority;
    LONG HighestLowPriority;
    LONG LowestLowBase;
    LONG HighestLowBase;
    ULONG LowPhases;
    ULONGLONG Iterations;
    ULONGLONG CpuUs;
} SPINNER;

static HANDLE (WINAPI *pAvSetMmThreadCharacteristicsW)(LPCWSTR, LPDWORD);
static BOOL (WINAPI *pAvRevertMmThreadCharacteristics)(HANDLE);
static LARGE_INTEGER Frequency;
static ULONG CpuCount;

static LONGLONG
NowUs(VOID)
{
    LARGE_INTEGER Now;

    QueryPerformanceCounter(&Now);
    return Now.QuadPart * 1000000 / Frequency.QuadPart;
}

static LONG
CurrentPriority(PLONG BasePriority)
{
    THREAD_BASIC_INFORMATION Info;
    NTSTATUS Status;

    Status = NtQueryInformationThread(GetCurrentThread(), ThreadBasicInformation, &Info, sizeof(Info), NULL);
    *BasePriority = NT_SUCCESS(Status) ? Info.BasePriority : -1;
    return NT_SUCCESS(Status) ? Info.Priority : -1;
}

static ULONGLONG
ThreadCpuUs(HANDLE Thread)
{
    FILETIME Create, Exit, Kernel, User;

    if (!GetThreadTimes(Thread, &Create, &Exit, &Kernel, &User))
        return 0;
    return (((ULONGLONG)Kernel.dwHighDateTime << 32 | Kernel.dwLowDateTime) +
            ((ULONGLONG)User.dwHighDateTime << 32 | User.dwLowDateTime)) / 10;
}

static DWORD WINAPI
SpinnerThread(PVOID Parameter)
{
    SPINNER *Spinner = Parameter;
    LONGLONG Start, Now, Last, BusyStart;
    LARGE_INTEGER Due;
    HANDLE Handle = NULL, Timer;
    DWORD Index = 0;
    LONG Priority, Base, Previous = -1;

    Timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (Timer == NULL)
        Timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    ok(Timer != NULL, "no timer: %lu\n", GetLastError());
    Spinner->FirstLowUs = -1;
    Spinner->LowestPriority = MAXLONG;
    Spinner->HighestLowPriority = 0;
    Spinner->LowestLowBase = MAXLONG;
    Spinner->HighestLowBase = 0;
    if (Spinner->Affinity != 0)
        ok(SetThreadAffinityMask(GetCurrentThread(), Spinner->Affinity) != 0, "affinity not set: %lu\n", GetLastError());
    if (Spinner->ThreadPriority != THREAD_PRIORITY_NORMAL)
        ok(SetThreadPriority(GetCurrentThread(), Spinner->ThreadPriority), "priority not set: %lu\n", GetLastError());
    if (Spinner->Task != NULL)
    {
        Handle = pAvSetMmThreadCharacteristicsW(Spinner->Task, &Index);
        ok(Handle != NULL, "%ls: not registered: %lu\n", Spinner->Task, GetLastError());
    }
    WaitForSingleObject(Spinner->Start, INFINITE);
    Start = Last = BusyStart = NowUs();
    do
    {
        Now = NowUs();
        Priority = CurrentPriority(&Base);
        Spinner->Iterations++;
        if (Handle != NULL)
        {
            if (Priority < LOW_REALTIME_PRIORITY)
            {
                if (Previous >= LOW_REALTIME_PRIORITY)
                    Spinner->LowPhases++;
                if (Spinner->FirstLowUs < 0)
                    Spinner->FirstLowUs = Now - Start;
                Spinner->LowUs += Now - Last;
                Spinner->LowestPriority = min(Spinner->LowestPriority, Priority);
                Spinner->HighestLowPriority = max(Spinner->HighestLowPriority, Priority);
                Spinner->LowestLowBase = min(Spinner->LowestLowBase, Base);
                Spinner->HighestLowBase = max(Spinner->HighestLowBase, Base);
            }
            else if (Spinner->FirstLowUs >= 0)
            {
                Spinner->HighUs += Now - Last;
            }
        }
        Previous = Priority;
        Last = Now;
        if (Spinner->Yield)
            SwitchToThread();
        if (Spinner->PeriodUs != 0 && Now - BusyStart >= Spinner->BusyUs && Timer != NULL)
        {
            Due.QuadPart = -(LONGLONG)(Spinner->PeriodUs - (Now - BusyStart)) * 10;
            SetWaitableTimer(Timer, &Due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(Timer, INFINITE);
            BusyStart = Last = NowUs();
        }
    } while (Now - Start < (LONGLONG)Spinner->DurationMs * 1000);
    Spinner->CpuUs = ThreadCpuUs(GetCurrentThread());
    if (Handle)
        ok_int(pAvRevertMmThreadCharacteristics(Handle), TRUE);
    if (Timer)
        CloseHandle(Timer);
    return 0;
}

static VOID
RunSpinners(SPINNER *Spinners, ULONG Count)
{
    HANDLE Threads[MAX_SPINNERS];
    HANDLE Start;
    ULONG i;

    ok(Count <= MAX_SPINNERS, "%lu spinners\n", Count);
    Count = min(Count, MAX_SPINNERS);
    Start = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (i = 0; i < Count; i++)
    {
        Spinners[i].Start = Start;
        Threads[i] = CreateThread(NULL, 0, SpinnerThread, &Spinners[i], 0, NULL);
        ok(Threads[i] != NULL, "CreateThread failed: %lu\n", GetLastError());
    }
    Sleep(50);
    SetEvent(Start);
    for (i = 0; i < Count; i++)
    {
        if (Threads[i])
        {
            ok_long(WaitForSingleObject(Threads[i], 30000), WAIT_OBJECT_0);
            CloseHandle(Threads[i]);
        }
    }
    CloseHandle(Start);
}

static VOID
TestBusyThread(PCWSTR Task)
{
    SPINNER Spinner = { Task, 1500 };
    LONGLONG Share;

    RunSpinners(&Spinner, 1);
    ok(Spinner.FirstLowUs >= 0, "%ls: a thread busy for 1.5 s was never lowered\n", Task);
    if (Spinner.FirstLowUs < 0)
        return;
    ok(Spinner.LowestPriority >= 1 && Spinner.HighestLowPriority <= 7,
       "%ls: lowered to %ld..%ld\n", Task, Spinner.LowestPriority, Spinner.HighestLowPriority);
    Share = Spinner.LowUs * 100 / max(Spinner.LowUs + Spinner.HighUs, 1);
    ok(Spinner.LowPhases >= 20, "%ls: lowered %lu times in 1.5 s\n", Task, Spinner.LowPhases);
    ok(Share <= 35, "%ls: %I64d%% of the time lowered\n", Task, Share);
    trace("%ls: first lowered at %I64d us to %ld..%ld, base %ld..%ld, %I64d%% of the time after, %lu phases, %I64d us lowered, %I64d us raised\n",
          Task, Spinner.FirstLowUs, Spinner.LowestPriority, Spinner.HighestLowPriority,
          Spinner.LowestLowBase, Spinner.HighestLowBase, Share,
          Spinner.LowPhases, Spinner.LowUs, Spinner.HighUs);
}

static VOID
TestYieldingThread(VOID)
{
    SPINNER Spinners[2];

    ZeroMemory(Spinners, sizeof(Spinners));
    Spinners[0].Task = L"Pro Audio";
    Spinners[0].DurationMs = 1500;
    Spinners[0].Yield = TRUE;
    Spinners[0].Affinity = 1;
    Spinners[1].DurationMs = 1500;
    Spinners[1].Affinity = 1;
    Spinners[1].ThreadPriority = THREAD_PRIORITY_IDLE;
    RunSpinners(Spinners, 2);
    trace("yielding: first lowered at %I64d us to %ld..%ld, %lu phases, %I64u iterations; idle-priority thread ran %I64u iterations\n",
          Spinners[0].FirstLowUs, Spinners[0].LowestPriority, Spinners[0].HighestLowPriority, Spinners[0].LowPhases,
          Spinners[0].Iterations, Spinners[1].Iterations);
    ok(Spinners[0].Iterations >= 10000, "a registered thread yielding to an idle-priority thread on its processor ran %I64u iterations\n",
       Spinners[0].Iterations);
}

static VOID
TestHalfBusyThread(VOID)
{
    SPINNER Spinner = { L"Pro Audio", 1500, 5000, 10000 };

    RunSpinners(&Spinner, 1);
    ok(Spinner.FirstLowUs < 0, "a thread busy 5 ms of every 10 ms was lowered at %I64d us to %ld\n",
       Spinner.FirstLowUs, Spinner.LowestPriority);
}

typedef struct _RESET_WHILE_LOWERED
{
    INT ThreadPriority;
    INT ReportedPriority;
    LONG LoweredPriority;
    LONG LowestPriority;
    LONG HighestPriority;
    LONG RevertedBase;
} RESET_WHILE_LOWERED;

static DWORD WINAPI
ResetWhileLoweredThread(PVOID Parameter)
{
    RESET_WHILE_LOWERED *Reset = Parameter;
    LONGLONG Start;
    HANDLE Handle;
    DWORD Index = 0;
    LONG Priority, Base;

    Reset->LoweredPriority = -1;
    Handle = pAvSetMmThreadCharacteristicsW(L"Pro Audio", &Index);
    ok(Handle != NULL, "not registered: %lu\n", GetLastError());
    if (Handle == NULL)
        return 0;

    Start = NowUs();
    do
    {
        Priority = CurrentPriority(&Base);
    } while (Priority >= LOW_REALTIME_PRIORITY && NowUs() - Start < 3000000);
    if (Priority < LOW_REALTIME_PRIORITY)
    {
        Reset->LoweredPriority = Priority;
        ok(SetThreadPriority(GetCurrentThread(), Reset->ThreadPriority), "priority not set: %lu\n", GetLastError());
        Reset->ReportedPriority = GetThreadPriority(GetCurrentThread());
        Reset->LowestPriority = MAXLONG;
        Reset->HighestPriority = 0;
        Start = NowUs();
        do
        {
            Priority = CurrentPriority(&Base);
            Reset->LowestPriority = min(Reset->LowestPriority, Priority);
            Reset->HighestPriority = max(Reset->HighestPriority, Priority);
        } while (NowUs() - Start < 200000);
    }
    ok_int(pAvRevertMmThreadCharacteristics(Handle), TRUE);
    CurrentPriority(&Reset->RevertedBase);
    return 0;
}

static VOID
TestResetWhileLowered(INT ThreadPriority)
{
    RESET_WHILE_LOWERED Reset = { ThreadPriority };
    HANDLE Thread;

    Thread = CreateThread(NULL, 0, ResetWhileLoweredThread, &Reset, 0, NULL);
    ok(Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (Thread == NULL)
        return;
    ok_long(WaitForSingleObject(Thread, 30000), WAIT_OBJECT_0);
    CloseHandle(Thread);
    ok(Reset.LoweredPriority >= 1 && Reset.LoweredPriority < LOW_REALTIME_PRIORITY, "%d: lowered to %ld\n", ThreadPriority, Reset.LoweredPriority);
    if (Reset.LoweredPriority < 0)
        return;
    ok_int(Reset.ReportedPriority, ThreadPriority);
    ok(Reset.HighestPriority == 24, "%d: raised to %ld after the reset\n", ThreadPriority, Reset.HighestPriority);
    ok(Reset.LowestPriority < LOW_REALTIME_PRIORITY, "%d: never lowered after the reset, %ld at the lowest\n", ThreadPriority, Reset.LowestPriority);
    ok_long(Reset.RevertedBase, 0);
}

static VOID
TestContention(PCWSTR Task)
{
    SPINNER Spinners[MAX_SPINNERS];
    ULONGLONG Registered = 0, Normal = 0, Total, RegisteredCpu = 0, NormalCpu = 0;
    ULONG i, Count = min(CpuCount, MAX_SPINNERS / 2);

    ZeroMemory(Spinners, sizeof(Spinners));
    for (i = 0; i < 2 * Count; i++)
    {
        Spinners[i].Task = (i < Count) ? Task : NULL;
        Spinners[i].DurationMs = 2000;
    }
    RunSpinners(Spinners, 2 * Count);
    for (i = 0; i < 2 * Count; i++)
    {
        if (i < Count)
        {
            Registered += Spinners[i].Iterations;
            RegisteredCpu += Spinners[i].CpuUs;
        }
        else
        {
            Normal += Spinners[i].Iterations;
            NormalCpu += Spinners[i].CpuUs;
        }
    }
    Total = max(Registered + Normal, 1);
    ok(Normal * 100 / Total >= 10, "%ls: normal threads ran %I64u of %I64u iterations\n", Task, Normal, Total);
    ok(Registered * 100 / Total >= 50, "%ls: registered threads ran %I64u of %I64u iterations\n", Task, Registered, Total);
    trace("%ls: %lu registered and %lu normal threads busy: %I64u%% of the iterations to the normal ones, cpu %I64u/%I64u us\n",
          Task, Count, Count, Normal * 100 / Total, NormalCpu, RegisteredCpu);
}

START_TEST(SystemResponsiveness)
{
    HMODULE Avrt = LoadLibraryW(L"avrt.dll");
    SYSTEM_INFO Info;

    ok(Avrt != NULL, "avrt.dll not loaded: %lu\n", GetLastError());
    if (!Avrt)
        return;
    pAvSetMmThreadCharacteristicsW = (PVOID)GetProcAddress(Avrt, "AvSetMmThreadCharacteristicsW");
    pAvRevertMmThreadCharacteristics = (PVOID)GetProcAddress(Avrt, "AvRevertMmThreadCharacteristics");
    if (!pAvSetMmThreadCharacteristicsW || !pAvRevertMmThreadCharacteristics)
    {
        ok(0, "avrt export missing\n");
        return;
    }
    QueryPerformanceFrequency(&Frequency);
    GetSystemInfo(&Info);
    CpuCount = Info.dwNumberOfProcessors;

    TestBusyThread(L"Pro Audio");
    TestBusyThread(L"Audio");
    TestBusyThread(L"Games");
    TestYieldingThread();
    TestHalfBusyThread();
    TestResetWhileLowered(THREAD_PRIORITY_LOWEST);
    TestResetWhileLowered(THREAD_PRIORITY_IDLE);
    TestResetWhileLowered(THREAD_PRIORITY_NORMAL);
    TestContention(L"Pro Audio");
    TestContention(L"Games");
}
