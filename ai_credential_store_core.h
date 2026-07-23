#pragma once

#include <string>

namespace jcut::ai {

struct StoredCredentialsCore {
    std::string accessToken;
    std::string refreshToken;
    std::string userId;
};

struct CredentialStoreConfigCore {
    std::string serviceName = "jcut.ai.auth";
    std::string applicationName = "PanelTalkEditor";
    std::string configDirectoryOverride;
    bool preferSystemStore = true;
};

struct CredentialStoreResultCore {
    bool ok = false;
    bool usedSystemStore = false;
    StoredCredentialsCore credentials;
    std::string error;
};

CredentialStoreResultCore loadStoredCredentialsCore(
    const CredentialStoreConfigCore& config = {});
CredentialStoreResultCore storeCredentialsCore(
    const StoredCredentialsCore& credentials,
    const CredentialStoreConfigCore& config = {});
CredentialStoreResultCore clearStoredCredentialsCore(
    const CredentialStoreConfigCore& config = {});

} // namespace jcut::ai
