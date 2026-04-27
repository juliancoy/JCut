#include "editor.h"
#include "editor_ai_helpers.h"

#include <cppmonetize/AuthIdentity.h>
#include <cppmonetize/MonetizeClient.h>
#include <cppmonetize/OAuthDesktopFlow.h>
#include <cppmonetize/TokenStore.h>

#include <QDateTime>
#include <QMessageBox>
#include <QMetaEnum>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QUrl>
#include <QWidget>
#include <QAbstractButton>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QTabWidget>
#include <QTextBrowser>

namespace {

constexpr auto kDefaultSupabaseGateway = "https://ivwutugdrpugjqglxabw.supabase.co";
bool isSupabaseProjectBase(const QString& baseUrl);
bool isAuthTokenFailure(const cppmonetize::ApiError& error);

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

bool isAuthTokenFailure(const cppmonetize::ApiError& error)
{
    if (error.statusCode == 401 || error.statusCode == 403) {
        return true;
    }
    const QString message = error.message.trimmed().toLower();
    const QString details = error.details.trimmed().toLower();
    const QString code = error.code.trimmed().toLower();
    return message.contains(QStringLiteral("invalid jwt")) ||
           message.contains(QStringLiteral("token")) ||
           message.contains(QStringLiteral("unauthorized")) ||
           message.contains(QStringLiteral("expired")) ||
           details.contains(QStringLiteral("invalid jwt")) ||
           details.contains(QStringLiteral("token")) ||
           code == QStringLiteral("invalid_jwt") ||
           code == QStringLiteral("unauthorized");
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
    QString base = m_aiProxyBaseUrl.trimmed();
    if (base.isEmpty()) {
        base = QStringLiteral("unset-gateway");
    }
    QByteArray keyBytes = base.toUtf8().toHex();
    if (keyBytes.size() > 64) {
        keyBytes = keyBytes.left(64);
    }
    return QStringLiteral("jcut.ai.%1").arg(QString::fromLatin1(keyBytes));
}

bool EditorWindow::readAiTokenFromSecureStore(QString* tokenOut) const
{
    if (tokenOut) {
        tokenOut->clear();
    }
    if (m_aiProxyBaseUrl.trimmed().isEmpty()) {
        return false;
    }

    cppmonetize::TokenStoreConfig cfg;
    cfg.appName = QStringLiteral("jcut");
    cfg.orgName = QStringLiteral("jcut");
    cfg.serviceName = aiSecureStoreServiceName();
    auto store = cppmonetize::createDefaultTokenStore(cfg);
    const auto tokenResult = store->loadToken();
    if (!tokenResult.hasValue()) {
        return false;
    }
    const QString token = tokenResult.value().trimmed();
    if (token.isEmpty()) {
        return false;
    }
    if (tokenOut) {
        *tokenOut = token;
    }
    return true;
}

bool EditorWindow::writeAiTokenToSecureStore(const QString& token, QString* errorOut) const
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
    if (m_aiProxyBaseUrl.trimmed().isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Gateway URL is required before storing token.");
        }
        return false;
    }

    cppmonetize::TokenStoreConfig cfg;
    cfg.appName = QStringLiteral("jcut");
    cfg.orgName = QStringLiteral("jcut");
    cfg.serviceName = aiSecureStoreServiceName();
    auto store = cppmonetize::createDefaultTokenStore(cfg);
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
    if (m_aiProxyBaseUrl.trimmed().isEmpty()) {
        return true;
    }
    cppmonetize::TokenStoreConfig cfg;
    cfg.appName = QStringLiteral("jcut");
    cfg.orgName = QStringLiteral("jcut");
    cfg.serviceName = aiSecureStoreServiceName();
    auto store = cppmonetize::createDefaultTokenStore(cfg);
    const auto clearResult = store->clear();
    if (!clearResult.hasValue()) {
        if (errorOut) {
            *errorOut = clearResult.error().message;
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
    const QString displayIdentity = aiDisplayIdentity(m_aiUserId, m_aiAuthToken);

    if (!loggedIn) {
        m_profileAvatarButton->setText(QStringLiteral("Login"));
        m_profileAvatarButton->setIcon(QIcon());
        m_profileAvatarButton->setStyleSheet(QString());
        m_profileAvatarButton->setMinimumSize(0, 30);
        m_profileAvatarButton->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        m_profileAvatarButton->setToolTip(QStringLiteral("Sign in to sync purchases and licenses"));
    } else {
        const QString avatarSeed = displayIdentity.isEmpty() ? QStringLiteral("user@jcut") : displayIdentity;
        m_profileAvatarButton->setText(QString());
        m_profileAvatarButton->setIcon(QIcon(buildFallbackAvatar(avatarSeed)));
        m_profileAvatarButton->setIconSize(QSize(28, 28));
        m_profileAvatarButton->setFixedSize(34, 34);
        m_profileAvatarButton->setStyleSheet(QStringLiteral(
            "QPushButton#tabs\\.profile_avatar_button {"
            " border: 1px solid #3a4359; border-radius: 17px; padding: 0;"
            " background: #223041; }"
            "QPushButton#tabs\\.profile_avatar_button:hover { border-color: #6ea8ff; background: #2b3c50; }"));
        m_profileAvatarButton->setToolTip(
            displayIdentity.isEmpty()
                ? QStringLiteral("Signed in")
                : QStringLiteral("Signed in as %1").arg(displayIdentity));
    }
    m_profileAvatarButton->setVisible(true);
    m_profileAvatarButton->raise();
}

void EditorWindow::onProfileAvatarButtonClicked()
{
    if (m_aiAuthToken.trimmed().isEmpty()) {
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
        QMessageBox::warning(this,
                             QStringLiteral("AI Login"),
                             loginResult.error().message.isEmpty()
                                 ? QStringLiteral("OAuth callback did not return a token.")
                                 : loginResult.error().message);
        finishLoginAttempt();
        return;
    }
    const QString token = loginResult.value().token.trimmed();
    const QString email = loginResult.value().email.trimmed();
    if (token.isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("AI Login"),
                             QStringLiteral("OAuth callback did not return a token."));
        finishLoginAttempt();
        return;
    }

    m_aiProxyBaseUrl = gatewayBase;
    m_aiAuthToken = token.trimmed();
    QString bestUser = email.trimmed();
    if (bestUser.isEmpty()) {
        bestUser = cppmonetize::parseAccessTokenIdentity(m_aiAuthToken).displayIdentity();
    }
    m_aiUserId = bestUser;

    QString secureStoreError;
    if (!writeAiTokenToSecureStore(m_aiAuthToken, &secureStoreError)) {
        QMessageBox::warning(this,
                             QStringLiteral("AI Login"),
                             QStringLiteral("Token saved for this session only. Secure storage failed: %1")
                                 .arg(secureStoreError));
    }
    refreshAiIntegrationState();
    scheduleSaveState();
    finishLoginAttempt();
}

void EditorWindow::clearAiGatewayLogin()
{
    QString secureStoreError;
    if (!clearAiTokenFromSecureStore(&secureStoreError)) {
        qWarning() << "Failed clearing AI token from secure store:" << secureStoreError;
    }
    m_aiAuthToken.clear();
    m_aiUserId.clear();
    refreshAiIntegrationState();
    scheduleSaveState();
}

void EditorWindow::refreshAiIntegrationState()
{
    m_aiContractVersion = QStringLiteral("unknown");
    QString status = QStringLiteral("AI disabled: login required");
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
        if (readAiTokenFromSecureStore(&secureToken)) {
            m_aiAuthToken = secureToken;
        } else {
            m_aiAuthToken = envToken;
        }
    }

    const QString base = m_aiProxyBaseUrl.trimmed();
    if (!m_featureAiPanel) {
        status = QStringLiteral("AI disabled: feature_ai_panel=false");
    } else if (base.isEmpty()) {
        status = QStringLiteral("AI disabled: configure gateway URL");
    } else if (m_aiAuthToken.trimmed().isEmpty()) {
        status = QStringLiteral("AI disabled: sign in (missing access token)");
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
        const auto entResult = client.getAiEntitlements(m_aiAuthToken);
        if (!entResult.hasValue()) {
            const cppmonetize::ApiError error = entResult.error();
            if (isAuthTokenFailure(error)) {
                QString secureStoreError;
                if (!clearAiTokenFromSecureStore(&secureStoreError) && !secureStoreError.trimmed().isEmpty()) {
                    qWarning() << "Failed clearing AI token after auth failure:" << secureStoreError;
                }
                m_aiAuthToken.clear();
                m_aiUserId.clear();
                status = QStringLiteral("AI disabled: sign in again (session expired)");
            } else {
                status = QStringLiteral("AI disabled: entitlement check failed (%1)")
                             .arg(error.message);
            }
        } else {
            const cppmonetize::AiEntitlements ent = entResult.value();
            applyEntitlements(ent);
        }
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
    if (m_aiLoginButton) {
        m_aiLoginButton->setEnabled(m_featureAiPanel);
    }
    if (m_aiLogoutButton) {
        m_aiLogoutButton->setEnabled(!m_aiAuthToken.isEmpty());
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
    updateProfileAvatarButton();
    if (m_aiStatusLabel) {
        m_aiStatusLabel->setText(QStringLiteral("%1 | Usage %2/%3 (fail %4)")
                                     .arg(status)
                                     .arg(m_aiUsageRequests)
                                     .arg(m_aiUsageBudgetCap)
                                     .arg(m_aiUsageFailures));
    }
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

    QString lastError;
    for (const QString& model : std::as_const(modelCandidates)) {
        for (int attempt = 0; attempt <= m_aiRequestRetries; ++attempt) {
            QJsonObject requestObj;
            requestObj[QStringLiteral("action")] = action;
            requestObj[QStringLiteral("model")] = model;
            requestObj[QStringLiteral("payload")] = payload;
            requestObj[QStringLiteral("context")] = buildAiProjectContext();

            const auto response = client.submitAiTask(m_aiAuthToken, requestObj);
            if (!response.hasValue()) {
                if (isAuthTokenFailure(response.error())) {
                    QString secureStoreError;
                    if (!clearAiTokenFromSecureStore(&secureStoreError) && !secureStoreError.trimmed().isEmpty()) {
                        qWarning() << "Failed clearing AI token after request auth failure:" << secureStoreError;
                    }
                    m_aiAuthToken.clear();
                    m_aiUserId.clear();
                    refreshAiIntegrationState();
                    if (errorOut) {
                        *errorOut = QStringLiteral("AI login expired. Please sign in again.");
                    }
                    scheduleSaveState();
                    return {};
                }
                lastError = QStringLiteral("AI request failed on %1: %2")
                                .arg(model, response.error().message);
                continue;
            }
            const QJsonObject obj = response.value();
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
    if (!m_featureAiSpeakerCleanup) {
        QMessageBox::information(this,
                                 QStringLiteral("Mine Transcript (AI)"),
                                 QStringLiteral("Feature disabled: feature_ai_speaker_cleanup=false"));
        return;
    }
    if (!m_timeline || !m_timeline->selectedClip()) {
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
    QFile transcriptFile(transcriptPath);
    if (!transcriptFile.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this,
                             QStringLiteral("Mine Transcript (AI)"),
                             QStringLiteral("No transcript JSON found for selected clip."));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument transcriptDoc = QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !transcriptDoc.isObject()) {
        QMessageBox::warning(this,
                             QStringLiteral("Mine Transcript (AI)"),
                             QStringLiteral("Transcript JSON is invalid."));
        return;
    }

    const QJsonObject root = transcriptDoc.object();
    const QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
    if (segments.isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("Mine Transcript (AI)"),
                             QStringLiteral("Transcript has no segments to mine."));
        return;
    }

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
        QMessageBox::warning(this,
                             QStringLiteral("Mine Transcript (AI)"),
                             QStringLiteral("Transcript text is empty after preprocessing."));
        return;
    }

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
                       "Speaker, Organization, Summary. "
                       "Speaker must be S# from the transcript markers.");
    payload[QStringLiteral("transcript_text")] = transcriptText;
    payload[QStringLiteral("speaker_legend")] = speakerLegend;
    bool ok = false;
    QString error;
    const QJsonObject response = runAiAction(
        QStringLiteral("mine_transcript_speakers"), payload, &ok, &error);
    if (!ok) {
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
        QMessageBox::warning(
            this,
            QStringLiteral("Mine Transcript (AI)"),
            QStringLiteral("AI did not return a parsable JSON list of speaker profiles."));
        return;
    }

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

        QJsonObject profile = profiles.value(sourceSpeakerId).toObject();
        const bool tokenIsNumber = QRegularExpression(QStringLiteral("^S\\d+$"),
                                                      QRegularExpression::CaseInsensitiveOption)
                                       .match(speakerToken)
                                       .hasMatch();
        if (!speakerToken.isEmpty() && !tokenIsNumber) {
            profile[QStringLiteral("name")] = speakerToken;
        } else if (!profile.contains(QStringLiteral("name"))) {
            profile[QStringLiteral("name")] = sourceSpeakerId;
        }
        if (!organization.isEmpty()) {
            profile[QStringLiteral("organization")] = organization;
        }
        if (!summary.isEmpty()) {
            profile[QStringLiteral("brief_description")] = summary;
            profile[QStringLiteral("description")] = summary;
        }
        profiles[sourceSpeakerId] = profile;
        ++appliedRows;
    }

    if (appliedRows <= 0) {
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
        QMessageBox::warning(this,
                             QStringLiteral("Mine Transcript (AI)"),
                             QStringLiteral("Failed to save transcript after AI mining."));
        return;
    }

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
        m_preview->update();
    }
    scheduleSaveState();
    pushHistorySnapshot();
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
