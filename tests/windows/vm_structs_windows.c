#include "hook/vm_structs.h"

#include <stdbool.h>
#include <string.h>
#include <windows.h>

#if defined(JNI2HOOK_TEST_NO_CRT)
int main(void);

void mainCRTStartup(void)
{
    ExitProcess((UINT)main());
}
#endif

int main(void)
{
    HMODULE jvm = LoadLibraryA("jvm.dll");
    if (jvm == NULL)
        return 1;

    FARPROC symbol = GetProcAddress(jvm, "fake_flag_value");
    bool *value = NULL;
    memcpy(&value, &symbol, sizeof(value));
    if (value == NULL || *value)
        return 1;

    bool previous = true;
    if (!vm_structs_set_bool_flag("AllowRedefinitionToAddDeleteMethods", true, &previous))
        return 1;
    if (previous || !*value)
        return 1;

    return 0;
}
