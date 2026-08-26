#ifndef JNI2HOOK_HOOK_CLASS_CACHE_H
#define JNI2HOOK_HOOK_CLASS_CACHE_H

#include "jvm.h"

#include <stddef.h>

/* Keeps the class file bytes a class had before jni2hook touched it.
 *
 * Getting at those bytes is the awkward part of the whole library. A loaded
 * class does not hand its class file back, so the only way is to ask the VM to
 * retransform it and catch the bytes in a ClassFileLoadHook. Three things about
 * that are easy to get wrong:
 *
 * 1. The bytes must be captured exactly once, before the first redefinition.
 *    RedefineClasses replaces the baseline that a later retransformation starts
 *    from, so capturing again after a hook is installed would hand back our own
 *    rewrite and the original body would be gone for good.
 *
 * 2. The hook has to be switched off again right after. It fires for every
 *    class the VM loads while enabled, on every thread, and leaving it on also
 *    breaks JNI DefineClass calls elsewhere in the process.
 *
 * 3. What arrives is the class as it currently stands, which for a modded game
 *    means after Mixin and friends have had their turn, not the pristine bytes
 *    from the jar. That is what we want, and it is the reason the bytes cannot
 *    simply be read from the jar on disk.
 */

bool class_cache_start(void);
void class_cache_stop(void);

/* Captures the class if it is not cached yet. Safe to call repeatedly. */
bool class_cache_ensure(jclass klass, const char *class_name, jvmtiError *out_error);

const unsigned char *class_cache_get(const char *class_name, jint *out_size);
void                 class_cache_forget(const char *class_name);
void                 class_cache_clear(void);
size_t               class_cache_size(void);

#endif
