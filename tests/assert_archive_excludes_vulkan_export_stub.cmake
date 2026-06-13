if(NOT DEFINED ARCHIVE OR NOT EXISTS "${ARCHIVE}")
    message(FATAL_ERROR "ARCHIVE does not exist: ${ARCHIVE}")
endif()

if(NOT DEFINED AR OR AR STREQUAL "")
    set(AR ar)
endif()

execute_process(
    COMMAND "${AR}" t "${ARCHIVE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE members
    ERROR_VARIABLE error_output
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Failed to inspect archive ${ARCHIVE}: ${error_output}")
endif()

if(members MATCHES "render_vulkan_export_stub\\.cpp\\.o")
    message(FATAL_ERROR "editor_core must not contain render_vulkan_export_stub.cpp.o")
endif()

if(NOT members MATCHES "offscreen_vulkan_renderer_backend\\.cpp\\.o")
    message(FATAL_ERROR "editor_core must contain offscreen_vulkan_renderer_backend.cpp.o")
endif()
