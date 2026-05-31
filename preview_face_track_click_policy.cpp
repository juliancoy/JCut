#include "preview_face_track_click_policy.h"

namespace jcut::preview {

FaceTrackClickAssignmentAction faceTrackClickAssignmentAction(bool hasTranscriptDocument,
                                                              bool hasActiveSpeaker,
                                                              bool activeCutMutable)
{
    if (!hasTranscriptDocument || !hasActiveSpeaker) {
        return FaceTrackClickAssignmentAction::SelectOnlyNoSpeaker;
    }
    if (!activeCutMutable) {
        return FaceTrackClickAssignmentAction::SelectOnlyReadOnly;
    }
    return FaceTrackClickAssignmentAction::AssignToSpeaker;
}

} // namespace jcut::preview
