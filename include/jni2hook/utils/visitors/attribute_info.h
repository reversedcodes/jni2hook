#ifndef JNI2HOOK_VISITORS_ATTRIBUTE_INFO_H
#define JNI2HOOK_VISITORS_ATTRIBUTE_INFO_H

#include "cp_info.h"

/* attribute_info, JVMS 4.7. The payload is kept as opaque bytes: an attribute
   is self describing through its length, so an attribute jni2hook has no
   reason to understand still survives a rewrite untouched. That is what keeps
   the parser working across class file versions it has never seen. */
typedef struct
{
    u2 attribute_name_index;
    u4 attribute_length;
    u1 *info; /* owned, attribute_length bytes */
} attribute_info;

void attribute_info_free(attribute_info *attribute);
classfile_status attribute_info_set_info(attribute_info *attribute, const u1 *bytes, u4 length);

typedef struct
{
    attribute_info *items;
    u2 count;
    u2 capacity;
} attribute_list;

void attribute_list_init(attribute_list *list);
void attribute_list_free(attribute_list *list);
classfile_status attribute_list_reserve(attribute_list *list, u2 capacity);

/* Appends a zeroed attribute and hands back a pointer to it. */
classfile_status attribute_list_append(attribute_list *list, attribute_info **out);
void attribute_list_remove(attribute_list *list, u2 index);

attribute_info *attribute_list_find(const attribute_list *list, const constant_pool *pool,
                                    const char *name);
bool attribute_list_remove_named(attribute_list *list, const constant_pool *pool,
                                 const char *name);

#endif
