cmake_minimum_required(VERSION 3.16)

project(jni2hook LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(jni2hook STATIC
    src/byte_stream.c
    src/class_file_parser.c
    src/class_transform.c
    src/jni2hook.c
    src/hook/class_cache.c
    src/hook/jvm.c
    src/hook/vm_structs.c
    src/class_file_writer.c
    src/visitors/attribute_info.c
    src/visitors/class_file.c
    src/visitors/code_attribute.c
    src/visitors/cp_info.c
    src/visitors/member_info.c
    src/visitors/visitor.c
)

target_include_directories(jni2hook
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/include/jni2hook/jni
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(MSVC)
    set(JNI2HOOK_WARNINGS /W4)
else()
    set(JNI2HOOK_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()

target_compile_options(jni2hook PRIVATE ${JNI2HOOK_WARNINGS})
set_target_properties(jni2hook PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NOT WIN32)
    target_link_libraries(jni2hook PUBLIC ${CMAKE_DL_LIBS})
endif()

option(JNI2HOOK_BUILD_TESTS "Build the test executables" ON)

if(JNI2HOOK_BUILD_TESTS)
    add_executable(j2h_roundtrip tests/roundtrip.c)
    target_link_libraries(j2h_roundtrip PRIVATE jni2hook)
    target_include_directories(j2h_roundtrip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(j2h_roundtrip PRIVATE ${JNI2HOOK_WARNINGS})
    add_executable(j2h_transform tests/transform.c)
    target_link_libraries(j2h_transform PRIVATE jni2hook)
    target_include_directories(j2h_transform PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(j2h_transform PRIVATE ${JNI2HOOK_WARNINGS})
    add_executable(j2h_jni_headers tests/jni_headers.c)
    target_link_libraries(j2h_jni_headers PRIVATE jni2hook)
    add_library(j2h_runtime_test SHARED tests/runtime/hook_runtime.c)
    target_link_libraries(j2h_runtime_test PRIVATE jni2hook)
    target_include_directories(j2h_runtime_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()
