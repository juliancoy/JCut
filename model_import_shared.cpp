#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif

#include "model_import_shared.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include <glm/gtc/matrix_transform.hpp>

bool hasExtension(const std::string& path, const char* ext)
{
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return std::filesystem::path(lower).extension() == ext;
}

glm::vec3 toGlmVec3(ufbx_vec3 value)
{
    return glm::vec3(static_cast<float>(value.x),
                     static_cast<float>(value.y),
                     static_cast<float>(value.z));
}

glm::vec2 toGlmVec2(ufbx_vec2 value)
{
    return glm::vec2(static_cast<float>(value.x),
                     static_cast<float>(value.y));
}

QString ufbxStringToQString(ufbx_string value)
{
    return QString::fromUtf8(value.data, static_cast<qsizetype>(value.length));
}

glm::mat3 normalMatrixFromTransform(const glm::mat4& transform)
{
    return glm::transpose(glm::inverse(glm::mat3(transform)));
}
