foreach(variable JAVAC WORK RUNTIME_DIR REMAP)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/compile" "${WORK}/plugin")

# The readable API the plugin is compiled against. The obfuscated ra and rv are
# deliberately not built here: the remapper works on the class file alone and
# never needs the classes it maps to exist.
execute_process(
    COMMAND "${JAVAC}" -d "${WORK}/compile"
            "${RUNTIME_DIR}/RemapApi.java" "${RUNTIME_DIR}/RemapValue.java"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "javac failed on the compile-time API with exit code ${result}")
endif()

execute_process(
    COMMAND "${JAVAC}" -cp "${WORK}/compile" -d "${WORK}/plugin"
            "${RUNTIME_DIR}/RemapPlugin.java"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "javac failed on the plugin with exit code ${result}")
endif()

execute_process(COMMAND "${REMAP}" "${WORK}/plugin/RemapPlugin.class" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "class remap test failed with exit code ${result}")
endif()
