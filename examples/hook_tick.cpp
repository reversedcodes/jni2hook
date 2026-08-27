/* The same hook in C++.
 *
 *     javac Tick.java
 *     java -cp . Tick ./libj2h_example_tick_cxx.so
 *
 * One rule applies here that has no counterpart in C: an exception must never
 * leave a detour. The JVM calls it through a plain function pointer and has no
 * landing pad for one, so an escaping exception terminates the process. Marking
 * the detour noexcept and catching everything is not belt and braces, it is the
 * only thing standing between a bad day and a crash with no stack trace. */

#include <jni2hook/jni2hook.h>

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace
{
jclass gClass = nullptr;
jmethodID gOriginal = nullptr;
int gTicks = 0;

void JNICALL DetourTick(JNIEnv *env, jobject self) noexcept
{
    try
    {
        std::printf("[hook] tick %d\n", ++gTicks);
        std::fflush(stdout);

        env->CallNonvirtualVoidMethod(self, gClass, gOriginal);
    }
    catch (...)
    {
    }
}

void Install()
{
    if (JNI2Hook_InitFromRunningVm() != JNI2HOOK_OK)
        return;

    JNIEnv *env = nullptr;
    if (JNI2Hook_Attach(&env) != JNI2HOOK_OK)
        return;

    jclass target = env->FindClass("Tick");
    jmethodID tick = target ? env->GetMethodID(target, "tick", "()V") : nullptr;
    if (tick == nullptr)
        return;

    gClass = static_cast<jclass>(env->NewGlobalRef(target));

    if (JNI2Hook_Install(tick, reinterpret_cast<void *>(&DetourTick), &gOriginal) == JNI2HOOK_OK)
    {
        std::printf("[hook] Tick.tick() hooked\n");
        std::fflush(stdout);
    }
}
} // namespace

#if defined(_WIN32)
DWORD WINAPI InstallThread(LPVOID)
{
    Install();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr);
        if (thread != nullptr)
            CloseHandle(thread);
    }
    return TRUE;
}
#else
__attribute__((constructor)) static void OnLoad()
{
    Install();
}
#endif
