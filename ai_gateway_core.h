#pragma once

#include <nlohmann/json_fwd.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jcut::ai {

struct GatewayErrorCore {
    int httpStatus = 0;
    std::string message;
    std::string details;

    explicit operator bool() const noexcept { return !message.empty(); }
};

struct EntitlementsCore {
    bool entitled = false;
    std::string contractVersion;
    std::string userId;
    std::vector<std::string> models;
    std::vector<std::string> fallbackOrder;
    int requestsPerMinute = 12;
    int projectBudget = 200;
    int timeoutMs = 15000;
    int retries = 1;
};

struct UsageStatusCore {
    std::string userId;
    std::string usageMonth;
    int freeLimit = 0;
    int freeUsed = 0;
    int freeRemaining = 0;
    bool hasSubscription = false;
    bool allowAiRequests = false;
    bool requiresSubscription = false;
};

struct AccessRowCore {
    std::string type;
    std::string item;
    std::string status;
    std::string period;
    std::string source;
};

struct AccountSnapshotCore {
    bool ok = false;
    bool aiEnabled = false;
    bool supabaseDirect = false;
    std::string status;
    EntitlementsCore entitlements;
    UsageStatusCore usage;
    std::vector<AccessRowCore> rows;
    GatewayErrorCore error;
};

struct TaskResponseCore {
    bool ok = false;
    std::string responseJson;
    std::string text;
    GatewayErrorCore error;
};

struct GatewayConfigCore {
    std::string baseUrl;
    int timeoutMs = 15000;
    std::string clientId = "jcut-desktop";
};

struct RefreshedSessionCore {
    bool ok = false;
    std::string accessToken;
    std::string refreshToken;
    std::string userId;
    GatewayErrorCore error;
};

struct BrowserLoginCore {
    bool ok = false;
    std::string authorizationUrl;
    std::string accessToken;
    std::string refreshToken;
    std::string userId;
    GatewayErrorCore error;
};

struct CheckoutLaunchCore {
    bool ok = false;
    std::string productSlug;
    std::string checkoutUrl;
    GatewayErrorCore error;
};

struct AccessTokenProfileCore {
    std::string email;
    std::string userId;
    std::string avatarUrl;

    std::string displayIdentity() const
    {
        return !email.empty() ? email : userId;
    }
};

struct RemoteImageCore {
    bool ok = false;
    std::string url;
    std::string contentType;
    std::vector<std::uint8_t> bytes;
    GatewayErrorCore error;
};

std::string normalizeGatewayBaseUrl(std::string value);
bool isSupabaseGatewayBase(const std::string& baseUrl);
AccessTokenProfileCore parseAccessTokenProfileCore(
    const std::string& accessToken);
RemoteImageCore downloadRemoteImageCore(
    const std::string& url,
    int timeoutMs = 10000,
    std::size_t maximumBytes = 4u * 1024u * 1024u);

bool parseEntitlementsCore(const nlohmann::json& root,
                           EntitlementsCore* out,
                           std::string* errorOut = nullptr);
bool parseUsageStatusCore(const nlohmann::json& root,
                          UsageStatusCore* out,
                          std::string* errorOut = nullptr);
std::vector<AccessRowCore> parseLicenseAccessRowsCore(
    const nlohmann::json& root);
std::string extractAiResponseTextCore(const nlohmann::json& root);

AccountSnapshotCore refreshAccountCore(const GatewayConfigCore& config,
                                       const std::string& accessToken);
TaskResponseCore submitTaskCore(const GatewayConfigCore& config,
                                const std::string& accessToken,
                                const std::string& action,
                                const std::string& model,
                                const nlohmann::json& payload,
                                const nlohmann::json& context);
RefreshedSessionCore refreshSupabaseSessionCore(
    const GatewayConfigCore& config,
    const std::string& refreshToken);
std::string buildSupabasePkceAuthorizeUrlCore(
    const std::string& supabaseBaseUrl,
    const std::string& provider,
    const std::string& redirectUrl,
    const std::string& codeVerifier);
BrowserLoginCore runSupabaseBrowserLoginCore(
    const GatewayConfigCore& config,
    const std::string& provider = "google",
    int callbackTimeoutMs = 180000,
    const std::atomic_bool* cancelRequested = nullptr);
CheckoutLaunchCore launchSubscriptionCheckoutCore(
    const GatewayConfigCore& config,
    const std::string& accessToken,
    const std::vector<std::string>& productSlugs);

} // namespace jcut::ai
