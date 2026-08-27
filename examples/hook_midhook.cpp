/* Observing a method without replacing it: JNI2Hook_InstallAt inserts a call at
 * a bytecode offset and leaves the body running.
 *
 *     java -cp . Tick ./libj2h_example_midhook.so
 *
 * Use this when you want to know that execution reached a point, not to change
 * what happens there. The detour takes no arguments and returns nothing; it
 * cannot see the locals of the method it sits in. */

#include <jni2hook/jni2hook.h>

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace
{
int gSeen = 0;

void JNICALL DetourSeen(JNIEnv *, jobject) noexcept
{
    std::printf("[midhook] reached tick() %d times\n", ++gSeen);
    std::fflush(stdout);
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

    /* Offset 0 is the start of the method. Any other offset has to name an
       instruction boundary, which is what a signature scan reports. */
    const jni2hook_status status =
        JNI2Hook_InstallAt(tick, 0, reinterpret_cast<void *>(&DetourSeen));

    std::printf("[midhook] %s\n", JNI2Hook_StatusMessage(status));
    std::fflush(stdout);
}
} // namespace

#if defined(_WIN32)
DWORD WINAPI InstallThread(LPVOID)
{
    Install();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
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
