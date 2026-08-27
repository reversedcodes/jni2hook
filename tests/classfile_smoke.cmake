foreach(variable JAVAC SOURCE WORK ROUNDTRIP TRANSFORM UTILS_API)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/classes")

execute_process(
    COMMAND "${JAVAC}" -g -d "${WORK}/classes" "${SOURCE}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "javac failed with exit code ${result}")
endif()

file(GLOB_RECURSE class_files "${WORK}/classes/*.class")
if(NOT class_files)
    message(FATAL_ERROR "javac produced no class files")
endif()

execute_process(COMMAND "${ROUNDTRIP}" ${class_files} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "roundtrip failed with exit code ${result}")
endif()

set(basic_class "${WORK}/classes/Basic.class")
execute_process(COMMAND "${UTILS_API}" "${basic_class}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "public utility API smoke test failed with exit code ${result}")
endif()

execute_process(
    COMMAND "${TRANSFORM}" "${basic_class}" compute "(DD)D" "$jni2hook"
            "${WORK}/Basic.native.class"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "native transform failed with exit code ${result}")
endif()

execute_process(
    COMMAND "${TRANSFORM}" "${basic_class}" --insert "compute:(DD)D@0" entryHook
            "${WORK}/Basic.insert.class"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "insert transform failed with exit code ${result}")
endif()

execute_process(
    COMMAND "${ROUNDTRIP}" "${WORK}/Basic.native.class" "${WORK}/Basic.insert.class"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "transformed class roundtrip failed with exit code ${result}")
endif()
