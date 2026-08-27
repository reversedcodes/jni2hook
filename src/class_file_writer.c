#include "jni2hook/utils/class_file_parser.h"

#include "jni2hook/utils/byte_stream.h"

static void write_constant_pool(byte_buffer *b, const constant_pool *pool)
{
    byte_buffer_u2(b, pool->count);

    for (u2 i = 1; i < pool->count; i++)
    {
        const cp_info *entry = &pool->entries[i];
        if (entry->tag == 0)
            continue; /* empty slot behind a Long or a Double */

        byte_buffer_u1(b, entry->tag);
        switch (entry->tag)
        {
        case JVM_CONSTANT_Utf8:
            byte_buffer_u2(b, entry->u.utf8.length);
            byte_buffer_bytes(b, entry->u.utf8.bytes, entry->u.utf8.length);
            break;
        case JVM_CONSTANT_Integer:
        case JVM_CONSTANT_Float:
            byte_buffer_bytes(b, entry->u.numeric.bytes, 4);
            break;
        case JVM_CONSTANT_Long:
        case JVM_CONSTANT_Double:
            byte_buffer_bytes(b, entry->u.numeric.bytes, 8);
            break;
        case JVM_CONSTANT_Class:
        case JVM_CONSTANT_Module:
        case JVM_CONSTANT_Package:
            byte_buffer_u2(b, entry->u.class_info.name_index);
            break;
        case JVM_CONSTANT_String:
            byte_buffer_u2(b, entry->u.string_info.string_index);
            break;
        case JVM_CONSTANT_Fieldref:
        case JVM_CONSTANT_Methodref:
        case JVM_CONSTANT_InterfaceMethodref:
            byte_buffer_u2(b, entry->u.ref.class_index);
            byte_buffer_u2(b, entry->u.ref.nat_index);
            break;
        case JVM_CONSTANT_NameAndType:
            byte_buffer_u2(b, entry->u.nat.name_index);
            byte_buffer_u2(b, entry->u.nat.descriptor_index);
            break;
        case JVM_CONSTANT_MethodHandle:
            byte_buffer_u1(b, entry->u.method_handle.reference_kind);
            byte_buffer_u2(b, entry->u.method_handle.reference_index);
            break;
        case JVM_CONSTANT_MethodType:
            byte_buffer_u2(b, entry->u.method_type.descriptor_index);
            break;
        case JVM_CONSTANT_Dynamic:
        case JVM_CONSTANT_InvokeDynamic:
            byte_buffer_u2(b, entry->u.dynamic.bootstrap_method_attr_index);
            byte_buffer_u2(b, entry->u.dynamic.nat_index);
            break;
        default:
            b->ok = false;
            return;
        }
    }
}

static void write_attributes(byte_buffer *b, const attribute_list *list)
{
    byte_buffer_u2(b, list->count);
    for (u2 i = 0; i < list->count; i++)
    {
        byte_buffer_u2(b, list->items[i].attribute_name_index);
        byte_buffer_u4(b, list->items[i].attribute_length);
        byte_buffer_bytes(b, list->items[i].info, list->items[i].attribute_length);
    }
}

static void write_members(byte_buffer *b, const member_list *list)
{
    byte_buffer_u2(b, list->count);
    for (u2 i = 0; i < list->count; i++)
    {
        byte_buffer_u2(b, list->items[i].access_flags);
        byte_buffer_u2(b, list->items[i].name_index);
        byte_buffer_u2(b, list->items[i].descriptor_index);
        write_attributes(b, &list->items[i].attributes);
    }
}

classfile_status classfile_serialize(const ClassFile *cf, u1 **out, size_t *out_size)
{
    if (cf == NULL || out == NULL || out_size == NULL)
        return CLASSFILE_ERR_TRUNCATED;

    byte_buffer b;
    byte_buffer_init(&b);
    byte_buffer_reserve(&b, 8192);

    byte_buffer_u4(&b, JAVA_CLASSFILE_MAGIC);
    byte_buffer_u2(&b, cf->minor_version);
    byte_buffer_u2(&b, cf->major_version);

    write_constant_pool(&b, &cf->constant_pool);

    byte_buffer_u2(&b, cf->access_flags);
    byte_buffer_u2(&b, cf->this_class);
    byte_buffer_u2(&b, cf->super_class);

    byte_buffer_u2(&b, cf->interfaces.count);
    for (u2 i = 0; i < cf->interfaces.count; i++)
        byte_buffer_u2(&b, cf->interfaces.items[i]);

    write_members(&b, &cf->fields);
    write_members(&b, &cf->methods);
    write_attributes(&b, &cf->attributes);

    if (!byte_buffer_ok(&b))
    {
        byte_buffer_free(&b);
        return CLASSFILE_ERR_OUT_OF_MEMORY;
    }

    *out = byte_buffer_release(&b, out_size);
    return CLASSFILE_OK;
}
