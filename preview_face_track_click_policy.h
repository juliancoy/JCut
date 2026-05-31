#pragma once

namespace jcut::preview {

enum class FaceTrackClickAssignmentAction {
    SelectOnlyNoSpeaker,
    SelectOnlyReadOnly,
    AssignToSpeaker
};

FaceTrackClickAssignmentAction faceTrackClickAssignmentAction(bool hasTranscriptDocument,
                                                              bool hasActiveSpeaker,
                                                              bool activeCutMutable);

} // namespace jcut::preview
