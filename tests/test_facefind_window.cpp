#include "facefind_window.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QtTest/QtTest>

namespace {

facefind::Candidate candidateRow(int trackId,
                                 const QString& clusterId,
                                 const QVector<int>& clusterTrackIds = {})
{
    facefind::Candidate candidate;
    candidate.trackId = trackId;
    candidate.clusterId = clusterId;
    candidate.clusterTrackIds = clusterTrackIds;
    candidate.frame = 12 + trackId;
    candidate.x = 0.25;
    candidate.y = 0.50;
    candidate.box = 0.18;
    candidate.score = 0.90 + (trackId * 0.01);
    candidate.clusterConfidence = 0.88;
    candidate.clusterStatus = QStringLiteral("auto_clustered");
    return candidate;
}

QDialog* findAssignmentDialog()
{
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* dialog = qobject_cast<QDialog*>(widget);
        if (dialog &&
            dialog->windowTitle() == QStringLiteral("FaceFind: Assign FaceDetections Clusters")) {
            return dialog;
        }
    }
    return nullptr;
}

QMessageBox* findMessageBox()
{
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* box = qobject_cast<QMessageBox*>(widget);
        if (box) {
            return box;
        }
    }
    return nullptr;
}

QPushButton* findPushButton(QDialog* dialog, const QString& text)
{
    if (!dialog) {
        return nullptr;
    }
    const auto buttons = dialog->findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->text() == text) {
            return button;
        }
    }
    return nullptr;
}

} // namespace

class FacefindWindowTest : public QObject {
    Q_OBJECT

private slots:
    void dialogAppliesAssignmentsFromWidgetState()
    {
        const QVector<facefind::Candidate> candidates{
            candidateRow(3, QStringLiteral("person_001"), {3, 7}),
            candidateRow(5, QStringLiteral("person_002"), {5})
        };
        const QStringList speakerIds{QStringLiteral("SPEAKER_A"), QStringLiteral("SPEAKER_B")};
        const QHash<QString, QString> speakerLabels{
            {QStringLiteral("SPEAKER_A"), QStringLiteral("Speaker A")},
            {QStringLiteral("SPEAKER_B"), QStringLiteral("Speaker B")}
        };
        const QStringList suggested{QStringLiteral("SPEAKER_B"), QStringLiteral("SPEAKER_A")};
        const QStringList autoSuggested{QStringLiteral("SPEAKER_A"), QStringLiteral("SPEAKER_B")};
        const QStringList defaultSources{QStringLiteral("Persisted (Human)"), QStringLiteral("Auto (Timing)")};

        QTimer::singleShot(0, this, []() {
            QDialog* dialog = findAssignmentDialog();
            QVERIFY(dialog);
            auto* table = dialog->findChild<QTableWidget*>();
            QVERIFY(table);
            QCOMPARE(table->rowCount(), 2);
            QCOMPARE(table->item(0, 1)->text(), QStringLiteral("person_001"));
            QCOMPARE(table->item(0, 2)->text(), QStringLiteral("T3, T7"));

            auto* useAutoButton = findPushButton(dialog, QStringLiteral("Use Auto Suggestions"));
            QVERIFY(useAutoButton);
            QTest::mouseClick(useAutoButton, Qt::LeftButton);

            auto* combo0 = qobject_cast<QComboBox*>(table->cellWidget(0, 8));
            auto* combo1 = qobject_cast<QComboBox*>(table->cellWidget(1, 8));
            QVERIFY(combo0);
            QVERIFY(combo1);
            QCOMPARE(combo0->currentData().toString(), QStringLiteral("SPEAKER_A"));
            QCOMPARE(combo1->currentData().toString(), QStringLiteral("SPEAKER_B"));
            QCOMPARE(table->item(0, 7)->text(), QStringLiteral("Auto (Timing)"));

            auto* applyButton = findPushButton(dialog, QStringLiteral("Apply Assignments"));
            QVERIFY(applyButton);
            QTest::mouseClick(applyButton, Qt::LeftButton);
        });

        const auto result =
            facefind::showFaceFindWindow(
                nullptr,
                candidates,
                speakerIds,
                speakerLabels,
                suggested,
                autoSuggested,
                defaultSources,
                QStringLiteral("Cluster summary"));

        QVERIFY(result.accepted);
        QCOMPARE(result.assignmentTableRows.size(), 2);
        QCOMPARE(result.assignmentsBySpeaker.size(), 2);

        const QJsonObject row0 = result.assignmentTableRows.at(0).toObject();
        QCOMPARE(row0.value(QStringLiteral("decision")).toString(), QStringLiteral("accepted"));
        QCOMPARE(row0.value(QStringLiteral("resolved_speaker_id")).toString(), QStringLiteral("SPEAKER_A"));
        const QJsonArray row0Tracks = row0.value(QStringLiteral("track_ids")).toArray();
        QCOMPARE(row0Tracks.size(), 2);
        QCOMPARE(row0Tracks.at(0).toInt(), 3);
        QCOMPARE(row0Tracks.at(1).toInt(), 7);
    }

    void invalidManualOverrideShowsWarningAndAllowsRetry()
    {
        const QVector<facefind::Candidate> candidates{
            candidateRow(8, QStringLiteral("person_003"), {8})
        };
        const QStringList speakerIds{QStringLiteral("SPEAKER_A")};
        const QHash<QString, QString> speakerLabels{
            {QStringLiteral("SPEAKER_A"), QStringLiteral("Speaker A")}
        };
        const QStringList suggested{QStringLiteral("SPEAKER_A")};
        const QStringList autoSuggested{QStringLiteral("SPEAKER_A")};
        const QStringList defaultSources{QStringLiteral("Auto (Timing)")};

        QTimer::singleShot(0, this, []() {
            QDialog* dialog = findAssignmentDialog();
            QVERIFY(dialog);
            auto* table = dialog->findChild<QTableWidget*>();
            QVERIFY(table);
            auto* manualEdit = qobject_cast<QLineEdit*>(table->cellWidget(0, 9));
            QVERIFY(manualEdit);
            manualEdit->setText(QStringLiteral("bad id!"));
            auto* applyButton = findPushButton(dialog, QStringLiteral("Apply Assignments"));
            QVERIFY(applyButton);
            QTest::mouseClick(applyButton, Qt::LeftButton);

            QTimer::singleShot(0, []() {
                QMessageBox* box = findMessageBox();
                QVERIFY(box);
                box->accept();

                QTimer::singleShot(0, []() {
                    QDialog* retryDialog = findAssignmentDialog();
                    QVERIFY(retryDialog);
                    auto* retryTable = retryDialog->findChild<QTableWidget*>();
                    QVERIFY(retryTable);
                    auto* retryManualEdit = qobject_cast<QLineEdit*>(retryTable->cellWidget(0, 9));
                    QVERIFY(retryManualEdit);
                    retryManualEdit->setText(QStringLiteral("SPEAKER_A"));
                    auto* retryApplyButton =
                        findPushButton(retryDialog, QStringLiteral("Apply Assignments"));
                    QVERIFY(retryApplyButton);
                    QTest::mouseClick(retryApplyButton, Qt::LeftButton);
                });
            });
        });

        const auto result =
            facefind::showFaceFindWindow(
                nullptr,
                candidates,
                speakerIds,
                speakerLabels,
                suggested,
                autoSuggested,
                defaultSources,
                QString());

        QVERIFY(result.accepted);
        QCOMPARE(result.assignmentTableRows.size(), 1);
        const QJsonObject row = result.assignmentTableRows.at(0).toObject();
        QCOMPARE(row.value(QStringLiteral("decision")).toString(), QStringLiteral("accepted"));
        QCOMPARE(row.value(QStringLiteral("manual_override")).toString(), QStringLiteral("SPEAKER_A"));
        QCOMPARE(row.value(QStringLiteral("resolved_speaker_id")).toString(), QStringLiteral("SPEAKER_A"));
    }
};

QTEST_MAIN(FacefindWindowTest)
#include "test_facefind_window.moc"
