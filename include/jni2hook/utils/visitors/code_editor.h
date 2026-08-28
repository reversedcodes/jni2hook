#ifndef JNI2HOOK_VISITORS_CODE_EDITOR_H
#define JNI2HOOK_VISITORS_CODE_EDITOR_H

#include "code_attribute.h"
#include "instruction.h"
#include "stack_map_table.h"
#include "type_annotation.h"

/* A Code attribute taken apart far enough to insert instructions into it.
 *
 * Six separate structures point into the bytecode by offset, and every one of
 * them has to follow when something is inserted:
 *
 *   branches and switches   inside the instruction list itself
 *   exception_table         start_pc, end_pc, handler_pc
 *   LineNumberTable         start_pc per entry
 *   LocalVariableTable      start_pc plus a length, so a span, not a point
 *   StackMapTable           the frame chain, and Uninitialized entries
 *   type annotations        localvar, offset and type_argument targets
 *
 * Rather than adding a delta to each of them, every reference is turned into
 * the index of the instruction it names, the list is spliced, the offsets are
 * recomputed and the references are read back out. Nothing then depends on how
 * much the code grew, which matters because it does not grow by a fixed amount:
 * a switch changes length when its padding shifts.
 *
 * Two kinds of reference behave differently at the insertion point itself, and
 * getting that wrong is a VerifyError rather than a subtlety:
 *
 *   A reference that names a *position* — a branch or switch target, a
 *   StackMapTable frame, an exception range bound or handler, a line number, a
 *   local variable scope — stays where it is, so it now names the first
 *   inserted instruction. A jump to that offset therefore runs the inserted
 *   code, which is what makes a hook at an interior offset fire on every path
 *   reaching it, and it keeps the frame paired with the branch target.
 *
 *   A reference that names a specific *instruction* — a StackMapTable
 *   Uninitialized entry naming its new, an offset_target or
 *   type_argument_target naming the instruction it annotates — moves with that
 *   instruction instead, because it has to keep naming the same opcode.
 *
 * JVMS allows LineNumberTable, LocalVariableTable and LocalVariableTypeTable to
 * appear more than once on a Code attribute. Repeated tables are read into one
 * table here and written back as one, which the JVM treats identically. */

typedef struct
{
    u2 start_pc;
    u2 line_number;
} line_number_entry;

typedef struct
{
    u2 start_pc;
    u2 length;
    u2 name_index;
    u2 descriptor_index;
    u2 index;
} local_variable_entry;

typedef struct
{
    u2                    max_stack;
    u2                    max_locals;
    instruction_list      instructions;
    exception_table       exceptions;

    bool                  has_line_numbers;
    u2                    line_number_name_index;
    line_number_entry    *line_numbers;
    u2                    line_number_count;

    bool                  has_locals;
    u2                    local_variable_name_index;
    local_variable_entry *locals;
    u2                    local_count;

    bool                  has_local_types;
    u2                    local_type_name_index;
    local_variable_entry *local_types;
    u2                    local_type_count;

    bool                  has_stack_map;
    u2                    stack_map_name_index;
    stack_map_table       stack_map;

    attribute_list        other_attributes;
    type_annotation_refs  type_annotations;
} code_editor;

void code_editor_init(code_editor *editor);
void code_editor_free(code_editor *editor);

classfile_status code_editor_load(const attribute_info *code, const constant_pool *pool,
                                  code_editor *out);
classfile_status code_editor_store(const code_editor *editor, attribute_info *code);

/* Inserts nodes before whatever currently sits at offset. The nodes are taken
   over, including any switch payload they own. extra_stack is added to
   max_stack, since the caller knows how deep the inserted code goes. */
classfile_status code_editor_insert(code_editor *editor, u4 offset,
                                    instruction *nodes, u4 count, u2 extra_stack);

#endif
