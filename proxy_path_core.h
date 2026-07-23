#pragma once

#include <string>
#include <vector>

namespace jcut {

std::vector<std::string> proxyCandidatePaths(const std::string& sourcePath);
std::string discoverExistingProxyPath(const std::string& sourcePath);
bool proxyPathIsUsable(const std::string& path);

} // namespace jcut
