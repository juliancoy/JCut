#include "speakers_tab.h"
#include "speakers_tab_internal.h"
#include "speaker_document_edit_ops.h"

#include "transcript_engine.h"

bool SpeakersTab::cycleFramingModeForSpeaker(const QString& speakerId)
{
    const SpeakerDocumentEditResult result =
        speaker_document_edit_ops::cycleFramingMode(m_transcriptSession, speakerId);
    if (!result.ok || !result.changed) {
        return false;
    }
    return saveLoadedTranscriptDocument();
}
