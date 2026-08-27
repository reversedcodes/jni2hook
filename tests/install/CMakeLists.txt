cmake_minimum_required(VERSION 3.16)

project(jni2hook_install_test LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(jni2hook CONFIG REQUIRED)

add_executable(jni2hook_install_test main.c)
target_link_libraries(jni2hook_install_test PRIVATE jni2hook::jni2hook)

add_executable(jni2hook_install_test_cxx main.cpp)
target_link_libraries(jni2hook_install_test_cxx PRIVATE jni2hook::jni2hook)
