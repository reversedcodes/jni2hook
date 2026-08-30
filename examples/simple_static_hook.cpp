/* Hooks a static method, whose second JNI parameter is jclass, not jobject. */

#include <jni2hook/jni2hook.h>

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace
{
jclass gClass = nullptr;
jmethodID gOriginal = nullptr;

jint JNICALL MultiplyHook(JNIEnv *env, jclass, jint left, jint right) noexcept
{
    const jint original = env->CallStaticIntMethod(gClass, gOriginal, left, right);
    std::printf("[hook] multiply(%d, %d) originally returned %d\n", left, right, original);
    std::fflush(stdout);
    return original * 2;
}

void Install()
{
    if (JNI2Hook_InitFromRunningVm() != JNI2HOOK_OK)
        return;

    JNIEnv *env = nullptr;
    if (JNI2Hook_Attach(&env) != JNI2HOOK_OK)
        return;

    jclass target = env->FindClass("SimpleTarget");
    jmethodID method = target ? env->GetStaticMethodID(target, "multiply", "(II)I") : nullptr;
    if (method == nullptr)
        return;

    gClass = static_cast<jclass>(env->NewGlobalRef(target));
    const jni2hook_status status =
        JNI2Hook_Install(method, reinterpret_cast<void *>(&MultiplyHook), &gOriginal);
    std::printf("[hook] install: %s\n", JNI2Hook_StatusMessage(status));
    std::fflush(stdout);
}

#if defined(_WIN32)
DWORD WINAPI InstallThread(LPVOID)
{
    Install();
    return 0;
}
#else
void *InstallThread(void *)
{
    Install();
    return nullptr;
}
#endif
} // namespace

/* JNI/JVMTI work stays outside the platform loader callback. */
#if defined(_WIN32)
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        if (HANDLE thread = CreateThread(nullptr, 0, &InstallThread, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
    return TRUE;
}
#else
__attribute__((constructor)) static void LibInit()
{
    pthread_t thread{};
    if (pthread_create(&thread, nullptr, &InstallThread, nullptr) == 0)
        pthread_detach(thread);
}
#endif
