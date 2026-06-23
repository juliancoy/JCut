#include <QtTest/QtTest>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QHeaderView>
#include <QComboBox>
#include <QPushButton>
#include <QDoubleSpinBox>
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

bool writeActiveEditableTranscript(const QString& clipPath, const QJsonArray& words) {
    const QString editablePath = transcriptEditablePathForClipFile(clipPath);
    if (!writeTranscript(editablePath, words)) {
        return false;
    }
    setActiveTranscriptPathForClipFile(clipPath, editablePath);
    invalidateTranscriptJsonCache(editablePath);
    invalidateTranscriptSpeakerProfileCache(editablePath);
    return true;
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

bool tableHasOutsideCutRow(const QTableWidget& table)
{
    for (int row = 0; row < table.rowCount(); ++row) {
        const QTableWidgetItem* item = table.item(row, 0);
        if (item && item->data(Qt::UserRole + 12).toBool()) {
            return true;
        }
    }
    return false;
}

TranscriptTab::Widgets makeTranscriptWidgets(QLineEdit* clipLabel,
                                             QLabel* detailsLabel,
                                             QTableWidget* table,
                                             QCheckBox* follow,
                                             QSpinBox* prependSpin,
                                             QSpinBox* postpendSpin,
                                             QCheckBox* speechEnabled,
                                             QSpinBox* speechFade)
{
    TranscriptTab::Widgets widgets;
    widgets.transcriptInspectorClipLabel = clipLabel;
    widgets.transcriptInspectorDetailsLabel = detailsLabel;
    widgets.transcriptTable = table;
    widgets.transcriptFollowCurrentWordCheckBox = follow;
    widgets.transcriptPrependMsSpin = prependSpin;
    widgets.transcriptPostpendMsSpin = postpendSpin;
    widgets.speechFilterEnabledCheckBox = speechEnabled;
    widgets.speechFilterFadeSamplesSpin = speechFade;
    return widgets;
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
    void testPauseRefreshRestoresFollowSelectedRow();
    void testFollowSkipsSkippedRowsAndClearsSelection();
    void testFollowUsesSourceTimesNotRenderTimes();
    void testFollowBridgesSmallGapsDuringFastPlayback();
    void testFollowBridgesSmallGapsDuringFastReversePlayback();
    void testOutsideCutRowsAreNotAutoSelected();
    void testTranscriptTableUsesStableRowGeometryForMouseActivation();
    void testOverlayTransformEditsUpdatePreviewImmediately();
    void testDeleteCurrentTranscriptionRemovesSelectedVersion();
};

void TestTranscriptTabFollow::testContinuousAlignmentAcrossFrames() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    QJsonArray words;
    words.push_back(word(QStringLiteral("a"), 0.0, 0.10));
    words.push_back(word(QStringLiteral("b"), 0.10, 0.20));
    words.push_back(word(QStringLiteral("c"), 0.20, 0.30));
    QVERIFY(writeActiveEditableTranscript(clipPath, words));

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
        makeTranscriptWidgets(&clipLabel, &detailsLabel, &table, &follow, &prependSpin, &postpendSpin, &speechEnabled, &speechFade),
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
    QTRY_VERIFY_WITH_TIMEOUT(table.rowCount() >= 3, 2000);

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

    QJsonArray words;
    words.push_back(word(QStringLiteral("a"), 0.0, 0.10));
    words.push_back(word(QStringLiteral("b"), 0.10, 0.20));
    QVERIFY(writeActiveEditableTranscript(clipPath, words));

    TimelineClip clip = makeAudioClip(QStringLiteral("clip-2"), clipPath);

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
        makeTranscriptWidgets(&clipLabel, &detailsLabel, &table, &follow, &prependSpin, &postpendSpin, &speechEnabled, &speechFade),
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
    QTRY_VERIFY_WITH_TIMEOUT(table.rowCount() >= 2, 2000);
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

    QJsonArray words;
    words.push_back(word(QStringLiteral("a"), 0.0, 0.10));
    words.push_back(word(QStringLiteral("b"), 0.10, 0.20));
    QVERIFY(writeActiveEditableTranscript(clipPath, words));

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
        makeTranscriptWidgets(&clipLabel, &detailsLabel, &table, &follow, &prependSpin, &postpendSpin, &speechEnabled, &speechFade),
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
    QTRY_VERIFY_WITH_TIMEOUT(table.rowCount() >= 2, 2000);
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

void TestTranscriptTabFollow::testPauseRefreshRestoresFollowSelectedRow() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    QJsonArray words;
    for (int i = 0; i < 50; ++i) {
        const double start = static_cast<double>(i) * 0.10;
        words.push_back(word(QStringLiteral("word%1").arg(i), start, start + 0.10));
    }
    QVERIFY(writeActiveEditableTranscript(clipPath, words));

    TimelineClip clip = makeAudioClip(QStringLiteral("clip-pause-refresh"), clipPath);
    clip.durationFrames = 300;
    clip.sourceDurationFrames = 300;

    QLineEdit clipLabel;
    QLabel detailsLabel;
    QTableWidget table;
    table.setColumnCount(kTranscriptTestColumnCount);
    table.resize(320, 120);
    table.show();
    QCheckBox follow;
    follow.setChecked(true);
    QSpinBox prependSpin;
    prependSpin.setValue(0);
    QSpinBox postpendSpin;
    postpendSpin.setValue(0);
    QCheckBox speechEnabled;
    QSpinBox speechFade;

    bool playbackActive = false;
    TranscriptTab tab(
        makeTranscriptWidgets(&clipLabel, &detailsLabel, &table, &follow, &prependSpin, &postpendSpin, &speechEnabled, &speechFade),
        TranscriptTab::Dependencies{
            [&clip]() -> const TimelineClip* { return &clip; },
            {},
            {},
            {},
            {},
            {},
            {},
            {},
            [&playbackActive]() { return playbackActive; }});
    tab.wire();
    tab.refresh();
    constexpr double kTargetSeconds = 1.55;
    QTRY_VERIFY_WITH_TIMEOUT(firstMatchRow(table, kTargetSeconds) >= 0, 2000);

    const int targetRow = firstMatchRow(table, kTargetSeconds);
    QVERIFY(targetRow > 10);
    tab.syncTableToPlayhead(100, kTargetSeconds);
    QCOMPARE(selectedRow(table), targetRow);

    table.selectRow(0);
    table.scrollToTop();
    QCoreApplication::processEvents();
    QCOMPARE(selectedRow(table), 0);
    QCOMPARE(table.rowAt(0), 0);

    playbackActive = false;
    tab.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(firstMatchRow(table, kTargetSeconds) >= 0, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(selectedRow(table), targetRow, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(table.rowAt(0) > 0, 2000);
}

void TestTranscriptTabFollow::testFollowSkipsSkippedRowsAndClearsSelection() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    QJsonArray words;
    words.push_back(word(QStringLiteral("a"), 0.0, 0.10, false));
    words.push_back(word(QStringLiteral("b"), 0.10, 0.20, true));
    words.push_back(word(QStringLiteral("c"), 0.20, 0.30, false));
    QVERIFY(writeActiveEditableTranscript(clipPath, words));

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
        makeTranscriptWidgets(&clipLabel, &detailsLabel, &table, &follow, &prependSpin, &postpendSpin, &speechEnabled, &speechFade),
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
    QTRY_VERIFY_WITH_TIMEOUT(table.rowCount() >= 3, 2000);
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

    QJsonArray words;
    // Reorder render timeline so render start/end diverge from source start/end.
    words.push_back(wordWithRenderOrder(QStringLiteral("a"), 0.0, 0.10, 1));
    words.push_back(wordWithRenderOrder(QStringLiteral("b"), 1.0, 1.10, 0));
    QVERIFY(writeActiveEditableTranscript(clipPath, words));

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
        makeTranscriptWidgets(&clipLabel, &detailsLabel, &table, &follow, &prependSpin, &postpendSpin, &speechEnabled, &speechFade),
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
    QTRY_COMPARE_WITH_TIMEOUT(table.rowCount(), 2, 2000);
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

    QJsonArray words;
    // Intentional micro-gaps emulate step-over at higher playback rates.
    words.push_back(word(QStringLiteral("a"), 0.000, 0.040));
    words.push_back(word(QStringLiteral("b"), 0.050, 0.090));
    words.push_back(word(QStringLiteral("c"), 0.100, 0.140));
    QVERIFY(writeActiveEditableTranscript(clipPath, words));

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
        makeTranscriptWidgets(&clipLabel, &detailsLabel, &table, &follow, &prependSpin, &postpendSpin, &speechEnabled, &speechFade),
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
    QTRY_VERIFY_WITH_TIMEOUT(table.rowCount() >= 3, 2000);
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

    QJsonArray words;
    words.push_back(word(QStringLiteral("a"), 0.000, 0.040));
    words.push_back(word(QStringLiteral("b"), 0.050, 0.090));
    words.push_back(word(QStringLiteral("c"), 0.100, 0.140));
    QVERIFY(writeActiveEditableTranscript(clipPath, words));

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
        makeTranscriptWidgets(&clipLabel, &detailsLabel, &table, &follow, &prependSpin, &postpendSpin, &speechEnabled, &speechFade),
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
    QTRY_VERIFY_WITH_TIMEOUT(table.rowCount() >= 3, 2000);
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

    QJsonArray originalWords;
    originalWords.push_back(word(QStringLiteral("keep"), 0.0, 0.10));
    originalWords.push_back(word(QStringLiteral("removed"), 0.40, 0.50));
    QVERIFY(writeTranscript(originalPath, originalWords));

    QJsonArray editableWords;
    editableWords.push_back(word(QStringLiteral("keep"), 0.0, 0.10));
    QVERIFY(writeActiveEditableTranscript(clipPath, editableWords));

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

    auto widgets = makeTranscriptWidgets(
        &clipLabel, &detailsLabel, &table, &follow, &prependSpin, &postpendSpin, &speechEnabled, &speechFade);
    widgets.transcriptShowExcludedLinesCheckBox = &showOutsideCutRows;

    TranscriptTab tab(
        widgets,
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
    QTRY_VERIFY_WITH_TIMEOUT(tableHasOutsideCutRow(table), 2000);

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

void TestTranscriptTabFollow::testTranscriptTableUsesStableRowGeometryForMouseActivation() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString clipPath = dir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(QFile(clipPath).open(QIODevice::WriteOnly));

    QJsonArray words;
    words.push_back(word(QStringLiteral("first"), 0.0, 0.10));
    words.push_back(word(QStringLiteral("second row with deliberately long text that must not wrap and change row height"), 0.10, 0.20));
    words.push_back(word(QStringLiteral("third"), 0.20, 0.30));
    QVERIFY(writeActiveEditableTranscript(clipPath, words));

    TimelineClip clip = makeAudioClip(QStringLiteral("clip-table-geometry"), clipPath);

    QLineEdit clipLabel;
    QLabel detailsLabel;
    QTableWidget table;
    table.setColumnCount(kTranscriptTestColumnCount);
    table.resize(640, 180);
    QCheckBox follow;
    follow.setChecked(false);
    QSpinBox prependSpin;
    prependSpin.setValue(0);
    QSpinBox postpendSpin;
    postpendSpin.setValue(0);
    QCheckBox speechEnabled;
    QSpinBox speechFade;

    int64_t lastSeekFrame = -1;
    TranscriptTab tab(
        makeTranscriptWidgets(&clipLabel, &detailsLabel, &table, &follow, &prependSpin, &postpendSpin, &speechEnabled, &speechFade),
        TranscriptTab::Dependencies{
            [&clip]() -> const TimelineClip* { return &clip; },
            {},
            {},
            {},
            {},
            {},
            {},
            [&lastSeekFrame](int64_t frame) { lastSeekFrame = frame; },
            []() { return false; }});
    tab.wire();
    tab.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(table.rowCount() >= 3, 2000);

    QVERIFY(!table.wordWrap());
    QCOMPARE(table.textElideMode(), Qt::ElideRight);
    QCOMPARE(table.selectionBehavior(), QAbstractItemView::SelectRows);
    QCOMPARE(table.verticalScrollMode(), QAbstractItemView::ScrollPerPixel);
    QVERIFY(table.verticalHeader() != nullptr);
    QCOMPARE(table.verticalHeader()->sectionResizeMode(0), QHeaderView::Fixed);

    const int expectedHeight = table.verticalHeader()->defaultSectionSize();
    QVERIFY(expectedHeight >= 28);
    for (int row = 0; row < table.rowCount(); ++row) {
        QCOMPARE(table.rowHeight(row), expectedHeight);
    }

    table.show();
    QVERIFY(QTest::qWaitForWindowExposed(&table));
    const int targetRow = qMin(1, table.rowCount() - 1);
    const QModelIndex targetIndex = table.model()->index(targetRow, kTranscriptTestTextColumn);
    QVERIFY(targetIndex.isValid());
    const QPoint clickPos = table.visualRect(targetIndex).center();
    QCOMPARE(table.rowAt(clickPos.y()), targetRow);

    QTest::mouseClick(table.viewport(), Qt::LeftButton, Qt::NoModifier, clickPos);
    QTRY_COMPARE(selectedRow(table), targetRow);
    QTRY_VERIFY(lastSeekFrame >= 0);
    const QTableWidgetItem* targetItem = table.item(targetRow, 0);
    QVERIFY(targetItem != nullptr);
    QCOMPARE(lastSeekFrame, targetItem->data(Qt::UserRole + 2).toLongLong());
}

void TestTranscriptTabFollow::testOverlayTransformEditsUpdatePreviewImmediately()
{
    TimelineClip clip = makeAudioClip(QStringLiteral("clip-overlay"), QStringLiteral("/tmp/overlay.wav"));
    clip.transcriptOverlay.enabled = true;
    clip.transcriptOverlay.useManualPlacement = true;
    clip.transcriptOverlay.translationX = 0.0;
    clip.transcriptOverlay.translationY = 0.0;
    clip.transcriptOverlay.boxWidth = 900.0;
    clip.transcriptOverlay.boxHeight = 220.0;

    QLineEdit clipLabel;
    QLabel detailsLabel;
    QTableWidget table;
    QCheckBox overlayEnabled;
    overlayEnabled.setChecked(true);
    QComboBox placementMode;
    placementMode.addItem(QStringLiteral("Manual"), true);
    placementMode.addItem(QStringLiteral("Follow Speaker"), false);
    QDoubleSpinBox overlayX;
    overlayX.setRange(-1.0, 1.0);
    overlayX.setValue(0.0);
    QDoubleSpinBox overlayY;
    overlayY.setRange(-1.0, 1.0);
    overlayY.setValue(0.0);
    QSpinBox overlayWidth;
    overlayWidth.setRange(
        static_cast<int>(TimelineClip::TranscriptOverlaySettings::kMinReadableBoxWidth),
        10000);
    overlayWidth.setValue(900);
    QSpinBox overlayHeight;
    overlayHeight.setRange(
        static_cast<int>(TimelineClip::TranscriptOverlaySettings::kMinReadableBoxHeight),
        10000);
    overlayHeight.setValue(220);

    int previewUpdateCount = 0;
    int saveCount = 0;
    int historyCount = 0;

    TranscriptTab::Widgets widgets;
    widgets.transcriptInspectorClipLabel = &clipLabel;
    widgets.transcriptInspectorDetailsLabel = &detailsLabel;
    widgets.transcriptTable = &table;
    widgets.transcriptOverlayEnabledCheckBox = &overlayEnabled;
    widgets.transcriptPlacementModeCombo = &placementMode;
    widgets.transcriptOverlayXSpin = &overlayX;
    widgets.transcriptOverlayYSpin = &overlayY;
    widgets.transcriptOverlayWidthSpin = &overlayWidth;
    widgets.transcriptOverlayHeightSpin = &overlayHeight;

    TranscriptTab tab(
        widgets,
        TranscriptTab::Dependencies{
            [&clip]() -> const TimelineClip* { return &clip; },
            [&clip](const QString& id, const std::function<void(TimelineClip&)>& updater) {
                if (id != clip.id) {
                    return false;
                }
                updater(clip);
                return true;
            },
            [&saveCount]() { ++saveCount; },
            [&historyCount]() { ++historyCount; },
            []() {},
            [&previewUpdateCount]() { ++previewUpdateCount; },
            {},
            {}});
    tab.wire();

    overlayX.setValue(0.25);
    QCOMPARE(clip.transcriptOverlay.translationX, 0.25);
    QVERIFY(clip.transcriptOverlay.useManualPlacement);
    QCOMPARE(previewUpdateCount, 1);
    QCOMPARE(saveCount, 1);
    QCOMPARE(historyCount, 1);

    overlayWidth.setValue(1100);
    QCOMPARE(clip.transcriptOverlay.boxWidth, 1100.0);
    QCOMPARE(previewUpdateCount, 2);

    placementMode.setCurrentIndex(1);
    QVERIFY(!clip.transcriptOverlay.useManualPlacement);
    QCOMPARE(previewUpdateCount, 3);

    overlayHeight.setValue(260);
    QCOMPARE(clip.transcriptOverlay.boxHeight, 260.0);
    QVERIFY(clip.transcriptOverlay.useManualPlacement);
    QCOMPARE(previewUpdateCount, 4);
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
    QVERIFY(writeActiveEditableTranscript(clipPath, words));

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

    auto widgets = makeTranscriptWidgets(
        &clipLabel, &detailsLabel, &table, &follow, &prependSpin, &postpendSpin, &speechEnabled, &speechFade);
    widgets.transcriptScriptVersionCombo = &scriptVersions;
    widgets.transcriptNewVersionButton = &newVersionButton;
    widgets.transcriptDeleteVersionButton = &deleteVersionButton;

    TranscriptTab tab(
        widgets,
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
    QTRY_VERIFY_WITH_TIMEOUT(scriptVersions.currentData().toString() == editablePath, 2000);

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
