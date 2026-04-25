#include <QtTest/QtTest>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <limits>

#include "../editor_shared.h"
#include "../transcript_tab.h"

namespace {

constexpr int kTranscriptTestColumnCount = 5;
constexpr int kTranscriptTestTextColumn = 3;

bool writeTranscript(const QString& path, const QJsonArray& words) {
    QJsonObject segment;
    segment[QStringLiteral("words")] = words;
    QJsonArray segments;
    segments.push_back(segment);
    QJsonObject root;
    root[QStringLiteral("segments")] = segments;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    return file.write(json) == json.size();
}

QJsonObject word(const QString& text, double startSeconds, double endSeconds, bool skipped = false) {
    QJsonObject obj;
    obj[QStringLiteral("word")] = text;
    obj[QStringLiteral("start")] = startSeconds;
    obj[QStringLiteral("end")] = endSeconds;
    obj[QStringLiteral("skipped")] = skipped;
    return obj;
}

QJsonObject wordWithRenderOrder(const QString& text,
                                double startSeconds,
                                double endSeconds,
                                int renderOrder,
                                bool skipped = false) {
    QJsonObject obj = word(text, startSeconds, endSeconds, skipped);
    obj[QStringLiteral("render_order")] = renderOrder;
    return obj;
}

TimelineClip makeAudioClip(const QString& id, const QString& path) {
    TimelineClip clip;
    clip.id = id;
    clip.filePath = path;
    clip.label = QStringLiteral("clip");
    clip.mediaType = ClipMediaType::Audio;
    clip.hasAudio = true;
    clip.startFrame = 0;
    clip.durationFrames = 120;
    clip.sourceInFrame = 0;
    clip.sourceDurationFrames = 120;
    return clip;
}

int selectedRow(const QTableWidget& table) {
    if (!table.selectionModel()) {
        return -1;
    }
    const QModelIndexList rows = table.selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return -1;
    }
    return rows.constFirst().row();
}

int firstMatchRow(const QTableWidget& table, double sourceSeconds) {
    for (int row = 0; row < table.rowCount(); ++row) {
        const QTableWidgetItem* startItem = table.item(row, 0);
        if (!startItem) {
            continue;
        }
        const bool outsideCut = startItem->data(Qt::UserRole + 12).toBool();
        if (outsideCut) {
            continue;
        }
        const double startSeconds = startItem->data(Qt::UserRole).toDouble();
        const double endSeconds = startItem->data(Qt::UserRole + 1).toDouble();
        if (sourceSeconds >= startSeconds && sourceSeconds < endSeconds) {
            return row;
        }
    }
    return -1;
}

} // namespace

class TestTranscriptTabFollow : public QObject {
    Q_OBJECT

private slots:
    void init() {
        clearAllActiveTranscriptPaths();
    }

    void cleanup() {
        clearAllActiveTranscriptPaths();
    }

    void testContinuousAlignmentAcrossFrames();
    void testFollowWorksWhileTableHasFocus();
    void testManualSelectionHoldWhilePausedThenResumeOnPlaybackAdvance();
    void testFollowSkipsSkippedRowsAndClearsSelection();
    void testFollowUsesSourceTimesNotRenderTimes();
    void testFollowBridgesSmallGapsDuringFastPlayback();
    void testFollowBridgesSmallGapsDuringFastReversePlayback();
    void testOutsideCutRowsAreNotAutoSelected();
    void testDeleteCurrentTranscriptionRemovesSelectedVersion();
};

void TestTranscriptTabFollow::testContinuousAlignmentAcrossFrames() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    const QString editablePath = transcriptEditablePathForClipFile(clipPath);
    QJsonArray words;
    words.push_back(word(QStringLiteral("a"), 0.0, 0.10));
    words.push_back(word(QStringLiteral("b"), 0.10, 0.20));
    words.push_back(word(QStringLiteral("c"), 0.20, 0.30));
    QVERIFY(writeTranscript(editablePath, words));

    TimelineClip clip = makeAudioClip(QStringLiteral("clip-1"), clipPath);

    QLineEdit clipLabel;
    QLabel detailsLabel;
    QTableWidget table;
    table.setColumnCount(kTranscriptTestColumnCount);
    QCheckBox follow;
    follow.setChecked(true);
    QSpinBox prependSpin;
    prependSpin.setValue(0);
    QSpinBox postpendSpin;
    postpendSpin.setValue(0);
    QCheckBox speechEnabled;
    QSpinBox speechFade;

    TranscriptTab tab(
        TranscriptTab::Widgets{
            &clipLabel, &detailsLabel, &table,
            nullptr, nullptr, nullptr, nullptr, nullptr,
            &follow,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            &prependSpin, &postpendSpin, &speechEnabled, &speechFade,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr},
        TranscriptTab::Dependencies{
            [&clip]() -> const TimelineClip* { return &clip; },
            {},
            {},
            {},
            {},
            {},
            {},
            {}});
    tab.wire();
    tab.refresh();

    QVERIFY2(table.rowCount() >= 3, "Transcript table did not load expected rows");

    int64_t minFrame = std::numeric_limits<int64_t>::max();
    int64_t maxFrame = 0;
    for (int row = 0; row < table.rowCount(); ++row) {
        const QTableWidgetItem* item = table.item(row, 0);
        QVERIFY(item != nullptr);
        minFrame = qMin(minFrame, item->data(Qt::UserRole + 2).toLongLong());
        maxFrame = qMax(maxFrame, item->data(Qt::UserRole + 3).toLongLong());
    }

    for (int64_t frame = minFrame; frame <= maxFrame; ++frame) {
        const double sourceSeconds = (static_cast<double>(frame) + 0.5) / kTimelineFps;
        const int expectedRow = firstMatchRow(table, sourceSeconds);
        tab.syncTableToPlayhead(0, sourceSeconds);
        const int actualRow = selectedRow(table);
        QCOMPARE(actualRow, expectedRow);
    }
}

void TestTranscriptTabFollow::testFollowWorksWhileTableHasFocus() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    const QString editablePath = transcriptEditablePathForClipFile(clipPath);
    QJsonArray words;
    words.push_back(word(QStringLiteral("a"), 0.0, 0.10));
    words.push_back(word(QStringLiteral("b"), 0.10, 0.20));
    QVERIFY(writeTranscript(editablePath, words));

    TimelineClip clip = makeAudioClip(QStringLiteral("clip-2"), clipPath);

    QLineEdit clipLabel;
    QLabel detailsLabel;
    QTableWidget table;
    table.setColumnCount(kTranscriptTestColumnCount);
    table.show();
    QCheckBox follow;
    follow.setChecked(true);
    QSpinBox prependSpin;
    prependSpin.setValue(0);
    QSpinBox postpendSpin;
    postpendSpin.setValue(0);
    QCheckBox speechEnabled;
    QSpinBox speechFade;

    TranscriptTab tab(
        TranscriptTab::Widgets{
            &clipLabel, &detailsLabel, &table,
            nullptr, nullptr, nullptr, nullptr, nullptr,
            &follow,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            &prependSpin, &postpendSpin, &speechEnabled, &speechFade,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr},
        TranscriptTab::Dependencies{
            [&clip]() -> const TimelineClip* { return &clip; },
            {},
            {},
            {},
            {},
            {},
            {},
            {}});
    tab.wire();
    tab.refresh();
    QVERIFY(table.rowCount() >= 2);

    table.setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();

    const QTableWidgetItem* targetItem = table.item(1, 0);
    QVERIFY(targetItem != nullptr);
    const double targetSeconds = targetItem->data(Qt::UserRole).toDouble();

    tab.syncTableToPlayhead(0, targetSeconds);
    QCOMPARE(selectedRow(table), 1);
}

void TestTranscriptTabFollow::testManualSelectionHoldWhilePausedThenResumeOnPlaybackAdvance() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    const QString editablePath = transcriptEditablePathForClipFile(clipPath);
    QJsonArray words;
    words.push_back(word(QStringLiteral("a"), 0.0, 0.10));
    words.push_back(word(QStringLiteral("b"), 0.10, 0.20));
    QVERIFY(writeTranscript(editablePath, words));

    TimelineClip clip = makeAudioClip(QStringLiteral("clip-3"), clipPath);

    QLineEdit clipLabel;
    QLabel detailsLabel;
    QTableWidget table;
    table.setColumnCount(kTranscriptTestColumnCount);
    table.show();
    QCheckBox follow;
    follow.setChecked(true);
    QSpinBox prependSpin;
    prependSpin.setValue(0);
    QSpinBox postpendSpin;
    postpendSpin.setValue(0);
    QCheckBox speechEnabled;
    QSpinBox speechFade;

    TranscriptTab tab(
        TranscriptTab::Widgets{
            &clipLabel, &detailsLabel, &table,
            nullptr, nullptr, nullptr, nullptr, nullptr,
            &follow,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            &prependSpin, &postpendSpin, &speechEnabled, &speechFade,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr},
        TranscriptTab::Dependencies{
            [&clip]() -> const TimelineClip* { return &clip; },
            {},
            {},
            {},
            {},
            {},
            {},
            {}});
    tab.wire();
    tab.refresh();
    QVERIFY(table.rowCount() >= 2);

    table.selectRow(0);
    QCoreApplication::processEvents();
    QCOMPARE(selectedRow(table), 0);

    const double row1Seconds = table.item(1, 0)->data(Qt::UserRole).toDouble();
    tab.syncTableToPlayhead(100, row1Seconds);
    QCOMPARE(selectedRow(table), 0);

    // Simulate playback progress: once sample advances, follow should resume
    // immediately without waiting for the manual-selection hold timeout.
    tab.syncTableToPlayhead(101, row1Seconds);
    QCOMPARE(selectedRow(table), 1);
}

void TestTranscriptTabFollow::testFollowSkipsSkippedRowsAndClearsSelection() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    const QString editablePath = transcriptEditablePathForClipFile(clipPath);
    QJsonArray words;
    words.push_back(word(QStringLiteral("a"), 0.0, 0.10, false));
    words.push_back(word(QStringLiteral("b"), 0.10, 0.20, true));
    words.push_back(word(QStringLiteral("c"), 0.20, 0.30, false));
    QVERIFY(writeTranscript(editablePath, words));

    TimelineClip clip = makeAudioClip(QStringLiteral("clip-skip"), clipPath);

    QLineEdit clipLabel;
    QLabel detailsLabel;
    QTableWidget table;
    table.setColumnCount(kTranscriptTestColumnCount);
    QCheckBox follow;
    follow.setChecked(true);
    QSpinBox prependSpin;
    prependSpin.setValue(0);
    QSpinBox postpendSpin;
    postpendSpin.setValue(0);
    QCheckBox speechEnabled;
    QSpinBox speechFade;

    TranscriptTab tab(
        TranscriptTab::Widgets{
            &clipLabel, &detailsLabel, &table,
            nullptr, nullptr, nullptr, nullptr, nullptr,
            &follow,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            &prependSpin, &postpendSpin, &speechEnabled, &speechFade,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr},
        TranscriptTab::Dependencies{
            [&clip]() -> const TimelineClip* { return &clip; },
            {},
            {},
            {},
            {},
            {},
            {},
            {}});
    tab.wire();
    tab.refresh();
    QVERIFY(table.rowCount() >= 3);

    int skippedRow = -1;
    int nonSkippedRow = -1;
    double nonSkippedSeconds = -1.0;
    double skippedSeconds = -1.0;
    for (int row = 0; row < table.rowCount(); ++row) {
        const QTableWidgetItem* item = table.item(row, 0);
        QVERIFY(item != nullptr);
        const bool isSkipped = item->data(Qt::UserRole + 7).toBool();
        if (isSkipped) {
            skippedRow = row;
            skippedSeconds = item->data(Qt::UserRole).toDouble();
        } else if (nonSkippedRow < 0) {
            nonSkippedRow = row;
            nonSkippedSeconds = item->data(Qt::UserRole).toDouble();
        }
    }

    QVERIFY(skippedRow >= 0);
    QVERIFY(nonSkippedRow >= 0);
    QVERIFY(skippedSeconds >= 0.0);
    QVERIFY(nonSkippedSeconds >= 0.0);

    tab.syncTableToPlayhead(0, nonSkippedSeconds);
    QCOMPARE(selectedRow(table), nonSkippedRow);

    tab.syncTableToPlayhead(0, skippedSeconds);
    QCOMPARE(selectedRow(table), -1);

    tab.syncTableToPlayhead(0, nonSkippedSeconds);
    QCOMPARE(selectedRow(table), nonSkippedRow);
}

void TestTranscriptTabFollow::testFollowUsesSourceTimesNotRenderTimes() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    const QString editablePath = transcriptEditablePathForClipFile(clipPath);
    QJsonArray words;
    // Reorder render timeline so render start/end diverge from source start/end.
    words.push_back(wordWithRenderOrder(QStringLiteral("a"), 0.0, 0.10, 1));
    words.push_back(wordWithRenderOrder(QStringLiteral("b"), 1.0, 1.10, 0));
    QVERIFY(writeTranscript(editablePath, words));

    TimelineClip clip = makeAudioClip(QStringLiteral("clip-source-vs-render"), clipPath);

    QLineEdit clipLabel;
    QLabel detailsLabel;
    QTableWidget table;
    table.setColumnCount(kTranscriptTestColumnCount);
    QCheckBox follow;
    follow.setChecked(true);
    QSpinBox prependSpin;
    prependSpin.setValue(0);
    QSpinBox postpendSpin;
    postpendSpin.setValue(0);
    QCheckBox speechEnabled;
    QSpinBox speechFade;

    TranscriptTab tab(
        TranscriptTab::Widgets{
            &clipLabel, &detailsLabel, &table,
            nullptr, nullptr, nullptr, nullptr, nullptr,
            &follow,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            &prependSpin, &postpendSpin, &speechEnabled, &speechFade,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr},
        TranscriptTab::Dependencies{
            [&clip]() -> const TimelineClip* { return &clip; },
            {},
            {},
            {},
            {},
            {},
            {},
            {}});
    tab.wire();
    tab.refresh();
    QCOMPARE(table.rowCount(), 2);

    int rowA = -1;
    double sourceSecondsA = -1.0;
    for (int row = 0; row < table.rowCount(); ++row) {
        const QTableWidgetItem* sourceItem = table.item(row, 0);
        QVERIFY(sourceItem != nullptr);
        const QString text = table.item(row, kTranscriptTestTextColumn)
            ? table.item(row, kTranscriptTestTextColumn)->text()
            : QString();
        if (text == QStringLiteral("a")) {
            rowA = row;
            sourceSecondsA = sourceItem->data(Qt::UserRole).toDouble();
            break;
        }
    }
    QVERIFY(rowA >= 0);
    QVERIFY(sourceSecondsA >= 0.0);

    tab.syncTableToPlayhead(0, sourceSecondsA);
    QCOMPARE(selectedRow(table), rowA);
}

void TestTranscriptTabFollow::testFollowBridgesSmallGapsDuringFastPlayback() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    const QString editablePath = transcriptEditablePathForClipFile(clipPath);
    QJsonArray words;
    // Intentional micro-gaps emulate step-over at higher playback rates.
    words.push_back(word(QStringLiteral("a"), 0.000, 0.040));
    words.push_back(word(QStringLiteral("b"), 0.050, 0.090));
    words.push_back(word(QStringLiteral("c"), 0.100, 0.140));
    QVERIFY(writeTranscript(editablePath, words));

    TimelineClip clip = makeAudioClip(QStringLiteral("clip-fast-follow"), clipPath);

    QLineEdit clipLabel;
    QLabel detailsLabel;
    QTableWidget table;
    table.setColumnCount(kTranscriptTestColumnCount);
    QCheckBox follow;
    follow.setChecked(true);
    QSpinBox prependSpin;
    prependSpin.setValue(0);
    QSpinBox postpendSpin;
    postpendSpin.setValue(0);
    QCheckBox speechEnabled;
    QSpinBox speechFade;

    TranscriptTab tab(
        TranscriptTab::Widgets{
            &clipLabel, &detailsLabel, &table,
            nullptr, nullptr, nullptr, nullptr, nullptr,
            &follow,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            &prependSpin, &postpendSpin, &speechEnabled, &speechFade,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr},
        TranscriptTab::Dependencies{
            [&clip]() -> const TimelineClip* { return &clip; },
            {},
            {},
            {},
            {},
            {},
            {},
            {}});
    tab.wire();
    tab.refresh();
    QVERIFY(table.rowCount() >= 3);

    int rowA = -1;
    int rowB = -1;
    int rowC = -1;
    for (int row = 0; row < table.rowCount(); ++row) {
        const QString text = table.item(row, kTranscriptTestTextColumn)
                                 ? table.item(row, kTranscriptTestTextColumn)->text()
                                 : QString();
        if (text == QStringLiteral("a")) rowA = row;
        if (text == QStringLiteral("b")) rowB = row;
        if (text == QStringLiteral("c")) rowC = row;
    }
    QVERIFY(rowA >= 0);
    QVERIFY(rowB >= 0);
    QVERIFY(rowC >= 0);

    tab.syncTableToPlayhead(100, 0.038);
    QCOMPARE(selectedRow(table), rowA);

    // Forward playhead now lands in the tiny a->b gap; follow should advance to b.
    tab.syncTableToPlayhead(101, 0.046);
    QCOMPARE(selectedRow(table), rowB);

    // Same behavior for b->c gap.
    tab.syncTableToPlayhead(102, 0.096);
    QCOMPARE(selectedRow(table), rowC);
}

void TestTranscriptTabFollow::testFollowBridgesSmallGapsDuringFastReversePlayback() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    const QString editablePath = transcriptEditablePathForClipFile(clipPath);
    QJsonArray words;
    words.push_back(word(QStringLiteral("a"), 0.000, 0.040));
    words.push_back(word(QStringLiteral("b"), 0.050, 0.090));
    words.push_back(word(QStringLiteral("c"), 0.100, 0.140));
    QVERIFY(writeTranscript(editablePath, words));

    TimelineClip clip = makeAudioClip(QStringLiteral("clip-fast-reverse-follow"), clipPath);

    QLineEdit clipLabel;
    QLabel detailsLabel;
    QTableWidget table;
    table.setColumnCount(kTranscriptTestColumnCount);
    QCheckBox follow;
    follow.setChecked(true);
    QSpinBox prependSpin;
    prependSpin.setValue(0);
    QSpinBox postpendSpin;
    postpendSpin.setValue(0);
    QCheckBox speechEnabled;
    QSpinBox speechFade;

    TranscriptTab tab(
        TranscriptTab::Widgets{
            &clipLabel, &detailsLabel, &table,
            nullptr, nullptr, nullptr, nullptr, nullptr,
            &follow,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            &prependSpin, &postpendSpin, &speechEnabled, &speechFade,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr},
        TranscriptTab::Dependencies{
            [&clip]() -> const TimelineClip* { return &clip; },
            {},
            {},
            {},
            {},
            {},
            {},
            {}});
    tab.wire();
    tab.refresh();
    QVERIFY(table.rowCount() >= 3);

    int rowB = -1;
    int rowC = -1;
    for (int row = 0; row < table.rowCount(); ++row) {
        const QString text = table.item(row, kTranscriptTestTextColumn)
                                 ? table.item(row, kTranscriptTestTextColumn)->text()
                                 : QString();
        if (text == QStringLiteral("b")) rowB = row;
        if (text == QStringLiteral("c")) rowC = row;
    }
    QVERIFY(rowB >= 0);
    QVERIFY(rowC >= 0);

    tab.syncTableToPlayhead(300, 0.112);
    QCOMPARE(selectedRow(table), rowC);

    // Reverse playhead now lands in c->b gap; follow should move back to b.
    tab.syncTableToPlayhead(299, 0.094);
    QCOMPARE(selectedRow(table), rowB);
}

void TestTranscriptTabFollow::testOutsideCutRowsAreNotAutoSelected() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    const QString originalPath = transcriptPathForClipFile(clipPath);
    const QString editablePath = transcriptEditablePathForClipFile(clipPath);

    QJsonArray originalWords;
    originalWords.push_back(word(QStringLiteral("keep"), 0.0, 0.10));
    originalWords.push_back(word(QStringLiteral("removed"), 0.40, 0.50));
    QVERIFY(writeTranscript(originalPath, originalWords));

    QJsonArray editableWords;
    editableWords.push_back(word(QStringLiteral("keep"), 0.0, 0.10));
    QVERIFY(writeTranscript(editablePath, editableWords));

    TimelineClip clip = makeAudioClip(QStringLiteral("clip-4"), clipPath);

    QLineEdit clipLabel;
    QLabel detailsLabel;
    QTableWidget table;
    table.setColumnCount(kTranscriptTestColumnCount);
    QCheckBox follow;
    follow.setChecked(true);
    QSpinBox prependSpin;
    prependSpin.setValue(0);
    QSpinBox postpendSpin;
    postpendSpin.setValue(0);
    QCheckBox speechEnabled;
    QSpinBox speechFade;
    QCheckBox showOutsideCutRows;
    showOutsideCutRows.setChecked(true);

    TranscriptTab tab(
        TranscriptTab::Widgets{
            &clipLabel, &detailsLabel, &table,
            nullptr, nullptr, nullptr, nullptr, nullptr,
            &follow,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            &prependSpin, &postpendSpin, &speechEnabled, &speechFade,
            nullptr, nullptr, nullptr, nullptr, nullptr, &showOutsideCutRows,
            nullptr, nullptr, nullptr},
        TranscriptTab::Dependencies{
            [&clip]() -> const TimelineClip* { return &clip; },
            {},
            {},
            {},
            {},
            {},
            {},
            {}});
    tab.wire();
    tab.refresh();

    int outsideRow = -1;
    double outsideSeconds = -1.0;
    for (int row = 0; row < table.rowCount(); ++row) {
        const QTableWidgetItem* item = table.item(row, 0);
        QVERIFY(item != nullptr);
        if (item->data(Qt::UserRole + 12).toBool()) {
            outsideRow = row;
            outsideSeconds = item->data(Qt::UserRole).toDouble();
            break;
        }
    }

    QVERIFY2(outsideRow >= 0, "Expected an outside-cut transcript row but none was found");
    tab.syncTableToPlayhead(0, outsideSeconds);
    QCOMPARE(selectedRow(table), -1);
}

void TestTranscriptTabFollow::testDeleteCurrentTranscriptionRemovesSelectedVersion() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    const QString originalPath = transcriptPathForClipFile(clipPath);
    const QString editablePath = transcriptEditablePathForClipFile(clipPath);

    QJsonArray words;
    words.push_back(word(QStringLiteral("a"), 0.0, 0.10));
    words.push_back(word(QStringLiteral("b"), 0.10, 0.20));
    QVERIFY(writeTranscript(originalPath, words));
    QVERIFY(writeTranscript(editablePath, words));

    TimelineClip clip = makeAudioClip(QStringLiteral("clip-5"), clipPath);

    QLineEdit clipLabel;
    QLabel detailsLabel;
    QTableWidget table;
    table.setColumnCount(kTranscriptTestColumnCount);
    QCheckBox follow;
    follow.setChecked(true);
    QSpinBox prependSpin;
    prependSpin.setValue(0);
    QSpinBox postpendSpin;
    postpendSpin.setValue(0);
    QCheckBox speechEnabled;
    QSpinBox speechFade;
    QComboBox scriptVersions;
    scriptVersions.setEditable(true);
    QPushButton newVersionButton;
    QPushButton deleteVersionButton;

    TranscriptTab tab(
        TranscriptTab::Widgets{
            &clipLabel, &detailsLabel, &table,
            nullptr, nullptr, nullptr, nullptr, nullptr,
            &follow,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            &prependSpin, &postpendSpin, &speechEnabled, &speechFade,
            nullptr, nullptr, &scriptVersions, &newVersionButton, &deleteVersionButton, nullptr,
            nullptr, nullptr, nullptr},
        TranscriptTab::Dependencies{
            [&clip]() -> const TimelineClip* { return &clip; },
            {},
            {},
            {},
            {},
            {},
            {},
            {}});
    tab.wire();
    tab.refresh();

    QCOMPARE(scriptVersions.currentData().toString(), editablePath);

    QVERIFY(QMetaObject::invokeMethod(&tab, "onTranscriptCreateVersion", Qt::DirectConnection));
    const QString createdPath = scriptVersions.currentData().toString();
    QVERIFY(!createdPath.isEmpty());
    QVERIFY(createdPath != editablePath);
    QVERIFY(QFileInfo::exists(createdPath));

    QVERIFY(QMetaObject::invokeMethod(&tab, "onTranscriptDeleteVersion", Qt::DirectConnection));
    QVERIFY(!QFileInfo::exists(createdPath));
    QCOMPARE(scriptVersions.currentData().toString(), editablePath);
}

QTEST_MAIN(TestTranscriptTabFollow)
#include "test_transcript_tab_follow.moc"
