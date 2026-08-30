# jni2hook

[![CI](https://github.com/reversedcodes/jni2hook/actions/workflows/ci.yml/badge.svg)](https://github.com/reversedcodes/jni2hook/actions/workflows/ci.yml)

jni2hook is a C11 library for hooking Java methods in a running HotSpot JVM.
It rewrites class files through JVMTI and has no build-time JDK dependency.

It supports:

- replacing Java methods while keeping the original body callable;
- inserting native callbacks at bytecode offsets;
- finding methods and fields with bytecode patterns;
- watching and parsing classes that have not been loaded yet;
- reading and editing class files without a running JVM.

## Requirements

- CMake 3.16 or newer
- a C11 compiler
- Linux or Windows
- a HotSpot-based JVM at runtime

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Build the examples with `-DJNI2HOOK_BUILD_EXAMPLES=ON`.

## CMake integration

```cmake
include(FetchContent)

set(JNI2HOOK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(JNI2HOOK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JNI2HOOK_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(jni2hook
    GIT_REPOSITORY https://github.com/reversedcodes/jni2hook.git
    GIT_TAG 1.1-Beta
)
FetchContent_MakeAvailable(jni2hook)

target_link_libraries(my_library PRIVATE jni2hook::jni2hook)
```

For a local copy, replace `FetchContent` with:

```cmake
add_subdirectory(external/jni2hook)
```

## Basic hook

```c
if (JNI2Hook_InitFromRunningVm() != JNI2HOOK_OK)
    return;

JNIEnv *env = NULL;
if (JNI2Hook_Attach(&env) != JNI2HOOK_OK)
    return;

jclass target = (*env)->FindClass(env, "example/Target");
jmethodID method = (*env)->GetMethodID(env, target, "tick", "()V");
jmethodID original = NULL;

jni2hook_status status = JNI2Hook_Install(method, native_detour, &original);
```

The detour must use the matching JNI signature.

## Watch an unloaded class

`JNI2Hook_WatchMethod` also observes future class loads. Their raw bytecode is
parsed in `ClassFileLoadHook`, and `ClassPrepare` resolves the match to a
`jmethodID`.

```c
jni2hook_method_watch *watch = NULL;
JNI2Hook_WatchMethod("1B 06 68 10 07 60 AC", &watch);

/* Called once after the target class has been prepared. */
jmethodID method = NULL;
uint32_t offset = 0;
if (JNI2Hook_GetWatchedMethod(watch, &method, &offset) == JNI2HOOK_OK) {
    JNI2Hook_DestroyMethodWatch(watch);
    JNI2Hook_InstallAt(method, offset, native_callback);
}
```

This is an O(1) readiness check, not another scan through loaded classes. A
complete example is in [`examples/simple_watch_hook.cpp`](examples/simple_watch_hook.cpp)
and [`examples/WatchExample.java`](examples/WatchExample.java).

## Unloading

Call `JNI2Hook_Uninstall` to restore a method and `JNI2Hook_Shutdown` before
unloading the native library. On Windows, run JNI/JVMTI work on a separate
thread instead of inside the `DllMain` loader lock.

JIT-compiled callers may briefly retain an old native target after uninstall.
If the detour code can be unloaded, use a permanent, disarmable trampoline
between the JVM and the library.

## License

jni2hook is licensed under the [MIT License](LICENSE). Vendored OpenJDK headers
retain their original GPLv2 with Classpath Exception notices.
