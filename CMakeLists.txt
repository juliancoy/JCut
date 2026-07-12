cmake_minimum_required(VERSION 3.24)

project(editor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(EDITOR_ASAN "Enable AddressSanitizer" OFF)
option(EDITOR_BUILD_TESTS "Build editor tests" ON)
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
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
find_package(X11 REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Concurrent Test
             OPTIONAL_COMPONENTS CorePrivate GuiPrivate)
get_target_property(JCUT_QT_NETWORK_DISABLED_FEATURES Qt6::Network QT_DISABLED_PUBLIC_FEATURES)
if(JCUT_QT_NETWORK_DISABLED_FEATURES MATCHES "(^|;)ssl(;|$)")
    message(FATAL_ERROR
        "The selected Qt6::Network was built without SSL support, so JCut cannot complete "
        "Supabase HTTPS login. Use a Qt build with the OpenSSL TLS backend enabled and "
        "deploy its matching tls/libqopensslbackend.so plugin. Current Qt6_DIR: ${Qt6_DIR}")
endif()
find_program(GLSLANG_VALIDATOR glslangValidator)
find_library(CUDA_DRIVER_LIBRARY cuda)

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
set(EDITOR_OPTIONAL_INCLUDE_DIRS "")
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

file(GLOB EDITOR_ALL_CPP CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

set(EDITOR_EXCLUDED_REGEX
    ".*/playback_controller\\.cpp$"
    ".*/professional_example\\.cpp$"
    ".*/test[0-9]*\\.cpp$"
    ".*/imgui_preview_window\\.cpp$"
    ".*/jcut_imgui_main\\.cpp$"
    # 3D model import/runtime sources depend on motive3d engine files that are not
    # part of this repository build graph (eg. primitive/texture engine runtime).
    # Exclude them from the editor target so standard builds remain reproducible.
    ".*/animation\\.cpp$"
    ".*/fbx_import\\.cpp$"
    ".*/gltf_import\\.cpp$"
    ".*/model\\.cpp$"
    ".*/model_import_shared\\.cpp$"
    ".*/model_runtime\\.cpp$"
    ".*/backend_headless_compare_main\\.cpp$"
    ".*/vulkan_facedetections_offscreen_main\\.cpp$"
    ".*/vulkan_facedetections_preview_main\\.cpp$"
    ".*/vulkan_facedetections_test_ui_main\\.cpp$"
    ".*/migrate_facedetections_artifacts_main\\.cpp$"
    ".*/vulkan_zero_copy_video_preprocess_main\\.cpp$"
)

set(EDITOR_APP_SOURCES "${EDITOR_ALL_CPP}")
foreach(regex IN LISTS EDITOR_EXCLUDED_REGEX)
    list(FILTER EDITOR_APP_SOURCES EXCLUDE REGEX "${regex}")
endforeach()

set(EDITOR_CORE_SOURCES "${EDITOR_APP_SOURCES}")
list(FILTER EDITOR_CORE_SOURCES EXCLUDE REGEX ".*/editor\\.cpp$")
list(FILTER EDITOR_CORE_SOURCES EXCLUDE REGEX ".*/editor_main\\.cpp$")
list(FILTER EDITOR_CORE_SOURCES EXCLUDE REGEX ".*/editor_ai_integration\\.cpp$")

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
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/nv12_y.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/nv12_uv.frag
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

add_library(editor_core STATIC ${EDITOR_CORE_SOURCES})
target_sources(editor_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline_widget.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ui_icons.qrc
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_overlay_cache_key.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/loiacono/loiacono_rolling.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/loiacono/loiacono_parallel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/loiacono/loiacono_vulkan_compute.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/loiacono/spectrum_settings_dialog.cpp
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
        rtaudio
)
if(TARGET PkgConfig::RUBBERBAND)
    target_link_libraries(editor_core PUBLIC PkgConfig::RUBBERBAND)
endif()
if(CUDA_DRIVER_LIBRARY)
    target_link_libraries(editor_core PUBLIC ${CUDA_DRIVER_LIBRARY})
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
        JCUT_VULKAN_SHADER_DIR="${JCUT_VULKAN_SHADER_OUTPUT_DIR}"
        JCUT_HAVE_NCNN=${JCUT_HAVE_NCNN}
        JCUT_HAVE_RUBBERBAND=${JCUT_HAVE_RUBBERBAND}
)
if(TARGET jcut_vulkan_shaders)
    add_dependencies(editor_core jcut_vulkan_shaders)
endif()

set(EDITOR_TEST_SUPPORT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_time_stretch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_time_stretch_cache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/async_decoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/auto_sync_plan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/decode_trace.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/detector_settings.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/facefind_window.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/frame_handle.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/facedetections_assignment_services.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/facedetections_continuity_artifacts.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/facedetections_tracking.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/facedetections_time_mapping.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/facestream_overlay_snapshot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/identity_resolution.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/media_pipeline_shared.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/memory_budget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/playback_clock_coordinator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/processing_job_docker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/processing_job_manifest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview_face_track_click_policy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview_frame_selection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview_view_transform.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/project_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/speaker_track_assignment_service.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/startup_project_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/track_avatar_utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline_cache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline_cache_requests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline_cache_seek_resync.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline_cache_storage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_document_edit_service.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_document_io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_document_save_controller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_document_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_runtime_cache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_tab.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_tab_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_tab_overlay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/clip_serialization.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/overlay_text_style.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_engine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transform_skip_aware_timing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/waveform_service.cpp
)

add_library(editor_test_support STATIC ${EDITOR_TEST_SUPPORT_SOURCES})
target_sources(editor_test_support PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/async_decoder.h
    ${CMAKE_CURRENT_SOURCE_DIR}/memory_budget.h
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline_cache.h
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline_cache_seek_resync.h
)

target_include_directories(editor_test_support
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann_json/single_include
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${EDITOR_OPTIONAL_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/rtaudio
)

target_link_libraries(editor_test_support
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        Qt6::Concurrent
        Threads::Threads
        CURL::libcurl
        PkgConfig::FFMPEG
        PkgConfig::FONT_RENDER
        rtaudio
)
if(TARGET PkgConfig::RUBBERBAND)
    target_link_libraries(editor_test_support PUBLIC PkgConfig::RUBBERBAND)
endif()
if(TARGET Qt6::GuiPrivate)
    target_link_libraries(editor_test_support PUBLIC Qt6::GuiPrivate)
endif()

if(TARGET apinames)
    add_dependencies(editor_test_support apinames)
endif()

target_compile_definitions(editor_test_support
    PUBLIC
        QT_DEPRECATED_WARNINGS
        JCUT_HAVE_OPENCV=0
        JCUT_HAVE_OPENCV_CONTRIB=0
        JCUT_HAVE_RUBBERBAND=${JCUT_HAVE_RUBBERBAND}
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

add_library(jcut_editor_runtime STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_document_core_json.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_document_render_bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui_project_io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_contract_json.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime_control_server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/cpu_overlay_render_backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_audio.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_engine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_speech_harmonic_isolator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mask_sidecar.cpp
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
target_link_libraries(jcut_editor_runtime
    PUBLIC
        editor_test_support
        Vulkan::Vulkan
)
if(CUDA_DRIVER_LIBRARY)
    target_link_libraries(jcut_editor_runtime PUBLIC ${CUDA_DRIVER_LIBRARY})
endif()
target_compile_definitions(jcut_editor_runtime
    PUBLIC
        JCUT_VULKAN_SHADER_DIR="${JCUT_VULKAN_SHADER_OUTPUT_DIR}"
        JCUT_HAS_CUDA_DRIVER=$<IF:$<BOOL:${CUDA_DRIVER_LIBRARY}>,1,0>
)
if(TARGET jcut_vulkan_shaders)
    add_dependencies(jcut_editor_runtime jcut_vulkan_shaders)
endif()

add_library(jcut_imgui_runtime STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/standalone_export_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/standalone_preview_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/standalone_timeline_renderer.cpp
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
        jcut_editor_runtime
        PkgConfig::FFMPEG
        Threads::Threads
)

add_executable(editor
    ${CMAKE_CURRENT_SOURCE_DIR}/editor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_ai_integration.cpp
)
target_link_libraries(editor PRIVATE editor_core)
if(TARGET CPPMonetize::cppmonetize)
    target_link_libraries(editor PRIVATE CPPMonetize::cppmonetize)
endif()
set_target_properties(editor PROPERTIES
    OUTPUT_NAME "jcut"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
jcut_force_bundled_ffmpeg_runtime(editor)

add_executable(jcut_imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/jcut_imgui_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/backends/imgui_impl_vulkan.cpp
)
target_include_directories(jcut_imgui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/backends
    ${X11_INCLUDE_DIR}
)
target_link_libraries(jcut_imgui PRIVATE
    jcut_imgui_runtime
    Vulkan::Vulkan
    ${X11_LIBRARIES}
)
set_target_properties(jcut_imgui PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
add_custom_command(TARGET jcut_imgui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:jcut_imgui>/assets/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/misc/fonts/Roboto-Medium.ttf"
        "$<TARGET_FILE_DIR:jcut_imgui>/assets/fonts/Roboto-Medium.ttf"
)
jcut_force_bundled_ffmpeg_runtime(jcut_imgui)

add_executable(project_roundtrip_cli
    ${CMAKE_CURRENT_SOURCE_DIR}/project_roundtrip_cli.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/project_manager.cpp
)
target_include_directories(project_roundtrip_cli
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann_json/single_include
)
target_link_libraries(project_roundtrip_cli
    PRIVATE
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
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_zero_copy_face_detector.cpp
)
target_include_directories(jcut_vulkan_zero_copy_video_preprocess PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann_json/single_include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(jcut_vulkan_zero_copy_video_preprocess PRIVATE
    editor_test_support
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
target_compile_definitions(jcut_vulkan_zero_copy_video_preprocess PRIVATE
    JCUT_VULKAN_SHADER_DIR="${JCUT_VULKAN_SHADER_OUTPUT_DIR}"
)
set_target_properties(jcut_vulkan_zero_copy_video_preprocess PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(TARGET jcut_vulkan_shaders)
    add_dependencies(jcut_vulkan_zero_copy_video_preprocess jcut_vulkan_shaders)
endif()
jcut_force_bundled_ffmpeg_runtime(jcut_vulkan_zero_copy_video_preprocess)

add_executable(jcut_vulkan_facedetections_offscreen
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_preview_support.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_document_edit_service.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_document_io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_document_save_controller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_document_session.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/facedetections_generation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/facedetections_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/offscreen_vulkan_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/offscreen_vulkan_renderer_backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/offscreen_vulkan_renderer_helpers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview_view_transform.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/cpu_overlay_render_backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_stats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_vulkan_shared.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_effect_synth.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/titles.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_audio_tab.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_detector_frame_handoff.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_pipeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_clear_helpers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_resources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_overlay_cache_key.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/title_mesh_extrusion.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_text_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_zero_copy_face_detector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_res10_ncnn_face_detector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_scrfd_ncnn_face_detector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/waveform_service.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/loiacono/loiacono_parallel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/loiacono/loiacono_rolling.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/loiacono/loiacono_vulkan_compute.cpp
)
target_include_directories(jcut_vulkan_facedetections_offscreen PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann_json/single_include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(jcut_vulkan_facedetections_offscreen PRIVATE
    editor_test_support
    jcut_imgui_preview_support
    Vulkan::Vulkan
    PkgConfig::FONT_RENDER
    PkgConfig::GLFW
)
if(CUDA_DRIVER_LIBRARY)
    target_link_libraries(jcut_vulkan_facedetections_offscreen PRIVATE ${CUDA_DRIVER_LIBRARY})
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
    JCUT_VULKAN_SHADER_DIR="${JCUT_VULKAN_SHADER_OUTPUT_DIR}"
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
