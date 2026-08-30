/* Watches bytecode from a class that is loaded after this library. */

#include <jni2hook/jni2hook.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace
{
/* iload_1, iconst_3, imul, bipush 7, iadd, ireturn */
constexpr char kComputePattern[] = "1B 06 68 10 07 60 AC";

std::atomic<jni2hook_method_watch *> gWatch{nullptr};

void JNICALL ComputeObserver(JNIEnv *, jobject) noexcept
{
    std::puts("[watch] compute() entered");
    std::fflush(stdout);
}

void RegisterWatch()
{
    if (JNI2Hook_InitFromRunningVm() != JNI2HOOK_OK)
        return;

    jni2hook_method_watch *watch = nullptr;
    const jni2hook_status status = JNI2Hook_WatchMethod(kComputePattern, &watch);
    if (status == JNI2HOOK_OK)
        gWatch.store(watch, std::memory_order_release);

    std::printf("[watch] register: %s\n", JNI2Hook_StatusMessage(status));
    std::fflush(stdout);
}

#if defined(_WIN32)
DWORD WINAPI WatchThread(LPVOID)
{
    RegisterWatch();
    return 0;
}
#else
void *WatchThread(void *)
{
    RegisterWatch();
    return nullptr;
}
#endif
} // namespace

extern "C" JNIEXPORT void JNICALL Java_WatchExample_installPreparedHook(JNIEnv *, jclass)
{
    jni2hook_method_watch *watch = gWatch.exchange(nullptr, std::memory_order_acq_rel);
    if (watch == nullptr)
        return;

    jmethodID method = nullptr;
    std::uint32_t offset = 0;
    const jni2hook_status ready = JNI2Hook_GetWatchedMethod(watch, &method, &offset);
    if (ready != JNI2HOOK_OK)
    {
        JNI2Hook_DestroyMethodWatch(watch);
        std::printf("[watch] not ready: %s\n", JNI2Hook_StatusMessage(ready));
        std::fflush(stdout);
        return;
    }

    JNI2Hook_DestroyMethodWatch(watch);
    const jni2hook_status status =
        JNI2Hook_InstallAt(method, offset, reinterpret_cast<void *>(&ComputeObserver));
    std::printf("[watch] install at %u: %s\n", offset, JNI2Hook_StatusMessage(status));
    std::fflush(stdout);
}

#if defined(_WIN32)
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        if (HANDLE thread = CreateThread(nullptr, 0, &WatchThread, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
    return TRUE;
}
#else
__attribute__((constructor)) static void LibInit()
{
    pthread_t thread{};
    if (pthread_create(&thread, nullptr, &WatchThread, nullptr) == 0)
        pthread_detach(thread);
}
#endif
