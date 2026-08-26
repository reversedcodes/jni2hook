#include "class_file_parser.h"

#include "byte_stream.h"

#include <string.h>

static classfile_status read_constant_pool(byte_cursor *c, constant_pool *pool)
{
    const u2 count = byte_cursor_u2(c);
    if (!byte_cursor_ok(c))
        return CLASSFILE_ERR_TRUNCATED;
    if (count < 1)
        return CLASSFILE_ERR_CONSTANT_INDEX;

    const classfile_status reserved = constant_pool_reserve(pool, count);
    if (reserved != CLASSFILE_OK)
        return reserved;
    pool->count = count;

    for (u2 i = 1; i < count; i++)
    {
        cp_info *entry = &pool->entries[i];
        entry->tag = byte_cursor_u1(c);

        switch (entry->tag)
        {
        case JVM_CONSTANT_Utf8:
        {
            const u2 length = byte_cursor_u2(c);
            const u1 *bytes = byte_cursor_bytes(c, length);
            if (bytes == NULL)
                return CLASSFILE_ERR_TRUNCATED;
            entry->tag = 0; /* set_utf8 frees the previous value, keep it inert */
            const classfile_status status = cp_info_set_utf8(entry, bytes, length);
            if (status != CLASSFILE_OK)
                return status;
            break;
        }
        case JVM_CONSTANT_Integer:
        case JVM_CONSTANT_Float:
        {
            const u1 *value = byte_cursor_bytes(c, 4);
            if (value == NULL)
                return CLASSFILE_ERR_TRUNCATED;
            memcpy(entry->u.numeric.bytes, value, 4);
            break;
        }
        case JVM_CONSTANT_Long:
        case JVM_CONSTANT_Double:
        {
            const u1 *value = byte_cursor_bytes(c, 8);
            if (value == NULL)
                return CLASSFILE_ERR_TRUNCATED;
            memcpy(entry->u.numeric.bytes, value, 8);
            /* JVMS 4.4.5: the following slot stays empty and is unusable. */
            i++;
            break;
        }
        case JVM_CONSTANT_Class:
        case JVM_CONSTANT_Module:
        case JVM_CONSTANT_Package:
            entry->u.class_info.name_index = byte_cursor_u2(c);
            break;
        case JVM_CONSTANT_String:
            entry->u.string_info.string_index = byte_cursor_u2(c);
            break;
        case JVM_CONSTANT_Fieldref:
        case JVM_CONSTANT_Methodref:
        case JVM_CONSTANT_InterfaceMethodref:
            entry->u.ref.class_index = byte_cursor_u2(c);
            entry->u.ref.nat_index = byte_cursor_u2(c);
            break;
        case JVM_CONSTANT_NameAndType:
            entry->u.nat.name_index = byte_cursor_u2(c);
            entry->u.nat.descriptor_index = byte_cursor_u2(c);
            break;
        case JVM_CONSTANT_MethodHandle:
            entry->u.method_handle.reference_kind = byte_cursor_u1(c);
            entry->u.method_handle.reference_index = byte_cursor_u2(c);
            break;
        case JVM_CONSTANT_MethodType:
            entry->u.method_type.descriptor_index = byte_cursor_u2(c);
            break;
        case JVM_CONSTANT_Dynamic:
        case JVM_CONSTANT_InvokeDynamic:
            entry->u.dynamic.bootstrap_method_attr_index = byte_cursor_u2(c);
            entry->u.dynamic.nat_index = byte_cursor_u2(c);
            break;
        default:
            return byte_cursor_ok(c) ? CLASSFILE_ERR_CONSTANT_TAG : CLASSFILE_ERR_TRUNCATED;
        }

        if (!byte_cursor_ok(c))
            return CLASSFILE_ERR_TRUNCATED;
    }

    return CLASSFILE_OK;
}

static classfile_status read_attributes(byte_cursor *c, attribute_list *list)
{
    const u2 count = byte_cursor_u2(c);
    if (!byte_cursor_ok(c))
        return CLASSFILE_ERR_TRUNCATED;

    classfile_status status = attribute_list_reserve(list, count);
    if (status != CLASSFILE_OK)
        return status;

    for (u2 i = 0; i < count; i++)
    {
        attribute_info *attribute = NULL;
        status = attribute_list_append(list, &attribute);
        if (status != CLASSFILE_OK)
            return status;

        attribute->attribute_name_index = byte_cursor_u2(c);
        const u4 length = byte_cursor_u4(c);

        const u1 *payload = byte_cursor_bytes(c, length);
        if (payload == NULL)
            return CLASSFILE_ERR_TRUNCATED;

        status = attribute_info_set_info(attribute, payload, length);
        if (status != CLASSFILE_OK)
            return status;
    }

    return CLASSFILE_OK;
}

static classfile_status read_members(byte_cursor *c, member_list *list)
{
    const u2 count = byte_cursor_u2(c);
    if (!byte_cursor_ok(c))
        return CLASSFILE_ERR_TRUNCATED;

    classfile_status status = member_list_reserve(list, count);
    if (status != CLASSFILE_OK)
        return status;

    for (u2 i = 0; i < count; i++)
    {
        member_info *member = NULL;
        status = member_list_append(list, &member);
        if (status != CLASSFILE_OK)
            return status;

        member->access_flags = byte_cursor_u2(c);
        member->name_index = byte_cursor_u2(c);
        member->descriptor_index = byte_cursor_u2(c);
        if (!byte_cursor_ok(c))
            return CLASSFILE_ERR_TRUNCATED;

        status = read_attributes(c, &member->attributes);
        if (status != CLASSFILE_OK)
            return status;
    }

    return CLASSFILE_OK;
}

static classfile_status read_interfaces(byte_cursor *c, interface_list *list)
{
    const u2 count = byte_cursor_u2(c);
    if (!byte_cursor_ok(c))
        return CLASSFILE_ERR_TRUNCATED;

    const classfile_status status = interface_list_reserve(list, count);
    if (status != CLASSFILE_OK)
        return status;

    for (u2 i = 0; i < count; i++)
    {
        const classfile_status appended = interface_list_append(list, byte_cursor_u2(c));
        if (appended != CLASSFILE_OK)
            return appended;
    }

    return byte_cursor_ok(c) ? CLASSFILE_OK : CLASSFILE_ERR_TRUNCATED;
}

classfile_status classfile_parse(const u1 *data, size_t size, ClassFile **out)
{
    if (data == NULL || out == NULL)
        return CLASSFILE_ERR_TRUNCATED;

    *out = NULL;

    ClassFile *cf = classFile_create();
    if (cf == NULL)
        return CLASSFILE_ERR_OUT_OF_MEMORY;

    byte_cursor c;
    byte_cursor_init(&c, data, size);

    classfile_status status;

    if (byte_cursor_u4(&c) != JAVA_CLASSFILE_MAGIC)
    {
        status = byte_cursor_ok(&c) ? CLASSFILE_ERR_MAGIC : CLASSFILE_ERR_TRUNCATED;
        goto failed;
    }

    cf->minor_version = byte_cursor_u2(&c);
    cf->major_version = byte_cursor_u2(&c);
    if (!byte_cursor_ok(&c))
    {
        status = CLASSFILE_ERR_TRUNCATED;
        goto failed;
    }
    if (cf->major_version < JAVA_MIN_SUPPORTED_VERSION)
    {
        status = CLASSFILE_ERR_VERSION;
        goto failed;
    }

    status = read_constant_pool(&c, &cf->constant_pool);
    if (status != CLASSFILE_OK)
        goto failed;

    cf->access_flags = byte_cursor_u2(&c);
    cf->this_class = byte_cursor_u2(&c);
    cf->super_class = byte_cursor_u2(&c);
    if (!byte_cursor_ok(&c))
    {
        status = CLASSFILE_ERR_TRUNCATED;
        goto failed;
    }

    status = read_interfaces(&c, &cf->interfaces);
    if (status != CLASSFILE_OK)
        goto failed;

    status = read_members(&c, &cf->fields);
    if (status != CLASSFILE_OK)
        goto failed;

    status = read_members(&c, &cf->methods);
    if (status != CLASSFILE_OK)
        goto failed;

    status = read_attributes(&c, &cf->attributes);
    if (status != CLASSFILE_OK)
        goto failed;

    if (!byte_cursor_exhausted(&c))
    {
        status = CLASSFILE_ERR_TRAILING_BYTES;
        goto failed;
    }

    *out = cf;
    return CLASSFILE_OK;

failed:
    classFile_destroy(cf);
    return status;
}
