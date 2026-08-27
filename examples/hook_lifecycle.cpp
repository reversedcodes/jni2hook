/* Install, ask, uninstall, shut down - and what to do when something fails.
 *
 *     java -cp . Tick ./libj2h_example_lifecycle.so
 *
 * Every entry point returns a jni2hook_status, and JNI2Hook_StatusMessage turns
 * it into a sentence. When that sentence is "a JVMTI operation failed", the
 * detail is in JNI2Hook_LastJvmtiError. Error 63 in particular has one cause
 * worth knowing, which is why the redefinition flag is reported here. */

#include <jni2hook/jni2hook.h>

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace
{
jclass gClass = nullptr;
jmethodID gOriginal = nullptr;

void JNICALL DetourTick(JNIEnv *env, jobject self) noexcept
{
    env->CallNonvirtualVoidMethod(self, gClass, gOriginal);
}

void Report(const char *what, jni2hook_status status)
{
    std::printf("[lifecycle] %-12s %s", what, JNI2Hook_StatusMessage(status));
    if (status == JNI2HOOK_ERR_JVMTI)
        std::printf(" (jvmti %d)", static_cast<int>(JNI2Hook_LastJvmtiError()));
    std::printf("\n");
    std::fflush(stdout);
}

void Install()
{
    Report("init", JNI2Hook_InitFromRunningVm());

    /* RedefineClasses will not add the copy that holds the original body
       unless a VM flag is on, and it is off by default on every current
       JDK. jni2hook sets it from inside; -1 here means every install will
       fail with JVMTI error 63 and this is the only place that says why. */
    const int flag = JNI2Hook_ForcedRedefinitionFlag();
    std::printf("[lifecycle] redefinition flag %s\n", flag == 1   ? "switched on"
                                                      : flag == 0 ? "already on"
                                                                  : "UNREACHABLE");

    JNIEnv *env = nullptr;
    if (JNI2Hook_Attach(&env) != JNI2HOOK_OK)
        return;

    jclass target = env->FindClass("Tick");
    jmethodID tick = target ? env->GetMethodID(target, "tick", "()V") : nullptr;
    if (tick == nullptr)
        return;

    gClass = static_cast<jclass>(env->NewGlobalRef(target));

    std::printf("[lifecycle] installed? %d\n", JNI2Hook_IsInstalled(tick));
    Report("install", JNI2Hook_Install(tick, reinterpret_cast<void *>(&DetourTick), &gOriginal));
    std::printf("[lifecycle] installed? %d\n", JNI2Hook_IsInstalled(tick));

    /* Installing twice is refused rather than silently stacking. */
    Report("again", JNI2Hook_Install(tick, reinterpret_cast<void *>(&DetourTick), nullptr));

    Report("uninstall", JNI2Hook_Uninstall(tick));
    std::printf("[lifecycle] installed? %d\n", JNI2Hook_IsInstalled(tick));

    Report("uninstall", JNI2Hook_Uninstall(tick));

    env->DeleteGlobalRef(gClass);
    JNI2Hook_Shutdown();
    std::printf("[lifecycle] shut down\n");
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
