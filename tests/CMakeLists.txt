set(EDITOR_TEST_TARGETS
    test_frame_handle
    test_memory_budget
    test_async_decoder
    test_timeline_cache
    test_integration
    test_transcript_logic
    test_transcript_tab_follow
    test_playback_policy
    test_transform_skip_aware_timing
    test_preview_geometry
    test_waveform_service
    test_shader_grading_logic
    test_grading_keyframes
    test_facestream_time_mapping
    test_facestream_tracking
    test_facestream_assignment
    test_facestream_flow
    test_vulkan_direct_render_parity
    test_facestream_preview_smoke
)

foreach(test_target IN LISTS EDITOR_TEST_TARGETS)
    add_executable(${test_target} ${CMAKE_CURRENT_SOURCE_DIR}/${test_target}.cpp)
    target_link_libraries(${test_target}
        PRIVATE
            editor_test_support
            Qt6::Test
    )
    target_compile_definitions(${test_target}
        PRIVATE
            JCUT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
            JCUT_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}/.."
            EDITOR_FFMPEG_PREFIX="${EDITOR_FFMPEG_PREFIX}"
    )
    jcut_force_bundled_ffmpeg_runtime(${test_target})
    add_test(NAME ${test_target} COMMAND ${test_target})
    set_tests_properties(${test_target} PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${EDITOR_FFMPEG_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
    )
endforeach()

add_executable(test_vulkan_subtitle_render
    ${CMAKE_CURRENT_SOURCE_DIR}/test_vulkan_subtitle_render.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../offscreen_vulkan_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../render_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../overlay_render_backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../render_stats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../titles.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../vulkan_detector_frame_handoff.cpp
)
target_link_libraries(test_vulkan_subtitle_render
    PRIVATE
        editor_test_support
        Qt6::Test
        Vulkan::Vulkan
)
if(CUDA_DRIVER_LIBRARY)
    target_link_libraries(test_vulkan_subtitle_render PRIVATE ${CUDA_DRIVER_LIBRARY})
endif()
target_compile_definitions(test_vulkan_subtitle_render
    PRIVATE
        JCUT_VULKAN_SHADER_DIR="${JCUT_VULKAN_SHADER_OUTPUT_DIR}"
        JCUT_HAS_CUDA_DRIVER=$<IF:$<BOOL:${CUDA_DRIVER_LIBRARY}>,1,0>
)
if(TARGET jcut_vulkan_shaders)
    add_dependencies(test_vulkan_subtitle_render jcut_vulkan_shaders)
endif()
jcut_force_bundled_ffmpeg_runtime(test_vulkan_subtitle_render)
add_test(NAME test_vulkan_subtitle_render COMMAND test_vulkan_subtitle_render)
set_tests_properties(test_vulkan_subtitle_render PROPERTIES
    ENVIRONMENT "LD_LIBRARY_PATH=${EDITOR_FFMPEG_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
)

add_executable(test_opengl_vulkan_render_exactness
    ${CMAKE_CURRENT_SOURCE_DIR}/test_opengl_vulkan_render_exactness.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../render_gpu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../offscreen_vulkan_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../gl_frame_texture_shared.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../render_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../overlay_render_backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../render_stats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../titles.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../vulkan_detector_frame_handoff.cpp
)
target_link_libraries(test_opengl_vulkan_render_exactness
    PRIVATE
        editor_test_support
        Qt6::Test
        Vulkan::Vulkan
)
if(CUDA_DRIVER_LIBRARY)
    target_link_libraries(test_opengl_vulkan_render_exactness PRIVATE ${CUDA_DRIVER_LIBRARY})
endif()
target_compile_definitions(test_opengl_vulkan_render_exactness
    PRIVATE
        JCUT_VULKAN_SHADER_DIR="${JCUT_VULKAN_SHADER_OUTPUT_DIR}"
        JCUT_HAS_CUDA_DRIVER=$<IF:$<BOOL:${CUDA_DRIVER_LIBRARY}>,1,0>
)
if(TARGET jcut_vulkan_shaders)
    add_dependencies(test_opengl_vulkan_render_exactness jcut_vulkan_shaders)
endif()
jcut_force_bundled_ffmpeg_runtime(test_opengl_vulkan_render_exactness)
add_test(NAME test_opengl_vulkan_render_exactness COMMAND test_opengl_vulkan_render_exactness)
set_tests_properties(test_opengl_vulkan_render_exactness PROPERTIES
    ENVIRONMENT "LD_LIBRARY_PATH=${EDITOR_FFMPEG_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
)

add_executable(facestream_midpoint_detection_harness
    ${CMAKE_CURRENT_SOURCE_DIR}/facestream_midpoint_detection_harness.cpp
)
target_link_libraries(facestream_midpoint_detection_harness
    PRIVATE
        editor_test_support
)
target_compile_definitions(facestream_midpoint_detection_harness
    PRIVATE
        JCUT_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}/.."
        EDITOR_FFMPEG_PREFIX="${EDITOR_FFMPEG_PREFIX}"
)
jcut_force_bundled_ffmpeg_runtime(facestream_midpoint_detection_harness)
