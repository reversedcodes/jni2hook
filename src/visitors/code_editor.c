#include "jni2hook/utils/visitors/code_editor.h"

#include "jni2hook/utils/byte_stream.h"

#include <stdlib.h>
#include <string.h>

void code_editor_init(code_editor *editor)
{
    memset(editor, 0, sizeof(*editor));
    instruction_list_init(&editor->instructions);
    exception_table_init(&editor->exceptions);
    stack_map_table_init(&editor->stack_map);
    attribute_list_init(&editor->other_attributes);
    type_annotation_refs_init(&editor->type_annotations);
}

void code_editor_free(code_editor *editor)
{
    if (editor == NULL)
        return;
    instruction_list_free(&editor->instructions);
    exception_table_free(&editor->exceptions);
    stack_map_table_free(&editor->stack_map);
    attribute_list_free(&editor->other_attributes);
    type_annotation_refs_free(&editor->type_annotations);
    free(editor->line_numbers);
    free(editor->locals);
    free(editor->local_types);
    code_editor_init(editor);
}

/* JVMS 4.7.12 and 4.7.13 allow any number of these tables on one Code
   attribute, so a second one grows the table already read rather than replacing
   it. Replacing it leaked the first table and silently dropped its entries. */
static classfile_status grow_table(void **entries, u2 *count, u2 added, size_t stride,
                                   void **out_appended)
{
    if ((size_t)*count + added > CLASSFILE_MAX_COUNT)
        return CLASSFILE_ERR_LIMIT_EXCEEDED;

    const u2 total = (u2)(*count + added);
    if (total == 0)
    {
        *out_appended = NULL;
        return CLASSFILE_OK;
    }

    void *grown = realloc(*entries, (size_t)total * stride);
    if (grown == NULL)
        return CLASSFILE_ERR_OUT_OF_MEMORY;

    memset((unsigned char *)grown + (size_t)*count * stride, 0, (size_t)added * stride);
    *entries = grown;
    *out_appended = (unsigned char *)grown + (size_t)*count * stride;
    *count = total;
    return CLASSFILE_OK;
}

static classfile_status read_line_numbers(const attribute_info *attribute, code_editor *editor)
{
    byte_cursor c;
    byte_cursor_init(&c, attribute->info, attribute->attribute_length);

    const u2 count = byte_cursor_u2(&c);
    if (!byte_cursor_ok(&c))
        return CLASSFILE_ERR_TRUNCATED;

    const u2 previous = editor->line_number_count;
    void *appended = NULL;
    const classfile_status status =
        grow_table((void **)&editor->line_numbers, &editor->line_number_count, count,
                   sizeof(*editor->line_numbers), &appended);
    if (status != CLASSFILE_OK)
        return status;

    line_number_entry *entries = appended;
    for (u2 i = 0; i < count; i++)
    {
        entries[i].start_pc    = byte_cursor_u2(&c);
        entries[i].line_number = byte_cursor_u2(&c);
    }

    if (!byte_cursor_ok(&c))
    {
        editor->line_number_count = previous;
        return CLASSFILE_ERR_TRUNCATED;
    }

    editor->has_line_numbers       = true;
    editor->line_number_name_index = attribute->attribute_name_index;
    return CLASSFILE_OK;
}

static classfile_status read_locals(const attribute_info *attribute,
                                    local_variable_entry **out, u2 *out_count)
{
    byte_cursor c;
    byte_cursor_init(&c, attribute->info, attribute->attribute_length);

    const u2 count = byte_cursor_u2(&c);
    if (!byte_cursor_ok(&c))
        return CLASSFILE_ERR_TRUNCATED;

    const u2 previous = *out_count;
    void *appended = NULL;
    const classfile_status status =
        grow_table((void **)out, out_count, count, sizeof(**out), &appended);
    if (status != CLASSFILE_OK)
        return status;

    local_variable_entry *entries = appended;
    for (u2 i = 0; i < count; i++)
    {
        entries[i].start_pc         = byte_cursor_u2(&c);
        entries[i].length           = byte_cursor_u2(&c);
        entries[i].name_index       = byte_cursor_u2(&c);
        entries[i].descriptor_index = byte_cursor_u2(&c);
        entries[i].index            = byte_cursor_u2(&c);
    }

    if (!byte_cursor_ok(&c))
    {
        *out_count = previous;
        return CLASSFILE_ERR_TRUNCATED;
    }

    return CLASSFILE_OK;
}

classfile_status code_editor_load(const attribute_info *code, const constant_pool *pool,
                                  code_editor *out)
{
    code_editor_init(out);

    code_attribute parsed;
    classfile_status status = code_attribute_parse(code, &parsed);
    if (status != CLASSFILE_OK)
        return status;

    out->max_stack  = parsed.max_stack;
    out->max_locals = parsed.max_locals;

    status = instruction_list_parse(parsed.code, parsed.code_length, &out->instructions);
    if (status != CLASSFILE_OK)
    {
        code_attribute_free(&parsed);
        code_editor_free(out);
        return status;
    }

    status = exception_table_reserve(&out->exceptions, parsed.exceptions.count);
    for (u2 i = 0; i < parsed.exceptions.count && status == CLASSFILE_OK; i++)
        status = exception_table_append(&out->exceptions, parsed.exceptions.items[i]);

    for (u2 i = 0; i < parsed.attributes.count && status == CLASSFILE_OK; i++)
    {
        const attribute_info *nested = &parsed.attributes.items[i];

        if (constant_pool_utf8_equals(pool, nested->attribute_name_index, "LineNumberTable"))
        {
            status = read_line_numbers(nested, out);
        }
        else if (constant_pool_utf8_equals(pool, nested->attribute_name_index,
                                           "LocalVariableTable"))
        {
            status = read_locals(nested, &out->locals, &out->local_count);
            out->has_locals               = status == CLASSFILE_OK;
            out->local_variable_name_index = nested->attribute_name_index;
        }
        else if (constant_pool_utf8_equals(pool, nested->attribute_name_index,
                                           "LocalVariableTypeTable"))
        {
            status = read_locals(nested, &out->local_types, &out->local_type_count);
            out->has_local_types       = status == CLASSFILE_OK;
            out->local_type_name_index = nested->attribute_name_index;
        }
        else if (constant_pool_utf8_equals(pool, nested->attribute_name_index, "StackMapTable"))
        {
            /* JVMS 4.7.4 permits at most one. A second would overwrite the
               frames of the first and leak them. */
            if (out->has_stack_map)
            {
                status = CLASSFILE_ERR_UNSUPPORTED;
                continue;
            }
            status = stack_map_table_parse(nested, &out->stack_map);
            out->has_stack_map        = status == CLASSFILE_OK;
            out->stack_map_name_index = nested->attribute_name_index;
        }
        else
        {
            attribute_info *copy = NULL;
            const u2 index = out->other_attributes.count;
            status = attribute_list_append(&out->other_attributes, &copy);
            if (status == CLASSFILE_OK)
            {
                copy->attribute_name_index = nested->attribute_name_index;
                status = attribute_info_set_info(copy, nested->info, nested->attribute_length);
            }

            /* The attribute stays opaque. Only the bytecode offsets buried in
               it are pulled out, so they can travel with the code. */
            if (status == CLASSFILE_OK &&
                type_annotation_is_carrier(pool, nested->attribute_name_index))
                status = type_annotation_collect(copy, index, &out->type_annotations);
        }
    }

    code_attribute_free(&parsed);

    if (status != CLASSFILE_OK)
    {
        code_editor_free(out);
        return status;
    }

    return CLASSFILE_OK;
}

static void write_line_numbers(byte_buffer *b, const code_editor *editor)
{
    byte_buffer_u2(b, editor->line_number_count);
    for (u2 i = 0; i < editor->line_number_count; i++)
    {
        byte_buffer_u2(b, editor->line_numbers[i].start_pc);
        byte_buffer_u2(b, editor->line_numbers[i].line_number);
    }
}

static void write_locals(byte_buffer *b, const local_variable_entry *entries, u2 count)
{
    byte_buffer_u2(b, count);
    for (u2 i = 0; i < count; i++)
    {
        byte_buffer_u2(b, entries[i].start_pc);
        byte_buffer_u2(b, entries[i].length);
        byte_buffer_u2(b, entries[i].name_index);
        byte_buffer_u2(b, entries[i].descriptor_index);
        byte_buffer_u2(b, entries[i].index);
    }
}

/* byte_buffer latches on failure rather than reporting per call, so the check
   belongs here. Releasing an unchecked buffer would hand back a truncated or
   empty payload and let the caller carry on as if it had written the table. */
static classfile_status take_buffer(byte_buffer *b, attribute_info *attribute, u2 name_index)
{
    if (!byte_buffer_ok(b))
    {
        byte_buffer_free(b);
        return CLASSFILE_ERR_OUT_OF_MEMORY;
    }

    size_t size = 0;
    u1 *payload = byte_buffer_release(b, &size);
    if (size > 0xFFFFFFFFu)
    {
        free(payload);
        return CLASSFILE_ERR_LIMIT_EXCEEDED;
    }

    free(attribute->info);
    attribute->attribute_name_index = name_index;
    attribute->info = payload;
    attribute->attribute_length = (u4)size;
    return CLASSFILE_OK;
}

classfile_status code_editor_store(const code_editor *editor, attribute_info *code)
{
    u1 *bytecode = NULL;
    u4  bytecode_length = 0;
    classfile_status status = instruction_list_encode(&editor->instructions,
                                                      &bytecode, &bytecode_length);
    if (status != CLASSFILE_OK)
        return status;

    code_attribute rebuilt;
    code_attribute_init(&rebuilt);
    rebuilt.max_stack  = editor->max_stack;
    rebuilt.max_locals = editor->max_locals;
    rebuilt.code       = bytecode;
    rebuilt.code_length = bytecode_length;

    status = exception_table_reserve(&rebuilt.exceptions, editor->exceptions.count);
    for (u2 i = 0; i < editor->exceptions.count && status == CLASSFILE_OK; i++)
        status = exception_table_append(&rebuilt.exceptions, editor->exceptions.items[i]);

    /* The order the attributes go back in is the order javac writes them, which
       keeps an untouched method byte identical. */
    if (status == CLASSFILE_OK && editor->has_line_numbers)
    {
        attribute_info *a = NULL;
        status = attribute_list_append(&rebuilt.attributes, &a);
        if (status == CLASSFILE_OK)
        {
            byte_buffer b;
            byte_buffer_init(&b);
            write_line_numbers(&b, editor);
            status = take_buffer(&b, a, editor->line_number_name_index);
        }
    }

    if (status == CLASSFILE_OK && editor->has_locals)
    {
        attribute_info *a = NULL;
        status = attribute_list_append(&rebuilt.attributes, &a);
        if (status == CLASSFILE_OK)
        {
            byte_buffer b;
            byte_buffer_init(&b);
            write_locals(&b, editor->locals, editor->local_count);
            status = take_buffer(&b, a, editor->local_variable_name_index);
        }
    }

    if (status == CLASSFILE_OK && editor->has_local_types)
    {
        attribute_info *a = NULL;
        status = attribute_list_append(&rebuilt.attributes, &a);
        if (status == CLASSFILE_OK)
        {
            byte_buffer b;
            byte_buffer_init(&b);
            write_locals(&b, editor->local_types, editor->local_type_count);
            status = take_buffer(&b, a, editor->local_type_name_index);
        }
    }

    if (status == CLASSFILE_OK && editor->has_stack_map)
    {
        attribute_info *a = NULL;
        status = attribute_list_append(&rebuilt.attributes, &a);
        if (status == CLASSFILE_OK)
        {
            a->attribute_name_index = editor->stack_map_name_index;
            status = stack_map_table_write(&editor->stack_map, a);
        }
    }

    for (u2 i = 0; i < editor->other_attributes.count && status == CLASSFILE_OK; i++)
    {
        attribute_info *a = NULL;
        status = attribute_list_append(&rebuilt.attributes, &a);
        if (status == CLASSFILE_OK)
        {
            a->attribute_name_index = editor->other_attributes.items[i].attribute_name_index;
            status = attribute_info_set_info(a, editor->other_attributes.items[i].info,
                                             editor->other_attributes.items[i].attribute_length);
        }
    }

    if (status == CLASSFILE_OK)
        status = code_attribute_write(&rebuilt, code);

    code_attribute_free(&rebuilt);
    return status;
}

static i4 offset_to_index(const instruction_list *list, i4 offset)
{
    if (offset < 0)
        return -1;
    if ((u4)offset == list->code_length)
        return (i4)list->count;

    for (u4 i = 0; i < list->count; i++)
    {
        if (list->items[i].offset == (u4)offset)
            return (i4)i;
    }
    return -1;
}

static i4 index_to_offset(const instruction_list *list, i4 index)
{
    if (index < 0)
        return -1;
    if ((u4)index >= list->count)
        return (i4)list->code_length;
    return (i4)list->items[index].offset;
}

typedef i4 (*reference_map)(const instruction_list *list, i4 value);

/* With apply false nothing is written, so the whole set of references can be
   checked before the first one is changed. */
static bool map_reference(const instruction_list *list, reference_map map, i4 *value, bool apply)
{
    const i4 mapped = map(list, *value);
    if (mapped < 0)
        return false;
    if (apply)
        *value = mapped;
    return true;
}

static bool map_u2_reference(const instruction_list *list, reference_map map, u2 *value,
                             bool apply)
{
    i4 mapped = *value;
    if (!map_reference(list, map, &mapped, apply))
        return false;
    if (apply)
        *value = (u2)mapped;
    return true;
}

/* Walks every offset the Code attribute stores and pushes it through map. Used
   once to turn offsets into instruction indices and once to turn them back. */
static bool map_all_references(code_editor *editor, reference_map map, bool apply)
{
    instruction_list *list = &editor->instructions;
    bool ok = true;

    for (u4 i = 0; i < list->count; i++)
    {
        instruction *node = &list->items[i];
        switch (node->kind)
        {
        case OPERAND_BRANCH:
        case OPERAND_BRANCH_WIDE:
            ok &= map_reference(list, map, &node->u.branch.target, apply);
            break;
        case OPERAND_TABLE_SWITCH:
        {
            ok &= map_reference(list, map, &node->u.table_switch.default_target, apply);
            const u4 entries = instruction_switch_entries(node);
            for (u4 k = 0; k < entries; k++)
                ok &= map_reference(list, map, &node->u.table_switch.targets[k], apply);
            break;
        }
        case OPERAND_LOOKUP_SWITCH:
        {
            ok &= map_reference(list, map, &node->u.lookup_switch.default_target, apply);
            const u4 pairs = instruction_switch_entries(node);
            for (u4 k = 0; k < pairs; k++)
                ok &= map_reference(list, map, &node->u.lookup_switch.pairs[k].target, apply);
            break;
        }
        default:
            break;
        }
    }

    for (u2 i = 0; i < editor->exceptions.count; i++)
    {
        exception_entry *entry = &editor->exceptions.items[i];
        ok &= map_u2_reference(list, map, &entry->start_pc, apply);
        ok &= map_u2_reference(list, map, &entry->end_pc, apply);
        ok &= map_u2_reference(list, map, &entry->handler_pc, apply);
    }

    for (u2 i = 0; i < editor->line_number_count; i++)
        ok &= map_u2_reference(list, map, &editor->line_numbers[i].start_pc, apply);

    /* A local variable is a span, so both ends move and the length is whatever
       is left between them afterwards. */
    local_variable_entry *tables[2] = { editor->locals, editor->local_types };
    const u2 counts[2] = { editor->local_count, editor->local_type_count };
    for (int t = 0; t < 2; t++)
    {
        for (u2 i = 0; i < counts[t]; i++)
        {
            i4 start = tables[t][i].start_pc;
            i4 end   = tables[t][i].start_pc + tables[t][i].length;
            ok &= map_reference(list, map, &start, apply);
            ok &= map_reference(list, map, &end, apply);
            if (apply)
            {
                tables[t][i].start_pc = (u2)start;
                tables[t][i].length   = (u2)(end - start);
            }
        }
    }

    for (u2 i = 0; i < editor->stack_map.count; i++)
    {
        stack_map_frame *frame = &editor->stack_map.frames[i];
        ok &= map_reference(list, map, &frame->offset, apply);

        for (u2 k = 0; k < frame->locals_count; k++)
            if (frame->locals[k].tag == JVM_ITEM_Uninitialized)
                ok &= map_reference(list, map, &frame->locals[k].offset, apply);
        for (u2 k = 0; k < frame->stack_count; k++)
            if (frame->stack[k].tag == JVM_ITEM_Uninitialized)
                ok &= map_reference(list, map, &frame->stack[k].offset, apply);
    }

    for (u2 i = 0; i < editor->type_annotations.count; i++)
    {
        type_annotation_ref *ref = &editor->type_annotations.items[i];
        ok &= map_reference(list, map, &ref->start, apply);
        if (ref->kind == TYPE_ANNOTATION_SPAN)
            ok &= map_reference(list, map, &ref->end, apply);
    }

    return ok;
}

/* A reference that names a position keeps that position, so the inserted code
   takes it over: a branch to it runs the hook, and the frame that covered it
   still covers the point the branch lands on. */
static i4 shift_after(i4 index, i4 at, u4 count)
{
    return index > at ? index + (i4)count : index;
}

/* A reference that names one specific instruction follows that instruction
   instead. An Uninitialized verification type has to keep naming its new, and a
   type annotation has to keep naming the opcode it was written on. */
static i4 shift_with(i4 index, i4 at, u4 count)
{
    return index >= at ? index + (i4)count : index;
}

classfile_status code_editor_insert(code_editor *editor, u4 offset,
                                    instruction *nodes, u4 count, u2 extra_stack)
{
    if (count == 0)
        return CLASSFILE_OK;

    instruction_list *list = &editor->instructions;

    const i4 at = offset_to_index(list, (i4)offset);
    if (at < 0)
        return CLASSFILE_ERR_BAD_OFFSET;

    if ((u4)editor->max_stack + extra_stack > 0xFFFFu)
        return CLASSFILE_ERR_LIMIT_EXCEEDED;

    /* Everything that can fail is settled before the first byte moves: the
       mapping is rehearsed without writing, then the list is grown. After this
       point the edit runs to the end, so the editor can never be left holding
       instruction indices where its caller expects bytecode offsets. */
    if (!map_all_references(editor, offset_to_index, false))
        return CLASSFILE_ERR_BAD_OFFSET;

    classfile_status status = instruction_list_reserve(list, list->count + count);
    if (status != CLASSFILE_OK)
        return status;

    map_all_references(editor, offset_to_index, true);

    const u4 tail = list->count - (u4)at;
    if (tail != 0)
        memmove(&list->items[at + (i4)count], &list->items[at], (size_t)tail * sizeof(*list->items));
    memcpy(&list->items[at], nodes, (size_t)count * sizeof(*nodes));
    list->count += count;

    for (u4 i = 0; i < list->count; i++)
    {
        instruction *node = &list->items[i];
        switch (node->kind)
        {
        case OPERAND_BRANCH:
        case OPERAND_BRANCH_WIDE:
            node->u.branch.target = shift_after(node->u.branch.target, at, count);
            break;
        case OPERAND_TABLE_SWITCH:
        {
            node->u.table_switch.default_target =
                shift_after(node->u.table_switch.default_target, at, count);
            const u4 entries = instruction_switch_entries(node);
            for (u4 k = 0; k < entries; k++)
                node->u.table_switch.targets[k] =
                    shift_after(node->u.table_switch.targets[k], at, count);
            break;
        }
        case OPERAND_LOOKUP_SWITCH:
        {
            node->u.lookup_switch.default_target =
                shift_after(node->u.lookup_switch.default_target, at, count);
            const u4 pairs = instruction_switch_entries(node);
            for (u4 k = 0; k < pairs; k++)
                node->u.lookup_switch.pairs[k].target =
                    shift_after(node->u.lookup_switch.pairs[k].target, at, count);
            break;
        }
        default:
            break;
        }
    }

    for (u2 i = 0; i < editor->exceptions.count; i++)
    {
        exception_entry *entry = &editor->exceptions.items[i];
        entry->start_pc   = (u2)shift_after(entry->start_pc, at, count);
        entry->end_pc     = (u2)shift_after(entry->end_pc, at, count);
        entry->handler_pc = (u2)shift_after(entry->handler_pc, at, count);
    }

    for (u2 i = 0; i < editor->line_number_count; i++)
        editor->line_numbers[i].start_pc =
            (u2)shift_after(editor->line_numbers[i].start_pc, at, count);

    local_variable_entry *tables[2] = { editor->locals, editor->local_types };
    const u2 counts[2] = { editor->local_count, editor->local_type_count };
    for (int t = 0; t < 2; t++)
    {
        for (u2 i = 0; i < counts[t]; i++)
        {
            const i4 start = shift_after(tables[t][i].start_pc, at, count);
            const i4 end   = shift_after(tables[t][i].start_pc + tables[t][i].length, at, count);
            tables[t][i].start_pc = (u2)start;
            tables[t][i].length   = (u2)(end - start);
        }
    }

    for (u2 i = 0; i < editor->stack_map.count; i++)
    {
        stack_map_frame *frame = &editor->stack_map.frames[i];
        frame->offset = shift_after(frame->offset, at, count);
        for (u2 k = 0; k < frame->locals_count; k++)
            if (frame->locals[k].tag == JVM_ITEM_Uninitialized)
                frame->locals[k].offset = shift_with(frame->locals[k].offset, at, count);
        for (u2 k = 0; k < frame->stack_count; k++)
            if (frame->stack[k].tag == JVM_ITEM_Uninitialized)
                frame->stack[k].offset = shift_with(frame->stack[k].offset, at, count);
    }

    for (u2 i = 0; i < editor->type_annotations.count; i++)
    {
        type_annotation_ref *ref = &editor->type_annotations.items[i];
        if (ref->kind == TYPE_ANNOTATION_SPAN)
        {
            ref->start = shift_after(ref->start, at, count);
            ref->end   = shift_after(ref->end, at, count);
        }
        else
        {
            ref->start = shift_with(ref->start, at, count);
        }
    }

    instruction_list_recompute_offsets(list);

    map_all_references(editor, index_to_offset, true);

    status = type_annotation_flush(&editor->type_annotations, &editor->other_attributes);
    if (status != CLASSFILE_OK)
        return status;

    editor->max_stack = (u2)(editor->max_stack + extra_stack);

    return CLASSFILE_OK;
}
