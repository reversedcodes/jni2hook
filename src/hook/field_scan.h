#ifndef JNI2HOOK_FIELD_SCAN_H
#define JNI2HOOK_FIELD_SCAN_H

#include "jni2hook/jni2hook.h"

/* Resolves a field the way a bytecode signature reaches it: find the method the
   pattern matches, step to a field access instruction inside it, and follow its
   constant pool index to the field it names.
 *
 * This is how a field whose name is obfuscated can still be found — the access
 * instruction is part of code that a signature already identifies, so the field
 * is named by position rather than by name. */
jni2hook_status field_scan_find_in_class(JNIEnv *env,
                                         jvmtiEnv *jvmti,
                                         jclass target,
                                         const char *pattern,
                                         uint32_t instruction_offset,
                                         jfieldID *out_field,
                                         int *out_is_static,
                                         jvmtiError *out_error);

#endif
