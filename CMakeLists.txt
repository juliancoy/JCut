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

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(EDITOR_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

find_package(PkgConfig REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Concurrent OpenGLWidgets Test)

set(EDITOR_FFMPEG_PREFIX
    "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg-install"
    CACHE PATH "Pinned FFmpeg install prefix used by this project")
set(EDITOR_FFMPEG_PKGCONFIG_DIR "${EDITOR_FFMPEG_PREFIX}/lib/pkgconfig")

if(NOT EXISTS "${EDITOR_FFMPEG_PKGCONFIG_DIR}/libavcodec.pc")
    message(FATAL_ERROR
        "Bundled FFmpeg pkg-config files not found at ${EDITOR_FFMPEG_PKGCONFIG_DIR}. "
        "Run ./build.sh to bootstrap the pinned FFmpeg build.")
endif()

set(_saved_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
if(_saved_pkg_config_path)
    set(ENV{PKG_CONFIG_PATH} "${EDITOR_FFMPEG_PKGCONFIG_DIR}:${_saved_pkg_config_path}")
else()
    set(ENV{PKG_CONFIG_PATH} "${EDITOR_FFMPEG_PKGCONFIG_DIR}")
endif()

pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libswscale
    libswresample
)

set(_expected_ffmpeg_libdir "${EDITOR_FFMPEG_PREFIX}/lib")
list(FIND FFMPEG_LIBRARY_DIRS "${_expected_ffmpeg_libdir}" _ffmpeg_libdir_match)
if(_ffmpeg_libdir_match EQUAL -1)
    message(FATAL_ERROR
        "Resolved FFmpeg libraries are not from the pinned prefix. "
        "Expected ${_expected_ffmpeg_libdir}, got: ${FFMPEG_LIBRARY_DIRS}")
endif()

set(MOTIVE3D_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../motive3d")
set(EDITOR_OPTIONAL_INCLUDE_DIRS "")
if(EXISTS "${MOTIVE3D_INCLUDE_DIR}")
    list(APPEND EDITOR_OPTIONAL_INCLUDE_DIRS
        "${MOTIVE3D_INCLUDE_DIR}/glm"
        "${MOTIVE3D_INCLUDE_DIR}/tinygltf"
        "${MOTIVE3D_INCLUDE_DIR}/ufbx"
        "${MOTIVE3D_INCLUDE_DIR}/glfw/include"
        "${MOTIVE3D_INCLUDE_DIR}"
    )
endif()

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
    add_subdirectory(external/CPPMonetize EXCLUDE_FROM_ALL)
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
    # 3D model import/runtime sources depend on motive3d engine files that are not
    # part of this repository build graph (eg. primitive/texture engine runtime).
    # Exclude them from the editor target so standard builds remain reproducible.
    ".*/animation\\.cpp$"
    ".*/fbx_import\\.cpp$"
    ".*/gltf_import\\.cpp$"
    ".*/model\\.cpp$"
    ".*/model_import_shared\\.cpp$"
    ".*/model_runtime\\.cpp$"
)

set(EDITOR_APP_SOURCES "${EDITOR_ALL_CPP}")
foreach(regex IN LISTS EDITOR_EXCLUDED_REGEX)
    list(FILTER EDITOR_APP_SOURCES EXCLUDE REGEX "${regex}")
endforeach()

set(EDITOR_CORE_SOURCES "${EDITOR_APP_SOURCES}")
list(FILTER EDITOR_CORE_SOURCES EXCLUDE REGEX ".*/editor\\.cpp$")
list(FILTER EDITOR_CORE_SOURCES EXCLUDE REGEX ".*/editor_main\\.cpp$")
list(FILTER EDITOR_CORE_SOURCES EXCLUDE REGEX ".*/editor_ai_integration\\.cpp$")

add_library(editor_core STATIC ${EDITOR_CORE_SOURCES})
target_sources(editor_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/preview.h
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline_widget.h
)

target_include_directories(editor_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
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
        Qt6::OpenGLWidgets
        OpenGL::GL
        Threads::Threads
        PkgConfig::FFMPEG
        rtaudio
)

set_target_properties(editor_core PROPERTIES
    BUILD_RPATH "${EDITOR_FFMPEG_PREFIX}/lib"
)

if(TARGET Qt6::GuiPrivate)
    target_link_libraries(editor_core PUBLIC Qt6::GuiPrivate)
endif()

target_compile_definitions(editor_core
    PUBLIC
        QT_DEPRECATED_WARNINGS
)

set(EDITOR_TEST_SUPPORT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/async_decoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/decode_trace.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/decoder_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/decoder_ffmpeg_utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/decoder_image_io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/debug_controls.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_shared.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_shared_effects.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_shared_keyframes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_shared_media.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_shared_render_sync.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_shared_timing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_shared_transcript.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/frame_handle.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/media_pipeline_shared.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/memory_budget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline_cache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline_cache_requests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline_cache_seek_resync.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline_cache_storage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_tab.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transcript_tab_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/clip_serialization.cpp
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
        Qt6::OpenGLWidgets
        OpenGL::GL
        Threads::Threads
        PkgConfig::FFMPEG
        rtaudio
)

if(TARGET Qt6::GuiPrivate)
    target_link_libraries(editor_test_support PUBLIC Qt6::GuiPrivate)
endif()

if(TARGET apinames)
    add_dependencies(editor_test_support apinames)
endif()

target_compile_definitions(editor_test_support
    PUBLIC
        QT_DEPRECATED_WARNINGS
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
    BUILD_RPATH "${EDITOR_FFMPEG_PREFIX}/lib"
)

if(APPLE)
    set_target_properties(editor PROPERTIES MACOSX_BUNDLE TRUE)
endif()

if(EDITOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
