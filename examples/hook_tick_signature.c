/* The same hook as hook_tick.c, except the method is found by what its bytecode
 * looks like instead of by its name.
 *
 *     javac Tick.java
 *     java -cp . Tick ./libj2h_example_tick_signature.so
 *
 * Put the two files side by side: they differ in one line. This is the reason
 * jni2hook exists. In an obfuscated program the method is not called "tick", it
 * is called "a" or "method_1574", and that changes with every release. The body
 * does not, so a pattern over the opcodes keeps finding it.
 *
 * Tick.tick() compiles to:
 *
 *     2A        aload_0
 *     59        dup
 *     B4 ?? ??  getfield     the constant pool index is masked out
 *     04        iconst_1
 *     60        iadd
 *     B5 ?? ??  putfield
 *     B1        return
 *
 * ?? covers exactly the bytes that move between builds: constant pool indices
 * and branch offsets. Read a pattern out of any class with javap -c. */

#include <jni2hook/jni2hook.h>

#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static const char *kTickPattern = "2A 59 B4 ?? ?? 04 60 B5 ?? ?? B1";

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
    if (JNI2Hook_InitFromRunningVm() != JNI2HOOK_OK)
        return;

    JNIEnv *env = NULL;
    if (JNI2Hook_Attach(&env) != JNI2HOOK_OK)
        return;

    jclass target = (*env)->FindClass(env, "Tick");
    if (target == NULL)
        return;

    /* The one line that differs from hook_tick.c, which asks for "tick" by
       name. The offset says where in the method the pattern matched. */
    jmethodID tick = NULL;
    uint32_t offset = 0;
    if (JNI2Hook_FindMethodInClass(target, kTickPattern, &tick, &offset) != JNI2HOOK_OK)
    {
        printf("[hook] no method matches the pattern\n");
        return;
    }

    g_class = (*env)->NewGlobalRef(env, target);

    union
    {
        void(JNICALL *function)(JNIEnv *, jobject);
        void *pointer;
    } detour = {detour_tick};

    if (JNI2Hook_Install(tick, detour.pointer, &g_original) == JNI2HOOK_OK)
    {
        printf("[hook] hooked the method matching the pattern at offset %u\n", offset);
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
