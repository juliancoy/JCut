#include "speaker_document_edit_ops.h"

#include "speakers_tab_internal.h"

#include <QJsonArray>
#include <QJsonObject>

#include <cmath>

namespace {

QJsonObject speakerProfiles(const TranscriptDocumentSession& session)
{
    return session.rootObject().value(QString(kTranscriptSpeakerProfilesKey)).toObject();
}

SpeakerDocumentEditResult makeUnchangedResult()
{
    return SpeakerDocumentEditResult{.ok = true, .changed = false};
}

SpeakerDocumentEditResult makeChangedResult(bool ok)
{
    return SpeakerDocumentEditResult{.ok = ok, .changed = ok};
}

} // namespace

namespace speaker_document_edit_ops {

SpeakerDocumentEditResult applyProfileCellEdit(TranscriptDocumentSession& session,
                                               const QString& speakerId,
                                               int column,
                                               const QString& valueText)
{
    if (!session.hasObjectDocument() || speakerId.trimmed().isEmpty()) {
        return {};
    }

    bool ok = true;
    const double parsed = (column == 2 || column == 3) ? valueText.toDouble(&ok) : 0.0;
    if ((column == 2 || column == 3) && !ok) {
        return {};
    }

    bool changed = false;
    const bool mutated = session.mutateRoot([&](QJsonObject& root) {
        QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
        QJsonObject profile = profiles.value(speakerId).toObject();
        QJsonObject location = profile.value(QString(kTranscriptSpeakerLocationKey)).toObject();

        if (column == 1) {
            const QString nextValue = valueText.trimmed().isEmpty() ? speakerId : valueText.trimmed();
            if (profile.value(QString(kTranscriptSpeakerNameKey)).toString().trimmed() == nextValue) {
                return false;
            }
            profile[QString(kTranscriptSpeakerNameKey)] = nextValue;
            changed = true;
        } else if (column == 2 || column == 3) {
            const double bounded = qBound(0.0, parsed, 1.0);
            const QString key = column == 2
                ? QString(kTranscriptSpeakerLocationXKey)
                : QString(kTranscriptSpeakerLocationYKey);
            if (std::abs(location.value(key).toDouble(column == 2 ? 0.5 : 0.85) - bounded) < 0.0001) {
                return false;
            }
            location[key] = bounded;
            profile[QString(kTranscriptSpeakerLocationKey)] = location;
            changed = true;
        } else {
            return false;
        }

        profiles[speakerId] = profile;
        root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
        return changed;
    });
    return SpeakerDocumentEditResult{.ok = mutated || !changed, .changed = changed};
}

SpeakerDocumentEditResult setTrackingEnabled(TranscriptDocumentSession& session,
                                             const QString& speakerId,
                                             bool enabled)
{
    if (!session.hasObjectDocument() || speakerId.trimmed().isEmpty()) {
        return {};
    }

    const QJsonObject profiles = speakerProfiles(session);
    const QJsonObject profile = profiles.value(speakerId).toObject();
    const QJsonObject tracking = speakerFramingObject(profile);
    if (tracking.isEmpty()) {
        return {};
    }
    if (enabled && !transcriptTrackingHasPointstream(tracking)) {
        return {};
    }

    bool changed = false;
    const bool mutated = session.mutateRoot([&](QJsonObject& root) {
        QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
        QJsonObject profile = profiles.value(speakerId).toObject();
        QJsonObject tracking = speakerFramingObject(profile);
        const bool previous = tracking.value(QString(kTranscriptSpeakerTrackingEnabledKey)).toBool(false);
        if (previous == enabled) {
            return false;
        }
        tracking[QString(kTranscriptSpeakerTrackingEnabledKey)] = enabled;
        setSpeakerFramingObject(profile, tracking);
        profiles[speakerId] = profile;
        root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
        changed = true;
        return true;
    });
    return SpeakerDocumentEditResult{.ok = mutated || !changed, .changed = changed};
}

SpeakerDocumentEditResult deleteAutoTrackPointstream(TranscriptDocumentSession& session,
                                                     const QString& speakerId)
{
    if (!session.hasObjectDocument() || speakerId.trimmed().isEmpty()) {
        return {};
    }

    const QJsonObject profiles = speakerProfiles(session);
    const QJsonObject profile = profiles.value(speakerId).toObject();
    const QJsonObject tracking = speakerFramingObject(profile);
    if (tracking.isEmpty()) {
        return {};
    }
    if (!transcriptTrackingHasPointstream(tracking)) {
        return makeUnchangedResult();
    }

    bool changed = false;
    const bool mutated = session.mutateRoot([&](QJsonObject& root) {
        QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
        QJsonObject profile = profiles.value(speakerId).toObject();
        QJsonObject tracking = speakerFramingObject(profile);
        tracking[QString(kTranscriptSpeakerTrackingKeyframesKey)] = QJsonArray();
        tracking[QString(kTranscriptSpeakerTrackingEnabledKey)] = false;
        tracking[QString(kTranscriptSpeakerTrackingModeKey)] = QStringLiteral("ReferencePoints");
        tracking[QString(kTranscriptSpeakerTrackingAutoStateKey)] = QStringLiteral("deleted_pointstream");
        setSpeakerFramingObject(profile, tracking);
        profiles[speakerId] = profile;
        root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
        changed = true;
        return true;
    });
    return SpeakerDocumentEditResult{.ok = mutated, .changed = mutated && changed};
}

SpeakerDocumentEditResult setSpeakerSkipped(TranscriptDocumentSession& session,
                                            const QString& speakerId,
                                            bool skipped)
{
    if (!session.hasObjectDocument() || speakerId.trimmed().isEmpty()) {
        return {};
    }

    const QString targetSpeaker = speakerId.trimmed();
    bool changed = false;
    const bool mutated = session.mutateRoot([&](QJsonObject& root) {
        QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
        for (int segIndex = 0; segIndex < segments.size(); ++segIndex) {
            QJsonObject segmentObj = segments.at(segIndex).toObject();
            const QString segmentSpeaker =
                segmentObj.value(QString(kTranscriptSegmentSpeakerKey)).toString().trimmed();
            QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
            for (int wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
                QJsonObject wordObj = words.at(wordIndex).toObject();
                QString wordSpeaker = wordObj.value(QString(kTranscriptWordSpeakerKey)).toString().trimmed();
                if (wordSpeaker.isEmpty()) {
                    wordSpeaker = segmentSpeaker;
                }
                if (wordSpeaker != targetSpeaker) {
                    continue;
                }
                if (wordObj.value(QStringLiteral("word")).toString().trimmed().isEmpty()) {
                    continue;
                }
                const bool previous = wordObj.value(QStringLiteral("skipped")).toBool(false);
                if (previous == skipped) {
                    continue;
                }
                wordObj[QStringLiteral("skipped")] = skipped;
                words.replace(wordIndex, wordObj);
                changed = true;
            }
            segmentObj[QStringLiteral("words")] = words;
            segments.replace(segIndex, segmentObj);
        }
        if (!changed) {
            return false;
        }
        root[QStringLiteral("segments")] = segments;
        return true;
    });
    return SpeakerDocumentEditResult{.ok = mutated || !changed, .changed = changed};
}

SpeakerDocumentEditResult applyProfileStringFieldUpdates(TranscriptDocumentSession& session,
                                                         const QString& fieldKey,
                                                         const QVector<SpeakerFieldValueUpdate>& updates)
{
    if (!session.hasObjectDocument() || fieldKey.trimmed().isEmpty() || updates.isEmpty()) {
        return {};
    }

    bool changed = false;
    const bool mutated = session.mutateRoot([&](QJsonObject& root) {
        QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
        for (const SpeakerFieldValueUpdate& update : updates) {
            const QString speakerId = update.speakerId.trimmed();
            if (speakerId.isEmpty()) {
                continue;
            }
            QJsonObject profile = profiles.value(speakerId).toObject();
            const QString nextValue = update.value.trimmed();
            if (profile.value(fieldKey).toString().trimmed() == nextValue) {
                continue;
            }
            profile[fieldKey] = nextValue;
            profiles[speakerId] = profile;
            changed = true;
        }
        if (!changed) {
            return false;
        }
        root[QString(kTranscriptSpeakerProfilesKey)] = profiles;
        return true;
    });
    return SpeakerDocumentEditResult{.ok = mutated || !changed, .changed = changed};
}

SpeakerDocumentEditResult cycleFramingMode(TranscriptDocumentSession& session,
                                           const QString& speakerId)
{
    if (!session.hasObjectDocument() || speakerId.trimmed().isEmpty()) {
        return {};
    }

    const QJsonObject profiles = speakerProfiles(session);
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
    }

    bool changed = false;
    const bool mutated = session.mutateRoot([&](QJsonObject& root) {
        QJsonObject profiles = root.value(QString(kTranscriptSpeakerProfilesKey)).toObject();
        QJsonObject profile = profiles.value(speakerId).toObject();
        QJsonObject tracking = speakerFramingObject(profile);
        if (tracking.value(QString(kTranscriptSpeakerTrackingModeKey)).toString().trimmed() == nextMode) {
            return false;
        }
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
        changed = true;
        return true;
    });
    return SpeakerDocumentEditResult{.ok = mutated || !changed, .changed = changed};
}

} // namespace speaker_document_edit_ops
