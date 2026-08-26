#include "jni2hook/jni2hook.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static jclass    g_target        = NULL;
static jmethodID g_compute_orig  = NULL;
static jmethodID g_greet_orig    = NULL;
static jmethodID g_locked_orig   = NULL;
static int       g_calls         = 0;

static jint JNICALL detour_compute(JNIEnv *env, jobject self, jint a, jint b)
{
    g_calls++;
    const jint original = (*env)->CallNonvirtualIntMethod(env, self, g_target, g_compute_orig, a, b);
    return original * 10;
}

static jstring JNICALL detour_greet(JNIEnv *env, jclass klass, jstring who)
{
    g_calls++;
    jstring original = (jstring)(*env)->CallStaticObjectMethod(env, klass, g_greet_orig, who);
    if (original == NULL)
        return NULL;

    const char *text = (*env)->GetStringUTFChars(env, original, NULL);
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "hooked:%s", text != NULL ? text : "");
    (*env)->ReleaseStringUTFChars(env, original, text);

    return (*env)->NewStringUTF(env, buffer);
}

static jint JNICALL detour_locked(JNIEnv *env, jobject self, jint x)
{
    g_calls++;
    return (*env)->CallNonvirtualIntMethod(env, self, g_target, g_locked_orig, x) + 1;
}

JNIEXPORT jint JNICALL Java_HookRuntimeTest_setup(JNIEnv *env, jclass unused)
{
    (void)unused;
    JavaVM *vm = NULL;
    if ((*env)->GetJavaVM(env, &vm) != JNI_OK)
        return -1;

    const jni2hook_status status = JNI2Hook_Init(vm);
    if (status != JNI2HOOK_OK)
    {
        fprintf(stderr, "  JNI2Hook_Init: %s (jvmti %d)\n",
                JNI2Hook_StatusMessage(status), (int)JNI2Hook_LastJvmtiError());
        return (jint)status;
    }

    const int flag = JNI2Hook_ForcedRedefinitionFlag();
    fprintf(stderr, "  AllowRedefinitionToAddDeleteMethods: %s\n",
            flag == 1 ? "was off, forced on" : flag == 0 ? "already on" : "UNREACHABLE");
    if (flag < 0)
        return -2;

    return 0;
}

static jint install(JNIEnv *env, jclass target, const char *name, const char *signature,
                    bool is_static, void *detour, jmethodID *out_original)
{
    if (g_target == NULL)
        g_target = (*env)->NewGlobalRef(env, target);

    jmethodID method = is_static ? (*env)->GetStaticMethodID(env, target, name, signature)
                                 : (*env)->GetMethodID(env, target, name, signature);
    if (method == NULL)
    {
        (*env)->ExceptionClear(env);
        return -1;
    }

    const jni2hook_status status = JNI2Hook_Install(method, detour, out_original);
    if (status != JNI2HOOK_OK)
    {
        fprintf(stderr, "  JNI2Hook_Install(%s%s): %s (jvmti %d)\n", name, signature,
                JNI2Hook_StatusMessage(status), (int)JNI2Hook_LastJvmtiError());
        return (jint)status;
    }
    return 0;
}

static jint uninstall(JNIEnv *env, jclass target, const char *name, const char *signature,
                      bool is_static)
{
    jmethodID method = is_static ? (*env)->GetStaticMethodID(env, target, name, signature)
                                 : (*env)->GetMethodID(env, target, name, signature);
    if (method == NULL)
    {
        (*env)->ExceptionClear(env);
        return -1;
    }

    const jni2hook_status status = JNI2Hook_Uninstall(method);
    if (status != JNI2HOOK_OK)
    {
        fprintf(stderr, "  JNI2Hook_Uninstall(%s%s): %s (jvmti %d)\n", name, signature,
                JNI2Hook_StatusMessage(status), (int)JNI2Hook_LastJvmtiError());
        return (jint)status;
    }
    return 0;
}

JNIEXPORT jint JNICALL Java_HookRuntimeTest_hookCompute(JNIEnv *env, jclass unused, jclass target)
{
    (void)unused;
    return install(env, target, "compute", "(II)I", false, (void *)detour_compute, &g_compute_orig);
}

JNIEXPORT jint JNICALL Java_HookRuntimeTest_hookGreet(JNIEnv *env, jclass unused, jclass target)
{
    (void)unused;
    return install(env, target, "greet", "(Ljava/lang/String;)Ljava/lang/String;", true,
                   (void *)detour_greet, &g_greet_orig);
}

JNIEXPORT jint JNICALL Java_HookRuntimeTest_hookLocked(JNIEnv *env, jclass unused, jclass target)
{
    (void)unused;
    return install(env, target, "locked", "(I)I", false, (void *)detour_locked, &g_locked_orig);
}

JNIEXPORT jint JNICALL Java_HookRuntimeTest_unhookCompute(JNIEnv *env, jclass unused, jclass target)
{
    (void)unused;
    return uninstall(env, target, "compute", "(II)I", false);
}

JNIEXPORT jint JNICALL Java_HookRuntimeTest_unhookGreet(JNIEnv *env, jclass unused, jclass target)
{
    (void)unused;
    return uninstall(env, target, "greet", "(Ljava/lang/String;)Ljava/lang/String;", true);
}

JNIEXPORT jint JNICALL Java_HookRuntimeTest_unhookLocked(JNIEnv *env, jclass unused, jclass target)
{
    (void)unused;
    return uninstall(env, target, "locked", "(I)I", false);
}

JNIEXPORT jint JNICALL Java_HookRuntimeTest_calls(JNIEnv *env, jclass unused)
{
    (void)env; (void)unused;
    return g_calls;
}

JNIEXPORT void JNICALL Java_HookRuntimeTest_resetCalls(JNIEnv *env, jclass unused)
{
    (void)env; (void)unused;
    g_calls = 0;
}

JNIEXPORT void JNICALL Java_HookRuntimeTest_teardown(JNIEnv *env, jclass unused)
{
    (void)unused;
    JNI2Hook_Shutdown();
    if (g_target != NULL)
    {
        (*env)->DeleteGlobalRef(env, g_target);
        g_target = NULL;
    }
}
