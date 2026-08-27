#ifndef JNI2HOOK_VISITORS_STACK_MAP_TABLE_H
#define JNI2HOOK_VISITORS_STACK_MAP_TABLE_H

#include "attribute_info.h"

/* StackMapTable, JVMS 4.7.4.
 *
 * The attribute stores each frame as a delta from the one before, and the delta
 * of every frame after the first is one less than the real distance. Both are
 * undone here: frames carry the absolute bytecode offset they belong to, and
 * the deltas are rebuilt on the way out. Inserting code then only means moving
 * the offsets, with no chain of deltas to patch by hand.
 *
 * Two things are easy to miss. A verification_type_info of kind Uninitialized
 * carries a bytecode offset of its own, pointing at the new instruction that
 * created the object, and that offset moves too. And a frame's encoding depends
 * on how large its delta is: a SAME_FRAME can only express a delta up to 63, so
 * a frame whose delta grows past that has to switch to the extended form. The
 * original frame type is kept so that untouched code re-emits byte for byte
 * even when javac chose a wider encoding than it had to. */

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
