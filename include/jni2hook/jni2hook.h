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
 * A stale JIT/MethodHandle call site may enter the detour after uninstall. The
 * VM is therefore bound to a resident trampoline rather than to native_function
 * itself: its page is never unmapped, and uninstall disarms it so a late call
 * returns a zero of the method's return type instead of reaching a function
 * that may be gone. A caller may unload itself once uninstall reports OK.
 *
 * The emitter exists for x86-64 only. Elsewhere native_function is bound
 * directly, and a caller that unloads itself carries the risk described above. */
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

/* Same, for a caller that already knows the names — from a mapping, say — and
   has no reason to scan bytecode. Only ClassPrepare can resolve this: the class
   name is compared there and the member looked up by name and descriptor.
 *
 * A class that is already loaded is not found by this; use
 * JNI2Hook_FindLoadedClass for that case and watch only what is still missing. */
jni2hook_status JNI2Hook_WatchMethodByName(const char *internal_class_name,
                                           const char *method_name,
                                           const char *method_signature, int method_static,
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

/* Finds a class the VM has already loaded, by JVM internal name such as
   "net/minecraft/client/Minecraft". Walks GetLoadedClasses and compares the
   real class signature, so a name that only a subset of loaders can see is
   still found.

   Class identity includes the defining loader: when several loaders have
   loaded the same name, the first match wins and JNI2Hook_GetClassLoader on
   the result tells which one it came from. out_class receives a local
   reference the caller owns. */
jni2hook_status JNI2Hook_FindLoadedClass(const char *internal_name, jclass *out_class);

/* Reports the defining loader of klass. The bootstrap loader is NULL, which
   is a success, not a failure. out_loader receives a local reference. */
jni2hook_status JNI2Hook_GetClassLoader(jclass klass, jobject *out_loader);

/* Defines a class from raw .class bytes held in memory: no temporary jar and
   nothing written to disk. internal_name uses '/' separators. Passing the
   loader of an already loaded target class is what lets the new class name
   that target's types.

   The JVM rejects a second definition of the same name in one loader, so this
   is a define, not an upsert. out_class is only written on success. */
jni2hook_status JNI2Hook_DefineClass(jobject loader, const char *internal_name,
                                     const unsigned char *bytes, size_t size,
                                     jclass *out_class);

/* Replaces the bytecode of a class the VM has already loaded. The new
   definition has to declare the same fields and methods; only their bodies
   may differ.

   This is the answer to a class that cannot be unloaded. The JVM has no way
   to remove a named class from a live loader, so a caller that wants its code
   gone from the process redefines it with a version whose methods do nothing.
   The name stays, the behaviour does not. */
jni2hook_status JNI2Hook_RedefineClass(jclass klass, const unsigned char *bytes, size_t size);

/* One compile-time class name and the name it carries at runtime, both JVM
   internal names. */
typedef struct
{
    const char *from;
    const char *to;
} jni2hook_class_mapping;

/* One member, named as it was compiled and as it exists at runtime.

   owner, name and descriptor identify the member in the input class file;
   a NULL descriptor matches any descriptor. The runtime_ fields are each
   optional: a NULL one leaves that part to the class mapping, so renaming
   only a method needs nothing but runtime_name. runtime_owner moves the
   reference to a different class, which the class mapping alone cannot do. */
typedef struct
{
    const char *owner;
    const char *name;
    const char *descriptor;

    const char *runtime_owner;
    const char *runtime_name;
    const char *runtime_descriptor;
} jni2hook_member_mapping;

typedef jni2hook_member_mapping jni2hook_method_mapping;
typedef jni2hook_member_mapping jni2hook_field_mapping;

/* Rewrites the symbolic references of a class file so that code compiled
   against readable names links against the obfuscated names a running VM
   actually has. This edits constant pool entries rather than replacing text,
   so a name is only touched where the format says a name belongs.

   Covered: CONSTANT_Class entries including array forms, this_class,
   super_class, interfaces, the owner, name and descriptor of every Field-,
   Method- and InterfaceMethodref, NameAndType entries reached through those
   and through Dynamic and InvokeDynamic, and the descriptors of the class's
   own fields and methods.

   The class's own members are matched against mappings whose owner is this
   class, its super class or one of its interfaces, so an override is renamed
   along with the method it overrides.

   A renamed reference is repointed at a newly interned entry rather than
   having its text edited, because one Utf8 can be shared by references that
   are not all being renamed. The old entry stays in the pool, unreferenced, so
   the compile-time text may still be visible in the bytes even though nothing
   links against it any more.

   Debug and reflection attributes are left alone: Signature,
   LocalVariableTable and LocalVariableTypeTable keep their compile-time
   names. The verifier does not read them, so linking is unaffected.

   out_bytes receives a buffer the caller owns and releases with
   JNI2Hook_FreeClassBytes. The input is not retained. */
jni2hook_status JNI2Hook_RemapClass(const unsigned char *input, size_t input_size,

                                    const jni2hook_class_mapping *class_mappings,
                                    size_t class_mapping_count,

                                    const jni2hook_method_mapping *method_mappings,
                                    size_t method_mapping_count,

                                    const jni2hook_field_mapping *field_mappings,
                                    size_t field_mapping_count,

                                    unsigned char **out_bytes, size_t *out_size);

void JNI2Hook_FreeClassBytes(unsigned char *bytes);

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
