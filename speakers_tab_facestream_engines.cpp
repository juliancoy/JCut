#include "speakers_tab.h"
#include "speakers_tab_internal.h"

#include "transcript_engine.h"

bool SpeakersTab::cycleFramingModeForSpeaker(const QString& speakerId)
{
    if (!m_loadedTranscriptDoc.isObject() || speakerId.trimmed().isEmpty()) {
        return false;
    }

    const QJsonObject root = m_loadedTranscriptDoc.object();
    const QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonObject profile = profiles.value(speakerId).toObject();
    const QJsonObject tracking = speakerFramingObject(profile);
    const bool hasPointstream = transcriptTrackingHasPointstream(tracking);
    const QString currentMode =
        tracking.value(QString(kTranscriptSpeakerTrackingModeKey)).toString(QStringLiteral("Manual")).trimmed();

    QString nextMode = QStringLiteral("Manual");
    if (currentMode.compare(QStringLiteral("manual"), Qt::CaseInsensitive) == 0) {
        nextMode = QStringLiteral("ReferencePoints");
    } else if (currentMode.compare(QStringLiteral("referencepoints"), Qt::CaseInsensitive) == 0) {
        nextMode = hasPointstream ? QStringLiteral("Tracked") : QStringLiteral("Manual");
    } else {
        nextMode = QStringLiteral("Manual");
    }

    if (!updateLoadedTranscriptDocument([&](QJsonObject& root) {
            QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
            QJsonObject profile = profiles.value(speakerId).toObject();
            QJsonObject tracking = speakerFramingObject(profile);
            tracking[QString(kTranscriptSpeakerTrackingModeKey)] = nextMode;
            if (nextMode == QStringLiteral("Manual")) {
                tracking[QString(kTranscriptSpeakerTrackingEnabledKey)] = false;
                tracking[QString(kTranscriptSpeakerTrackingAutoStateKey)] = QStringLiteral("manual");
            } else if (nextMode == QStringLiteral("ReferencePoints")) {
                tracking[QString(kTranscriptSpeakerTrackingEnabledKey)] = false;
                tracking[QString(kTranscriptSpeakerTrackingAutoStateKey)] = QStringLiteral("refs_only");
            }

            setSpeakerFramingObject(profile, tracking);
            profiles[speakerId] = profile;
            root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
            return true;
        })) {
        return false;
    }
    return saveLoadedTranscriptDocument();
}
