#include "preview_face_track_click_policy.h"

#include <QtTest/QtTest>

class TestPreviewFaceTrackClickPolicy : public QObject {
    Q_OBJECT

private slots:
    void selectsOnlyWhenTranscriptHasNoActiveSpeaker()
    {
        QCOMPARE(jcut::preview::faceTrackClickAssignmentAction(
                     true,
                     false,
                     true),
                 jcut::preview::FaceTrackClickAssignmentAction::SelectOnlyNoSpeaker);
    }

    void selectsOnlyWhenTranscriptDocumentIsUnavailable()
    {
        QCOMPARE(jcut::preview::faceTrackClickAssignmentAction(
                     false,
                     true,
                     true),
                 jcut::preview::FaceTrackClickAssignmentAction::SelectOnlyNoSpeaker);
    }

    void selectsOnlyWhenCutIsReadOnly()
    {
        QCOMPARE(jcut::preview::faceTrackClickAssignmentAction(
                     true,
                     true,
                     false),
                 jcut::preview::FaceTrackClickAssignmentAction::SelectOnlyReadOnly);
    }

    void assignsOnlyWithSpeakerAndMutableCut()
    {
        QCOMPARE(jcut::preview::faceTrackClickAssignmentAction(
                     true,
                     true,
                     true),
                 jcut::preview::FaceTrackClickAssignmentAction::AssignToSpeaker);
    }
};

QTEST_MAIN(TestPreviewFaceTrackClickPolicy)
#include "test_preview_face_track_click_policy.moc"
