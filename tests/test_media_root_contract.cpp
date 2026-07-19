#include <QtTest/QtTest>

#include <QFile>
#include <QRegularExpression>

namespace {

QString sourceFile(const QString& relativePath)
{
    QFile file(QStringLiteral(JCUT_SOURCE_DIR "/") + relativePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QString functionBody(const QString& source, const QString& signature)
{
    const qsizetype signatureStart = source.indexOf(signature);
    if (signatureStart < 0) {
        return {};
    }
    const qsizetype openingBrace = source.indexOf(QLatin1Char('{'), signatureStart);
    if (openingBrace < 0) {
        return {};
    }

    int depth = 0;
    for (qsizetype index = openingBrace; index < source.size(); ++index) {
        if (source.at(index) == QLatin1Char('{')) {
            ++depth;
        } else if (source.at(index) == QLatin1Char('}') && --depth == 0) {
            return source.mid(signatureStart, index - signatureStart + 1);
        }
    }
    return {};
}

} // namespace

class TestMediaRootContract final : public QObject
{
    Q_OBJECT

private slots:
    void changingMediaRootDoesNotMutateProjectStorage();
    void stateRestorePreservesSavedMediaRoot();
};

void TestMediaRootContract::changingMediaRootDoesNotMutateProjectStorage()
{
    const QString body = functionBody(
        sourceFile(QStringLiteral("project_state.cpp")),
        QStringLiteral("void EditorWindow::changeMediaRoot(const QString &path)"));
    QVERIFY2(!body.isEmpty(), "changeMediaRoot implementation must remain discoverable");
    QVERIFY(body.contains(QStringLiteral("m_explorerPane->setInitialRootPath(requestedRoot)")));
    QVERIFY(body.contains(QStringLiteral("scheduleSaveState()")));
    QVERIFY(!body.contains(QStringLiteral("changeRootDirPath")));
    QVERIFY(!body.contains(QRegularExpression(QStringLiteral("\\bloadState\\s*\\("))));
    QVERIFY(!body.contains(QStringLiteral("switchToProject")));
}

void TestMediaRootContract::stateRestorePreservesSavedMediaRoot()
{
    const QString body = functionBody(
        sourceFile(QStringLiteral("editor.cpp")),
        QStringLiteral("void EditorWindow::applyStateJson(const QJsonObject &root)"));
    QVERIFY2(!body.isEmpty(), "applyStateJson implementation must remain discoverable");
    QVERIFY(body.contains(QStringLiteral("const QString mediaRoot = resolvedRootPath")));
    QVERIFY(body.contains(QStringLiteral("m_explorerPane->setInitialRootPath(mediaRoot)")));
    QVERIFY(!body.contains(QRegularExpression(
        QStringLiteral("mediaRoot\\s*=\\s*\\([^;]*projectsRoot"))));
}

QTEST_APPLESS_MAIN(TestMediaRootContract)
#include "test_media_root_contract.moc"
