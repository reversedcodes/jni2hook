#ifndef JNI2HOOK_HOOK_CLASS_CACHE_H
#define JNI2HOOK_HOOK_CLASS_CACHE_H

#include "jvm.h"

#include <stddef.h>

/* Keeps the class file bytes a class had before jni2hook touched it.
 *
 * Getting at those bytes is the awkward part of the whole library. A loaded
 * class does not hand its class file back, so the only way is to ask the VM to
 * retransform it and catch the bytes in a ClassFileLoadHook. Four things about
 * that are easy to get wrong:
 *
 * 1. The bytes must be captured exactly once, before the first redefinition.
 *    RedefineClasses replaces the baseline that a later retransformation starts
 *    from, so capturing again after a hook is installed would hand back our own
 *    rewrite and the original body would be gone for good.
 *
 * 2. Capture-only mode is switched off immediately. Method watches deliberately
 *    keep the event enabled, but initial loads go only to the read-only parser
 *    and are never confused with the class currently being retransformed.
 *
 * 3. What arrives is the class as it currently stands, which for a modded game
 *    means after Mixin and friends have had their turn, not the pristine bytes
 *    from the jar. That is what we want, and it is the reason the bytes cannot
 *    simply be read from the jar on disk.
 *
 * 4. A binary name is not an identity. Two class loaders may each define a
 *    class of the same name, and a modded game has plenty of loaders, so every
 *    entry is keyed on the jclass itself and the callback matches on
 *    class_being_redefined rather than on the name it is handed.
 *
 * The callback fires on whichever thread happens to be loading a class, so
 * everything here is taken under a lock of its own.
 */

bool class_cache_start(void);
void class_cache_stop(void);

/* Keeps the class-file and class-prepare events enabled while unresolved
   method watches need them. The capture path still enables the file event
   temporarily when there are no watches. */
bool class_cache_set_watch_events(bool enabled);

/* Captures the class if it is not cached yet. Safe to call repeatedly. */
bool class_cache_ensure(jclass klass, const char *class_name, jvmtiError *out_error);

/* The bytes belong to the cache and stay valid until class_cache_clear. The
   caller has to hold jni2hook's own lock across the use, which is what keeps a
   concurrent shutdown from clearing the cache under the returned pointer. */
const unsigned char *class_cache_get(JNIEnv *env, jclass klass, jint *out_size);
void                 class_cache_forget(JNIEnv *env, jclass klass);
void                 class_cache_clear(void);
size_t               class_cache_size(void);

#endif
