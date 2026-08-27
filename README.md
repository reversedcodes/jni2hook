# jni2hook

jni2hook is a C11 library for hooking Java methods in a running HotSpot JVM.
It rewrites class files through JVMTI and has no build-time JDK dependency. JNI,
JVMTI, and class-file constants are vendored from OpenJDK.

The library supports:

- replacing a Java method with a native detour while preserving its original body;
- inserting a `void` native callback at a bytecode offset;
- finding methods and fields with bytecode patterns;
- reading and editing class files without a running JVM.

## Requirements

- CMake 3.16 or newer
- a C11 compiler
- Linux or Windows
- a HotSpot-based JVM at runtime

The build intentionally uses the compiler's C extensions. The runtime needs
POSIX and GNU interfaces such as `PTHREAD_MUTEX_RECURSIVE` and `RTLD_DEFAULT`.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Tests are built by default when jni2hook is the top-level project. Examples are
optional:

```sh
cmake -S . -B build -DJNI2HOOK_BUILD_EXAMPLES=ON
cmake --build build
```

## Use with CMake

With `FetchContent`:

```cmake
include(FetchContent)

set(JNI2HOOK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(JNI2HOOK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JNI2HOOK_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(jni2hook
    GIT_REPOSITORY https://github.com/reversedcodes/jni2hook.git
    GIT_TAG 2dc959f4fb49cd1ee7fb8b7a8535a7bcc1d7da1f
)

FetchContent_MakeAvailable(jni2hook)
target_link_libraries(my_library PRIVATE jni2hook::jni2hook)
```

As a subdirectory:

```cmake
add_subdirectory(external/jni2hook)
target_link_libraries(my_library PRIVATE jni2hook::jni2hook)
```

Or install and consume the package:

```sh
cmake --install build --prefix /path/to/prefix
```

```cmake
find_package(jni2hook CONFIG REQUIRED)
target_link_libraries(my_library PRIVATE jni2hook::jni2hook)
```

## Basic API

Initialize from an injected native library, locate a method, and install a
detour:

```c
jni2hook_status status = JNI2Hook_InitFromRunningVm();
if (status != JNI2HOOK_OK)
    return;

JNIEnv *env = NULL;
if (JNI2Hook_Attach(&env) != JNI2HOOK_OK)
    return;

jclass target = (*env)->FindClass(env, "example/Target");
jmethodID method = (*env)->GetMethodID(env, target, "tick", "()V");
jmethodID original = NULL;

status = JNI2Hook_Install(method, native_detour, &original);
```

On Windows, do not call the API while the loader lock is held in `DllMain`.
Start an initialization thread and return from `DllMain` without waiting for it.

Use `JNI2Hook_Uninstall` to restore a method and `JNI2Hook_Shutdown` before the
library is unloaded. A detour must use the matching JNI signature. See
[`examples/`](examples/) for replacement hooks, inserted callbacks, bytecode
scans, field resolution, and class-file utilities.

## Important limitation

After uninstalling, JIT-compiled callers may briefly retain the old native
target. If the detour code can be unloaded, place a permanent, disarmable
trampoline between the JVM and the library.

## License

jni2hook is licensed under the [MIT License](LICENSE). Vendored OpenJDK headers
retain their original GPLv2 with Classpath Exception notices.
