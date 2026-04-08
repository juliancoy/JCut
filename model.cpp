#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif

#include "model.h"

#include "animation.h"
#include "engine.h"
#include "fbx_import.h"
#include "gltf_import.h"
#include "model_import_shared.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/component_wise.hpp>

#include "stb_image.h"
#include "ufbx.h"

namespace
{

void bindFbxMeshAnimation(Model& model,
                          const ufbx_mesh* mesh,
                          uint32_t skinDeformerIndex,
                          std::vector<uint32_t> sourceVertexIndices,
                          std::vector<glm::uvec4> jointIndices,
                          std::vector<glm::vec4> jointWeights,
                          bool gpuSkinningEligible)
{
    if (!(model.animated && model.fbxAnimationRuntime))
    {
        return;
    }

    if (gpuSkinningEligible)
    {
        motive::animation::addFbxMeshBinding(*model.fbxAnimationRuntime,
                                             mesh->element_id,
                                             skinDeformerIndex,
                                             std::move(sourceVertexIndices),
                                             std::move(jointIndices),
                                             std::move(jointWeights));
    }
    else
    {
        motive::animation::addFbxMeshBinding(*model.fbxAnimationRuntime,
                                             mesh->element_id,
                                             std::move(sourceVertexIndices));
    }
}

void importFbxVerticesIntoModel(Model& model,
                                Engine* engine,
                                const ufbx_mesh* mesh,
                                std::vector<Vertex> vertices,
                                std::vector<uint32_t> sourceVertexIndices,
                                int materialIndex,
                                const char* summaryLabel)
{
    std::vector<glm::uvec4> jointIndices;
    std::vector<glm::vec4> jointWeights;
    uint32_t skinDeformerIndex = 0;
    bool gpuSkinningEligible = false;

    if (model.animated &&
        model.fbxAnimationRuntime &&
        mesh->skin_deformers.count > 0 &&
        sourceVertexIndices.size() == vertices.size())
    {
        jointIndices.resize(vertices.size());
        jointWeights.resize(vertices.size());
        for (size_t vertexIndex = 0; vertexIndex < vertices.size(); ++vertexIndex)
        {
            getFbxSkinInfluences(mesh,
                                 sourceVertexIndices[vertexIndex],
                                 jointIndices[vertexIndex],
                                 jointWeights[vertexIndex],
                                 &skinDeformerIndex);
            vertices[vertexIndex].joints = jointIndices[vertexIndex];
            vertices[vertexIndex].weights = jointWeights[vertexIndex];
        }
        gpuSkinningEligible = true;
    }

    model.meshes.emplace_back(engine, &model, vertices, false);

    bindFbxMeshAnimation(model,
                         mesh,
                         skinDeformerIndex,
                         std::move(sourceVertexIndices),
                         std::move(jointIndices),
                         std::move(jointWeights),
                         gpuSkinningEligible);

    Primitive* primitive = model.meshes.back().primitives.empty()
        ? nullptr
        : model.meshes.back().primitives.front().get();
    if (!primitive)
    {
        return;
    }

    primitive->gpuSkinningEnabled = gpuSkinningEligible;
    primitive->cullMode = PrimitiveCullMode::Disabled;
    primitive->createTextureSampler();

    const bool appliedTexture = applyFbxBaseColorTexture(model.meshes.back(), mesh, materialIndex);
    if (!appliedTexture)
    {
        applyFbxMaterialColorFallback(model.meshes.back(), mesh, materialIndex);
    }
    if (primitive->textureImage == VK_NULL_HANDLE)
    {
        primitive->createDefaultTexture();
    }
    primitive->finalizeTextureResources();

    std::cout << summaryLabel << ": mesh=\""
              << (mesh->name.data ? std::string(mesh->name.data, mesh->name.length) : std::string("<unnamed>"))
              << "\" materialIndex=" << materialIndex
              << " verts=" << primitive->vertexCount
              << " indices=" << primitive->indexCount
              << " textureApplied=" << (appliedTexture ? "yes" : "no")
              << " alphaMode=" << primitiveAlphaModeName(primitive->alphaMode)
              << " cullMode=" << primitiveCullModeName(primitive->cullMode)
              << " alphaCutoff=" << primitive->alphaCutoff
              << std::endl;
}

struct CombinedPrimitiveData
{
    int materialIndex = -1;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

}

VkVertexInputBindingDescription Vertex::getBindingDescription()
{
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(Vertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return bindingDescription;
}

std::array<VkVertexInputAttributeDescription, 5> Vertex::getAttributeDescriptions()
{
    std::array<VkVertexInputAttributeDescription, 5> attributeDescriptions{};

    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[0].offset = offsetof(Vertex, pos);

    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[1].offset = offsetof(Vertex, normal);

    attributeDescriptions[2].binding = 0;
    attributeDescriptions[2].location = 2;
    attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[2].offset = offsetof(Vertex, texCoord);

    attributeDescriptions[3].binding = 0;
    attributeDescriptions[3].location = 3;
    attributeDescriptions[3].format = VK_FORMAT_R32G32B32A32_UINT;
    attributeDescriptions[3].offset = offsetof(Vertex, joints);

    attributeDescriptions[4].binding = 0;
    attributeDescriptions[4].location = 4;
    attributeDescriptions[4].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescriptions[4].offset = offsetof(Vertex, weights);

    return attributeDescriptions;
}

Mesh::Mesh(Engine *engine, Model *model, tinygltf::Mesh gltfmesh)
    : model(model), engine(engine)
{
    std::unordered_map<int, size_t> materialToGroupIndex;
    std::vector<CombinedPrimitiveData> combinedGroups;

    for (const auto &tprimitive : gltfmesh.primitives)
    {
        std::vector<Vertex> primitiveVertices;
        std::vector<uint32_t> primitiveIndices;
        if (!extractVerticesAndIndicesFromGltfPrimitive(*model->tgltfModel, tprimitive, glm::mat4(1.0f), primitiveVertices, primitiveIndices))
        {
            continue;
        }

        const int materialIndex = tprimitive.material;
        auto it = materialToGroupIndex.find(materialIndex);
        if (it == materialToGroupIndex.end())
        {
            const size_t newIndex = combinedGroups.size();
            materialToGroupIndex.emplace(materialIndex, newIndex);
            combinedGroups.push_back(CombinedPrimitiveData{materialIndex, {}, {}});
            it = materialToGroupIndex.find(materialIndex);
        }

        CombinedPrimitiveData& group = combinedGroups[it->second];
        const uint32_t vertexOffset = static_cast<uint32_t>(group.vertices.size());
        group.vertices.insert(group.vertices.end(), primitiveVertices.begin(), primitiveVertices.end());
        group.indices.reserve(group.indices.size() + primitiveIndices.size());
        for (uint32_t index : primitiveIndices)
        {
            group.indices.push_back(vertexOffset + index);
        }
    }

    primitives.reserve(combinedGroups.size());
    for (auto& group : combinedGroups)
    {
        if (group.vertices.empty() || group.indices.empty())
        {
            continue;
        }
        primitives.emplace_back(std::make_unique<Primitive>(engine, this, group.vertices, group.indices, group.materialIndex));
    }
}

Mesh::Mesh(Engine *engine, Model *model, const std::vector<GltfCombinedPrimitiveData>& combinedPrimitives)
    : model(model), engine(engine)
{
    primitives.reserve(combinedPrimitives.size());
    for (const auto& group : combinedPrimitives)
    {
        if (group.vertices.empty() || group.indices.empty())
        {
            continue;
        }
        primitives.emplace_back(std::make_unique<Primitive>(engine, this, group.vertices, group.indices, group.materialIndex));
    }
}

Mesh::Mesh(Engine *engine, Model *model, const std::vector<Vertex> &vertices, bool initializeTextureResources)
    : model(model), engine(engine)
{
    primitives.emplace_back(std::make_unique<Primitive>(engine, this, vertices, initializeTextureResources));
}

Mesh::Mesh(Mesh &&other) noexcept
    : model(other.model),
      primitives(std::move(other.primitives)),
      engine(other.engine)
{
}

Mesh &Mesh::operator=(Mesh &&other) noexcept
{
    if (this != &other)
    {
        model = other.model;
        engine = other.engine;
        primitives = std::move(other.primitives);
    }
    return *this;
}

Mesh::~Mesh()
{
}

Model::Model(const std::vector<Vertex> &vertices, Engine *engine) : engine(engine)
{
    if (engine) engine->beginBatchUpload();
    std::cout << "[Debug] Creating procedural Model at " << this << " with " << vertices.size() << " vertices." << std::endl;
    meshes.emplace_back(engine, this, vertices);
    normalizedBaseTransform = glm::mat4(1.0f);
    worldTransform = normalizedBaseTransform;
    std::cout << "[Debug] Model " << this << " finished initialization. Mesh count: " << meshes.size() << std::endl;
    recomputeBounds();
    if (engine) engine->endBatchUpload();
}

Model::Model(const std::string &gltfPath, Engine *engine, bool consolidateMeshes) : engine(engine)
{
    if (engine) engine->beginBatchUpload();
    name = gltfPath;
    meshConsolidationEnabled = consolidateMeshes;
    if (hasExtension(gltfPath, ".fbx"))
    {
        std::cout << "[Debug] Loading FBX Model at " << this << " from " << gltfPath << std::endl;
        ufbx_load_opts opts = {};
        opts.generate_missing_normals = true;
        ufbx_error error;
        ufbx_scene* scene = ufbx_load_file(gltfPath.c_str(), &opts, &error);
        if (!scene)
        {
            if (engine) engine->endBatchUpload();
            throw std::runtime_error("FBX error: " + std::string(error.description.data, error.description.length));
        }

        for (size_t i = 0; i < scene->anim_stacks.count; ++i)
        {
            const ufbx_anim_stack* animStack = scene->anim_stacks.data[i];
            if (!animStack)
            {
                continue;
            }
            std::string clipName = animStack->name.data && animStack->name.length > 0
                ? std::string(animStack->name.data, animStack->name.length)
                : ("Animation " + std::to_string(i));
            if (clipName.empty())
            {
                clipName = "Animation " + std::to_string(i);
            }
            animationClips.push_back(AnimationClipInfo{clipName});
        }
        if (!animationClips.empty())
        {
            animated = true;
            fbxAnimationRuntime = motive::animation::createFbxRuntime(scene);
        }
        std::cout << "[FBX] Animation stacks discovered: " << animationClips.size();
        if (!animationClips.empty())
        {
            std::cout << " [";
            for (size_t i = 0; i < animationClips.size(); ++i)
            {
                if (i > 0)
                {
                    std::cout << ", ";
                }
                std::cout << animationClips[i].name;
            }
            std::cout << "]";
        }
        std::cout << std::endl;

        for (size_t i = 0; i < scene->meshes.count; ++i)
        {
            const ufbx_mesh* mesh = scene->meshes.data[i];
            bool importedMaterialParts = false;
            if (mesh && mesh->material_parts.count > 1)
            {
                for (size_t partIndex = 0; partIndex < mesh->material_parts.count; ++partIndex)
                {
                    const ufbx_mesh_part& materialPart = mesh->material_parts.data[partIndex];
                    std::vector<uint32_t> sourceVertexIndices;
                    std::vector<Vertex> vertices = buildVerticesFromFbxMeshPart(mesh, materialPart, animated ? &sourceVertexIndices : nullptr);
                    if (vertices.empty())
                    {
                        continue;
                    }

                    const int materialIndex = materialIndexForFbxMeshPart(mesh, materialPart);
                    importFbxVerticesIntoModel(*this,
                                               engine,
                                               mesh,
                                               std::move(vertices),
                                               std::move(sourceVertexIndices),
                                               materialIndex,
                                               "[FBX] Material part import summary");
                    importedMaterialParts = true;
                }
            }
            if (importedMaterialParts)
            {
                continue;
            }

            std::vector<uint32_t> sourceVertexIndices;
            std::vector<Vertex> vertices = buildVerticesFromFbxMesh(mesh, animated ? &sourceVertexIndices : nullptr);
            if (!vertices.empty())
            {
                importFbxVerticesIntoModel(*this,
                                           engine,
                                           mesh,
                                           std::move(vertices),
                                           std::move(sourceVertexIndices),
                                           0,
                                           "[FBX] Mesh import summary");
            }
        }

        if (!animated)
        {
            ufbx_free_scene(scene);
        }

        if (meshes.empty())
        {
            if (engine) engine->endBatchUpload();
            if (animated)
            {
                fbxAnimationRuntime.reset();
                animated = false;
            }
            throw std::runtime_error("FBX contains no renderable meshes.");
        }

        std::cout << "[Debug] FBX Model " << this << " loaded successfully. Mesh count: " << meshes.size() << std::endl;
        recomputeBounds();
        if (animated && fbxAnimationRuntime && !animationClips.empty())
        {
            setAnimationPlaybackState(animationClips.front().name, true, true, 1.0f);
            updateAnimation(0.0);
        }
        if (engine) engine->endBatchUpload();
        return;
    }

    std::cout << "[Debug] Loading GLTF Model at " << this << " from " << gltfPath << std::endl;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    bool success = false;
    const std::string ext = gltfPath.substr(gltfPath.find_last_of('.'));
    tgltfModel = new tinygltf::Model();

    if (ext == ".glb")
    {
        success = loader.LoadBinaryFromFile(tgltfModel, &err, &warn, gltfPath);
    }
    else if (ext == ".gltf")
    {
        success = loader.LoadASCIIFromFile(tgltfModel, &err, &warn, gltfPath);
    }
    else
    {
        if (engine) engine->endBatchUpload();
        throw std::runtime_error("Unsupported file extension: " + ext);
    }

    if (!warn.empty())
        std::cout << "GLTF warning: " << warn << std::endl;
    if (!err.empty())
    {
        if (engine) engine->endBatchUpload();
        throw std::runtime_error("GLTF error: " + err);
    }
    if (!success)
    {
        if (engine) engine->endBatchUpload();
        throw std::runtime_error("Failed to load GLTF file: " + gltfPath);
    }
    if (tgltfModel->meshes.empty())
    {
        if (engine) engine->endBatchUpload();
        throw std::runtime_error("GLTF contains no meshes.");
    }

    for (size_t i = 0; i < tgltfModel->animations.size(); ++i)
    {
        const auto& animation = tgltfModel->animations[i];
        std::string clipName = animation.name.empty() ? ("Animation " + std::to_string(i)) : animation.name;
        animationClips.push_back(AnimationClipInfo{clipName});
    }

    std::vector<int> rootNodeIndices;
    if (tgltfModel->defaultScene >= 0 && tgltfModel->defaultScene < static_cast<int>(tgltfModel->scenes.size()))
    {
        const auto& scene = tgltfModel->scenes[static_cast<size_t>(tgltfModel->defaultScene)];
        rootNodeIndices.assign(scene.nodes.begin(), scene.nodes.end());
    }
    else if (!tgltfModel->scenes.empty())
    {
        const auto& scene = tgltfModel->scenes.front();
        rootNodeIndices.assign(scene.nodes.begin(), scene.nodes.end());
    }
    else
    {
        rootNodeIndices.resize(tgltfModel->nodes.size());
        std::iota(rootNodeIndices.begin(), rootNodeIndices.end(), 0);
    }

    if (meshConsolidationEnabled)
    {
        std::unordered_map<int, size_t> materialToGroupIndex;
        std::vector<GltfCombinedPrimitiveData> combinedGroups;
        std::vector<std::vector<GltfCombinedPrimitiveData>> unusedNodeGroups;
        for (int nodeIndex : rootNodeIndices)
        {
            collectGltfNodeGroupsRecursive(*tgltfModel, nodeIndex, glm::mat4(1.0f), unusedNodeGroups, &materialToGroupIndex, &combinedGroups);
        }

        meshes.clear();
        meshes.reserve(combinedGroups.empty() ? 0 : 1);
        if (!combinedGroups.empty())
        {
            meshes.emplace_back(engine, this, combinedGroups);
        }
    }
    else
    {
        std::vector<std::vector<GltfCombinedPrimitiveData>> perNodeGroups;
        for (int nodeIndex : rootNodeIndices)
        {
            collectGltfNodeGroupsRecursive(*tgltfModel, nodeIndex, glm::mat4(1.0f), perNodeGroups);
        }

        meshes.clear();
        meshes.reserve(perNodeGroups.size());
        for (const auto& nodeGroups : perNodeGroups)
        {
            if (!nodeGroups.empty())
            {
                meshes.emplace_back(engine, this, nodeGroups);
            }
        }
    }

    std::cout << "[Debug] GLTF Model " << this << " loaded successfully. Mesh count: " << meshes.size() << std::endl;
    recomputeBounds();
    if (engine) engine->endBatchUpload();
}

Model::~Model()
{
    meshes.clear();

    if (tgltfModel != nullptr)
    {
        delete tgltfModel;
        tgltfModel = nullptr;
    }
}
