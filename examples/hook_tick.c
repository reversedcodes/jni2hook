/* Hooking a tick method from a library injected into a running JVM.
 *
 *     javac Tick.java
 *     java -cp . Tick ./libj2h_example_tick.so
 */

#include <jni2hook/jni2hook.h>

#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static jclass g_class = NULL;
static jmethodID g_original = NULL;
static int g_ticks = 0;

static void JNICALL detour_tick(JNIEnv *env, jobject self)
{
    printf("[hook] tick %d\n", ++g_ticks);
    fflush(stdout);

    (*env)->CallNonvirtualVoidMethod(env, self, g_class, g_original);
}

static void install(void)
{
    /* Nobody hands an injected library a JavaVM, so it looks up the one already
       running. Its thread is not a Java thread either and has to attach first. */
    if (JNI2Hook_InitFromRunningVm() != JNI2HOOK_OK)
        return;

    JNIEnv *env = NULL;
    if (JNI2Hook_Attach(&env) != JNI2HOOK_OK)
        return;

    jclass target = (*env)->FindClass(env, "Tick");
    jmethodID tick = target ? (*env)->GetMethodID(env, target, "tick", "()V") : NULL;
    if (tick == NULL)
        return;

    g_class = (*env)->NewGlobalRef(env, target);

    /* JNI wants a void*, and ISO C will not cast a function pointer to one. */
    union
    {
        void(JNICALL *function)(JNIEnv *, jobject);
        void *pointer;
    } detour = {detour_tick};

    if (JNI2Hook_Install(tick, detour.pointer, &g_original) == JNI2HOOK_OK)
    {
        printf("[hook] Tick.tick() hooked\n");
        fflush(stdout);
    }
}

#if defined(_WIN32)
static DWORD WINAPI install_thread(LPVOID unused)
{
    (void)unused;
    install();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(NULL, 0, install_thread, NULL, 0, NULL);
        if (thread != NULL)
            CloseHandle(thread);
    }
    return TRUE;
}
#else
__attribute__((constructor)) static void on_load(void)
{
    install();
}
#endif
