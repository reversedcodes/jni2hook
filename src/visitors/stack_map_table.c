#include "stack_map_table.h"

#include "../byte_stream.h"

#include <stdlib.h>
#include <string.h>

void stack_map_table_init(stack_map_table *table)
{
    table->frames   = NULL;
    table->count    = 0;
    table->capacity = 0;
}

void stack_map_table_free(stack_map_table *table)
{
    if (table->frames != NULL)
    {
        for (u2 i = 0; i < table->count; i++)
        {
            free(table->frames[i].locals);
            free(table->frames[i].stack);
        }
        free(table->frames);
    }
    stack_map_table_init(table);
}

static classfile_status reserve(stack_map_table *table, u2 capacity)
{
    if (capacity <= table->capacity)
        return CLASSFILE_OK;

    stack_map_frame *grown = realloc(table->frames, (size_t)capacity * sizeof(*grown));
    if (grown == NULL)
        return CLASSFILE_ERR_OUT_OF_MEMORY;

    memset(grown + table->capacity, 0, (size_t)(capacity - table->capacity) * sizeof(*grown));
    table->frames   = grown;
    table->capacity = capacity;
    return CLASSFILE_OK;
}

static classfile_status read_types(byte_cursor *c, u2 count, i4 frame_offset,
                                   verification_type **out)
{
    *out = NULL;
    if (count == 0)
        return CLASSFILE_OK;

    verification_type *types = calloc(count, sizeof(*types));
    if (types == NULL)
        return CLASSFILE_ERR_OUT_OF_MEMORY;

    for (u2 i = 0; i < count; i++)
    {
        types[i].tag = byte_cursor_u1(c);
        if (types[i].tag == JVM_ITEM_Object)
            types[i].cpool_index = byte_cursor_u2(c);
        else if (types[i].tag == JVM_ITEM_Uninitialized)
            types[i].offset = (i4)byte_cursor_u2(c);
        (void)frame_offset;

        if (!byte_cursor_ok(c))
        {
            free(types);
            return CLASSFILE_ERR_TRUNCATED;
        }
    }

    *out = types;
    return CLASSFILE_OK;
}

static void write_types(byte_buffer *b, const verification_type *types, u2 count)
{
    for (u2 i = 0; i < count; i++)
    {
        byte_buffer_u1(b, types[i].tag);
        if (types[i].tag == JVM_ITEM_Object)
            byte_buffer_u2(b, types[i].cpool_index);
        else if (types[i].tag == JVM_ITEM_Uninitialized)
            byte_buffer_u2(b, (u2)types[i].offset);
    }
}

classfile_status stack_map_table_parse(const attribute_info *attribute, stack_map_table *out)
{
    stack_map_table_init(out);

    byte_cursor c;
    byte_cursor_init(&c, attribute->info, attribute->attribute_length);

    const u2 count = byte_cursor_u2(&c);
    if (!byte_cursor_ok(&c))
        return CLASSFILE_ERR_TRUNCATED;

    classfile_status status = reserve(out, count);
    if (status != CLASSFILE_OK)
        return status;

    i4   previous = -1;
    bool first    = true;

    for (u2 i = 0; i < count; i++)
    {
        stack_map_frame *frame = &out->frames[out->count];
        memset(frame, 0, sizeof(*frame));

        const u1 tag = byte_cursor_u1(&c);
        frame->raw_tag = tag;

        i4 delta = 0;
        if (tag <= 63)
        {
            frame->kind = FRAME_SAME;
            delta       = tag;
        }
        else if (tag <= 127)
        {
            frame->kind = FRAME_SAME_LOCALS_1_STACK_ITEM;
            delta       = tag - 64;
        }
        else if (tag == 247)
        {
            frame->kind = FRAME_SAME_LOCALS_1_STACK_ITEM;
            delta       = (i4)byte_cursor_u2(&c);
        }
        else if (tag >= 248 && tag <= 250)
        {
            frame->kind       = FRAME_CHOP;
            frame->chop_count = (u2)(251 - tag);
            delta             = (i4)byte_cursor_u2(&c);
        }
        else if (tag == 251)
        {
            frame->kind = FRAME_SAME;
            delta       = (i4)byte_cursor_u2(&c);
        }
        else if (tag >= 252 && tag <= 254)
        {
            frame->kind = FRAME_APPEND;
            delta       = (i4)byte_cursor_u2(&c);
        }
        else if (tag == 255)
        {
            frame->kind = FRAME_FULL;
            delta       = (i4)byte_cursor_u2(&c);
        }
        else
        {
            stack_map_table_free(out);
            return CLASSFILE_ERR_TRUNCATED;
        }

        /* JVMS 4.7.4: the first frame sits at the delta, every later one is a
           further delta plus one past the frame before it. */
        frame->offset = first ? delta : previous + delta + 1;
        previous      = frame->offset;
        first         = false;

        if (frame->kind == FRAME_SAME_LOCALS_1_STACK_ITEM)
        {
            status = read_types(&c, 1, frame->offset, &frame->stack);
            frame->stack_count = 1;
        }
        else if (frame->kind == FRAME_APPEND)
        {
            frame->locals_count = (u2)(frame->raw_tag - 251);
            status = read_types(&c, frame->locals_count, frame->offset, &frame->locals);
        }
        else if (frame->kind == FRAME_FULL)
        {
            frame->locals_count = byte_cursor_u2(&c);
            status = read_types(&c, frame->locals_count, frame->offset, &frame->locals);
            if (status == CLASSFILE_OK)
            {
                frame->stack_count = byte_cursor_u2(&c);
                status = read_types(&c, frame->stack_count, frame->offset, &frame->stack);
            }
        }

        out->count++;

        if (status != CLASSFILE_OK || !byte_cursor_ok(&c))
        {
            stack_map_table_free(out);
            return status != CLASSFILE_OK ? status : CLASSFILE_ERR_TRUNCATED;
        }
    }

    if (!byte_cursor_exhausted(&c))
    {
        stack_map_table_free(out);
        return CLASSFILE_ERR_TRAILING_BYTES;
    }

    return CLASSFILE_OK;
}

classfile_status stack_map_table_write(const stack_map_table *table, attribute_info *attribute)
{
    byte_buffer b;
    byte_buffer_init(&b);
    byte_buffer_reserve(&b, 64);

    byte_buffer_u2(&b, table->count);

    i4   previous = -1;
    bool first    = true;

    for (u2 i = 0; i < table->count; i++)
    {
        const stack_map_frame *frame = &table->frames[i];
        const i4 delta = first ? frame->offset : frame->offset - previous - 1;
        previous = frame->offset;
        first    = false;

        if (delta < 0 || delta > 0xFFFF)
        {
            byte_buffer_free(&b);
            return CLASSFILE_ERR_LIMIT_EXCEEDED;
        }

        switch (frame->kind)
        {
        case FRAME_SAME:
            /* Keep the compact form only while it can still hold the delta. */
            if (frame->raw_tag <= 63 && delta <= 63)
                byte_buffer_u1(&b, (u1)delta);
            else
            {
                byte_buffer_u1(&b, 251);
                byte_buffer_u2(&b, (u2)delta);
            }
            break;

        case FRAME_SAME_LOCALS_1_STACK_ITEM:
            if (frame->raw_tag <= 127 && delta <= 63)
                byte_buffer_u1(&b, (u1)(64 + delta));
            else
            {
                byte_buffer_u1(&b, 247);
                byte_buffer_u2(&b, (u2)delta);
            }
            write_types(&b, frame->stack, 1);
            break;

        case FRAME_CHOP:
            byte_buffer_u1(&b, (u1)(251 - frame->chop_count));
            byte_buffer_u2(&b, (u2)delta);
            break;

        case FRAME_APPEND:
            byte_buffer_u1(&b, (u1)(251 + frame->locals_count));
            byte_buffer_u2(&b, (u2)delta);
            write_types(&b, frame->locals, frame->locals_count);
            break;

        case FRAME_FULL:
            byte_buffer_u1(&b, 255);
            byte_buffer_u2(&b, (u2)delta);
            byte_buffer_u2(&b, frame->locals_count);
            write_types(&b, frame->locals, frame->locals_count);
            byte_buffer_u2(&b, frame->stack_count);
            write_types(&b, frame->stack, frame->stack_count);
            break;

        default:
            byte_buffer_free(&b);
            return CLASSFILE_ERR_TRUNCATED;
        }
    }

    if (!byte_buffer_ok(&b))
    {
        byte_buffer_free(&b);
        return CLASSFILE_ERR_OUT_OF_MEMORY;
    }

    size_t size = 0;
    u1 *payload = byte_buffer_release(&b, &size);

    free(attribute->info);
    attribute->info             = payload;
    attribute->attribute_length = (u4)size;
    return CLASSFILE_OK;
}
