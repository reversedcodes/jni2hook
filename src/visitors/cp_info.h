#ifndef JNI2HOOK_VISITORS_CP_INFO_H
#define JNI2HOOK_VISITORS_CP_INFO_H

#include "visitor.h"

#include "../byte_stream.h"

/* cp_info, JVMS 4.4. A tag of zero marks the unusable slot that follows a Long
   or a Double (JVMS 4.4.5) and is never written back out. */
typedef struct
{
    u1 tag;
    union
    {
        struct
        {
            u1 *bytes;
            u2 length;
        } utf8; /* owned */
        struct
        {
            u1 bytes[8];
        } numeric; /* Integer, Float, Long, Double */
        struct
        {
            u2 name_index;
        } class_info; /* Class, Module, Package */
        struct
        {
            u2 string_index;
        } string_info;
        struct
        {
            u2 class_index, nat_index;
        } ref; /* Field-, Method-, InterfaceMethodref */
        struct
        {
            u2 name_index, descriptor_index;
        } nat; /* NameAndType */
        struct
        {
            u1 reference_kind;
            u2 reference_index;
        } method_handle;
        struct
        {
            u2 descriptor_index;
        } method_type;
        struct
        {
            u2 bootstrap_method_attr_index, nat_index;
        } dynamic;
    } u;
} cp_info;

void cp_info_free(cp_info *entry);

/* Slots consumed by an entry: 2 for Long and Double, 1 for everything else. */
u1 cp_info_slots(u1 tag);
bool cp_info_tag_is_known(u1 tag);

classfile_status cp_info_set_utf8(cp_info *entry, const u1 *bytes, u2 length);

/* The constant pool keeps the stored count, which is the number of entries plus
   one, so that an index taken from the class file can be used unchanged. */
typedef struct
{
    cp_info *entries;
    u2 count;
    u2 capacity;
} constant_pool;

/* Reads a constant_pool table, count first, as it appears in a class file.
   Also used on the bytes JVMTI GetConstantPool hands back for a loaded class. */
classfile_status constant_pool_read(byte_cursor *c, constant_pool *pool);

void constant_pool_init(constant_pool *pool);
void constant_pool_free(constant_pool *pool);
classfile_status constant_pool_reserve(constant_pool *pool, u2 capacity);

/* Appends one entry and reports the index it was given. Long and Double also
   claim the following slot, as the format requires. */
classfile_status constant_pool_append(constant_pool *pool, const cp_info *entry, u2 *index);

const cp_info *constant_pool_at(const constant_pool *pool, u2 index);
bool constant_pool_utf8(const constant_pool *pool, u2 index, const u1 **bytes, u2 *length);
bool constant_pool_utf8_equals(const constant_pool *pool, u2 index, const char *text);

#endif
