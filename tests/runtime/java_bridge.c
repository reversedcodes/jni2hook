/* Whether a hook can stop doing its object work through JNI.
 *
 * no_flag_hook.c showed that a hooked method can call into a class jni2hook
 * supplies, carrying its receiver and arguments, without anything being added
 * to the hooked class. That callee was generated from C and could only be a
 * native stub: a class defined into the bootstrap loader cannot name the types
 * of the class it is called from, so there was nothing it could do with the
 * receiver except hand it straight to native.
 *
 * Two changes here:
 *
 *   The callee is written in Java and compiled by javac, then embedded as a
 *   byte buffer and handed to DefineClass. Here it is read from a file the test
 *   is pointed at, which is the same thing a step earlier -- turning a class
 *   file into a C array is a build step, not a mechanism.
 *
 *   It is defined into the *target's own class loader* rather than the
 *   bootstrap one. That is what lets it name the target's types, so the work it
 *   does on the receiver is ordinary Java: target.scale(n), target.label(). The
 *   JNI equivalent is GetMethodID plus CallIntMethod plus an exception check
 *   per step, with the signature written out as a string.
 *
 * Native is still reachable, and still gets called -- but only with what it
 * asked for, and only where being native is the point. */

#include "jni2hook/jni2hook.h"

#include "hook/class_cache.h"
#include "hook/jvm.h"
#include "hook/vm_structs.h"
#include "jni2hook/utils/class_file_parser.h"
#include "jni2hook/utils/class_transform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BRIDGE_NAME "HookBridge"
#define HANDLE_BRIDGE_NAME "HandleBridge"

/* Which method of the target the handle bridge should be given, by class file
   slot rather than by name. The slot is what survives obfuscation; the name is
   read out of the class file at run time, so it is never written down here.
   Note that javac does not emit methods in source order -- this one is second,
   not third -- which is exactly why the layout is read rather than assumed. */
/* Which methods of the target the bridge is given, named by descriptor and
   found in the class file at run time. No method name is written down on either
   side of the bridge, which is the point: the Minecraft equivalents are renamed
   every version.
 *
 * A slot index is what a generator would emit, and Omega does exactly that from
 * the jar it targets. It cannot be written down here, because the slot is a
 * property of one compiled artifact and not of the source: javac 17 puts stat
 * before work in this class and javac 25 puts work before stat. Deriving it is
 * what a generator does too, only offline. */
static const struct
{
    const char *descriptor;
} kBound[] = {
    {"(I)I"},                       /* scale, for the observing hook */
    {"(ILjava/lang/String;)I"},     /* work, for calling the original */
};

static int g_reports = 0;
static jint g_id = 0;
static jint g_value = 0;
static char g_label[64] = {0};

/* The name-free bridge reports without a label, so it needs its own binding. */
static void JNICALL on_report_plain(JNIEnv *env, jclass owner, jint id, jint value)
{
    (void)env;
    (void)owner;
    g_reports++;
    g_id = id;
    g_value = value;
}

static void JNICALL on_report(JNIEnv *env, jclass owner, jint id, jint value, jstring label)
{
    (void)owner;
    g_reports++;
    g_id = id;
    g_value = value;

    g_label[0] = 0;
    if (label != NULL)
    {
        const char *chars = (*env)->GetStringUTFChars(env, label, NULL);
        if (chars != NULL)
        {
            snprintf(g_label, sizeof(g_label), "%s", chars);
            (*env)->ReleaseStringUTFChars(env, label, chars);
        }
    }
}

static int failures = 0;

static void check(const char *what, int condition)
{
    printf("  %-60s %s\n", what, condition ? "ok" : "FAIL");
    if (!condition)
        failures++;
}

static unsigned char *read_file(const char *path, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return NULL;

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return NULL;
    }
    const long length = ftell(file);
    rewind(file);
    if (length <= 0)
    {
        fclose(file);
        return NULL;
    }

    unsigned char *bytes = malloc((size_t)length);
    if (bytes != NULL && fread(bytes, 1, (size_t)length, file) != (size_t)length)
    {
        free(bytes);
        bytes = NULL;
    }
    fclose(file);

    if (bytes != NULL)
        *out_size = (size_t)length;
    return bytes;
}

JNIEXPORT jint JNICALL Java_BridgeTest_run(JNIEnv *env, jclass unused, jclass target,
                                           jstring bridge_path)
{
    (void)unused;

    const jni2hook_status init = JNI2Hook_InitFromRunningVm();
    check("JNI2Hook_Init", init == JNI2HOOK_OK);
    if (init != JNI2HOOK_OK)
        return 1;

    /* 1. The compiled bridge, as bytes. */
    const char *path = (*env)->GetStringUTFChars(env, bridge_path, NULL);
    size_t bridge_size = 0;
    unsigned char *bridge = path != NULL ? read_file(path, &bridge_size) : NULL;
    if (path != NULL)
        (*env)->ReleaseStringUTFChars(env, bridge_path, path);

    check("read the compiled bridge class", bridge != NULL);
    if (bridge == NULL)
        return 1;
    printf("      %zu bytes of javac output, handed over as a buffer\n", bridge_size);

    /* 2. Into the target's own loader, which is what lets it name the target. */
    jvmtiEnv *jvmti = jvm_jvmti();
    jobject loader = NULL;
    check("GetClassLoader of the target",
          (*jvmti)->GetClassLoader(jvmti, target, &loader) == JVMTI_ERROR_NONE);
    printf("      target loader: %s\n", loader == NULL ? "bootstrap" : "application");

    jclass bridge_class = (*env)->DefineClass(env, BRIDGE_NAME, loader, (const jbyte *)bridge,
                                              (jsize)bridge_size);
    if ((*env)->ExceptionCheck(env))
    {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
    free(bridge);
    check("DefineClass into that loader", bridge_class != NULL);
    if (bridge_class == NULL)
        return 1;

    union
    {
        void *pointer;
        void (JNICALL *report)(JNIEnv *, jclass, jint, jint, jstring);
    } entry = {NULL};
    entry.report = on_report;

    JNINativeMethod binding;
    binding.name = (char *)"report";
    binding.signature = (char *)"(IILjava/lang/String;)V";
    binding.fnPtr = entry.pointer;
    check("RegisterNatives on the bridge",
          (*env)->RegisterNatives(env, bridge_class, &binding, 1) == 0);
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);

    /* 3. Same rules as before: nothing is added to the target, flag stays off. */
    char *class_name = jvm_class_name_of(target);
    jvmtiError error = JVMTI_ERROR_NONE;
    check("captured the original bytes",
          class_name != NULL && class_cache_ensure(target, class_name, &error));
    free(class_name);

    bool previous = false;
    check("cleared AllowRedefinitionToAddDeleteMethods",
          vm_structs_set_bool_flag("AllowRedefinitionToAddDeleteMethods", false, &previous));

    jint original_size = 0;
    const unsigned char *original = class_cache_get(env, target, &original_size);
    ClassFile *cf = NULL;
    check("parsed the original",
          original != NULL &&
              classfile_parse(original, (size_t)original_size, &cf) == CLASSFILE_OK);
    if (cf == NULL)
        return 1;

    const u2 before = cf->methods.count;

    classfile_status cause = CLASSFILE_OK;
    const transform_status transformed = class_transform_insert_static_call(
        cf, "work", "(ILjava/lang/String;)I", 0, BRIDGE_NAME, "enter", 7, true, &cause);
    if (transformed != TRANSFORM_OK)
        printf("      transform: %s (%s)\n", transform_status_message(transformed),
               classfile_status_message(cause));
    check("inserted the call to the Java bridge", transformed == TRANSFORM_OK);
    check("the target gained no method", cf->methods.count == before);

    u1 *rewritten = NULL;
    size_t rewritten_size = 0;
    const classfile_status serialized = classfile_serialize(cf, &rewritten, &rewritten_size);
    classFile_destroy(cf);
    check("serialized", serialized == CLASSFILE_OK);
    if (serialized != CLASSFILE_OK)
        return 1;

    jvmtiClassDefinition definition;
    definition.klass = target;
    definition.class_byte_count = (jint)rewritten_size;
    definition.class_bytes = rewritten;

    const jvmtiError redefined = (*jvmti)->RedefineClasses(jvmti, 1, &definition);
    free(rewritten);
    if (redefined != JVMTI_ERROR_NONE)
        printf("      RedefineClasses returned %d\n", (int)redefined);
    check("RedefineClasses with the flag off", redefined == JVMTI_ERROR_NONE);

    return failures;
}

/* Resolves the target's method at BOUND_SLOT without anyone naming it, turns it
   into a java.lang.reflect.Method and lets the bridge make a MethodHandle of
   it. That is the whole answer to "why not just void*": the receiver can stay
   an Object, as long as the things to call on it were bound beforehand. */
static bool bind_by_slot(JNIEnv *env, jclass target, jclass bridge)
{
    jint original_size = 0;
    const unsigned char *original = class_cache_get(env, target, &original_size);
    if (original == NULL)
        return false;

    jni2hook_method_layout layout;
    memset(&layout, 0, sizeof(layout));
    if (JNI2Hook_ReadMethodLayout(original, (size_t)original_size, &layout) != JNI2HOOK_OK)
        return false;

    for (size_t i = 0; i < layout.count; i++)
        printf("      slot %zu: %s%s\n", i, layout.methods[i].name,
               layout.methods[i].descriptor);

    jmethodID bind = (*env)->GetStaticMethodID(env, bridge, "bind", "(ILjava/lang/Object;)V");
    if (bind == NULL)
    {
        (*env)->ExceptionClear(env);
        JNI2Hook_FreeMethodLayout(&layout);
        return false;
    }

    bool ok = true;
    for (size_t i = 0; i < sizeof(kBound) / sizeof(kBound[0]) && ok; i++)
    {
        jobject reflected = NULL;
        size_t found = layout.count;

        for (size_t slot = 0; slot < layout.count && reflected == NULL; slot++)
        {
            if (strcmp(layout.methods[slot].descriptor, kBound[i].descriptor) != 0)
                continue;

            /* More than one method can share a descriptor -- scale and stat
               both read (I)I -- so the one that binds as an instance method is
               the one meant. */
            jmethodID method = (*env)->GetMethodID(env, target, layout.methods[slot].name,
                                                   layout.methods[slot].descriptor);
            if (method == NULL)
            {
                (*env)->ExceptionClear(env);
                continue;
            }

            reflected = (*env)->ToReflectedMethod(env, target, method, JNI_FALSE);
            if (reflected == NULL)
                (*env)->ExceptionClear(env);
            else
                found = slot;
        }

        if (reflected == NULL)
        {
            printf("      no instance method with descriptor %s\n", kBound[i].descriptor);
            ok = false;
            break;
        }

        printf("      %s is at slot %zu\n", kBound[i].descriptor, found);

        (*env)->CallStaticVoidMethod(env, bridge, bind, (jint)i, reflected);
        if ((*env)->ExceptionCheck(env))
        {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
            ok = false;
        }
    }

    JNI2Hook_FreeMethodLayout(&layout);
    return ok;
}

/* Defines the name-free bridge into the target's own loader, binds its native
   report, captures the target's original bytes, clears the VM flag so nothing
   after this may rely on it, hands the bridge its method handles, and inserts
   the forwarding call. */
static jint install_handle_bridge(JNIEnv *env, jclass target, jstring bridge_path)
{
    failures = 0;

    const char *path = (*env)->GetStringUTFChars(env, bridge_path, NULL);
    size_t bridge_size = 0;
    unsigned char *bridge = path != NULL ? read_file(path, &bridge_size) : NULL;
    if (path != NULL)
        (*env)->ReleaseStringUTFChars(env, bridge_path, path);
    check("read the name-free bridge", bridge != NULL);
    if (bridge == NULL)
        return 1;

    jvmtiEnv *jvmti = jvm_jvmti();
    jobject loader = NULL;
    (*jvmti)->GetClassLoader(jvmti, target, &loader);

    jclass bridge_class = (*env)->DefineClass(env, HANDLE_BRIDGE_NAME, loader,
                                              (const jbyte *)bridge, (jsize)bridge_size);
    if ((*env)->ExceptionCheck(env))
    {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
    free(bridge);
    check("DefineClass", bridge_class != NULL);
    if (bridge_class == NULL)
        return 1;

    union
    {
        void *pointer;
        void (JNICALL *report)(JNIEnv *, jclass, jint, jint);
    } entry = {NULL};
    entry.report = on_report_plain;

    JNINativeMethod binding;
    binding.name = (char *)"report";
    binding.signature = (char *)"(II)V";
    binding.fnPtr = entry.pointer;
    check("RegisterNatives", (*env)->RegisterNatives(env, bridge_class, &binding, 1) == 0);
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);

    char *class_name = jvm_class_name_of(target);
    jvmtiError capture = JVMTI_ERROR_NONE;
    const bool got = class_name != NULL && class_cache_ensure(target, class_name, &capture);
    check("captured the original bytes", got);
    printf("      target is %s, capture said jvmti %d\n",
           class_name != NULL ? class_name : "<null>", (int)capture);
    free(class_name);

    bool previous = false;
    vm_structs_set_bool_flag("AllowRedefinitionToAddDeleteMethods", false, &previous);

    check("bound the target's methods by slot, never by name",
          bind_by_slot(env, target, bridge_class));

    jint original_size = 0;
    const unsigned char *original = class_cache_get(env, target, &original_size);
    ClassFile *cf = NULL;
    check("parsed the original",
          original != NULL &&
              classfile_parse(original, (size_t)original_size, &cf) == CLASSFILE_OK);
    if (cf == NULL)
        return 1;

    const u2 before = cf->methods.count;
    classfile_status cause = CLASSFILE_OK;
    const transform_status transformed = class_transform_insert_static_call(
        cf, "work", "(ILjava/lang/String;)I", 0, HANDLE_BRIDGE_NAME, "enter", 9, true, &cause);
    if (transformed != TRANSFORM_OK)
        printf("      transform: %s (%s)\n", transform_status_message(transformed),
               classfile_status_message(cause));
    check("inserted the call", transformed == TRANSFORM_OK);
    check("the target gained no method", cf->methods.count == before);

    u1 *rewritten = NULL;
    size_t rewritten_size = 0;
    const classfile_status serialized = classfile_serialize(cf, &rewritten, &rewritten_size);
    classFile_destroy(cf);
    if (serialized != CLASSFILE_OK)
        return 1;

    jvmtiClassDefinition definition;
    definition.klass = target;
    definition.class_byte_count = (jint)rewritten_size;
    definition.class_bytes = rewritten;
    const jvmtiError redefined = (*jvmti)->RedefineClasses(jvmti, 1, &definition);
    free(rewritten);
    check("RedefineClasses with the flag off", redefined == JVMTI_ERROR_NONE);

    return failures;
}

JNIEXPORT jint JNICALL Java_BridgeTest_runHandles(JNIEnv *env, jclass unused, jclass target,
                                                  jstring bridge_path)
{
    (void)unused;
    return install_handle_bridge(env, target, bridge_path);
}

/* The same installation against a target that is not where the easy test put
   it: behind a class loader of its own, or inside a named module. */
JNIEXPORT jint JNICALL Java_LoaderCasesTest_install(JNIEnv *env, jclass unused, jclass target,
                                                    jstring bridge_path)
{
    (void)unused;
    const jni2hook_status init = JNI2Hook_InitFromRunningVm();
    if (init != JNI2HOOK_OK)
    {
        printf("  JNI2Hook_Init failed: %s\n", JNI2Hook_StatusMessage(init));
        return 1;
    }
    return install_handle_bridge(env, target, bridge_path);
}

/* The guarded form: the same bridge, but it decides whether the body runs. */
JNIEXPORT jint JNICALL Java_GuardTest_installGuard(JNIEnv *env, jclass unused,
                                                         jclass target, jstring bridge_path)
{
    (void)unused;
    const jni2hook_status init = JNI2Hook_InitFromRunningVm();
    if (init != JNI2HOOK_OK)
        return 1;

    const jint installed = install_handle_bridge(env, target, bridge_path);
    if (installed != 0)
        return installed;

    jint original_size = 0;
    const unsigned char *original = class_cache_get(env, target, &original_size);
    ClassFile *cf = NULL;
    if (original == NULL ||
        classfile_parse(original, (size_t)original_size, &cf) != CLASSFILE_OK)
        return 1;

    const u2 before = cf->methods.count;

    /* Every shape the frame builder has to get right, guarded in one pass. */
    static const struct
    {
        const char *method;
        const char *descriptor;
        const char *callee;
        const char *value_callee;
    } guards[] = {
        {"work", "(ILjava/lang/String;)I", "guard", "guardValue"},
        {"act", "(JD)V", "guardVoid", NULL},
        {"pick", "([Ljava/lang/String;Z)Ljava/lang/String;", "guardRef", NULL},
        {"stat", "(I)I", "guardStatic", NULL},
    };

    transform_status transformed = TRANSFORM_OK;
    for (size_t i = 0; i < sizeof(guards) / sizeof(guards[0]); i++)
    {
        classfile_status cause = CLASSFILE_OK;
        const transform_status one = class_transform_insert_guarded_call(
            cf, guards[i].method, guards[i].descriptor, HANDLE_BRIDGE_NAME, guards[i].callee,
            guards[i].value_callee, 11, &cause);
        if (one != TRANSFORM_OK)
        {
            printf("      %s%s: %s (%s)\n", guards[i].method, guards[i].descriptor,
                   transform_status_message(one), classfile_status_message(cause));
            transformed = one;
        }
    }
    check("inserted a guard into every shape", transformed == TRANSFORM_OK);
    check("the target still gained no method", cf->methods.count == before);

    u1 *rewritten = NULL;
    size_t rewritten_size = 0;
    const classfile_status serialized = classfile_serialize(cf, &rewritten, &rewritten_size);
    classFile_destroy(cf);
    if (serialized != CLASSFILE_OK)
        return 1;

    jvmtiEnv *jvmti = jvm_jvmti();
    jvmtiClassDefinition definition;
    definition.klass = target;
    definition.class_byte_count = (jint)rewritten_size;
    definition.class_bytes = rewritten;
    const jvmtiError redefined = (*jvmti)->RedefineClasses(jvmti, 1, &definition);
    free(rewritten);
    if (redefined != JVMTI_ERROR_NONE)
        printf("      RedefineClasses returned %d\n", (int)redefined);
    check("RedefineClasses accepted the guarded body", redefined == JVMTI_ERROR_NONE);

    return failures;
}

JNIEXPORT jint JNICALL Java_LoaderCasesTest_reports(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;
    return g_reports;
}

JNIEXPORT jint JNICALL Java_GuardTest_reports(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;
    return g_reports;
}

JNIEXPORT jint JNICALL Java_GuardTest_reportedValue(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;
    return g_value;
}

JNIEXPORT void JNICALL Java_GuardTest_shutdown(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;
    JNI2Hook_Shutdown();
}

JNIEXPORT jint JNICALL Java_LoaderCasesTest_reportedValue(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;
    return g_value;
}

JNIEXPORT void JNICALL Java_LoaderCasesTest_shutdown(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;
    JNI2Hook_Shutdown();
}

JNIEXPORT jint JNICALL Java_BridgeTest_reports(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;
    return g_reports;
}

JNIEXPORT jint JNICALL Java_BridgeTest_reportedId(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;
    return g_id;
}

JNIEXPORT jint JNICALL Java_BridgeTest_reportedValue(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;
    return g_value;
}

JNIEXPORT jstring JNICALL Java_BridgeTest_reportedLabel(JNIEnv *env, jclass unused)
{
    (void)unused;
    return (*env)->NewStringUTF(env, g_label);
}

JNIEXPORT void JNICALL Java_BridgeTest_shutdown(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;
    JNI2Hook_Shutdown();
}
