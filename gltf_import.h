#pragma once

#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <../tinygltf/tiny_gltf.h>

#include "model.h"

glm::mat4 tinygltfNodeLocalTransform(const tinygltf::Node& node);

bool extractVerticesAndIndicesFromGltfPrimitive(const tinygltf::Model& model,
                                                const tinygltf::Primitive& primitive,
                                                const glm::mat4& transform,
                                                std::vector<Vertex>& outVertices,
                                                std::vector<uint32_t>& outIndices);

void appendNodePrimitivesToGroups(const tinygltf::Model& model,
                                  const tinygltf::Node& node,
                                  const glm::mat4& worldTransform,
                                  std::unordered_map<int, size_t>& materialToGroupIndex,
                                  std::vector<GltfCombinedPrimitiveData>& groups);

void collectGltfNodeGroupsRecursive(const tinygltf::Model& model,
                                    int nodeIndex,
                                    const glm::mat4& parentTransform,
                                    std::vector<std::vector<GltfCombinedPrimitiveData>>& outNodeGroups,
                                    std::unordered_map<int, size_t>* globalMaterialToGroupIndex = nullptr,
                                    std::vector<GltfCombinedPrimitiveData>* globalGroups = nullptr);
