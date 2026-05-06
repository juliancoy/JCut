#include "editor.h"
#include "editor_ai_helpers.h"

#include <cppmonetize/AuthIdentity.h>
#include <cppmonetize/ApiErrorUtils.h>
#include <cppmonetize/MonetizeClient.h>
#include <cppmonetize/OAuthDesktopFlow.h>
#include <cppmonetize/TokenStore.h>

#include <QDateTime>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QMetaEnum>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QClipboard>
#include <QWidget>
#include <QAbstractButton>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QHeaderView>
#include <QVBoxLayout>

namespace {

constexpr auto kDefaultSupabaseGateway = "https://ivwutugdrpugjqglxabw.supabase.co";
bool isSupabaseProjectBase(const QString& baseUrl);

cppmonetize::MonetizeClient createJCutMonetizeClient(const QString& apiBaseUrl,
                                                      int timeoutMs,
                                                      const QString& contractPrefix = QStringLiteral("1."))
{
    cppmonetize::ClientConfig cfg;
    cfg.apiBaseUrl = apiBaseUrl;
    cfg.timeoutMs = timeoutMs;
    cfg.clientId = QStringLiteral("jcut-desktop");
    cfg.requiredContractPrefix = contractPrefix;
    const bool suppressSupabaseEntitlement404 = isSupabaseProjectBase(apiBaseUrl);
    cfg.telemetryHook = [suppressSupabaseEntitlement404](const cppmonetize::RequestTelemetryEvent& event) {
        if (event.success) {
            return;
        }
        if (suppressSupabaseEntitlement404 &&
            event.statusCode == 404 &&
            event.operation.trimmed() == QStringLiteral("/api/ai/entitlements")) {
            // Supabase direct mode probes this endpoint, then falls back to functions.
            return;
        }
        qWarning().noquote() << "[CPPMonetize][JCut]"
                             << event.operation
                             << "status=" << event.statusCode
                             << "request_id=" << event.clientRequestId
                             << "message=" << event.message;
    };
    return cppmonetize::MonetizeClient(cfg);
}

QString aiDisplayIdentity(const QString& explicitIdentity, const QString& accessToken)
{
    const QString trimmed = explicitIdentity.trimmed();
    if (!trimmed.isEmpty()) {
        return trimmed;
    }
    return cppmonetize::parseAccessTokenIdentity(accessToken).displayIdentity();
}

bool isSupabaseProjectBase(const QString& baseUrl)
{
    const QUrl url(baseUrl.trimmed());
    if (!url.isValid()) {
        return false;
    }
    return url.host().trimmed().toLower().endsWith(QStringLiteral(".supabase.co"));
}

QString entitlementFailureStatus(const cppmonetize::ApiError& error)
{
    if (cppmonetize::isSubscriptionRequiredError(error)) {
        return QStringLiteral("AI unavailable: logged in, but no active AI subscription.");
    }
    if (cppmonetize::isBackendSchemaMisconfigured(error)) {
        return QStringLiteral(
            "AI unavailable: backend AI setup incomplete (missing DB migration/function).");
    }
    if (cppmonetize::isAuthTokenFailure(error)) {
        return QStringLiteral("AI unavailable: logged in, but AI endpoint rejected auth token.");
    }
    const QString msg = error.message.trimmed().isEmpty()
        ? QStringLiteral("Unknown error")
        : error.message.trimmed();
    if (error.statusCode > 0) {
        return QStringLiteral("AI unavailable: entitlement check failed (HTTP %1: %2)")
            .arg(error.statusCode)
            .arg(msg);
    }
    return QStringLiteral("AI unavailable: entitlement check failed (%1)").arg(msg);
}

QColor profileBadgeBaseColor(ProfileAccessBadge badge)
{
    switch (badge) {
    case ProfileAccessBadge::Subscribed:
        return QColor(0xe6, 0xc2, 0x44);
    case ProfileAccessBadge::Basic:
        return QColor(0x7d, 0x8a, 0x9c);
    case ProfileAccessBadge::Unknown:
    default:
        return QColor(0x3a, 0x43, 0x59);
    }
}

QString profileBadgeLabel(ProfileAccessBadge badge)
{
    switch (badge) {
    case ProfileAccessBadge::Subscribed:
        return QStringLiteral("subscribed");
    case ProfileAccessBadge::Basic:
        return QStringLiteral("basic");
    case ProfileAccessBadge::Unknown:
    default:
        return QStringLiteral("unknown");
    }
}

QString jsonValueToCompactString(const QJsonValue& v)
{
    if (v.isString()) {
        return v.toString().trimmed();
    }
    if (v.isDouble()) {
        return QString::number(v.toDouble());
    }
    if (v.isBool()) {
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    if (v.isObject()) {
        return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    }
    if (v.isArray()) {
        return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
    }
    return QString();
}

QString decodeOAuthErrorText(const QString& raw)
{
    QString normalized = raw.trimmed();
    normalized.replace(QLatin1Char('+'), QLatin1Char(' '));
    return QUrl::fromPercentEncoding(normalized.toUtf8());
}

QString friendlyOAuthErrorMessage(const QString& rawMessage)
{
    const QString decoded = decodeOAuthErrorText(rawMessage);
    const QString lowered = decoded.toLower();
    if (lowered.contains(QStringLiteral("hook_restrict_signup_by_email_domain")) ||
        lowered.contains(QStringLiteral("restrict_signup_by_email_domain")) ||
        lowered.contains(QStringLiteral("email domain"))) {
        return QStringLiteral(
            "Sign-in is blocked for this email domain.\n\n"
            "Use an allowed organization email address, or ask an admin to allow your domain.");
    }
    return decoded;
}

QString widgetLabelText(const QWidget* widget)
{
    if (!widget) {
        return QString();
    }
    if (const auto* button = qobject_cast<const QAbstractButton*>(widget)) {
        return button->text().trimmed();
    }
    if (const auto* label = qobject_cast<const QLabel*>(widget)) {
        return label->text().trimmed();
    }
    if (const auto* lineEdit = qobject_cast<const QLineEdit*>(widget)) {
        return lineEdit->text().trimmed();
    }
    if (const auto* plainText = qobject_cast<const QPlainTextEdit*>(widget)) {
        return plainText->toPlainText().trimmed();
    }
    if (const auto* combo = qobject_cast<const QComboBox*>(widget)) {
        return combo->currentText().trimmed();
    }
    if (const auto* group = qobject_cast<const QGroupBox*>(widget)) {
        return group->title().trimmed();
    }
    if (const auto* tab = qobject_cast<const QTabWidget*>(widget)) {
        if (tab->currentIndex() >= 0) {
            return tab->tabText(tab->currentIndex()).trimmed();
        }
    }
    return QString();
}

QString aiActivityPreviewText(const QString& text, int maxLen = 180)
{
    QString preview = text;
    preview.replace(QStringLiteral("\r\n"), QStringLiteral(" "));
    preview.replace(QLatin1Char('\n'), QLatin1Char(' '));
    preview = preview.simplified();
    if (preview.size() > maxLen) {
        preview = preview.left(maxLen - 1) + QChar(0x2026);
    }
    return preview;
}

void appendUiHierarchyLines(const QObject* node, int depth, QStringList* lines)
{
    if (!node || !lines) {
        return;
    }
    const QString indent(depth * 2, QLatin1Char(' '));
    QString line = QStringLiteral("%1- %2").arg(indent, QString::fromLatin1(node->metaObject()->className()));
    if (!node->objectName().trimmed().isEmpty()) {
        line += QStringLiteral(" name=\"%1\"").arg(node->objectName().trimmed());
    }
    if (const auto* widget = qobject_cast<const QWidget*>(node)) {
        const QRect g = widget->geometry();
        line += QStringLiteral(" visible=%1 enabled=%2 geom=%3,%4,%5x%6")
                    .arg(widget->isVisible() ? QStringLiteral("true") : QStringLiteral("false"))
                    .arg(widget->isEnabled() ? QStringLiteral("true") : QStringLiteral("false"))
                    .arg(g.x())
                    .arg(g.y())
                    .arg(g.width())
                    .arg(g.height());
        const QString label = widgetLabelText(widget);
        if (!label.isEmpty()) {
            line += QStringLiteral(" text=\"%1\"").arg(label.left(160).toHtmlEscaped());
        }
    }
    lines->push_back(line);
    for (const QObject* child : node->children()) {
        appendUiHierarchyLines(child, depth + 1, lines);
    }
}

}  // namespace

QString EditorWindow::aiSecureStoreServiceName() const
{
    return QStringLiteral("jcut.ai.auth");
}

namespace {
QString legacyAiSecureStoreServiceNameForBase(const QString& baseUrl)
{
    QString base = baseUrl.trimmed();
    if (base.isEmpty()) {
        base = QStringLiteral("unset-gateway");
    }
    QByteArray keyBytes = base.toUtf8().toHex();
    if (keyBytes.size() > 64) {
        keyBytes = keyBytes.left(64);
    }
    return QStringLiteral("jcut.ai.%1").arg(QString::fromLatin1(keyBytes));
}
} // namespace

bool EditorWindow::readAiTokenFromSecureStore(QString* tokenOut,
                                              QString* refreshTokenOut,
                                              QString* userIdOut) const
{
    if (tokenOut) {
        tokenOut->clear();
    }
    if (refreshTokenOut) {
        refreshTokenOut->clear();
    }
    if (userIdOut) {
        userIdOut->clear();
    }

    const QStringList serviceNames{
        aiSecureStoreServiceName(),
        legacyAiSecureStoreServiceNameForBase(m_aiProxyBaseUrl),
        legacyAiSecureStoreServiceNameForBase(QStringLiteral("unset-gateway"))};

    for (const QString& serviceName : serviceNames) {
        cppmonetize::TokenStoreConfig cfg;
        cfg.appName = QStringLiteral("jcut");
        cfg.orgName = QStringLiteral("jcut");
        cfg.serviceName = serviceName;
        auto store = cppmonetize::createDefaultTokenStore(cfg);
        const auto tokenResult = store->loadToken();
        if (!tokenResult.hasValue()) {
            continue;
        }
        const QString token = tokenResult.value().trimmed();
        if (token.isEmpty()) {
            continue;
        }
        const QString refreshToken;
        const QString userId = store->loadUserId().hasValue()
            ? store->loadUserId().value().trimmed()
            : QString();
        if (tokenOut) {
            *tokenOut = token;
        }
        if (refreshTokenOut) {
            *refreshTokenOut = refreshToken;
        }
        if (userIdOut) {
            *userIdOut = userId;
        }
        return true;
    }
    return false;
}

bool EditorWindow::writeAiTokenToSecureStore(const QString& token,
                                             const QString& refreshToken,
                                             QString* errorOut) const
{
    if (errorOut) {
        errorOut->clear();
    }
    if (token.trimmed().isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Cannot store empty token.");
        }
        return false;
    }

    cppmonetize::TokenStoreConfig cfg;
    cfg.appName = QStringLiteral("jcut");
    cfg.orgName = QStringLiteral("jcut");
    cfg.serviceName = aiSecureStoreServiceName();
    auto store = cppmonetize::createDefaultTokenStore(cfg);
    Q_UNUSED(refreshToken);
    const auto writeResult = store->storeToken(token.trimmed(), m_aiUserId.trimmed());
    if (!writeResult.hasValue()) {
        if (errorOut) {
            *errorOut = writeResult.error().message;
        }
        return false;
    }
    return true;
}

bool EditorWindow::clearAiTokenFromSecureStore(QString* errorOut) const
{
    if (errorOut) {
        errorOut->clear();
    }

    const QStringList serviceNames{
        aiSecureStoreServiceName(),
        legacyAiSecureStoreServiceNameForBase(m_aiProxyBaseUrl),
        legacyAiSecureStoreServiceNameForBase(QStringLiteral("unset-gateway"))};

    QString lastError;
    bool anySuccess = false;
    for (const QString& serviceName : serviceNames) {
        cppmonetize::TokenStoreConfig cfg;
        cfg.appName = QStringLiteral("jcut");
        cfg.orgName = QStringLiteral("jcut");
        cfg.serviceName = serviceName;
        auto store = cppmonetize::createDefaultTokenStore(cfg);
        const auto clearResult = store->clear();
        if (clearResult.hasValue()) {
            anySuccess = true;
            continue;
        }
        const QString err = clearResult.error().message.trimmed();
        if (!err.isEmpty()) {
            lastError = err;
        }
    }
    if (!anySuccess && !lastError.isEmpty()) {
        if (errorOut) {
            *errorOut = lastError;
        }
        return false;
    }
    return true;
}

void EditorWindow::updateProfileAvatarButton()
{
    if (!m_profileAvatarButton) {
        return;
    }
    const bool loggedIn = !m_aiAuthToken.trimmed().isEmpty();
    if (m_profileCopyLoginButton) {
        m_profileCopyLoginButton->setVisible(!loggedIn);
        m_profileCopyLoginButton->setEnabled(!m_aiOAuthInProgress);
        m_profileCopyLoginButton->setToolTip(QStringLiteral("Copy browser sign-in URL"));
    }
    const QString displayIdentity = aiDisplayIdentity(m_aiUserId, m_aiAuthToken);

    if (!loggedIn) {
        m_aiProfileAvatarUrl.clear();
        m_aiProfileAvatarPixmap = QPixmap();
        m_aiProfileAvatarFetchInFlight = false;
        m_aiProfileAvatarLoadFailed = false;
        m_aiProfileBadge = ProfileAccessBadge::Unknown;
        m_profileAvatarAnimationTimer.stop();
        m_profileAvatarAnimationTick = 0;
        m_profileAvatarButton->setText(QStringLiteral("Log In"));
        m_profileAvatarButton->setIcon(QIcon());
        m_profileAvatarButton->setStyleSheet(QString());
        m_profileAvatarButton->setMinimumSize(0, 30);
        m_profileAvatarButton->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        m_profileAvatarButton->setToolTip(
            QStringLiteral("AI unavailable: credentials required. Log In to continue."));
    } else {
        const QString requestedAvatarUrl = cppmonetize::profileImageUrlFromAccessToken(m_aiAuthToken);
        if (requestedAvatarUrl != m_aiProfileAvatarUrl) {
            m_aiProfileAvatarUrl = requestedAvatarUrl;
            m_aiProfileAvatarPixmap = QPixmap();
            m_aiProfileAvatarFetchInFlight = false;
            m_aiProfileAvatarLoadFailed = false;
        }
        if (!m_aiProfileAvatarUrl.isEmpty() &&
            m_aiProfileAvatarPixmap.isNull() &&
            !m_aiProfileAvatarFetchInFlight &&
            !m_aiProfileAvatarLoadFailed) {
            requestProfileAvatarImage(m_aiProfileAvatarUrl);
        }

        m_profileAvatarButton->setText(QString());
        m_profileAvatarButton->setIcon(QIcon(buildProfileAvatarChip()));
        m_profileAvatarButton->setIconSize(QSize(34, 34));
        m_profileAvatarButton->setFixedSize(34, 34);
        m_profileAvatarButton->setStyleSheet(QStringLiteral(
            "QPushButton#tabs\\.profile_avatar_button {"
            " border: none; border-radius: 17px; padding: 0; background: transparent; }"
            "QPushButton#tabs\\.profile_avatar_button:hover { opacity: 0.95; }"));
        m_profileAvatarButton->setToolTip(
            displayIdentity.isEmpty()
                ? QStringLiteral("Signed in (%1)").arg(profileBadgeLabel(m_aiProfileBadge))
                : QStringLiteral("Signed in as %1 (%2)").arg(displayIdentity, profileBadgeLabel(m_aiProfileBadge)));

        const bool animateGold = (m_aiProfileBadge == ProfileAccessBadge::Subscribed);
        if (animateGold) {
            if (!m_profileAvatarAnimationTimer.isActive()) {
                connect(&m_profileAvatarAnimationTimer, &QTimer::timeout, this, [this]() {
                    m_profileAvatarAnimationTick = (m_profileAvatarAnimationTick + 1) % 120;
                    if (m_profileAvatarButton && !m_aiAuthToken.trimmed().isEmpty()) {
                        m_profileAvatarButton->setIcon(QIcon(buildProfileAvatarChip()));
                    }
                }, Qt::UniqueConnection);
                m_profileAvatarAnimationTimer.start(55);
            }
        } else {
            m_profileAvatarAnimationTimer.stop();
            m_profileAvatarAnimationTick = 0;
        }
    }
    m_profileAvatarButton->setVisible(true);
    m_profileAvatarButton->raise();
}

QPixmap EditorWindow::buildProfileAvatarChip() const
{
    constexpr int chipSize = 34;
    constexpr int avatarSize = 28;
    const int avatarInset = (chipSize - avatarSize) / 2;
    QPixmap chip(chipSize, chipSize);
    chip.fill(Qt::transparent);

    QPainter p(&chip);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);

    const ProfileAccessBadge badge = m_aiProfileBadge;
    if (badge == ProfileAccessBadge::Subscribed) {
        const qreal t = static_cast<qreal>(m_profileAvatarAnimationTick) / 120.0;
        QRadialGradient rg(QPointF(chipSize / 2.0, chipSize / 2.0), chipSize / 2.0);
        rg.setColorAt(0.0, QColor(255, 245, 196));
        rg.setColorAt(0.55, QColor(237, 196, 78));
        rg.setColorAt(1.0, QColor(161, 118, 26));
        p.setBrush(rg);
        p.drawEllipse(0, 0, chipSize, chipSize);

        // Animated textured sheen for full_access.
        for (int i = -chipSize; i < chipSize * 2; i += 4) {
            const int shift = static_cast<int>(t * 28.0) % 28;
            const int x = i + shift;
            QColor stripe((i / 4) % 2 == 0 ? QColor(255, 230, 140) : QColor(255, 210, 95));
            stripe.setAlpha(44);
            p.setPen(QPen(stripe, 2.0));
            p.drawLine(x, 0, x - chipSize, chipSize);
        }
        QPen ringPen(QColor(255, 242, 170, 200));
        ringPen.setWidth(2);
        p.setPen(ringPen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(1, 1, chipSize - 2, chipSize - 2);
        p.setPen(Qt::NoPen);
    } else {
        p.setBrush(profileBadgeBaseColor(badge));
        p.drawEllipse(0, 0, chipSize, chipSize);
    }

    QPixmap avatar = m_aiProfileAvatarPixmap;
    if (avatar.isNull()) {
        const QString displayIdentity = aiDisplayIdentity(m_aiUserId, m_aiAuthToken);
        const QString avatarSeed = displayIdentity.isEmpty() ? QStringLiteral("user@jcut") : displayIdentity;
        avatar = buildFallbackAvatar(avatarSeed);
    }
    avatar = avatar.scaled(QSize(avatarSize, avatarSize), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);

    QPainterPath clipPath;
    clipPath.addEllipse(QRectF(avatarInset, avatarInset, avatarSize, avatarSize));
    p.setClipPath(clipPath);
    p.drawPixmap(avatarInset, avatarInset, avatarSize, avatarSize, avatar);
    p.setClipping(false);

    QPen borderPen(QColor(18, 24, 34, 210));
    borderPen.setWidth(1);
    p.setPen(borderPen);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(avatarInset, avatarInset, avatarSize, avatarSize);
    return chip;
}

void EditorWindow::requestProfileAvatarImage(const QString& imageUrl)
{
    const QString normalized = imageUrl.trimmed();
    if (normalized.isEmpty()) {
        return;
    }
    const QUrl url(normalized);
    if (!url.isValid()) {
        return;
    }
    if (!m_aiProfileAvatarNetwork) {
        m_aiProfileAvatarNetwork = new QNetworkAccessManager(this);
    }
    m_aiProfileAvatarFetchInFlight = true;
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "image/*");
    QNetworkReply* reply = m_aiProfileAvatarNetwork->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, normalized]() {
        reply->deleteLater();
        m_aiProfileAvatarFetchInFlight = false;
        if (normalized != m_aiProfileAvatarUrl) {
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            m_aiProfileAvatarLoadFailed = true;
            updateProfileAvatarButton();
            return;
        }
        QPixmap pixmap;
        if (!pixmap.loadFromData(reply->readAll())) {
            m_aiProfileAvatarLoadFailed = true;
            updateProfileAvatarButton();
            return;
        }
        m_aiProfileAvatarLoadFailed = false;
        m_aiProfileAvatarPixmap = pixmap.scaled(
            QSize(28, 28), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        updateProfileAvatarButton();
    });
}

void EditorWindow::onProfileAvatarButtonClicked()
{
    const bool loggedIn = !m_aiAuthToken.trimmed().isEmpty();
    if (!loggedIn) {
        configureAiGatewayLogin();
        return;
    }

    const QString displayIdentity = aiDisplayIdentity(m_aiUserId, m_aiAuthToken);
    QMenu menu(this);
    QAction* profileAction = menu.addAction(
        displayIdentity.isEmpty()
            ? QStringLiteral("Signed in")
            : QStringLiteral("Signed in as %1").arg(displayIdentity));
    profileAction->setEnabled(false);
    menu.addSeparator();
    QAction* switchAction = menu.addAction(QStringLiteral("Switch Account"));
    QAction* logoutAction = menu.addAction(QStringLiteral("Sign Out"));

    QAction* picked =
        menu.exec(m_profileAvatarButton->mapToGlobal(QPoint(0, m_profileAvatarButton->height())));
    if (picked == switchAction) {
        configureAiGatewayLogin();
    } else if (picked == logoutAction) {
        clearAiGatewayLogin();
    }
}

void EditorWindow::configureAiGatewayLogin()
{
    if (m_aiOAuthInProgress) {
        QMessageBox::information(this,
                                 QStringLiteral("AI Login"),
                                 QStringLiteral("A login attempt is already in progress."));
        return;
    }

    m_aiOAuthInProgress = true;
    if (m_profileAvatarButton) {
        m_profileAvatarButton->setEnabled(false);
    }
    auto finishLoginAttempt = [this]() {
        m_aiOAuthInProgress = false;
        if (m_profileAvatarButton) {
            m_profileAvatarButton->setEnabled(true);
        }
    };

    QString gatewayBase = normalizeBaseUrl(m_aiProxyBaseUrl);
    if (gatewayBase.isEmpty()) {
        gatewayBase = normalizeBaseUrl(qEnvironmentVariable("SUPABASE_URL"));
    }
    if (gatewayBase.isEmpty()) {
        gatewayBase = normalizeBaseUrl(QString::fromLatin1(kDefaultSupabaseGateway));
    }

    cppmonetize::OAuthDesktopFlow oauthFlow;
    const auto oauthCfgResult = oauthFlow.resolveSupabaseConfig(gatewayBase, m_aiRequestTimeoutMs);
    if (!oauthCfgResult.hasValue()) {
        QMessageBox::warning(
            this,
            QStringLiteral("AI Login"),
            oauthCfgResult.error().message.isEmpty()
                ? QStringLiteral("Supabase OAuth is not enabled for this gateway.")
                : QStringLiteral("Failed to load Supabase OAuth config: %1").arg(oauthCfgResult.error().message));
        finishLoginAttempt();
        return;
    }
    const cppmonetize::OAuthConfig resolvedCfg = oauthCfgResult.value();
    cppmonetize::OAuthConfig flowCfg;
    flowCfg.enabled = true;
    flowCfg.supabaseUrl = resolvedCfg.supabaseUrl;
    flowCfg.supabaseAnonKey = resolvedCfg.supabaseAnonKey;

    const auto loginResult = oauthFlow.signInWithBrowserPkce(flowCfg, QStringLiteral("google"), 180000);
    if (!loginResult.hasValue()) {
        const QString rawMessage = loginResult.error().message;
        const QString friendly = friendlyOAuthErrorMessage(rawMessage);
        QMessageBox::warning(this,
                             QStringLiteral("AI Login"),
                             rawMessage.isEmpty()
                                 ? QStringLiteral("OAuth callback did not return a token.")
                                 : friendly);
        finishLoginAttempt();
        return;
    }
    const QString token = loginResult.value().token.trimmed();
    const QString refreshToken;
    const QString email = loginResult.value().email.trimmed();
    if (token.isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("AI Login"),
                             QStringLiteral("OAuth callback did not return a token."));
        finishLoginAttempt();
        return;
    }
    const auto verificationState = cppmonetize::emailVerificationStateFromAccessToken(token);
    if (verificationState == cppmonetize::EmailVerificationState::Unverified) {
        QString verificationMessage = QStringLiteral("AI requires a verified email address.\n\n");
        QString verificationEmail = email;
        if (verificationEmail.isEmpty()) {
            verificationEmail = cppmonetize::emailFromAccessToken(token);
        }
        if (!verificationEmail.isEmpty()) {
            verificationMessage += QStringLiteral("An email has been sent to %1.\n\n").arg(verificationEmail);
        }
        verificationMessage += QStringLiteral("Please verify your email, then Log In to Continue.");
        QMessageBox::warning(
            this,
            QStringLiteral("AI Login"),
            verificationMessage);
        finishLoginAttempt();
        return;
    }

    m_aiProxyBaseUrl = gatewayBase;
    m_aiAuthToken = token.trimmed();
    m_aiRefreshToken = refreshToken;
    m_aiRejectedAuthToken.clear();
    QString bestUser = email.trimmed();
    if (bestUser.isEmpty()) {
        bestUser = cppmonetize::parseAccessTokenIdentity(m_aiAuthToken).displayIdentity();
    }
    m_aiUserId = bestUser;

    QString secureStoreError;
    if (!writeAiTokenToSecureStore(m_aiAuthToken, m_aiRefreshToken, &secureStoreError)) {
        QMessageBox::warning(this,
                             QStringLiteral("AI Login"),
                             QStringLiteral("Token saved for this session only. Secure storage failed: %1")
                                 .arg(secureStoreError));
    }
    refreshAiIntegrationState();
    if (!m_aiIntegrationEnabled) {
        QMessageBox::warning(
            this,
            QStringLiteral("AI Login"),
            QStringLiteral("Sign-in completed, but AI is still unavailable.\n\n%1")
                .arg(m_aiIntegrationStatus));
    }
    scheduleSaveState();
    finishLoginAttempt();
}

void EditorWindow::copySupabaseSignInUrl()
{
    QString gatewayBase = normalizeBaseUrl(m_aiProxyBaseUrl);
    if (gatewayBase.isEmpty()) {
        gatewayBase = normalizeBaseUrl(qEnvironmentVariable("SUPABASE_URL"));
    }
    if (gatewayBase.isEmpty()) {
        gatewayBase = normalizeBaseUrl(QString::fromLatin1(kDefaultSupabaseGateway));
    }

    cppmonetize::OAuthDesktopFlow oauthFlow;
    const auto oauthCfgResult = oauthFlow.resolveSupabaseConfig(gatewayBase, m_aiRequestTimeoutMs);
    if (!oauthCfgResult.hasValue()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Copy Sign-In URL"),
            oauthCfgResult.error().message.isEmpty()
                ? QStringLiteral("Supabase OAuth is not enabled for this gateway.")
                : QStringLiteral("Failed to load Supabase OAuth config: %1").arg(oauthCfgResult.error().message));
        return;
    }
    const cppmonetize::OAuthConfig cfg = oauthCfgResult.value();
    const QString authUrl = oauthFlow.buildSupabaseAuthorizeUrl(
        cfg,
        QStringLiteral("google"),
        QStringLiteral("http://127.0.0.1/callback"),
        true);
    if (authUrl.trimmed().isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("Copy Sign-In URL"),
                             QStringLiteral("Unable to build sign-in URL from OAuth config."));
        return;
    }

    if (QGuiApplication::clipboard()) {
        QGuiApplication::clipboard()->setText(authUrl);
    }
    QMessageBox::information(
        this,
        QStringLiteral("Copy Sign-In URL"),
        QStringLiteral("Copied browser sign-in URL.\n\nOpen it in the browser profile/account you want first."));
}

void EditorWindow::clearAiGatewayLogin()
{
    QString secureStoreError;
    if (!clearAiTokenFromSecureStore(&secureStoreError)) {
        qWarning() << "Failed clearing AI token from secure store:" << secureStoreError;
    }
    m_aiAuthToken.clear();
    m_aiRefreshToken.clear();
    m_aiUserId.clear();
    refreshAiIntegrationState();
    scheduleSaveState();
}

void EditorWindow::refreshAiIntegrationState()
{
    m_aiContractVersion = QStringLiteral("unknown");
    QString status = QStringLiteral("AI unavailable: credentials required. Log In to continue.");
    bool enabled = false;
    QStringList modelOptions{
        QStringLiteral("deepseek-chat"),
        QStringLiteral("gpt-4o-mini"),
        QStringLiteral("mistral-small"),
        QStringLiteral("qwen2.5-7b-instruct")};
    QStringList fallbackModels;
    QString serviceUrl;
    int rateLimit = qMax(1, m_aiRateLimitPerMinute);
    int budgetCap = qMax(1, m_aiUsageBudgetCap);
    int timeoutMs = qMax(1000, m_aiRequestTimeoutMs);
    int retries = qBound(0, m_aiRequestRetries, 3);
    bool tokenRejectedUnverifiedEmail = false;
    const QString envBaseUrl = normalizeBaseUrl(qEnvironmentVariable("SUPABASE_URL"));
    const QString envToken = qEnvironmentVariable("JCUT_AI_AUTH_TOKEN").trimmed();
    if (m_aiProxyBaseUrl.trimmed().isEmpty()) {
        m_aiProxyBaseUrl = envBaseUrl;
    }
    if (m_aiProxyBaseUrl.trimmed().isEmpty()) {
        m_aiProxyBaseUrl = QString::fromLatin1(kDefaultSupabaseGateway);
    }
    if (m_aiAuthToken.trimmed().isEmpty()) {
        QString secureToken;
        QString secureRefreshToken;
        QString secureUserId;
        if (readAiTokenFromSecureStore(&secureToken, &secureRefreshToken, &secureUserId)) {
            m_aiAuthToken = secureToken;
            m_aiRefreshToken = secureRefreshToken;
            if (!secureUserId.isEmpty()) {
                m_aiUserId = secureUserId;
            }
        } else if (!envToken.isEmpty() && envToken != m_aiRejectedAuthToken) {
            m_aiAuthToken = envToken;
        }
    }
    if (!m_aiAuthToken.trimmed().isEmpty() &&
        cppmonetize::emailVerificationStateFromAccessToken(m_aiAuthToken) == cppmonetize::EmailVerificationState::Unverified) {
        tokenRejectedUnverifiedEmail = true;
        QString secureStoreError;
        if (!clearAiTokenFromSecureStore(&secureStoreError) && !secureStoreError.trimmed().isEmpty()) {
            qWarning() << "Failed clearing unverified AI token from secure store:" << secureStoreError;
        }
        m_aiAuthToken.clear();
        m_aiRefreshToken.clear();
        m_aiUserId.clear();
        m_aiProfileBadge = ProfileAccessBadge::Unknown;
    }

    const QString base = m_aiProxyBaseUrl.trimmed();
    if (!m_featureAiPanel) {
        status = QStringLiteral("AI disabled: feature_ai_panel=false");
    } else if (base.isEmpty()) {
        status = QStringLiteral("AI disabled: configure gateway URL");
    } else if (m_aiAuthToken.trimmed().isEmpty()) {
        status = tokenRejectedUnverifiedEmail
            ? QStringLiteral("AI unavailable: email not verified. Verify email, then Log In to continue.")
            : QStringLiteral("AI unavailable: credentials required. Log In to continue.");
    } else {
        QString normalizedBase = base;
        while (normalizedBase.endsWith(QLatin1Char('/'))) {
            normalizedBase.chop(1);
        }
        serviceUrl = normalizedBase + QStringLiteral("/api/ai/task");
        auto applyEntitlements = [&](const cppmonetize::AiEntitlements& ent) {
            const bool entitled = ent.entitled;
            m_aiContractVersion = ent.contractVersion.trimmed();
            const bool contractOk = m_aiContractVersion.startsWith(QStringLiteral("1."));
            m_aiUserId = ent.userId.trimmed();
            if (!ent.models.isEmpty()) {
                modelOptions = ent.models;
            }
            fallbackModels = ent.fallbackOrder;
            if (ent.requestsPerMinute > 0) {
                rateLimit = qMax(1, ent.requestsPerMinute);
            }
            if (ent.projectBudget > 0) {
                budgetCap = qMax(1, ent.projectBudget);
            }
            if (ent.timeoutMs > 0) {
                timeoutMs = qMax(1000, ent.timeoutMs);
            }
            retries = qBound(0, ent.retries, 3);

            enabled = entitled && contractOk;
            if (!contractOk) {
                status = QStringLiteral("AI disabled: unsupported contract '%1'").arg(m_aiContractVersion);
            } else if (!entitled) {
                status = QStringLiteral("AI disabled: user not entitled");
            } else {
                status = QStringLiteral("AI enabled for %1 (%2 req/min, budget %3)")
                             .arg(m_aiUserId.isEmpty() ? QStringLiteral("user") : m_aiUserId)
                             .arg(QString::number(rateLimit))
                             .arg(QString::number(budgetCap));
            }
        };

        cppmonetize::MonetizeClient client =
            createJCutMonetizeClient(normalizedBase, timeoutMs);
        std::optional<cppmonetize::AiEntitlements> entitlements;
        std::optional<cppmonetize::AiUsageStatus> usageStatus;
        bool entitledByContract = false;
        const auto tryRefreshAuthToken = [this]() -> bool {
            Q_UNUSED(this);
            return false;
        };

        auto entResult = client.getAiEntitlements(m_aiAuthToken);
        if (!entResult.hasValue() && cppmonetize::isAuthTokenFailure(entResult.error())) {
            if (tryRefreshAuthToken()) {
                entResult = client.getAiEntitlements(m_aiAuthToken);
            }
        }
        if (!entResult.hasValue()) {
            const cppmonetize::ApiError error = entResult.error();
            if (cppmonetize::isBackendSchemaMisconfigured(error)) {
                enabled = true;
                status = QStringLiteral(
                    "AI enabled (degraded mode): entitlement usage tables missing; subscription enforced at request time.");
            } else if (cppmonetize::isAuthTokenFailure(error)) {
                const QString rejectedToken = m_aiAuthToken.trimmed();
                QString secureStoreError;
                if (!clearAiTokenFromSecureStore(&secureStoreError) && !secureStoreError.trimmed().isEmpty()) {
                    qWarning() << "Failed clearing AI token after auth failure:" << secureStoreError;
                }
                m_aiAuthToken.clear();
                m_aiRefreshToken.clear();
                m_aiUserId.clear();
                m_aiProfileBadge = ProfileAccessBadge::Unknown;
                if (!rejectedToken.isEmpty()) {
                    m_aiRejectedAuthToken = rejectedToken;
                }
                status = QStringLiteral("AI unavailable: credentials invalid or expired. Log In to continue.");
            } else {
                status = entitlementFailureStatus(error);
            }
        } else {
            const cppmonetize::AiEntitlements ent = entResult.value();
            entitlements = ent;
            entitledByContract = ent.entitled;
            applyEntitlements(ent);
        }
        const bool canQueryUsage =
            !m_aiAuthToken.trimmed().isEmpty() && !isSupabaseProjectBase(normalizedBase);
        if (canQueryUsage) {
            const auto usageResult = client.getAiUsageStatus(m_aiAuthToken);
            if (usageResult.hasValue()) {
                usageStatus = usageResult.value();
            }
        }
        const cppmonetize::CapabilitySet capabilities =
            cppmonetize::deriveCapabilitySet(entitlements, usageStatus);
        const bool allowAiRequests =
            capabilities.values.value(QStringLiteral("ai.allow_requests")).toBool(false);
        const bool hasSubscription =
            capabilities.values.value(QStringLiteral("ai.has_subscription")).toBool(false);
        if (entitledByContract && !allowAiRequests) {
            // Keep current behavior in degraded/supabase-direct flows where contract check is authoritative.
            enabled = true;
        } else if (!entitledByContract && !allowAiRequests && !m_aiAuthToken.trimmed().isEmpty()) {
            enabled = false;
        }
        ProfileAccessBadge detectedBadge =
            m_aiAuthToken.trimmed().isEmpty()
                ? ProfileAccessBadge::Unknown
                : (hasSubscription || allowAiRequests
                       ? ProfileAccessBadge::Subscribed
                       : ProfileAccessBadge::Basic);
        m_aiProfileBadge = detectedBadge;
    }
    m_aiRateLimitPerMinute = rateLimit;
    m_aiUsageBudgetCap = budgetCap;
    m_aiRequestTimeoutMs = timeoutMs;
    m_aiRequestRetries = retries;
    m_aiFallbackModels = fallbackModels;
    m_aiIntegrationEnabled = enabled;
    m_aiIntegrationStatus = status;
    m_aiServiceUrl = serviceUrl;
    if (m_aiModelCombo) {
        QSignalBlocker blocker(m_aiModelCombo);
        m_aiModelCombo->clear();
        for (const QString& model : modelOptions) {
            m_aiModelCombo->addItem(model);
        }
        int preferred = m_aiModelCombo->findText(m_aiSelectedModel, Qt::MatchFixedString);
        if (preferred < 0) {
            preferred = m_aiModelCombo->findText(QStringLiteral("deepseek-chat"), Qt::MatchFixedString);
        }
        if (preferred < 0 && m_aiModelCombo->count() > 0) {
            preferred = 0;
        }
        if (preferred >= 0) {
            m_aiModelCombo->setCurrentIndex(preferred);
            m_aiSelectedModel = m_aiModelCombo->itemText(preferred);
        }
        m_aiModelCombo->setEnabled(enabled && m_featureAiPanel);
    }
    if (m_aiChatInputLineEdit) {
        const bool chatEnabled = enabled && m_featureAiPanel;
        m_aiChatInputLineEdit->setEnabled(chatEnabled);
        m_aiChatInputLineEdit->setToolTip(chatEnabled ? QString() : status);
    }
    if (m_aiChatSendButton) {
        const bool chatEnabled = enabled && m_featureAiPanel;
        m_aiChatSendButton->setEnabled(chatEnabled);
        m_aiChatSendButton->setToolTip(chatEnabled ? QString() : status);
    }
    if (m_aiChatClearButton) {
        m_aiChatClearButton->setEnabled(m_aiChatHistoryEdit && !m_aiChatHistoryEdit->toPlainText().trimmed().isEmpty());
    }
    if (m_aiSubscribeButton) {
        const bool loggedIn = !m_aiAuthToken.trimmed().isEmpty();
        m_aiSubscribeButton->setEnabled(m_featureAiPanel && loggedIn);
        if (!m_featureAiPanel) {
            m_aiSubscribeButton->setToolTip(QStringLiteral("AI Assist disabled by feature flag."));
        } else if (!loggedIn) {
            m_aiSubscribeButton->setToolTip(QStringLiteral("Log In first, then subscribe to AI access."));
        } else {
            m_aiSubscribeButton->setToolTip(QStringLiteral("Open checkout to subscribe/unlock AI access."));
        }
    }
    updateProfileAvatarButton();
    if (m_aiStatusLabel) {
        m_aiStatusLabel->setText(QStringLiteral("%1 | Usage %2/%3 (fail %4)")
                                     .arg(status)
                                     .arg(m_aiUsageRequests)
                                     .arg(m_aiUsageBudgetCap)
                                     .arg(m_aiUsageFailures));
    }
    refreshAccessTabData();
    for (QPushButton* btn : {m_aiTranscribeButton,
                             m_aiFindSpeakerNamesButton,
                             m_aiFindOrganizationsButton,
                             m_aiCleanAssignmentsButton}) {
        if (btn) {
            const bool speakerCleanupAction =
                (btn == m_aiFindSpeakerNamesButton ||
                 btn == m_aiFindOrganizationsButton ||
                 btn == m_aiCleanAssignmentsButton);
            const bool allowed = enabled && (!speakerCleanupAction || m_featureAiSpeakerCleanup);
            btn->setEnabled(allowed);
            btn->setToolTip(allowed
                                ? QString()
                                : (!m_featureAiSpeakerCleanup && speakerCleanupAction
                                       ? QStringLiteral("AI speaker cleanup disabled by feature flag.")
                                       : status));
        }
    }
}

void EditorWindow::refreshAccessTabData()
{
    if (!m_accessStatusLabel || !m_accessTable) {
        return;
    }

    m_accessTable->setRowCount(0);
    auto addRow = [this](const QString& type,
                         const QString& item,
                         const QString& status,
                         const QString& period,
                         const QString& source) {
        const int row = m_accessTable->rowCount();
        m_accessTable->insertRow(row);
        m_accessTable->setItem(row, 0, new QTableWidgetItem(type));
        m_accessTable->setItem(row, 1, new QTableWidgetItem(item));
        m_accessTable->setItem(row, 2, new QTableWidgetItem(status));
        m_accessTable->setItem(row, 3, new QTableWidgetItem(period));
        m_accessTable->setItem(row, 4, new QTableWidgetItem(source));
    };

    if (m_accessRefreshButton) {
        m_accessRefreshButton->setEnabled(m_featureAiPanel);
    }

    if (!m_featureAiPanel) {
        m_accessStatusLabel->setText(QStringLiteral("AI/account features disabled by feature flag."));
        return;
    }
    if (m_aiAuthToken.trimmed().isEmpty()) {
        m_accessStatusLabel->setText(QStringLiteral("Log in using top-right account button to view access."));
        return;
    }

    QString normalizedBase = normalizeBaseUrl(m_aiProxyBaseUrl);
    if (normalizedBase.isEmpty()) {
        normalizedBase = normalizeBaseUrl(qEnvironmentVariable("SUPABASE_URL"));
    }
    if (normalizedBase.isEmpty()) {
        normalizedBase = normalizeBaseUrl(QString::fromLatin1(kDefaultSupabaseGateway));
    }
    if (normalizedBase.isEmpty()) {
        m_accessStatusLabel->setText(QStringLiteral("Gateway URL is not configured."));
        return;
    }

    cppmonetize::MonetizeClient client =
        createJCutMonetizeClient(normalizedBase, m_aiRequestTimeoutMs);
    auto tryRefreshAuthToken = [this]() -> bool {
        Q_UNUSED(this);
        return false;
    };
    const bool supabaseDirect = isSupabaseProjectBase(normalizedBase);

    int rowCount = 0;
    cppmonetize::Result<QJsonObject> licenseResult =
        cppmonetize::Result<QJsonObject>::fail(cppmonetize::ApiError{});
    if (!supabaseDirect) {
        licenseResult = client.getLicense(m_aiAuthToken);
    }
    if (!supabaseDirect && licenseResult.hasValue()) {
        const QJsonObject root = licenseResult.value();
        const auto parseArray = [&](const QJsonArray& arr, const QString& typeLabel) {
            for (const QJsonValue& value : arr) {
                if (!value.isObject()) {
                    continue;
                }
                const QJsonObject obj = value.toObject();
                const QString item =
                    obj.value(QStringLiteral("product_slug")).toString().trimmed().isEmpty()
                        ? (!obj.value(QStringLiteral("slug")).toString().trimmed().isEmpty()
                               ? obj.value(QStringLiteral("slug")).toString().trimmed()
                               : (!obj.value(QStringLiteral("name")).toString().trimmed().isEmpty()
                                      ? obj.value(QStringLiteral("name")).toString().trimmed()
                                      : (!obj.value(QStringLiteral("scope_id")).toString().trimmed().isEmpty()
                                             ? obj.value(QStringLiteral("scope_id")).toString().trimmed()
                                             : obj.value(QStringLiteral("product_id")).toString().trimmed())))
                        : obj.value(QStringLiteral("product_slug")).toString().trimmed();
                const QString status =
                    !obj.value(QStringLiteral("status")).toString().trimmed().isEmpty()
                        ? obj.value(QStringLiteral("status")).toString().trimmed()
                        : (!jsonValueToCompactString(obj.value(QStringLiteral("active"))).isEmpty()
                               ? jsonValueToCompactString(obj.value(QStringLiteral("active")))
                               : QStringLiteral("unknown"));
                const QString starts = !obj.value(QStringLiteral("starts_at")).toString().trimmed().isEmpty()
                    ? obj.value(QStringLiteral("starts_at")).toString().trimmed()
                    : obj.value(QStringLiteral("current_period_start")).toString().trimmed();
                const QString ends = !obj.value(QStringLiteral("ends_at")).toString().trimmed().isEmpty()
                    ? obj.value(QStringLiteral("ends_at")).toString().trimmed()
                    : obj.value(QStringLiteral("current_period_end")).toString().trimmed();
                const QString period = starts.isEmpty() && ends.isEmpty()
                    ? QString()
                    : QStringLiteral("%1 -> %2")
                          .arg(starts.isEmpty() ? QStringLiteral("n/a") : starts,
                               ends.isEmpty() ? QStringLiteral("n/a") : ends);
                const QString source =
                    !obj.value(QStringLiteral("provider")).toString().trimmed().isEmpty()
                        ? obj.value(QStringLiteral("provider")).toString().trimmed()
                        : (!obj.value(QStringLiteral("source_type")).toString().trimmed().isEmpty()
                               ? obj.value(QStringLiteral("source_type")).toString().trimmed()
                               : obj.value(QStringLiteral("source_id")).toString().trimmed());
                addRow(typeLabel,
                       item.isEmpty() ? QStringLiteral("(unknown)") : item,
                       status,
                       period,
                       source);
                rowCount += 1;
            }
        };

        parseArray(root.value(QStringLiteral("subscriptions")).toArray(), QStringLiteral("Subscription"));
        parseArray(root.value(QStringLiteral("purchases")).toArray(), QStringLiteral("Purchase"));
        parseArray(root.value(QStringLiteral("products")).toArray(), QStringLiteral("Product"));
        parseArray(root.value(QStringLiteral("entitlements")).toArray(), QStringLiteral("Entitlement"));
    }

    cppmonetize::Result<cppmonetize::AiUsageStatus> usageResult =
        cppmonetize::Result<cppmonetize::AiUsageStatus>::fail(cppmonetize::ApiError{});
    if (!supabaseDirect) {
        usageResult = client.getAiUsageStatus(m_aiAuthToken);
        if (usageResult.hasValue()) {
            const auto& usage = usageResult.value();
            addRow(QStringLiteral("AI Usage"),
                   QStringLiteral("ai-platform"),
                   usage.hasSubscription ? QStringLiteral("has_subscription=true")
                                         : QStringLiteral("has_subscription=false"),
                   QStringLiteral("%1 (free used %2/%3)")
                       .arg(usage.usageMonth,
                            QString::number(usage.freeUsed),
                            QString::number(usage.freeLimit)),
                   usage.allowAiRequests ? QStringLiteral("allow_ai_requests=true")
                                         : QStringLiteral("allow_ai_requests=false"));
            rowCount += 1;
        }
    } else {
        const auto entResult = client.getAiEntitlements(m_aiAuthToken);
        if (entResult.hasValue()) {
            const auto& ent = entResult.value();
            addRow(QStringLiteral("AI Entitlement"),
                   QStringLiteral("ai-platform"),
                   ent.entitled ? QStringLiteral("entitled=true")
                                : QStringLiteral("entitled=false"),
                   QStringLiteral("contract %1").arg(ent.contractVersion),
                   QStringLiteral("supabase-direct"));
            rowCount += 1;
        } else {
            usageResult = cppmonetize::Result<cppmonetize::AiUsageStatus>::fail(entResult.error());
        }
    }

    if (rowCount > 0) {
        m_accessStatusLabel->setText(
            supabaseDirect
                ? QStringLiteral("Loaded %1 access records (Supabase direct mode).").arg(rowCount)
                : QStringLiteral("Loaded %1 access records.").arg(rowCount));
    } else if (!supabaseDirect && !licenseResult.hasValue()) {
        m_accessStatusLabel->setText(
            QStringLiteral("Unable to load access data: %1").arg(licenseResult.error().message));
    } else if (!usageResult.hasValue()) {
        m_accessStatusLabel->setText(
            QStringLiteral("Loaded account data, but AI usage status failed: %1")
                .arg(usageResult.error().message));
    } else {
        m_accessStatusLabel->setText(QStringLiteral("No subscriptions or purchases returned for this account."));
    }

    m_accessTable->resizeColumnsToContents();
}

QJsonObject EditorWindow::buildAiProjectContext() const
{
    QJsonObject root;
    root[QStringLiteral("current_frame")] = static_cast<qint64>(m_timeline ? m_timeline->currentFrame() : 0);
    root[QStringLiteral("selected_clip_id")] = m_timeline ? m_timeline->selectedClipId() : QString();
    QJsonArray clips;
    if (m_timeline) {
        for (const TimelineClip& clip : m_timeline->clips()) {
            QJsonObject c;
            c[QStringLiteral("id")] = clip.id;
            c[QStringLiteral("label")] = clip.label;
            c[QStringLiteral("file_path")] = clip.filePath;
            c[QStringLiteral("track_index")] = clip.trackIndex;
            c[QStringLiteral("start_frame")] = static_cast<qint64>(clip.startFrame);
            c[QStringLiteral("duration_frames")] = static_cast<qint64>(clip.durationFrames);
            c[QStringLiteral("has_audio")] = clip.hasAudio;
            c[QStringLiteral("media_type")] = static_cast<int>(clip.mediaType);
            clips.push_back(c);
        }
    }
    root[QStringLiteral("clips")] = clips;
    return root;
}

QString EditorWindow::buildAiUiHierarchySnapshot() const
{
    QStringList lines;
    appendUiHierarchyLines(this, 0, &lines);
    return lines.join(QLatin1Char('\n'));
}

QJsonObject EditorWindow::runAiAction(const QString& action,
                                      const QJsonObject& payload,
                                      bool* okOut,
                                      QString* errorOut)
{
    if (okOut) {
        *okOut = false;
    }
    if (!m_aiIntegrationEnabled) {
        if (errorOut) {
            *errorOut = m_aiIntegrationStatus;
        }
        return {};
    }
    if (m_aiServiceUrl.trimmed().isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("AI service URL is not configured.");
        }
        return {};
    }
    if (m_aiAuthToken.trimmed().isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("AI login required.");
        }
        return {};
    }
    if (m_aiUsageRequests >= m_aiUsageBudgetCap) {
        if (errorOut) {
            *errorOut = QStringLiteral("AI budget exhausted for this project (%1 requests).")
                            .arg(m_aiUsageBudgetCap);
        }
        return {};
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QVector<qint64> recent;
    recent.reserve(m_aiRecentRequestEpochMs.size());
    for (qint64 ts : std::as_const(m_aiRecentRequestEpochMs)) {
        if (nowMs - ts < 60000) {
            recent.push_back(ts);
        }
    }
    m_aiRecentRequestEpochMs = recent;
    if (m_aiRecentRequestEpochMs.size() >= m_aiRateLimitPerMinute) {
        if (errorOut) {
            *errorOut = QStringLiteral("AI rate limit reached (%1 requests/min).")
                            .arg(m_aiRateLimitPerMinute);
        }
        return {};
    }

    QStringList modelCandidates;
    if (!m_aiSelectedModel.trimmed().isEmpty()) {
        modelCandidates.push_back(m_aiSelectedModel.trimmed());
    }
    for (const QString& fallback : std::as_const(m_aiFallbackModels)) {
        const QString normalized = fallback.trimmed();
        if (!normalized.isEmpty() && !modelCandidates.contains(normalized)) {
            modelCandidates.push_back(normalized);
        }
    }
    if (modelCandidates.isEmpty()) {
        modelCandidates.push_back(QStringLiteral("deepseek-chat"));
    }

    QString normalizedBase = m_aiProxyBaseUrl.trimmed();
    while (normalizedBase.endsWith(QLatin1Char('/'))) {
        normalizedBase.chop(1);
    }
    cppmonetize::MonetizeClient client =
        createJCutMonetizeClient(normalizedBase, m_aiRequestTimeoutMs);
    const auto tryRefreshAuthToken = [this]() -> bool {
        Q_UNUSED(this);
        return false;
    };

    QString lastError;
    for (const QString& model : std::as_const(modelCandidates)) {
        for (int attempt = 0; attempt <= m_aiRequestRetries; ++attempt) {
            QJsonObject requestObj;
            requestObj[QStringLiteral("action")] = action;
            requestObj[QStringLiteral("model")] = model;
            requestObj[QStringLiteral("payload")] = payload;
            requestObj[QStringLiteral("context")] = buildAiProjectContext();
            if (m_aiActivityDialog) {
                appendAiActivityLine(
                    QStringLiteral("Request"),
                    QStringLiteral("Submitting %1 via model %2 (attempt %3)")
                        .arg(action, model)
                        .arg(attempt + 1),
                    QString::fromUtf8(QJsonDocument(requestObj).toJson(QJsonDocument::Indented)));
            }

            const auto response = client.submitAiTask(m_aiAuthToken, requestObj);
            if (!response.hasValue()) {
                if (m_aiActivityDialog) {
                    const auto& err = response.error();
                    appendAiActivityLine(
                        QStringLiteral("Response Error"),
                        QStringLiteral("HTTP %1 %2")
                            .arg(err.statusCode)
                            .arg(err.message),
                        QStringLiteral("code=%1\ndetails=%2")
                            .arg(err.code.trimmed().isEmpty() ? QStringLiteral("(none)") : err.code.trimmed(),
                                 err.details.trimmed().isEmpty() ? QStringLiteral("(none)") : err.details.trimmed()));
                }
                if (cppmonetize::isSubscriptionRequiredError(response.error())) {
                    if (errorOut) {
                        *errorOut = QStringLiteral("AI subscription required. Click \"Subscribe to AI\" in AI Assist.");
                    }
                    return {};
                }
                if (cppmonetize::isAuthTokenFailure(response.error())) {
                    if (tryRefreshAuthToken()) {
                        continue;
                    }
                    QString secureStoreError;
                    if (!clearAiTokenFromSecureStore(&secureStoreError) && !secureStoreError.trimmed().isEmpty()) {
                        qWarning() << "Failed clearing AI token after request auth failure:" << secureStoreError;
                    }
                    m_aiAuthToken.clear();
                    m_aiRefreshToken.clear();
                    m_aiUserId.clear();
                    refreshAiIntegrationState();
                    if (errorOut) {
                        *errorOut = QStringLiteral("AI unavailable: credentials invalid or expired. Log In to continue.");
                    }
                    scheduleSaveState();
                    return {};
                }
                lastError = QStringLiteral("AI request failed on %1: %2")
                                .arg(model, response.error().message);
                continue;
            }
            const QJsonObject obj = response.value();
            if (m_aiActivityDialog) {
                appendAiActivityLine(
                    QStringLiteral("Response"),
                    QStringLiteral("Received response for %1 on model %2").arg(action, model),
                    QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Indented)));
            }
            const QJsonObject errObj = obj.value(QStringLiteral("error")).toObject();
            if (!errObj.isEmpty()) {
                const QString code = errObj.value(QStringLiteral("code")).toString().trimmed().toLower();
                const QString message = errObj.value(QStringLiteral("message")).toString().trimmed();
                lastError = QStringLiteral("AI error (%1): %2").arg(code, message);
                const bool retryable =
                    code == QStringLiteral("timeout") ||
                    code == QStringLiteral("rate_limit") ||
                    code == QStringLiteral("service_unavailable");
                if (retryable) {
                    continue;
                }
                break;
            }
            m_aiUsageRequests += 1;
            m_aiRecentRequestEpochMs.push_back(QDateTime::currentMSecsSinceEpoch());
            if (okOut) {
                *okOut = true;
            }
            if (m_aiStatusLabel) {
                m_aiStatusLabel->setText(QStringLiteral("%1 | Usage %2/%3 (fail %4)")
                                             .arg(m_aiIntegrationStatus)
                                             .arg(m_aiUsageRequests)
                                             .arg(m_aiUsageBudgetCap)
                                             .arg(m_aiUsageFailures));
            }
            scheduleSaveState();
            return obj;
        }
    }

    m_aiUsageFailures += 1;
    if (m_aiStatusLabel) {
        m_aiStatusLabel->setText(QStringLiteral("%1 | Usage %2/%3 (fail %4)")
                                     .arg(m_aiIntegrationStatus)
                                     .arg(m_aiUsageRequests)
                                     .arg(m_aiUsageBudgetCap)
                                     .arg(m_aiUsageFailures));
    }
    if (errorOut) {
        *errorOut = lastError.isEmpty() ? QStringLiteral("AI request failed.") : lastError;
    }
    scheduleSaveState();
    return {};
}

void EditorWindow::runAiTranscribeForSelection()
{
    if (!m_timeline || !m_timeline->selectedClip()) {
        QMessageBox::information(this, QStringLiteral("AI Transcribe"), QStringLiteral("Select a clip first."));
        return;
    }
    bool ok = false;
    QString error;
    QJsonObject payload;
    payload[QStringLiteral("clip_file_path")] = m_timeline->selectedClip()->filePath;
    payload[QStringLiteral("clip_label")] = m_timeline->selectedClip()->label;
    const QJsonObject response = runAiAction(QStringLiteral("transcribe_clip"), payload, &ok, &error);
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("AI Transcribe"), error);
        return;
    }
    QMessageBox::information(this,
                             QStringLiteral("AI Transcribe"),
                             response.value(QStringLiteral("message")).toString(
                                 QStringLiteral("Transcription request submitted.")));
}

void EditorWindow::runAiFindSpeakerNames()
{
    ensureAiActivityWindow(QStringLiteral("AI Activity - Mine Transcript"), true);
    appendAiActivityLine(QStringLiteral("Start"), QStringLiteral("Mine Transcript (AI) requested."));
    QCoreApplication::processEvents();

    if (!m_featureAiSpeakerCleanup) {
        appendAiActivityLine(QStringLiteral("Blocked"),
                             QStringLiteral("Feature flag disabled: feature_ai_speaker_cleanup=false"));
        QMessageBox::information(this,
                                 QStringLiteral("Mine Transcript (AI)"),
                                 QStringLiteral("Feature disabled: feature_ai_speaker_cleanup=false"));
        return;
    }
    if (!m_timeline || !m_timeline->selectedClip()) {
        appendAiActivityLine(QStringLiteral("Blocked"), QStringLiteral("No selected clip."));
        QMessageBox::information(this,
                                 QStringLiteral("Mine Transcript (AI)"),
                                 QStringLiteral("Select a clip first."));
        return;
    }
    const TimelineClip* clip = m_timeline->selectedClip();
    if (!clip) {
        return;
    }

    QString transcriptPath = activeTranscriptPathForClipFile(clip->filePath);
    if (transcriptPath.isEmpty()) {
        transcriptPath = transcriptPathForClipFile(clip->filePath);
    }
    appendAiActivityLine(QStringLiteral("Load"),
                         QStringLiteral("Reading transcript JSON: %1").arg(transcriptPath));
    QCoreApplication::processEvents();
    QFile transcriptFile(transcriptPath);
    if (!transcriptFile.open(QIODevice::ReadOnly)) {
        appendAiActivityLine(QStringLiteral("Failed"), QStringLiteral("Transcript JSON file not found/readable."));
        QMessageBox::warning(this,
                             QStringLiteral("Mine Transcript (AI)"),
                             QStringLiteral("No transcript JSON found for selected clip."));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument transcriptDoc = QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !transcriptDoc.isObject()) {
        appendAiActivityLine(QStringLiteral("Failed"), QStringLiteral("Transcript JSON parse failed."));
        QMessageBox::warning(this,
                             QStringLiteral("Mine Transcript (AI)"),
                             QStringLiteral("Transcript JSON is invalid."));
        return;
    }

    const QJsonObject root = transcriptDoc.object();
    const QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
    if (segments.isEmpty()) {
        appendAiActivityLine(QStringLiteral("Failed"), QStringLiteral("Transcript has no segments."));
        QMessageBox::warning(this,
                             QStringLiteral("Mine Transcript (AI)"),
                             QStringLiteral("Transcript has no segments to mine."));
        return;
    }
    appendAiActivityLine(QStringLiteral("Prepare"),
                         QStringLiteral("Loaded transcript with %1 segment(s).").arg(segments.size()));
    QCoreApplication::processEvents();

    QHash<QString, QString> speakerIdToNumber;
    QHash<QString, QString> numberToSpeakerId;
    int nextSpeakerNumber = 1;
    auto ensureSpeakerNumber = [&](const QString& speakerId) -> QString {
        const QString trimmed = speakerId.trimmed();
        if (trimmed.isEmpty()) {
            return QString();
        }
        auto it = speakerIdToNumber.constFind(trimmed);
        if (it != speakerIdToNumber.constEnd()) {
            return it.value();
        }
        const QString numberToken = QStringLiteral("S%1").arg(nextSpeakerNumber++);
        speakerIdToNumber.insert(trimmed, numberToken);
        numberToSpeakerId.insert(numberToken.toUpper(), trimmed);
        return numberToken;
    };

    QString transcriptText;
    transcriptText.reserve(20000);
    QString currentNumberToken;
    for (const QJsonValue& segValue : segments) {
        const QJsonObject segObj = segValue.toObject();
        const QString segmentSpeaker = segObj.value(QStringLiteral("speaker")).toString().trimmed();
        const QJsonArray words = segObj.value(QStringLiteral("words")).toArray();
        if (words.isEmpty()) {
            const QString text = segObj.value(QStringLiteral("text")).toString().trimmed();
            const QString numberToken = ensureSpeakerNumber(segmentSpeaker);
            if (!numberToken.isEmpty() && numberToken != currentNumberToken) {
                if (!transcriptText.isEmpty()) {
                    transcriptText += QLatin1Char('\n');
                }
                transcriptText += QStringLiteral("[%1]\n").arg(numberToken);
                currentNumberToken = numberToken;
            }
            if (!text.isEmpty()) {
                if (!transcriptText.isEmpty() &&
                    !transcriptText.endsWith(QLatin1Char('\n')) &&
                    !transcriptText.endsWith(QLatin1Char(' '))) {
                    transcriptText += QLatin1Char(' ');
                }
                transcriptText += text;
            }
            continue;
        }
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                continue;
            }
            QString speakerId = wordObj.value(QStringLiteral("speaker")).toString().trimmed();
            if (speakerId.isEmpty()) {
                speakerId = segmentSpeaker;
            }
            const QString numberToken = ensureSpeakerNumber(speakerId);
            if (!numberToken.isEmpty() && numberToken != currentNumberToken) {
                if (!transcriptText.isEmpty()) {
                    transcriptText += QLatin1Char('\n');
                }
                transcriptText += QStringLiteral("[%1]\n").arg(numberToken);
                currentNumberToken = numberToken;
            }
            QString word = wordObj.value(QStringLiteral("word")).toString().trimmed();
            word.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
            if (word.isEmpty()) {
                continue;
            }
            if (!transcriptText.isEmpty() &&
                !transcriptText.endsWith(QLatin1Char('\n')) &&
                !transcriptText.endsWith(QLatin1Char(' '))) {
                transcriptText += QLatin1Char(' ');
            }
            transcriptText += word;
        }
    }

    if (transcriptText.trimmed().isEmpty() || numberToSpeakerId.isEmpty()) {
        appendAiActivityLine(QStringLiteral("Failed"),
                             QStringLiteral("Transcript text empty after preprocessing."));
        QMessageBox::warning(this,
                             QStringLiteral("Mine Transcript (AI)"),
                             QStringLiteral("Transcript text is empty after preprocessing."));
        return;
    }
    appendAiActivityLine(
        QStringLiteral("Prepare"),
        QStringLiteral("Built AI payload from %1 speaker token(s).").arg(numberToSpeakerId.size()));
    QCoreApplication::processEvents();

    QJsonArray speakerLegend;
    for (auto it = numberToSpeakerId.constBegin(); it != numberToSpeakerId.constEnd(); ++it) {
        QJsonObject row;
        row[QStringLiteral("speaker_number")] = it.key();
        row[QStringLiteral("source_speaker_id")] = it.value();
        speakerLegend.push_back(row);
    }

    QJsonObject payload;
    payload[QStringLiteral("task")] = QStringLiteral("mine_speaker_profiles");
    payload[QStringLiteral("format")] = QStringLiteral("json_array");
    payload[QStringLiteral("instructions")] =
        QStringLiteral("You are given transcript text with speaker changes marked as [S#]. "
                       "Return JSON array only. Each item must have keys: "
                       "Speaker, Name, Organization, Summary. "
                       "Speaker must be S# from the transcript markers. "
                       "Name should be a likely human-readable speaker name when inferable, else empty.");
    payload[QStringLiteral("transcript_text")] = transcriptText;
    payload[QStringLiteral("speaker_legend")] = speakerLegend;
    appendAiActivityLine(QStringLiteral("AI Request"),
                         QStringLiteral("Submitting transcript mining request to AI gateway..."));
    QCoreApplication::processEvents();
    bool ok = false;
    QString error;
    QElapsedTimer aiTimer;
    aiTimer.start();
    const QJsonObject response = runAiAction(
        QStringLiteral("mine_transcript_speakers"), payload, &ok, &error);
    appendAiActivityLine(QStringLiteral("AI Response"),
                         QStringLiteral("Gateway responded in %1 ms.").arg(aiTimer.elapsed()));
    QCoreApplication::processEvents();
    if (!ok) {
        appendAiActivityLine(QStringLiteral("Failed"), QStringLiteral("AI request failed: %1").arg(error));
        QMessageBox::warning(this, QStringLiteral("Mine Transcript (AI)"), error);
        return;
    }

    auto extractArray = [](const QJsonObject& obj) -> QJsonArray {
        const QJsonArray direct = obj.value(QStringLiteral("speakers")).toArray();
        if (!direct.isEmpty()) {
            return direct;
        }
        const QJsonObject resultObj = obj.value(QStringLiteral("result")).toObject();
        const QJsonArray resultSpeakers = resultObj.value(QStringLiteral("speakers")).toArray();
        if (!resultSpeakers.isEmpty()) {
            return resultSpeakers;
        }
        const QJsonArray resultItems = resultObj.value(QStringLiteral("items")).toArray();
        if (!resultItems.isEmpty()) {
            return resultItems;
        }
        const QJsonObject payloadObj = obj.value(QStringLiteral("payload")).toObject();
        const QJsonArray payloadSpeakers = payloadObj.value(QStringLiteral("speakers")).toArray();
        if (!payloadSpeakers.isEmpty()) {
            return payloadSpeakers;
        }
        const QStringList textKeys{
            QStringLiteral("text"),
            QStringLiteral("output"),
            QStringLiteral("content"),
            QStringLiteral("message")};
        for (const QString& key : textKeys) {
            const QString text = obj.value(key).toString().trimmed();
            if (text.isEmpty()) {
                continue;
            }
            const int startBracket = text.indexOf(QLatin1Char('['));
            const int endBracket = text.lastIndexOf(QLatin1Char(']'));
            if (startBracket < 0 || endBracket <= startBracket) {
                continue;
            }
            const QByteArray jsonBytes =
                text.mid(startBracket, endBracket - startBracket + 1).toUtf8();
            QJsonParseError parseErr;
            const QJsonDocument parsed = QJsonDocument::fromJson(jsonBytes, &parseErr);
            if (parseErr.error == QJsonParseError::NoError && parsed.isArray()) {
                return parsed.array();
            }
        }
        return {};
    };

    const QJsonArray minedRows = extractArray(response);
    if (minedRows.isEmpty()) {
        appendAiActivityLine(QStringLiteral("Failed"),
                             QStringLiteral("AI response was not a parsable JSON list."));
        QMessageBox::warning(
            this,
            QStringLiteral("Mine Transcript (AI)"),
            QStringLiteral("AI did not return a parsable JSON list of speaker profiles."));
        return;
    }
    appendAiActivityLine(QStringLiteral("Parse"),
                         QStringLiteral("Parsed %1 row(s) from AI response.").arg(minedRows.size()));
    QCoreApplication::processEvents();

    QJsonObject nextRoot = root;
    QJsonObject profiles = nextRoot.value(QStringLiteral("speaker_profiles")).toObject();
    int appliedRows = 0;
    for (const QJsonValue& value : minedRows) {
        const QJsonObject row = value.toObject();
        if (row.isEmpty()) {
            continue;
        }
        QString speakerToken = row.value(QStringLiteral("Speaker")).toString().trimmed();
        if (speakerToken.isEmpty()) {
            speakerToken = row.value(QStringLiteral("speaker")).toString().trimmed();
        }
        QString organization = row.value(QStringLiteral("Organization")).toString().trimmed();
        if (organization.isEmpty()) {
            organization = row.value(QStringLiteral("organization")).toString().trimmed();
        }
        QString inferredName = row.value(QStringLiteral("Name")).toString().trimmed();
        if (inferredName.isEmpty()) {
            inferredName = row.value(QStringLiteral("name")).toString().trimmed();
        }
        if (inferredName.isEmpty()) {
            inferredName = row.value(QStringLiteral("DisplayName")).toString().trimmed();
        }
        if (inferredName.isEmpty()) {
            inferredName = row.value(QStringLiteral("display_name")).toString().trimmed();
        }
        QString summary = row.value(QStringLiteral("Summary")).toString().trimmed();
        if (summary.isEmpty()) {
            summary = row.value(QStringLiteral("summary")).toString().trimmed();
        }
        QString sourceSpeakerId = numberToSpeakerId.value(speakerToken.toUpper());
        if (sourceSpeakerId.isEmpty() && speakerIdToNumber.contains(speakerToken)) {
            sourceSpeakerId = speakerToken;
        }
        if (sourceSpeakerId.isEmpty()) {
            continue;
        }

        const QJsonObject baseProfile = profiles.value(sourceSpeakerId).toObject();
        SpeakerProfile profile = speakerProfileFromJson(sourceSpeakerId, baseProfile);
        const bool tokenIsNumber = QRegularExpression(QStringLiteral("^S\\d+$"),
                                                      QRegularExpression::CaseInsensitiveOption)
                                       .match(speakerToken)
                                       .hasMatch();
        if (!inferredName.isEmpty()) {
            profile.name = inferredName;
        } else if (!speakerToken.isEmpty() && !tokenIsNumber) {
            profile.name = speakerToken;
        } else if (profile.name.trimmed().isEmpty()) {
            profile.name = sourceSpeakerId;
        }
        if (!organization.isEmpty()) {
            profile.organization = organization;
        }
        if (!summary.isEmpty()) {
            profile.description = summary;
        }
        profiles[sourceSpeakerId] = speakerProfileToJson(profile, baseProfile);
        ++appliedRows;
    }

    if (appliedRows <= 0) {
        appendAiActivityLine(QStringLiteral("Failed"),
                             QStringLiteral("No AI rows matched transcript speaker IDs."));
        QMessageBox::warning(
            this,
            QStringLiteral("Mine Transcript (AI)"),
            QStringLiteral("AI response could not be matched to transcript speaker IDs."));
        return;
    }

    nextRoot[QStringLiteral("speaker_profiles")] = profiles;
    QJsonDocument nextDoc(nextRoot);
    editor::TranscriptEngine engine;
    if (!engine.saveTranscriptJson(transcriptPath, nextDoc)) {
        appendAiActivityLine(QStringLiteral("Failed"), QStringLiteral("Failed saving updated transcript JSON."));
        QMessageBox::warning(this,
                             QStringLiteral("Mine Transcript (AI)"),
                             QStringLiteral("Failed to save transcript after AI mining."));
        return;
    }
    appendAiActivityLine(QStringLiteral("Save"),
                         QStringLiteral("Saved transcript updates to disk."));
    QCoreApplication::processEvents();

    invalidateTranscriptSpeakerProfileCache(transcriptPath);
    m_transcriptEngine.invalidateCache();
    if (m_transcriptTab) {
        m_transcriptTab->refresh();
    }
    if (m_speakersTab) {
        m_speakersTab->refresh();
    }
    if (m_preview) {
        m_preview->invalidateTranscriptOverlayCache(clip->filePath);
        m_preview->asWidget()->update();
    }
    scheduleSaveState();
    pushHistorySnapshot();
    appendAiActivityLine(
        QStringLiteral("Done"),
        QStringLiteral("Updated %1 speaker profile entr%2.")
            .arg(appliedRows)
            .arg(appliedRows == 1 ? QStringLiteral("y") : QStringLiteral("ies")));
    QMessageBox::information(
        this,
        QStringLiteral("Mine Transcript (AI)"),
        QStringLiteral("Updated %1 speaker profile entries from transcript mining.")
            .arg(appliedRows));
}

void EditorWindow::runAiFindOrganizations()
{
    if (!m_featureAiSpeakerCleanup) {
        QMessageBox::information(this,
                                 QStringLiteral("Find Organizations (AI)"),
                                 QStringLiteral("Feature disabled: feature_ai_speaker_cleanup=false"));
        return;
    }
    if (!m_speakersTab) {
        return;
    }
    m_speakersTab->runAiFindOrganizations();
}

void EditorWindow::runAiCleanAssignments()
{
    if (!m_featureAiSpeakerCleanup) {
        QMessageBox::information(this,
                                 QStringLiteral("Clean Assignments (AI)"),
                                 QStringLiteral("Feature disabled: feature_ai_speaker_cleanup=false"));
        return;
    }
    if (!m_speakersTab) {
        return;
    }
    m_speakersTab->runAiCleanSpuriousAssignments();
}

void EditorWindow::runAiSubscribeCheckout()
{
    refreshAiIntegrationState();
    if (!m_featureAiPanel) {
        QMessageBox::information(this,
                                 QStringLiteral("Subscribe to AI"),
                                 QStringLiteral("Feature disabled: feature_ai_panel=false"));
        return;
    }
    if (m_aiAuthToken.trimmed().isEmpty()) {
        QMessageBox::information(this,
                                 QStringLiteral("Subscribe to AI"),
                                 QStringLiteral("Log In first using the top-right account button."));
        return;
    }

    QString gatewayBase = normalizeBaseUrl(m_aiProxyBaseUrl);
    if (gatewayBase.isEmpty()) {
        gatewayBase = normalizeBaseUrl(qEnvironmentVariable("SUPABASE_URL"));
    }
    if (gatewayBase.isEmpty()) {
        gatewayBase = normalizeBaseUrl(QString::fromLatin1(kDefaultSupabaseGateway));
    }
    if (gatewayBase.isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("Subscribe to AI"),
                             QStringLiteral("Gateway base URL is not configured."));
        return;
    }

    QStringList productSlugs;
    const QString envSlug = qEnvironmentVariable("JCUT_AI_SUBSCRIPTION_SLUG").trimmed();
    if (!envSlug.isEmpty()) {
        productSlugs.push_back(envSlug);
    }
    for (const QString& fallback : {QStringLiteral("jsynth-pro-subscription"),
                                    QStringLiteral("jcut-ai-subscription"),
                                    QStringLiteral("ai-platform")}) {
        if (!productSlugs.contains(fallback)) {
            productSlugs.push_back(fallback);
        }
    }

    QString normalizedBase = gatewayBase;
    while (normalizedBase.endsWith(QLatin1Char('/'))) {
        normalizedBase.chop(1);
    }
    cppmonetize::MonetizeClient client =
        createJCutMonetizeClient(normalizedBase, m_aiRequestTimeoutMs);

    cppmonetize::ApiError lastError;
    for (const QString& slug : std::as_const(productSlugs)) {
        const auto checkout = client.createCheckoutForSlug(m_aiAuthToken, slug);
        if (!checkout.hasValue()) {
            lastError = checkout.error();
            continue;
        }
        const QUrl checkoutUrl = checkout.value().checkoutUrl;
        if (!checkoutUrl.isValid() || checkoutUrl.isEmpty()) {
            continue;
        }
        if (!QDesktopServices::openUrl(checkoutUrl)) {
            QMessageBox::warning(this,
                                 QStringLiteral("Subscribe to AI"),
                                 QStringLiteral("Failed to open checkout URL:\n%1").arg(checkoutUrl.toString()));
            return;
        }
        QMessageBox::information(
            this,
            QStringLiteral("Subscribe to AI"),
            QStringLiteral("Opened checkout in browser.\n\nComplete purchase, then return and click top-right Log In."));
        return;
    }

    const QString errMsg = lastError.message.trimmed().isEmpty()
        ? QStringLiteral("Unable to start checkout for known subscription product slugs.")
        : lastError.message.trimmed();
    QMessageBox::warning(
        this,
        QStringLiteral("Subscribe to AI"),
        QStringLiteral("Unable to open subscription checkout.\n\n%1").arg(errMsg));
}

void EditorWindow::appendAiChatLine(const QString& role, const QString& text)
{
    if (!m_aiChatHistoryEdit) {
        return;
    }
    const QString trimmedRole = role.trimmed().isEmpty() ? QStringLiteral("Assistant") : role.trimmed();
    const QString trimmedText = text.trimmed().isEmpty() ? QStringLiteral("(empty)") : text.trimmed();
    QString roleColor = QStringLiteral("#9ec3ff");
    if (trimmedRole.compare(QStringLiteral("Assistant"), Qt::CaseInsensitive) == 0) {
        roleColor = QStringLiteral("#8ee59a");
    } else if (trimmedRole.compare(QStringLiteral("System"), Qt::CaseInsensitive) == 0) {
        roleColor = QStringLiteral("#f2d179");
    } else if (trimmedRole.compare(QStringLiteral("Error"), Qt::CaseInsensitive) == 0) {
        roleColor = QStringLiteral("#ff9a9a");
    }
    const QString html = QStringLiteral(
        "<div style=\"margin:0 0 10px 0;\">"
        "<div style=\"color:%1;font-weight:600;\">%2</div>"
        "<div style=\"white-space:pre-wrap;color:#d6dee8;\">%3</div>"
        "</div>")
                             .arg(roleColor,
                                  trimmedRole.toHtmlEscaped(),
                                  trimmedText.toHtmlEscaped());
    m_aiChatHistoryEdit->append(html);
    if (QScrollBar* bar = m_aiChatHistoryEdit->verticalScrollBar()) {
        bar->setValue(bar->maximum());
    }
    if (m_aiChatClearButton) {
        m_aiChatClearButton->setEnabled(true);
    }
}

void EditorWindow::ensureAiActivityWindow(const QString& title, bool clearLog)
{
    QWidget* dialogWidget = m_aiActivityDialog.data();
    if (!dialogWidget) {
        auto* dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose, true);
        dialog->resize(760, 460);
        auto* layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        auto* logTree = new QTreeWidget(dialog);
        logTree->setObjectName(QStringLiteral("ai.activity.log_tree"));
        logTree->setColumnCount(3);
        logTree->setHeaderLabels(
            QStringList{QStringLiteral("Time"), QStringLiteral("Phase"), QStringLiteral("Summary")});
        logTree->setRootIsDecorated(true);
        logTree->setAlternatingRowColors(true);
        logTree->setUniformRowHeights(false);
        logTree->setWordWrap(true);
        logTree->setIndentation(20);
        logTree->setStyleSheet(QStringLiteral(
            "QTreeWidget { background:#0f1720; color:#d8e6f5; alternate-background-color:#121f2c; }"
            "QTreeWidget::item { padding:4px 2px; }"
            "QHeaderView::section { background:#162433; color:#a7c0d8; padding:4px; border:0; }"));
        logTree->header()->setStretchLastSection(true);
        logTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        logTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        logTree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
        layout->addWidget(logTree, 1);

        auto* closeButton = new QPushButton(QStringLiteral("Close"), dialog);
        connect(closeButton, &QPushButton::clicked, dialog, &QDialog::close);
        layout->addWidget(closeButton, 0, Qt::AlignRight);

        connect(dialog, &QObject::destroyed, this, [this]() {
            m_aiActivityDialog = nullptr;
            m_aiActivityLogTree = nullptr;
        });

        m_aiActivityDialog = dialog;
        m_aiActivityLogTree = logTree;
        dialogWidget = dialog;
    }

    dialogWidget->setWindowTitle(title.trimmed().isEmpty() ? QStringLiteral("AI Activity") : title.trimmed());
    dialogWidget->show();
    dialogWidget->raise();
    dialogWidget->activateWindow();
    if (clearLog && m_aiActivityLogTree) {
        m_aiActivityLogTree->clear();
    }
}

void EditorWindow::appendAiActivityLine(const QString& phase,
                                        const QString& details,
                                        const QString& exactText)
{
    ensureAiActivityWindow(QStringLiteral("AI Activity"));
    if (!m_aiActivityLogTree) {
        return;
    }
    const QString safePhase = phase.trimmed().isEmpty() ? QStringLiteral("Info") : phase.trimmed();
    const QString safeDetails = details.trimmed().isEmpty() ? QStringLiteral("(no details)") : details.trimmed();
    const QString safeExact = exactText.trimmed();
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    auto* top = new QTreeWidgetItem(m_aiActivityLogTree);
    top->setText(0, timestamp);
    top->setText(1, safePhase);
    top->setText(2, safeDetails);
    top->setToolTip(2, safeDetails);
    if (!safeExact.isEmpty()) {
        top->setText(2, QStringLiteral("%1  [▶ Expand exact text]").arg(aiActivityPreviewText(safeDetails)));
        top->setToolTip(2, safeDetails);
        auto* child = new QTreeWidgetItem(top);
        child->setText(1, QStringLiteral("Exact Payload"));
        child->setText(2, safeExact);
        child->setToolTip(2, safeExact);
        QFont mono = child->font(2);
        mono.setFamily(QStringLiteral("Monospace"));
        mono.setStyleHint(QFont::TypeWriter);
        child->setFont(2, mono);
        top->setExpanded(false);
    }
    m_aiActivityLogTree->scrollToItem(top);
    if (QScrollBar* bar = m_aiActivityLogTree->verticalScrollBar()) {
        bar->setValue(bar->maximum());
    }
}

void EditorWindow::runAiChatPrompt()
{
    if (!m_aiChatInputLineEdit) {
        return;
    }
    QString prompt = m_aiChatInputLineEdit->toPlainText().trimmed();
    if (prompt.isEmpty()) {
        return;
    }
    const bool includeUi = prompt.contains(QStringLiteral("@ui"), Qt::CaseInsensitive);
    prompt.replace(QStringLiteral("@ui"), QString(), Qt::CaseInsensitive);
    prompt = prompt.trimmed();
    if (prompt.isEmpty()) {
        appendAiChatLine(
            QStringLiteral("System"),
            QStringLiteral("Please enter a prompt. `@ui` alone sends no question."));
        return;
    }

    if (includeUi) {
        appendAiChatLine(
            QStringLiteral("System"),
            QStringLiteral("Attached full UI hierarchy context for this turn."));
    }
    appendAiChatLine(QStringLiteral("You"), prompt);
    m_aiChatInputLineEdit->setPlainText(QString());

    m_aiChatMessages.push_back({QStringLiteral("user"), prompt});
    while (m_aiChatMessages.size() > m_aiChatMaxMessages) {
        m_aiChatMessages.removeFirst();
    }

    QString conversationContext;
    for (const AiChatMessage& msg : std::as_const(m_aiChatMessages)) {
        const QString roleTag = msg.role.compare(QStringLiteral("assistant"), Qt::CaseInsensitive) == 0
            ? QStringLiteral("ASSISTANT")
            : QStringLiteral("USER");
        conversationContext += QStringLiteral("[%1]\n%2\n\n").arg(roleTag, msg.content);
    }
    if (includeUi) {
        conversationContext += QStringLiteral("[UI_HIERARCHY]\n");
        conversationContext += buildAiUiHierarchySnapshot();
        conversationContext += QStringLiteral("\n\n");
    }

    QJsonObject payload;
    payload[QStringLiteral("task")] = QStringLiteral("chat_agent");
    payload[QStringLiteral("instructions")] =
        QStringLiteral("You are JCut Agent. You are stateful across turns. "
                       "Use the provided conversation and optional UI hierarchy to answer accurately. "
                       "Give concise, actionable responses.");
    payload[QStringLiteral("transcript_text")] = conversationContext;
    bool ok = false;
    QString error;
    const QString savedModel = m_aiSelectedModel;
    m_aiSelectedModel = QStringLiteral("deepseek-chat");
    const QJsonObject response = runAiAction(QStringLiteral("chat"), payload, &ok, &error);
    m_aiSelectedModel = savedModel;
    if (!ok) {
        appendAiChatLine(QStringLiteral("Error"), QStringLiteral("AI request failed: %1").arg(error));
        return;
    }

    QString reply = response.value(QStringLiteral("text")).toString().trimmed();
    if (reply.isEmpty()) {
        reply = response.value(QStringLiteral("message")).toString().trimmed();
    }
    if (reply.isEmpty()) {
        reply = response.value(QStringLiteral("output")).toString().trimmed();
    }
    if (reply.isEmpty()) {
        const QJsonObject resultObj = response.value(QStringLiteral("result")).toObject();
        reply = resultObj.value(QStringLiteral("text")).toString().trimmed();
        if (reply.isEmpty()) {
            reply = resultObj.value(QStringLiteral("message")).toString().trimmed();
        }
    }
    if (reply.isEmpty()) {
        reply = QStringLiteral("No text response returned.");
    }
    m_aiChatMessages.push_back({QStringLiteral("assistant"), reply});
    while (m_aiChatMessages.size() > m_aiChatMaxMessages) {
        m_aiChatMessages.removeFirst();
    }
    appendAiChatLine(QStringLiteral("Assistant"), reply);
}
