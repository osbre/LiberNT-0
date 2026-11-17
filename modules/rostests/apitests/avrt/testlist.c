#define STANDALONE
#include <apitest.h>

extern void func_AvSetMmThreadCharacteristics(void);
extern void func_SystemResponsiveness(void);
extern void func_TaskProfiles(void);

const struct test winetest_testlist[] =
{
    { "AvSetMmThreadCharacteristics", func_AvSetMmThreadCharacteristics },
    { "SystemResponsiveness", func_SystemResponsiveness },
    { "TaskProfiles", func_TaskProfiles },
    { 0, 0 }
};
