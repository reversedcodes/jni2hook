#include "field_scan.h"

#include "bytecode_scan.h"
#include "visitors/cp_info.h"

#include <stdlib.h>
#include <string.h>

static bool utf8_to_buffer(const constant_pool *pool, u2 index, char *out, size_t size)
{
    const u1 *bytes = NULL;
    u2 length = 0;
    if (!constant_pool_utf8(pool, index, &bytes, &length) || (size_t)length + 1 > size)
        return false;
    memcpy(out, bytes, length);
    out[length] = 0;
    return true;
}

/* GetConstantPool hands back the entries without the count that precedes them
   in a class file, so it is put back in front and the ordinary reader is used
   rather than a second parser that could drift from the first. */
static classfile_status read_pool_of(jvmtiEnv *jvmti, jclass target, constant_pool *out)
{
    jint count = 0;
    jint byte_count = 0;
    unsigned char *bytes = NULL;

    if ((*jvmti)->GetConstantPool(jvmti, target, &count, &byte_count, &bytes) != JVMTI_ERROR_NONE)
        return CLASSFILE_ERR_TRUNCATED;
    if (bytes == NULL || byte_count < 0 || count < 1)
    {
        if (bytes != NULL)
            (*jvmti)->Deallocate(jvmti, bytes);
        return CLASSFILE_ERR_TRUNCATED;
    }

    const size_t total = (size_t)byte_count + 2u;
    u1 *buffer = malloc(total);
    if (buffer == NULL)
    {
        (*jvmti)->Deallocate(jvmti, bytes);
        return CLASSFILE_ERR_OUT_OF_MEMORY;
    }

    buffer[0] = (u1)(((u2)count >> 8) & 0xFF);
    buffer[1] = (u1)((u2)count & 0xFF);
    memcpy(buffer + 2, bytes, (size_t)byte_count);
    (*jvmti)->Deallocate(jvmti, bytes);

    byte_cursor c;
    byte_cursor_init(&c, buffer, total);

    constant_pool_init(out);
    const classfile_status status = constant_pool_read(&c, out);
    free(buffer);

    if (status != CLASSFILE_OK)
        constant_pool_free(out);
    return status;
}

jni2hook_status field_scan_find_in_class(JNIEnv *env,
                                         jvmtiEnv *jvmti,
                                         jclass target,
                                         const char *pattern,
                                         uint32_t instruction_offset,
                                         jfieldID *out_field,
                                         int *out_is_static,
                                         jvmtiError *out_error)
{
    if (out_error != NULL)
        *out_error = JVMTI_ERROR_NONE;
    if (env == NULL || jvmti == NULL || target == NULL || pattern == NULL || out_field == NULL)
        return JNI2HOOK_ERR_NOT_FOUND;

    jmethodID method = NULL;
    uint32_t match_offset = 0;
    const jni2hook_status found =
        bytecode_scan_find_in_class(jvmti, target, pattern, &method, &match_offset, out_error);
    if (found != JNI2HOOK_OK)
        return found;

    jint length = 0;
    unsigned char *code = NULL;
    if ((*jvmti)->GetBytecodes(jvmti, method, &length, &code) != JVMTI_ERROR_NONE || code == NULL)
    {
        if (code != NULL)
            (*jvmti)->Deallocate(jvmti, code);
        return JNI2HOOK_ERR_JVMTI;
    }

    const size_t at = (size_t)match_offset + instruction_offset;
    if (at + 3u > (size_t)length)
    {
        (*jvmti)->Deallocate(jvmti, code);
        return JNI2HOOK_ERR_NOT_FOUND;
    }

    const u1 opcode = code[at];
    if (opcode != JVM_OPC_getstatic && opcode != JVM_OPC_putstatic &&
        opcode != JVM_OPC_getfield && opcode != JVM_OPC_putfield)
    {
        (*jvmti)->Deallocate(jvmti, code);
        return JNI2HOOK_ERR_NOT_FOUND;
    }

    const u2 field_index = (u2)(((u2)code[at + 1] << 8) | (u2)code[at + 2]);
    (*jvmti)->Deallocate(jvmti, code);

    constant_pool pool;
    if (read_pool_of(jvmti, target, &pool) != CLASSFILE_OK)
        return JNI2HOOK_ERR_JVMTI;

    const cp_info *field = constant_pool_at(&pool, field_index);
    if (field == NULL || field->tag != JVM_CONSTANT_Fieldref)
    {
        constant_pool_free(&pool);
        return JNI2HOOK_ERR_NOT_FOUND;
    }

    const cp_info *nat = constant_pool_at(&pool, field->u.ref.nat_index);
    if (nat == NULL || nat->tag != JVM_CONSTANT_NameAndType)
    {
        constant_pool_free(&pool);
        return JNI2HOOK_ERR_NOT_FOUND;
    }

    char name[256];
    char descriptor[512];
    const bool named = utf8_to_buffer(&pool, nat->u.nat.name_index, name, sizeof(name)) &&
                       utf8_to_buffer(&pool, nat->u.nat.descriptor_index, descriptor,
                                      sizeof(descriptor));
    constant_pool_free(&pool);
    if (!named)
        return JNI2HOOK_ERR_NOT_FOUND;

    const bool is_static = opcode == JVM_OPC_getstatic || opcode == JVM_OPC_putstatic;
    jfieldID resolved = is_static ? (*env)->GetStaticFieldID(env, target, name, descriptor)
                                  : (*env)->GetFieldID(env, target, name, descriptor);
    if (resolved == NULL)
    {
        (*env)->ExceptionClear(env);
        return JNI2HOOK_ERR_NOT_FOUND;
    }

    *out_field = resolved;
    if (out_is_static != NULL)
        *out_is_static = is_static ? 1 : 0;
    return JNI2HOOK_OK;
}
