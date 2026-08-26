#include "cp_info.h"

#include <stdlib.h>
#include <string.h>

void cp_info_free(cp_info *entry)
{
    if (entry == NULL)
        return;
    if (entry->tag == JVM_CONSTANT_Utf8)
        free(entry->u.utf8.bytes);
    memset(entry, 0, sizeof(*entry));
}

u1 cp_info_slots(u1 tag)
{
    return (tag == JVM_CONSTANT_Long || tag == JVM_CONSTANT_Double) ? 2 : 1;
}

bool cp_info_tag_is_known(u1 tag)
{
    switch (tag)
    {
    case JVM_CONSTANT_Utf8:
    case JVM_CONSTANT_Integer:
    case JVM_CONSTANT_Float:
    case JVM_CONSTANT_Long:
    case JVM_CONSTANT_Double:
    case JVM_CONSTANT_Class:
    case JVM_CONSTANT_String:
    case JVM_CONSTANT_Fieldref:
    case JVM_CONSTANT_Methodref:
    case JVM_CONSTANT_InterfaceMethodref:
    case JVM_CONSTANT_NameAndType:
    case JVM_CONSTANT_MethodHandle:
    case JVM_CONSTANT_MethodType:
    case JVM_CONSTANT_Dynamic:
    case JVM_CONSTANT_InvokeDynamic:
    case JVM_CONSTANT_Module:
    case JVM_CONSTANT_Package:
        return true;
    default:
        return false;
    }
}

classfile_status cp_info_set_utf8(cp_info *entry, const u1 *bytes, u2 length)
{
    u1 *copy = malloc(length ? length : 1);
    if (copy == NULL)
        return CLASSFILE_ERR_OUT_OF_MEMORY;
    if (length != 0)
        memcpy(copy, bytes, length);

    cp_info_free(entry);
    entry->tag = JVM_CONSTANT_Utf8;
    entry->u.utf8.bytes = copy;
    entry->u.utf8.length = length;
    return CLASSFILE_OK;
}

void constant_pool_init(constant_pool *pool)
{
    pool->entries = NULL;
    pool->count = 0;
    pool->capacity = 0;
}

void constant_pool_free(constant_pool *pool)
{
    if (pool->entries != NULL)
    {
        for (u2 i = 1; i < pool->count; i++)
            cp_info_free(&pool->entries[i]);
        free(pool->entries);
    }
    constant_pool_init(pool);
}

classfile_status constant_pool_reserve(constant_pool *pool, u2 capacity)
{
    if (capacity <= pool->capacity)
        return CLASSFILE_OK;

    cp_info *grown = realloc(pool->entries, (size_t)capacity * sizeof(*grown));
    if (grown == NULL)
        return CLASSFILE_ERR_OUT_OF_MEMORY;

    memset(grown + pool->capacity, 0, (size_t)(capacity - pool->capacity) * sizeof(*grown));
    pool->entries = grown;
    pool->capacity = capacity;
    return CLASSFILE_OK;
}

classfile_status constant_pool_append(constant_pool *pool, const cp_info *entry, u2 *index)
{
    const u1 slots = cp_info_slots(entry->tag);
    const u2 first = pool->count < 1 ? 1 : pool->count;

    if ((size_t)first + slots > CLASSFILE_MAX_COUNT)
        return CLASSFILE_ERR_LIMIT_EXCEEDED;

    const u2 needed = (u2)(first + slots);
    u2 capacity = pool->capacity ? pool->capacity : 16;
    while (capacity < needed)
    {
        if ((size_t)capacity * 2 > CLASSFILE_MAX_COUNT)
        {
            capacity = CLASSFILE_MAX_COUNT;
            break;
        }
        capacity = (u2)(capacity * 2);
    }

    const classfile_status status = constant_pool_reserve(pool, capacity);
    if (status != CLASSFILE_OK)
        return status;

    pool->entries[first] = *entry;
    pool->count = needed;

    if (index != NULL)
        *index = first;
    return CLASSFILE_OK;
}

const cp_info *constant_pool_at(const constant_pool *pool, u2 index)
{
    if (pool == NULL || index == 0 || index >= pool->count)
        return NULL;
    const cp_info *entry = &pool->entries[index];
    return entry->tag == 0 ? NULL : entry;
}

bool constant_pool_utf8(const constant_pool *pool, u2 index, const u1 **bytes, u2 *length)
{
    const cp_info *entry = constant_pool_at(pool, index);
    if (entry == NULL || entry->tag != JVM_CONSTANT_Utf8)
        return false;
    if (bytes != NULL)
        *bytes = entry->u.utf8.bytes;
    if (length != NULL)
        *length = entry->u.utf8.length;
    return true;
}

bool constant_pool_utf8_equals(const constant_pool *pool, u2 index, const char *text)
{
    const u1 *bytes;
    u2 length;
    if (!constant_pool_utf8(pool, index, &bytes, &length))
        return false;
    const size_t text_length = strlen(text);
    return length == text_length && memcmp(bytes, text, text_length) == 0;
}
