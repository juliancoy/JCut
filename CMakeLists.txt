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
option(JCUT_USE_OPENCV_CONTRIB "Enable OpenCV contrib modules (tracking backends)" ON)
option(JCUT_ENABLE_OPENCV_CUDA "Build bundled OpenCV with CUDA DNN support when CUDA/cuDNN are available" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(EDITOR_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

find_package(PkgConfig REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Concurrent OpenGLWidgets Test)
find_program(GLSLANG_VALIDATOR glslangValidator)
find_library(CUDA_DRIVER_LIBRARY cuda)

set(JCUT_OPENCV_SOURCE "none")
set(JCUT_OPENCV_LIBS "")
set(JCUT_OPENCV_INCLUDES "")
set(JCUT_HAVE_OPENCV_CONTRIB 0)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/opencv/CMakeLists.txt")
    if(JCUT_USE_OPENCV_CONTRIB AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/opencv_contrib/modules/tracking/CMakeLists.txt")
        set(OPENCV_EXTRA_MODULES_PATH
            "${CMAKE_CURRENT_SOURCE_DIR}/external/opencv_contrib/modules"
            CACHE PATH "" FORCE)
        set(BUILD_LIST "core,imgproc,objdetect,dnn,video,tracking" CACHE STRING "" FORCE)
        set(JCUT_HAVE_OPENCV_CONTRIB 1)
    else()
        set(BUILD_LIST "core,imgproc,objdetect,dnn" CACHE STRING "" FORCE)
    endif()
    set(BUILD_opencv_world OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_PERF_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(INSTALL_CREATE_UNINSTALL_TARGET OFF CACHE BOOL "" FORCE)
    set(WITH_IPP OFF CACHE BOOL "" FORCE)
    set(WITH_VULKAN ON CACHE BOOL "" FORCE)
    if(JCUT_ENABLE_OPENCV_CUDA)
        set(WITH_CUDA ON CACHE BOOL "" FORCE)
        set(WITH_CUBLAS ON CACHE BOOL "" FORCE)
        set(WITH_CUDNN ON CACHE BOOL "" FORCE)
        set(OPENCV_DNN_CUDA ON CACHE BOOL "" FORCE)
    endif()
    set(_saved_automoc "${CMAKE_AUTOMOC}")
    set(_saved_autorcc "${CMAKE_AUTORCC}")
    set(_saved_autouic "${CMAKE_AUTOUIC}")
    set(CMAKE_AUTOMOC OFF)
    set(CMAKE_AUTORCC OFF)
    set(CMAKE_AUTOUIC OFF)
    add_subdirectory(external/opencv EXCLUDE_FROM_ALL)
    set(CMAKE_AUTOMOC "${_saved_automoc}")
    set(CMAKE_AUTORCC "${_saved_autorcc}")
    set(CMAKE_AUTOUIC "${_saved_autouic}")
    set(JCUT_OPENCV_SOURCE "submodule")
    set(JCUT_OPENCV_LIBS opencv_core opencv_imgproc opencv_objdetect opencv_dnn)
    if(JCUT_HAVE_OPENCV_CONTRIB)
        list(APPEND JCUT_OPENCV_LIBS opencv_video opencv_tracking)
    endif()
    set(JCUT_OPENCV_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/external/opencv/modules/core/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/external/opencv/modules/dnn/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/external/opencv/modules/imgproc/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/external/opencv/modules/objdetect/include"
        "${CMAKE_CURRENT_BINARY_DIR}/external/opencv"
        "${CMAKE_CURRENT_BINARY_DIR}")
    if(JCUT_HAVE_OPENCV_CONTRIB)
        list(APPEND JCUT_OPENCV_INCLUDES
            "${CMAKE_CURRENT_SOURCE_DIR}/external/opencv/modules/video/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/external/opencv_contrib/modules/tracking/include")
    endif()
else()
    if(JCUT_USE_OPENCV_CONTRIB)
        find_package(OpenCV QUIET COMPONENTS core imgproc objdetect dnn video tracking)
    else()
        find_package(OpenCV QUIET COMPONENTS core imgproc objdetect dnn)
    endif()
    if(OpenCV_FOUND)
        set(JCUT_OPENCV_SOURCE "system")
        set(JCUT_OPENCV_LIBS ${OpenCV_LIBS})
        set(JCUT_OPENCV_INCLUDES ${OpenCV_INCLUDE_DIRS})
        if(JCUT_USE_OPENCV_CONTRIB)
            list(FIND OpenCV_LIBS opencv_tracking _opencv_tracking_idx)
            if(NOT _opencv_tracking_idx EQUAL -1)
                set(JCUT_HAVE_OPENCV_CONTRIB 1)
            endif()
        endif()
    endif()
endif()

option(JCUT_USE_SYSTEM_FFMPEG
    "Use system FFmpeg libraries. Required when system FFmpeg provides encoders such as NVENC that the pinned build lacks."
    OFF)
set(EDITOR_FFMPEG_PREFIX
    "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg-install"
    CACHE PATH "Pinned FFmpeg install prefix used when JCUT_USE_SYSTEM_FFMPEG is OFF")

if(NOT JCUT_USE_SYSTEM_FFMPEG)
    set(EDITOR_FFMPEG_PKGCONFIG_DIR "${EDITOR_FFMPEG_PREFIX}/lib/pkgconfig")
    if(NOT EXISTS "${EDITOR_FFMPEG_PKGCONFIG_DIR}/libavcodec.pc")
        message(FATAL_ERROR
            "Bundled FFmpeg pkg-config files not found at ${EDITOR_FFMPEG_PKGCONFIG_DIR}. "
            "Run ./build.sh to bootstrap the pinned FFmpeg build or configure with -DJCUT_USE_SYSTEM_FFMPEG=ON.")
    endif()

    set(_saved_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
    if(_saved_pkg_config_path)
        set(ENV{PKG_CONFIG_PATH} "${EDITOR_FFMPEG_PKGCONFIG_DIR}:${_saved_pkg_config_path}")
    else()
        set(ENV{PKG_CONFIG_PATH} "${EDITOR_FFMPEG_PKGCONFIG_DIR}")
    endif()
endif()

pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libswscale
    libswresample
)

if(NOT JCUT_USE_SYSTEM_FFMPEG)
    set(_expected_ffmpeg_libdir "${EDITOR_FFMPEG_PREFIX}/lib")
    list(FIND FFMPEG_LIBRARY_DIRS "${_expected_ffmpeg_libdir}" _ffmpeg_libdir_match)
    if(_ffmpeg_libdir_match EQUAL -1)
        message(FATAL_ERROR
            "Resolved FFmpeg libraries are not from the pinned prefix. "
            "Expected ${_expected_ffmpeg_libdir}, got: ${FFMPEG_LIBRARY_DIRS}")
    endif()
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
    ".*/backend_headless_compare_main\\.cpp$"
    ".*/vulkan_dnn_video_infer_main\\.cpp$"
    ".*/vulkan_boxstream_offscreen_main\\.cpp$"
    ".*/vulkan_boxstream_test_ui_main\\.cpp$"
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
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/mask.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/mask.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/nv12.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/nv12_y.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/nv12_uv.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/yuv420p_u.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/yuv420p_v.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/yuv420p.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/face_preprocess.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/face_infer_heuristic.comp
)
set(JCUT_VULKAN_SHADER_OUTPUTS "")
if(GLSLANG_VALIDATOR)
    foreach(shader IN LISTS JCUT_VULKAN_SHADER_SOURCES)
        get_filename_component(shader_name ${shader} NAME)
        set(out_file "${JCUT_VULKAN_SHADER_OUTPUT_DIR}/${shader_name}.spv")
        add_custom_command(
            OUTPUT ${out_file}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${JCUT_VULKAN_SHADER_OUTPUT_DIR}"
            COMMAND ${GLSLANG_VALIDATOR} -V "${shader}" -o "${out_file}"
            DEPENDS "${shader}"
            COMMENT "Compiling Vulkan shader ${shader_name}"
            VERBATIM
        )
        list(APPEND JCUT_VULKAN_SHADER_OUTPUTS "${out_file}")
    endforeach()
    add_custom_target(jcut_vulkan_shaders DEPENDS ${JCUT_VULKAN_SHADER_OUTPUTS})
else()
    message(WARNING "glslangValidator not found; Vulkan shader SPIR-V build artifacts will not be generated.")
endif()

add_library(editor_core STATIC ${EDITOR_CORE_SOURCES})
target_sources(editor_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/opengl_preview.h
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
        Vulkan::Vulkan
        Threads::Threads
        PkgConfig::FFMPEG
        rtaudio
)
if(CUDA_DRIVER_LIBRARY)
    target_link_libraries(editor_core PUBLIC ${CUDA_DRIVER_LIBRARY})
    target_compile_definitions(editor_core PUBLIC JCUT_HAS_CUDA_DRIVER=1)
else()
    target_compile_definitions(editor_core PUBLIC JCUT_HAS_CUDA_DRIVER=0)
endif()
if(JCUT_OPENCV_LIBS)
    target_link_libraries(editor_core PUBLIC ${JCUT_OPENCV_LIBS})
    target_include_directories(editor_core PUBLIC ${JCUT_OPENCV_INCLUDES})
    target_compile_definitions(editor_core PUBLIC JCUT_HAVE_OPENCV=1)
    target_compile_definitions(editor_core PUBLIC JCUT_HAVE_OPENCV_CONTRIB=${JCUT_HAVE_OPENCV_CONTRIB})
else()
    target_compile_definitions(editor_core PUBLIC JCUT_HAVE_OPENCV=0)
    target_compile_definitions(editor_core PUBLIC JCUT_HAVE_OPENCV_CONTRIB=0)
endif()

if(NOT JCUT_USE_SYSTEM_FFMPEG)
    set_target_properties(editor_core PROPERTIES
        BUILD_RPATH "${EDITOR_FFMPEG_PREFIX}/lib"
    )
endif()

if(TARGET Qt6::GuiPrivate)
    target_link_libraries(editor_core PUBLIC Qt6::GuiPrivate)
endif()

target_compile_definitions(editor_core
    PUBLIC
        QT_DEPRECATED_WARNINGS
        JCUT_VULKAN_SHADER_DIR="${JCUT_VULKAN_SHADER_OUTPUT_DIR}"
)
if(TARGET jcut_vulkan_shaders)
    add_dependencies(editor_core jcut_vulkan_shaders)
endif()

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
if(JCUT_OPENCV_LIBS)
    target_link_libraries(editor_test_support PUBLIC ${JCUT_OPENCV_LIBS})
    target_include_directories(editor_test_support PUBLIC ${JCUT_OPENCV_INCLUDES})
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
if(NOT JCUT_USE_SYSTEM_FFMPEG)
    set_target_properties(editor PROPERTIES
        BUILD_RPATH "${EDITOR_FFMPEG_PREFIX}/lib"
    )
endif()

add_executable(project_roundtrip_cli
    ${CMAKE_CURRENT_SOURCE_DIR}/project_roundtrip_cli.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/project_manager.cpp
)
target_link_libraries(project_roundtrip_cli
    PRIVATE
        Qt6::Core
        Qt6::Widgets
)

if(APPLE)
    set_target_properties(editor PROPERTIES MACOSX_BUNDLE TRUE)
endif()

add_executable(jcut_vulkan_dnn_video_infer
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_dnn_video_infer_main.cpp
)
target_include_directories(jcut_vulkan_dnn_video_infer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${JCUT_OPENCV_INCLUDES}
)
target_link_libraries(jcut_vulkan_dnn_video_infer PRIVATE
    editor_test_support
    ${JCUT_OPENCV_LIBS}
    PkgConfig::FFMPEG
)
if(NOT JCUT_USE_SYSTEM_FFMPEG)
    target_link_libraries(jcut_vulkan_dnn_video_infer PRIVATE
        "${EDITOR_FFMPEG_PREFIX}/lib/libswresample.so")
    target_link_options(jcut_vulkan_dnn_video_infer PRIVATE
        "LINKER:--no-as-needed")
endif()
set_target_properties(jcut_vulkan_dnn_video_infer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(NOT JCUT_USE_SYSTEM_FFMPEG)
    set_target_properties(jcut_vulkan_dnn_video_infer PROPERTIES
        BUILD_RPATH "${EDITOR_FFMPEG_PREFIX}/lib"
    )
endif()

add_executable(jcut_vulkan_zero_copy_video_preprocess
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_zero_copy_video_preprocess_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_zero_copy_face_detector.cpp
)
target_include_directories(jcut_vulkan_zero_copy_video_preprocess PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(jcut_vulkan_zero_copy_video_preprocess PRIVATE
    editor_test_support
    Vulkan::Vulkan
)
if(NOT JCUT_USE_SYSTEM_FFMPEG)
    target_link_libraries(jcut_vulkan_zero_copy_video_preprocess PRIVATE
        "${EDITOR_FFMPEG_PREFIX}/lib/libswresample.so")
    target_link_options(jcut_vulkan_zero_copy_video_preprocess PRIVATE
        "LINKER:--no-as-needed")
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
if(NOT JCUT_USE_SYSTEM_FFMPEG)
    set_target_properties(jcut_vulkan_zero_copy_video_preprocess PROPERTIES
        BUILD_RPATH "${EDITOR_FFMPEG_PREFIX}/lib"
    )
endif()

add_executable(jcut_vulkan_boxstream_offscreen
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_boxstream_offscreen_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/boxstream_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/offscreen_vulkan_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_cpu_fallback.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_stats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/titles.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_zero_copy_face_detector.cpp
)
target_include_directories(jcut_vulkan_boxstream_offscreen PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${JCUT_OPENCV_INCLUDES}
)
target_link_libraries(jcut_vulkan_boxstream_offscreen PRIVATE
    editor_test_support
    Vulkan::Vulkan
    ${JCUT_OPENCV_LIBS}
)
if(NOT JCUT_USE_SYSTEM_FFMPEG)
    target_link_libraries(jcut_vulkan_boxstream_offscreen PRIVATE
        "${EDITOR_FFMPEG_PREFIX}/lib/libswresample.so")
    target_link_options(jcut_vulkan_boxstream_offscreen PRIVATE
        "LINKER:--no-as-needed")
endif()
target_compile_definitions(jcut_vulkan_boxstream_offscreen PRIVATE
    JCUT_VULKAN_SHADER_DIR="${JCUT_VULKAN_SHADER_OUTPUT_DIR}"
    JCUT_HAVE_OPENCV_CONTRIB=${JCUT_HAVE_OPENCV_CONTRIB}
)
if(JCUT_OPENCV_LIBS)
    target_compile_definitions(jcut_vulkan_boxstream_offscreen PRIVATE JCUT_HAVE_OPENCV=1)
else()
    target_compile_definitions(jcut_vulkan_boxstream_offscreen PRIVATE JCUT_HAVE_OPENCV=0)
endif()
set_target_properties(jcut_vulkan_boxstream_offscreen PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(TARGET jcut_vulkan_shaders)
    add_dependencies(jcut_vulkan_boxstream_offscreen jcut_vulkan_shaders)
endif()
if(NOT JCUT_USE_SYSTEM_FFMPEG)
    set_target_properties(jcut_vulkan_boxstream_offscreen PROPERTIES
        BUILD_RPATH "${EDITOR_FFMPEG_PREFIX}/lib"
    )
endif()

add_executable(jcut_vulkan_boxstream_test_ui
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_boxstream_test_ui_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/boxstream_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/offscreen_vulkan_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render_stats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/titles.cpp
)
target_link_libraries(jcut_vulkan_boxstream_test_ui PRIVATE
    editor_test_support
    Vulkan::Vulkan
)
if(NOT JCUT_USE_SYSTEM_FFMPEG)
    target_link_libraries(jcut_vulkan_boxstream_test_ui PRIVATE
        "${EDITOR_FFMPEG_PREFIX}/lib/libswresample.so")
    target_link_options(jcut_vulkan_boxstream_test_ui PRIVATE
        "LINKER:--no-as-needed")
endif()
target_compile_definitions(jcut_vulkan_boxstream_test_ui PRIVATE
    JCUT_HAVE_OPENCV_CONTRIB=${JCUT_HAVE_OPENCV_CONTRIB}
    JCUT_VULKAN_SHADER_DIR="${JCUT_VULKAN_SHADER_OUTPUT_DIR}"
)
if(JCUT_OPENCV_LIBS)
    target_compile_definitions(jcut_vulkan_boxstream_test_ui PRIVATE JCUT_HAVE_OPENCV=1)
else()
    target_compile_definitions(jcut_vulkan_boxstream_test_ui PRIVATE JCUT_HAVE_OPENCV=0)
endif()
set_target_properties(jcut_vulkan_boxstream_test_ui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(NOT JCUT_USE_SYSTEM_FFMPEG)
    set_target_properties(jcut_vulkan_boxstream_test_ui PROPERTIES
        BUILD_RPATH "${EDITOR_FFMPEG_PREFIX}/lib"
    )
endif()
add_dependencies(jcut_vulkan_boxstream_test_ui jcut_vulkan_boxstream_offscreen)

add_executable(jcut_headless_backend_compare
    ${CMAKE_CURRENT_SOURCE_DIR}/backend_headless_compare_main.cpp
)
set_target_properties(jcut_headless_backend_compare PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(EDITOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
