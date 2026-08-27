#include "jni2hook/jni2hook.h"

#include "jni2hook/utils/class_file_parser.h"

#include <stdlib.h>
#include <string.h>

static char *duplicate_utf8(const constant_pool *pool, u2 index)
{
    const u1 *bytes = NULL;
    u2 length = 0;
    if (!constant_pool_utf8(pool, index, &bytes, &length))
        return NULL;

    char *copy = malloc((size_t)length + 1);
    if (copy == NULL)
        return NULL;
    memcpy(copy, bytes, length);
    copy[length] = '\0';
    return copy;
}

void JNI2Hook_FreeMethodLayout(jni2hook_method_layout *layout)
{
    if (layout == NULL)
        return;
    for (size_t i = 0; i < layout->count; i++)
    {
        free(layout->methods[i].name);
        free(layout->methods[i].descriptor);
    }
    free(layout->methods);
    memset(layout, 0, sizeof(*layout));
}

jni2hook_status JNI2Hook_ReadMethodLayout(const unsigned char *class_bytes,
                                          size_t class_size,
                                          jni2hook_method_layout *out_layout)
{
    if (class_bytes == NULL || class_size == 0 || out_layout == NULL)
        return JNI2HOOK_ERR_CLASS_FILE;
    memset(out_layout, 0, sizeof(*out_layout));

    ClassFile *cf = NULL;
    if (classfile_parse(class_bytes, class_size, &cf) != CLASSFILE_OK)
        return JNI2HOOK_ERR_CLASS_FILE;

    const size_t count = cf->methods.count;
    jni2hook_method_info *methods = count != 0 ? calloc(count, sizeof(*methods)) : NULL;
    if (count != 0 && methods == NULL)
    {
        classFile_destroy(cf);
        return JNI2HOOK_ERR_OUT_OF_MEMORY;
    }

    out_layout->methods = methods;
    out_layout->count = count;
    for (size_t i = 0; i < count; i++)
    {
        if (!constant_pool_utf8(&cf->constant_pool, cf->methods.items[i].name_index, NULL, NULL) ||
            !constant_pool_utf8(&cf->constant_pool, cf->methods.items[i].descriptor_index, NULL, NULL))
        {
            classFile_destroy(cf);
            JNI2Hook_FreeMethodLayout(out_layout);
            return JNI2HOOK_ERR_CLASS_FILE;
        }

        methods[i].name = duplicate_utf8(&cf->constant_pool, cf->methods.items[i].name_index);
        methods[i].descriptor =
            duplicate_utf8(&cf->constant_pool, cf->methods.items[i].descriptor_index);
        if (methods[i].name == NULL || methods[i].descriptor == NULL)
        {
            classFile_destroy(cf);
            JNI2Hook_FreeMethodLayout(out_layout);
            return JNI2HOOK_ERR_OUT_OF_MEMORY;
        }
    }

    classFile_destroy(cf);
    return JNI2HOOK_OK;
}
