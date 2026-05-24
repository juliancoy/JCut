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
    test_facedetections_time_mapping
    test_facedetections_tracking
    test_facedetections_assignment
    test_facedetections_flow
    test_facedetections_artifacts
    test_facefind_window
    test_vulkan_direct_render_parity
    test_facedetections_preview_smoke
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

add_executable(test_facedetections_processed_artifact
    ${CMAKE_CURRENT_SOURCE_DIR}/test_facedetections_processed_artifact.cpp
)
target_link_libraries(test_facedetections_processed_artifact
    PRIVATE
        editor_test_support
        Qt6::Test
)
target_compile_definitions(test_facedetections_processed_artifact
    PRIVATE
        JCUT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
        JCUT_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}/.."
        EDITOR_FFMPEG_PREFIX="${EDITOR_FFMPEG_PREFIX}"
)
jcut_force_bundled_ffmpeg_runtime(test_facedetections_processed_artifact)
add_test(NAME test_facedetections_processed_artifact COMMAND test_facedetections_processed_artifact)
set_tests_properties(test_facedetections_processed_artifact PROPERTIES
    ENVIRONMENT "LD_LIBRARY_PATH=${EDITOR_FFMPEG_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
)

target_sources(test_facedetections_flow
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../speaker_flow_debug.cpp
)

find_program(JCUT_PYTHON3_EXECUTABLE NAMES python3 python REQUIRED)

add_test(
    NAME test_ui_smoke_harness_offscreen
    COMMAND ${JCUT_PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/ui_smoke_harness.py
            --offscreen
            --skip-build
)
set_tests_properties(test_ui_smoke_harness_offscreen PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "LD_LIBRARY_PATH=${EDITOR_FFMPEG_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
)

add_test(
    NAME test_speaker_flow_e2e_offscreen
    COMMAND ${JCUT_PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/ui_smoke_harness.py
            --offscreen
            --skip-build
            --exercise-generated-track-assignment
)
set_tests_properties(test_speaker_flow_e2e_offscreen PROPERTIES
    TIMEOUT 300
    ENVIRONMENT "LD_LIBRARY_PATH=${EDITOR_FFMPEG_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
)

add_test(
    NAME test_rest_smoke_offscreen
    COMMAND ${JCUT_PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/rest_smoke_test.py
            --offscreen
            --skip-build
            --skip-restart
            --startup-timeout 30
            --play-timeout 25
            --advance-timeout 25
            --pause-timeout 10
            --monitor-seconds 2
)
set_tests_properties(test_rest_smoke_offscreen PROPERTIES
    TIMEOUT 90
    ENVIRONMENT "LD_LIBRARY_PATH=${EDITOR_FFMPEG_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
)

add_executable(facedetections_midpoint_detection_harness
    ${CMAKE_CURRENT_SOURCE_DIR}/facedetections_midpoint_detection_harness.cpp
)
target_link_libraries(facedetections_midpoint_detection_harness
    PRIVATE
        editor_test_support
)
target_compile_definitions(facedetections_midpoint_detection_harness
    PRIVATE
        JCUT_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}/.."
        EDITOR_FFMPEG_PREFIX="${EDITOR_FFMPEG_PREFIX}"
)
jcut_force_bundled_ffmpeg_runtime(facedetections_midpoint_detection_harness)

add_executable(facedetections_track_coverage_harness
    ${CMAKE_CURRENT_SOURCE_DIR}/facedetections_track_coverage_harness.cpp
)
target_link_libraries(facedetections_track_coverage_harness
    PRIVATE
        editor_test_support
)
target_compile_definitions(facedetections_track_coverage_harness
    PRIVATE
        JCUT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
        JCUT_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}/.."
        EDITOR_FFMPEG_PREFIX="${EDITOR_FFMPEG_PREFIX}"
)
jcut_force_bundled_ffmpeg_runtime(facedetections_track_coverage_harness)
