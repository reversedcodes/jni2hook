#include "bytecode_scan.h"

#include "jni2hook/utils/visitors/attribute_info.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static int hex_value(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

void bytecode_pattern_destroy(bytecode_pattern *pattern)
{
    if (pattern == NULL)
        return;
    free(pattern->bytes);
    free(pattern->masks);
    memset(pattern, 0, sizeof(*pattern));
}

jni2hook_status bytecode_pattern_compile(const char *text, bytecode_pattern *out)
{
    if (out == NULL)
        return JNI2HOOK_ERR_INVALID_PATTERN;
    memset(out, 0, sizeof(*out));
    if (text == NULL)
        return JNI2HOOK_ERR_INVALID_PATTERN;

    size_t count = 0;
    const char *cursor = text;
    while (*cursor != '\0')
    {
        while (isspace((unsigned char)*cursor) != 0)
            cursor++;
        if (*cursor == '\0')
            break;
        count++;
        while (*cursor != '\0' && isspace((unsigned char)*cursor) == 0)
            cursor++;
    }

    if (count == 0)
        return JNI2HOOK_ERR_INVALID_PATTERN;

    out->bytes = malloc(count);
    out->masks = malloc(count);
    if (out->bytes == NULL || out->masks == NULL)
    {
        bytecode_pattern_destroy(out);
        return JNI2HOOK_ERR_OUT_OF_MEMORY;
    }

    cursor = text;
    while (*cursor != '\0')
    {
        while (isspace((unsigned char)*cursor) != 0)
            cursor++;
        if (*cursor == '\0')
            break;

        const char *begin = cursor;
        while (*cursor != '\0' && isspace((unsigned char)*cursor) == 0)
            cursor++;
        const size_t length = (size_t)(cursor - begin);

        if ((length == 1 && begin[0] == '?') ||
            (length == 2 && begin[0] == '?' && begin[1] == '?'))
        {
            out->bytes[out->length] = 0;
            out->masks[out->length] = 0;
        }
        else
        {
            const int high = length == 2 ? hex_value(begin[0]) : -1;
            const int low = length == 2 ? hex_value(begin[1]) : -1;
            if (high < 0 || low < 0)
            {
                bytecode_pattern_destroy(out);
                return JNI2HOOK_ERR_INVALID_PATTERN;
            }
            out->bytes[out->length] = (unsigned char)((high << 4) | low);
            out->masks[out->length] = 0xFFu;
        }
        out->length++;
    }

    return JNI2HOOK_OK;
}

static const unsigned char *bytecode_pattern_find(const bytecode_pattern *pattern,
                                                  const unsigned char *bytes,
                                                  size_t length)
{
    if (bytes == NULL || length < pattern->length)
        return NULL;

    const size_t last = length - pattern->length;
    for (size_t offset = 0; offset <= last; offset++)
    {
        bool matches = true;
        for (size_t i = 0; i < pattern->length; i++)
        {
            if (pattern->masks[i] != 0 && bytes[offset + i] != pattern->bytes[i])
            {
                matches = false;
                break;
            }
        }
        if (matches)
            return bytes + offset;
    }
    return NULL;
}

bool bytecode_pattern_find_in_class_file(const bytecode_pattern *pattern,
                                         const ClassFile *class_file,
                                         size_t *out_method_index,
                                         uint32_t *out_offset)
{
    if (pattern == NULL || class_file == NULL)
        return false;

    for (u2 i = 0; i < class_file->methods.count; i++)
    {
        const method_info *method = &class_file->methods.items[i];
        const attribute_info *code =
            attribute_list_find(&method->attributes, &class_file->constant_pool, "Code");
        if (code == NULL || code->attribute_length < 8)
            continue;

        const unsigned char *info = code->info;
        const uint32_t length = ((uint32_t)info[4] << 24) | ((uint32_t)info[5] << 16) |
                                ((uint32_t)info[6] << 8) | (uint32_t)info[7];
        if ((uint64_t)length + 8u > code->attribute_length)
            continue;

        const unsigned char *match = bytecode_pattern_find(pattern, info + 8, length);
        if (match == NULL)
            continue;

        if (out_method_index != NULL)
            *out_method_index = i;
        if (out_offset != NULL)
            *out_offset = (uint32_t)(match - (info + 8));
        return true;
    }
    return false;
}

static bool scan_prepared_class(jvmtiEnv *jvmti,
                                jclass target,
                                const bytecode_pattern *pattern,
                                jmethodID *out_method,
                                uint32_t *out_offset,
                                jni2hook_search_stats *stats)
{
    jint method_count = 0;
    jmethodID *methods = NULL;
    if ((*jvmti)->GetClassMethods(jvmti, target, &method_count, &methods) != JVMTI_ERROR_NONE)
    {
        if (stats != NULL)
            stats->classes_unavailable++;
        return false;
    }

    if (stats != NULL)
        stats->classes_scanned++;

    bool found = false;
    for (jint i = 0; i < method_count && !found; i++)
    {
        jint bytecode_length = 0;
        unsigned char *bytecode = NULL;
        const jvmtiError error =
            (*jvmti)->GetBytecodes(jvmti, methods[i], &bytecode_length, &bytecode);
        if (error != JVMTI_ERROR_NONE)
        {
            if (stats != NULL)
            {
                if (error == JVMTI_ERROR_NATIVE_METHOD || error == JVMTI_ERROR_ABSENT_INFORMATION)
                    stats->methods_without_code++;
                else
                    stats->methods_unavailable++;
            }
            continue;
        }

        if (bytecode == NULL || bytecode_length <= 0)
        {
            if (stats != NULL)
                stats->methods_without_code++;
            if (bytecode != NULL)
                (*jvmti)->Deallocate(jvmti, bytecode);
            continue;
        }

        if (stats != NULL)
            stats->methods_scanned++;

        const unsigned char *match =
            bytecode_pattern_find(pattern, bytecode, (size_t)bytecode_length);
        if (match != NULL)
        {
            *out_method = methods[i];
            *out_offset = (uint32_t)(match - bytecode);
            found = true;
        }

        (*jvmti)->Deallocate(jvmti, bytecode);
    }

    (*jvmti)->Deallocate(jvmti, (unsigned char *)methods);
    return found;
}

bool bytecode_pattern_find_in_prepared_class(jvmtiEnv *jvmti, jclass target,
                                             const bytecode_pattern *pattern,
                                             jmethodID *out_method,
                                             uint32_t *out_offset)
{
    if (jvmti == NULL || target == NULL || pattern == NULL || out_method == NULL ||
        out_offset == NULL)
        return false;
    return scan_prepared_class(jvmti, target, pattern, out_method, out_offset, NULL);
}

jni2hook_status bytecode_scan_find(JNIEnv *env,
                                   jvmtiEnv *jvmti,
                                   const char *pattern_text,
                                   jmethodID *out_method,
                                   uint32_t *out_offset,
                                   jni2hook_search_stats *out_stats,
                                   jvmtiError *out_error)
{
    bytecode_pattern pattern;
    jni2hook_status status = bytecode_pattern_compile(pattern_text, &pattern);
    if (status != JNI2HOOK_OK)
        return status;

    if (out_stats != NULL)
        memset(out_stats, 0, sizeof(*out_stats));

    jint class_count = 0;
    jclass *classes = NULL;
    const jvmtiError error = (*jvmti)->GetLoadedClasses(jvmti, &class_count, &classes);
    if (error != JVMTI_ERROR_NONE)
    {
        *out_error = error;
        bytecode_pattern_destroy(&pattern);
        return JNI2HOOK_ERR_JVMTI;
    }

    if (out_stats != NULL)
        out_stats->classes_total = (size_t)class_count;

    bool found = false;
    for (jint i = 0; i < class_count; i++)
    {
        if (!found)
            found = scan_prepared_class(jvmti, classes[i], &pattern, out_method, out_offset,
                                        out_stats);
        (*env)->DeleteLocalRef(env, classes[i]);
    }

    (*jvmti)->Deallocate(jvmti, (unsigned char *)classes);
    bytecode_pattern_destroy(&pattern);
    return found ? JNI2HOOK_OK : JNI2HOOK_ERR_NOT_FOUND;
}

jni2hook_status bytecode_scan_find_in_class(jvmtiEnv *jvmti,
                                            jclass target,
                                            const char *pattern_text,
                                            jmethodID *out_method,
                                            uint32_t *out_offset,
                                            jvmtiError *out_error)
{
    (void)out_error;
    bytecode_pattern pattern;
    jni2hook_status status = bytecode_pattern_compile(pattern_text, &pattern);
    if (status != JNI2HOOK_OK)
        return status;

    const bool found = scan_prepared_class(jvmti, target, &pattern, out_method, out_offset, NULL);
    bytecode_pattern_destroy(&pattern);
    return found ? JNI2HOOK_OK : JNI2HOOK_ERR_NOT_FOUND;
}
