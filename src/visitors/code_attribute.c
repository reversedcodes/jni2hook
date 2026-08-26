#include "code_attribute.h"

#include "../byte_stream.h"

#include <stdlib.h>
#include <string.h>

void exception_table_init(exception_table *table)
{
    table->items    = NULL;
    table->count    = 0;
    table->capacity = 0;
}

void exception_table_free(exception_table *table)
{
    free(table->items);
    exception_table_init(table);
}

classfile_status exception_table_reserve(exception_table *table, u2 capacity)
{
    if (capacity <= table->capacity)
        return CLASSFILE_OK;

    exception_entry *grown = realloc(table->items, (size_t)capacity * sizeof(*grown));
    if (grown == NULL)
        return CLASSFILE_ERR_OUT_OF_MEMORY;

    memset(grown + table->capacity, 0, (size_t)(capacity - table->capacity) * sizeof(*grown));
    table->items    = grown;
    table->capacity = capacity;
    return CLASSFILE_OK;
}

classfile_status exception_table_append(exception_table *table, exception_entry entry)
{
    if (table->count == CLASSFILE_MAX_COUNT)
        return CLASSFILE_ERR_LIMIT_EXCEEDED;

    if (table->count == table->capacity) {
        u2 capacity = table->capacity ? table->capacity : 4;
        if ((size_t)capacity * 2 > CLASSFILE_MAX_COUNT)
            capacity = CLASSFILE_MAX_COUNT;
        else
            capacity = (u2)(capacity * 2);

        const classfile_status status = exception_table_reserve(table, capacity);
        if (status != CLASSFILE_OK)
            return status;
    }

    table->items[table->count++] = entry;
    return CLASSFILE_OK;
}

void code_attribute_init(code_attribute *code)
{
    code->max_stack   = 0;
    code->max_locals  = 0;
    code->code_length = 0;
    code->code        = NULL;
    exception_table_init(&code->exceptions);
    attribute_list_init(&code->attributes);
}

void code_attribute_free(code_attribute *code)
{
    if (code == NULL)
        return;
    free(code->code);
    exception_table_free(&code->exceptions);
    attribute_list_free(&code->attributes);
    code_attribute_init(code);
}

classfile_status code_attribute_set_code(code_attribute *code, const u1 *bytes, u4 length)
{
    u1 *copy = malloc(length ? length : 1);
    if (copy == NULL)
        return CLASSFILE_ERR_OUT_OF_MEMORY;
    if (length != 0)
        memcpy(copy, bytes, length);

    free(code->code);
    code->code        = copy;
    code->code_length = length;
    return CLASSFILE_OK;
}

classfile_status code_attribute_parse(const attribute_info *attribute, code_attribute *out)
{
    code_attribute_init(out);

    byte_cursor c;
    byte_cursor_init(&c, attribute->info, attribute->attribute_length);

    out->max_stack  = byte_cursor_u2(&c);
    out->max_locals = byte_cursor_u2(&c);

    const u4  code_length = byte_cursor_u4(&c);
    const u1 *code_bytes  = byte_cursor_bytes(&c, code_length);
    if (code_bytes == NULL) {
        code_attribute_free(out);
        return CLASSFILE_ERR_TRUNCATED;
    }

    classfile_status status = code_attribute_set_code(out, code_bytes, code_length);
    if (status != CLASSFILE_OK) {
        code_attribute_free(out);
        return status;
    }

    const u2 exception_count = byte_cursor_u2(&c);
    status = exception_table_reserve(&out->exceptions, exception_count);
    if (status != CLASSFILE_OK) {
        code_attribute_free(out);
        return status;
    }

    for (u2 i = 0; i < exception_count; i++) {
        exception_entry entry;
        entry.start_pc   = byte_cursor_u2(&c);
        entry.end_pc     = byte_cursor_u2(&c);
        entry.handler_pc = byte_cursor_u2(&c);
        entry.catch_type = byte_cursor_u2(&c);
        if (!byte_cursor_ok(&c)) {
            code_attribute_free(out);
            return CLASSFILE_ERR_TRUNCATED;
        }
        status = exception_table_append(&out->exceptions, entry);
        if (status != CLASSFILE_OK) {
            code_attribute_free(out);
            return status;
        }
    }

    const u2 attribute_count = byte_cursor_u2(&c);
    status = attribute_list_reserve(&out->attributes, attribute_count);
    if (status != CLASSFILE_OK) {
        code_attribute_free(out);
        return status;
    }

    for (u2 i = 0; i < attribute_count; i++) {
        attribute_info *nested = NULL;
        status = attribute_list_append(&out->attributes, &nested);
        if (status != CLASSFILE_OK) {
            code_attribute_free(out);
            return status;
        }

        nested->attribute_name_index = byte_cursor_u2(&c);
        const u4  length  = byte_cursor_u4(&c);
        const u1 *payload = byte_cursor_bytes(&c, length);
        if (payload == NULL) {
            code_attribute_free(out);
            return CLASSFILE_ERR_TRUNCATED;
        }

        status = attribute_info_set_info(nested, payload, length);
        if (status != CLASSFILE_OK) {
            code_attribute_free(out);
            return status;
        }
    }

    if (!byte_cursor_exhausted(&c)) {
        code_attribute_free(out);
        return CLASSFILE_ERR_TRAILING_BYTES;
    }

    return CLASSFILE_OK;
}

classfile_status code_attribute_write(const code_attribute *code, attribute_info *attribute)
{
    byte_buffer b;
    byte_buffer_init(&b);
    byte_buffer_reserve(&b, code->code_length + 64);

    byte_buffer_u2(&b, code->max_stack);
    byte_buffer_u2(&b, code->max_locals);
    byte_buffer_u4(&b, code->code_length);
    byte_buffer_bytes(&b, code->code, code->code_length);

    byte_buffer_u2(&b, code->exceptions.count);
    for (u2 i = 0; i < code->exceptions.count; i++) {
        byte_buffer_u2(&b, code->exceptions.items[i].start_pc);
        byte_buffer_u2(&b, code->exceptions.items[i].end_pc);
        byte_buffer_u2(&b, code->exceptions.items[i].handler_pc);
        byte_buffer_u2(&b, code->exceptions.items[i].catch_type);
    }

    byte_buffer_u2(&b, code->attributes.count);
    for (u2 i = 0; i < code->attributes.count; i++) {
        byte_buffer_u2(&b, code->attributes.items[i].attribute_name_index);
        byte_buffer_u4(&b, code->attributes.items[i].attribute_length);
        byte_buffer_bytes(&b, code->attributes.items[i].info,
                          code->attributes.items[i].attribute_length);
    }

    if (!byte_buffer_ok(&b)) {
        byte_buffer_free(&b);
        return CLASSFILE_ERR_OUT_OF_MEMORY;
    }

    size_t size = 0;
    u1 *payload = byte_buffer_release(&b, &size);
    if (size > 0xFFFFFFFFu) {
        free(payload);
        return CLASSFILE_ERR_LIMIT_EXCEEDED;
    }

    free(attribute->info);
    attribute->info             = payload;
    attribute->attribute_length = (u4)size;
    return CLASSFILE_OK;
}
