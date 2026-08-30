#ifndef JNI2HOOK_HOOK_CLASS_CACHE_H
#define JNI2HOOK_HOOK_CLASS_CACHE_H

#include "jvm.h"

#include <stddef.h>

/* Caches each class's bytes once, before its first jni2hook redefinition. The
   bytes come from ClassFileLoadHook and entries are keyed by jclass identity. */

bool class_cache_start(void);
void class_cache_stop(void);

/* Keeps class load/prepare events enabled while unresolved watches exist. */
bool class_cache_set_watch_events(bool enabled);

/* Captures the class once. Safe to call repeatedly. */
bool class_cache_ensure(jclass klass, const char *class_name, jvmtiError *out_error);

/* Returned bytes remain cache-owned; the caller must prevent concurrent clear. */
const unsigned char *class_cache_get(JNIEnv *env, jclass klass, jint *out_size);
void                 class_cache_forget(JNIEnv *env, jclass klass);
void                 class_cache_clear(void);
size_t               class_cache_size(void);

#endif
