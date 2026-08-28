#include "jni2hook/utils/visitors/cp_info.h"

#include "jni2hook/utils/byte_stream.h"

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

classfile_status constant_pool_read(byte_cursor *c, constant_pool *pool)
{
    const u2 count = byte_cursor_u2(c);
    if (!byte_cursor_ok(c))
        return CLASSFILE_ERR_TRUNCATED;
    if (count < 1)
        return CLASSFILE_ERR_CONSTANT_INDEX;

    const classfile_status reserved = constant_pool_reserve(pool, count);
    if (reserved != CLASSFILE_OK)
        return reserved;
    pool->count = count;

    for (u2 i = 1; i < count; i++)
    {
        cp_info *entry = &pool->entries[i];
        entry->tag = byte_cursor_u1(c);

        switch (entry->tag)
        {
        case JVM_CONSTANT_Utf8:
        {
            const u2 length = byte_cursor_u2(c);
            const u1 *bytes = byte_cursor_bytes(c, length);
            if (bytes == NULL)
                return CLASSFILE_ERR_TRUNCATED;
            entry->tag = 0; /* set_utf8 frees the previous value, keep it inert */
            const classfile_status status = cp_info_set_utf8(entry, bytes, length);
            if (status != CLASSFILE_OK)
                return status;
            break;
        }
        case JVM_CONSTANT_Integer:
        case JVM_CONSTANT_Float:
        {
            const u1 *value = byte_cursor_bytes(c, 4);
            if (value == NULL)
                return CLASSFILE_ERR_TRUNCATED;
            memcpy(entry->u.numeric.bytes, value, 4);
            break;
        }
        case JVM_CONSTANT_Long:
        case JVM_CONSTANT_Double:
        {
            const u1 *value = byte_cursor_bytes(c, 8);
            if (value == NULL)
                return CLASSFILE_ERR_TRUNCATED;
            memcpy(entry->u.numeric.bytes, value, 8);
            /* JVMS 4.4.5: the following slot stays empty and is unusable, which
               also means it has to exist. A Long in the last slot leaves the
               pool one entry short, and constant_pool_append would later hand
               that very index out and shift every later index the JVM reads. */
            if ((u4)i + 1u >= (u4)count)
                return CLASSFILE_ERR_CONSTANT_INDEX;
            i++;
            break;
        }
        case JVM_CONSTANT_Class:
        case JVM_CONSTANT_Module:
        case JVM_CONSTANT_Package:
            entry->u.class_info.name_index = byte_cursor_u2(c);
            break;
        case JVM_CONSTANT_String:
            entry->u.string_info.string_index = byte_cursor_u2(c);
            break;
        case JVM_CONSTANT_Fieldref:
        case JVM_CONSTANT_Methodref:
        case JVM_CONSTANT_InterfaceMethodref:
            entry->u.ref.class_index = byte_cursor_u2(c);
            entry->u.ref.nat_index = byte_cursor_u2(c);
            break;
        case JVM_CONSTANT_NameAndType:
            entry->u.nat.name_index = byte_cursor_u2(c);
            entry->u.nat.descriptor_index = byte_cursor_u2(c);
            break;
        case JVM_CONSTANT_MethodHandle:
            entry->u.method_handle.reference_kind = byte_cursor_u1(c);
            entry->u.method_handle.reference_index = byte_cursor_u2(c);
            break;
        case JVM_CONSTANT_MethodType:
            entry->u.method_type.descriptor_index = byte_cursor_u2(c);
            break;
        case JVM_CONSTANT_Dynamic:
        case JVM_CONSTANT_InvokeDynamic:
            entry->u.dynamic.bootstrap_method_attr_index = byte_cursor_u2(c);
            entry->u.dynamic.nat_index = byte_cursor_u2(c);
            break;
        default:
            return byte_cursor_ok(c) ? CLASSFILE_ERR_CONSTANT_TAG : CLASSFILE_ERR_TRUNCATED;
        }

        if (!byte_cursor_ok(c))
            return CLASSFILE_ERR_TRUNCATED;
    }

    return CLASSFILE_OK;
}
