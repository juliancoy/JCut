set(EDITOR_CPP "${SOURCE_DIR}/editor.cpp")
set(INSPECTOR_CPP "${SOURCE_DIR}/inspector_pane.cpp")
set(SPEAKERS_CPP "${SOURCE_DIR}/speakers_tab.cpp")

foreach(path IN ITEMS "${EDITOR_CPP}" "${INSPECTOR_CPP}" "${SPEAKERS_CPP}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Required source file does not exist: ${path}")
    endif()
endforeach()

file(READ "${EDITOR_CPP}" editor_source)
file(READ "${INSPECTOR_CPP}" inspector_source)
file(READ "${SPEAKERS_CPP}" speakers_source)

if(NOT inspector_source MATCHES
   "m_speakersSubtabs = new QTabWidget\\(speakerListPanel\\)")
    message(FATAL_ERROR
        "The visible Speakers work tabs must be exposed through speakersSubtabs() so refresh routing can observe page changes")
endif()

if(inspector_source MATCHES "content->setMinimumWidth\\(1040\\)" OR
   NOT inspector_source MATCHES
       "speakerSectionsLayout->addWidget\\(speakerSectionsFilterRow\\)[^}]*speakerSectionsLayout->addWidget\\(m_speakerSectionsTable, 1\\)[^}]*speakerSectionsLayout->addWidget\\(speakerSectionsControlsGroup\\)")
    message(FATAL_ERROR
        "Speakers must use responsive width with filter, table, then secondary controls")
endif()

if(NOT inspector_source MATCHES "speakers.sections.search" OR
   NOT inspector_source MATCHES "speakers.sections.summary" OR
   NOT inspector_source MATCHES "sectionsHeader->setSectionResizeMode\\(7, QHeaderView::Stretch\\)")
    message(FATAL_ERROR
        "Sections UI must provide filtering, a result summary, and a responsive transcript column")
endif()

if(NOT speakers_source MATCHES
   "normalized.compare\\(QStringLiteral\\(\"Sections\"\\)[^}]*m_widgets.speakerShowContiguousSectionsCheckBox->setChecked\\(true\\)")
    message(FATAL_ERROR
        "SpeakersTab must reconcile section mode from the actual subtab navigation event")
endif()

if(inspector_source MATCHES "m_speakerSectionsTable->hide\\(\\)" OR
   speakers_source MATCHES "speakerSectionsTable->setVisible\\(showSections\\)")
    message(FATAL_ERROR
        "Tables on separate speaker subtab pages must not have a second visibility authority")
endif()

if(NOT inspector_source MATCHES
   "speakerSectionsControlsGroup->setSizePolicy\\(QSizePolicy::Preferred, QSizePolicy::Fixed\\)")
    message(FATAL_ERROR
        "Section controls must not expand vertically and displace the table")
endif()

if(editor_source MATCHES
   "QSignalBlocker[^;]*m_speakerShowContiguousSectionsCheckBox")
    message(FATAL_ERROR
        "State restoration must not block the section-mode toggled signal")
endif()

if(NOT inspector_source MATCHES
   "syncSectionModeFromWorkTab\\(speakerWorkTabs->currentIndex\\(\\)\\)")
    message(FATAL_ERROR
        "The speaker work tab must initialize mode from its current page")
endif()

if(NOT inspector_source MATCHES
   "index == sectionsTabIndex[^}]*m_speakerShowContiguousSectionsCheckBox->setChecked\\(true\\)")
    message(FATAL_ERROR
        "Selecting Sections must enable contiguous-section mode")
endif()
