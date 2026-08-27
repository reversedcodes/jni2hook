#include "jni2hook/utils/class_file_parser.h"

#include "jni2hook/utils/byte_stream.h"

#include <string.h>

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

    status = constant_pool_read(&c, &cf->constant_pool);
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
