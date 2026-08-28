cmake_minimum_required(VERSION 3.16)

project(jni2hook VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)

if(PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(JNI2HOOK_IS_TOP_LEVEL ON)
else()
    set(JNI2HOOK_IS_TOP_LEVEL OFF)
endif()

if(JNI2HOOK_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Extensions stay on deliberately. The library reaches for RTLD_DEFAULT and
# PTHREAD_MUTEX_RECURSIVE, which are POSIX and GNU rather than ISO C, so
# claiming strict conformance here and then defining _GNU_SOURCE to get around
# it would only be a longer way of saying the same thing.

option(JNI2HOOK_BUILD_TESTS    "Build the test executables" ${JNI2HOOK_IS_TOP_LEVEL})
option(JNI2HOOK_BUILD_EXAMPLES "Build the examples"         OFF)
option(JNI2HOOK_INSTALL        "Generate install rules"     ${JNI2HOOK_IS_TOP_LEVEL})

add_library(jni2hook STATIC
    src/byte_stream.c
    src/class_layout.c
    src/class_file_parser.c
    src/class_file_writer.c
    src/class_transform.c
    src/jni2hook.c
    src/hook/bytecode_scan.c
    src/hook/class_cache.c
    src/hook/field_scan.c
    src/hook/jvm.c
    src/hook/vm_structs.c
    src/visitors/attribute_info.c
    src/visitors/class_file.c
    src/visitors/code_attribute.c
    src/visitors/code_editor.c
    src/visitors/constructor_init.c
    src/visitors/cp_info.c
    src/visitors/instruction.c
    src/visitors/member_info.c
    src/visitors/stack_map_table.c
    src/visitors/type_annotation.c
    src/visitors/visitor.c
)

add_library(jni2hook::jni2hook ALIAS jni2hook)

target_include_directories(jni2hook
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/jni2hook/jni>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/jni2hook/jni>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(MSVC)
    set(JNI2HOOK_WARNINGS /W4)
else()
    set(JNI2HOOK_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()

target_compile_options(jni2hook PRIVATE ${JNI2HOOK_WARNINGS})

set_target_properties(jni2hook PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    VERSION ${PROJECT_VERSION}
)

if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(jni2hook PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

if(JNI2HOOK_BUILD_TESTS)
    enable_testing()

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
    add_test(NAME jni_headers COMMAND j2h_jni_headers)

    add_executable(j2h_utils_api tests/utils_api.c)
    target_link_libraries(j2h_utils_api PRIVATE jni2hook)
    target_compile_options(j2h_utils_api PRIVATE ${JNI2HOOK_WARNINGS})

    find_package(Java QUIET COMPONENTS Development)
    if(Java_JAVAC_EXECUTABLE)
        add_test(NAME classfile_smoke
            COMMAND ${CMAKE_COMMAND}
                -DJAVAC=${Java_JAVAC_EXECUTABLE}
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus/Basic.java
                -DWORK=${CMAKE_CURRENT_BINARY_DIR}/classfile-smoke
                -DROUNDTRIP=$<TARGET_FILE:j2h_roundtrip>
                -DTRANSFORM=$<TARGET_FILE:j2h_transform>
                -DUTILS_API=$<TARGET_FILE:j2h_utils_api>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/classfile_smoke.cmake
        )

        # An insertion at offset 0 shifts everything by the same amount and
        # never lands on a reference. These two cover the interior offsets,
        # where a branch target, a frame or an exception bound sits exactly on
        # the insertion point: one asks the JVM whether the result still
        # verifies, the other asserts where each reference ended up, which is
        # the part a verifier cannot see.
        # Exit code 2 means the environment could not run the check, which ctest
        # reports as skipped rather than passed. insert_offsets needs a JDK with
        # java.lang.classfile for VerifyAll, so 24 or newer; shift_semantics
        # runs no JVM at all and works with any javac.
        if(UNIX)
            add_test(NAME insert_offsets
                COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_insert_offsets.sh)
            set_tests_properties(insert_offsets PROPERTIES
                SKIP_RETURN_CODE 2
                ENVIRONMENT "TRANSFORM=$<TARGET_FILE:j2h_transform>;WORK=${CMAKE_CURRENT_BINARY_DIR}/insert-offsets")

            add_test(NAME shift_semantics
                COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_shift_semantics.sh)
            set_tests_properties(shift_semantics PROPERTIES
                SKIP_RETURN_CODE 2
                ENVIRONMENT "TRANSFORM=$<TARGET_FILE:j2h_transform>;WORK=${CMAKE_CURRENT_BINARY_DIR}/shift-semantics")
        endif()
    endif()

    add_library(j2h_runtime_test SHARED tests/runtime/hook_runtime.c)
    target_link_libraries(j2h_runtime_test PRIVATE jni2hook)
    target_include_directories(j2h_runtime_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    # Whether a hook can avoid adding a method to the class it hooks, and so
    # avoid AllowRedefinitionToAddDeleteMethods entirely.
    add_library(j2h_no_flag_hook SHARED tests/runtime/no_flag_hook.c)
    target_link_libraries(j2h_no_flag_hook PRIVATE jni2hook)
    target_include_directories(j2h_no_flag_hook PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(j2h_no_flag_hook PRIVATE ${JNI2HOOK_WARNINGS})

    if(WIN32)
        add_library(j2h_fake_jvm SHARED tests/windows/fake_jvm.c)
        set_target_properties(j2h_fake_jvm PROPERTIES OUTPUT_NAME jvm PREFIX "")

        add_executable(j2h_vm_structs_windows tests/windows/vm_structs_windows.c)
        target_link_libraries(j2h_vm_structs_windows PRIVATE jni2hook)
        target_include_directories(j2h_vm_structs_windows PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        add_dependencies(j2h_vm_structs_windows j2h_fake_jvm)
        add_test(NAME vm_structs_windows COMMAND j2h_vm_structs_windows)
    endif()
endif()

if(JNI2HOOK_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(JNI2HOOK_INSTALL)
    include(CMakePackageConfigHelpers)

    install(TARGETS jni2hook
        EXPORT jni2hookTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT jni2hookTargets
        FILE jni2hookTargets.cmake
        NAMESPACE jni2hook::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/jni2hook
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/jni2hookConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion
    )

    configure_package_config_file(
        cmake/jni2hookConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/jni2hookConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/jni2hook
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/jni2hookConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/jni2hookConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/jni2hook
    )

    install(FILES README.md LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
    )
endif()
