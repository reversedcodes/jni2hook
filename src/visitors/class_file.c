#include "class_file.h"

#include <stdlib.h>
#include <string.h>

void interface_list_init(interface_list *list)
{
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void interface_list_free(interface_list *list)
{
    free(list->items);
    interface_list_init(list);
}

classfile_status interface_list_reserve(interface_list *list, u2 capacity)
{
    if (capacity <= list->capacity)
        return CLASSFILE_OK;

    u2 *grown = realloc(list->items, (size_t)capacity * sizeof(*grown));
    if (grown == NULL)
        return CLASSFILE_ERR_OUT_OF_MEMORY;

    memset(grown + list->capacity, 0, (size_t)(capacity - list->capacity) * sizeof(*grown));
    list->items = grown;
    list->capacity = capacity;
    return CLASSFILE_OK;
}

classfile_status interface_list_append(interface_list *list, u2 class_index)
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

        const classfile_status status = interface_list_reserve(list, capacity);
        if (status != CLASSFILE_OK)
            return status;
    }

    list->items[list->count++] = class_index;
    return CLASSFILE_OK;
}

ClassFile *classFile_create(void)
{
    ClassFile *cf = calloc(1, sizeof(*cf));
    if (cf == NULL)
        return NULL;

    constant_pool_init(&cf->constant_pool);
    interface_list_init(&cf->interfaces);
    member_list_init(&cf->fields);
    member_list_init(&cf->methods);
    attribute_list_init(&cf->attributes);
    return cf;
}

void classFile_destroy(ClassFile *cf)
{
    if (cf == NULL)
        return;

    attribute_list_free(&cf->attributes);
    member_list_free(&cf->methods);
    member_list_free(&cf->fields);
    interface_list_free(&cf->interfaces);
    constant_pool_free(&cf->constant_pool);
    free(cf);
}

bool classFile_utf8_equals(const ClassFile *cf, u2 index, const char *text)
{
    return cf != NULL && constant_pool_utf8_equals(&cf->constant_pool, index, text);
}

method_info *classFile_find_method(ClassFile *cf, const char *name, const char *descriptor)
{
    if (cf == NULL)
        return NULL;
    return member_list_find(&cf->methods, &cf->constant_pool, name, descriptor);
}

field_info *classFile_find_field(ClassFile *cf, const char *name, const char *descriptor)
{
    if (cf == NULL)
        return NULL;
    return member_list_find(&cf->fields, &cf->constant_pool, name, descriptor);
}

attribute_info *classFile_find_attribute(ClassFile *cf, const char *name)
{
    if (cf == NULL)
        return NULL;
    return attribute_list_find(&cf->attributes, &cf->constant_pool, name);
}

classfile_status classFile_intern_utf8(ClassFile *cf, const char *text, u2 *index)
{
    if (cf == NULL || text == NULL)
        return CLASSFILE_ERR_CONSTANT_INDEX;

    const size_t length = strlen(text);
    if (length > CLASSFILE_MAX_COUNT)
        return CLASSFILE_ERR_LIMIT_EXCEEDED;

    for (u2 i = 1; i < cf->constant_pool.count; i++)
    {
        const cp_info *entry = constant_pool_at(&cf->constant_pool, i);
        if (entry == NULL || entry->tag != JVM_CONSTANT_Utf8)
            continue;
        if (entry->u.utf8.length == length && memcmp(entry->u.utf8.bytes, text, length) == 0)
        {
            if (index != NULL)
                *index = i;
            return CLASSFILE_OK;
        }
    }

    cp_info entry;
    memset(&entry, 0, sizeof(entry));
    const classfile_status status = cp_info_set_utf8(&entry, (const u1 *)text, (u2)length);
    if (status != CLASSFILE_OK)
        return status;

    const classfile_status appended = constant_pool_append(&cf->constant_pool, &entry, index);
    if (appended != CLASSFILE_OK)
        cp_info_free(&entry);
    return appended;
}

static classfile_status intern_simple(ClassFile *cf, u1 tag, u2 a, u2 b, u2 *index)
{
    for (u2 i = 1; i < cf->constant_pool.count; i++)
    {
        const cp_info *entry = constant_pool_at(&cf->constant_pool, i);
        if (entry == NULL || entry->tag != tag)
            continue;

        bool same = false;
        if (tag == JVM_CONSTANT_Class)
            same = entry->u.class_info.name_index == a;
        else if (tag == JVM_CONSTANT_NameAndType)
            same = entry->u.nat.name_index == a && entry->u.nat.descriptor_index == b;
        else
            same = entry->u.ref.class_index == a && entry->u.ref.nat_index == b;

        if (same)
        {
            if (index != NULL)
                *index = i;
            return CLASSFILE_OK;
        }
    }

    cp_info entry;
    memset(&entry, 0, sizeof(entry));
    entry.tag = tag;
    if (tag == JVM_CONSTANT_Class)
        entry.u.class_info.name_index = a;
    else if (tag == JVM_CONSTANT_NameAndType)
    {
        entry.u.nat.name_index       = a;
        entry.u.nat.descriptor_index = b;
    }
    else
    {
        entry.u.ref.class_index = a;
        entry.u.ref.nat_index   = b;
    }

    return constant_pool_append(&cf->constant_pool, &entry, index);
}

classfile_status classFile_intern_class(ClassFile *cf, const char *name, u2 *index)
{
    u2 name_index = 0;
    const classfile_status status = classFile_intern_utf8(cf, name, &name_index);
    if (status != CLASSFILE_OK)
        return status;
    return intern_simple(cf, JVM_CONSTANT_Class, name_index, 0, index);
}

classfile_status classFile_intern_name_and_type(ClassFile *cf, const char *name,
                                                const char *descriptor, u2 *index)
{
    u2 name_index = 0, descriptor_index = 0;
    classfile_status status = classFile_intern_utf8(cf, name, &name_index);
    if (status != CLASSFILE_OK)
        return status;
    status = classFile_intern_utf8(cf, descriptor, &descriptor_index);
    if (status != CLASSFILE_OK)
        return status;
    return intern_simple(cf, JVM_CONSTANT_NameAndType, name_index, descriptor_index, index);
}

classfile_status classFile_intern_methodref(ClassFile *cf, u2 class_index, const char *name,
                                            const char *descriptor, u2 *index)
{
    u2 nat_index = 0;
    const classfile_status status = classFile_intern_name_and_type(cf, name, descriptor, &nat_index);
    if (status != CLASSFILE_OK)
        return status;
    return intern_simple(cf, JVM_CONSTANT_Methodref, class_index, nat_index, index);
}
