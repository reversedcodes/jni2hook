#ifndef JNI2HOOK_HOOK_CLASS_WATCH_H
#define JNI2HOOK_HOOK_CLASS_WATCH_H

#include "jni2hook/jni2hook.h"

jni2hook_status class_watch_create(JNIEnv *env, const char *pattern,
                                    jni2hook_method_watch **out_watch);
jni2hook_status class_watch_get(jni2hook_method_watch *watch, jmethodID *out_method,
                                 uint32_t *out_offset);
void class_watch_destroy(JNIEnv *env, jni2hook_method_watch *watch);

/* Resolves a match found by the ordinary loaded-class scan. This can race with
   the load callbacks; whichever path reaches READY first owns the result. */
jni2hook_status class_watch_resolve_loaded(JNIEnv *env, jvmtiEnv *jvmti,
                                           jni2hook_method_watch *watch,
                                           jmethodID method, uint32_t offset);

void class_watch_on_class_file_load(JNIEnv *env, jobject loader, jint class_data_len,
                                    const unsigned char *class_data);
void class_watch_on_class_prepare(jvmtiEnv *jvmti, JNIEnv *env, jclass klass);
void class_watch_clear(JNIEnv *env);

#endif
