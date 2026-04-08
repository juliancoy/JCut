#pragma once

#include <array>
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>

#include <QImage>
#include <QString>
#include <GLFW/glfw3.h>
#include <../tinygltf/tiny_gltf.h>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "object_transform.h"
#include "animation.h"

class Engine;
class Model;
class Mesh;
class Primitive;
class Texture;

constexpr uint32_t kMaxPrimitiveSkinJoints = 256;

struct SkinMatrixPalette
{
    std::array<glm::mat4, kMaxPrimitiveSkinJoints> joints{};
};

struct Vertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 texCoord;
    glm::uvec4 joints = glm::uvec4(0u);
    glm::vec4 weights = glm::vec4(0.0f);

    static VkVertexInputBindingDescription getBindingDescription();
    static std::array<VkVertexInputAttributeDescription, 5> getAttributeDescriptions();
};

struct GltfCombinedPrimitiveData {
    int materialIndex = -1;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

struct SharedTextureResources {
    Engine* engine = nullptr;
    VkImage textureImage = VK_NULL_HANDLE;
    VkDeviceMemory textureImageMemory = VK_NULL_HANDLE;
    VkImageView textureImageView = VK_NULL_HANDLE;
    VkSampler textureSampler = VK_NULL_HANDLE;
    uint32_t textureWidth = 0;
    uint32_t textureHeight = 0;
    VkFormat textureFormat = VK_FORMAT_UNDEFINED;

    ~SharedTextureResources();
};

#include "primitive.h"

class Mesh {
public:
    Mesh(Engine* engine, Model* model, const std::vector<Vertex>& vertices, bool initializeTextureResources = true);
    Mesh(Engine* engine, Model* model, tinygltf::Mesh);
    Mesh(Engine* engine, Model* model, const std::vector<GltfCombinedPrimitiveData>& combinedPrimitives);
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&& other) noexcept;
    Mesh& operator=(Mesh&& other) noexcept;
    ~Mesh();

    Model* model = nullptr;
    std::vector<std::unique_ptr<Primitive>> primitives;
    Engine* engine = nullptr;
};

class Model {
public:
    struct AnimationClipInfo
    {
        std::string name;
    };

    Model(const std::string& gltfPath, Engine* engine, bool consolidateMeshes = true);
    Model(const std::vector<Vertex>& vertices, Engine* engine);
    ~Model();

    void scaleToUnitBox();
    void resizeToUnitBox();
    void scale(const glm::vec3& factors);
    void translate(const glm::vec3& offset);
    void rotate(float angleRadians, const glm::vec3& axis);
    void rotate(float xDegrees, float yDegrees, float zDegrees);
    void setSceneTransform(const glm::vec3& translation, const glm::vec3& rotationDegrees, const glm::vec3& scaleFactors);
    void setPaintOverride(bool enabled, const glm::vec3& color);
    void setAnimationPlaybackState(const std::string& clipName, bool playing, bool loop, float speed);
    void updateAnimation(double deltaSeconds);
    void recomputeBounds();

    tinygltf::Model* tgltfModel = nullptr;

    std::string name = "Added Vertices";
    std::vector<Mesh> meshes;
    Engine* engine = nullptr;
    GLFWwindow* window = nullptr;
    std::vector<Texture*> textures;
    bool visible = true;
    bool animated = false;
    glm::mat4 normalizedBaseTransform = glm::mat4(1.0f);
    glm::mat4 worldTransform = glm::mat4(1.0f);
    glm::vec3 boundsCenter = glm::vec3(0.0f);
    float boundsRadius = 0.0f;
    bool meshConsolidationEnabled = true;
    std::vector<AnimationClipInfo> animationClips;
    std::unique_ptr<motive::animation::FbxRuntime> fbxAnimationRuntime;

private:
    void applyTransformToPrimitives(const glm::mat4& transform);
    bool computeProceduralBounds(glm::vec3& minBounds, glm::vec3& maxBounds) const;

    std::unordered_map<int, std::weak_ptr<SharedTextureResources>> gltfMaterialTextureCache;
    friend class Primitive;
};
