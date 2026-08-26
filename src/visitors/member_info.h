#ifndef JNI2HOOK_VISITORS_MEMBER_INFO_H
#define JNI2HOOK_VISITORS_MEMBER_INFO_H

#include "attribute_info.h"

/* field_info and method_info have the same shape (JVMS 4.5 and 4.6), so one
   node serves both and the two names stay available for readability. */
typedef struct
{
    u2 access_flags;
    u2 name_index;
    u2 descriptor_index;
    attribute_list attributes;
} member_info;

typedef member_info field_info;
typedef member_info method_info;

void member_info_init(member_info *member);
void member_info_free(member_info *member);

/* Deep copy, so the copy owns its own attribute payloads. */
classfile_status member_info_copy(const member_info *source, member_info *target);

bool member_info_is_static(const member_info *member);
bool member_info_is_native(const member_info *member);
void member_info_set_native(member_info *member, bool native);

typedef struct
{
    member_info *items;
    u2 count;
    u2 capacity;
} member_list;

void member_list_init(member_list *list);
void member_list_free(member_list *list);
classfile_status member_list_reserve(member_list *list, u2 capacity);
classfile_status member_list_append(member_list *list, member_info **out);
void member_list_remove(member_list *list, u2 index);

member_info *member_list_find(member_list *list, const constant_pool *pool,
                              const char *name, const char *descriptor);

#endif
