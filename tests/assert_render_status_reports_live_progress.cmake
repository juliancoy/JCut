set(SETUP_SOURCE "${SOURCE_DIR}/editor_setup.cpp")
set(ROUTES_SOURCE "${SOURCE_DIR}/control_server_worker_routes.cpp")

foreach(path IN ITEMS "${SETUP_SOURCE}" "${ROUTES_SOURCE}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Required source file does not exist: ${path}")
    endif()
endforeach()

file(READ "${SETUP_SOURCE}" setup_source)
file(READ "${ROUTES_SOURCE}" routes_source)

if(setup_source MATCHES "nullptr, // renderResultCallback")
    message(FATAL_ERROR "Control server renderResultCallback must report live export progress")
endif()

foreach(required IN ITEMS
        "m_renderInProgress"
        "m_liveRenderProfile"
        "m_lastRenderProfile"
        "frames_completed"
        "total_frames")
    if(NOT setup_source MATCHES "${required}")
        message(FATAL_ERROR "editor_setup.cpp render status callback is missing ${required}")
    endif()
endforeach()

foreach(required IN ITEMS
        "liveRenderProgress"
        "framesCompleted"
        "totalFrames"
        "active")
    if(NOT routes_source MATCHES "${required}")
        message(FATAL_ERROR "/render/status response is missing ${required}")
    endif()
endforeach()
