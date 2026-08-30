#ifndef JNI2HOOK_VISITORS_STACK_MAP_TABLE_H
#define JNI2HOOK_VISITORS_STACK_MAP_TABLE_H

#include "attribute_info.h"

/* Frames use absolute bytecode offsets in memory and deltas on disk. raw_tag is
   retained for byte-identical output; Uninitialized offsets move with their new
   instruction. */

enum
{
    FRAME_SAME,
    FRAME_SAME_LOCALS_1_STACK_ITEM,
    FRAME_CHOP,
    FRAME_APPEND,
    FRAME_FULL
};

typedef struct
{
    u1 tag;
    u2 cpool_index;   /* ITEM_Object */
    i4 offset;        /* ITEM_Uninitialized, absolute */
} verification_type;

typedef struct
{
    u1 raw_tag;
    u1 kind;
    i4 offset;
    u2 chop_count;

    verification_type *locals;
    u2                 locals_count;
    verification_type *stack;
    u2                 stack_count;
} stack_map_frame;

typedef struct
{
    stack_map_frame *frames;
    u2               count;
    u2               capacity;
} stack_map_table;

void stack_map_table_init(stack_map_table *table);
void stack_map_table_free(stack_map_table *table);

classfile_status stack_map_table_parse(const attribute_info *attribute, stack_map_table *out);
classfile_status stack_map_table_write(const stack_map_table *table, attribute_info *attribute);

#endif
