#include <QtTest/QtTest>

#include "../editor_document_core_json.h"
#include "../imgui_project_io.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTemporaryDir>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace {

using json = nlohmann::json;

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
        file.write(bytes) == bytes.size();
}

bool writeJson(const QString& path, const json& root)
{
    return writeBytes(path, QByteArray::fromStdString(root.dump(2) + "\n"));
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

std::optional<json> readJson(const QString& path)
{
    try {
        return json::parse(readBytes(path).toStdString());
    } catch (...) {
        return std::nullopt;
    }
}

jcut::EditorDocumentCore documentNamed(const std::string& name)
{
    jcut::EditorDocumentCore document;
    document.projectName = name;
    document.tracks = {{1, "Video 1", true}};
    document.transport.currentFrame = 73;
    document.exportRequest.outputSize = {1920, 1080};
    document.exportRequest.outputFps = 30.0;
    document.exportRequest.outputFormat = "mp4";
    return document;
}

jcut::EditorDocumentCore historyDocument(
    const std::string& name,
    int currentFrame,
    std::size_t clipCount)
{
    jcut::EditorDocumentCore document = documentNamed(name);
    document.transport.currentFrame = currentFrame;
    for (std::size_t index = 0; index < clipCount; ++index) {
        jcut::EditorClip clip;
        clip.id = static_cast<int>(index + 1);
        clip.trackId = 1;
        clip.label = "Clip " + std::to_string(index + 1);
        clip.startFrame = static_cast<int>(index * 20);
        clip.durationFrames = 10;
        clip.sourcePath = "clip-" + std::to_string(index + 1) + ".mp4";
        clip.persistentId = "history-clip-" + std::to_string(index + 1);
        clip.mediaKind = "video";
        clip.sourceDurationFrames = 10;
        document.clips.push_back(std::move(clip));
    }
    return document;
}

jcut::ImGuiProjectSession sessionFor(
    const QString& statePath,
    const QString& historyPath,
    const json& legacyStateRoot)
{
    jcut::ImGuiProjectSession session;
    session.statePath = statePath.toStdString();
    session.historyPath = historyPath.toStdString();
    session.legacyStateRoot = legacyStateRoot;
    return session;
}

bool applyHistoryPatch(json* base, const json& patch)
{
    if (!base || !base->is_object() || !patch.is_object()) {
        return false;
    }

    const json set = patch.value("set", json::object());
    const json remove = patch.value("remove", json::array());
    if (!set.is_object() || !remove.is_array()) {
        return false;
    }
    for (const auto& [key, value] : set.items()) {
        const json::iterator existing = base->find(key);
        if (existing != base->end() && existing->is_object() && value.is_object() &&
            (value.contains("set") || value.contains("remove"))) {
            json nested = *existing;
            if (!applyHistoryPatch(&nested, value)) {
                return false;
            }
            (*base)[key] = std::move(nested);
        } else {
            (*base)[key] = value;
        }
    }
    for (const json& key : remove) {
        if (!key.is_string()) {
            return false;
        }
        base->erase(key.get<std::string>());
    }
    return true;
}

json qtCompatibleHistorySnapshot(json snapshot)
{
    snapshot.erase("__historyTranscriptDocuments");
    snapshot.erase("stateRevision");
    json timeline = snapshot.value("timeline", json::array());
    if (timeline.is_array()) {
        for (json& clip : timeline) {
            if (clip.is_object()) {
                clip.erase("speakerFramingKeyframes");
            }
        }
        snapshot["timeline"] = std::move(timeline);
    }
    json selectedClip = snapshot.value("selectedClip", json::object());
    if (selectedClip.is_object()) {
        selectedClip.erase("speakerFramingKeyframes");
    }
    snapshot["selectedClip"] = std::move(selectedClip);
    return snapshot;
}

bool writeManagedProject(QDir* root,
                         const QString& projectId,
                         const jcut::EditorDocumentCore& document)
{
    if (!root ||
        !root->mkpath(QStringLiteral("projects/") + projectId)) {
        return false;
    }
    const json state = jcut::toLegacyStateJson(document);
    const json history = {
        {"index", 0},
        {"entries", json::array({qtCompatibleHistorySnapshot(state)})}
    };
    return writeJson(
               root->filePath(
                   QStringLiteral("projects/") + projectId +
                   QStringLiteral("/state.json")),
               state) &&
        writeJson(
            root->filePath(
                QStringLiteral("projects/") + projectId +
                QStringLiteral("/history.json")),
            history);
}

class ScopedProjectRoot {
public:
    explicit ScopedProjectRoot(const QString& path)
        : m_previousRoot(qgetenv("JCUT_PROJECT_ROOT"))
        , m_active(qputenv("JCUT_PROJECT_ROOT", path.toUtf8()))
    {
    }

    ~ScopedProjectRoot()
    {
        if (m_previousRoot.isEmpty()) {
            qunsetenv("JCUT_PROJECT_ROOT");
        } else {
            qputenv("JCUT_PROJECT_ROOT", m_previousRoot);
        }
    }

    bool active() const { return m_active; }

private:
    QByteArray m_previousRoot;
    bool m_active = false;
};

class ScopedProjectIoFailures {
public:
    ScopedProjectIoFailures()
    {
        jcut::testing::clearImGuiProjectIoFailures();
    }

    ~ScopedProjectIoFailures()
    {
        jcut::testing::clearImGuiProjectIoFailures();
    }

    void inject(jcut::testing::ImGuiProjectIoFailurePoint point)
    {
        jcut::testing::injectNextImGuiProjectIoFailure(point);
    }
};

} // namespace

class TestImGuiProjectHistory : public QObject {
    Q_OBJECT

private slots:
    void legacyHistoryPreservesEntireStackAndAvoidsDuplicateSnapshots();
    void snapshotDeltaHistoryBranchesAtActiveIndex();
    void malformedHistoryFailsBeforeProjectFilesAreChanged();
    void missingHistoryStartsWithTheCurrentSnapshot();
    void malformedStateRecoversFromSnapshotDeltaHistory();
    void failedStateCommitRollsBackHistory();
    void configuredHistoryLimitsPreserveExtendedStack();
    void trimmingPreservesActiveOldestSnapshot();
    void concurrentSavesSerializeAndRejectStaleSession();
    void unsupportedHistoryFormatIsRejected();
    void legacyHistoryNavigationPreservesRedoEntries();
    void snapshotDeltaHistoryNavigationPreservesPatches();
    void historyNavigationRejectsInvalidAndEscapedRequests();
    void historyNavigationRejectsStaleSessions();
    void historyNavigationRollsBackIndexWhenStateCommitFails();
    void projectListAndSelectionReuseActiveProjectStore();
    void newProjectSanitizesUniquelyAndRejectsTraversal();
    void newProjectFailuresCleanOrReportReservedDirectory();
    void saveAsCopiesHistoryWithoutChangingTheSource();
    void saveAsMarkerFailurePreservesSourceAndCleansDestination();
    void renamePreservesFilesUpdatesMarkerAndHandlesCollisions();
    void renameFailuresPreserveOrRecoverAConsistentIdentity();
    void activationMarkerFailureKeepsPreviousProjectActive();
    void committedProjectLoadFailureRestoresPreviousIdentity();
    void managedSessionsRejectSymlinkEscapes();
    void staleManagedSaveCannotRecreateProjectAfterRename();
    void legacyStateOverridesApplyAfterConcurrencyValidation();
    void autosaveWritesCompatibleSnapshotAndTrimsBackups();
    void imguiShellWiresLifecycleActionsThroughSharedDirtyGuard();
};

void TestImGuiProjectHistory::legacyHistoryPreservesEntireStackAndAvoidsDuplicateSnapshots()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString statePath = tempDir.filePath(QStringLiteral("state.json"));
    const QString historyPath = tempDir.filePath(QStringLiteral("history.json"));

    const json firstSnapshot = {
        {"projectName", "First Snapshot"},
        {"tracks", json::array({{{"name", "Video 1"}}})},
        {"timeline", json::array()},
        {"legacyOnly", {{"revision", 1}}}
    };
    json secondSnapshot = firstSnapshot;
    secondSnapshot["projectName"] = "Second Snapshot";
    secondSnapshot["legacyOnly"]["revision"] = 2;
    const json originalHistory = {
        {"index", 1},
        {"entries", json::array({firstSnapshot, secondSnapshot})},
        {"retainedMetadata", {{"writer", "qt"}}}
    };
    QVERIFY(writeJson(statePath, secondSnapshot));
    QVERIFY(writeJson(historyPath, originalHistory));

    const jcut::EditorDocumentCore updated = documentNamed("Saved From ImGui");
    jcut::ImGuiProjectSession session =
        sessionFor(statePath, historyPath, secondSnapshot);
    const json expectedSnapshot = jcut::toLegacyStateJson(updated, &secondSnapshot);

    std::string error;
    QVERIFY2(jcut::saveImGuiProjectSession(session, updated, &error), error.c_str());

    const std::optional<json> savedState = readJson(statePath);
    const std::optional<json> savedHistory = readJson(historyPath);
    QVERIFY(savedState.has_value());
    QVERIFY(savedHistory.has_value());
    QVERIFY(*savedState == expectedSnapshot);
    QCOMPARE(savedHistory->at("index").get<int>(), 2);
    QCOMPARE(savedHistory->at("entries").size(), std::size_t(3));
    QVERIFY(savedHistory->at("entries").at(0) == qtCompatibleHistorySnapshot(firstSnapshot));
    QVERIFY(savedHistory->at("entries").at(1) == qtCompatibleHistorySnapshot(secondSnapshot));
    QVERIFY(savedHistory->at("entries").at(2) == qtCompatibleHistorySnapshot(expectedSnapshot));
    QCOMPARE(QString::fromStdString(
                 savedHistory->at("retainedMetadata").at("writer").get<std::string>()),
             QStringLiteral("qt"));

    session.legacyStateRoot = *savedState;
    error.clear();
    QVERIFY2(jcut::saveImGuiProjectSession(session, updated, &error), error.c_str());
    const std::optional<json> savedAgain = readJson(historyPath);
    QVERIFY(savedAgain.has_value());
    QCOMPARE(savedAgain->at("entries").size(), std::size_t(3));
    QCOMPARE(savedAgain->at("index").get<int>(), 2);
}

void TestImGuiProjectHistory::snapshotDeltaHistoryBranchesAtActiveIndex()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString statePath = tempDir.filePath(QStringLiteral("state.json"));
    const QString historyPath = tempDir.filePath(QStringLiteral("history.json"));

    const json baseSnapshot = {
        {"projectName", "Base Snapshot"},
        {"tracks", json::array({{{"name", "Video 1"}}})},
        {"timeline", json::array()},
        {"legacyOnly", {{"revision", 1}, {"obsolete", true}, {"retained", "yes"}}}
    };
    const json redoPatch = {
        {"set", {
            {"projectName", "Redo Snapshot"},
            {"legacyOnly", {
                {"set", {{"revision", 2}}},
                {"remove", json::array({"obsolete"})}
            }}
        }},
        {"remove", json::array()}
    };
    json redoSnapshot = baseSnapshot;
    QVERIFY(applyHistoryPatch(&redoSnapshot, redoPatch));

    const json originalHistory = {
        {"format", "snapshot-delta-v1"},
        {"index", 0},
        {"base", baseSnapshot},
        {"patches", json::array({redoPatch})},
        {"retainedMetadata", {{"writer", "qt-delta"}}}
    };
    QVERIFY(writeJson(statePath, baseSnapshot));
    QVERIFY(writeJson(historyPath, originalHistory));

    const jcut::EditorDocumentCore updated = documentNamed("Delta Saved From ImGui");
    const jcut::ImGuiProjectSession session =
        sessionFor(statePath, historyPath, baseSnapshot);
    const json expectedSnapshot = jcut::toLegacyStateJson(updated, &baseSnapshot);

    std::string error;
    QVERIFY2(jcut::saveImGuiProjectSession(session, updated, &error), error.c_str());

    const std::optional<json> savedHistory = readJson(historyPath);
    QVERIFY(savedHistory.has_value());
    QCOMPARE(QString::fromStdString(savedHistory->at("format").get<std::string>()),
             QStringLiteral("snapshot-delta-v1"));
    QCOMPARE(savedHistory->at("index").get<int>(), 1);
    QVERIFY(savedHistory->at("base") == qtCompatibleHistorySnapshot(baseSnapshot));
    QCOMPARE(savedHistory->at("patches").size(), std::size_t(1));
    QVERIFY(savedHistory->at("patches").at(0) != redoPatch);
    QVERIFY(!savedHistory->contains("entries"));
    QCOMPARE(QString::fromStdString(
                 savedHistory->at("retainedMetadata").at("writer").get<std::string>()),
             QStringLiteral("qt-delta"));

    json reconstructed = savedHistory->at("base");
    QVERIFY(applyHistoryPatch(&reconstructed, savedHistory->at("patches").at(0)));
    QVERIFY(reconstructed == qtCompatibleHistorySnapshot(expectedSnapshot));
}

void TestImGuiProjectHistory::malformedHistoryFailsBeforeProjectFilesAreChanged()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString statePath = tempDir.filePath(QStringLiteral("state.json"));
    const QString historyPath = tempDir.filePath(QStringLiteral("history.json"));
    const QByteArray originalState = R"({"projectName":"Original","tracks":[],"timeline":[]})";
    const QByteArray malformedHistory = "{this-is-not-json\n";
    QVERIFY(writeBytes(statePath, originalState));
    QVERIFY(writeBytes(historyPath, malformedHistory));

    const json legacyRoot = json::parse(originalState.toStdString());
    const jcut::EditorDocumentCore updated = documentNamed("Must Not Be Written");
    const jcut::ImGuiProjectSession session = sessionFor(statePath, historyPath, legacyRoot);

    std::string error;
    QVERIFY(!jcut::saveImGuiProjectSession(session, updated, &error));
    QVERIFY(QString::fromStdString(error).contains(QStringLiteral("parse project history")));
    QCOMPARE(readBytes(statePath), originalState);
    QCOMPARE(readBytes(historyPath), malformedHistory);
}

void TestImGuiProjectHistory::missingHistoryStartsWithTheCurrentSnapshot()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString statePath = tempDir.filePath(QStringLiteral("state.json"));
    const QString historyPath = tempDir.filePath(QStringLiteral("history.json"));
    const json originalState = {
        {"projectName", "Original"},
        {"tracks", json::array()},
        {"timeline", json::array()}
    };
    QVERIFY(writeJson(statePath, originalState));
    QVERIFY(!QFile::exists(historyPath));

    const jcut::EditorDocumentCore updated = documentNamed("First ImGui Snapshot");
    const jcut::ImGuiProjectSession session =
        sessionFor(statePath, historyPath, originalState);
    const json expectedSnapshot = jcut::toLegacyStateJson(updated, &originalState);

    std::string error;
    QVERIFY2(jcut::saveImGuiProjectSession(session, updated, &error), error.c_str());
    const std::optional<json> savedHistory = readJson(historyPath);
    QVERIFY(savedHistory.has_value());
    QCOMPARE(savedHistory->at("index").get<int>(), 0);
    QCOMPARE(savedHistory->at("entries").size(), std::size_t(1));
    QVERIFY(savedHistory->at("entries").at(0) == expectedSnapshot);
}

void TestImGuiProjectHistory::malformedStateRecoversFromSnapshotDeltaHistory()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QByteArray previousRoot = qgetenv("JCUT_PROJECT_ROOT");
    QVERIFY(qputenv("JCUT_PROJECT_ROOT", tempDir.path().toUtf8()));

    QDir root(tempDir.path());
    QVERIFY(root.mkpath(QStringLiteral("projects/recovery")));
    QVERIFY(writeBytes(root.filePath(QStringLiteral("projects/.current_project")), "recovery\n"));
    const QString statePath = root.filePath(QStringLiteral("projects/recovery/state.json"));
    const QString historyPath = root.filePath(QStringLiteral("projects/recovery/history.json"));
    QVERIFY(writeBytes(statePath, "{malformed-state\n"));

    const json base = {
        {"projectName", "Recovered"},
        {"tracks", json::array({{{"name", "Video"}}})},
        {"timeline", json::array()}
    };
    const json current = {
        {"projectName", "Recovered Current"},
        {"tracks", json::array({{{"name", "Video"}}})},
        {"timeline", json::array()}
    };
    const json patch = {
        {"set", {{"projectName", "Recovered Current"}}},
        {"remove", json::array()}
    };
    QVERIFY(writeJson(historyPath, {
        {"format", "snapshot-delta-v1"},
        {"index", 1},
        {"base", base},
        {"patches", json::array({patch})}
    }));

    std::string error;
    const std::optional<jcut::ImGuiProjectSession> session =
        jcut::loadActiveImGuiProjectSession(&error);
    QVERIFY2(session.has_value(), error.c_str());
    QCOMPARE(QString::fromStdString(session->document.projectName),
             QStringLiteral("Recovered Current"));

    jcut::EditorDocumentCore edited = session->document;
    edited.projectName = "Recovered And Saved";
    QVERIFY2(jcut::saveImGuiProjectSession(*session, edited, &error), error.c_str());
    const std::optional<json> repairedState = readJson(statePath);
    QVERIFY(repairedState.has_value());
    QCOMPARE(QString::fromStdString(repairedState->value("projectName", std::string{})),
             QStringLiteral("Recovered And Saved"));

    if (previousRoot.isEmpty()) {
        qunsetenv("JCUT_PROJECT_ROOT");
    } else {
        qputenv("JCUT_PROJECT_ROOT", previousRoot);
    }
}

void TestImGuiProjectHistory::failedStateCommitRollsBackHistory()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString stateDir = tempDir.filePath(QStringLiteral("read-only-state"));
    const QString historyDir = tempDir.filePath(QStringLiteral("writable-history"));
    QVERIFY(QDir().mkpath(stateDir));
    QVERIFY(QDir().mkpath(historyDir));
    const QString statePath = QDir(stateDir).filePath(QStringLiteral("state.json"));
    const QString historyPath = QDir(historyDir).filePath(QStringLiteral("history.json"));

    const json originalState = {
        {"projectName", "Original"},
        {"tracks", json::array({{{"name", "Video"}}})},
        {"timeline", json::array()}
    };
    const json originalHistory = {
        {"index", 0},
        {"entries", json::array({originalState})}
    };
    QVERIFY(writeJson(statePath, originalState));
    QVERIFY(writeJson(historyPath, originalHistory));
    const QByteArray originalHistoryBytes = readBytes(historyPath);
    QVERIFY(QFile::setPermissions(
        stateDir,
        QFileDevice::ReadOwner | QFileDevice::ExeOwner));

    const jcut::ImGuiProjectSession session =
        sessionFor(statePath, historyPath, originalState);
    std::string error;
    const bool saved = jcut::saveImGuiProjectSession(
        session, documentNamed("New State"), &error);
    QVERIFY(QFile::setPermissions(
        stateDir,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    QVERIFY2(!saved, error.c_str());
    QVERIFY2(!error.empty(), "failed save did not report an error");
    QCOMPARE(readBytes(historyPath), originalHistoryBytes);
    QCOMPARE(readBytes(statePath), QByteArray::fromStdString(originalState.dump(2) + "\n"));
}

void TestImGuiProjectHistory::configuredHistoryLimitsPreserveExtendedStack()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString statePath = tempDir.filePath(QStringLiteral("state.json"));
    const QString historyPath = tempDir.filePath(QStringLiteral("history.json"));

    json entries = json::array();
    for (int index = 0; index < 150; ++index) {
        entries.push_back({
            {"projectName", "Revision " + std::to_string(index)},
            {"historyMaxEntries", 200},
            {"historyMaxMegabytes", 256},
            {"tracks", json::array({{{"name", "Video"}}})},
            {"timeline", json::array()}
        });
    }
    const json currentState = entries.back();
    QVERIFY(writeJson(statePath, currentState));
    QVERIFY(writeJson(historyPath, {{"index", 149}, {"entries", entries}}));

    jcut::EditorDocumentCore document = documentNamed("Revision 150");
    const jcut::ImGuiProjectSession session =
        sessionFor(statePath, historyPath, currentState);
    std::string error;
    QVERIFY2(jcut::saveImGuiProjectSession(session, document, &error), error.c_str());

    const std::optional<json> savedHistory = readJson(historyPath);
    QVERIFY(savedHistory.has_value());
    QCOMPARE(savedHistory->at("entries").size(), std::size_t(151));
    QCOMPARE(savedHistory->at("index").get<int>(), 150);
}

void TestImGuiProjectHistory::trimmingPreservesActiveOldestSnapshot()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString statePath = tempDir.filePath(QStringLiteral("state.json"));
    const QString historyPath = tempDir.filePath(QStringLiteral("history.json"));

    const jcut::EditorDocumentCore document = documentNamed("Active Oldest");
    const json settings = {
        {"historyMaxEntries", 10},
        {"historyMaxMegabytes", 256}
    };
    const json activeState = jcut::toLegacyStateJson(document, &settings);
    json entries = json::array({activeState});
    for (int index = 1; index < 12; ++index) {
        json redo = activeState;
        redo["projectName"] = "Redo " + std::to_string(index);
        entries.push_back(std::move(redo));
    }
    QVERIFY(writeJson(statePath, activeState));
    QVERIFY(writeJson(historyPath, {{"index", 0}, {"entries", entries}}));

    const jcut::ImGuiProjectSession session =
        sessionFor(statePath, historyPath, activeState);
    std::string error;
    QVERIFY2(jcut::saveImGuiProjectSession(session, document, &error), error.c_str());

    const std::optional<json> savedHistory = readJson(historyPath);
    QVERIFY(savedHistory.has_value());
    QCOMPARE(savedHistory->at("entries").size(), std::size_t(10));
    QCOMPARE(savedHistory->at("index").get<int>(), 0);
    QVERIFY(savedHistory->at("entries").at(0) ==
            qtCompatibleHistorySnapshot(activeState));
}

void TestImGuiProjectHistory::concurrentSavesSerializeAndRejectStaleSession()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString statePath = tempDir.filePath(QStringLiteral("state.json"));
    const QString historyPath = tempDir.filePath(QStringLiteral("history.json"));
    const json initialState = jcut::toLegacyStateJson(documentNamed("Initial"));
    QVERIFY(writeJson(statePath, initialState));
    QVERIFY(writeJson(historyPath, {
        {"index", 0},
        {"entries", json::array({qtCompatibleHistorySnapshot(initialState)})}
    }));
    const jcut::ImGuiProjectSession session =
        sessionFor(statePath, historyPath, initialState);

    auto saveNamed = [&](const std::string& name) {
        std::string error;
        const bool saved = jcut::saveImGuiProjectSession(
            session, documentNamed(name), &error);
        return std::make_pair(saved, error);
    };
    auto first = std::async(std::launch::async, saveNamed, "Concurrent A");
    auto second = std::async(std::launch::async, saveNamed, "Concurrent B");
    const auto firstResult = first.get();
    const auto secondResult = second.get();
    QCOMPARE(static_cast<int>(firstResult.first) + static_cast<int>(secondResult.first), 1);

    const std::optional<json> savedState = readJson(statePath);
    const std::optional<json> savedHistory = readJson(historyPath);
    QVERIFY(savedState.has_value());
    QVERIFY(savedHistory.has_value());
    const std::size_t activeIndex = savedHistory->at("index").get<std::size_t>();
    QVERIFY(savedHistory->at("entries").at(activeIndex) ==
            qtCompatibleHistorySnapshot(*savedState));
}

void TestImGuiProjectHistory::unsupportedHistoryFormatIsRejected()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString statePath = tempDir.filePath(QStringLiteral("state.json"));
    const QString historyPath = tempDir.filePath(QStringLiteral("history.json"));
    const json state = {
        {"projectName", "Original"},
        {"tracks", json::array()},
        {"timeline", json::array()}
    };
    QVERIFY(writeJson(statePath, state));
    QVERIFY(writeJson(historyPath, {
        {"format", "snapshot-delta-v2"},
        {"index", 0},
        {"base", state},
        {"patches", json::array()}
    }));

    const QByteArray originalState = readBytes(statePath);
    const QByteArray originalHistory = readBytes(historyPath);
    std::string error;
    QVERIFY(!jcut::saveImGuiProjectSession(
        sessionFor(statePath, historyPath, state), documentNamed("Rejected"), &error));
    QVERIFY(QString::fromStdString(error).contains(QStringLiteral("unsupported")));
    QCOMPARE(readBytes(statePath), originalState);
    QCOMPARE(readBytes(historyPath), originalHistory);
}

void TestImGuiProjectHistory::legacyHistoryNavigationPreservesRedoEntries()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ScopedProjectRoot projectRoot(tempDir.path());
    QVERIFY(projectRoot.active());

    QDir root(tempDir.path());
    QVERIFY(root.mkpath(QStringLiteral("projects/alpha")));
    const json first = qtCompatibleHistorySnapshot(
        jcut::toLegacyStateJson(historyDocument("History Project", 10, 0)));
    const json second = qtCompatibleHistorySnapshot(
        jcut::toLegacyStateJson(historyDocument("History Project", 20, 1)));
    const json third = qtCompatibleHistorySnapshot(
        jcut::toLegacyStateJson(historyDocument("History Project", 30, 2)));
    const json entries = json::array({first, second, third});
    const QString statePath =
        root.filePath(QStringLiteral("projects/alpha/state.json"));
    const QString historyPath =
        root.filePath(QStringLiteral("projects/alpha/history.json"));
    QVERIFY(writeJson(statePath, third));
    QVERIFY(writeJson(historyPath, {
        {"index", 2},
        {"entries", entries},
        {"retainedMetadata", {{"writer", "qt"}}}
    }));
    QVERIFY(writeBytes(
        root.filePath(QStringLiteral("projects/.current_project")), "alpha\n"));
    const QByteArray listedStateBytes = readBytes(statePath);
    const QByteArray listedHistoryBytes = readBytes(historyPath);

    std::string error;
    const std::optional<jcut::ImGuiProjectSession> session =
        jcut::loadActiveImGuiProjectSession(&error);
    QVERIFY2(session.has_value(), error.c_str());
    const auto listing = jcut::listImGuiProjectHistoryEntries(*session, &error);
    QVERIFY2(listing.has_value(), error.c_str());
    QCOMPARE(listing->size(), std::size_t(3));
    QCOMPARE(listing->at(0).index, std::size_t(0));
    QVERIFY(!listing->at(0).isActive);
    QCOMPARE(QString::fromStdString(listing->at(0).projectName),
             QStringLiteral("History Project"));
    QCOMPARE(listing->at(0).currentFrame, std::int64_t(10));
    QCOMPARE(listing->at(0).clipCount, std::size_t(0));
    QCOMPARE(listing->at(1).currentFrame, std::int64_t(20));
    QCOMPARE(listing->at(1).clipCount, std::size_t(1));
    QVERIFY(listing->at(2).isActive);
    QCOMPARE(QString::fromStdString(listing->at(2).projectName),
             QStringLiteral("History Project"));
    QCOMPARE(listing->at(2).currentFrame, std::int64_t(30));
    QCOMPARE(listing->at(2).clipCount, std::size_t(2));
    QCOMPARE(readBytes(statePath), listedStateBytes);
    QCOMPARE(readBytes(historyPath), listedHistoryBytes);

    const std::optional<jcut::ImGuiProjectSession> firstSession =
        jcut::activateImGuiProjectHistoryEntry(*session, 0, &error);
    QVERIFY2(firstSession.has_value(), error.c_str());
    QCOMPARE(QString::fromStdString(firstSession->document.projectName),
             QStringLiteral("History Project"));
    QCOMPARE(firstSession->document.transport.currentFrame, 10);
    QCOMPARE(firstSession->document.clips.size(), std::size_t(0));
    const std::optional<json> firstState = readJson(statePath);
    const std::optional<json> firstHistory = readJson(historyPath);
    QVERIFY(firstState.has_value());
    QVERIFY(firstHistory.has_value());
    QVERIFY(*firstState == first);
    QCOMPARE(firstHistory->at("index").get<int>(), 0);
    QCOMPARE(firstHistory->at("entries").size(), std::size_t(3));
    QVERIFY(firstHistory->at("entries") == entries);
    QCOMPARE(QString::fromStdString(
                 firstHistory->at("retainedMetadata")
                     .at("writer").get<std::string>()),
             QStringLiteral("qt"));

    const auto firstListing =
        jcut::listImGuiProjectHistoryEntries(*firstSession, &error);
    QVERIFY2(firstListing.has_value(), error.c_str());
    QVERIFY(firstListing->at(0).isActive);
    QVERIFY(!firstListing->at(2).isActive);

    const std::optional<jcut::ImGuiProjectSession> thirdSession =
        jcut::activateImGuiProjectHistoryEntry(*firstSession, 2, &error);
    QVERIFY2(thirdSession.has_value(), error.c_str());
    QCOMPARE(QString::fromStdString(thirdSession->document.projectName),
             QStringLiteral("History Project"));
    QCOMPARE(thirdSession->document.transport.currentFrame, 30);
    QCOMPARE(thirdSession->document.clips.size(), std::size_t(2));
    const std::optional<json> restoredHistory = readJson(historyPath);
    QVERIFY(restoredHistory.has_value());
    QCOMPARE(restoredHistory->at("index").get<int>(), 2);
    QCOMPARE(restoredHistory->at("entries").size(), std::size_t(3));
    QVERIFY(restoredHistory->at("entries") == entries);
}

void TestImGuiProjectHistory::snapshotDeltaHistoryNavigationPreservesPatches()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ScopedProjectRoot projectRoot(tempDir.path());
    QVERIFY(projectRoot.active());

    QDir root(tempDir.path());
    QVERIFY(root.mkpath(QStringLiteral("projects/alpha")));
    const json base = qtCompatibleHistorySnapshot(
        jcut::toLegacyStateJson(historyDocument("Delta Project", 10, 0)));
    const json secondSource = qtCompatibleHistorySnapshot(
        jcut::toLegacyStateJson(historyDocument("Delta Project", 20, 1)));
    json second = base;
    second["currentFrame"] = 20;
    second["timeline"] = secondSource.at("timeline");
    const json thirdSource = qtCompatibleHistorySnapshot(
        jcut::toLegacyStateJson(historyDocument("Delta Project", 30, 2)));
    json third = second;
    third["currentFrame"] = 30;
    third["timeline"] = thirdSource.at("timeline");
    const json patches = json::array({
        {{"set", {
             {"currentFrame", 20},
             {"timeline", second.at("timeline")}
         }},
         {"remove", json::array()}},
        {{"set", {
             {"currentFrame", 30},
             {"timeline", third.at("timeline")}
         }},
         {"remove", json::array()}}
    });
    const json history = {
        {"format", "snapshot-delta-v1"},
        {"index", 2},
        {"base", base},
        {"patches", patches},
        {"retainedMetadata", {{"writer", "qt-delta"}}}
    };
    const QString statePath =
        root.filePath(QStringLiteral("projects/alpha/state.json"));
    const QString historyPath =
        root.filePath(QStringLiteral("projects/alpha/history.json"));
    QVERIFY(writeJson(statePath, third));
    QVERIFY(writeJson(historyPath, history));
    QVERIFY(writeBytes(
        root.filePath(QStringLiteral("projects/.current_project")), "alpha\n"));

    std::string error;
    const std::optional<jcut::ImGuiProjectSession> session =
        jcut::loadActiveImGuiProjectSession(&error);
    QVERIFY2(session.has_value(), error.c_str());
    const auto listing = jcut::listImGuiProjectHistoryEntries(*session, &error);
    QVERIFY2(listing.has_value(), error.c_str());
    QCOMPARE(listing->size(), std::size_t(3));
    QCOMPARE(QString::fromStdString(listing->at(1).projectName),
             QStringLiteral("Delta Project"));
    QCOMPARE(listing->at(0).currentFrame, std::int64_t(10));
    QCOMPARE(listing->at(0).clipCount, std::size_t(0));
    QCOMPARE(listing->at(1).currentFrame, std::int64_t(20));
    QCOMPARE(listing->at(1).clipCount, std::size_t(1));
    QVERIFY(listing->at(2).isActive);
    QCOMPARE(listing->at(2).currentFrame, std::int64_t(30));
    QCOMPARE(listing->at(2).clipCount, std::size_t(2));

    const std::optional<jcut::ImGuiProjectSession> secondSession =
        jcut::activateImGuiProjectHistoryEntry(*session, 1, &error);
    QVERIFY2(secondSession.has_value(), error.c_str());
    QCOMPARE(QString::fromStdString(secondSession->document.projectName),
             QStringLiteral("Delta Project"));
    QCOMPARE(secondSession->document.transport.currentFrame, 20);
    QCOMPARE(secondSession->document.clips.size(), std::size_t(1));
    const std::optional<json> savedState = readJson(statePath);
    const std::optional<json> savedHistory = readJson(historyPath);
    QVERIFY(savedState.has_value());
    QVERIFY(savedHistory.has_value());
    QVERIFY(*savedState == second);
    QCOMPARE(savedHistory->at("index").get<int>(), 1);
    QCOMPARE(QString::fromStdString(savedHistory->at("format").get<std::string>()),
             QStringLiteral("snapshot-delta-v1"));
    QVERIFY(savedHistory->at("base") == base);
    QVERIFY(savedHistory->at("patches") == patches);
    QCOMPARE(QString::fromStdString(
                 savedHistory->at("retainedMetadata")
                     .at("writer").get<std::string>()),
             QStringLiteral("qt-delta"));

    const std::optional<jcut::ImGuiProjectSession> thirdSession =
        jcut::activateImGuiProjectHistoryEntry(*secondSession, 2, &error);
    QVERIFY2(thirdSession.has_value(), error.c_str());
    QCOMPARE(QString::fromStdString(thirdSession->document.projectName),
             QStringLiteral("Delta Project"));
    QCOMPARE(thirdSession->document.transport.currentFrame, 30);
    QCOMPARE(thirdSession->document.clips.size(), std::size_t(2));
    const std::optional<json> restoredHistory = readJson(historyPath);
    QVERIFY(restoredHistory.has_value());
    QCOMPARE(restoredHistory->at("index").get<int>(), 2);
    QVERIFY(restoredHistory->at("patches") == patches);
}

void TestImGuiProjectHistory::historyNavigationRejectsInvalidAndEscapedRequests()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ScopedProjectRoot projectRoot(tempDir.path());
    QVERIFY(projectRoot.active());

    QDir root(tempDir.path());
    QVERIFY(root.mkpath(QStringLiteral("projects/alpha")));
    const json first = qtCompatibleHistorySnapshot(
        jcut::toLegacyStateJson(documentNamed("First")));
    const json second = qtCompatibleHistorySnapshot(
        jcut::toLegacyStateJson(documentNamed("Second")));
    const QString statePath =
        root.filePath(QStringLiteral("projects/alpha/state.json"));
    const QString historyPath =
        root.filePath(QStringLiteral("projects/alpha/history.json"));
    QVERIFY(writeJson(statePath, second));
    QVERIFY(writeJson(historyPath, {
        {"index", 1},
        {"entries", json::array({first, second})}
    }));
    QVERIFY(writeBytes(
        root.filePath(QStringLiteral("projects/.current_project")), "alpha\n"));

    std::string error;
    const std::optional<jcut::ImGuiProjectSession> session =
        jcut::loadActiveImGuiProjectSession(&error);
    QVERIFY2(session.has_value(), error.c_str());
    const QByteArray stateBytes = readBytes(statePath);
    const QByteArray historyBytes = readBytes(historyPath);

    QVERIFY(!jcut::activateImGuiProjectHistoryEntry(
        *session, 2, &error).has_value());
    QVERIFY(QString::fromStdString(error).contains(
        QStringLiteral("out of range")));
    QCOMPARE(readBytes(statePath), stateBytes);
    QCOMPARE(readBytes(historyPath), historyBytes);

    jcut::ImGuiProjectSession escaped = *session;
    escaped.statePath = root.filePath(QStringLiteral("outside-state.json"))
                            .toStdString();
    error.clear();
    QVERIFY(!jcut::listImGuiProjectHistoryEntries(
        escaped, &error).has_value());
    QVERIFY(QString::fromStdString(error).contains(
        QStringLiteral("outside the active project store")));
    error.clear();
    QVERIFY(!jcut::activateImGuiProjectHistoryEntry(
        escaped, 0, &error).has_value());
    QVERIFY(QString::fromStdString(error).contains(
        QStringLiteral("outside the active project store")));
    QCOMPARE(readBytes(statePath), stateBytes);
    QCOMPARE(readBytes(historyPath), historyBytes);
}

void TestImGuiProjectHistory::historyNavigationRejectsStaleSessions()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ScopedProjectRoot projectRoot(tempDir.path());
    QVERIFY(projectRoot.active());

    QDir root(tempDir.path());
    QVERIFY(root.mkpath(QStringLiteral("projects/alpha")));
    const json first = qtCompatibleHistorySnapshot(
        jcut::toLegacyStateJson(documentNamed("First")));
    const json second = qtCompatibleHistorySnapshot(
        jcut::toLegacyStateJson(documentNamed("Second")));
    const QString statePath =
        root.filePath(QStringLiteral("projects/alpha/state.json"));
    const QString historyPath =
        root.filePath(QStringLiteral("projects/alpha/history.json"));
    QVERIFY(writeJson(statePath, first));
    QVERIFY(writeJson(historyPath, {
        {"index", 0},
        {"entries", json::array({first, second})}
    }));
    QVERIFY(writeBytes(
        root.filePath(QStringLiteral("projects/.current_project")), "alpha\n"));

    std::string error;
    const std::optional<jcut::ImGuiProjectSession> staleSession =
        jcut::loadActiveImGuiProjectSession(&error);
    QVERIFY2(staleSession.has_value(), error.c_str());

    QVERIFY(writeJson(statePath, second));
    QVERIFY(writeJson(historyPath, {
        {"index", 1},
        {"entries", json::array({first, second})}
    }));
    const QByteArray newerStateBytes = readBytes(statePath);
    const QByteArray newerHistoryBytes = readBytes(historyPath);

    QVERIFY(!jcut::listImGuiProjectHistoryEntries(
        *staleSession, &error).has_value());
    QVERIFY(QString::fromStdString(error).contains(
        QStringLiteral("changed on disk")));
    error.clear();
    QVERIFY(!jcut::activateImGuiProjectHistoryEntry(
        *staleSession, 0, &error).has_value());
    QVERIFY(QString::fromStdString(error).contains(
        QStringLiteral("changed on disk")));
    QCOMPARE(readBytes(statePath), newerStateBytes);
    QCOMPARE(readBytes(historyPath), newerHistoryBytes);
}

void TestImGuiProjectHistory::historyNavigationRollsBackIndexWhenStateCommitFails()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ScopedProjectRoot projectRoot(tempDir.path());
    QVERIFY(projectRoot.active());
    ScopedProjectIoFailures failures;

    QDir root(tempDir.path());
    QVERIFY(root.mkpath(QStringLiteral("projects/alpha")));
    const json first = qtCompatibleHistorySnapshot(
        jcut::toLegacyStateJson(documentNamed("First")));
    const json second = qtCompatibleHistorySnapshot(
        jcut::toLegacyStateJson(documentNamed("Second")));
    const QString statePath =
        root.filePath(QStringLiteral("projects/alpha/state.json"));
    const QString historyPath =
        root.filePath(QStringLiteral("projects/alpha/history.json"));
    QVERIFY(writeJson(statePath, second));
    QVERIFY(writeJson(historyPath, {
        {"index", 1},
        {"entries", json::array({first, second})},
        {"retainedMetadata", {{"writer", "qt"}}}
    }));
    QVERIFY(writeBytes(
        root.filePath(QStringLiteral("projects/.current_project")), "alpha\n"));

    std::string error;
    const std::optional<jcut::ImGuiProjectSession> session =
        jcut::loadActiveImGuiProjectSession(&error);
    QVERIFY2(session.has_value(), error.c_str());
    const QByteArray stateBytes = readBytes(statePath);
    const QByteArray historyBytes = readBytes(historyPath);

    failures.inject(
        jcut::testing::ImGuiProjectIoFailurePoint::PairedProjectStateCommit);
    QVERIFY(!jcut::activateImGuiProjectHistoryEntry(
        *session, 0, &error).has_value());
    QVERIFY(QString::fromStdString(error).contains(
        QStringLiteral("injected paired project state commit failure")));
    QCOMPARE(readBytes(statePath), stateBytes);
    QCOMPARE(readBytes(historyPath), historyBytes);

    const auto listing = jcut::listImGuiProjectHistoryEntries(*session, &error);
    QVERIFY2(listing.has_value(), error.c_str());
    QCOMPARE(listing->size(), std::size_t(2));
    QVERIFY(!listing->at(0).isActive);
    QVERIFY(listing->at(1).isActive);
}

void TestImGuiProjectHistory::projectListAndSelectionReuseActiveProjectStore()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QByteArray previousRoot = qgetenv("JCUT_PROJECT_ROOT");
    QVERIFY(qputenv("JCUT_PROJECT_ROOT", tempDir.path().toUtf8()));

    QDir root(tempDir.path());
    QVERIFY(root.mkpath(QStringLiteral("projects/alpha")));
    QVERIFY(root.mkpath(QStringLiteral("projects/beta")));
    QVERIFY(root.mkpath(QStringLiteral("projects/broken")));
    const json validState = {
        {"projectName", "Valid"},
        {"tracks", json::array()},
        {"timeline", json::array()}
    };
    QVERIFY(writeJson(root.filePath(QStringLiteral("projects/alpha/state.json")), validState));
    QVERIFY(writeJson(root.filePath(QStringLiteral("projects/beta/state.json")), validState));
    QVERIFY(writeBytes(root.filePath(QStringLiteral("projects/broken/state.json")), "{broken\n"));
    QVERIFY(writeBytes(root.filePath(QStringLiteral("projects/.current_project")), "alpha\n"));

    std::string error;
    const std::vector<std::string> ids = jcut::availableImGuiProjectIds(&error);
    QVERIFY2(error.empty(), error.c_str());
    QCOMPARE(ids.size(), std::size_t(3));
    QCOMPARE(QString::fromStdString(ids.at(0)), QStringLiteral("alpha"));
    QCOMPARE(QString::fromStdString(ids.at(1)), QStringLiteral("beta"));
    QCOMPARE(QString::fromStdString(ids.at(2)), QStringLiteral("broken"));

    QVERIFY2(jcut::setActiveImGuiProject("beta", &error), error.c_str());
    QCOMPARE(QString::fromUtf8(readBytes(root.filePath(QStringLiteral("projects/.current_project")))).trimmed(),
             QStringLiteral("beta"));
    QVERIFY(!jcut::setActiveImGuiProject("broken", &error));
    QCOMPARE(QString::fromUtf8(readBytes(root.filePath(QStringLiteral("projects/.current_project")))).trimmed(),
             QStringLiteral("beta"));
    QVERIFY(!jcut::setActiveImGuiProject("missing", &error));

    if (previousRoot.isEmpty()) {
        qunsetenv("JCUT_PROJECT_ROOT");
    } else {
        qputenv("JCUT_PROJECT_ROOT", previousRoot);
    }
}

void TestImGuiProjectHistory::newProjectSanitizesUniquelyAndRejectsTraversal()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ScopedProjectRoot projectRoot(tempDir.path());
    QVERIFY(projectRoot.active());

    QDir root(tempDir.path());
    QVERIFY(root.mkpath(QStringLiteral("projects/default")));
    QVERIFY(root.mkpath(QStringLiteral("projects/my-project")));
    const json originalState = jcut::toLegacyStateJson(documentNamed("Original"));
    const json originalHistory = {
        {"index", 0},
        {"entries", json::array({qtCompatibleHistorySnapshot(originalState)})}
    };
    const QString originalStatePath =
        root.filePath(QStringLiteral("projects/default/state.json"));
    const QString originalHistoryPath =
        root.filePath(QStringLiteral("projects/default/history.json"));
    QVERIFY(writeJson(originalStatePath, originalState));
    QVERIFY(writeJson(originalHistoryPath, originalHistory));
    QVERIFY(writeBytes(
        root.filePath(QStringLiteral("projects/.current_project")), "default\n"));
    const QByteArray originalStateBytes = readBytes(originalStatePath);
    const QByteArray originalHistoryBytes = readBytes(originalHistoryPath);

    std::string error;
    QVERIFY(!jcut::createImGuiProjectSession("../escape", &error).has_value());
    QVERIFY(QString::fromStdString(error).contains(QStringLiteral("traversal")));
    QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("escape"))));
    QCOMPARE(
        QString::fromUtf8(readBytes(
            root.filePath(QStringLiteral("projects/.current_project")))).trimmed(),
        QStringLiteral("default"));

    error.clear();
    const std::optional<jcut::ImGuiProjectSession> created =
        jcut::createImGuiProjectSession(" My Project! ", &error);
    QVERIFY2(created.has_value(), error.c_str());
    QCOMPARE(QString::fromStdString(created->projectId),
             QStringLiteral("my-project-2"));
    QCOMPARE(QString::fromStdString(created->document.projectName),
             QStringLiteral("My Project!"));
    QCOMPARE(QString::fromStdString(created->statePath),
             root.filePath(QStringLiteral("projects/my-project-2/state.json")));
    QCOMPARE(
        QString::fromUtf8(readBytes(
            root.filePath(QStringLiteral("projects/.current_project")))).trimmed(),
        QStringLiteral("my-project-2"));

    const std::optional<json> createdState = readJson(
        root.filePath(QStringLiteral("projects/my-project-2/state.json")));
    const std::optional<json> createdHistory = readJson(
        root.filePath(QStringLiteral("projects/my-project-2/history.json")));
    QVERIFY(createdState.has_value());
    QVERIFY(createdHistory.has_value());
    QCOMPARE(QString::fromStdString(
                 createdState->at("projectName").get<std::string>()),
             QStringLiteral("My Project!"));
    QCOMPARE(createdHistory->at("index").get<int>(), 0);
    QCOMPARE(createdHistory->at("entries").size(), std::size_t(1));
    QVERIFY(createdHistory->at("entries").at(0) ==
            qtCompatibleHistorySnapshot(*createdState));
    QCOMPARE(readBytes(originalStatePath), originalStateBytes);
    QCOMPARE(readBytes(originalHistoryPath), originalHistoryBytes);
}

void TestImGuiProjectHistory::newProjectFailuresCleanOrReportReservedDirectory()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ScopedProjectRoot projectRoot(tempDir.path());
    QVERIFY(projectRoot.active());
    ScopedProjectIoFailures failures;

    QDir root(tempDir.path());
    QVERIFY(writeManagedProject(&root, QStringLiteral("alpha"),
                                documentNamed("Alpha")));
    const QString markerPath =
        root.filePath(QStringLiteral("projects/.current_project"));
    QVERIFY(writeBytes(markerPath, "alpha\n"));
    const QString alphaStatePath =
        root.filePath(QStringLiteral("projects/alpha/state.json"));
    const QByteArray alphaStateBytes = readBytes(alphaStatePath);

    failures.inject(
        jcut::testing::ImGuiProjectIoFailurePoint::ActiveProjectMarkerCommit);
    std::string error;
    QVERIFY(!jcut::createImGuiProjectSession(
        "Failed New", &error).has_value());
    QVERIFY(QString::fromStdString(error).contains(
        QStringLiteral("injected active-project marker commit failure")));
    QVERIFY(!QFileInfo::exists(
        root.filePath(QStringLiteral("projects/failed-new"))));
    QCOMPARE(QString::fromUtf8(readBytes(markerPath)).trimmed(),
             QStringLiteral("alpha"));
    QCOMPARE(readBytes(alphaStatePath), alphaStateBytes);

    failures.inject(
        jcut::testing::ImGuiProjectIoFailurePoint::ActiveProjectMarkerCommit);
    failures.inject(
        jcut::testing::ImGuiProjectIoFailurePoint::
            ReservedProjectDirectoryCleanup);
    error.clear();
    QVERIFY(!jcut::createImGuiProjectSession(
        "Retained New", &error).has_value());
    const QString cleanupError = QString::fromStdString(error);
    QVERIFY(cleanupError.contains(
        QStringLiteral("injected active-project marker commit failure")));
    QVERIFY(cleanupError.contains(
        QStringLiteral("injected reserved-project directory cleanup failure")));
    const QString retainedPath =
        root.filePath(QStringLiteral("projects/retained-new"));
    QVERIFY(QFileInfo(retainedPath).isDir());
    QCOMPARE(QString::fromUtf8(readBytes(markerPath)).trimmed(),
             QStringLiteral("alpha"));
    QCOMPARE(readBytes(alphaStatePath), alphaStateBytes);
    QVERIFY(QDir(retainedPath).removeRecursively());
}

void TestImGuiProjectHistory::saveAsCopiesHistoryWithoutChangingTheSource()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ScopedProjectRoot projectRoot(tempDir.path());
    QVERIFY(projectRoot.active());

    QDir root(tempDir.path());
    QVERIFY(root.mkpath(QStringLiteral("projects/alpha")));
    const json firstState = jcut::toLegacyStateJson(documentNamed("First"));
    const json currentState = jcut::toLegacyStateJson(documentNamed("Current"));
    const json sourceHistory = {
        {"index", 1},
        {"entries", json::array({
            qtCompatibleHistorySnapshot(firstState),
            qtCompatibleHistorySnapshot(currentState)})},
        {"retainedMetadata", {{"writer", "qt"}}}
    };
    const QString sourceStatePath =
        root.filePath(QStringLiteral("projects/alpha/state.json"));
    const QString sourceHistoryPath =
        root.filePath(QStringLiteral("projects/alpha/history.json"));
    QVERIFY(writeJson(sourceStatePath, currentState));
    QVERIFY(writeJson(sourceHistoryPath, sourceHistory));
    QVERIFY(writeBytes(
        root.filePath(QStringLiteral("projects/.current_project")), "alpha\n"));
    const QByteArray sourceStateBytes = readBytes(sourceStatePath);
    const QByteArray sourceHistoryBytes = readBytes(sourceHistoryPath);

    std::string error;
    const std::optional<jcut::ImGuiProjectSession> sourceSession =
        jcut::loadActiveImGuiProjectSession(&error);
    QVERIFY2(sourceSession.has_value(), error.c_str());
    const jcut::EditorDocumentCore dirtyDocument = documentNamed("Dirty Save As");
    const std::optional<jcut::ImGuiProjectSession> savedAs =
        jcut::saveImGuiProjectSessionAs(
            *sourceSession, dirtyDocument, "Client Cut", &error);
    QVERIFY2(savedAs.has_value(), error.c_str());
    QCOMPARE(QString::fromStdString(savedAs->projectId),
             QStringLiteral("client-cut"));
    QCOMPARE(readBytes(sourceStatePath), sourceStateBytes);
    QCOMPARE(readBytes(sourceHistoryPath), sourceHistoryBytes);
    QCOMPARE(
        QString::fromUtf8(readBytes(
            root.filePath(QStringLiteral("projects/.current_project")))).trimmed(),
        QStringLiteral("client-cut"));

    const std::optional<json> newState = readJson(
        root.filePath(QStringLiteral("projects/client-cut/state.json")));
    const std::optional<json> newHistory = readJson(
        root.filePath(QStringLiteral("projects/client-cut/history.json")));
    QVERIFY(newState.has_value());
    QVERIFY(newHistory.has_value());
    QCOMPARE(QString::fromStdString(
                 newState->at("projectName").get<std::string>()),
             QStringLiteral("Dirty Save As"));
    QCOMPARE(newHistory->at("index").get<int>(), 2);
    QCOMPARE(newHistory->at("entries").size(), std::size_t(3));
    QVERIFY(newHistory->at("entries").at(0) ==
            qtCompatibleHistorySnapshot(firstState));
    QVERIFY(newHistory->at("entries").at(1) ==
            qtCompatibleHistorySnapshot(currentState));
    QVERIFY(newHistory->at("entries").at(2) ==
            qtCompatibleHistorySnapshot(*newState));
    QCOMPARE(QString::fromStdString(
                 newHistory->at("retainedMetadata").at("writer").get<std::string>()),
             QStringLiteral("qt"));

    error.clear();
    const std::optional<jcut::ImGuiProjectSession> collision =
        jcut::saveImGuiProjectSessionAs(
            *savedAs, dirtyDocument, "Client Cut", &error);
    QVERIFY2(collision.has_value(), error.c_str());
    QCOMPARE(QString::fromStdString(collision->projectId),
             QStringLiteral("client-cut-2"));
    QCOMPARE(
        QString::fromUtf8(readBytes(
            root.filePath(QStringLiteral("projects/.current_project")))).trimmed(),
        QStringLiteral("client-cut-2"));
}

void TestImGuiProjectHistory::saveAsMarkerFailurePreservesSourceAndCleansDestination()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ScopedProjectRoot projectRoot(tempDir.path());
    QVERIFY(projectRoot.active());
    ScopedProjectIoFailures failures;

    QDir root(tempDir.path());
    QVERIFY(writeManagedProject(&root, QStringLiteral("alpha"),
                                documentNamed("Alpha")));
    const QString markerPath =
        root.filePath(QStringLiteral("projects/.current_project"));
    QVERIFY(writeBytes(markerPath, "alpha\n"));
    const QString statePath =
        root.filePath(QStringLiteral("projects/alpha/state.json"));
    const QString historyPath =
        root.filePath(QStringLiteral("projects/alpha/history.json"));
    const QByteArray stateBytes = readBytes(statePath);
    const QByteArray historyBytes = readBytes(historyPath);

    std::string error;
    const std::optional<jcut::ImGuiProjectSession> sourceSession =
        jcut::loadActiveImGuiProjectSession(&error);
    QVERIFY2(sourceSession.has_value(), error.c_str());

    failures.inject(
        jcut::testing::ImGuiProjectIoFailurePoint::ActiveProjectMarkerCommit);
    const std::optional<jcut::ImGuiProjectSession> failed =
        jcut::saveImGuiProjectSessionAs(
            *sourceSession,
            documentNamed("Unsaved Copy"),
            "Failed Copy",
            &error);
    QVERIFY(!failed.has_value());
    QVERIFY(QString::fromStdString(error).contains(
        QStringLiteral("injected active-project marker commit failure")));
    QVERIFY(!QFileInfo::exists(
        root.filePath(QStringLiteral("projects/failed-copy"))));
    QCOMPARE(QString::fromUtf8(readBytes(markerPath)).trimmed(),
             QStringLiteral("alpha"));
    QCOMPARE(readBytes(statePath), stateBytes);
    QCOMPARE(readBytes(historyPath), historyBytes);
}

void TestImGuiProjectHistory::renamePreservesFilesUpdatesMarkerAndHandlesCollisions()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ScopedProjectRoot projectRoot(tempDir.path());
    QVERIFY(projectRoot.active());

    QDir root(tempDir.path());
    QVERIFY(root.mkpath(QStringLiteral("projects/alpha")));
    QVERIFY(root.mkpath(QStringLiteral("projects/beta")));
    const json state = jcut::toLegacyStateJson(documentNamed("Rename Me"));
    const json history = {
        {"index", 0},
        {"entries", json::array({qtCompatibleHistorySnapshot(state)})}
    };
    const QString sourceStatePath =
        root.filePath(QStringLiteral("projects/alpha/state.json"));
    const QString sourceHistoryPath =
        root.filePath(QStringLiteral("projects/alpha/history.json"));
    QVERIFY(writeJson(sourceStatePath, state));
    QVERIFY(writeJson(sourceHistoryPath, history));
    QVERIFY(writeBytes(
        root.filePath(QStringLiteral("projects/.current_project")), "alpha\n"));
    const QByteArray stateBytes = readBytes(sourceStatePath);
    const QByteArray historyBytes = readBytes(sourceHistoryPath);

    std::string error;
    const std::optional<jcut::ImGuiProjectSession> sourceSession =
        jcut::loadActiveImGuiProjectSession(&error);
    QVERIFY2(sourceSession.has_value(), error.c_str());
    const std::optional<jcut::ImGuiProjectSession> unchanged =
        jcut::renameImGuiProjectSession(*sourceSession, "ALPHA", &error);
    QVERIFY2(unchanged.has_value(), error.c_str());
    QCOMPARE(QString::fromStdString(unchanged->projectId), QStringLiteral("alpha"));
    QVERIFY(QFileInfo::exists(sourceStatePath));
    QCOMPARE(readBytes(sourceStatePath), stateBytes);
    QCOMPARE(readBytes(sourceHistoryPath), historyBytes);

    const std::optional<jcut::ImGuiProjectSession> renamed =
        jcut::renameImGuiProjectSession(*unchanged, "Beta", &error);
    QVERIFY2(renamed.has_value(), error.c_str());
    QCOMPARE(QString::fromStdString(renamed->projectId),
             QStringLiteral("beta-2"));
    QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("projects/alpha"))));
    QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("projects/beta"))));
    QCOMPARE(readBytes(
                 root.filePath(QStringLiteral("projects/beta-2/state.json"))),
             stateBytes);
    QCOMPARE(readBytes(
                 root.filePath(QStringLiteral("projects/beta-2/history.json"))),
             historyBytes);
    QCOMPARE(
        QString::fromUtf8(readBytes(
            root.filePath(QStringLiteral("projects/.current_project")))).trimmed(),
        QStringLiteral("beta-2"));

    error.clear();
    QVERIFY(!jcut::renameImGuiProjectSession(
        *renamed, "../escape", &error).has_value());
    QVERIFY(QString::fromStdString(error).contains(QStringLiteral("traversal")));
    QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("projects/beta-2"))));
    QCOMPARE(
        QString::fromUtf8(readBytes(
            root.filePath(QStringLiteral("projects/.current_project")))).trimmed(),
        QStringLiteral("beta-2"));

    jcut::ImGuiProjectSession forged = *renamed;
    forged.projectId = "../beta-2";
    error.clear();
    QVERIFY(!jcut::renameImGuiProjectSession(
        forged, "Gamma", &error).has_value());
    QVERIFY(QString::fromStdString(error).contains(QStringLiteral("invalid")));
    QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("projects/beta-2"))));
}

void TestImGuiProjectHistory::renameFailuresPreserveOrRecoverAConsistentIdentity()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ScopedProjectRoot projectRoot(tempDir.path());
    QVERIFY(projectRoot.active());
    ScopedProjectIoFailures failures;

    QDir root(tempDir.path());
    QVERIFY(writeManagedProject(&root, QStringLiteral("alpha"),
                                documentNamed("Alpha")));
    const QString markerPath =
        root.filePath(QStringLiteral("projects/.current_project"));
    QVERIFY(writeBytes(markerPath, "alpha\n"));
    const QString sourceStatePath =
        root.filePath(QStringLiteral("projects/alpha/state.json"));
    const QString sourceHistoryPath =
        root.filePath(QStringLiteral("projects/alpha/history.json"));
    const QByteArray stateBytes = readBytes(sourceStatePath);
    const QByteArray historyBytes = readBytes(sourceHistoryPath);

    std::string error;
    const std::optional<jcut::ImGuiProjectSession> sourceSession =
        jcut::loadActiveImGuiProjectSession(&error);
    QVERIFY2(sourceSession.has_value(), error.c_str());

    failures.inject(
        jcut::testing::ImGuiProjectIoFailurePoint::ProjectDirectoryRename);
    QVERIFY(!jcut::renameImGuiProjectSession(
        *sourceSession, "Rename Commit Failure", &error).has_value());
    QVERIFY(QString::fromStdString(error).contains(
        QStringLiteral("failed to rename project directory")));
    QVERIFY(QFileInfo(
        root.filePath(QStringLiteral("projects/alpha"))).isDir());
    QVERIFY(!QFileInfo::exists(root.filePath(
        QStringLiteral("projects/rename-commit-failure"))));
    QCOMPARE(QString::fromUtf8(readBytes(markerPath)).trimmed(),
             QStringLiteral("alpha"));

    failures.inject(
        jcut::testing::ImGuiProjectIoFailurePoint::ActiveProjectMarkerCommit);
    error.clear();
    QVERIFY(!jcut::renameImGuiProjectSession(
        *sourceSession, "Marker Failure", &error).has_value());
    QVERIFY(QString::fromStdString(error).contains(
        QStringLiteral("injected active-project marker commit failure")));
    QVERIFY(QFileInfo(
        root.filePath(QStringLiteral("projects/alpha"))).isDir());
    QVERIFY(!QFileInfo::exists(
        root.filePath(QStringLiteral("projects/marker-failure"))));
    QCOMPARE(QString::fromUtf8(readBytes(markerPath)).trimmed(),
             QStringLiteral("alpha"));
    QCOMPARE(readBytes(sourceStatePath), stateBytes);
    QCOMPARE(readBytes(sourceHistoryPath), historyBytes);

    failures.inject(
        jcut::testing::ImGuiProjectIoFailurePoint::ActiveProjectMarkerCommit);
    failures.inject(
        jcut::testing::ImGuiProjectIoFailurePoint::
            ProjectDirectoryRenameRollback);
    failures.inject(
        jcut::testing::ImGuiProjectIoFailurePoint::
            RenamedProjectMarkerRecoveryCommit);
    error.clear();
    QVERIFY(!jcut::renameImGuiProjectSession(
        *sourceSession, "Restored Rename", &error).has_value());
    const QString restoredError = QString::fromStdString(error);
    QVERIFY(restoredError.contains(QStringLiteral(
        "injected renamed-project marker recovery commit failure")));
    QVERIFY(restoredError.contains(
        QStringLiteral("restored original project directory")));
    QVERIFY(QFileInfo(
        root.filePath(QStringLiteral("projects/alpha"))).isDir());
    QVERIFY(!QFileInfo::exists(
        root.filePath(QStringLiteral("projects/restored-rename"))));
    QCOMPARE(QString::fromUtf8(readBytes(markerPath)).trimmed(),
             QStringLiteral("alpha"));
    QCOMPARE(readBytes(sourceStatePath), stateBytes);
    QCOMPARE(readBytes(sourceHistoryPath), historyBytes);

    failures.inject(
        jcut::testing::ImGuiProjectIoFailurePoint::ActiveProjectMarkerCommit);
    failures.inject(
        jcut::testing::ImGuiProjectIoFailurePoint::
            ProjectDirectoryRenameRollback);
    error.clear();
    const std::optional<jcut::ImGuiProjectSession> recovered =
        jcut::renameImGuiProjectSession(
            *sourceSession, "Recovered Rename", &error);
    QVERIFY2(recovered.has_value(), error.c_str());
    QCOMPARE(QString::fromStdString(recovered->projectId),
             QStringLiteral("recovered-rename"));
    QVERIFY(!QFileInfo::exists(
        root.filePath(QStringLiteral("projects/alpha"))));
    const QString recoveredStatePath = root.filePath(
        QStringLiteral("projects/recovered-rename/state.json"));
    const QString recoveredHistoryPath = root.filePath(
        QStringLiteral("projects/recovered-rename/history.json"));
    QCOMPARE(readBytes(recoveredStatePath), stateBytes);
    QCOMPARE(readBytes(recoveredHistoryPath), historyBytes);
    QCOMPARE(QString::fromUtf8(readBytes(markerPath)).trimmed(),
             QStringLiteral("recovered-rename"));
}

void TestImGuiProjectHistory::activationMarkerFailureKeepsPreviousProjectActive()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ScopedProjectRoot projectRoot(tempDir.path());
    QVERIFY(projectRoot.active());
    ScopedProjectIoFailures failures;

    QDir root(tempDir.path());
    QVERIFY(writeManagedProject(&root, QStringLiteral("alpha"),
                                documentNamed("Alpha")));
    QVERIFY(writeManagedProject(&root, QStringLiteral("beta"),
                                documentNamed("Beta")));
    const QString markerPath =
        root.filePath(QStringLiteral("projects/.current_project"));
    QVERIFY(writeBytes(markerPath, "alpha\n"));
    const QString betaStatePath =
        root.filePath(QStringLiteral("projects/beta/state.json"));
    const QString betaHistoryPath =
        root.filePath(QStringLiteral("projects/beta/history.json"));
    const QByteArray betaStateBytes = readBytes(betaStatePath);
    const QByteArray betaHistoryBytes = readBytes(betaHistoryPath);

    failures.inject(
        jcut::testing::ImGuiProjectIoFailurePoint::ActiveProjectMarkerCommit);
    std::string error;
    const std::optional<jcut::ImGuiProjectSession> activated =
        jcut::activateImGuiProjectSession("beta", &error);
    QVERIFY(!activated.has_value());
    QVERIFY(QString::fromStdString(error).contains(
        QStringLiteral("injected active-project marker commit failure")));
    QCOMPARE(QString::fromUtf8(readBytes(markerPath)).trimmed(),
             QStringLiteral("alpha"));
    QCOMPARE(readBytes(betaStatePath), betaStateBytes);
    QCOMPARE(readBytes(betaHistoryPath), betaHistoryBytes);

    const std::optional<jcut::ImGuiProjectSession> active =
        jcut::loadActiveImGuiProjectSession(&error);
    QVERIFY2(active.has_value(), error.c_str());
    QCOMPARE(QString::fromStdString(active->projectId), QStringLiteral("alpha"));
}

void TestImGuiProjectHistory::committedProjectLoadFailureRestoresPreviousIdentity()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ScopedProjectRoot projectRoot(tempDir.path());
    QVERIFY(projectRoot.active());
    ScopedProjectIoFailures failures;

    QDir root(tempDir.path());
    QVERIFY(writeManagedProject(&root, QStringLiteral("alpha"),
                                documentNamed("Alpha")));
    const QString markerPath =
        root.filePath(QStringLiteral("projects/.current_project"));
    QVERIFY(writeBytes(markerPath, "alpha\n"));
    const QString alphaStatePath =
        root.filePath(QStringLiteral("projects/alpha/state.json"));
    const QString alphaHistoryPath =
        root.filePath(QStringLiteral("projects/alpha/history.json"));
    const QByteArray alphaStateBytes = readBytes(alphaStatePath);
    const QByteArray alphaHistoryBytes = readBytes(alphaHistoryPath);

    std::string error;
    const std::optional<jcut::ImGuiProjectSession> sourceSession =
        jcut::loadActiveImGuiProjectSession(&error);
    QVERIFY2(sourceSession.has_value(), error.c_str());

    failures.inject(
        jcut::testing::ImGuiProjectIoFailurePoint::ResultingProjectSessionLoad);
    QVERIFY(!jcut::createImGuiProjectSession(
        "Rolled Back New", &error).has_value());
    QVERIFY(QString::fromStdString(error).contains(
        QStringLiteral("injected resulting project session load failure")));
    QVERIFY(!QFileInfo::exists(
        root.filePath(QStringLiteral("projects/rolled-back-new"))));
    QCOMPARE(QString::fromUtf8(readBytes(markerPath)).trimmed(),
             QStringLiteral("alpha"));

    failures.inject(
        jcut::testing::ImGuiProjectIoFailurePoint::ResultingProjectSessionLoad);
    error.clear();
    QVERIFY(!jcut::saveImGuiProjectSessionAs(
        *sourceSession,
        documentNamed("Rolled Back Copy State"),
        "Rolled Back Copy",
        &error).has_value());
    QVERIFY(QString::fromStdString(error).contains(
        QStringLiteral("injected resulting project session load failure")));
    QVERIFY(!QFileInfo::exists(
        root.filePath(QStringLiteral("projects/rolled-back-copy"))));
    QCOMPARE(QString::fromUtf8(readBytes(markerPath)).trimmed(),
             QStringLiteral("alpha"));
    QCOMPARE(readBytes(alphaStatePath), alphaStateBytes);
    QCOMPARE(readBytes(alphaHistoryPath), alphaHistoryBytes);
}

void TestImGuiProjectHistory::managedSessionsRejectSymlinkEscapes()
{
    namespace fs = std::filesystem;

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ScopedProjectRoot projectRoot(tempDir.path());
    QVERIFY(projectRoot.active());

    QDir root(tempDir.path());
    QVERIFY(root.mkpath(QStringLiteral("projects")));
    QVERIFY(root.mkpath(QStringLiteral("outside-project")));
    const json state = jcut::toLegacyStateJson(documentNamed("Outside"));
    const json history = {
        {"index", 0},
        {"entries", json::array({qtCompatibleHistorySnapshot(state)})}
    };
    QVERIFY(writeJson(
        root.filePath(QStringLiteral("outside-project/state.json")), state));
    QVERIFY(writeJson(
        root.filePath(QStringLiteral("outside-project/history.json")), history));

    std::error_code symlinkError;
    fs::create_directory_symlink(
        fs::path(tempDir.path().toStdString()) / "outside-project",
        fs::path(tempDir.path().toStdString()) / "projects" / "escape",
        symlinkError);
    if (symlinkError) {
        QSKIP("filesystem does not permit symlink creation for this test");
    }
    QVERIFY(writeBytes(
        root.filePath(QStringLiteral("projects/.current_project")), "escape\n"));

    std::string error;
    QVERIFY(!jcut::activateImGuiProjectSession("escape", &error).has_value());
    QVERIFY(!error.empty());
    jcut::ImGuiProjectSession escapedSession;
    escapedSession.document = documentNamed("Outside");
    escapedSession.projectId = "escape";
    escapedSession.rootDirPath = tempDir.path().toStdString();
    escapedSession.statePath = root.filePath(
        QStringLiteral("projects/escape/state.json")).toStdString();
    escapedSession.historyPath = root.filePath(
        QStringLiteral("projects/escape/history.json")).toStdString();
    escapedSession.legacyStateRoot = state;
    error.clear();
    QVERIFY(!jcut::saveImGuiProjectSession(
        escapedSession, documentNamed("Must Not Escape"), &error));
    QVERIFY(QString::fromStdString(error).contains(
        QStringLiteral("symbolic link")));
    const std::optional<json> unchangedOutsideState = readJson(root.filePath(
        QStringLiteral("outside-project/state.json")));
    QVERIFY(unchangedOutsideState.has_value());
    QVERIFY(unchangedOutsideState->at("projectName") == state.at("projectName"));

    symlinkError.clear();
    QVERIFY(fs::remove(
        fs::path(tempDir.path().toStdString()) / "projects" / "escape",
        symlinkError));
    QVERIFY2(!symlinkError, symlinkError.message().c_str());
    QVERIFY(root.mkpath(QStringLiteral("projects/alpha")));
    const QString outsideStatePath =
        root.filePath(QStringLiteral("outside-state.json"));
    QVERIFY(writeJson(outsideStatePath, state));
    fs::create_symlink(
        fs::path(outsideStatePath.toStdString()),
        fs::path(tempDir.path().toStdString()) /
            "projects" / "alpha" / "state.json",
        symlinkError);
    QVERIFY2(!symlinkError, symlinkError.message().c_str());
    QVERIFY(writeJson(
        root.filePath(QStringLiteral("projects/alpha/history.json")), history));
    QVERIFY(writeBytes(
        root.filePath(QStringLiteral("projects/.current_project")), "alpha\n"));

    error.clear();
    QVERIFY(!jcut::loadActiveImGuiProjectSession(&error).has_value());
    QVERIFY(QString::fromStdString(error).contains(
        QStringLiteral("symbolic links")));
}

void TestImGuiProjectHistory::staleManagedSaveCannotRecreateProjectAfterRename()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ScopedProjectRoot projectRoot(tempDir.path());
    QVERIFY(projectRoot.active());

    QDir root(tempDir.path());
    QVERIFY(root.mkpath(QStringLiteral("projects/alpha")));
    const json state = jcut::toLegacyStateJson(documentNamed("Before Rename"));
    const json history = {
        {"index", 0},
        {"entries", json::array({qtCompatibleHistorySnapshot(state)})}
    };
    QVERIFY(writeJson(
        root.filePath(QStringLiteral("projects/alpha/state.json")), state));
    QVERIFY(writeJson(
        root.filePath(QStringLiteral("projects/alpha/history.json")), history));
    QVERIFY(writeBytes(
        root.filePath(QStringLiteral("projects/.current_project")), "alpha\n"));

    std::string error;
    const std::optional<jcut::ImGuiProjectSession> oldSession =
        jcut::loadActiveImGuiProjectSession(&error);
    QVERIFY2(oldSession.has_value(), error.c_str());
    const std::optional<jcut::ImGuiProjectSession> renamed =
        jcut::renameImGuiProjectSession(*oldSession, "Renamed", &error);
    QVERIFY2(renamed.has_value(), error.c_str());
    QCOMPARE(QString::fromStdString(renamed->projectId),
             QStringLiteral("renamed"));
    const QString renamedStatePath =
        root.filePath(QStringLiteral("projects/renamed/state.json"));
    const QByteArray renamedStateBytes = readBytes(renamedStatePath);

    error.clear();
    QVERIFY(!jcut::saveImGuiProjectSession(
        *oldSession, documentNamed("Stale Save"), &error));
    QVERIFY2(!error.empty(), "stale managed save did not report an error");
    QVERIFY(!QFileInfo::exists(
        root.filePath(QStringLiteral("projects/alpha"))));
    QCOMPARE(readBytes(renamedStatePath), renamedStateBytes);
    QCOMPARE(
        QString::fromUtf8(readBytes(
            root.filePath(QStringLiteral("projects/.current_project")))).trimmed(),
        QStringLiteral("renamed"));

    error.clear();
    QVERIFY2(jcut::saveImGuiProjectSession(
        *renamed, documentNamed("Current Save"), &error), error.c_str());
    const std::optional<json> savedState = readJson(renamedStatePath);
    QVERIFY(savedState.has_value());
    QCOMPARE(QString::fromStdString(
                 savedState->at("projectName").get<std::string>()),
             QStringLiteral("Current Save"));
}

void TestImGuiProjectHistory::legacyStateOverridesApplyAfterConcurrencyValidation()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString statePath = tempDir.filePath(QStringLiteral("state.json"));
    const QString historyPath = tempDir.filePath(QStringLiteral("history.json"));
    const json baseState = {
        {"projectName", "Transcript Project"},
        {"tracks", json::array({{{"name", "Video 1"}}})},
        {"timeline", json::array()},
        {"transcriptActiveCutPath", "/old/cut.json"},
        {"legacyKeep", {{"value", 17}}},
    };
    QVERIFY(writeJson(statePath, baseState));

    jcut::ImGuiProjectSession session =
        sessionFor(statePath, historyPath, baseState);
    session.legacyStateOverrides = {
        {"transcriptActiveCutPath", "/new/cut.json"},
        {"transcriptShowExcludedLines", true},
    };

    std::string error;
    QVERIFY2(jcut::saveImGuiProjectSession(
                 session, documentNamed("Transcript Project"), &error),
             error.c_str());
    const std::optional<json> savedState = readJson(statePath);
    const std::optional<json> savedHistory = readJson(historyPath);
    QVERIFY(savedState.has_value());
    QVERIFY(savedHistory.has_value());
    QCOMPARE(savedState->value("transcriptActiveCutPath", std::string{}),
             std::string("/new/cut.json"));
    QVERIFY(savedState->value("transcriptShowExcludedLines", false));
    QCOMPARE(savedState->at("legacyKeep").at("value").get<int>(), 17);
    QCOMPARE(savedHistory->at("entries").back().value(
                 "transcriptActiveCutPath", std::string{}),
             std::string("/new/cut.json"));
    QCOMPARE(session.legacyStateRoot, baseState);
}

void TestImGuiProjectHistory::autosaveWritesCompatibleSnapshotAndTrimsBackups()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString statePath = tempDir.filePath(QStringLiteral("state.json"));
    const QString historyPath = tempDir.filePath(QStringLiteral("history.json"));
    const json state = {
        {"projectName", "Autosave Original"},
        {"tracks", json::array({{{"name", "Video 1"}}})},
        {"timeline", json::array()},
        {"unknownFutureField", {{"keep", 42}}},
    };
    const json history = {
        {"index", 0},
        {"entries", json::array({state})},
    };
    QVERIFY(writeJson(statePath, state));
    QVERIFY(writeJson(historyPath, history));
    QVERIFY(writeJson(
        tempDir.filePath(
            QStringLiteral("state_backup_2020-01-01_00-00-00.json")),
        state));
    QVERIFY(writeJson(
        tempDir.filePath(
            QStringLiteral("state_backup_2021-01-01_00-00-00.json")),
        state));
    const QByteArray originalState = readBytes(statePath);
    const QByteArray originalHistory = readBytes(historyPath);

    jcut::ImGuiProjectSession session =
        sessionFor(statePath, historyPath, state);
    session.legacyStateOverrides = {
        {"autosaveIntervalMinutes", 3},
        {"autosaveMaxBackups", 2},
    };
    std::string backupPath;
    std::string error;
    QVERIFY2(
        jcut::writeImGuiProjectAutosave(
            session,
            documentNamed("Autosave Current"),
            2,
            &backupPath,
            &error),
        error.c_str());
    QVERIFY(QFileInfo::exists(QString::fromStdString(backupPath)));
    QCOMPARE(readBytes(statePath), originalState);
    QCOMPARE(readBytes(historyPath), originalHistory);

    const std::optional<json> backup =
        readJson(QString::fromStdString(backupPath));
    QVERIFY(backup.has_value());
    QCOMPARE(
        backup->value("projectName", std::string{}),
        std::string("Autosave Current"));
    QCOMPARE(
        backup->at("unknownFutureField").at("keep").get<int>(), 42);
    QCOMPARE(backup->value("autosaveIntervalMinutes", 0), 3);
    QCOMPARE(backup->value("autosaveMaxBackups", 0), 2);
    const QStringList backups = QDir(tempDir.path()).entryList(
        {QStringLiteral("state_backup_*.json")},
        QDir::Files,
        QDir::Name);
    QCOMPARE(backups.size(), 2);
    QVERIFY(!backups.contains(
        QStringLiteral("state_backup_2020-01-01_00-00-00.json")));
}

void TestImGuiProjectHistory::imguiShellWiresLifecycleActionsThroughSharedDirtyGuard()
{
    QFile source(QStringLiteral(JCUT_SOURCE_DIR "/jcut_imgui_main.cpp"));
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QString body = QString::fromUtf8(source.readAll());

    QVERIFY(body.contains(QStringLiteral("void requestProjectLifecycleAction(")));
    QVERIFY(body.contains(QStringLiteral("bool performProjectLifecycleAction(")));
    QVERIFY(body.contains(QStringLiteral("drawProjectLifecyclePopup(&shellState)")));
    QVERIFY(body.contains(QStringLiteral(
        "jcut::activateImGuiProjectSession(projectId, &error)")));
    QVERIFY(!body.contains(QStringLiteral("jcut::setActiveImGuiProject(")));
    QVERIFY(body.contains(QStringLiteral("ProjectLifecycleAction::NewProject")));
    QVERIFY(body.contains(QStringLiteral("ProjectLifecycleAction::SaveAs")));
    QVERIFY(body.contains(QStringLiteral("ProjectLifecycleAction::Rename")));
    QVERIFY(body.contains(QStringLiteral(
        "save changes before creating or renaming a project")));
    QVERIFY(body.contains(QStringLiteral(
        "jcut::listImGuiProjectHistoryEntries(")));
    QVERIFY(body.contains(QStringLiteral(
        "jcut::activateImGuiProjectHistoryEntry(")));
    QVERIFY(body.contains(QStringLiteral(
        "save changes before restoring project history")));
    QVERIFY(body.contains(QStringLiteral(
        "void invalidateProjectHistoryCache(ShellState* shellState)")));
    QVERIFY(body.contains(QStringLiteral(
        "setLegacyStateOverride(shellState, \"mediaRoot\", mediaRoot)")));
    QVERIFY(body.contains(QStringLiteral(
        "setLegacyStateOverride(shellState, \"explorerRoot\", mediaRoot)")));
    QVERIFY(body.contains(QStringLiteral(
        "{\"mediaRoot\", shellState.mediaRootDirectory}")));
    QVERIFY(body.contains(QStringLiteral(
        "media root changed; save the project to keep it")));
    const qsizetype adoptStart = body.indexOf(
        QStringLiteral("void adoptSavedProjectSession("));
    const qsizetype adoptEnd = body.indexOf(
        QStringLiteral("const char* projectLifecycleTitle("), adoptStart);
    QVERIFY(adoptStart >= 0);
    QVERIFY(adoptEnd > adoptStart);
    const QString adoptBody = body.mid(adoptStart, adoptEnd - adoptStart);
    QVERIFY(adoptBody.contains(QStringLiteral(
        "invalidateProjectHistoryCache(shellState);")));
    QVERIFY(body.contains(QStringLiteral("Save Project As")));
    QVERIFY(!body.contains(QStringLiteral("New (shared ProjectStore pending)")));
}

QTEST_MAIN(TestImGuiProjectHistory)
#include "test_imgui_project_history.moc"
