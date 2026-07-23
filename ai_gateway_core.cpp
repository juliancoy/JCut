#include "ai_gateway_core.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace jcut::ai {
namespace {

using json = nlohmann::json;

struct HttpResult {
    int status = 0;
    std::string body;
    GatewayErrorCore error;
};

bool ensureCurlInitialized()
{
    static std::once_flag curlInit;
    static CURLcode curlInitResult = CURLE_FAILED_INIT;
    std::call_once(curlInit, [] {
        curlInitResult = curl_global_init(CURL_GLOBAL_DEFAULT);
    });
    return curlInitResult == CURLE_OK;
}

std::string trim(std::string value)
{
    const auto visible = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), visible));
    value.erase(std::find_if(value.rbegin(), value.rend(), visible).base(),
                value.end());
    return value;
}

std::string scalarText(const json& value)
{
    if (value.is_string()) return trim(value.get<std::string>());
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (value.is_number()) return value.dump();
    if (value.is_object() || value.is_array()) return value.dump();
    return {};
}

std::string errorMessage(const json& root)
{
    for (const char* key : {"error", "message", "detail", "details"}) {
        const auto it = root.find(key);
        if (it == root.end()) continue;
        if (it->is_string()) {
            const std::string value = trim(it->get<std::string>());
            if (!value.empty()) return value;
        } else if (it->is_object()) {
            const std::string nested = errorMessage(*it);
            if (!nested.empty()) return nested;
        }
    }
    return {};
}

std::string supabaseAnonKey()
{
    if (const char* environmentKey = std::getenv("SUPABASE_ANON_KEY");
        environmentKey && *environmentKey) {
        return environmentKey;
    }
    if (const char* alternateKey = std::getenv("SB_ANON_KEY");
        alternateKey && *alternateKey) {
        return alternateKey;
    }
#ifdef JCUT_DEFAULT_SUPABASE_ANON_KEY
    return JCUT_DEFAULT_SUPABASE_ANON_KEY;
#else
    return {};
#endif
}

std::string percentEncode(std::string_view value)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char byte : value) {
        if (std::isalnum(byte) || byte == '-' || byte == '_' ||
            byte == '.' || byte == '~') {
            result.push_back(static_cast<char>(byte));
        } else {
            result.push_back('%');
            result.push_back(hex[byte >> 4U]);
            result.push_back(hex[byte & 0x0fU]);
        }
    }
    return result;
}

std::string percentDecode(std::string_view value)
{
    const auto digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    std::string result;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            const int high = digit(value[index + 1]);
            const int low = digit(value[index + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        result.push_back(value[index] == '+' ? ' ' : value[index]);
    }
    return result;
}

std::string queryValue(std::string_view target, std::string_view name)
{
    const std::size_t question = target.find('?');
    if (question == std::string_view::npos) return {};
    std::string_view query = target.substr(question + 1);
    while (!query.empty()) {
        const std::size_t separator = query.find('&');
        const std::string_view pair = query.substr(0, separator);
        const std::size_t equals = pair.find('=');
        if (percentDecode(pair.substr(0, equals)) == name) {
            return equals == std::string_view::npos
                ? std::string{} : percentDecode(pair.substr(equals + 1));
        }
        if (separator == std::string_view::npos) break;
        query.remove_prefix(separator + 1);
    }
    return {};
}

std::string base64Url(const unsigned char* bytes, std::size_t length)
{
    std::string encoded(4 * ((length + 2) / 3), '\0');
    const int encodedLength = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()),
        bytes,
        static_cast<int>(length));
    encoded.resize(static_cast<std::size_t>(std::max(0, encodedLength)));
    std::replace(encoded.begin(), encoded.end(), '+', '-');
    std::replace(encoded.begin(), encoded.end(), '/', '_');
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    return encoded;
}

std::string randomPkceVerifier()
{
    unsigned char bytes[48]{};
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) return {};
    return base64Url(bytes, sizeof(bytes));
}

std::string pkceChallenge(std::string_view verifier)
{
    unsigned char digest[SHA256_DIGEST_LENGTH]{};
    SHA256(
        reinterpret_cast<const unsigned char*>(verifier.data()),
        verifier.size(),
        digest);
    return base64Url(digest, sizeof(digest));
}

bool openBrowserUrl(const std::string& url)
{
    pid_t child = -1;
    std::vector<std::string> values{"xdg-open", url};
    std::vector<char*> arguments{
        values[0].data(), values[1].data(), nullptr};
    if (posix_spawnp(
            &child, values[0].c_str(), nullptr, nullptr,
            arguments.data(), environ) != 0) {
        return false;
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

size_t appendCurlBody(char* bytes, size_t size, size_t count, void* opaque)
{
    const size_t length = size * count;
    static_cast<std::string*>(opaque)->append(bytes, length);
    return length;
}

struct BoundedDownload {
    std::vector<std::uint8_t> bytes;
    std::size_t maximumBytes = 0;
    bool exceeded = false;
};

size_t appendBoundedDownload(
    char* bytes, size_t size, size_t count, void* opaque)
{
    const size_t length = size * count;
    auto* download = static_cast<BoundedDownload*>(opaque);
    if (!download || length > download->maximumBytes ||
        download->bytes.size() > download->maximumBytes - length) {
        if (download) download->exceeded = true;
        return 0;
    }
    const auto* begin =
        reinterpret_cast<const std::uint8_t*>(bytes);
    download->bytes.insert(
        download->bytes.end(), begin, begin + length);
    return length;
}

std::string decodeBase64Url(std::string value)
{
    std::replace(value.begin(), value.end(), '-', '+');
    std::replace(value.begin(), value.end(), '_', '/');
    while (value.size() % 4 != 0) value.push_back('=');
    if (value.empty()) return {};
    std::vector<unsigned char> decoded(
        (value.size() / 4) * 3);
    const int length = EVP_DecodeBlock(
        decoded.data(),
        reinterpret_cast<const unsigned char*>(value.data()),
        static_cast<int>(value.size()));
    if (length < 0) return {};
    std::size_t actual = static_cast<std::size_t>(length);
    if (!value.empty() && value.back() == '=') --actual;
    if (value.size() > 1 && value[value.size() - 2] == '=') --actual;
    return std::string(
        reinterpret_cast<const char*>(decoded.data()), actual);
}

bool isHttpImageUrl(const std::string& value)
{
    CURLU* parsed = curl_url();
    if (!parsed) return false;
    const CURLUcode setResult = curl_url_set(
        parsed, CURLUPART_URL, value.c_str(), 0);
    char* scheme = nullptr;
    const CURLUcode schemeResult =
        setResult == CURLUE_OK
        ? curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0)
        : setResult;
    std::string normalized = scheme ? scheme : "";
    if (scheme) curl_free(scheme);
    curl_url_cleanup(parsed);
    std::transform(
        normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    return schemeResult == CURLUE_OK &&
        (normalized == "https" || normalized == "http");
}

HttpResult requestJson(const GatewayConfigCore& config,
                       const std::string& path,
                       const std::string& method,
                       const std::string& accessToken,
                       const json* body,
                       const std::string& apiKey = {})
{
    HttpResult result;
    if (!ensureCurlInitialized()) {
        result.error.message = "libcurl global initialization failed";
        return result;
    }
    const std::string base = normalizeGatewayBaseUrl(config.baseUrl);
    if (base.empty()) {
        result.error.message = "Gateway URL is not configured";
        return result;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error.message = "Could not initialize gateway request";
        return result;
    }
    const std::string url = base + (path.starts_with('/') ? path : "/" + path);
    const std::string payload = body ? body->dump() : std::string{};
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    const std::string clientHeader = "X-CPPMonetize-Client: " + config.clientId;
    headers = curl_slist_append(headers, clientHeader.c_str());
    const std::string authHeader = "Authorization: Bearer " + trim(accessToken);
    if (!trim(accessToken).empty()) {
        headers = curl_slist_append(headers, authHeader.c_str());
    }
    const std::string apiKeyHeader = "apikey: " + trim(apiKey);
    if (!trim(apiKey).empty()) {
        headers = curl_slist_append(headers, apiKeyHeader.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(std::max(1000, config.timeoutMs)));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                     static_cast<long>(std::max(1000, config.timeoutMs)));
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "JCut/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendCurlBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(payload.size()));
    }

    const CURLcode requestResult = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    result.status = static_cast<int>(status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (requestResult != CURLE_OK) {
        result.error.httpStatus = result.status;
        result.error.message = curl_easy_strerror(requestResult);
        result.error.details = url;
        return result;
    }
    json parsed;
    try {
        parsed = json::parse(result.body);
    } catch (const json::exception&) {
        if (result.status >= 400 || result.body.empty()) {
            result.error.httpStatus = result.status;
            result.error.message =
                result.status >= 400 ? "Gateway returned an HTTP error"
                                     : "Gateway returned an empty response";
            result.error.details = result.body;
        } else {
            result.error.httpStatus = result.status;
            result.error.message = "Gateway response was not valid JSON";
            result.error.details = result.body;
        }
        return result;
    }
    if (result.status >= 400) {
        result.error.httpStatus = result.status;
        result.error.message = errorMessage(parsed);
        if (result.error.message.empty()) result.error.message = "Gateway request failed";
        result.error.details = result.body;
    }
    return result;
}

bool parseBody(const HttpResult& response, json* out, GatewayErrorCore* error)
{
    if (response.error) {
        if (error) *error = response.error;
        return false;
    }
    try {
        *out = json::parse(response.body);
        return out->is_object();
    } catch (const json::exception& exception) {
        if (error) {
            error->httpStatus = response.status;
            error->message = "Gateway response was not valid JSON";
            error->details = exception.what();
        }
        return false;
    }
}

std::vector<std::string> stringList(const json& root, const char* key)
{
    std::vector<std::string> values;
    const auto it = root.find(key);
    if (it == root.end() || !it->is_array()) return values;
    for (const json& item : *it) {
        std::string value;
        if (item.is_string()) value = trim(item.get<std::string>());
        else if (item.is_object()) value = trim(item.value("id", std::string{}));
        if (!value.empty() &&
            std::find(values.begin(), values.end(), value) == values.end()) {
            values.push_back(std::move(value));
        }
    }
    return values;
}

void appendLicenseArray(const json& root,
                        const char* key,
                        const char* type,
                        std::vector<AccessRowCore>* rows)
{
    const auto array = root.find(key);
    if (array == root.end() || !array->is_array()) return;
    for (const json& object : *array) {
        if (!object.is_object()) continue;
        AccessRowCore row;
        row.type = type;
        for (const char* itemKey :
             {"product_slug", "slug", "name", "scope_id", "product_id"}) {
            row.item = scalarText(object.value(itemKey, json{}));
            if (!row.item.empty()) break;
        }
        row.status = scalarText(object.value("status", json{}));
        if (row.status.empty()) {
            row.status = scalarText(object.value("active", json{}));
        }
        std::string starts = scalarText(object.value("starts_at", json{}));
        if (starts.empty()) {
            starts = scalarText(object.value("current_period_start", json{}));
        }
        std::string ends = scalarText(object.value("ends_at", json{}));
        if (ends.empty()) {
            ends = scalarText(object.value("current_period_end", json{}));
        }
        if (!starts.empty() || !ends.empty()) {
            row.period = (starts.empty() ? "n/a" : starts) + " -> " +
                (ends.empty() ? "n/a" : ends);
        }
        for (const char* sourceKey : {"provider", "source_type", "source_id"}) {
            row.source = scalarText(object.value(sourceKey, json{}));
            if (!row.source.empty()) break;
        }
        if (row.item.empty()) row.item = "(unknown)";
        if (row.status.empty()) row.status = "unknown";
        rows->push_back(std::move(row));
    }
}

} // namespace

std::string normalizeGatewayBaseUrl(std::string value)
{
    value = trim(std::move(value));
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

bool isSupabaseGatewayBase(const std::string& baseUrl)
{
    std::string normalized = normalizeGatewayBaseUrl(baseUrl);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::size_t scheme = normalized.find("://");
    const std::size_t hostBegin = scheme == std::string::npos ? 0 : scheme + 3;
    const std::size_t hostEnd = normalized.find_first_of(":/", hostBegin);
    const std::string host = normalized.substr(
        hostBegin, hostEnd == std::string::npos ? std::string::npos
                                                : hostEnd - hostBegin);
    return host.size() > 12 && host.ends_with(".supabase.co");
}

AccessTokenProfileCore parseAccessTokenProfileCore(
    const std::string& accessToken)
{
    AccessTokenProfileCore profile;
    const std::string token = trim(accessToken);
    const std::size_t first = token.find('.');
    if (first == std::string::npos) return profile;
    const std::size_t second = token.find('.', first + 1);
    const std::string payloadPart = token.substr(
        first + 1,
        second == std::string::npos
            ? std::string::npos
            : second - first - 1);
    const std::string decoded = decodeBase64Url(payloadPart);
    if (decoded.empty()) return profile;
    json payload;
    try {
        payload = json::parse(decoded);
    } catch (const json::exception&) {
        return profile;
    }
    if (!payload.is_object()) return profile;
    profile.email = trim(payload.value("email", std::string{}));
    profile.userId = trim(payload.value("sub", std::string{}));
    const json metadata =
        payload.contains("user_metadata") &&
            payload["user_metadata"].is_object()
        ? payload["user_metadata"]
        : json::object();
    if (profile.email.empty()) {
        profile.email = trim(
            metadata.value("email", std::string{}));
    }
    profile.avatarUrl = trim(
        payload.value("avatar_url", std::string{}));
    if (profile.avatarUrl.empty()) {
        profile.avatarUrl = trim(
            payload.value("picture", std::string{}));
    }
    if (profile.avatarUrl.empty()) {
        profile.avatarUrl = trim(
            metadata.value("avatar_url", std::string{}));
    }
    if (profile.avatarUrl.empty()) {
        profile.avatarUrl = trim(
            metadata.value("picture", std::string{}));
    }
    if (!isHttpImageUrl(profile.avatarUrl)) {
        profile.avatarUrl.clear();
    }
    return profile;
}

RemoteImageCore downloadRemoteImageCore(
    const std::string& url,
    int timeoutMs,
    std::size_t maximumBytes)
{
    RemoteImageCore result;
    result.url = trim(url);
    if (!isHttpImageUrl(result.url)) {
        result.error.message =
            "Profile image URL must use HTTP or HTTPS";
        return result;
    }
    if (maximumBytes == 0) {
        result.error.message =
            "Profile image size limit is invalid";
        return result;
    }
    if (!ensureCurlInitialized()) {
        result.error.message =
            "libcurl global initialization failed";
        return result;
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error.message =
            "Could not initialize profile image request";
        return result;
    }
    BoundedDownload download;
    download.maximumBytes = maximumBytes;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: image/*");
    curl_easy_setopt(curl, CURLOPT_URL, result.url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(
        curl,
        CURLOPT_CONNECTTIMEOUT_MS,
        static_cast<long>(std::max(1000, timeoutMs)));
    curl_easy_setopt(
        curl,
        CURLOPT_TIMEOUT_MS,
        static_cast<long>(std::max(1000, timeoutMs)));
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "JCut/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendBoundedDownload);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);
    const CURLcode requestResult = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    char* contentType = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &contentType);
    if (contentType) result.contentType = contentType;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (download.exceeded) {
        result.error.httpStatus = static_cast<int>(status);
        result.error.message =
            "Profile image exceeds the download size limit";
        return result;
    }
    if (requestResult != CURLE_OK) {
        result.error.httpStatus = static_cast<int>(status);
        result.error.message = curl_easy_strerror(requestResult);
        result.error.details = result.url;
        return result;
    }
    if (status < 200 || status >= 300) {
        result.error.httpStatus = static_cast<int>(status);
        result.error.message =
            "Profile image request returned an HTTP error";
        return result;
    }
    if (download.bytes.empty()) {
        result.error.httpStatus = static_cast<int>(status);
        result.error.message =
            "Profile image response was empty";
        return result;
    }
    result.bytes = std::move(download.bytes);
    result.ok = true;
    return result;
}

bool parseEntitlementsCore(const json& root,
                           EntitlementsCore* out,
                           std::string* errorOut)
{
    if (!out || !root.is_object()) {
        if (errorOut) *errorOut = "AI entitlements response must be an object";
        return false;
    }
    EntitlementsCore parsed;
    parsed.entitled = root.value("entitled", false);
    parsed.contractVersion = trim(root.value(
        "contract_version", root.value("version", std::string{})));
    if (root.contains("user") && root["user"].is_object()) {
        parsed.userId = trim(root["user"].value("id", std::string{}));
    }
    if (parsed.userId.empty()) {
        parsed.userId = trim(root.value("user_id", std::string{}));
    }
    parsed.models = stringList(root, "models");
    parsed.fallbackOrder = stringList(root, "fallback_order");
    if (root.contains("limits") && root["limits"].is_object()) {
        const json& limits = root["limits"];
        parsed.requestsPerMinute =
            std::max(1, limits.value("requests_per_minute", 12));
        parsed.projectBudget =
            std::max(1, limits.value("project_budget", 200));
        parsed.timeoutMs = std::max(1000, limits.value("timeout_ms", 15000));
        parsed.retries = std::clamp(limits.value("retries", 1), 0, 3);
    }
    if (parsed.contractVersion.empty()) {
        if (errorOut) *errorOut =
            "AI entitlements response is missing contract version";
        return false;
    }
    *out = std::move(parsed);
    return true;
}

bool parseUsageStatusCore(const json& root,
                          UsageStatusCore* out,
                          std::string* errorOut)
{
    if (!out || !root.is_object()) {
        if (errorOut) *errorOut = "AI usage response must be an object";
        return false;
    }
    UsageStatusCore parsed;
    parsed.userId = trim(root.value("user_id", std::string{}));
    parsed.usageMonth = trim(root.value("usage_month", std::string{}));
    parsed.freeLimit = root.value("free_limit", 0);
    parsed.freeUsed = root.value("free_used", 0);
    parsed.freeRemaining = root.value("free_remaining", 0);
    parsed.hasSubscription = root.value("has_subscription", false);
    parsed.allowAiRequests =
        root.value("allow_ai_requests", root.value("ok", false));
    parsed.requiresSubscription = root.value("requires_subscription", false);
    if (parsed.userId.empty() && parsed.usageMonth.empty()) {
        if (errorOut) *errorOut =
            "AI usage response is missing usage_month/user_id";
        return false;
    }
    *out = std::move(parsed);
    return true;
}

std::vector<AccessRowCore> parseLicenseAccessRowsCore(const json& root)
{
    std::vector<AccessRowCore> rows;
    if (!root.is_object()) return rows;
    appendLicenseArray(root, "subscriptions", "Subscription", &rows);
    appendLicenseArray(root, "purchases", "Purchase", &rows);
    appendLicenseArray(root, "products", "Product", &rows);
    appendLicenseArray(root, "entitlements", "Entitlement", &rows);
    return rows;
}

std::string extractAiResponseTextCore(const json& root)
{
    if (!root.is_object()) return {};
    for (const char* key : {"text", "message", "output"}) {
        const std::string value = scalarText(root.value(key, json{}));
        if (!value.empty()) return value;
    }
    if (root.contains("result")) {
        const std::string nested = extractAiResponseTextCore(root["result"]);
        if (!nested.empty()) return nested;
    }
    const json choices =
        root.contains("response") && root["response"].is_object()
        ? root["response"].value("choices", json::array())
        : root.value("choices", json::array());
    if (choices.is_array() && !choices.empty() && choices.front().is_object()) {
        const json& first = choices.front();
        if (first.contains("message") && first["message"].is_object()) {
            return scalarText(first["message"].value("content", json{}));
        }
        return scalarText(first.value("text", json{}));
    }
    return {};
}

AccountSnapshotCore refreshAccountCore(const GatewayConfigCore& config,
                                       const std::string& accessToken)
{
    AccountSnapshotCore snapshot;
    snapshot.supabaseDirect = isSupabaseGatewayBase(config.baseUrl);
    if (trim(accessToken).empty()) {
        snapshot.status = "Credentials required. Provide a session token or JCUT_AI_AUTH_TOKEN.";
        snapshot.error.message = snapshot.status;
        return snapshot;
    }

    json entitlementJson;
    HttpResult entitlementResponse = requestJson(
        config, "/api/ai/entitlements", "GET", accessToken, nullptr);
    if (!parseBody(entitlementResponse, &entitlementJson, &snapshot.error) &&
        snapshot.supabaseDirect) {
        snapshot.error = {};
        entitlementResponse = requestJson(
            config, "/functions/v1/ai_request", "GET", accessToken, nullptr);
        if (!parseBody(entitlementResponse, &entitlementJson, &snapshot.error)) {
            snapshot.status = "AI entitlement refresh failed: " +
                snapshot.error.message;
            return snapshot;
        }
        snapshot.entitlements.entitled =
            entitlementJson.value("allow_ai_requests", false);
        snapshot.entitlements.contractVersion = "1.supabase";
        snapshot.entitlements.userId =
            trim(entitlementJson.value("user_id", std::string{}));
        snapshot.entitlements.models = {"deepseek-chat"};
        snapshot.entitlements.fallbackOrder = snapshot.entitlements.models;
        snapshot.entitlements.timeoutMs = std::max(1000, config.timeoutMs);
    } else if (snapshot.error) {
        snapshot.status = "AI entitlement refresh failed: " +
            snapshot.error.message;
        return snapshot;
    } else {
        std::string parseError;
        if (!parseEntitlementsCore(
                entitlementJson, &snapshot.entitlements, &parseError)) {
            snapshot.error.httpStatus = entitlementResponse.status;
            snapshot.error.message = parseError;
            snapshot.status = "AI entitlement refresh failed: " + parseError;
            return snapshot;
        }
    }

    if (!snapshot.entitlements.contractVersion.starts_with("1.")) {
        snapshot.status = "AI disabled: unsupported contract '" +
            snapshot.entitlements.contractVersion + "'";
        snapshot.ok = true;
        return snapshot;
    }

    json usageJson;
    const std::string usagePath =
        snapshot.supabaseDirect ? "/functions/v1/ai_request" : "/api/ai/request";
    const HttpResult usageResponse =
        requestJson(config, usagePath, "GET", accessToken, nullptr);
    GatewayErrorCore usageError;
    if (parseBody(usageResponse, &usageJson, &usageError)) {
        std::string ignored;
        if (parseUsageStatusCore(usageJson, &snapshot.usage, &ignored)) {
            AccessRowCore usageRow;
            usageRow.type = "AI Usage";
            usageRow.item = "ai-platform";
            usageRow.status = snapshot.usage.hasSubscription
                ? "has_subscription=true" : "has_subscription=false";
            usageRow.period = snapshot.usage.usageMonth +
                " (free used " + std::to_string(snapshot.usage.freeUsed) +
                "/" + std::to_string(snapshot.usage.freeLimit) + ")";
            usageRow.source = snapshot.usage.allowAiRequests
                ? "allow_ai_requests=true" : "allow_ai_requests=false";
            snapshot.rows.push_back(std::move(usageRow));
        }
    }

    if (!snapshot.supabaseDirect) {
        json licenseJson;
        const HttpResult licenseResponse =
            requestJson(config, "/license", "GET", accessToken, nullptr);
        GatewayErrorCore ignored;
        if (parseBody(licenseResponse, &licenseJson, &ignored)) {
            std::vector<AccessRowCore> licenseRows =
                parseLicenseAccessRowsCore(licenseJson);
            snapshot.rows.insert(snapshot.rows.begin(),
                                 std::make_move_iterator(licenseRows.begin()),
                                 std::make_move_iterator(licenseRows.end()));
        }
    } else if (snapshot.rows.empty()) {
        snapshot.rows.push_back(AccessRowCore{
            "AI Entitlement",
            "ai-platform",
            snapshot.entitlements.entitled ? "entitled=true" : "entitled=false",
            "contract " + snapshot.entitlements.contractVersion,
            "supabase-direct"});
    }

    snapshot.ok = true;
    snapshot.aiEnabled = snapshot.entitlements.entitled ||
        snapshot.usage.allowAiRequests;
    snapshot.status = snapshot.aiEnabled
        ? "AI enabled for " +
            (snapshot.entitlements.userId.empty()
                ? std::string("user") : snapshot.entitlements.userId)
        : "AI disabled: user not entitled";
    return snapshot;
}

TaskResponseCore submitTaskCore(const GatewayConfigCore& config,
                                const std::string& accessToken,
                                const std::string& action,
                                const std::string& model,
                                const json& payload,
                                const json& context)
{
    TaskResponseCore result;
    json request{
        {"action", action},
        {"model", trim(model).empty() ? "deepseek-chat" : trim(model)},
        {"payload", payload},
        {"context", context},
    };
    std::string path = "/api/ai/task";
    json directRequest;
    const json* requestBody = &request;
    if (isSupabaseGatewayBase(config.baseUrl)) {
        const std::string instructions = trim(payload.value(
            "instructions",
            "You are an assistant for desktop editing workflows. Be concise."));
        const std::string transcript = payload.value(
            "transcript_text", std::string{});
        const std::string prompt = !trim(transcript).empty()
            ? "Action: " + action + "\n\nTranscript:\n" + transcript
            : "Action: " + action + "\n\nPayload:\n" + payload.dump(2);
        directRequest = json{
            {"model", request["model"]},
            {"messages", json::array({
                json{{"role", "system"}, {"content", instructions}},
                json{{"role", "user"}, {"content", prompt}},
            })},
            {"temperature", 0.2},
        };
        path = "/functions/v1/deepseek_chat";
        requestBody = &directRequest;
    }
    const HttpResult response =
        requestJson(config, path, "POST", accessToken, requestBody);
    json responseJson;
    if (!parseBody(response, &responseJson, &result.error)) return result;
    result.ok = true;
    result.responseJson = responseJson.dump();
    result.text = extractAiResponseTextCore(responseJson);
    return result;
}

RefreshedSessionCore refreshSupabaseSessionCore(
    const GatewayConfigCore& config,
    const std::string& refreshToken)
{
    RefreshedSessionCore result;
    if (trim(refreshToken).empty()) {
        result.error.message = "Refresh token is missing";
        return result;
    }
    if (!isSupabaseGatewayBase(config.baseUrl)) {
        result.error.message =
            "Token refresh requires a Supabase project gateway URL";
        return result;
    }
    const std::string anonKey = supabaseAnonKey();
    if (anonKey.empty()) {
        result.error.message =
            "Supabase anon key is not configured";
        return result;
    }
    const json request{{"refresh_token", trim(refreshToken)}};
    const HttpResult response = requestJson(
        config,
        "/auth/v1/token?grant_type=refresh_token",
        "POST",
        {},
        &request,
        anonKey);
    json root;
    if (!parseBody(response, &root, &result.error)) return result;
    result.accessToken = trim(root.value("access_token", std::string{}));
    result.refreshToken = trim(root.value("refresh_token", std::string{}));
    if (root.contains("session") && root["session"].is_object()) {
        if (result.accessToken.empty()) {
            result.accessToken = trim(
                root["session"].value("access_token", std::string{}));
        }
        if (result.refreshToken.empty()) {
            result.refreshToken = trim(
                root["session"].value("refresh_token", std::string{}));
        }
    }
    if (root.contains("user") && root["user"].is_object()) {
        result.userId = trim(
            root["user"].value("email", std::string{}));
        if (result.userId.empty()) {
            result.userId = trim(
                root["user"].value("id", std::string{}));
        }
    }
    if (result.accessToken.empty()) {
        result.error.httpStatus = response.status;
        result.error.message =
            "Supabase refresh response is missing an access token";
        return result;
    }
    if (result.refreshToken.empty()) result.refreshToken = trim(refreshToken);
    result.ok = true;
    return result;
}

std::string buildSupabasePkceAuthorizeUrlCore(
    const std::string& supabaseBaseUrl,
    const std::string& provider,
    const std::string& redirectUrl,
    const std::string& codeVerifier)
{
    const std::string base = normalizeGatewayBaseUrl(supabaseBaseUrl);
    if (base.empty() || redirectUrl.empty() || codeVerifier.empty()) return {};
    return base + "/auth/v1/authorize?provider=" +
        percentEncode(provider.empty() ? "google" : provider) +
        "&redirect_to=" + percentEncode(redirectUrl) +
        "&code_challenge=" + percentEncode(pkceChallenge(codeVerifier)) +
        "&code_challenge_method=S256&flow_type=pkce&response_type=code";
}

BrowserLoginCore runSupabaseBrowserLoginCore(
    const GatewayConfigCore& config,
    const std::string& provider,
    int callbackTimeoutMs,
    const std::atomic_bool* cancelRequested)
{
    BrowserLoginCore result;
    if (!isSupabaseGatewayBase(config.baseUrl)) {
        result.error.message =
            "Browser login requires a Supabase project gateway URL";
        return result;
    }
    const std::string anonKey = supabaseAnonKey();
    if (anonKey.empty()) {
        result.error.message = "Supabase anon key is not configured";
        return result;
    }
    const int server = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server < 0) {
        result.error.message =
            "Unable to create localhost OAuth callback socket";
        return result;
    }
    int reuse = 1;
    (void)::setsockopt(
        server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(
            server,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) != 0 ||
        ::listen(server, 1) != 0) {
        ::close(server);
        result.error.message =
            "Unable to bind localhost OAuth callback socket";
        return result;
    }
    socklen_t addressLength = sizeof(address);
    if (::getsockname(
            server,
            reinterpret_cast<sockaddr*>(&address),
            &addressLength) != 0) {
        ::close(server);
        result.error.message =
            "Unable to resolve localhost OAuth callback port";
        return result;
    }
    const std::string verifier = randomPkceVerifier();
    if (verifier.empty()) {
        ::close(server);
        result.error.message =
            "Unable to generate PKCE verifier";
        return result;
    }
    const std::string redirect =
        "http://127.0.0.1:" +
        std::to_string(ntohs(address.sin_port)) + "/callback";
    result.authorizationUrl = buildSupabasePkceAuthorizeUrlCore(
        config.baseUrl, provider, redirect, verifier);
    if (!openBrowserUrl(result.authorizationUrl)) {
        ::close(server);
        result.error.message = "Unable to open the browser login URL";
        result.error.details = result.authorizationUrl;
        return result;
    }

    pollfd descriptor{server, POLLIN, 0};
    int pollResult = 0;
    const auto callbackDeadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(std::max(1000, callbackTimeoutMs));
    while (std::chrono::steady_clock::now() < callbackDeadline) {
        if (cancelRequested && cancelRequested->load()) {
            ::close(server);
            result.error.message = "OAuth login canceled";
            return result;
        }
        pollResult = ::poll(&descriptor, 1, 200);
        if (pollResult != 0) break;
    }
    if (pollResult <= 0) {
        ::close(server);
        result.error.message = pollResult == 0
            ? "OAuth callback timed out"
            : "OAuth callback wait failed";
        return result;
    }
    const int connection =
        ::accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
    ::close(server);
    if (connection < 0) {
        result.error.message =
            "Unable to accept OAuth callback";
        return result;
    }
    timeval receiveTimeout{
        std::max(1, config.timeoutMs / 1000), 0};
    (void)::setsockopt(
        connection, SOL_SOCKET, SO_RCVTIMEO,
        &receiveTimeout, sizeof(receiveTimeout));
    std::string request;
    char buffer[4096];
    while (request.size() < 64U * 1024U &&
           request.find("\r\n\r\n") == std::string::npos) {
        const ssize_t count =
            ::read(connection, buffer, sizeof(buffer));
        if (count > 0) {
            request.append(buffer, static_cast<std::size_t>(count));
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    const std::size_t firstSpace = request.find(' ');
    const std::size_t secondSpace = firstSpace == std::string::npos
        ? std::string::npos : request.find(' ', firstSpace + 1);
    const std::string target =
        firstSpace == std::string::npos ||
        secondSpace == std::string::npos
        ? std::string{}
        : request.substr(
            firstSpace + 1, secondSpace - firstSpace - 1);
    const std::string callbackError = queryValue(
        target, "error_description").empty()
        ? queryValue(target, "error")
        : queryValue(target, "error_description");
    const std::string code = queryValue(target, "code");
    const bool callbackOk = callbackError.empty() && !code.empty();
    const std::string page = callbackOk
        ? "<!doctype html><title>JCut sign-in complete</title>"
          "<h1>Sign-in complete</h1><p>Return to JCut. "
          "This tab can be closed.</p>"
        : "<!doctype html><title>JCut sign-in failed</title>"
          "<h1>Sign-in failed</h1><p>Return to JCut and try again.</p>";
    const std::string response =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\nContent-Length: " +
        std::to_string(page.size()) + "\r\n\r\n" + page;
    std::size_t responseOffset = 0;
    while (responseOffset < response.size()) {
        const ssize_t count = ::write(
            connection,
            response.data() + responseOffset,
            response.size() - responseOffset);
        if (count > 0) {
            responseOffset += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    ::close(connection);
    if (!callbackOk) {
        result.error.message = callbackError.empty()
            ? "OAuth callback did not contain an authorization code"
            : callbackError;
        return result;
    }

    const json exchangeRequest{
        {"auth_code", code},
        {"code_verifier", verifier},
    };
    const HttpResult exchange = requestJson(
        config,
        "/auth/v1/token?grant_type=pkce",
        "POST",
        {},
        &exchangeRequest,
        anonKey);
    json root;
    if (!parseBody(exchange, &root, &result.error)) return result;
    result.accessToken = trim(root.value("access_token", std::string{}));
    result.refreshToken = trim(root.value("refresh_token", std::string{}));
    if (root.contains("session") && root["session"].is_object()) {
        if (result.accessToken.empty()) {
            result.accessToken = trim(
                root["session"].value("access_token", std::string{}));
        }
        if (result.refreshToken.empty()) {
            result.refreshToken = trim(
                root["session"].value("refresh_token", std::string{}));
        }
    }
    if (root.contains("user") && root["user"].is_object()) {
        result.userId = trim(root["user"].value("email", std::string{}));
        if (result.userId.empty()) {
            result.userId = trim(root["user"].value("id", std::string{}));
        }
    }
    if (result.accessToken.empty()) {
        result.error.message =
            "OAuth token exchange returned no access token";
        return result;
    }
    result.ok = true;
    return result;
}

CheckoutLaunchCore launchSubscriptionCheckoutCore(
    const GatewayConfigCore& config,
    const std::string& accessToken,
    const std::vector<std::string>& productSlugs)
{
    CheckoutLaunchCore result;
    if (trim(accessToken).empty()) {
        result.error.message = "Login is required before checkout";
        return result;
    }
    for (const std::string& slugValue : productSlugs) {
        const std::string slug = trim(slugValue);
        if (slug.empty()) continue;
        const json request = json::object();
        const HttpResult response = requestJson(
            config,
            "/api/packs/" + percentEncode(slug) +
                "/purchase/stripe",
            "POST",
            accessToken,
            &request);
        json root;
        GatewayErrorCore error;
        if (!parseBody(response, &root, &error)) {
            result.error = std::move(error);
            continue;
        }
        const std::string checkoutUrl =
            trim(root.value("checkout_url", std::string{}));
        if (checkoutUrl.empty()) {
            result.error.message =
                "Checkout response is missing checkout_url";
            continue;
        }
        if (!openBrowserUrl(checkoutUrl)) {
            result.error.message =
                "Unable to open checkout URL in the browser";
            result.error.details = checkoutUrl;
            return result;
        }
        result.ok = true;
        result.productSlug = slug;
        result.checkoutUrl = checkoutUrl;
        return result;
    }
    if (result.error.message.empty()) {
        result.error.message =
            "No subscription product slug could start checkout";
    }
    return result;
}

} // namespace jcut::ai
