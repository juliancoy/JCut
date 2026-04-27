#include "editor.h"
#include "editor_ai_helpers.h"

#include <cppmonetize/AuthIdentity.h>
#include <cppmonetize/MonetizeClient.h>
#include <cppmonetize/OAuthDesktopFlow.h>
#include <cppmonetize/TokenStore.h>

#include <QDateTime>
#include <QMessageBox>
#include <QSignalBlocker>

namespace {

constexpr auto kDefaultSupabaseGateway = "https://ivwutugdrpugjqglxabw.supabase.co";

cppmonetize::MonetizeClient createJCutMonetizeClient(const QString& apiBaseUrl,
                                                      int timeoutMs,
                                                      const QString& contractPrefix = QStringLiteral("1."))
{
    cppmonetize::ClientConfig cfg;
    cfg.apiBaseUrl = apiBaseUrl;
    cfg.timeoutMs = timeoutMs;
    cfg.clientId = QStringLiteral("jcut-desktop");
    cfg.requiredContractPrefix = contractPrefix;
    cfg.telemetryHook = [](const cppmonetize::RequestTelemetryEvent& event) {
        if (event.success) {
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
            status = QStringLiteral("AI disabled: entitlement check failed (%1)")
                         .arg(entResult.error().message);
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
