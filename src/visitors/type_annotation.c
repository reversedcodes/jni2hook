#include "jni2hook/utils/visitors/type_annotation.h"

#include "jni2hook/utils/byte_stream.h"

#include <stdlib.h>
#include <string.h>

/* An element_value may hold an annotation, which may hold an array of
   element_values again. Real annotations nest two or three deep; the cap only
   stops a crafted attribute from running the stack out. */
enum
{
    MAX_ANNOTATION_DEPTH = 32
};

void type_annotation_refs_init(type_annotation_refs *refs)
{
    refs->items = NULL;
    refs->count = 0;
    refs->capacity = 0;
}

void type_annotation_refs_free(type_annotation_refs *refs)
{
    free(refs->items);
    type_annotation_refs_init(refs);
}

static classfile_status append_ref(type_annotation_refs *refs, u2 attribute_index,
                                   u4 payload_offset, type_annotation_ref_kind kind,
                                   i4 start, i4 end)
{
    if (refs->count == CLASSFILE_MAX_COUNT)
        return CLASSFILE_ERR_LIMIT_EXCEEDED;

    if (refs->count == refs->capacity)
    {
        u2 capacity = refs->capacity ? refs->capacity : 8;
        if ((size_t)capacity * 2 > CLASSFILE_MAX_COUNT)
            capacity = CLASSFILE_MAX_COUNT;
        else
            capacity = (u2)(capacity * 2);

        type_annotation_ref *grown = realloc(refs->items, (size_t)capacity * sizeof(*grown));
        if (grown == NULL)
            return CLASSFILE_ERR_OUT_OF_MEMORY;

        memset(grown + refs->capacity, 0, (size_t)(capacity - refs->capacity) * sizeof(*grown));
        refs->items = grown;
        refs->capacity = capacity;
    }

    type_annotation_ref *ref = &refs->items[refs->count++];
    ref->attribute_index = attribute_index;
    ref->payload_offset = payload_offset;
    ref->kind = kind;
    ref->start = start;
    ref->end = end;
    return CLASSFILE_OK;
}

static bool skip_element_value(byte_cursor *c, int depth);

static bool skip_annotation(byte_cursor *c, int depth)
{
    if (depth > MAX_ANNOTATION_DEPTH)
        return false;

    (void)byte_cursor_u2(c);
    const u2 pairs = byte_cursor_u2(c);
    if (!byte_cursor_ok(c))
        return false;

    for (u2 i = 0; i < pairs; i++)
    {
        (void)byte_cursor_u2(c);
        if (!skip_element_value(c, depth + 1))
            return false;
    }
    return byte_cursor_ok(c);
}

/* JVMS 4.7.16.1. Nothing in an element_value is a bytecode offset, so it only
   has to be stepped over accurately enough to find the next annotation. */
static bool skip_element_value(byte_cursor *c, int depth)
{
    if (depth > MAX_ANNOTATION_DEPTH)
        return false;

    const u1 tag = byte_cursor_u1(c);
    if (!byte_cursor_ok(c))
        return false;

    switch (tag)
    {
    case 'B':
    case 'C':
    case 'D':
    case 'F':
    case 'I':
    case 'J':
    case 'S':
    case 'Z':
    case 's':
    case 'c':
        (void)byte_cursor_u2(c);
        break;

    case 'e':
        (void)byte_cursor_u2(c);
        (void)byte_cursor_u2(c);
        break;

    case '@':
        if (!skip_annotation(c, depth + 1))
            return false;
        break;

    case '[':
    {
        const u2 values = byte_cursor_u2(c);
        if (!byte_cursor_ok(c))
            return false;
        for (u2 i = 0; i < values; i++)
        {
            if (!skip_element_value(c, depth + 1))
                return false;
        }
        break;
    }

    default:
        return false;
    }

    return byte_cursor_ok(c);
}

/* JVMS 4.7.20.1. Only the three target kinds that appear on a Code attribute
   carry a bytecode offset; the rest are stepped over so that the walk stays in
   sync with the annotations behind them. */
static classfile_status read_target(byte_cursor *c, u1 target_type, u2 attribute_index,
                                    type_annotation_refs *refs)
{
    classfile_status status = CLASSFILE_OK;

    switch (target_type)
    {
    case 0x00:
    case 0x01:
    case 0x16:
        (void)byte_cursor_u1(c);
        break;

    case 0x10:
    case 0x17:
    case 0x42:
        (void)byte_cursor_u2(c);
        break;

    case 0x11:
    case 0x12:
        (void)byte_cursor_u1(c);
        (void)byte_cursor_u1(c);
        break;

    case 0x13:
    case 0x14:
    case 0x15:
        break;

    case 0x40:
    case 0x41:
    {
        const u2 entries = byte_cursor_u2(c);
        if (!byte_cursor_ok(c))
            return CLASSFILE_ERR_TRUNCATED;

        for (u2 i = 0; i < entries && status == CLASSFILE_OK; i++)
        {
            const u4 at = (u4)byte_cursor_offset(c);
            const u2 start = byte_cursor_u2(c);
            const u2 length = byte_cursor_u2(c);
            (void)byte_cursor_u2(c);
            if (!byte_cursor_ok(c))
                return CLASSFILE_ERR_TRUNCATED;

            status = append_ref(refs, attribute_index, at, TYPE_ANNOTATION_SPAN, (i4)start,
                                (i4)start + (i4)length);
        }
        break;
    }

    case 0x43:
    case 0x44:
    case 0x45:
    case 0x46:
    {
        const u4 at = (u4)byte_cursor_offset(c);
        const u2 value = byte_cursor_u2(c);
        if (!byte_cursor_ok(c))
            return CLASSFILE_ERR_TRUNCATED;
        status = append_ref(refs, attribute_index, at, TYPE_ANNOTATION_INSTRUCTION, (i4)value, 0);
        break;
    }

    case 0x47:
    case 0x48:
    case 0x49:
    case 0x4A:
    case 0x4B:
    {
        const u4 at = (u4)byte_cursor_offset(c);
        const u2 value = byte_cursor_u2(c);
        (void)byte_cursor_u1(c);
        if (!byte_cursor_ok(c))
            return CLASSFILE_ERR_TRUNCATED;
        status = append_ref(refs, attribute_index, at, TYPE_ANNOTATION_INSTRUCTION, (i4)value, 0);
        break;
    }

    default:
        return CLASSFILE_ERR_UNSUPPORTED;
    }

    if (status != CLASSFILE_OK)
        return status;
    return byte_cursor_ok(c) ? CLASSFILE_OK : CLASSFILE_ERR_TRUNCATED;
}

bool type_annotation_is_carrier(const constant_pool *pool, u2 name_index)
{
    return constant_pool_utf8_equals(pool, name_index, "RuntimeVisibleTypeAnnotations") ||
           constant_pool_utf8_equals(pool, name_index, "RuntimeInvisibleTypeAnnotations");
}

classfile_status type_annotation_collect(const attribute_info *attribute, u2 attribute_index,
                                         type_annotation_refs *refs)
{
    byte_cursor c;
    byte_cursor_init(&c, attribute->info, attribute->attribute_length);

    const u2 count = byte_cursor_u2(&c);
    if (!byte_cursor_ok(&c))
        return CLASSFILE_ERR_TRUNCATED;

    for (u2 i = 0; i < count; i++)
    {
        const u1 target_type = byte_cursor_u1(&c);
        if (!byte_cursor_ok(&c))
            return CLASSFILE_ERR_TRUNCATED;

        const classfile_status status = read_target(&c, target_type, attribute_index, refs);
        if (status != CLASSFILE_OK)
            return status;

        const u1 path_length = byte_cursor_u1(&c);
        if (!byte_cursor_ok(&c))
            return CLASSFILE_ERR_TRUNCATED;
        for (u1 k = 0; k < path_length; k++)
        {
            (void)byte_cursor_u1(&c);
            (void)byte_cursor_u1(&c);
        }
        if (!byte_cursor_ok(&c))
            return CLASSFILE_ERR_TRUNCATED;

        if (!skip_annotation(&c, 0))
            return CLASSFILE_ERR_TRUNCATED;
    }

    return byte_cursor_exhausted(&c) ? CLASSFILE_OK : CLASSFILE_ERR_TRAILING_BYTES;
}

static void write_u2(u1 *payload, u4 at, u2 value)
{
    payload[at] = (u1)(value >> 8);
    payload[at + 1] = (u1)(value & 0xFF);
}

classfile_status type_annotation_flush(const type_annotation_refs *refs,
                                       attribute_list *attributes)
{
    for (u2 i = 0; i < refs->count; i++)
    {
        const type_annotation_ref *ref = &refs->items[i];
        if (ref->attribute_index >= attributes->count)
            return CLASSFILE_ERR_UNSUPPORTED;

        attribute_info *attribute = &attributes->items[ref->attribute_index];
        const u4 span = ref->kind == TYPE_ANNOTATION_SPAN ? 4u : 2u;
        if (attribute->info == NULL || ref->payload_offset + span > attribute->attribute_length)
            return CLASSFILE_ERR_UNSUPPORTED;

        if (ref->start < 0 || ref->start > (i4)JVM_MAX_CODE_LENGTH)
            return CLASSFILE_ERR_CODE_TOO_LARGE;
        write_u2(attribute->info, ref->payload_offset, (u2)ref->start);

        if (ref->kind == TYPE_ANNOTATION_SPAN)
        {
            if (ref->end < ref->start || ref->end > (i4)JVM_MAX_CODE_LENGTH)
                return CLASSFILE_ERR_CODE_TOO_LARGE;
            write_u2(attribute->info, ref->payload_offset + 2u, (u2)(ref->end - ref->start));
        }
    }

    return CLASSFILE_OK;
}
