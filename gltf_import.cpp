#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif

#include "gltf_import.h"
#include "model_import_shared.h"

#include <iostream>
#include <numeric>
#include <stdexcept>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

glm::mat4 tinygltfNodeLocalTransform(const tinygltf::Node& node)
{
    if (node.matrix.size() == 16)
    {
        glm::mat4 matrix(1.0f);
        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
            {
                matrix[column][row] = static_cast<float>(node.matrix[column * 4 + row]);
            }
        }
        return matrix;
    }

    glm::mat4 matrix(1.0f);
    if (node.translation.size() == 3)
    {
        matrix = glm::translate(matrix, glm::vec3(static_cast<float>(node.translation[0]),
                                                  static_cast<float>(node.translation[1]),
                                                  static_cast<float>(node.translation[2])));
    }
    if (node.rotation.size() == 4)
    {
        const glm::quat rotation(static_cast<float>(node.rotation[3]),
                                 static_cast<float>(node.rotation[0]),
                                 static_cast<float>(node.rotation[1]),
                                 static_cast<float>(node.rotation[2]));
        matrix *= glm::mat4_cast(rotation);
    }
    if (node.scale.size() == 3)
    {
        matrix = glm::scale(matrix, glm::vec3(static_cast<float>(node.scale[0]),
                                              static_cast<float>(node.scale[1]),
                                              static_cast<float>(node.scale[2])));
    }
    return matrix;
}

bool extractVerticesAndIndicesFromGltfPrimitive(const tinygltf::Model& model,
                                                const tinygltf::Primitive& primitive,
                                                const glm::mat4& transform,
                                                std::vector<Vertex>& outVertices,
                                                std::vector<uint32_t>& outIndices)
{
    if (primitive.attributes.count("POSITION") == 0 ||
        primitive.attributes.count("NORMAL") == 0 ||
        primitive.attributes.count("TEXCOORD_0") == 0)
    {
        std::cerr << "Skipping primitive due to missing attributes.\n";
        return false;
    }

    const auto& posAccessor = model.accessors[primitive.attributes.at("POSITION")];
    const auto& normAccessor = model.accessors[primitive.attributes.at("NORMAL")];
    const auto& texAccessor = model.accessors[primitive.attributes.at("TEXCOORD_0")];

    const auto& posView = model.bufferViews[posAccessor.bufferView];
    const auto& normView = model.bufferViews[normAccessor.bufferView];
    const auto& texView = model.bufferViews[texAccessor.bufferView];

    const float* positions = reinterpret_cast<const float*>(&model.buffers[posView.buffer].data[posView.byteOffset + posAccessor.byteOffset]);
    const float* normals = reinterpret_cast<const float*>(&model.buffers[normView.buffer].data[normView.byteOffset + normAccessor.byteOffset]);
    const float* texCoords = reinterpret_cast<const float*>(&model.buffers[texView.buffer].data[texView.byteOffset + texAccessor.byteOffset]);

    if (posAccessor.count != normAccessor.count || posAccessor.count != texAccessor.count)
    {
        throw std::runtime_error("GLTF attribute counts mismatch.");
    }

    outVertices.resize(posAccessor.count);
    const glm::mat3 normalMatrix = normalMatrixFromTransform(transform);
    for (size_t i = 0; i < posAccessor.count; ++i)
    {
        const glm::vec3 position(positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]);
        const glm::vec3 normal(normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2]);
        outVertices[i].pos = glm::vec3(transform * glm::vec4(position, 1.0f));
        outVertices[i].normal = glm::normalize(normalMatrix * normal);
        outVertices[i].texCoord = glm::vec2(texCoords[i * 2], texCoords[i * 2 + 1]);
    }

    if (primitive.indices >= 0)
    {
        const auto& indexAccessor = model.accessors[primitive.indices];
        const auto& indexView = model.bufferViews[indexAccessor.bufferView];
        const auto& indexBufferData = model.buffers[indexView.buffer];

        const uint8_t* dataPtr = reinterpret_cast<const uint8_t*>(
            &indexBufferData.data[indexView.byteOffset + indexAccessor.byteOffset]);

        auto componentSize = [](int componentType) -> size_t {
            switch (componentType)
            {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                return sizeof(uint8_t);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                return sizeof(uint16_t);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                return sizeof(uint32_t);
            default:
                throw std::runtime_error("Unsupported index component type in GLTF");
            }
        };

        size_t stride = indexView.byteStride ? indexView.byteStride : componentSize(indexAccessor.componentType);
        outIndices.resize(indexAccessor.count);

        for (size_t i = 0; i < indexAccessor.count; ++i)
        {
            const uint8_t* elementPtr = dataPtr + i * stride;
            switch (indexAccessor.componentType)
            {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                outIndices[i] = *reinterpret_cast<const uint8_t*>(elementPtr);
                break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                outIndices[i] = *reinterpret_cast<const uint16_t*>(elementPtr);
                break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                outIndices[i] = *reinterpret_cast<const uint32_t*>(elementPtr);
                break;
            default:
                throw std::runtime_error("Unsupported index component type in GLTF");
            }
        }
    }
    else
    {
        outIndices.resize(outVertices.size());
        std::iota(outIndices.begin(), outIndices.end(), 0u);
    }

    return true;
}

void appendNodePrimitivesToGroups(const tinygltf::Model& model,
                                  const tinygltf::Node& node,
                                  const glm::mat4& worldTransform,
                                  std::unordered_map<int, size_t>& materialToGroupIndex,
                                  std::vector<GltfCombinedPrimitiveData>& groups)
{
    if (node.mesh < 0 || node.mesh >= static_cast<int>(model.meshes.size()))
    {
        return;
    }

    const auto& gltfmesh = model.meshes[static_cast<size_t>(node.mesh)];
    for (const auto& tprimitive : gltfmesh.primitives)
    {
        std::vector<Vertex> primitiveVertices;
        std::vector<uint32_t> primitiveIndices;
        if (!extractVerticesAndIndicesFromGltfPrimitive(model, tprimitive, worldTransform, primitiveVertices, primitiveIndices))
        {
            continue;
        }

        const int materialIndex = tprimitive.material;
        auto it = materialToGroupIndex.find(materialIndex);
        if (it == materialToGroupIndex.end())
        {
            const size_t newIndex = groups.size();
            materialToGroupIndex.emplace(materialIndex, newIndex);
            groups.push_back(GltfCombinedPrimitiveData{materialIndex, {}, {}});
            it = materialToGroupIndex.find(materialIndex);
        }

        GltfCombinedPrimitiveData& group = groups[it->second];
        const uint32_t vertexOffset = static_cast<uint32_t>(group.vertices.size());
        group.vertices.insert(group.vertices.end(), primitiveVertices.begin(), primitiveVertices.end());
        group.indices.reserve(group.indices.size() + primitiveIndices.size());
        for (uint32_t index : primitiveIndices)
        {
            group.indices.push_back(vertexOffset + index);
        }
    }
}

void collectGltfNodeGroupsRecursive(const tinygltf::Model& model,
                                    int nodeIndex,
                                    const glm::mat4& parentTransform,
                                    std::vector<std::vector<GltfCombinedPrimitiveData>>& outNodeGroups,
                                    std::unordered_map<int, size_t>* globalMaterialToGroupIndex,
                                    std::vector<GltfCombinedPrimitiveData>* globalGroups)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size()))
    {
        return;
    }

    const auto& node = model.nodes[static_cast<size_t>(nodeIndex)];
    const glm::mat4 worldTransform = parentTransform * tinygltfNodeLocalTransform(node);

    if (node.mesh >= 0)
    {
        if (globalMaterialToGroupIndex && globalGroups)
        {
            appendNodePrimitivesToGroups(model, node, worldTransform, *globalMaterialToGroupIndex, *globalGroups);
        }
        else
        {
            std::unordered_map<int, size_t> materialToGroupIndex;
            std::vector<GltfCombinedPrimitiveData> nodeGroups;
            appendNodePrimitivesToGroups(model, node, worldTransform, materialToGroupIndex, nodeGroups);
            if (!nodeGroups.empty())
            {
                outNodeGroups.push_back(std::move(nodeGroups));
            }
        }
    }

    for (int childIndex : node.children)
    {
        collectGltfNodeGroupsRecursive(model, childIndex, worldTransform, outNodeGroups, globalMaterialToGroupIndex, globalGroups);
    }
}
