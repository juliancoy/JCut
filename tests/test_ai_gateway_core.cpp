#include "../ai_gateway_core.h"
#include "../ai_credential_store_core.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::string base64Url(std::string value)
{
    std::string encoded(
        4 * ((value.size() + 2) / 3), '\0');
    const int length = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()),
        reinterpret_cast<const unsigned char*>(value.data()),
        static_cast<int>(value.size()));
    encoded.resize(static_cast<std::size_t>(length));
    std::replace(encoded.begin(), encoded.end(), '+', '-');
    std::replace(encoded.begin(), encoded.end(), '/', '_');
    while (!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }
    return encoded;
}

} // namespace

int main()
{
    using nlohmann::json;

    expect(
        jcut::ai::normalizeGatewayBaseUrl(" https://gateway.test/// ") ==
            "https://gateway.test",
        "gateway normalization trims whitespace and trailing slashes");
    expect(
        jcut::ai::isSupabaseGatewayBase(
            "https://project.supabase.co/"),
        "Supabase gateway detection accepts project origins");
    expect(
        !jcut::ai::isSupabaseGatewayBase(
            "https://supabase.co.attacker.test"),
        "Supabase gateway detection uses the hostname suffix");
    const std::string profileToken =
        "header." +
        base64Url(json{
            {"sub", "user-42"},
            {"user_metadata",
             {
                 {"email", "person@example.test"},
                 {"picture", "https://images.example.test/avatar.png"},
             }},
        }.dump()) +
        ".signature";
    const jcut::ai::AccessTokenProfileCore profile =
        jcut::ai::parseAccessTokenProfileCore(profileToken);
    expect(
        profile.email == "person@example.test" &&
            profile.userId == "user-42",
        "shared access-token profile parser resolves metadata identity");
    expect(
        profile.displayIdentity() == "person@example.test",
        "shared token profile prefers email display identity");
    expect(
        profile.avatarUrl ==
            "https://images.example.test/avatar.png",
        "shared token profile resolves metadata picture URL");
    const std::string unsafeAvatarToken =
        "header." +
        base64Url(json{
            {"sub", "user-unsafe"},
            {"picture", "file:///tmp/avatar.png"},
        }.dump()) +
        ".signature";
    expect(
        jcut::ai::parseAccessTokenProfileCore(
            unsafeAvatarToken)
            .avatarUrl.empty(),
        "profile avatar parser rejects non-network URL schemes");
    expect(
        !jcut::ai::downloadRemoteImageCore(
             "file:///tmp/avatar.png")
             .ok,
        "remote image downloader rejects non-HTTP URLs without I/O");

    jcut::ai::EntitlementsCore entitlements;
    std::string error;
    expect(
        jcut::ai::parseEntitlementsCore(
            json{
                {"entitled", true},
                {"contract_version", "1.2"},
                {"user", {{"id", "user-7"}}},
                {"models", json::array({
                    "deepseek-chat",
                    json{{"id", "fallback-model"}}})},
                {"fallback_order", json::array({"fallback-model"})},
                {"limits",
                 {
                     {"requests_per_minute", 8},
                     {"project_budget", 91},
                     {"timeout_ms", 4200},
                     {"retries", 2},
                 }},
            },
            &entitlements,
            &error),
        "entitlements parse");
    expect(entitlements.entitled, "entitlement enabled");
    expect(entitlements.userId == "user-7", "nested user id parse");
    expect(entitlements.models.size() == 2, "string and object model parse");
    expect(entitlements.projectBudget == 91, "project budget parse");

    jcut::ai::UsageStatusCore usage;
    expect(
        jcut::ai::parseUsageStatusCore(
            json{
                {"user_id", "user-7"},
                {"usage_month", "2026-07"},
                {"free_limit", 10},
                {"free_used", 3},
                {"free_remaining", 7},
                {"has_subscription", true},
                {"allow_ai_requests", true},
            },
            &usage,
            &error),
        "usage status parse");
    expect(usage.allowAiRequests, "usage request capability parse");
    expect(usage.freeRemaining == 7, "usage counters parse");

    const auto rows = jcut::ai::parseLicenseAccessRowsCore(json{
        {"subscriptions",
         json::array({json{
             {"product_slug", "jcut-pro"},
             {"status", "active"},
             {"current_period_start", "2026-07-01"},
             {"current_period_end", "2026-08-01"},
             {"provider", "stripe"},
         }})},
        {"entitlements",
         json::array({json{
             {"scope_id", "ai-platform"},
             {"active", true},
             {"source_type", "subscription"},
         }})},
    });
    expect(rows.size() == 2, "license arrays become access rows");
    expect(rows[0].item == "jcut-pro", "subscription slug parse");
    expect(rows[0].period == "2026-07-01 -> 2026-08-01",
           "subscription period parse");
    expect(rows[1].status == "true", "boolean status parse");

    expect(
        jcut::ai::extractAiResponseTextCore(
            json{{"result", {{"message", "nested response"}}}}) ==
            "nested response",
        "nested result response text");
    expect(
        jcut::ai::extractAiResponseTextCore(json{
            {"response",
             {{"choices",
               json::array({json{
                   {"message", {{"content", "provider response"}}},
               }})}}},
        }) == "provider response",
        "provider choices response text");
    expect(
        !jcut::ai::refreshSupabaseSessionCore(
             jcut::ai::GatewayConfigCore{
                 "https://project.supabase.co", 1000, "test"},
             {})
             .ok,
        "refresh rejects a missing refresh token without network access");
    const std::string authorizeUrl =
        jcut::ai::buildSupabasePkceAuthorizeUrlCore(
            "https://project.supabase.co/",
            "google",
            "http://127.0.0.1:4567/callback",
            "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");
    expect(
        authorizeUrl.find(
            "code_challenge=E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM") !=
            std::string::npos,
        "PKCE authorization URL uses the RFC 7636 S256 challenge");
    expect(
        authorizeUrl.find(
            "redirect_to=http%3A%2F%2F127.0.0.1%3A4567%2Fcallback") !=
            std::string::npos,
        "PKCE authorization URL percent-encodes the callback");
    expect(
        !jcut::ai::launchSubscriptionCheckoutCore(
             jcut::ai::GatewayConfigCore{
                 "https://gateway.test", 1000, "test"},
             {},
             {"ai-platform"})
             .ok,
        "checkout rejects missing login without network access");

    const std::filesystem::path credentialDirectory =
        std::filesystem::temp_directory_path() /
        ("jcut-ai-credentials-" + std::to_string(
            std::chrono::steady_clock::now()
                .time_since_epoch().count()));
    jcut::ai::CredentialStoreConfigCore credentialConfig;
    credentialConfig.configDirectoryOverride =
        credentialDirectory.string();
    credentialConfig.preferSystemStore = false;
    const jcut::ai::StoredCredentialsCore expectedCredentials{
        "access-secret", "refresh-secret", "user-9"};
    const auto stored = jcut::ai::storeCredentialsCore(
        expectedCredentials, credentialConfig);
    expect(stored.ok && !stored.usedSystemStore,
           "private credential fallback stores a session");
    const auto loaded = jcut::ai::loadStoredCredentialsCore(
        credentialConfig);
    expect(loaded.ok, "private credential fallback loads a session");
    expect(loaded.credentials.accessToken == "access-secret",
           "stored access token roundtrip");
    expect(loaded.credentials.refreshToken == "refresh-secret",
           "stored refresh token roundtrip");
    expect(loaded.credentials.userId == "user-9",
           "stored user id roundtrip");
    std::error_code permissionError;
    const auto permissions = std::filesystem::status(
        credentialDirectory / "auth_token.txt", permissionError).permissions();
    expect(
        !permissionError &&
            (permissions & std::filesystem::perms::group_all) ==
                std::filesystem::perms::none &&
            (permissions & std::filesystem::perms::others_all) ==
                std::filesystem::perms::none,
        "credential fallback file excludes group/other permissions");
    expect(
        jcut::ai::clearStoredCredentialsCore(credentialConfig).ok,
        "private credential fallback clears");
    expect(
        !std::filesystem::exists(
            credentialDirectory / "auth_token.txt"),
        "access token file removed");
    std::filesystem::remove_all(credentialDirectory, permissionError);

    if (failures == 0) {
        std::cout << "AI gateway core assertions passed\n";
    }
    return failures == 0 ? 0 : 1;
}
