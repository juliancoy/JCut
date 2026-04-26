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
