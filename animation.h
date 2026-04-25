#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>

struct ufbx_scene;
struct ufbx_mesh;
struct ufbx_anim;
struct Vertex;
class Model;

namespace motive::animation {

struct FbxClipRuntime
{
    std::string name;
    const ufbx_anim* anim = nullptr;
    double timeBegin = 0.0;
    double timeEnd = 0.0;
};

struct FbxMeshBinding
{
    uint32_t meshElementId = 0;
    std::vector<uint32_t> vertexIndices;
    uint32_t skinDeformerIndex = 0;
    std::vector<glm::uvec4> jointIndices;
    std::vector<glm::vec4> jointWeights;
    bool gpuSkinningEligible = false;
};

struct FbxRuntime
{
    ~FbxRuntime();

    struct ufbx_scene* scene = nullptr;
    std::vector<FbxClipRuntime> clips;
    std::vector<FbxMeshBinding> meshBindings;
    int activeClipIndex = -1;
    double timeSeconds = 0.0;
    bool playing = true;
    bool loop = true;
    float speed = 1.0f;
};

std::unique_ptr<FbxRuntime> createFbxRuntime(ufbx_scene* scene);
void addFbxMeshBinding(FbxRuntime& runtime, uint32_t meshElementId, std::vector<uint32_t> vertexIndices);
void addFbxMeshBinding(FbxRuntime& runtime,
                       uint32_t meshElementId,
                       uint32_t skinDeformerIndex,
                       std::vector<uint32_t> vertexIndices,
                       std::vector<glm::uvec4> jointIndices,
                       std::vector<glm::vec4> jointWeights);
void setFbxPlaybackState(FbxRuntime& runtime, const std::string& clipName, bool playing, bool loop, float speed);
bool updateFbxAnimation(Model& model, FbxRuntime& runtime, double deltaSeconds);

}  // namespace motive::animation
