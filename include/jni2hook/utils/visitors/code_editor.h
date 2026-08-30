#ifndef JNI2HOOK_VISITORS_CODE_EDITOR_H
#define JNI2HOOK_VISITORS_CODE_EDITOR_H

#include "code_attribute.h"
#include "instruction.h"
#include "stack_map_table.h"
#include "type_annotation.h"

/* Editable Code attribute. During insertion, bytecode references are mapped to
   instruction indices so switch padding and other non-uniform growth are safe.
   Positional references stay on the insertion point; references to a specific
   instruction move with that instruction. */

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
