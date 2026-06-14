set(SPEAKERS_HEADER "${SOURCE_DIR}/speakers_tab.h")
set(SPEAKERS_SOURCE "${SOURCE_DIR}/speakers_tab.cpp")
set(INSPECTOR_SOURCE "${SOURCE_DIR}/inspector_pane.cpp")
set(INTERACTIONS_SOURCE "${SOURCE_DIR}/speakers_tab_interactions.cpp")
set(WIRING_SOURCE "${SOURCE_DIR}/speakers_tab_wiring.cpp")
set(TRACKS_SOURCE "${SOURCE_DIR}/tracks.cpp")
set(SPEAKERS_INTERNAL "${SOURCE_DIR}/speakers_tab_internal.h")
set(KEYFRAMES_SOURCE "${SOURCE_DIR}/editor_shared_keyframes.cpp")
set(CONTROL_ROUTES_SOURCE "${SOURCE_DIR}/control_server_worker_routes.cpp")

foreach(path IN ITEMS
        "${SPEAKERS_HEADER}"
        "${SPEAKERS_SOURCE}"
        "${SPEAKERS_INTERNAL}"
        "${INSPECTOR_SOURCE}"
        "${INTERACTIONS_SOURCE}"
        "${WIRING_SOURCE}"
        "${TRACKS_SOURCE}"
        "${KEYFRAMES_SOURCE}"
        "${CONTROL_ROUTES_SOURCE}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Required source file does not exist: ${path}")
    endif()
endforeach()

file(READ "${SPEAKERS_HEADER}" speakers_header)
file(READ "${SPEAKERS_SOURCE}" speakers_source)
file(READ "${SPEAKERS_INTERNAL}" speakers_internal)
file(READ "${INSPECTOR_SOURCE}" inspector_source)
file(READ "${INTERACTIONS_SOURCE}" interactions_source)
file(READ "${WIRING_SOURCE}" wiring_source)
file(READ "${TRACKS_SOURCE}" tracks_source)
file(READ "${KEYFRAMES_SOURCE}" keyframes_source)
file(READ "${CONTROL_ROUTES_SOURCE}" control_routes_source)

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
        "continuityStreamFromSectionAssignment"
        "contiguousSectionAssignmentForSection"
        "streamByTrackId"
        "sectionTrackEntriesFromAssignment"
        "sectionTrackIdStringsFromAssignment"
        "applySpeakerSectionRowTint"
        "QIcon\\(avatar\\)"
        "setItem\\(row, SpeakerSectionAvatarColumn, avatarItem\\)"
        "setItem\\(row, SpeakerSectionSpeakerColumn, speakerItem\\)")
    if(NOT speakers_source MATCHES "${required}")
        message(FATAL_ERROR "Contiguous transcript section avatar population is missing ${required}")
    endif()
endforeach()

foreach(required IN ITEMS
        "source_absolute"
        "source_frame"
        "box_size"
        "avatarStream = continuityStreamFromSectionAssignment\\(avatarAssignment\\)")
    if(NOT speakers_source MATCHES "${required}")
        message(FATAL_ERROR "Contiguous transcript section avatars must fall back to row assignment anchors when stream objects are missing: missing ${required}")
    endif()
endforeach()

if(NOT speakers_internal MATCHES "SpeakerSectionItemDelegate" OR
   NOT wiring_source MATCHES "SpeakerSectionItemDelegate")
    message(FATAL_ERROR "Contiguous transcript section table must preserve speaker-tinted selected rows")
endif()

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

foreach(required IN ITEMS
        "section_track_map"
        "assignTrackToContiguousSection"
        "deassignTrackFromContiguousSection"
        "contiguousTranscriptSectionModeActive"
        "contiguous_section_mode_active")
    if(NOT tracks_source MATCHES "${required}")
        message(FATAL_ERROR "Contiguous transcript mode must use row-scoped section-track persistence: missing ${required}")
    endif()
endforeach()

if(NOT tracks_source MATCHES "sameSection" OR
   NOT tracks_source MATCHES "sameTrack" OR
   NOT tracks_source MATCHES "sectionTrackEntriesWithTrack" OR
   NOT tracks_source MATCHES "row\\[QStringLiteral\\(\"tracks\"\\)\\] = entries" OR
   NOT tracks_source MATCHES "resolvedPayload\\[QStringLiteral\\(\"section_track_map\"\\)\\] = nextMap")
    message(FATAL_ERROR "Contiguous transcript section mapping must maintain row-scoped multi-track section assignments")
endif()

if(NOT tracks_source MATCHES "PlayheadTrackAssignedSpeakerIdRole" OR
   NOT tracks_source MATCHES "contiguousTranscriptSectionModeActive\\(\\) \\? QString\\(\\) : assignedSpeakerId")
    message(FATAL_ERROR "Contiguous transcript playhead candidates must not expose speaker-level track identity assignments")
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

if(NOT tracks_source MATCHES "editableClip\\.speakerFramingKeyframes\\.clear\\(\\)" OR
   tracks_source MATCHES "editableClip\\.speakerFramingKeyframes\\.push_back\\(framing\\)")
    message(FATAL_ERROR "Face-track clicks must leave speaker framing live; they must not bake a one-frame transform key")
endif()

foreach(required IN ITEMS
        "sectionTrackMapForClip"
        "collectSectionTrackAssignmentsForSpeaker"
        "transcriptFrameForMediaSourcePosition"
        "matchedSectionAssignment"
        "warmAssignedContinuityForSpeakerAtTranscriptFrame")
    if(NOT keyframes_source MATCHES "${required}")
        message(FATAL_ERROR "Runtime speaker framing must consume warmed contiguous section-track mappings: missing ${required}")
    endif()
endforeach()

if(NOT keyframes_source MATCHES "collectSectionTrackAssignmentsForSpeaker\\(transcriptRoot,[ \n\r\t]*clip,[ \n\r\t]*trimmedSpeakerId,[ \n\r\t]*mediaSourceFramePosition" OR
   NOT keyframes_source MATCHES "jcut::speakertrack::assignmentMapForClip\\(transcriptRoot, clip\\.id\\)")
    message(FATAL_ERROR "Runtime speaker framing must prefer section_track_map at the current frame and only then fall back to identity mapping")
endif()

foreach(required IN ITEMS
        "/speaker-flow/track-map"
        "section_track_map"
        "track_identity_map"
        "contiguous_sections"
        "active_section"
        "active_assignment"
        "contiguousTranscriptSectionsForRoute"
        "speaker_flow_clip"
        "selected_clip_file_path"
        "transcriptPath"
        "clipId"
        "QUrl::fromPercentEncoding")
    if(NOT control_routes_source MATCHES "${required}")
        message(FATAL_ERROR "REST API must expose the complete speaker-flow track map for contiguous-row avatar debugging: missing ${required}")
    endif()
endforeach()
