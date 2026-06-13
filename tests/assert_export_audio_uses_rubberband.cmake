set(RENDER_AUDIO_SOURCE "${SOURCE_DIR}/render_audio.cpp")

if(NOT EXISTS "${RENDER_AUDIO_SOURCE}")
    message(FATAL_ERROR "Required source file does not exist: ${RENDER_AUDIO_SOURCE}")
endif()

file(READ "${RENDER_AUDIO_SOURCE}" render_audio_source)

foreach(required IN ITEMS
        "audio_time_stretch.h"
        "exportRubberBandSettings"
        "timeStretchPreservePitch"
        "AudioTimeStretchBackend::RubberBand"
        "Rubber Band pitch-preserving audio stretch failed")
    if(NOT render_audio_source MATCHES "${required}")
        message(FATAL_ERROR "render_audio.cpp export path is missing ${required}")
    endif()
endforeach()

if(render_audio_source MATCHES "mixAudioChunk\\([^;]*speed\\);")
    message(FATAL_ERROR "export audio must not varispeed-mix source audio directly; mix at 1.0 then Rubber Band stretch")
endif()

if(NOT render_audio_source MATCHES "mixAudioChunk\\([^;]*1\\.0\\);")
    message(FATAL_ERROR "export audio must mix source audio at normal pitch before Rubber Band stretch")
endif()
