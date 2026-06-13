set(SPEAKERS_HEADER "${SOURCE_DIR}/speakers_tab.h")
set(SPEAKERS_SOURCE "${SOURCE_DIR}/speakers_tab.cpp")
set(INSPECTOR_SOURCE "${SOURCE_DIR}/inspector_pane.cpp")
set(INTERACTIONS_SOURCE "${SOURCE_DIR}/speakers_tab_interactions.cpp")
set(WIRING_SOURCE "${SOURCE_DIR}/speakers_tab_wiring.cpp")
set(TRACKS_SOURCE "${SOURCE_DIR}/tracks.cpp")

foreach(path IN ITEMS
        "${SPEAKERS_HEADER}"
        "${SPEAKERS_SOURCE}"
        "${INSPECTOR_SOURCE}"
        "${INTERACTIONS_SOURCE}"
        "${WIRING_SOURCE}"
        "${TRACKS_SOURCE}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Required source file does not exist: ${path}")
    endif()
endforeach()

file(READ "${SPEAKERS_HEADER}" speakers_header)
file(READ "${SPEAKERS_SOURCE}" speakers_source)
file(READ "${INSPECTOR_SOURCE}" inspector_source)
file(READ "${INTERACTIONS_SOURCE}" interactions_source)
file(READ "${WIRING_SOURCE}" wiring_source)
file(READ "${TRACKS_SOURCE}" tracks_source)

foreach(required IN ITEMS
        "SpeakerSectionAvatarColumn = 0"
        "SpeakerSectionSpeakerColumn = 2"
        "SpeakerSectionColumnCount = 7")
    if(NOT speakers_header MATCHES "${required}")
        message(FATAL_ERROR "Contiguous transcript section table column contract is missing ${required}")
    endif()
endforeach()

if(NOT inspector_source MATCHES "m_speakerSectionsTable->setColumnCount\\(7\\)" OR
   NOT inspector_source MATCHES "QStringLiteral\\(\"Avatar\"\\)")
    message(FATAL_ERROR "Contiguous transcript section table must expose a left-side Avatar column")
endif()

foreach(required IN ITEMS
        "continuityTrackAvatar"
        "streamByTrackId"
        "QIcon\\(avatar\\)"
        "setItem\\(row, SpeakerSectionAvatarColumn, avatarItem\\)"
        "setItem\\(row, SpeakerSectionSpeakerColumn, speakerItem\\)")
    if(NOT speakers_source MATCHES "${required}")
        message(FATAL_ERROR "Contiguous transcript section avatar population is missing ${required}")
    endif()
endforeach()

if(speakers_source MATCHES "speakerSectionsTable->item\\([^,]+, 1\\)" OR
   speakers_source MATCHES "speakerSectionsTable->setCurrentCell\\([^,]+, 1\\)" OR
   interactions_source MATCHES "speakerSectionsTable->item\\([^,]+, 1\\)" OR
   interactions_source MATCHES "speakerSectionsTable->setCurrentCell\\([^,]+, 1\\)" OR
   wiring_source MATCHES "speakerSectionsTable->item\\([^,]+, 1\\)")
    message(FATAL_ERROR "Contiguous transcript section code must use named columns after adding Avatar")
endif()

foreach(required IN ITEMS
        "table == m_widgets.speakerSectionsTable"
        "SpeakerSectionSpeakerColumn"
        "SpeakerSectionSpeakerIdRole")
    if(NOT speakers_source MATCHES "${required}")
        message(FATAL_ERROR "selectedSpeakerId must resolve speaker IDs from the contiguous section speaker column")
    endif()
endforeach()

if(NOT speakers_header MATCHES "refreshVisibleSpeakerSectionAssignments" OR
   NOT speakers_source MATCHES "refreshVisibleSpeakerSectionAssignments" OR
   NOT tracks_source MATCHES "refreshVisibleSpeakerSectionAssignments\\(trimmedSpeakerId\\)")
    message(FATAL_ERROR "Track assignment persistence must update visible contiguous section rows without rebuilding the table")
endif()

if(tracks_source MATCHES "refreshSpeakerSectionsTable\\(m_transcriptSession\\.rootObject\\(\\)\\)")
    message(FATAL_ERROR "Track assignment persistence must not rebuild all contiguous transcript sections")
endif()

foreach(required IN ITEMS
        "ensurePersistentTrackAvatarMemory"
        "assignedTrackIds"
        "avatarDecoder"
        "setAllowHardwareFrameMaterialization\\(true\\)")
    if(NOT tracks_source MATCHES "${required}")
        message(FATAL_ERROR "Track assignment must persist avatar memory for assigned contiguous-section tracks: missing ${required}")
    endif()
endforeach()

if(tracks_source MATCHES "setDebugDecodePreference\\(editor::DecodePreference::Hardware\\)" OR
   tracks_source MATCHES "previousDecodePreference")
    message(FATAL_ERROR "Avatar generation must not mutate global decode preference")
endif()
