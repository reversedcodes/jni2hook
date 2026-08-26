cmake_minimum_required(VERSION 3.16)

project(jni2hook LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(JAVA_HOME "$ENV{JAVA_HOME}" CACHE PATH "JDK providing jni.h and jvmti.h")
if(NOT JAVA_HOME)
    find_program(JNI2HOOK_JAVAC NAMES javac)
    if(JNI2HOOK_JAVAC)
        get_filename_component(JNI2HOOK_JAVAC "${JNI2HOOK_JAVAC}" REALPATH)
        get_filename_component(JAVA_HOME "${JNI2HOOK_JAVAC}" DIRECTORY)
        get_filename_component(JAVA_HOME "${JAVA_HOME}" DIRECTORY)
    endif()
endif()

set(JAVA_INCLUDE_DIRS "")
if(EXISTS "${JAVA_HOME}/include/jni.h")
    set(JAVA_INCLUDE_DIRS "${JAVA_HOME}/include")
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        list(APPEND JAVA_INCLUDE_DIRS "${JAVA_HOME}/include/win32")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        list(APPEND JAVA_INCLUDE_DIRS "${JAVA_HOME}/include/darwin")
    else()
        list(APPEND JAVA_INCLUDE_DIRS "${JAVA_HOME}/include/linux")
    endif()
else()
    message(STATUS "jni2hook: no JDK found, building the class file parser only")
endif()

add_library(jni2hook STATIC
    src/byte_stream.c
    src/class_file_parser.c
    src/class_file_writer.c
    src/visitors/attribute_info.c
    src/visitors/class_file.c
    src/visitors/code_attribute.c
    src/visitors/cp_info.c
    src/visitors/member_info.c
)

target_include_directories(jni2hook
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(jni2hook PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion
)

set_target_properties(jni2hook PROPERTIES POSITION_INDEPENDENT_CODE ON)

option(JNI2HOOK_BUILD_TESTS "Build the class file roundtrip test" ON)

if(JNI2HOOK_BUILD_TESTS)
    add_executable(j2h_roundtrip tests/roundtrip.c)
    target_link_libraries(j2h_roundtrip PRIVATE jni2hook)
    target_include_directories(j2h_roundtrip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()
