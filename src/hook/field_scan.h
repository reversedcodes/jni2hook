#ifndef JNI2HOOK_FIELD_SCAN_H
#define JNI2HOOK_FIELD_SCAN_H

#include "jni2hook/jni2hook.h"

/* Resolves the field access at instruction_offset inside a pattern match. */
jni2hook_status field_scan_find_in_class(JNIEnv *env,
                                         jvmtiEnv *jvmti,
                                         jclass target,
                                         const char *pattern,
                                         uint32_t instruction_offset,
                                         jfieldID *out_field,
                                         int *out_is_static,
                                         jvmtiError *out_error);

#endif
