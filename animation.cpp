#include "animation.h"

#include "model.h"
#include "ufbx.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

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

glm::mat4 toGlmMat4(const ufbx_matrix& value)
{
    glm::mat4 result(1.0f);
    result[0][0] = static_cast<float>(value.m00); result[0][1] = static_cast<float>(value.m10); result[0][2] = static_cast<float>(value.m20); result[0][3] = 0.0f;
    result[1][0] = static_cast<float>(value.m01); result[1][1] = static_cast<float>(value.m11); result[1][2] = static_cast<float>(value.m21); result[1][3] = 0.0f;
    result[2][0] = static_cast<float>(value.m02); result[2][1] = static_cast<float>(value.m12); result[2][2] = static_cast<float>(value.m22); result[2][3] = 0.0f;
    result[3][0] = static_cast<float>(value.m03); result[3][1] = static_cast<float>(value.m13); result[3][2] = static_cast<float>(value.m23); result[3][3] = 1.0f;
    return result;
}

const ufbx_mesh* findMeshByElementId(const ufbx_scene* scene, uint32_t elementId)
{
    if (!scene)
    {
        return nullptr;
    }
    for (size_t i = 0; i < scene->meshes.count; ++i)
    {
        const ufbx_mesh* mesh = scene->meshes.data[i];
        if (mesh && mesh->element_id == elementId)
        {
            return mesh;
        }
    }
    return nullptr;
}

std::vector<Vertex> buildAnimatedVerticesFromFbxMesh(const ufbx_mesh* mesh, const std::vector<uint32_t>& vertexIndices)
{
    std::vector<Vertex> vertices;
    if (!mesh || vertexIndices.empty())
    {
        return vertices;
    }

    vertices.resize(vertexIndices.size());
    for (size_t i = 0; i < vertexIndices.size(); ++i)
    {
        const size_t index = vertexIndices[i];
        Vertex vertex{};
        const bool useSkinnedPosition = mesh->skinned_position.exists;
        const bool useSkinnedNormal = mesh->skinned_normal.exists;
        vertex.pos = useSkinnedPosition
            ? toGlmVec3(ufbx_get_vertex_vec3(&mesh->skinned_position, index))
            : toGlmVec3(ufbx_get_vertex_vec3(&mesh->vertex_position, index));
        vertex.normal = useSkinnedNormal
            ? toGlmVec3(ufbx_get_vertex_vec3(&mesh->skinned_normal, index))
            : (mesh->vertex_normal.exists
                ? toGlmVec3(ufbx_get_vertex_vec3(&mesh->vertex_normal, index))
                : glm::vec3(0.0f, 1.0f, 0.0f));
        vertex.texCoord = mesh->vertex_uv.exists
            ? toGlmVec2(ufbx_get_vertex_vec2(&mesh->vertex_uv, index))
            : glm::vec2(0.0f);
        vertices[i] = vertex;
    }
    return vertices;
}

double initialPlaybackTime(const motive::animation::FbxClipRuntime& clip, bool playing)
{
    if (!playing)
    {
        return clip.timeBegin;
    }

    const double clipDuration = std::max(clip.timeEnd - clip.timeBegin, 0.0);
    if (clipDuration <= 0.0)
    {
        return clip.timeBegin;
    }

    // Many authored FBX clips include a bind/rest pose sample at the exact start.
    // Prime playback one frame forward so the first visible frame is the animated pose.
    const double frameStep = 1.0 / 30.0;
    return std::min(clip.timeBegin + std::min(frameStep, clipDuration), clip.timeEnd);
}

}  // namespace

namespace motive::animation {

FbxRuntime::~FbxRuntime()
{
    if (scene)
    {
        ufbx_free_scene(scene);
        scene = nullptr;
    }
}

std::unique_ptr<FbxRuntime> createFbxRuntime(ufbx_scene* scene)
{
    auto runtime = std::make_unique<FbxRuntime>();
    runtime->scene = scene;
    if (!scene)
    {
        return runtime;
    }

    runtime->clips.reserve(scene->anim_stacks.count);
    for (size_t i = 0; i < scene->anim_stacks.count; ++i)
    {
        const ufbx_anim_stack* animStack = scene->anim_stacks.data[i];
        if (!animStack || !animStack->anim)
        {
            continue;
        }
        std::string clipName = animStack->name.data && animStack->name.length > 0
            ? std::string(animStack->name.data, animStack->name.length)
            : ("Animation " + std::to_string(i));
        runtime->clips.push_back(FbxClipRuntime{clipName, animStack->anim, animStack->time_begin, animStack->time_end});
    }
    if (!runtime->clips.empty())
    {
        runtime->activeClipIndex = 0;
        runtime->timeSeconds = initialPlaybackTime(runtime->clips[0], runtime->playing);
    }
    return runtime;
}

void addFbxMeshBinding(FbxRuntime& runtime, uint32_t meshElementId, std::vector<uint32_t> vertexIndices)
{
    runtime.meshBindings.push_back(FbxMeshBinding{meshElementId, std::move(vertexIndices)});
}

void addFbxMeshBinding(FbxRuntime& runtime,
                       uint32_t meshElementId,
                       uint32_t skinDeformerIndex,
                       std::vector<uint32_t> vertexIndices,
                       std::vector<glm::uvec4> jointIndices,
                       std::vector<glm::vec4> jointWeights)
{
    FbxMeshBinding binding;
    binding.meshElementId = meshElementId;
    binding.skinDeformerIndex = skinDeformerIndex;
    binding.vertexIndices = std::move(vertexIndices);
    binding.jointIndices = std::move(jointIndices);
    binding.jointWeights = std::move(jointWeights);
    binding.gpuSkinningEligible = !binding.jointIndices.empty() &&
                                  binding.jointIndices.size() == binding.vertexIndices.size() &&
                                  binding.jointWeights.size() == binding.vertexIndices.size();
    runtime.meshBindings.push_back(std::move(binding));
}

void setFbxPlaybackState(FbxRuntime& runtime, const std::string& clipName, bool playing, bool loop, float speed)
{
    runtime.playing = playing;
    runtime.loop = loop;
    runtime.speed = speed;

    if (runtime.clips.empty())
    {
        runtime.activeClipIndex = -1;
        runtime.timeSeconds = 0.0;
        return;
    }

    int nextClipIndex = runtime.activeClipIndex;
    if (!clipName.empty())
    {
        for (int i = 0; i < static_cast<int>(runtime.clips.size()); ++i)
        {
            if (runtime.clips[i].name == clipName)
            {
                nextClipIndex = i;
                break;
            }
        }
    }
    if (nextClipIndex < 0)
    {
        nextClipIndex = 0;
    }
    if (nextClipIndex != runtime.activeClipIndex)
    {
        runtime.activeClipIndex = nextClipIndex;
        runtime.timeSeconds = initialPlaybackTime(runtime.clips[nextClipIndex], runtime.playing);
    }
}

bool updateFbxAnimation(Model& model, FbxRuntime& runtime, double deltaSeconds)
{
    if (!runtime.scene || runtime.activeClipIndex < 0 || runtime.activeClipIndex >= static_cast<int>(runtime.clips.size()))
    {
        return false;
    }

    const FbxClipRuntime& clip = runtime.clips[runtime.activeClipIndex];
    const double clipDuration = std::max(clip.timeEnd - clip.timeBegin, 0.0);
    if (runtime.playing && clipDuration > 0.0)
    {
        runtime.timeSeconds += deltaSeconds * static_cast<double>(runtime.speed);
        if (runtime.loop)
        {
            while (runtime.timeSeconds > clip.timeEnd)
            {
                runtime.timeSeconds -= clipDuration;
            }
        }
        else if (runtime.timeSeconds > clip.timeEnd)
        {
            runtime.timeSeconds = clip.timeEnd;
        }
    }

    // Check if any mesh needs CPU skinning (skinned vertex positions).
    // GPU skinning only needs joint matrices, which don't require evaluate_skinning=true.
    bool needsCpuSkinning = false;
    const size_t meshCount = std::min(model.meshes.size(), runtime.meshBindings.size());
    for (size_t meshIndex = 0; meshIndex < meshCount; ++meshIndex)
    {
        const FbxMeshBinding& binding = runtime.meshBindings[meshIndex];
        if (meshIndex < model.meshes.size() && !model.meshes[meshIndex].primitives.empty())
        {
            const Primitive* primitive = model.meshes[meshIndex].primitives.front().get();
            if (primitive && !primitive->gpuSkinningEnabled && binding.gpuSkinningEligible)
            {
                needsCpuSkinning = true;
                break;
            }
        }
    }

    ufbx_evaluate_opts evalOpts = {};
    evalOpts.evaluate_skinning = needsCpuSkinning;  // Skip expensive topology sort for GPU skinning
    ufbx_error error;
    ufbx_scene* evaluatedScene = ufbx_evaluate_scene(runtime.scene, clip.anim, runtime.timeSeconds, &evalOpts, &error);
    if (!evaluatedScene)
    {
        return false;
    }

    bool updated = false;
    for (size_t meshIndex = 0; meshIndex < meshCount; ++meshIndex)
    {
        const FbxMeshBinding& binding = runtime.meshBindings[meshIndex];
        const ufbx_mesh* evaluatedMesh = findMeshByElementId(evaluatedScene, binding.meshElementId);
        if (!evaluatedMesh || model.meshes[meshIndex].primitives.empty() || !model.meshes[meshIndex].primitives.front())
        {
            continue;
        }

        Primitive* primitive = model.meshes[meshIndex].primitives.front().get();
        if (!primitive)
        {
            continue;
        }

        // Build animated vertices for bounds computation (needed even with GPU skinning)
        std::vector<Vertex> vertices = buildAnimatedVerticesFromFbxMesh(evaluatedMesh, binding.vertexIndices);
        
        if (primitive->gpuSkinningEnabled && binding.gpuSkinningEligible)
        {
            const ufbx_skin_deformer* skin = binding.skinDeformerIndex < evaluatedMesh->skin_deformers.count
                ? evaluatedMesh->skin_deformers.data[binding.skinDeformerIndex]
                : nullptr;
            const ufbx_node* meshNode = evaluatedMesh->instances.count > 0 ? evaluatedMesh->instances.data[0] : nullptr;
            if (skin && meshNode)
            {
                glm::mat4 invGeometryToWorld = glm::inverse(toGlmMat4(meshNode->geometry_to_world));
                std::vector<glm::mat4> jointMatrices;
                jointMatrices.reserve(std::min<size_t>(skin->clusters.count, kMaxPrimitiveSkinJoints));
                for (size_t clusterIndex = 0; clusterIndex < skin->clusters.count && clusterIndex < kMaxPrimitiveSkinJoints; ++clusterIndex)
                {
                    const ufbx_skin_cluster* cluster = skin->clusters.data[clusterIndex];
                    if (!cluster)
                    {
                        jointMatrices.push_back(glm::mat4(1.0f));
                        continue;
                    }
                    jointMatrices.push_back(invGeometryToWorld * toGlmMat4(cluster->geometry_to_world));
                }
                primitive->updateSkinningMatrices(jointMatrices);
                // Still update CPU vertices for bounds computation, even with GPU skinning
                if (!vertices.empty())
                {
                    primitive->cpuVertices = vertices;
                }
                updated = true;
                continue;
            }
        }
        if (vertices.empty())
        {
            continue;
        }

        if (binding.gpuSkinningEligible)
        {
            const size_t count = std::min(vertices.size(), binding.jointIndices.size());
            for (size_t i = 0; i < count; ++i)
            {
                vertices[i].joints = binding.jointIndices[i];
                vertices[i].weights = binding.jointWeights[i];
            }
        }

        primitive->updateVertexData(vertices);
        updated = true;
    }

    ufbx_free_scene(evaluatedScene);
    if (updated)
    {
        model.recomputeBounds();
    }
    return updated;
}

}  // namespace motive::animation
