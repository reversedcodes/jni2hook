add_executable(j2h_example_dump dump_class.c)
target_link_libraries(j2h_example_dump PRIVATE jni2hook)
target_compile_options(j2h_example_dump PRIVATE ${JNI2HOOK_WARNINGS})

add_executable(j2h_example_rewrite rewrite_class.c)
target_link_libraries(j2h_example_rewrite PRIVATE jni2hook)
target_compile_options(j2h_example_rewrite PRIVATE ${JNI2HOOK_WARNINGS})

add_library(j2h_example_tick SHARED hook_tick.c)
target_link_libraries(j2h_example_tick PRIVATE jni2hook)
target_compile_options(j2h_example_tick PRIVATE ${JNI2HOOK_WARNINGS})

add_library(j2h_example_tick_signature SHARED hook_tick_signature.c)
target_link_libraries(j2h_example_tick_signature PRIVATE jni2hook)
target_compile_options(j2h_example_tick_signature PRIVATE ${JNI2HOOK_WARNINGS})

enable_language(CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(j2h_example_tick_cxx SHARED hook_tick.cpp)
target_link_libraries(j2h_example_tick_cxx PRIVATE jni2hook)
target_compile_options(j2h_example_tick_cxx PRIVATE ${JNI2HOOK_WARNINGS})

foreach(example midhook field lifecycle)
    add_library(j2h_example_${example} SHARED hook_${example}.cpp)
    target_link_libraries(j2h_example_${example} PRIVATE jni2hook)
    target_compile_options(j2h_example_${example} PRIVATE ${JNI2HOOK_WARNINGS})
endforeach()

foreach(example void return static midhook pattern)
    add_library(j2h_example_simple_${example} SHARED simple_${example}_hook.cpp)
    target_link_libraries(j2h_example_simple_${example} PRIVATE jni2hook)
    target_compile_options(j2h_example_simple_${example} PRIVATE ${JNI2HOOK_WARNINGS})
endforeach()

add_executable(j2h_example_layout read_layout.cpp)
target_link_libraries(j2h_example_layout PRIVATE jni2hook)
target_compile_options(j2h_example_layout PRIVATE ${JNI2HOOK_WARNINGS})

configure_file(Tick.java ${CMAKE_CURRENT_BINARY_DIR}/Tick.java COPYONLY)
configure_file(SimpleTarget.java ${CMAKE_CURRENT_BINARY_DIR}/SimpleTarget.java COPYONLY)
