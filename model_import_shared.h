#pragma once

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif

#include <string>

#include <QString>
#include <glm/glm.hpp>

#include "ufbx.h"

bool hasExtension(const std::string& path, const char* ext);
glm::vec3 toGlmVec3(ufbx_vec3 value);
glm::vec2 toGlmVec2(ufbx_vec2 value);
QString ufbxStringToQString(ufbx_string value);
glm::mat3 normalMatrixFromTransform(const glm::mat4& transform);
