#ifndef JNI2HOOK_H
#define JNI2HOOK_H

#include <stddef.h>
#include <stdint.h>

#include <jni.h>
#include <jvmti.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum
{
    JNI2HOOK_OK = 0,
    JNI2HOOK_ERR_NOT_INITIALIZED,
    JNI2HOOK_ERR_NO_JNI,
    JNI2HOOK_ERR_NO_JVMTI,
    JNI2HOOK_ERR_CAPABILITIES,
    JNI2HOOK_ERR_JVMTI,
    JNI2HOOK_ERR_JNI,
    JNI2HOOK_ERR_JAVA_EXCEPTION,
    JNI2HOOK_ERR_CLASS_NOT_CACHED,
    JNI2HOOK_ERR_CLASS_FILE,
    JNI2HOOK_ERR_TRANSFORM,
    JNI2HOOK_ERR_ALREADY_HOOKED,
    JNI2HOOK_ERR_NOT_HOOKED,
    JNI2HOOK_ERR_INVALID_PATTERN,
    JNI2HOOK_ERR_NOT_FOUND,
    JNI2HOOK_ERR_OUT_OF_MEMORY
} jni2hook_status;

typedef struct
{
    size_t classes_total;
    size_t classes_scanned;
    size_t classes_unavailable;
    size_t methods_scanned;
    size_t methods_unavailable;
    size_t methods_without_code;
} jni2hook_search_stats;

typedef struct
{
    char *name;
    char *descriptor;
} jni2hook_method_info;

typedef struct
{
    jni2hook_method_info *methods;
    size_t count;
} jni2hook_method_layout;

typedef struct jni2hook_method_watch jni2hook_method_watch;

const char *JNI2Hook_StatusMessage(jni2hook_status status);

/* The JVMTI error behind the last JNI2HOOK_ERR_JVMTI, for diagnostics. */
jvmtiError JNI2Hook_LastJvmtiError(void);

/* Result of handling AllowRedefinitionToAddDeleteMethods: 1 changed, 0 already
   enabled, -1 unavailable. The last value makes installs fail with JVMTI 63. */
int JNI2Hook_ForcedRedefinitionFlag(void);

/* Binds to the VM and acquires the JVMTI capabilities. Call once. */
jni2hook_status JNI2Hook_Init(JavaVM *vm);

/* Finds the running JVM and attaches the calling thread before initialization. */
jni2hook_status JNI2Hook_InitFromRunningVm(void);

/* Returns a JNIEnv, attaching the caller as a daemon when necessary. */
jni2hook_status JNI2Hook_Attach(JNIEnv **out_env);

/* Makes method native and binds it to native_function, parking the original
   body in a private copy that out_original then names.
 *
 * native_function must have the JNI signature of the method: JNIEnv*, then
 * jclass for a static method or jobject for an instance method, then the
 * declared arguments.
 *
 * A stale JIT/MethodHandle call site may enter native_function after uninstall.
 * Code that can be unloaded therefore needs a resident, disarmable trampoline. */
jni2hook_status JNI2Hook_Install(jmethodID method, void *native_function, jmethodID *out_original);

/* Inserts a call to native_function at bytecode_offset while leaving the
   method body in place. The offset names an instruction in the original class
   body; installing another call in the same method does not shift it. For an
   instance constructor, an offset before the initializing this()/super() call
   is moved directly after that call.

   native_function has the signature (JNIEnv *, jobject) for an instance
   method and (JNIEnv *, jclass) for a static method, and returns void. */
jni2hook_status JNI2Hook_InstallAt(jmethodID method, uint32_t bytecode_offset,
                                   void *native_function);

/* Finds the first method whose bytecode contains pattern. Tokens are two hex
   digits separated by whitespace; ? and ?? match any byte. The returned offset
   is relative to the original method bytecode and can be passed directly to
   JNI2Hook_InstallAt. */
jni2hook_status JNI2Hook_FindMethod(const char *pattern, jmethodID *out_method,
                                    uint32_t *out_bytecode_offset,
                                    jni2hook_search_stats *out_stats);

jni2hook_status JNI2Hook_FindMethodInClass(jclass target, const char *pattern,
                                           jmethodID *out_method, uint32_t *out_bytecode_offset);

/* Watches loaded and future classes. Initial class bytes are parsed before a
 * jclass exists, then ClassPrepare resolves the match to a jmethodID.
 *
 * GetWatchedMethod returns JNI2HOOK_ERR_NOT_FOUND while the watch is pending.
 * Destroy the watch after consuming the result or when no longer needed. */
jni2hook_status JNI2Hook_WatchMethod(const char *pattern,
                                     jni2hook_method_watch **out_watch);
jni2hook_status JNI2Hook_GetWatchedMethod(jni2hook_method_watch *watch,
                                          jmethodID *out_method,
                                          uint32_t *out_bytecode_offset);
void JNI2Hook_DestroyMethodWatch(jni2hook_method_watch *watch);

/* Resolves the field access at instruction_offset inside a pattern match. */
jni2hook_status JNI2Hook_FindFieldInClass(jclass target, const char *pattern,
                                          uint32_t instruction_offset, jfieldID *out_field,
                                          int *out_is_static);

/* Reads method names and descriptors in their original class-file order. The
   returned layout owns all memory and must be released with the matching free
   function. This parser does not require JNI2Hook_Init or a running JVM. */
jni2hook_status JNI2Hook_ReadMethodLayout(const unsigned char *class_bytes, size_t class_size,
                                          jni2hook_method_layout *out_layout);

void JNI2Hook_FreeMethodLayout(jni2hook_method_layout *layout);

/* Restores method and removes all callbacks registered on it. Other methods in
 * the class stay hooked. On failure the detours remain live; do not unload. */
jni2hook_status JNI2Hook_Uninstall(jmethodID method);

/* Removes one inserted callback identified by method, offset and function.
   On failure the callback remains live. */
jni2hook_status JNI2Hook_UninstallAt(jmethodID method, uint32_t bytecode_offset,
                                     void *native_function);

int JNI2Hook_IsInstalled(jmethodID method);

/* Restores every hook and the VM flag, then releases JVMTI. A non-OK result
   means at least one detour remains live and the callback library must stay
   mapped. JNI2HOOK_OK still does not drain stale JIT call sites. */
jni2hook_status JNI2Hook_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
