#include "jni2hook/utils/visitors/member_info.h"

#include <stdlib.h>
#include <string.h>

void member_info_init(member_info *member)
{
    member->access_flags = 0;
    member->name_index = 0;
    member->descriptor_index = 0;
    attribute_list_init(&member->attributes);
}

void member_info_free(member_info *member)
{
    if (member == NULL)
        return;
    attribute_list_free(&member->attributes);
    member_info_init(member);
}

classfile_status member_info_copy(const member_info *source, member_info *target)
{
    member_info_init(target);
    target->access_flags = source->access_flags;
    target->name_index = source->name_index;
    target->descriptor_index = source->descriptor_index;

    classfile_status status = attribute_list_reserve(&target->attributes, source->attributes.count);
    if (status != CLASSFILE_OK)
        return status;

    for (u2 i = 0; i < source->attributes.count; i++)
    {
        attribute_info *copy = NULL;
        status = attribute_list_append(&target->attributes, &copy);
        if (status != CLASSFILE_OK)
        {
            member_info_free(target);
            return status;
        }
        copy->attribute_name_index = source->attributes.items[i].attribute_name_index;
        status = attribute_info_set_info(copy, source->attributes.items[i].info,
                                         source->attributes.items[i].attribute_length);
        if (status != CLASSFILE_OK)
        {
            member_info_free(target);
            return status;
        }
    }

    return CLASSFILE_OK;
}

bool member_info_is_static(const member_info *member)
{
    return (member->access_flags & JVM_ACC_STATIC) != 0;
}

bool member_info_is_native(const member_info *member)
{
    return (member->access_flags & JVM_ACC_NATIVE) != 0;
}

void member_info_set_native(member_info *member, bool native)
{
    if (native)
        member->access_flags = (u2)(member->access_flags | JVM_ACC_NATIVE);
    else
        member->access_flags = (u2)(member->access_flags & ~(u2)JVM_ACC_NATIVE);
}

void member_list_init(member_list *list)
{
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void member_list_free(member_list *list)
{
    if (list->items != NULL)
    {
        for (u2 i = 0; i < list->count; i++)
            member_info_free(&list->items[i]);
        free(list->items);
    }
    member_list_init(list);
}

classfile_status member_list_reserve(member_list *list, u2 capacity)
{
    if (capacity <= list->capacity)
        return CLASSFILE_OK;

    member_info *grown = realloc(list->items, (size_t)capacity * sizeof(*grown));
    if (grown == NULL)
        return CLASSFILE_ERR_OUT_OF_MEMORY;

    memset(grown + list->capacity, 0, (size_t)(capacity - list->capacity) * sizeof(*grown));
    list->items = grown;
    list->capacity = capacity;
    return CLASSFILE_OK;
}

classfile_status member_list_append(member_list *list, member_info **out)
{
    if (list->count == CLASSFILE_MAX_COUNT)
        return CLASSFILE_ERR_LIMIT_EXCEEDED;

    if (list->count == list->capacity)
    {
        u2 capacity = list->capacity ? list->capacity : 8;
        if ((size_t)capacity * 2 > CLASSFILE_MAX_COUNT)
            capacity = CLASSFILE_MAX_COUNT;
        else
            capacity = (u2)(capacity * 2);

        const classfile_status status = member_list_reserve(list, capacity);
        if (status != CLASSFILE_OK)
            return status;
    }

    member_info *member = &list->items[list->count++];
    member_info_init(member);
    if (out != NULL)
        *out = member;
    return CLASSFILE_OK;
}

void member_list_remove(member_list *list, u2 index)
{
    if (index >= list->count)
        return;

    member_info_free(&list->items[index]);
    const u2 tail = (u2)(list->count - index - 1);
    if (tail != 0)
        memmove(&list->items[index], &list->items[index + 1], (size_t)tail * sizeof(*list->items));
    list->count--;
    memset(&list->items[list->count], 0, sizeof(*list->items));
}

member_info *member_list_find(member_list *list, const constant_pool *pool,
                              const char *name, const char *descriptor)
{
    if (list == NULL)
        return NULL;
    for (u2 i = 0; i < list->count; i++)
    {
        if (!constant_pool_utf8_equals(pool, list->items[i].name_index, name))
            continue;
        if (descriptor != NULL &&
            !constant_pool_utf8_equals(pool, list->items[i].descriptor_index, descriptor))
            continue;
        return &list->items[i];
    }
    return NULL;
}
