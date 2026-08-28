#ifndef JNI2HOOK_VISITORS_TYPE_ANNOTATION_H
#define JNI2HOOK_VISITORS_TYPE_ANNOTATION_H

#include "attribute_info.h"

/* RuntimeVisibleTypeAnnotations and RuntimeInvisibleTypeAnnotations are the
 * sixth structure that points into the bytecode by offset, and the only one the
 * opaque-attribute rule cannot carry unchanged through an insertion.
 *
 * JVMS 4.7.20: a type_annotation sitting on a Code attribute names a bytecode
 * position through localvar_target (a scope span), offset_target (the new,
 * instanceof, or method reference instruction it annotates) and
 * type_argument_target (the cast or invocation it annotates). Leaving those
 * alone after an insertion makes them name the wrong instruction.
 *
 * The attribute itself stays opaque. What is collected here is the byte
 * position of each offset inside the payload plus its current value, so the
 * values can travel through the same index mapping as every other reference and
 * be written straight back into the bytes afterwards. */

typedef enum
{
    /* localvar_target: a start_pc with a length behind it, so a scope, and it
       moves the way a LocalVariableTable entry does. */
    TYPE_ANNOTATION_SPAN,
    /* offset_target and type_argument_target: the identity of one instruction,
       which has to keep naming that instruction when it moves. */
    TYPE_ANNOTATION_INSTRUCTION
} type_annotation_ref_kind;

typedef struct
{
    u2 attribute_index;
    u4 payload_offset;
    type_annotation_ref_kind kind;
    i4 start;
    i4 end;
} type_annotation_ref;

typedef struct
{
    type_annotation_ref *items;
    u2 count;
    u2 capacity;
} type_annotation_refs;

void type_annotation_refs_init(type_annotation_refs *refs);
void type_annotation_refs_free(type_annotation_refs *refs);

/* True for the two attribute names that carry bytecode offsets. */
bool type_annotation_is_carrier(const constant_pool *pool, u2 name_index);

/* Walks one such attribute and records every offset it points at.
   attribute_index is the caller's own handle for the attribute, handed back
   unchanged in each ref. */
classfile_status type_annotation_collect(const attribute_info *attribute, u2 attribute_index,
                                         type_annotation_refs *refs);

/* Writes the current values back into the payloads of the attributes the refs
   name. Attributes are addressed by the attribute_index the refs carry. */
classfile_status type_annotation_flush(const type_annotation_refs *refs,
                                       attribute_list *attributes);

#endif
