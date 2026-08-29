#ifndef JNI2HOOK_BYTECODE_SCAN_H
#define JNI2HOOK_BYTECODE_SCAN_H

#include "jni2hook/jni2hook.h"
#include "jni2hook/utils/visitors/class_file.h"

#include <stdbool.h>

typedef struct
{
    unsigned char *bytes;
    unsigned char *masks;
    size_t length;
} bytecode_pattern;

jni2hook_status bytecode_pattern_compile(const char *text, bytecode_pattern *out);
void bytecode_pattern_destroy(bytecode_pattern *pattern);

/* Matches against a parsed class file. This is the path used by
   ClassFileLoadHook, before the VM has created a jclass or jmethodID. */
bool bytecode_pattern_find_in_class_file(const bytecode_pattern *pattern,
                                         const ClassFile *class_file,
                                         size_t *out_method_index,
                                         uint32_t *out_offset);

/* Single-class counterpart for the narrow case where watch registration lands
   after ClassFileLoadHook but before ClassPrepare. */
bool bytecode_pattern_find_in_prepared_class(jvmtiEnv *jvmti, jclass target,
                                             const bytecode_pattern *pattern,
                                             jmethodID *out_method,
                                             uint32_t *out_offset);

jni2hook_status bytecode_scan_find(JNIEnv *env,
                                   jvmtiEnv *jvmti,
                                   const char *pattern,
                                   jmethodID *out_method,
                                   uint32_t *out_offset,
                                   jni2hook_search_stats *out_stats,
                                   jvmtiError *out_error);

jni2hook_status bytecode_scan_find_in_class(jvmtiEnv *jvmti,
                                            jclass target,
                                            const char *pattern,
                                            jmethodID *out_method,
                                            uint32_t *out_offset,
                                            jvmtiError *out_error);

#endif
