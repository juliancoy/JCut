set(RENDER_REQUEST_SOURCE "${SOURCE_DIR}/editor_render_tools.cpp")
set(RENDER_HEADER "${SOURCE_DIR}/render.h")
set(VULKAN_RENDERER_SOURCE "${SOURCE_DIR}/offscreen_vulkan_renderer_backend.cpp")

foreach(path IN ITEMS "${RENDER_REQUEST_SOURCE}" "${RENDER_HEADER}" "${VULKAN_RENDERER_SOURCE}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Required source file does not exist: ${path}")
    endif()
endforeach()

file(READ "${RENDER_REQUEST_SOURCE}" request_source)
file(READ "${RENDER_HEADER}" render_header)
file(READ "${VULKAN_RENDERER_SOURCE}" vulkan_source)

foreach(required IN ITEMS
        "showCurrentSpeakerName"
        "showCurrentSpeakerOrganization"
        "currentSpeakerNameTextScale"
        "currentSpeakerOrganizationTextScale"
        "currentSpeakerNameVerticalPosition"
        "currentSpeakerOrganizationVerticalPosition")
    if(NOT render_header MATCHES "${required}")
        message(FATAL_ERROR "RenderRequest is missing ${required}")
    endif()
    if(NOT request_source MATCHES "${required}")
        message(FATAL_ERROR "buildRenderRequestFromOutputControls does not populate ${required}")
    endif()
endforeach()

foreach(required IN ITEMS
        "buildSpeakerLabelOverlayImage"
        "renderSpeakerLabelOverlay"
        "activeTranscriptPathForClipFile"
        "speakerProfileFromJson"
        "speakerAtTranscriptSourceFrame")
    if(NOT vulkan_source MATCHES "${required}")
        message(FATAL_ERROR "Offscreen Vulkan export speaker label path is missing ${required}")
    endif()
endforeach()

if(vulkan_source MATCHES "QPainter painter\\(&rendered\\)")
    message(FATAL_ERROR "Speaker labels must not be drawn by a post-render QPainter pass")
endif()
