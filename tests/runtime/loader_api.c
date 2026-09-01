/* The loader and remapper API against a live VM.
 *
 * The interesting case is the last one: RemapPlugin was compiled against
 * RemapApi and RemapValue, which are not on the test's class path, so the only
 * way it can define, link and run is if every reference to them was rewritten
 * to the obfuscated ra and rv first. */

#include "jni2hook/jni2hook.h"

#include <stdio.h>
#include <stdlib.h>

static int failures = 0;

static void check(const char *what, int condition)
{
    printf("  %-58s %s\n", what, condition ? "ok" : "FAIL");
    if (!condition)
        failures++;
}

static jni2hook_status ensure_init(void)
{
    static int done = 0;
    if (done)
        return JNI2HOOK_OK;

    const jni2hook_status status = JNI2Hook_InitFromRunningVm();
    if (status == JNI2HOOK_OK)
        done = 1;

    return status;
}

JNIEXPORT jboolean JNICALL Java_LoaderApiTest_findLoadedClass(JNIEnv *env, jclass unused,
                                                              jstring name)
{
    (void)unused;

    if (ensure_init() != JNI2HOOK_OK)
        return JNI_FALSE;

    const char *text = (*env)->GetStringUTFChars(env, name, NULL);
    if (text == NULL)
        return JNI_FALSE;

    jclass found = NULL;
    const jni2hook_status status = JNI2Hook_FindLoadedClass(text, &found);
    (*env)->ReleaseStringUTFChars(env, name, text);

    if (status != JNI2HOOK_OK)
        return JNI_FALSE;

    (*env)->DeleteLocalRef(env, found);
    return JNI_TRUE;
}

/* A name nothing has loaded has to come back as NOT_FOUND rather than as some
   other failure, otherwise a caller cannot tell "absent" from "broken". */
JNIEXPORT jboolean JNICALL Java_LoaderApiTest_findMissingClass(JNIEnv *env, jclass unused,
                                                               jstring name)
{
    (void)unused;

    if (ensure_init() != JNI2HOOK_OK)
        return JNI_FALSE;

    const char *text = (*env)->GetStringUTFChars(env, name, NULL);
    if (text == NULL)
        return JNI_FALSE;

    jclass found = NULL;
    const jni2hook_status status = JNI2Hook_FindLoadedClass(text, &found);
    (*env)->ReleaseStringUTFChars(env, name, text);

    if (status == JNI2HOOK_OK)
    {
        (*env)->DeleteLocalRef(env, found);
        return JNI_FALSE;
    }

    return status == JNI2HOOK_ERR_NOT_FOUND ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_LoaderApiTest_loaderNameOf(JNIEnv *env, jclass unused, jstring name)
{
    (void)unused;

    if (ensure_init() != JNI2HOOK_OK)
        return NULL;

    const char *text = (*env)->GetStringUTFChars(env, name, NULL);
    if (text == NULL)
        return NULL;

    jclass found = NULL;
    jni2hook_status status = JNI2Hook_FindLoadedClass(text, &found);
    (*env)->ReleaseStringUTFChars(env, name, text);
    if (status != JNI2HOOK_OK)
        return NULL;

    jobject loader = NULL;
    status = JNI2Hook_GetClassLoader(found, &loader);
    (*env)->DeleteLocalRef(env, found);
    if (status != JNI2HOOK_OK)
        return NULL;

    /* NULL is the bootstrap loader, which is an answer, so it gets a name of
       its own rather than being reported as a failure. */
    if (loader == NULL)
        return (*env)->NewStringUTF(env, "<bootstrap>");

    jclass loader_class = (*env)->GetObjectClass(env, loader);
    jmethodID get_name = (*env)->GetMethodID(env, loader_class, "getName", "()Ljava/lang/String;");
    jstring loader_name = get_name == NULL ? NULL : (*env)->CallObjectMethod(env, loader, get_name);

    if ((*env)->ExceptionCheck(env) == JNI_TRUE)
        (*env)->ExceptionClear(env);

    (*env)->DeleteLocalRef(env, loader_class);
    (*env)->DeleteLocalRef(env, loader);
    return loader_name;
}

JNIEXPORT jclass JNICALL Java_LoaderApiTest_defineRemapped(JNIEnv *env, jclass unused, jstring name,
                                                           jbyteArray class_bytes)
{
    (void)unused;

    if (ensure_init() != JNI2HOOK_OK)
        return NULL;

    /* The compile-time names on the left are what RemapPlugin was built
       against; the runtime names on the right are all the VM has. */
    const jni2hook_class_mapping classes[] = {
        {"RemapApi", "ra"},
        {"RemapValue", "rv"},
    };

    const jni2hook_method_mapping methods[] = {
        {"RemapApi", "greet", "(LRemapValue;)Ljava/lang/String;", NULL, "b", NULL},
        {"RemapValue", "text", "()Ljava/lang/String;", NULL, "d", NULL},
    };

    jclass base = NULL;
    check("FindLoadedClass(ra)", JNI2Hook_FindLoadedClass("ra", &base) == JNI2HOOK_OK);
    if (base == NULL)
        return NULL;

    jobject loader = NULL;
    check("GetClassLoader(ra)", JNI2Hook_GetClassLoader(base, &loader) == JNI2HOOK_OK);
    (*env)->DeleteLocalRef(env, base);

    const jsize size = (*env)->GetArrayLength(env, class_bytes);
    jbyte *input = (*env)->GetByteArrayElements(env, class_bytes, NULL);
    if (input == NULL)
        return NULL;

    unsigned char *remapped = NULL;
    size_t remapped_size = 0;
    const jni2hook_status status =
        JNI2Hook_RemapClass((const unsigned char *)input, (size_t)size, classes,
                            sizeof(classes) / sizeof(classes[0]), methods,
                            sizeof(methods) / sizeof(methods[0]), NULL, 0, &remapped,
                            &remapped_size);

    (*env)->ReleaseByteArrayElements(env, class_bytes, input, JNI_ABORT);
    check("RemapClass", status == JNI2HOOK_OK);
    if (status != JNI2HOOK_OK)
        return NULL;

    check("remapping changed the class", remapped_size != (size_t)size ||
                                             remapped[0] == 0xCA);

    const char *text = (*env)->GetStringUTFChars(env, name, NULL);
    jclass defined = NULL;
    const jni2hook_status defined_status =
        text == NULL ? JNI2HOOK_ERR_JNI
                     : JNI2Hook_DefineClass(loader, text, remapped, remapped_size, &defined);

    if (text != NULL)
        (*env)->ReleaseStringUTFChars(env, name, text);

    JNI2Hook_FreeClassBytes(remapped);
    if (loader != NULL)
        (*env)->DeleteLocalRef(env, loader);

    check("DefineClass from memory", defined_status == JNI2HOOK_OK);

    /* Defining the same name twice in one loader is a LinkageError, which the
       API has to report rather than swallow. */
    if (defined_status == JNI2HOOK_OK)
    {
        jclass again = NULL;
        const jni2hook_status duplicate =
            JNI2Hook_DefineClass(loader, "RemapPlugin", remapped, remapped_size, &again);
        check("a second define of the same name is refused", duplicate != JNI2HOOK_OK);
        if (again != NULL)
            (*env)->DeleteLocalRef(env, again);
    }

    return defined;
}

JNIEXPORT void JNICALL Java_LoaderApiTest_shutdown(JNIEnv *env, jclass unused)
{
    (void)env;
    (void)unused;

    JNI2Hook_Shutdown();
    if (failures != 0)
        printf("  %d native failures\n", failures);
}
