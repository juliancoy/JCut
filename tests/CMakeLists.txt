set(EDITOR_TEST_TARGETS
    test_frame_handle
    test_memory_budget
    test_async_decoder
    test_timeline_cache
    test_integration
    test_transcript_logic
    test_transcript_tab_follow
    test_playback_policy
    test_temporal_sync_contract
    test_realtime_render_contract
    test_realtime_frame_loss_probe
    test_standard_preview_presentation_pipeline
    test_audio_time_stretch
    test_audio_time_stretch_cache
    test_audio_mix_policy
    test_transform_skip_aware_timing
    test_preview_geometry
    test_waveform_service
    test_shader_grading_logic
    test_grading_keyframes
    test_editor_runtime
    test_facedetections_time_mapping
    test_facedetections_tracking
    test_facedetections_assignment
    test_facedetections_flow
    test_facedetections_artifacts
    test_track_avatar_geometry
    test_facefind_window
    test_vulkan_direct_render_parity
    test_facedetections_preview_smoke
    test_preview_face_track_click_policy
    test_facestream_overlay_snapshot
    test_direct_vulkan_handoff_pipeline_contract
    test_no_proxy_hardware_playback_contract
    test_vulkan_text_generation
    test_editor_runtime_vulkan_export_backend
)

foreach(test_target IN LISTS EDITOR_TEST_TARGETS)
    add_executable(${test_target} ${CMAKE_CURRENT_SOURCE_DIR}/${test_target}.cpp)
    target_link_libraries(${test_target}
        PRIVATE
            editor_test_support
            jcut_editor_runtime
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

target_sources(test_editor_runtime
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../editor_runtime_qt_bridge.cpp
)

target_sources(test_no_proxy_hardware_playback_contract
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../playback_frame_pipeline.cpp
)

target_sources(test_audio_mix_policy
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../audio_engine.cpp
)

target_sources(test_standard_preview_presentation_pipeline
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../playback_frame_pipeline.cpp
)

target_sources(test_vulkan_text_generation
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../vulkan_clear_helpers.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../vulkan_text_renderer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../vulkan_resources.cpp
)
target_link_libraries(test_vulkan_text_generation
    PRIVATE
        Vulkan::Vulkan
)
target_compile_definitions(test_vulkan_text_generation
    PRIVATE
        JCUT_VULKAN_SHADER_DIR="${JCUT_VULKAN_SHADER_OUTPUT_DIR}"
)
if(TARGET jcut_vulkan_shaders)
    add_dependencies(test_vulkan_text_generation jcut_vulkan_shaders)
endif()

set_tests_properties(
    test_playback_policy
    test_temporal_sync_contract
    test_realtime_render_contract
    test_realtime_frame_loss_probe
    test_standard_preview_presentation_pipeline
    test_timeline_cache
    test_audio_time_stretch_cache
    test_audio_mix_policy
    test_transcript_logic
    test_transform_skip_aware_timing
    test_direct_vulkan_handoff_pipeline_contract
    test_no_proxy_hardware_playback_contract
    PROPERTIES LABELS "temporal"
)

add_executable(test_vulkan_subtitle_render
    ${CMAKE_CURRENT_SOURCE_DIR}/test_vulkan_subtitle_render.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../offscreen_vulkan_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../offscreen_vulkan_renderer_backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../offscreen_vulkan_renderer_helpers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../render_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../cpu_overlay_render_backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../render_stats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../render_vulkan_shared.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../titles.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../vulkan_detector_frame_handoff.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../vulkan_clear_helpers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../vulkan_resources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../vulkan_text_renderer.cpp
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

add_test(NAME test_editor_core_excludes_vulkan_export_stub
    COMMAND ${CMAKE_COMMAND}
        -DARCHIVE=$<TARGET_FILE:editor_core>
        -DAR=${CMAKE_AR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_archive_excludes_vulkan_export_stub.cmake
)

add_test(NAME test_contiguous_transcript_sections_state_persistence
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/..
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_contiguous_transcript_sections_state_persistence.cmake
)

add_test(NAME test_contiguous_sections_avatar_column
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/..
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_contiguous_sections_avatar_column.cmake
)

add_test(NAME test_render_status_reports_live_progress
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/..
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_render_status_reports_live_progress.cmake
)

add_test(NAME test_export_audio_uses_rubberband
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/..
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_export_audio_uses_rubberband.cmake
)

add_test(NAME test_export_speaker_labels_in_vulkan_path
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/..
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_export_speaker_labels_in_vulkan_path.cmake
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
        ${CMAKE_CURRENT_SOURCE_DIR}/../vulkan_facedetections_offscreen_benchmark.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../vulkan_facedetections_offscreen_artifact_io.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../vulkan_facedetections_offscreen_resume_state.cpp
)

find_program(JCUT_PYTHON3_EXECUTABLE NAMES python3 python REQUIRED)

add_test(
    NAME test_live_playback_lag_probe
    COMMAND ${JCUT_PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/live_playback_lag_probe.py
            --seconds 2
            --allow-not-playing
)
set_tests_properties(test_live_playback_lag_probe PROPERTIES
    LABELS "temporal;live"
    SKIP_RETURN_CODE 77
)

add_test(
    NAME test_imgui_binary_no_qt
    COMMAND ${JCUT_PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_imgui_binary_no_qt.py
            ${CMAKE_CURRENT_BINARY_DIR}/../jcut_imgui
)
set_tests_properties(test_imgui_binary_no_qt PROPERTIES
    LABELS "imgui;linkage"
    SKIP_RETURN_CODE 77
)

add_executable(test_imgui_standalone_render
    ${CMAKE_CURRENT_SOURCE_DIR}/test_imgui_standalone_render.cpp
)
target_link_libraries(test_imgui_standalone_render
    PRIVATE
        jcut_imgui_runtime
        Qt6::Test
)
target_compile_definitions(test_imgui_standalone_render
    PRIVATE
        JCUT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
        JCUT_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}/.."
        EDITOR_FFMPEG_PREFIX="${EDITOR_FFMPEG_PREFIX}"
)
jcut_force_bundled_ffmpeg_runtime(test_imgui_standalone_render)
add_test(NAME test_imgui_standalone_render COMMAND test_imgui_standalone_render)
set_tests_properties(test_imgui_standalone_render PROPERTIES
    LABELS "imgui;render"
    ENVIRONMENT "LD_LIBRARY_PATH=${EDITOR_FFMPEG_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
)

add_executable(test_imgui_standalone_export
    ${CMAKE_CURRENT_SOURCE_DIR}/test_imgui_standalone_export.cpp
)
target_link_libraries(test_imgui_standalone_export
    PRIVATE
        jcut_imgui_runtime
        Qt6::Test
)
target_compile_definitions(test_imgui_standalone_export
    PRIVATE
        JCUT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
        JCUT_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}/.."
        EDITOR_FFMPEG_PREFIX="${EDITOR_FFMPEG_PREFIX}"
)
jcut_force_bundled_ffmpeg_runtime(test_imgui_standalone_export)
add_test(NAME test_imgui_standalone_export COMMAND test_imgui_standalone_export)
set_tests_properties(test_imgui_standalone_export PROPERTIES
    LABELS "imgui;render;export"
    ENVIRONMENT "LD_LIBRARY_PATH=${EDITOR_FFMPEG_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
)

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
    RESOURCE_LOCK jcut_offscreen_control_server
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
    RESOURCE_LOCK jcut_offscreen_control_server
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
    RESOURCE_LOCK jcut_offscreen_control_server
)

add_test(
    NAME test_ui_responsiveness_budget_offscreen
    COMMAND ${JCUT_PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/rest_smoke_test.py
            --offscreen
            --skip-build
            --skip-restart
            --startup-timeout 30
            --play-timeout 25
            --advance-timeout 25
            --pause-timeout 10
            --monitor-seconds 6
            --poll-interval 0.2
            --profile-interval 1.0
            --stall-timeout 2.0
            --max-playhead-latency-ms 750
            --max-profile-latency-ms 1500
            --max-heartbeat-age-ms 1500
)
set_tests_properties(test_ui_responsiveness_budget_offscreen PROPERTIES
    TIMEOUT 120
    ENVIRONMENT "LD_LIBRARY_PATH=${EDITOR_FFMPEG_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
    RESOURCE_LOCK jcut_offscreen_control_server
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
