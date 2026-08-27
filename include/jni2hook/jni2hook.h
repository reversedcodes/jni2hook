#ifndef JNI2HOOK_H
#define JNI2HOOK_H

#include <stddef.h>
#include <stdint.h>

#include <jni.h>
#include <jvmti.h>

#ifdef __cplusplus
extern "C" {
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

const char *JNI2Hook_StatusMessage(jni2hook_status status);

/* The JVMTI error behind the last JNI2HOOK_ERR_JVMTI, for diagnostics. */
jvmtiError JNI2Hook_LastJvmtiError(void);

/* What Init made of AllowRedefinitionToAddDeleteMethods, the flag that gates
   every method the rewrite adds and is off by default on current JDKs:
     1  it was off and jni2hook switched it on
     0  it was already on, nothing to do
    -1  the flag could not be reached, so installing a hook will fail with
        JVMTI_ERROR_UNSUPPORTED_REDEFINITION_METHOD_ADDED
   Worth checking right after Init, because -1 only shows up as a failed install
   much later otherwise. */
int JNI2Hook_ForcedRedefinitionFlag(void);

/* Binds to the VM and acquires the JVMTI capabilities. Call once. */
jni2hook_status JNI2Hook_Init(JavaVM *vm);

/* Attaches the calling thread to the VM if it is not a Java thread yet and
   hands back its JNIEnv. Every other entry point does this for itself, so this
   is only needed when the caller wants a JNIEnv of its own. */
jni2hook_status JNI2Hook_Attach(JNIEnv **out_env);

/* Makes method native and binds it to native_function, parking the original
   body in a private copy that out_original then names.
 *
 * native_function must have the JNI signature of the method: JNIEnv*, then
 * jclass for a static method or jobject for an instance method, then the
 * declared arguments.
 *
 * Note what this does not solve: after JNI2Hook_Uninstall the VM may still
 * enter native_function for a while. A JIT compiled caller reached through a
 * MethodHandle call site keeps the old target and can arrive hundreds of
 * milliseconds later. If the code behind native_function can be unmapped, for
 * instance because it lives in a library that gets unloaded, it needs a
 * trampoline that stays mapped and can be disarmed. jni2hook cannot do that for
 * the caller. */
jni2hook_status JNI2Hook_Install(jmethodID method, void *native_function, jmethodID *out_original);

/* Inserts a call to native_function at bytecode_offset while leaving the
   method body in place. The offset names an instruction in the original class
   body; installing another call in the same method does not shift it. For an
   instance constructor, an offset before the initializing this()/super() call
   is moved directly after that call.

   native_function has the signature (JNIEnv *, jobject) for an instance
   method and (JNIEnv *, jclass) for a static method, and returns void. */
jni2hook_status JNI2Hook_InstallAt(jmethodID method,
                                  uint32_t bytecode_offset,
                                  void *native_function);

/* Finds the first method whose bytecode contains pattern. Tokens are two hex
   digits separated by whitespace; ? and ?? match any byte. The returned offset
   is relative to the original method bytecode and can be passed directly to
   JNI2Hook_InstallAt. */
jni2hook_status JNI2Hook_FindMethod(const char *pattern,
                                   jmethodID *out_method,
                                   uint32_t *out_bytecode_offset,
                                   jni2hook_search_stats *out_stats);

jni2hook_status JNI2Hook_FindMethodInClass(jclass target,
                                          const char *pattern,
                                          jmethodID *out_method,
                                          uint32_t *out_bytecode_offset);

/* Resolves a field through a bytecode signature: the pattern picks the method,
   instruction_offset steps to a field access inside it, and the constant pool
   index of that instruction names the field. Lets an obfuscated field be found
   by where it is used rather than by its name. */
jni2hook_status JNI2Hook_FindFieldInClass(jclass target,
                                          const char *pattern,
                                          uint32_t instruction_offset,
                                          jfieldID *out_field,
                                          int *out_is_static);

/* Reads method names and descriptors in their original class-file order. The
   returned layout owns all memory and must be released with the matching free
   function. This parser does not require JNI2Hook_Init or a running JVM. */
jni2hook_status JNI2Hook_ReadMethodLayout(const unsigned char *class_bytes,
                                          size_t class_size,
                                          jni2hook_method_layout *out_layout);

void JNI2Hook_FreeMethodLayout(jni2hook_method_layout *layout);

/* Puts the body back and drops any copies or inserted calls for method. Other
   hooks on the same class stay. */
jni2hook_status JNI2Hook_Uninstall(jmethodID method);

int  JNI2Hook_IsInstalled(jmethodID method);

/* Removes every hook and releases the JVMTI environment. */
void JNI2Hook_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
