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
)

foreach(test_target IN LISTS EDITOR_TEST_TARGETS)
    add_executable(${test_target} ${CMAKE_CURRENT_SOURCE_DIR}/${test_target}.cpp)
    target_link_libraries(${test_target}
        PRIVATE
            editor_test_support
            Qt6::Test
    )
    set_target_properties(${test_target} PROPERTIES
        BUILD_RPATH "${EDITOR_FFMPEG_PREFIX}/lib"
    )
    add_test(NAME ${test_target} COMMAND ${test_target})
    set_tests_properties(${test_target} PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${EDITOR_FFMPEG_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
    )
endforeach()

add_executable(test_vulkan_subtitle_render
    ${CMAKE_CURRENT_SOURCE_DIR}/test_vulkan_subtitle_render.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../offscreen_vulkan_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../render_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../render_stats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../titles.cpp
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
set_target_properties(test_vulkan_subtitle_render PROPERTIES
    BUILD_RPATH "${EDITOR_FFMPEG_PREFIX}/lib"
)
add_test(NAME test_vulkan_subtitle_render COMMAND test_vulkan_subtitle_render)
set_tests_properties(test_vulkan_subtitle_render PROPERTIES
    ENVIRONMENT "LD_LIBRARY_PATH=${EDITOR_FFMPEG_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
)
