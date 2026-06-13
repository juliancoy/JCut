set(PROJECT_STATE "${SOURCE_DIR}/project_state.cpp")
set(EDITOR_CPP "${SOURCE_DIR}/editor.cpp")
set(EDITOR_BINDINGS "${SOURCE_DIR}/editor_inspector_bindings.cpp")
set(EDITOR_H "${SOURCE_DIR}/editor.h")

foreach(path IN ITEMS "${PROJECT_STATE}" "${EDITOR_CPP}" "${EDITOR_BINDINGS}" "${EDITOR_H}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Required source file does not exist: ${path}")
    endif()
endforeach()

file(READ "${PROJECT_STATE}" project_state_source)
file(READ "${EDITOR_CPP}" editor_source)
file(READ "${EDITOR_BINDINGS}" bindings_source)
file(READ "${EDITOR_H}" header_source)

set(state_key "speakerShowContiguousTranscriptSections")
set(widget_name "m_speakerShowContiguousSectionsCheckBox")

if(project_state_source MATCHES "root\\[QStringLiteral\\(\"${state_key}\"\\)\\][^\n]*\n[^\n]*${widget_name}")
else()
    message(FATAL_ERROR "buildStateJson must write ${state_key} from ${widget_name}")
endif()

if(editor_source MATCHES "root\\.value\\(QStringLiteral\\(\"${state_key}\"\\)\\)\\.toBool\\(false\\)" AND
   editor_source MATCHES "${widget_name}->setChecked\\(speakerShowContiguousTranscriptSections\\)")
else()
    message(FATAL_ERROR "applyStateJson must read ${state_key} and restore ${widget_name}")
endif()

if(bindings_source MATCHES "${widget_name}[^\n]*=[^\n]*\n[^\n]*m_inspectorPane->speakerShowContiguousSectionsCheckBox\\(\\)" AND
   bindings_source MATCHES "${widget_name}.*QCheckBox::toggled")
else()
    message(FATAL_ERROR "${widget_name} must be bound and schedule state persistence when toggled")
endif()

if(NOT header_source MATCHES "QCheckBox \\*${widget_name} = nullptr;")
    message(FATAL_ERROR "EditorWindow must own ${widget_name} for project-state persistence")
endif()
