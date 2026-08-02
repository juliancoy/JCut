cmake_minimum_required(VERSION 3.24)

project(editor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(EDITOR_ASAN "Enable AddressSanitizer" OFF)
option(EDITOR_BUILD_TESTS "Build editor tests" ON)
set(JCUT_REQUIRED_QT_VERSION "6.8.3" CACHE STRING
    "Exact Qt version used to build and distribute JCut.")
option(JCUT_ENABLE_RUBBERBAND_TIME_STRETCH
    "Use Rubber Band as the default pitch-preserving audio time-stretch backend when available."
    ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(EDITOR_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

find_package(PkgConfig REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_package(X11 REQUIRED)
find_package(Qt6 ${JCUT_REQUIRED_QT_VERSION} EXACT REQUIRED
             COMPONENTS Core Gui Widgets Network Concurrent Test)
if(NOT TARGET Qt6::CorePrivate OR NOT TARGET Qt6::GuiPrivate)
    message(FATAL_ERROR
        "JCut uses Qt private QRhi APIs and requires the CorePrivate and GuiPrivate "
        "targets from the exact Qt ${JCUT_REQUIRED_QT_VERSION} SDK. "
        "Current Qt6_DIR: ${Qt6_DIR}")
endif()
get_target_property(JCUT_QT_NETWORK_DISABLED_FEATURES Qt6::Network QT_DISABLED_PUBLIC_FEATURES)
if(JCUT_QT_NETWORK_DISABLED_FEATURES MATCHES "(^|;)ssl(;|$)")
    message(FATAL_ERROR
        "The selected Qt6::Network was built without SSL support, so JCut cannot complete "
        "Supabase HTTPS login. Use a Qt build with the OpenSSL TLS backend enabled and "
        "deploy its matching tls/libqopensslbackend.so plugin. Current Qt6_DIR: ${Qt6_DIR}")
endif()
find_program(GLSLANG_VALIDATOR glslangValidator)
find_library(CUDA_DRIVER_LIBRARY cuda)
find_path(CUDA_DRIVER_INCLUDE_DIR cuda.h
    PATH_SUFFIXES include include/cuda)
if(CUDA_DRIVER_LIBRARY AND CUDA_DRIVER_INCLUDE_DIR)
    set(JCUT_HAS_CUDA_DRIVER ON)
else()
    set(JCUT_HAS_CUDA_DRIVER OFF)
endif()

function(jcut_fix_qt_private_target target_name)
    if(NOT TARGET "${target_name}")
        return()
    endif()

    get_target_property(_qt_private_includes "${target_name}" INTERFACE_INCLUDE_DIRECTORIES)
    if(NOT _qt_private_includes)
        return()
    endif()

    set(_fixed_qt_private_includes "")
    foreach(_include_dir IN LISTS _qt_private_includes)
        # Generator expressions (Qt >= 6.7 private targets) resolve at build
        # time; pass them through untouched.
        if(_include_dir MATCHES "\\$<")
            list(APPEND _fixed_qt_private_includes "${_include_dir}")
            continue()
        endif()
        if(EXISTS "${_include_dir}")
            list(APPEND _fixed_qt_private_includes "${_include_dir}")
            continue()
        endif()

        string(REPLACE
            "/usr/include/x86_64-linux-gnu/qt6"
            "${CMAKE_CURRENT_SOURCE_DIR}/.deps/qt6-base-private-dev/usr/include/x86_64-linux-gnu/qt6"
            _local_include_dir
            "${_include_dir}")
        if(EXISTS "${_local_include_dir}")
            list(APPEND _fixed_qt_private_includes "${_local_include_dir}")
        else()
            message(FATAL_ERROR
                "${target_name} references missing Qt private include directory '${_include_dir}'. "
                "Install qt6-base-private-dev or rerun ./build.sh to fetch a local copy.")
        endif()
    endforeach()

    set_target_properties("${target_name}" PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_fixed_qt_private_includes}")
endfunction()

# A few legacy runtime headers still include QWidget declarations even though
# their implementation targets do not call or link Qt Widgets. Keep those
# private compile requirements from becoming static-library link dependencies.
function(jcut_use_qt_widgets_headers_only target_name)
    target_include_directories("${target_name}" PRIVATE
        $<TARGET_PROPERTY:Qt6::Widgets,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions("${target_name}" PRIVATE
        $<TARGET_PROPERTY:Qt6::Widgets,INTERFACE_COMPILE_DEFINITIONS>)
endfunction()

jcut_fix_qt_private_target(Qt6::GuiPrivate)
jcut_fix_qt_private_target(Qt6::CorePrivate)

option(JCUT_USE_SYSTEM_FFMPEG
    "Deprecated escape hatch. This project now requires the bundled FFmpeg build."
    OFF)
set(EDITOR_FFMPEG_PREFIX
    "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg-install"
    CACHE PATH "Pinned FFmpeg install prefix. Bundled FFmpeg is mandatory.")
set(EDITOR_RUBBERBAND_PREFIX
    "${CMAKE_CURRENT_SOURCE_DIR}/.deps/rubberband-install"
    CACHE PATH "Vendored Rubber Band install prefix used by build.sh.")

if(JCUT_USE_SYSTEM_FFMPEG)
    message(FATAL_ERROR
        "JCUT_USE_SYSTEM_FFMPEG=ON is no longer supported. "
        "This project now requires the bundled FFmpeg build at ${EDITOR_FFMPEG_PREFIX}.")
endif()

if(NOT JCUT_USE_SYSTEM_FFMPEG)
    set(EDITOR_FFMPEG_PKGCONFIG_DIR "${EDITOR_FFMPEG_PREFIX}/lib/pkgconfig")
    if(NOT EXISTS "${EDITOR_FFMPEG_PKGCONFIG_DIR}/libavcodec.pc")
        message(FATAL_ERROR
            "Bundled FFmpeg pkg-config files not found at ${EDITOR_FFMPEG_PKGCONFIG_DIR}. "
            "Run ./build.sh to bootstrap the pinned FFmpeg build.")
    endif()

    set(_saved_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
    set(_jcut_pkg_config_path "${EDITOR_FFMPEG_PKGCONFIG_DIR}")
    set(_jcut_rubberband_pkgconfig_dir "${EDITOR_RUBBERBAND_PREFIX}/lib/pkgconfig")
    if(EXISTS "${_jcut_rubberband_pkgconfig_dir}/rubberband.pc")
        set(_jcut_pkg_config_path "${_jcut_pkg_config_path}:${_jcut_rubberband_pkgconfig_dir}")
    endif()
    if(_saved_pkg_config_path)
        set(ENV{PKG_CONFIG_PATH} "${_jcut_pkg_config_path}:${_saved_pkg_config_path}")
    else()
        set(ENV{PKG_CONFIG_PATH} "${_jcut_pkg_config_path}")
    endif()
endif()

pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libswscale
    libswresample
)
pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET
    glfw3
)
pkg_check_modules(FONT_RENDER REQUIRED IMPORTED_TARGET
    fontconfig
    freetype2
)
set(JCUT_HAVE_PIPEWIRE 0)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_check_modules(PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3>=0.3)
    if(TARGET PkgConfig::PIPEWIRE)
        set(JCUT_HAVE_PIPEWIRE 1)
    endif()
endif()
set(JCUT_HAVE_RUBBERBAND 0)
if(JCUT_ENABLE_RUBBERBAND_TIME_STRETCH)
    pkg_check_modules(RUBBERBAND QUIET IMPORTED_TARGET rubberband>=3.0.0)
    if(TARGET PkgConfig::RUBBERBAND)
        set(JCUT_HAVE_RUBBERBAND 1)
    else()
        message(FATAL_ERROR
            "Rubber Band was not found via pkg-config, but JCUT_ENABLE_RUBBERBAND_TIME_STRETCH is ON. "
            "Run ./build.sh with Meson installed, or install librubberband development files. "
            "SOLA fallback is intentionally disabled.")
    endif()
endif()

# Imported targets get SYSTEM include treatment (-isystem), which the compiler
# searches after every plain -I directory and in interface order — on macOS,
# Qt's interface includes put /opt/homebrew/include ahead of the pinned FFmpeg
# include dir, so a Homebrew FFmpeg of a different major version shadowed the
# bundled headers while the bundled libraries were linked. Mixed struct
# layouts silently corrupt AVCodecParameters/AVCodecContext field reads
# (sample_rate read as 0, 0x0 frame sizes). Emit the pinned include dirs as
# plain -I so they always win over any -isystem system prefix.
if(TARGET PkgConfig::FFMPEG)
    set_target_properties(PkgConfig::FFMPEG PROPERTIES IMPORTED_NO_SYSTEM TRUE)
endif()
if(TARGET PkgConfig::RUBBERBAND)
    set_target_properties(PkgConfig::RUBBERBAND PROPERTIES IMPORTED_NO_SYSTEM TRUE)
endif()

if(NOT JCUT_USE_SYSTEM_FFMPEG)
    set(_expected_ffmpeg_libdir "${EDITOR_FFMPEG_PREFIX}/lib")
    list(FIND FFMPEG_LIBRARY_DIRS "${_expected_ffmpeg_libdir}" _ffmpeg_libdir_match)
    if(_ffmpeg_libdir_match EQUAL -1)
        message(FATAL_ERROR
            "Resolved FFmpeg libraries are not from the pinned prefix. "
            "Expected ${_expected_ffmpeg_libdir}, got: ${FFMPEG_LIBRARY_DIRS}")
    endif()
endif()

function(jcut_force_bundled_ffmpeg_runtime target_name)
    if(JCUT_USE_SYSTEM_FFMPEG OR NOT TARGET "${target_name}")
        return()
    endif()

    set_target_properties("${target_name}" PROPERTIES
        BUILD_RPATH "${EDITOR_FFMPEG_PREFIX}/lib"
    )
    if(NOT APPLE)
        target_link_options("${target_name}" PRIVATE
            "LINKER:--disable-new-dtags")
    endif()
endfunction()

set(MOTIVE3D_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../motive3d")
set(EDITOR_OPTIONAL_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/external/earcut.hpp/include")
if(EXISTS "${MOTIVE3D_INCLUDE_DIR}")
    list(APPEND EDITOR_OPTIONAL_INCLUDE_DIRS
        "${MOTIVE3D_INCLUDE_DIR}/glm"
        "${MOTIVE3D_INCLUDE_DIR}/tinygltf"
        "${MOTIVE3D_INCLUDE_DIR}/ufbx"
        "${MOTIVE3D_INCLUDE_DIR}"
    )
endif()

set(RTAUDIO_TARGETNAME_UNINSTALL "rtaudio_uninstall" CACHE STRING "" FORCE)
add_subdirectory(rtaudio EXCLUDE_FROM_ALL)

add_library(jcut_video_decode_capabilities_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/video_decode_capabilities_core.cpp
)
set_target_properties(jcut_video_decode_capabilities_core PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_video_decode_capabilities_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(jcut_video_decode_capabilities_core PUBLIC
    __STDC_CONSTANT_MACROS
)
target_link_libraries(jcut_video_decode_capabilities_core PUBLIC
    PkgConfig::FFMPEG
)

add_library(jcut_audio_output_backend STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_output_backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/capabilities_detector.cpp
)
set_target_properties(jcut_audio_output_backend PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_audio_output_backend
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/rtaudio
)
target_link_libraries(jcut_audio_output_backend
    PUBLIC
        jcut_video_decode_capabilities_core
        Threads::Threads
        rtaudio
)
target_compile_definitions(jcut_audio_output_backend
    PUBLIC
        JCUT_HAVE_PIPEWIRE=${JCUT_HAVE_PIPEWIRE}
)
if(TARGET PkgConfig::PIPEWIRE)
    target_link_libraries(jcut_audio_output_backend PRIVATE PkgConfig::PIPEWIRE)
endif()
set(JCUT_SUPABASE_URL "https://ivwutugdrpugjqglxabw.supabase.co" CACHE STRING
    "Default Supabase project URL for desktop auth.")
set(JCUT_SUPABASE_ANON_KEY
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Iml2d3V0dWdkcnB1Z2pxZ2x4YWJ3Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzcwNTM2NTIsImV4cCI6MjA5MjYyOTY1Mn0.P5Z2lvCK4qNtIN5TsvYRqjVfaLsetoT3b7dUklLeMms"
    CACHE STRING "Default Supabase anon key for desktop auth.")
if(NOT DEFINED CPPMONETIZE_DEFAULT_SUPABASE_URL)
    set(CPPMONETIZE_DEFAULT_SUPABASE_URL "${JCUT_SUPABASE_URL}" CACHE STRING
        "Built-in Supabase URL fallback for CPPMonetize.")
endif()
if(NOT DEFINED CPPMONETIZE_DEFAULT_SUPABASE_ANON_KEY)
    set(CPPMONETIZE_DEFAULT_SUPABASE_ANON_KEY "${JCUT_SUPABASE_ANON_KEY}" CACHE STRING
        "Built-in Supabase anon key fallback for CPPMonetize.")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/CPPMonetize/CMakeLists.txt")
    set(CPPMONETIZE_BUILD_TESTS OFF CACHE BOOL
        "Build CPPMonetize tests when CPPMonetize is embedded in JCut." FORCE)
    add_subdirectory(external/CPPMonetize EXCLUDE_FROM_ALL)
    if(EDITOR_BUILD_TESTS AND TARGET cppmonetize_tests AND TARGET cppmonetize_oauth_tests)
        # The subproject registers these tests via include(CTest)/BUILD_TESTING
        # (it ignores CPPMONETIZE_BUILD_TESTS), but the directory is added
        # EXCLUDE_FROM_ALL, so ctest listed binaries that were never built
        # ("Not Run"). Build them with the rest of the suite; both are
        # millisecond network-free unit tests.
        set_target_properties(cppmonetize_tests cppmonetize_oauth_tests
            PROPERTIES EXCLUDE_FROM_ALL FALSE)
    endif()
endif()

set(JCUT_HAVE_NCNN 0)
set(JCUT_NCNN_CAN_WRAP_VULKAN_DEVICE 0)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ncnn/CMakeLists.txt")
    set(JCUT_NCNN_VERSION_PIN "20260512" CACHE STRING
        "Pinned vendored ncnn version to avoid date-based rebuild churn on CMake reconfigure.")
    set(NCNN_VERSION "${JCUT_NCNN_VERSION_PIN}" CACHE STRING "" FORCE)
    set(NCNN_VULKAN ON CACHE BOOL "" FORCE)
    set(NCNN_SHARED_LIB OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
    set(NCNN_INSTALL_SDK OFF CACHE BOOL "" FORCE)
    set(NCNN_OPENMP OFF CACHE BOOL "" FORCE)
    set(NCNN_SIMPLEVK OFF CACHE BOOL "" FORCE)
    set(_saved_automoc "${CMAKE_AUTOMOC}")
    set(_saved_autorcc "${CMAKE_AUTORCC}")
    set(_saved_autouic "${CMAKE_AUTOUIC}")
    set(CMAKE_AUTOMOC OFF)
    set(CMAKE_AUTORCC OFF)
    set(CMAKE_AUTOUIC OFF)
    add_subdirectory(external/ncnn EXCLUDE_FROM_ALL)
    if(TARGET ncnn)
        target_compile_options(ncnn PRIVATE
            $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wno-unknown-pragmas>
            $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wno-unused-parameter>
        )
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/external/ncnn/src/gpu.h" _jcut_ncnn_gpu_h)
        if(_jcut_ncnn_gpu_h MATCHES "VulkanDevice[ \t\r\n]*\\([ \t\r\n]*int[ \t\r\n]+device_index[ \t\r\n]*,[ \t\r\n]*VkDevice")
            set(JCUT_NCNN_CAN_WRAP_VULKAN_DEVICE 1)
        endif()
    endif()
    set(CMAKE_AUTOMOC "${_saved_automoc}")
    set(CMAKE_AUTORCC "${_saved_autorcc}")
    set(CMAKE_AUTOUIC "${_saved_autouic}")
    set(JCUT_HAVE_NCNN 1)
endif()

execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE EDITOR_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT EDITOR_GIT_COMMIT)
    set(EDITOR_GIT_COMMIT "unknown")
endif()

execute_process(
    COMMAND git diff --quiet --ignore-submodules HEAD --
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    RESULT_VARIABLE EDITOR_GIT_DIRTY_RESULT
    ERROR_QUIET
)
if(EDITOR_GIT_DIRTY_RESULT EQUAL 0)
    set(EDITOR_GIT_DIRTY 0)
else()
    set(EDITOR_GIT_DIRTY 1)
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/build_info.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.h
    @ONLY
)

set(EDITOR_QT_UI_SOURCES
    clips_tab.cpp
    control_server.cpp
    control_server_http_utils.cpp
    control_server_media_diag.cpp
    control_server_ui_utils.cpp
    control_server_worker.cpp
    control_server_worker_routes.cpp
    control_server_worker_routes_ui.cpp
    corrections_tab.cpp
    decoder_benchmark_utils.cpp
    direct_vulkan_frame_handoff_pipeline.cpp
    direct_vulkan_preview_audio.cpp
    direct_vulkan_preview_config.cpp
    direct_vulkan_preview_geometry.cpp
    direct_vulkan_preview_host_widget.cpp
    direct_vulkan_preview_interaction.cpp
    direct_vulkan_preview_overlay_rendering.cpp
    direct_vulkan_preview_presenter.cpp
    direct_vulkan_preview_renderer_export.cpp
    direct_vulkan_preview_renderer_recording.cpp
    direct_vulkan_preview_renderer_resources.cpp
    direct_vulkan_preview_transcript.cpp
    direct_vulkan_preview_window.cpp
    editor_ai_helpers.cpp
    editor_birefnet.cpp
    editor_editor_pane.cpp
    editor_facedetections_preview_harness.cpp
    editor_input.cpp
    editor_inspector_bindings.cpp
    editor_media_tools.cpp
    editor_optimization.cpp
    editor_pane.cpp
    editor_playback.cpp
    editor_preview_edit_helpers.cpp
    editor_profiling.cpp
    editor_render_tools.cpp
    editor_runtime_qt_bridge.cpp
    editor_setup.cpp
    editor_tabs.cpp
    editor_title_opacity_keyframe_ops.cpp
    editor_transform_keyframe_ops.cpp
    effects_tab.cpp
    explorer_pane.cpp
    export_vulkan_preview_widget.cpp
    gpu_compositor.cpp
    grading_histogram_widget.cpp
    grading_tab.cpp
    grading_tab_curve.cpp
    history_tab.cpp
    inspector_controller.cpp
    inspector_pane.cpp
    inspector_pane_audio_tab.cpp
    inspector_pane_collections.cpp
    inspector_pane_speakers_tab.cpp
    inspector_pane_secondary_tabs.cpp
    inspector_pane_speakers_continuity_tab.cpp
    inspector_pane_transcript_tab.cpp
    inspector_pane_visual_tabs.cpp
    inspector_pane_wiring.cpp
    keyframe_tab_base.cpp
    keyframe_table_shared.cpp
    mask_fuzzy_remove.cpp
    mask_tab.cpp
    media_drag_payload.cpp
    null_preview_surface.cpp
    opacity_tab.cpp
    output_tab.cpp
    pipeline_tab.cpp
    playback_frame_pipeline.cpp
    preview_renderer_backend.cpp
    preview_speaker_profiles.cpp
    preview_surface_factory.cpp
    preview_widget_factory.cpp
    profile_tab.cpp
    project_state.cpp
    projects.cpp
    properties_tab.cpp
    speaker_document_edit_ops.cpp
    speaker_export_harness.cpp
    speaker_flow_debug.cpp
    speaker_section_selection_timing_service.cpp
    speakers_tab.cpp
    speakers_tab_facedetections_actions.cpp
    speakers_tab_facedetections_engines.cpp
    speakers_tab_facedetections_generation.cpp
    speakers_tab_interactions.cpp
    speakers_tab_interactions_ai.cpp
    speakers_tab_interactions_ai_mining.cpp
    speakers_tab_navigation.cpp
    speakers_tab_reference_preview.cpp
    speakers_tab_wiring.cpp
    speakers_table.cpp
    sync_tab.cpp
    table_tab_base.cpp
    timeline_container.cpp
    timeline_layout.cpp
    timeline_renderer.cpp
    timeline_widget_clipboard.cpp
    timeline_widget_context_menu.cpp
    timeline_widget_core.cpp
    timeline_widget_input.cpp
    timeline_widget_layout.cpp
    timeline_widget_model.cpp
    timeline_widget_paint.cpp
    titles_tab.cpp
    track_sidebar.cpp
    tracks.cpp
    tracks_tab.cpp
    transport_icons.cpp
    video_keyframe_tab.cpp
    vulkan_backend.cpp
    vulkan_preview_surface.cpp
    vulkan_preview_surface_facedetections.cpp
    vulkan_preview_surface_profiling.cpp
)

set(JCUT_VULKAN_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/vulkan_shaders")
set(JCUT_VULKAN_SHADER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/effects.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/effects.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/text.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/text.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/title_mesh.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/title_mesh.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/audio_waveform.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/audio_waveform.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/audio_waveform_process.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/audio_spectrum_loiacono.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/audio_spectrum_goertzel.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/audio_spectrum_fft.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/audio_spectrum_normalize.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/audio_spectrum_history.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/audio_spectrum_loiacono_tile.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/audio_spectrum_goertzel_tile.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/audio_spectrum_fft_tile.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/audio_spectrum_tile_normalize.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/audio_spectrogram_history_normalize.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/mask.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/mask.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/nv12.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/rgba_to_nv12_buffer.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/yuv420p_u.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/yuv420p_v.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/yuv420p.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/nv12_buffer_to_rgba.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/mask_prepare.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/mask_morph.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/mask_blur.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/face_preprocess.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/scrfd_preprocess.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/face_infer_heuristic.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/neutral_composite.comp
)
set(JCUT_VULKAN_SHADER_OUTPUTS "")
if(GLSLANG_VALIDATOR)
    set(JCUT_VULKAN_SHADER_TARGETS "")
    foreach(shader IN LISTS JCUT_VULKAN_SHADER_SOURCES)
        get_filename_component(shader_name ${shader} NAME)
        set(out_file "${JCUT_VULKAN_SHADER_OUTPUT_DIR}/${shader_name}.spv")
        string(MAKE_C_IDENTIFIER "${shader_name}" shader_target_name)
        add_custom_target(jcut_vulkan_shader_${shader_target_name}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${JCUT_VULKAN_SHADER_OUTPUT_DIR}"
            COMMAND ${GLSLANG_VALIDATOR} -V "${shader}" -o "${out_file}"
            BYPRODUCTS "${out_file}"
            DEPENDS "${shader}"
            COMMENT "Compiling Vulkan shader ${shader_name}"
            VERBATIM
        )
        list(APPEND JCUT_VULKAN_SHADER_TARGETS "jcut_vulkan_shader_${shader_target_name}")
        list(APPEND JCUT_VULKAN_SHADER_OUTPUTS "${out_file}")
    endforeach()
    add_custom_target(jcut_vulkan_shaders DEPENDS ${JCUT_VULKAN_SHADER_TARGETS})
else()
    message(WARNING "glslangValidator not found; Vulkan shader SPIR-V build artifacts will not be generated.")
endif()

add_library(jcut_frame_payload_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/frame_payload_core.cpp
)
set_target_properties(jcut_frame_payload_core PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_frame_payload_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(jcut_frame_payload_core PRIVATE PkgConfig::FFMPEG)

add_library(jcut_audio_dynamics_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_dynamics_core.cpp
)
set_target_properties(jcut_audio_dynamics_core PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_audio_dynamics_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

add_library(jcut_frame_handle_qrhi_adapter OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/frame_handle_qrhi_adapter.cpp
)
set_target_properties(jcut_frame_handle_qrhi_adapter PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_frame_handle_qrhi_adapter
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(jcut_frame_handle_qrhi_adapter
    PUBLIC
        jcut_frame_payload_core
        Qt6::Core
        Qt6::Gui
)
if(TARGET Qt6::GuiPrivate)
    target_link_libraries(jcut_frame_handle_qrhi_adapter PRIVATE Qt6::GuiPrivate)
endif()

add_library(jcut_vulkan_frame_import_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_external_frame_import_core.cpp
)
set_target_properties(jcut_vulkan_frame_import_core PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_vulkan_frame_import_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(jcut_vulkan_frame_import_core PUBLIC Vulkan::Vulkan)

add_library(jcut_vulkan_hardware_frame_import_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_hardware_frame_import_core.cpp
)
set_target_properties(jcut_vulkan_hardware_frame_import_core PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_vulkan_hardware_frame_import_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(jcut_vulkan_hardware_frame_import_core
    PUBLIC
        jcut_frame_payload_core
        Vulkan::Vulkan
    PRIVATE
        PkgConfig::FFMPEG
)
if(JCUT_HAS_CUDA_DRIVER)
    target_link_libraries(jcut_vulkan_hardware_frame_import_core
        PRIVATE ${CUDA_DRIVER_LIBRARY})
    target_include_directories(jcut_vulkan_hardware_frame_import_core
        PRIVATE ${CUDA_DRIVER_INCLUDE_DIR})
endif()
target_compile_definitions(jcut_vulkan_hardware_frame_import_core
    PRIVATE
        JCUT_HAS_CUDA_DRIVER=$<IF:$<BOOL:${JCUT_HAS_CUDA_DRIVER}>,1,0>
        JCUT_SKIP_CUDA_EXTERNAL_MEMORY_DESTROY=1
)
if(TARGET jcut_vulkan_shaders)
    add_dependencies(
        jcut_vulkan_hardware_frame_import_core
        jcut_vulkan_shaders)
endif()
target_link_libraries(jcut_vulkan_frame_import_core
    PUBLIC jcut_vulkan_hardware_frame_import_core)

add_library(jcut_transcript_document_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/face_artifact_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/face_processing_job_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/speaker_section_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/speaker_title_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_mining_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_document_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_document_mutation_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_cut_session_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_overlay_core.cpp
)
set_target_properties(jcut_transcript_document_core PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_transcript_document_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann_json/single_include
)
target_link_libraries(jcut_transcript_document_core PUBLIC ZLIB::ZLIB)

add_library(jcut_ai_gateway_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/ai_gateway_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ai_credential_store_core.cpp
)
set_target_properties(jcut_ai_gateway_core PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_ai_gateway_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann_json/single_include
)
target_link_libraries(jcut_ai_gateway_core PUBLIC CURL::libcurl OpenSSL::Crypto)
target_compile_definitions(jcut_ai_gateway_core PRIVATE
    JCUT_DEFAULT_SUPABASE_ANON_KEY="${JCUT_SUPABASE_ANON_KEY}"
)

add_library(jcut_qt_process_tree_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/qt_process_tree.cpp
)
set_target_properties(jcut_qt_process_tree_core PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_qt_process_tree_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(jcut_qt_process_tree_core PUBLIC Qt6::Core)

add_library(editor_core STATIC ${EDITOR_QT_UI_SOURCES})
target_sources(editor_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline_widget.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ui_icons.qrc
)

target_include_directories(editor_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann_json/single_include
        ${CMAKE_CURRENT_SOURCE_DIR}/loiacono
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${EDITOR_OPTIONAL_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/rtaudio
)

target_link_libraries(editor_core
    PUBLIC
        jcut_ai_gateway_core
        jcut_audio_dynamics_core
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        Qt6::Concurrent
        Vulkan::Vulkan
        Threads::Threads
        CURL::libcurl
        PkgConfig::FFMPEG
        PkgConfig::FONT_RENDER
        jcut_audio_output_backend
        rtaudio
        jcut_frame_handle_qrhi_adapter
        jcut_frame_payload_core
        jcut_qt_process_tree_core
        jcut_transcript_document_core
        jcut_vulkan_frame_import_core
)
if(TARGET PkgConfig::RUBBERBAND)
    target_link_libraries(editor_core PUBLIC PkgConfig::RUBBERBAND)
endif()
if(JCUT_HAS_CUDA_DRIVER)
    target_link_libraries(editor_core PUBLIC ${CUDA_DRIVER_LIBRARY})
    target_include_directories(editor_core PUBLIC ${CUDA_DRIVER_INCLUDE_DIR})
    target_compile_definitions(editor_core PUBLIC JCUT_HAS_CUDA_DRIVER=1)
else()
    target_compile_definitions(editor_core PUBLIC JCUT_HAS_CUDA_DRIVER=0)
endif()
target_compile_definitions(editor_core PUBLIC
    JCUT_HAVE_OPENCV=0
    JCUT_HAVE_OPENCV_CONTRIB=0
    JCUT_NCNN_CAN_WRAP_VULKAN_DEVICE=${JCUT_NCNN_CAN_WRAP_VULKAN_DEVICE}
)
if(TARGET ncnn)
    target_link_libraries(editor_core PUBLIC ncnn)
endif()

jcut_force_bundled_ffmpeg_runtime(editor_core)

if(TARGET Qt6::GuiPrivate)
    target_link_libraries(editor_core PUBLIC Qt6::GuiPrivate)
endif()

target_compile_definitions(editor_core
    PUBLIC
        QT_DEPRECATED_WARNINGS
        JCUT_HAVE_NCNN=${JCUT_HAVE_NCNN}
        JCUT_HAVE_RUBBERBAND=${JCUT_HAVE_RUBBERBAND}
    PRIVATE
        JCUT_VULKAN_SHADER_DIR="${JCUT_VULKAN_SHADER_OUTPUT_DIR}"
)
if(TARGET jcut_vulkan_shaders)
    add_dependencies(editor_core jcut_vulkan_shaders)
endif()

add_library(jcut_media_path_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/image_sequence_directory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/proxy_path_core.cpp
)
set_target_properties(jcut_media_path_core PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_media_path_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(editor_core PUBLIC jcut_media_path_core)

add_library(jcut_decoder_policy_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/decoder_policy_core.cpp
)
set_target_properties(jcut_decoder_policy_core PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_decoder_policy_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(editor_core PUBLIC jcut_decoder_policy_core)

add_library(jcut_ffmpeg_hardware_device_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg_hardware_device_core.cpp
)
set_target_properties(jcut_ffmpeg_hardware_device_core PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_ffmpeg_hardware_device_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(jcut_ffmpeg_hardware_device_core
    PUBLIC
        jcut_decoder_policy_core
        jcut_video_decode_capabilities_core
        PkgConfig::FFMPEG
)
target_link_libraries(editor_core PUBLIC jcut_ffmpeg_hardware_device_core)

add_library(jcut_audio_time_stretch_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_time_stretch_core.cpp
)
set_target_properties(jcut_audio_time_stretch_core PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_audio_time_stretch_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(jcut_audio_time_stretch_core PUBLIC
    JCUT_HAVE_RUBBERBAND=${JCUT_HAVE_RUBBERBAND}
)
if(TARGET PkgConfig::RUBBERBAND)
    target_link_libraries(jcut_audio_time_stretch_core PUBLIC PkgConfig::RUBBERBAND)
endif()
target_link_libraries(editor_core PUBLIC jcut_audio_time_stretch_core)

add_library(jcut_waveform_envelope_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/waveform_envelope_core.cpp
)
set_target_properties(jcut_waveform_envelope_core PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_waveform_envelope_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(editor_core PUBLIC jcut_waveform_envelope_core)

add_library(jcut_mask_frame_map_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/birefnet_job_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mask_frame_map_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mask_sidecar_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/process_job_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/prompt_mask_job_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcription_job_core.cpp
)
set_target_properties(jcut_mask_frame_map_core PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_mask_frame_map_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann_json/single_include
)
target_link_libraries(jcut_mask_frame_map_core PUBLIC PkgConfig::FFMPEG)
target_link_libraries(editor_core PUBLIC jcut_mask_frame_map_core)

# Keep preview/decode QObject metadata out of editor_test_support's widget-facing
# AUTOMOC unit. Pulling one aggregate MOC object into jcut_imgui would otherwise
# also pull ProjectManager and TranscriptTab, and with them Qt6::Widgets.
add_library(jcut_preview_decode_runtime OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/async_decoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/async_decoder.h
    ${CMAKE_CURRENT_SOURCE_DIR}/memory_budget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/memory_budget.h
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline_cache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline_cache.h
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline_cache_requests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline_cache_seek_resync.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline_cache_seek_resync.h
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline_cache_storage.cpp
)
set_target_properties(jcut_preview_decode_runtime PROPERTIES
    AUTOMOC ON
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_preview_decode_runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann_json/single_include
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${EDITOR_OPTIONAL_INCLUDE_DIRS}
)
target_link_libraries(jcut_preview_decode_runtime
    PUBLIC
        jcut_frame_payload_core
        Qt6::Core
        Qt6::Gui
        Threads::Threads
        PkgConfig::FFMPEG
)
target_compile_definitions(jcut_preview_decode_runtime
    PUBLIC
        QT_DEPRECATED_WARNINGS
        JCUT_HAVE_OPENCV=0
        JCUT_HAVE_OPENCV_CONTRIB=0
        JCUT_HAVE_RUBBERBAND=${JCUT_HAVE_RUBBERBAND}
)

set(JCUT_RUNTIME_SUPPORT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_time_stretch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_time_stretch_cache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/auto_sync_plan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/decode_trace.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/decoder_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/decoder_ffmpeg_utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/decoder_image_io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/debug_controls.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_shared.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_effect_presets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_shared_effects.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_shared_keyframes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_shared_keyframes_cache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_shared_media.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_shared_render_sync.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_shared_timing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_shared_transcript.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_shared_transcript_overlay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/frame_handle.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/facedetections_assignment_services.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/facedetections_continuity_artifacts.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/facedetections_tracking.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/facedetections_time_mapping.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/facestream_overlay_snapshot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/identity_resolution.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/media_pipeline_shared.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mask_sidecar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/playback_clock_coordinator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/processing_job_docker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/processing_job_manifest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview_face_track_click_policy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview_frame_selection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview_view_transform.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/speaker_track_assignment_service.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/track_avatar_utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline_clip_title.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_document_edit_service.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_document_io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_runtime_cache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/clip_serialization.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/overlay_text_style.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_engine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transform_skip_aware_timing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/waveform_service.cpp
)

set(EDITOR_WIDGET_TEST_SUPPORT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/action_profiler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/detector_settings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/facefind_window.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview_view_transform_qt_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/project_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/startup_project_state.cpp
    # These future-based save/session helpers are currently consumed only by
    # the widget transcript surfaces; keep Qt Concurrent out of ImGui runtime support.
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_document_save_controller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_document_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_tab.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_tab_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_tab_overlay.cpp
)

add_library(jcut_image_decode_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/core/image_file_decoder.cpp
)
set_target_properties(jcut_image_decode_core PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_image_decode_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(editor_core PRIVATE jcut_image_decode_core)

add_library(jcut_runtime_support STATIC ${JCUT_RUNTIME_SUPPORT_SOURCES})

target_include_directories(jcut_runtime_support
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann_json/single_include
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${EDITOR_OPTIONAL_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/rtaudio
)
jcut_use_qt_widgets_headers_only(jcut_runtime_support)

target_link_libraries(jcut_runtime_support
    PRIVATE
        jcut_audio_dynamics_core
        jcut_transcript_document_core
        jcut_preview_decode_runtime
        jcut_image_decode_core
    PUBLIC
        jcut_decoder_policy_core
        jcut_ffmpeg_hardware_device_core
        jcut_video_decode_capabilities_core
        jcut_frame_payload_core
        jcut_media_path_core
        jcut_audio_time_stretch_core
        jcut_mask_frame_map_core
        Qt6::Core
        Qt6::Gui
        Threads::Threads
        CURL::libcurl
        PkgConfig::FFMPEG
        PkgConfig::FONT_RENDER
        jcut_audio_output_backend
        rtaudio
)
if(TARGET PkgConfig::RUBBERBAND)
    target_link_libraries(jcut_runtime_support PUBLIC PkgConfig::RUBBERBAND)
endif()

if(TARGET apinames)
    add_dependencies(jcut_runtime_support apinames)
endif()

target_compile_definitions(jcut_runtime_support
    PUBLIC
        QT_DEPRECATED_WARNINGS
        JCUT_HAVE_OPENCV=0
        JCUT_HAVE_OPENCV_CONTRIB=0
        JCUT_HAVE_RUBBERBAND=${JCUT_HAVE_RUBBERBAND}
)

jcut_force_bundled_ffmpeg_runtime(jcut_runtime_support)

add_library(editor_test_support STATIC ${EDITOR_WIDGET_TEST_SUPPORT_SOURCES})
target_include_directories(editor_test_support
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann_json/single_include
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${EDITOR_OPTIONAL_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/rtaudio
)
target_link_libraries(editor_test_support
    PRIVATE
        jcut_transcript_document_core
        Qt6::Concurrent
    PUBLIC
        jcut_runtime_support
        jcut_frame_handle_qrhi_adapter
        Qt6::Widgets
)

jcut_force_bundled_ffmpeg_runtime(editor_test_support)

add_library(jcut_imgui_preview_support STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui_preview_window.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/backends/imgui_impl_glfw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/backends/imgui_impl_vulkan.cpp
)
target_include_directories(jcut_imgui_preview_support
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/backends
        ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(jcut_imgui_preview_support
    PUBLIC
        editor_test_support
        Vulkan::Vulkan
        PkgConfig::GLFW
)

add_library(jcut_editor_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_document_core_json.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui_project_io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_contract_json.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime_control_server.cpp
)
set_target_properties(jcut_editor_core PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_editor_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann_json/single_include
)
target_link_libraries(jcut_editor_core
    PUBLIC
        jcut_ai_gateway_core
        jcut_audio_dynamics_core
        jcut_decoder_policy_core
        jcut_transcript_document_core
        jcut_media_path_core
        Threads::Threads
)

add_library(jcut_editor_runtime STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_document_render_bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/cpu_overlay_render_backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_audio.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_engine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_engine_cache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_engine_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_engine_mix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_engine_profiling.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_speech_harmonic_isolator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_codecs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_export.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_gpu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_qt_compat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_stats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_vulkan_shared.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_effect_synth.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/offscreen_vulkan_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/offscreen_vulkan_renderer_backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/offscreen_vulkan_renderer_helpers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_detector_frame_handoff.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_clear_helpers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_mask_preprocessor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_resources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_overlay_cache_key.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/title_mesh_extrusion.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_text_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/titles.cpp
)
target_include_directories(jcut_editor_runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann_json/single_include
)
jcut_use_qt_widgets_headers_only(jcut_editor_runtime)
target_link_libraries(jcut_editor_runtime
    PUBLIC
        jcut_editor_core
        jcut_runtime_support
        jcut_waveform_envelope_core
        Vulkan::Vulkan
    PRIVATE
        jcut_vulkan_frame_import_core
        jcut_vulkan_hardware_frame_import_core
)
if(JCUT_HAS_CUDA_DRIVER)
    target_link_libraries(jcut_editor_runtime PUBLIC ${CUDA_DRIVER_LIBRARY})
    target_include_directories(jcut_editor_runtime PUBLIC ${CUDA_DRIVER_INCLUDE_DIR})
endif()
target_compile_definitions(jcut_editor_runtime
    PUBLIC
        JCUT_HAS_CUDA_DRIVER=$<IF:$<BOOL:${JCUT_HAS_CUDA_DRIVER}>,1,0>
)
if(TARGET jcut_vulkan_shaders)
    add_dependencies(jcut_editor_runtime jcut_vulkan_shaders)
endif()

add_library(jcut_loiacono_runtime STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/loiacono/loiacono_rolling.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/loiacono/loiacono_parallel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/loiacono/loiacono_vulkan_compute.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/loiacono/spectrum_settings_dialog.cpp
)
target_include_directories(jcut_loiacono_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/loiacono
)
target_link_libraries(jcut_loiacono_runtime PUBLIC Qt6::Widgets Vulkan::Vulkan)

add_library(jcut_face_preview_runtime STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_preview_support.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/facedetections_generation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/facedetections_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_audio_tab.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_pipeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_zero_copy_face_detector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_res10_ncnn_face_detector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_scrfd_ncnn_face_detector.cpp
)
target_include_directories(jcut_face_preview_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(jcut_face_preview_runtime PUBLIC
    editor_test_support
    jcut_editor_runtime
    jcut_loiacono_runtime
    Vulkan::Vulkan
)
target_compile_definitions(jcut_face_preview_runtime PUBLIC
    JCUT_HAVE_NCNN=${JCUT_HAVE_NCNN}
    JCUT_HAVE_OPENCV=0
    JCUT_HAVE_OPENCV_CONTRIB=0
    JCUT_NCNN_CAN_WRAP_VULKAN_DEVICE=${JCUT_NCNN_CAN_WRAP_VULKAN_DEVICE}
)
if(JCUT_HAS_CUDA_DRIVER)
    target_link_libraries(jcut_face_preview_runtime PUBLIC ${CUDA_DRIVER_LIBRARY})
    target_include_directories(jcut_face_preview_runtime PUBLIC ${CUDA_DRIVER_INCLUDE_DIR})
    target_compile_definitions(jcut_face_preview_runtime PUBLIC JCUT_HAS_CUDA_DRIVER=1)
else()
    target_compile_definitions(jcut_face_preview_runtime PUBLIC JCUT_HAS_CUDA_DRIVER=0)
endif()
if(TARGET ncnn)
    target_link_libraries(jcut_face_preview_runtime PUBLIC ncnn)
endif()
if(TARGET jcut_vulkan_shaders)
    add_dependencies(jcut_face_preview_runtime jcut_vulkan_shaders)
endif()

# The Qt frontend owns only widget/application sources. Shared domain, decode,
# render, and detector implementations are compiled once by their libraries.
target_link_libraries(editor_core PUBLIC
    editor_test_support
    jcut_editor_core
    jcut_editor_runtime
    jcut_preview_decode_runtime
    jcut_face_preview_runtime
    jcut_loiacono_runtime
)
target_link_libraries(jcut_imgui_preview_support PUBLIC jcut_face_preview_runtime)

add_library(jcut_imgui_vulkan_import STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui_vulkan_frame_importer.cpp
)
set_target_properties(jcut_imgui_vulkan_import PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_imgui_vulkan_import
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(jcut_imgui_vulkan_import
    PUBLIC
        jcut_vulkan_frame_import_core
        Vulkan::Vulkan
)

add_library(jcut_imgui_standalone_runtime STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/proxy_generation_job_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/standalone_audio_mixer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/standalone_export_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/standalone_timeline_renderer.cpp
)
set_target_properties(jcut_imgui_standalone_runtime PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_imgui_standalone_runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann_json/single_include
)
target_link_libraries(jcut_imgui_standalone_runtime
    PUBLIC
        jcut_decoder_policy_core
        jcut_ffmpeg_hardware_device_core
        jcut_video_decode_capabilities_core
        jcut_frame_payload_core
        jcut_editor_core
        jcut_media_path_core
        jcut_audio_time_stretch_core
        jcut_mask_frame_map_core
        PkgConfig::FFMPEG
        PkgConfig::FONT_RENDER
        Threads::Threads
)

add_library(jcut_imgui_audio_runtime STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui_audio_runtime.cpp
)
set_target_properties(jcut_imgui_audio_runtime PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_imgui_audio_runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann_json/single_include
)
target_link_libraries(jcut_imgui_audio_runtime
    PUBLIC
        jcut_imgui_standalone_runtime
        jcut_waveform_envelope_core
        rtaudio
        Threads::Threads
)

add_library(jcut_imgui_runtime STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/standalone_preview_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui_gpu_renderer_bridge.cpp
)
set_target_properties(jcut_imgui_runtime PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
target_include_directories(jcut_imgui_runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann_json/single_include
)
target_link_libraries(jcut_imgui_runtime
    PUBLIC
        jcut_imgui_standalone_runtime
        jcut_imgui_audio_runtime
        jcut_imgui_vulkan_import
        jcut_vulkan_hardware_frame_import_core
        Vulkan::Vulkan
        ${CMAKE_DL_LIBS}
)

add_library(jcut_imgui_gpu_renderer SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui_gpu_renderer_plugin.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_compositor_core.cpp
)
target_include_directories(jcut_imgui_gpu_renderer
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann_json/single_include
)
target_link_libraries(jcut_imgui_gpu_renderer
    PRIVATE
        jcut_imgui_standalone_runtime
        Vulkan::Vulkan
)
target_compile_definitions(jcut_imgui_gpu_renderer PRIVATE
    JCUT_VULKAN_SHADER_DIR="${JCUT_VULKAN_SHADER_OUTPUT_DIR}"
)
if(TARGET jcut_vulkan_shaders)
    add_dependencies(
        jcut_imgui_gpu_renderer
        jcut_vulkan_shaders)
endif()
set_target_properties(jcut_imgui_gpu_renderer PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
    OUTPUT_NAME "jcut_imgui_gpu_renderer"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

add_library(jcut_qt_application STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/editor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_ai_integration.cpp
)
target_link_libraries(jcut_qt_application PRIVATE "$<LINK_GROUP:RESCAN,editor_core>")
target_link_libraries(jcut_qt_application PRIVATE jcut_imgui_preview_support)
if(TARGET CPPMonetize::cppmonetize)
    target_link_libraries(jcut_qt_application PRIVATE CPPMonetize::cppmonetize)
endif()
add_executable(editor
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_entry.cpp
)
target_link_libraries(editor PRIVATE jcut_qt_application)
set_target_properties(editor PROPERTIES
    OUTPUT_NAME "jcut"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
jcut_force_bundled_ffmpeg_runtime(editor)

if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)
    set_target_properties(editor PROPERTIES
        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    )
    install(TARGETS editor
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
    if(JCUT_VULKAN_SHADER_OUTPUTS)
        install(FILES ${JCUT_VULKAN_SHADER_OUTPUTS}
            DESTINATION "${CMAKE_INSTALL_DATADIR}/jcut/shaders"
        )
    endif()
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/io.github.jcut.JCut.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
    )
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/io.github.jcut.JCut.svg"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
    )
endif()

add_library(jcut_imgui_application STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/jcut_imgui_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/misc/cpp/imgui_stdlib.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/backends/imgui_impl_vulkan.cpp
)
target_include_directories(jcut_imgui_application PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/backends
    ${X11_INCLUDE_DIR}
)
target_link_libraries(jcut_imgui_application PRIVATE
    jcut_imgui_runtime
    Vulkan::Vulkan
    ${X11_LIBRARIES}
)
target_compile_definitions(jcut_imgui_application PRIVATE
    JCUT_VULKAN_SHADER_DIR="${JCUT_VULKAN_SHADER_OUTPUT_DIR}"
)
set_target_properties(jcut_imgui_application PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)
add_executable(jcut_imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/jcut_imgui_entry.cpp
)
target_link_libraries(jcut_imgui PRIVATE jcut_imgui_application)
set_target_properties(jcut_imgui PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
add_dependencies(jcut_imgui jcut_imgui_gpu_renderer)
add_custom_command(TARGET jcut_imgui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:jcut_imgui>/assets/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/misc/fonts/Roboto-Medium.ttf"
        "$<TARGET_FILE_DIR:jcut_imgui>/assets/fonts/Roboto-Medium.ttf"
)
jcut_force_bundled_ffmpeg_runtime(jcut_imgui)

add_executable(project_roundtrip_cli
    ${CMAKE_CURRENT_SOURCE_DIR}/project_roundtrip_cli.cpp
)
target_include_directories(project_roundtrip_cli
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann_json/single_include
)
target_link_libraries(project_roundtrip_cli
    PRIVATE
        editor_test_support
        Qt6::Core
        Qt6::Widgets
)

add_executable(jcut_migrate_facedetections_artifacts
    ${CMAKE_CURRENT_SOURCE_DIR}/migrate_facedetections_artifacts_main.cpp
)
target_link_libraries(jcut_migrate_facedetections_artifacts
    PRIVATE
        editor_test_support
)
set_target_properties(jcut_migrate_facedetections_artifacts PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
jcut_force_bundled_ffmpeg_runtime(jcut_migrate_facedetections_artifacts)

if(APPLE)
    set_target_properties(editor PROPERTIES MACOSX_BUNDLE TRUE)
endif()

add_executable(jcut_vulkan_zero_copy_video_preprocess
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_zero_copy_video_preprocess_main.cpp
)
target_include_directories(jcut_vulkan_zero_copy_video_preprocess PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann_json/single_include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(jcut_vulkan_zero_copy_video_preprocess PRIVATE
    jcut_face_preview_runtime
    Vulkan::Vulkan
)
if(NOT JCUT_USE_SYSTEM_FFMPEG)
    if(APPLE)
        target_link_libraries(jcut_vulkan_zero_copy_video_preprocess PRIVATE
            "${EDITOR_FFMPEG_PREFIX}/lib/libswresample.dylib")
    else()
        target_link_libraries(jcut_vulkan_zero_copy_video_preprocess PRIVATE
            "${EDITOR_FFMPEG_PREFIX}/lib/libswresample.so")
        target_link_options(jcut_vulkan_zero_copy_video_preprocess PRIVATE
            "LINKER:--no-as-needed")
    endif()
endif()
set_target_properties(jcut_vulkan_zero_copy_video_preprocess PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(TARGET jcut_vulkan_shaders)
    add_dependencies(jcut_vulkan_zero_copy_video_preprocess jcut_vulkan_shaders)
endif()
jcut_force_bundled_ffmpeg_runtime(jcut_vulkan_zero_copy_video_preprocess)

add_executable(jcut_vulkan_facedetections_offscreen
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_facedetections_offscreen_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_facedetections_offscreen_artifact_io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_facedetections_offscreen_benchmark.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_facedetections_offscreen_checkpoint_writer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_facedetections_offscreen_control_panel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_facedetections_offscreen_detection_filters.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_facedetections_offscreen_opencv_dnn.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_facedetections_offscreen_options.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_facedetections_offscreen_preview_io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_facedetections_offscreen_progress.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_facedetections_offscreen_resume_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_facedetections_offscreen_runner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_facedetections_offscreen_tuning.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_facedetections_offscreen_vulkan_harness.cpp
)
target_include_directories(jcut_vulkan_facedetections_offscreen PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann_json/single_include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(jcut_vulkan_facedetections_offscreen PRIVATE
    editor_test_support
    jcut_imgui_preview_support
    jcut_face_preview_runtime
    jcut_editor_runtime
    jcut_vulkan_frame_import_core
    Qt6::Network
    Vulkan::Vulkan
    PkgConfig::FONT_RENDER
    PkgConfig::GLFW
)
if(JCUT_HAS_CUDA_DRIVER)
    target_link_libraries(jcut_vulkan_facedetections_offscreen PRIVATE ${CUDA_DRIVER_LIBRARY})
    target_include_directories(jcut_vulkan_facedetections_offscreen PRIVATE ${CUDA_DRIVER_INCLUDE_DIR})
    target_compile_definitions(jcut_vulkan_facedetections_offscreen PRIVATE JCUT_HAS_CUDA_DRIVER=1)
else()
    target_compile_definitions(jcut_vulkan_facedetections_offscreen PRIVATE JCUT_HAS_CUDA_DRIVER=0)
endif()
if(TARGET ncnn)
    target_link_libraries(jcut_vulkan_facedetections_offscreen PRIVATE ncnn)
endif()
if(NOT JCUT_USE_SYSTEM_FFMPEG)
    if(APPLE)
        target_link_libraries(jcut_vulkan_facedetections_offscreen PRIVATE
            "${EDITOR_FFMPEG_PREFIX}/lib/libswresample.dylib")
    else()
        target_link_libraries(jcut_vulkan_facedetections_offscreen PRIVATE
            "${EDITOR_FFMPEG_PREFIX}/lib/libswresample.so")
        target_link_options(jcut_vulkan_facedetections_offscreen PRIVATE
            "LINKER:--no-as-needed")
    endif()
endif()
target_compile_definitions(jcut_vulkan_facedetections_offscreen PRIVATE
    JCUT_FACESTREAM_OFFSCREEN_STANDALONE=1
    JCUT_HAVE_OPENCV_CONTRIB=0
    JCUT_HAVE_NCNN=${JCUT_HAVE_NCNN}
    JCUT_HAVE_OPENCV=0
    JCUT_NCNN_CAN_WRAP_VULKAN_DEVICE=${JCUT_NCNN_CAN_WRAP_VULKAN_DEVICE}
    JCUT_SKIP_CUDA_EXTERNAL_MEMORY_DESTROY=1
)
set_target_properties(jcut_vulkan_facedetections_offscreen PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(TARGET jcut_vulkan_shaders)
    add_dependencies(jcut_vulkan_facedetections_offscreen jcut_vulkan_shaders)
endif()
jcut_force_bundled_ffmpeg_runtime(jcut_vulkan_facedetections_offscreen)

if(EDITOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
