/* Two questions, both about getting rid of AllowRedefinitionToAddDeleteMethods.
 *
 * Every hook jni2hook installs today puts a new method on the target class --
 * the copy holding the original body, or the inserted native callback. That is
 * exactly what RedefineClasses refuses unless the flag is on, and the flag is a
 * deprecated product flag that nothing supported can change in a running VM, so
 * hook/vm_structs.c writes it straight into HotSpot's own flag table. CLAUDE.md
 * is blunt about what that means: if HotSpot ever drops the flag, the copy
 * design dies with it.
 *
 *   1. Can a hook reach native code without adding a method to the class it
 *      hooks, and therefore without the flag? The entry point goes on a class
 *      jni2hook defines itself, so the target only ever gets its constant pool
 *      grown and one Code attribute rewritten, which needs no flag at all.
 *
 *   2. Can the hooked method's own arguments be forwarded, or is the hook stuck
 *      with a bare identifier? Both a receiver and a mix of primitives and
 *      references are pushed here and read back on the native side.
 *
 * The helper class is generated rather than compiled: jni2hook already has a
 * class file writer, so no JDK is needed at build time. It is emitted at major
 * version 52, which every JVM from 8 onwards loads. */

#include "jni2hook/jni2hook.h"

#include "hook/class_cache.h"
#include "hook/jvm.h"
#include "hook/vm_structs.h"
#include "jni2hook/utils/class_file_parser.h"
#include "jni2hook/utils/class_transform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HELPER_NAME "j2h/GeneratedHooks"

typedef struct
{
    const char *name;
    const char *descriptor;
    void *function;
} helper_method;

static int g_plain_calls = 0;
static int g_plain_id = 0;

static int g_mixed_calls = 0;
static int g_mixed_id = 0;
static jint g_mixed_int = 0;
static jlong g_mixed_long = 0;
static jdouble g_mixed_double = 0;
static int g_mixed_receiver_ok = 0;
static int g_mixed_text_ok = 0;

/* Without argument forwarding the callee takes only the identifier, so this is
   the descriptor the transform generates: (I)V. */
static void JNICALL on_plain(JNIEnv *env, jclass owner, jint id)
{
    (void)env;
    (void)owner;
    g_plain_calls++;
    g_plain_id = id;
}

static void JNICALL on_mixed(JNIEnv *env, jclass owner, jint id, jobject receiver, jint number,
                             jobject text, jlong big, jdouble fraction)
{
    (void)owner;
    g_mixed_calls++;
    g_mixed_id = id;
    g_mixed_int = number;
    g_mixed_long = big;
    g_mixed_double = fraction;

    /* The receiver arrives as a plain jobject, so the whole JNI object surface
       is available: this is what marshalling through FFM could not give. */
    g_mixed_receiver_ok = receiver != NULL;

    if (text != NULL)
    {
        const char *chars = (*env)->GetStringUTFChars(env, (jstring)text, NULL);
        if (chars != NULL)
        {
            g_mixed_text_ok = strcmp(chars, "hello") == 0;
            (*env)->ReleaseStringUTFChars(env, (jstring)text, chars);
        }
    }
}

/* A class with nothing but native methods: no Code attribute is needed, and
   without a constructor there is nothing else to emit. */
static u1 *build_helper_class(const helper_method *methods, size_t count, size_t *out_size)
{
    ClassFile *cf = classFile_create();
    if (cf == NULL)
        return NULL;

    cf->minor_version = 0;
    cf->major_version = JAVA_8_VERSION;
    cf->access_flags = (u2)(JVM_ACC_PUBLIC | JVM_ACC_SUPER | JVM_ACC_FINAL);

    if (classFile_intern_class(cf, HELPER_NAME, &cf->this_class) != CLASSFILE_OK ||
        classFile_intern_class(cf, "java/lang/Object", &cf->super_class) != CLASSFILE_OK)
    {
        classFile_destroy(cf);
        return NULL;
    }

    for (size_t i = 0; i < count; i++)
    {
        u2 name_index = 0, descriptor_index = 0;
        if (classFile_intern_utf8(cf, methods[i].name, &name_index) != CLASSFILE_OK ||
            classFile_intern_utf8(cf, methods[i].descriptor, &descriptor_index) != CLASSFILE_OK)
        {
            classFile_destroy(cf);
            return NULL;
        }

        member_info *method = NULL;
        if (member_list_append(&cf->methods, &method) != CLASSFILE_OK)
        {
            classFile_destroy(cf);
            return NULL;
        }

        method->access_flags = (u2)(JVM_ACC_PUBLIC | JVM_ACC_STATIC | JVM_ACC_NATIVE);
        method->name_index = name_index;
        method->descriptor_index = descriptor_index;
    }

    u1 *bytes = NULL;
    const classfile_status status = classfile_serialize(cf, &bytes, out_size);
    classFile_destroy(cf);
    return status == CLASSFILE_OK ? bytes : NULL;
}

static int failures = 0;

static void check(const char *what, int condition)
{
    printf("  %-60s %s\n", what, condition ? "ok" : "FAIL");
    if (!condition)
        failures++;
}

typedef struct
{
    const char *method;
    const char *descriptor;
    const char *callee;
    int id;
    bool forward;
} rewrite_request;

/* Every rewrite starts from the bytes the class had before jni2hook touched it
   and applies all of them, which is what reapply does too: applying one at a
   time would mean each redefinition discarded the one before it. */
static bool rewrite_all(JNIEnv *env, jclass target, const rewrite_request *requests, size_t count)
{
    jint original_size = 0;
    const unsigned char *original = class_cache_get(env, target, &original_size);
    if (original == NULL)
        return false;

    ClassFile *cf = NULL;
    if (classfile_parse(original, (size_t)original_size, &cf) != CLASSFILE_OK)
        return false;

    const u2 before = cf->methods.count;

    for (size_t i = 0; i < count; i++)
    {
        classfile_status cause = CLASSFILE_OK;
        const transform_status transformed = class_transform_insert_static_call(
            cf, requests[i].method, requests[i].descriptor, 0, HELPER_NAME, requests[i].callee,
            requests[i].id, requests[i].forward, &cause);
        if (transformed != TRANSFORM_OK)
        {
            printf("      %s%s: %s (%s)\n", requests[i].method, requests[i].descriptor,
                   transform_status_message(transformed), classfile_status_message(cause));
            classFile_destroy(cf);
            return false;
        }
    }

    if (cf->methods.count != before)
    {
        printf("      the target gained %d methods\n", (int)(cf->methods.count - before));
        classFile_destroy(cf);
        return false;
    }

    u1 *rewritten = NULL;
    size_t rewritten_size = 0;
    const classfile_status serialized = classfile_serialize(cf, &rewritten, &rewritten_size);
    classFile_destroy(cf);
    if (serialized != CLASSFILE_OK)
        return false;

    jvmtiEnv *jvmti = jvm_jvmti();
    jvmtiClassDefinition definition;
    definition.klass = target;
    definition.class_byte_count = (jint)rewritten_size;
    definition.class_bytes = rewritten;

    const jvmtiError error = (*jvmti)->RedefineClasses(jvmti, 1, &definition);
    free(rewritten);

    if (error != JVMTI_ERROR_NONE)
        printf("      RedefineClasses returned %d%s\n", (int)error,
               error == JVMTI_ERROR_UNSUPPORTED_REDEFINITION_METHOD_ADDED
                   ? " (METHOD_ADDED -- a method was added after all)"
                   : "");
    return error == JVMTI_ERROR_NONE;
}

JNIEXPORT jint JNICALL Java_NoFlagHookTest_run(JNIEnv *env, jclass unused, jclass target)
{
    (void)unused;

    const jni2hook_status init = JNI2Hook_InitFromRunningVm();
    check("JNI2Hook_Init", init == JNI2HOOK_OK);
    if (init != JNI2HOOK_OK)
        return 1;

    union
    {
        void *pointer;
        void (JNICALL *plain)(JNIEnv *, jclass, jint);
    } plain_entry = {NULL};
    plain_entry.plain = on_plain;

    union
    {
        void *pointer;
        void (JNICALL *mixed)(JNIEnv *, jclass, jint, jobject, jint, jobject, jlong, jdouble);
    } mixed_entry = {NULL};
    mixed_entry.mixed = on_mixed;

    const helper_method methods[] = {
        {"onPlain", "(I)V", plain_entry.pointer},
        {"onMixed", "(ILjava/lang/Object;ILjava/lang/Object;JD)V", mixed_entry.pointer},
    };

    size_t helper_size = 0;
    u1 *helper = build_helper_class(methods, sizeof(methods) / sizeof(methods[0]), &helper_size);
    check("generated the helper class file", helper != NULL);
    if (helper == NULL)
        return 1;
    printf("      %zu bytes, class file major %d, %zu native methods\n", helper_size,
           JAVA_8_VERSION, sizeof(methods) / sizeof(methods[0]));

    jclass helper_class = (*env)->DefineClass(env, HELPER_NAME, NULL, (const jbyte *)helper,
                                              (jsize)helper_size);
    if ((*env)->ExceptionCheck(env))
    {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
    free(helper);
    check("DefineClass accepted it", helper_class != NULL);
    if (helper_class == NULL)
        return 1;

    JNINativeMethod bindings[2];
    for (size_t i = 0; i < 2; i++)
    {
        bindings[i].name = (char *)methods[i].name;
        bindings[i].signature = (char *)methods[i].descriptor;
        bindings[i].fnPtr = methods[i].function;
    }
    check("RegisterNatives on our own class",
          (*env)->RegisterNatives(env, helper_class, bindings, 2) == 0);
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);

    char *class_name = jvm_class_name_of(target);
    jvmtiError error = JVMTI_ERROR_NONE;
    check("captured the original bytes of the target",
          class_name != NULL && class_cache_ensure(target, class_name, &error));
    free(class_name);

    /* The flag Init forced on goes back off, which puts the VM into the state
       every unpatched JDK is in. Everything after this has to work without it. */
    bool previous = false;
    check("cleared AllowRedefinitionToAddDeleteMethods",
          vm_structs_set_bool_flag("AllowRedefinitionToAddDeleteMethods", false, &previous));
    printf("      the flag was %s, now off\n", previous ? "on" : "off");

    const rewrite_request requests[] = {
        {"compute", "(I)I", "onPlain", 4242, false},
        {"mixed", "(ILjava/lang/String;JD)I", "onMixed", 77, true},
    };

    check("rewrote both methods without adding one, flag off",
          rewrite_all(env, target, requests, sizeof(requests) / sizeof(requests[0])));

    return failures;
}

JNIEXPORT jint JNICALL Java_NoFlagHookTest_plainCalls(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;
    return g_plain_calls;
}

JNIEXPORT jint JNICALL Java_NoFlagHookTest_plainId(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;
    return g_plain_id;
}

JNIEXPORT jint JNICALL Java_NoFlagHookTest_mixedCalls(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;
    return g_mixed_calls;
}

JNIEXPORT jint JNICALL Java_NoFlagHookTest_mixedId(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;
    return g_mixed_id;
}

JNIEXPORT jint JNICALL Java_NoFlagHookTest_mixedInt(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;
    return g_mixed_int;
}

JNIEXPORT jlong JNICALL Java_NoFlagHookTest_mixedLong(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;
    return g_mixed_long;
}

JNIEXPORT jdouble JNICALL Java_NoFlagHookTest_mixedDouble(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;
    return g_mixed_double;
}

JNIEXPORT jboolean JNICALL Java_NoFlagHookTest_mixedReceiverArrived(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;
    return g_mixed_receiver_ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_NoFlagHookTest_mixedTextArrived(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;
    return g_mixed_text_ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_NoFlagHookTest_shutdown(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;
    JNI2Hook_Shutdown();
}
