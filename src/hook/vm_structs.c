#include "vm_structs.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef struct
{
    const char *type_name;
    const char *field_name;
    const char *type_string;
    int32_t     is_static;
    uint64_t    offset;
    void       *address;
} struct_entry;

static const unsigned char *g_structs = NULL;
static const unsigned char *g_types   = NULL;

static uint64_t g_struct_type_name_offset   = 0;
static uint64_t g_struct_field_name_offset  = 0;
static uint64_t g_struct_type_string_offset = 0;
static uint64_t g_struct_is_static_offset   = 0;
static uint64_t g_struct_offset_offset      = 0;
static uint64_t g_struct_address_offset     = 0;
static uint64_t g_struct_stride             = 0;

static uint64_t g_type_type_name_offset = 0;
static uint64_t g_type_size_offset      = 0;
static uint64_t g_type_stride           = 0;

static bool g_ready = false;

static void *resolve(const char *symbol)
{
#if defined(_WIN32)
    HMODULE jvm = GetModuleHandleA("jvm.dll");
    if (jvm == NULL)
        return NULL;
    FARPROC address = GetProcAddress(jvm, symbol);
    void *result = NULL;
    memcpy(&result, &address, sizeof(result));
    return result;
#else
    void *found = dlsym(RTLD_DEFAULT, symbol);
    if (found != NULL)
        return found;

    void *jvm = dlopen("libjvm.so", RTLD_NOLOAD | RTLD_LAZY);
    return jvm != NULL ? dlsym(jvm, symbol) : NULL;
#endif
}

static bool read_offset(const char *symbol, uint64_t *out)
{
    const uint64_t *value = resolve(symbol);
    if (value == NULL)
        return false;
    *out = *value;
    return true;
}

static struct_entry read_struct_entry(const unsigned char *base)
{
    struct_entry entry;
    entry.type_name   = *(const char *const *)(base + g_struct_type_name_offset);
    entry.field_name  = *(const char *const *)(base + g_struct_field_name_offset);
    entry.type_string = *(const char *const *)(base + g_struct_type_string_offset);
    entry.is_static   = *(const int32_t *)(base + g_struct_is_static_offset);
    entry.offset      = *(const uint64_t *)(base + g_struct_offset_offset);
    entry.address     = *(void *const *)(base + g_struct_address_offset);
    return entry;
}

bool vm_structs_init(void)
{
    if (g_ready)
        return true;

    const void *const *structs = resolve("gHotSpotVMStructs");
    const void *const *types   = resolve("gHotSpotVMTypes");
    if (structs == NULL || types == NULL)
        return false;

    g_structs = *(const unsigned char *const *)structs;
    g_types   = *(const unsigned char *const *)types;
    if (g_structs == NULL || g_types == NULL)
        return false;

    if (!read_offset("gHotSpotVMStructEntryTypeNameOffset",   &g_struct_type_name_offset)  ||
        !read_offset("gHotSpotVMStructEntryFieldNameOffset",  &g_struct_field_name_offset) ||
        !read_offset("gHotSpotVMStructEntryTypeStringOffset", &g_struct_type_string_offset)||
        !read_offset("gHotSpotVMStructEntryIsStaticOffset",   &g_struct_is_static_offset)  ||
        !read_offset("gHotSpotVMStructEntryOffsetOffset",     &g_struct_offset_offset)     ||
        !read_offset("gHotSpotVMStructEntryAddressOffset",    &g_struct_address_offset)    ||
        !read_offset("gHotSpotVMStructEntryArrayStride",      &g_struct_stride)            ||
        !read_offset("gHotSpotVMTypeEntryTypeNameOffset",     &g_type_type_name_offset)    ||
        !read_offset("gHotSpotVMTypeEntrySizeOffset",         &g_type_size_offset)         ||
        !read_offset("gHotSpotVMTypeEntryArrayStride",        &g_type_stride))
        return false;

    if (g_struct_stride == 0 || g_type_stride == 0)
        return false;

    g_ready = true;
    return true;
}

static bool type_size_of(const char *type_name, uint64_t *out_size)
{
    for (const unsigned char *p = g_types;; p += g_type_stride)
    {
        const char *name = *(const char *const *)(p + g_type_type_name_offset);
        if (name == NULL)
            return false;
        if (strcmp(name, type_name) == 0)
        {
            *out_size = *(const uint64_t *)(p + g_type_size_offset);
            return true;
        }
    }
}

static const char *find_flag_type(uint64_t *out_size)
{
    static const char *const candidates[] = { "JVMFlag", "Flag" };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
    {
        if (type_size_of(candidates[i], out_size) && *out_size != 0)
            return candidates[i];
    }
    return NULL;
}

bool vm_structs_set_bool_flag(const char *name, bool value, bool *out_previous)
{
    if (!vm_structs_init() || name == NULL)
        return false;

    uint64_t flag_size = 0;
    const char *flag_type = find_flag_type(&flag_size);
    if (flag_type == NULL)
        return false;

    void    *flags_slot     = NULL;
    void    *num_flags_slot = NULL;
    uint64_t name_offset    = 0;
    uint64_t addr_offset    = 0;
    bool     have_name      = false;
    bool     have_addr      = false;

    for (const unsigned char *p = g_structs;; p += g_struct_stride)
    {
        const struct_entry entry = read_struct_entry(p);
        if (entry.type_name == NULL || entry.field_name == NULL)
            break;
        if (strcmp(entry.type_name, flag_type) != 0)
            continue;

        if (entry.is_static != 0)
        {
            if (strcmp(entry.field_name, "flags") == 0)
                flags_slot = entry.address;
            else if (strcmp(entry.field_name, "numFlags") == 0)
                num_flags_slot = entry.address;
        }
        else
        {
            if (strcmp(entry.field_name, "_name") == 0)
            {
                name_offset = entry.offset;
                have_name   = true;
            }
            else if (strcmp(entry.field_name, "_addr") == 0)
            {
                addr_offset = entry.offset;
                have_addr   = true;
            }
        }
    }

    if (flags_slot == NULL || num_flags_slot == NULL || !have_name || !have_addr)
        return false;

    const unsigned char *table = *(const unsigned char *const *)flags_slot;
    const size_t count = *(const size_t *)num_flags_slot;
    if (table == NULL || count == 0 || count > 100000)
        return false;

    for (size_t i = 0; i < count; i++)
    {
        const unsigned char *entry = table + i * flag_size;
        const char *flag_name = *(const char *const *)(entry + name_offset);
        if (flag_name == NULL || strcmp(flag_name, name) != 0)
            continue;

        bool *slot = *(bool *const *)(entry + addr_offset);
        if (slot == NULL)
            return false;

        if (out_previous != NULL)
            *out_previous = *slot;
        *slot = value;
        return true;
    }

    return false;
}
