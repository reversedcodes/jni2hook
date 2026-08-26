#include "attribute_info.h"

#include <stdlib.h>
#include <string.h>

void attribute_info_free(attribute_info *attribute)
{
    if (attribute == NULL)
        return;
    free(attribute->info);
    memset(attribute, 0, sizeof(*attribute));
}

classfile_status attribute_info_set_info(attribute_info *attribute, const u1 *bytes, u4 length)
{
    u1 *copy = malloc(length ? length : 1);
    if (copy == NULL)
        return CLASSFILE_ERR_OUT_OF_MEMORY;
    if (length != 0)
        memcpy(copy, bytes, length);

    free(attribute->info);
    attribute->info = copy;
    attribute->attribute_length = length;
    return CLASSFILE_OK;
}

void attribute_list_init(attribute_list *list)
{
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void attribute_list_free(attribute_list *list)
{
    if (list->items != NULL)
    {
        for (u2 i = 0; i < list->count; i++)
            attribute_info_free(&list->items[i]);
        free(list->items);
    }
    attribute_list_init(list);
}

classfile_status attribute_list_reserve(attribute_list *list, u2 capacity)
{
    if (capacity <= list->capacity)
        return CLASSFILE_OK;

    attribute_info *grown = realloc(list->items, (size_t)capacity * sizeof(*grown));
    if (grown == NULL)
        return CLASSFILE_ERR_OUT_OF_MEMORY;

    memset(grown + list->capacity, 0, (size_t)(capacity - list->capacity) * sizeof(*grown));
    list->items = grown;
    list->capacity = capacity;
    return CLASSFILE_OK;
}

classfile_status attribute_list_append(attribute_list *list, attribute_info **out)
{
    if (list->count == CLASSFILE_MAX_COUNT)
        return CLASSFILE_ERR_LIMIT_EXCEEDED;

    if (list->count == list->capacity)
    {
        u2 capacity = list->capacity ? list->capacity : 4;
        if ((size_t)capacity * 2 > CLASSFILE_MAX_COUNT)
            capacity = CLASSFILE_MAX_COUNT;
        else
            capacity = (u2)(capacity * 2);

        const classfile_status status = attribute_list_reserve(list, capacity);
        if (status != CLASSFILE_OK)
            return status;
    }

    attribute_info *attribute = &list->items[list->count++];
    memset(attribute, 0, sizeof(*attribute));
    if (out != NULL)
        *out = attribute;
    return CLASSFILE_OK;
}

void attribute_list_remove(attribute_list *list, u2 index)
{
    if (index >= list->count)
        return;

    attribute_info_free(&list->items[index]);
    const u2 tail = (u2)(list->count - index - 1);
    if (tail != 0)
        memmove(&list->items[index], &list->items[index + 1], (size_t)tail * sizeof(*list->items));
    list->count--;
    memset(&list->items[list->count], 0, sizeof(*list->items));
}

attribute_info *attribute_list_find(const attribute_list *list, const constant_pool *pool,
                                    const char *name)
{
    if (list == NULL)
        return NULL;
    for (u2 i = 0; i < list->count; i++)
    {
        if (constant_pool_utf8_equals(pool, list->items[i].attribute_name_index, name))
            return &list->items[i];
    }
    return NULL;
}

bool attribute_list_remove_named(attribute_list *list, const constant_pool *pool, const char *name)
{
    if (list == NULL)
        return false;
    for (u2 i = 0; i < list->count; i++)
    {
        if (constant_pool_utf8_equals(pool, list->items[i].attribute_name_index, name))
        {
            attribute_list_remove(list, i);
            return true;
        }
    }
    return false;
}
