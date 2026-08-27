#include "jvm.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <stdlib.h>
#include <string.h>

static JavaVM *g_vm = NULL;
static jvmtiEnv *g_jvmti = NULL;

static jvmtiEnv *create_jvmti(JavaVM *vm)
{
    static const jint versions[] = {JVMTI_VERSION_1_2, JVMTI_VERSION_1_1, JVMTI_VERSION_1_0};

    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++)
    {
        jvmtiEnv *jvmti = NULL;
        if ((*vm)->GetEnv(vm, (void **)&jvmti, versions[i]) == JNI_OK && jvmti != NULL)
            return jvmti;
    }
    return NULL;
}

typedef jint (*get_created_vms_fn)(JavaVM **, jsize, jsize *);

JavaVM *jvm_find_running(void)
{
    get_created_vms_fn get_vms = NULL;

#if defined(_WIN32)
    HMODULE jvm = GetModuleHandleA("jvm.dll");
    if (jvm != NULL)
    {
        FARPROC symbol = GetProcAddress(jvm, "JNI_GetCreatedJavaVMs");
        if (symbol != NULL)
        {
            _Static_assert(sizeof(get_vms) == sizeof(symbol),
                           "Windows function pointers must have one representation");
            memcpy(&get_vms, &symbol, sizeof(get_vms));
        }
    }
#else
    union
    {
        void *symbol;
        get_created_vms_fn function;
    } found = {dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs")};
    get_vms = found.function;
#endif

    if (get_vms == NULL)
        return NULL;

    JavaVM *vm = NULL;
    jsize count = 0;
    if (get_vms(&vm, 1, &count) != JNI_OK || count < 1)
        return NULL;

    return vm;
}

bool jvm_bind(JavaVM *vm)
{
    if (vm == NULL)
        return false;

    g_vm = vm;
    if (g_jvmti == NULL)
        g_jvmti = create_jvmti(vm);

    return g_jvmti != NULL;
}

void jvm_release(void)
{
    if (g_jvmti != NULL)
    {
        (*g_jvmti)->DisposeEnvironment(g_jvmti);
        g_jvmti = NULL;
    }
    g_vm = NULL;
}

JavaVM *jvm_vm(void)
{
    return g_vm;
}

jvmtiEnv *jvm_jvmti(void)
{
    return g_jvmti;
}

JNIEnv *jvm_env(void)
{
    if (g_vm == NULL)
        return NULL;

    JNIEnv *env = NULL;
    const jint result = (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
    if (result == JNI_OK && env != NULL)
        return env;

    if (result != JNI_EDETACHED)
        return NULL;

    JavaVMAttachArgs args;
    args.version = JNI_VERSION_1_6;
    args.name = "jni2hook";
    args.group = NULL;

    if ((*g_vm)->AttachCurrentThreadAsDaemon(g_vm, (void **)&env, &args) != JNI_OK)
        return NULL;

    return env;
}

bool jvm_is_live(void)
{
    if (g_jvmti == NULL)
        return false;
    if (jvm_env() == NULL)
        return false;

    jvmtiPhase phase = JVMTI_PHASE_DEAD;
    if ((*g_jvmti)->GetPhase(g_jvmti, &phase) != JVMTI_ERROR_NONE)
        return false;

    return phase == JVMTI_PHASE_LIVE;
}

bool jvm_clear_exception(JNIEnv *env)
{
    if (env == NULL || (*env)->ExceptionCheck(env) == JNI_FALSE)
        return false;

    (*env)->ExceptionClear(env);
    return true;
}

char *jvm_take_string(char *jvmti_string)
{
    if (jvmti_string == NULL)
        return NULL;

    char *copy = NULL;
    const size_t length = strlen(jvmti_string);

    copy = malloc(length + 1);
    if (copy != NULL)
        memcpy(copy, jvmti_string, length + 1);

    if (g_jvmti != NULL)
        (*g_jvmti)->Deallocate(g_jvmti, (unsigned char *)jvmti_string);

    return copy;
}

char *jvm_class_name_of(jclass klass)
{
    if (g_jvmti == NULL || klass == NULL)
        return NULL;

    char *signature = NULL;
    if ((*g_jvmti)->GetClassSignature(g_jvmti, klass, &signature, NULL) != JVMTI_ERROR_NONE)
        return NULL;

    char *owned = jvm_take_string(signature);
    if (owned == NULL)
        return NULL;

    const size_t length = strlen(owned);
    if (length >= 2 && owned[0] == 'L' && owned[length - 1] == ';')
    {
        memmove(owned, owned + 1, length - 2);
        owned[length - 2] = 0;
    }

    return owned;
}
