#ifndef JNI2HOOK_BYTECODE_SCAN_H
#define JNI2HOOK_BYTECODE_SCAN_H

#include "jni2hook/jni2hook.h"

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
