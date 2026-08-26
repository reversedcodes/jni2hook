#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <windows.h>

#if defined(JNI2HOOK_FAKE_NO_CRT)
BOOL WINAPI DllMainCRTStartup(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
#endif

typedef struct
{
    const char *type_name;
    const char *field_name;
    const char *type_string;
    int32_t is_static;
    uint64_t offset;
    void *address;
} fake_struct_entry;

typedef struct
{
    const char *type_name;
    const char *superclass_name;
    int32_t is_oop;
    int32_t is_integer;
    int32_t is_unsigned;
    uint64_t size;
} fake_type_entry;

typedef struct
{
    const char *name;
    bool *address;
} fake_flag;

__declspec(dllexport) bool fake_flag_value = false;

static fake_flag flag_table[] = {
    { "AllowRedefinitionToAddDeleteMethods", &fake_flag_value }
};
static fake_flag *flags = flag_table;
static size_t num_flags = sizeof(flag_table) / sizeof(flag_table[0]);

static fake_struct_entry struct_table[] = {
    { "JVMFlag", "flags", "JVMFlag*", 1, 0, &flags },
    { "JVMFlag", "numFlags", "size_t", 1, 0, &num_flags },
    { "JVMFlag", "_name", "const char*", 0, offsetof(fake_flag, name), NULL },
    { "JVMFlag", "_addr", "void*", 0, offsetof(fake_flag, address), NULL },
    { NULL, NULL, NULL, 0, 0, NULL }
};

static fake_type_entry type_table[] = {
    { "JVMFlag", NULL, 0, 0, 0, sizeof(fake_flag) },
    { NULL, NULL, 0, 0, 0, 0 }
};

__declspec(dllexport) void *gHotSpotVMStructs = struct_table;
__declspec(dllexport) void *gHotSpotVMTypes = type_table;

__declspec(dllexport) uint64_t gHotSpotVMStructEntryTypeNameOffset =
    offsetof(fake_struct_entry, type_name);
__declspec(dllexport) uint64_t gHotSpotVMStructEntryFieldNameOffset =
    offsetof(fake_struct_entry, field_name);
__declspec(dllexport) uint64_t gHotSpotVMStructEntryTypeStringOffset =
    offsetof(fake_struct_entry, type_string);
__declspec(dllexport) uint64_t gHotSpotVMStructEntryIsStaticOffset =
    offsetof(fake_struct_entry, is_static);
__declspec(dllexport) uint64_t gHotSpotVMStructEntryOffsetOffset =
    offsetof(fake_struct_entry, offset);
__declspec(dllexport) uint64_t gHotSpotVMStructEntryAddressOffset =
    offsetof(fake_struct_entry, address);
__declspec(dllexport) uint64_t gHotSpotVMStructEntryArrayStride = sizeof(fake_struct_entry);

__declspec(dllexport) uint64_t gHotSpotVMTypeEntryTypeNameOffset =
    offsetof(fake_type_entry, type_name);
__declspec(dllexport) uint64_t gHotSpotVMTypeEntrySizeOffset =
    offsetof(fake_type_entry, size);
__declspec(dllexport) uint64_t gHotSpotVMTypeEntryArrayStride = sizeof(fake_type_entry);
