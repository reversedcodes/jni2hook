/* Reaching a field whose name is obfuscated, and scanning every loaded class.
 *
 *     java -cp . Tick ./libj2h_example_field.so
 *
 * JNI2Hook_FindFieldInClass takes the same pattern as a method scan plus an
 * offset into the match. At that offset it expects a field access instruction
 * and follows its constant pool index, so the field is named by where it is
 * used rather than by what it is called. */

#include <jni2hook/jni2hook.h>

#include <cstdint>
#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace
{
/* aload_0, dup, getfield, iconst_1, iadd, putfield, return */
const char *kTickPattern = "2A 59 B4 ?? ?? 04 60 B5 ?? ?? B1";

void Install()
{
    if (JNI2Hook_InitFromRunningVm() != JNI2HOOK_OK)
        return;

    JNIEnv *env = nullptr;
    if (JNI2Hook_Attach(&env) != JNI2HOOK_OK)
        return;

    jclass target = env->FindClass("Tick");
    if (target == nullptr)
        return;

    /* The getfield sits two bytes into the match. */
    jfieldID field = nullptr;
    int isStatic = 0;
    jni2hook_status status = JNI2Hook_FindFieldInClass(target, kTickPattern, 2, &field, &isStatic);
    if (status != JNI2HOOK_OK)
    {
        std::printf("[field] %s\n", JNI2Hook_StatusMessage(status));
        return;
    }

    jmethodID ctor = env->GetMethodID(target, "<init>", "()V");
    jobject probe = env->NewObject(target, ctor);
    std::printf("[field] resolved through bytecode, %s, reads %d\n",
                isStatic ? "static" : "instance", env->GetIntField(probe, field));

    /* The same pattern without a class searches every loaded class instead,
       which is what you want when you do not know where the method lives. */
    jmethodID found = nullptr;
    std::uint32_t offset = 0;
    jni2hook_search_stats stats{};
    status = JNI2Hook_FindMethod(kTickPattern, &found, &offset, &stats);

    std::printf("[field] global scan: %s after %zu classes, %zu methods\n",
                JNI2Hook_StatusMessage(status), stats.classes_scanned, stats.methods_scanned);
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
