#ifndef JNI2HOOK_VISITORS_CLASSFILE_H
#define JNI2HOOK_VISITORS_CLASSFILE_H

#include "member_info.h"

/* The interfaces array of JVMS 4.1, a plain list of constant pool indices. */
typedef struct
{
    u2 *items;
    u2 count;
    u2 capacity;
} interface_list;

void interface_list_init(interface_list *list);
void interface_list_free(interface_list *list);
classfile_status interface_list_reserve(interface_list *list, u2 capacity);
classfile_status interface_list_append(interface_list *list, u2 class_index);

/* ClassFile, JVMS 4.1. The root of the node tree. */
typedef struct
{
    u2 minor_version;
    u2 major_version;

    constant_pool constant_pool;

    u2 access_flags;
    u2 this_class;
    u2 super_class;

    interface_list interfaces;
    member_list fields;
    member_list methods;
    attribute_list attributes;
} ClassFile;

ClassFile *classFile_create(void);
void classFile_destroy(ClassFile *cf);

/* Convenience wrappers that reach into the constant pool of this class. */
bool classFile_utf8_equals(const ClassFile *cf, u2 index, const char *text);

method_info *classFile_find_method(ClassFile *cf, const char *name, const char *descriptor);
field_info *classFile_find_field(ClassFile *cf, const char *name, const char *descriptor);
attribute_info *classFile_find_attribute(ClassFile *cf, const char *name);

/* Interns a Utf8 entry, reusing an identical one when the pool already has it. */
classfile_status classFile_intern_utf8(ClassFile *cf, const char *text, u2 *index);
classfile_status classFile_intern_class(ClassFile *cf, const char *name, u2 *index);
classfile_status classFile_intern_name_and_type(ClassFile *cf, const char *name,
                                                const char *descriptor, u2 *index);
classfile_status classFile_intern_methodref(ClassFile *cf, u2 class_index, const char *name,
                                            const char *descriptor, u2 *index);

#endif
