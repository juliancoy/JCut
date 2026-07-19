#include <QtTest/QtTest>

#include "../explorer_pane.h"

#include <QFileDialog>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>

class TestExplorerPane final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void initialRootUpdatesBrowserWithoutUserChangeSignals();
    void opensOneAsynchronousQtDirectoryDialog();
    void acceptedDirectoryIsEmitted();
};

void TestExplorerPane::initTestCase()
{
    qputenv("JCUT_DIRECTORY_DIALOG_BACKEND", "qt");
}

void TestExplorerPane::initialRootUpdatesBrowserWithoutUserChangeSignals()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    ExplorerPane pane;
    QSignalSpy rootChangedSpy(&pane, &ExplorerPane::folderRootChanged);
    QSignalSpy stateChangedSpy(&pane, &ExplorerPane::stateChanged);
    pane.setInitialRootPath(root.path());

    QCOMPARE(pane.currentRootPath(), QDir(root.path()).absolutePath());
    QCOMPARE(rootChangedSpy.size(), 0);
    QCOMPARE(stateChangedSpy.size(), 0);
}

void TestExplorerPane::opensOneAsynchronousQtDirectoryDialog()
{
    ExplorerPane pane;
    pane.show();
    auto* button = pane.findChild<QPushButton*>(QStringLiteral("explorer.media_root_button"));
    QVERIFY(button);

    button->click();
    auto dialogs = pane.findChildren<QFileDialog*>();
    QCOMPARE(dialogs.size(), 1);
    QVERIFY(dialogs.constFirst()->testOption(QFileDialog::DontUseNativeDialog));
    QCOMPARE(dialogs.constFirst()->windowModality(), Qt::WindowModal);
    QCOMPARE(dialogs.constFirst()->labelText(QFileDialog::Accept),
             QStringLiteral("Select Folder"));

    button->click();
    QCOMPARE(pane.findChildren<QFileDialog*>().size(), 1);
    dialogs.constFirst()->reject();
    QTRY_VERIFY(pane.findChildren<QFileDialog*>().isEmpty());
}

void TestExplorerPane::acceptedDirectoryIsEmitted()
{
    QTemporaryDir selectedDirectory;
    QVERIFY(selectedDirectory.isValid());

    ExplorerPane pane;
    QSignalSpy chosenSpy(&pane, &ExplorerPane::folderRootChosen);
    auto* button = pane.findChild<QPushButton*>(QStringLiteral("explorer.media_root_button"));
    QVERIFY(button);
    button->click();

    auto* dialog = pane.findChild<QFileDialog*>();
    QVERIFY(dialog);
    dialog->setDirectory(selectedDirectory.path());
    QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection));

    QTRY_COMPARE(chosenSpy.size(), 1);
    QCOMPARE(chosenSpy.constFirst().constFirst().toString(),
             QDir(selectedDirectory.path()).absolutePath());
    QTRY_VERIFY(pane.findChildren<QFileDialog*>().isEmpty());
}

QTEST_MAIN(TestExplorerPane)
#include "test_explorer_pane.moc"
