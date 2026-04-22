#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif

#include "model.h"
#include "engine.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/component_wise.hpp>

void Model::scaleToUnitBox()
{
    glm::vec3 minBounds(std::numeric_limits<float>::max());
    glm::vec3 maxBounds(std::numeric_limits<float>::lowest());
    bool foundPositions = false;

    if (tgltfModel)
    {
        for (const auto &meshData : tgltfModel->meshes)
        {
            for (const auto &primitiveData : meshData.primitives)
            {
                auto attrIt = primitiveData.attributes.find("POSITION");
                if (attrIt == primitiveData.attributes.end())
                {
                    continue;
                }

                const tinygltf::Accessor &accessor = tgltfModel->accessors[attrIt->second];
                const tinygltf::BufferView &bufferView = tgltfModel->bufferViews[accessor.bufferView];
                const tinygltf::Buffer &buffer = tgltfModel->buffers[bufferView.buffer];

                size_t stride = accessor.ByteStride(bufferView);
                if (stride == 0)
                {
                    stride = 3 * sizeof(float);
                }

                const uint8_t *dataPtr = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
                for (size_t i = 0; i < accessor.count; ++i)
                {
                    const float *position = reinterpret_cast<const float *>(dataPtr + i * stride);
                    glm::vec3 pos(position[0], position[1], position[2]);
                    minBounds = glm::min(minBounds, pos);
                    maxBounds = glm::max(maxBounds, pos);
                    foundPositions = true;
                }
            }
        }
    }
    else
    {
        foundPositions = computeProceduralBounds(minBounds, maxBounds);
        if (!foundPositions)
        {
            std::cerr << "[Warning] scaleToUnitBox: Unable to compute bounds for procedural model " << name << std::endl;
            return;
        }
    }

    if (!foundPositions)
    {
        std::cerr << "[Warning] scaleToUnitBox: No position data found on model " << name << std::endl;
        return;
    }

    glm::vec3 extent = maxBounds - minBounds;
    float maxExtent = glm::compMax(extent);
    if (maxExtent <= 0.0f || !std::isfinite(maxExtent))
    {
        std::cerr << "[Warning] scaleToUnitBox: Invalid extent for model " << name << std::endl;
        return;
    }

    glm::vec3 center = (minBounds + maxBounds) * 0.5f;
    float scale = 1.0f / maxExtent;

    glm::mat4 translation = glm::translate(glm::mat4(1.0f), -center);
    glm::mat4 scaleMat = glm::scale(glm::mat4(1.0f), glm::vec3(scale));
    glm::mat4 transform = scaleMat * translation;
    normalizedBaseTransform = transform;
    worldTransform = transform;

    for (auto &mesh : meshes)
    {
        for (auto &primitive : mesh.primitives)
        {
            if (primitive)
            {
                primitive->transform = transform;
                if (primitive->ObjectTransformUBOMapped)
                {
                    ObjectTransform updated = primitive->buildObjectTransformData();
                    memcpy(primitive->ObjectTransformUBOMapped, &updated, sizeof(updated));
                }
            }
        }
    }

    recomputeBounds();
    std::cout << "[Debug] Model " << name << " scaled to unit box (scale=" << scale << ")" << std::endl;
}

void Model::resizeToUnitBox()
{
    scaleToUnitBox();
}

void Model::scale(const glm::vec3 &factors)
{
    applyTransformToPrimitives(glm::scale(glm::mat4(1.0f), factors));
}

void Model::translate(const glm::vec3 &offset)
{
    applyTransformToPrimitives(glm::translate(glm::mat4(1.0f), offset));
}

void Model::rotate(float angleRadians, const glm::vec3 &axis)
{
    glm::vec3 normAxis = glm::length(axis) == 0.0f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::normalize(axis);
    applyTransformToPrimitives(glm::rotate(glm::mat4(1.0f), angleRadians, normAxis));
}

void Model::rotate(float xDegrees, float yDegrees, float zDegrees)
{
    glm::mat4 rotationMat(1.0f);

    if (xDegrees != 0.0f)
    {
        rotationMat = glm::rotate(rotationMat, glm::radians(xDegrees), glm::vec3(1.0f, 0.0f, 0.0f));
    }
    if (yDegrees != 0.0f)
    {
        rotationMat = glm::rotate(rotationMat, glm::radians(yDegrees), glm::vec3(0.0f, 1.0f, 0.0f));
    }
    if (zDegrees != 0.0f)
    {
        rotationMat = glm::rotate(rotationMat, glm::radians(zDegrees), glm::vec3(0.0f, 0.0f, 1.0f));
    }

    applyTransformToPrimitives(rotationMat);
}

void Model::setSceneTransform(const glm::vec3& translation, const glm::vec3& rotationDegrees, const glm::vec3& scaleFactors)
{
    glm::mat4 modelMatrix = glm::translate(glm::mat4(1.0f), translation);
    modelMatrix = glm::rotate(modelMatrix, glm::radians(rotationDegrees.x), glm::vec3(1.0f, 0.0f, 0.0f));
    modelMatrix = glm::rotate(modelMatrix, glm::radians(rotationDegrees.y), glm::vec3(0.0f, 1.0f, 0.0f));
    modelMatrix = glm::rotate(modelMatrix, glm::radians(rotationDegrees.z), glm::vec3(0.0f, 0.0f, 1.0f));
    modelMatrix = glm::scale(modelMatrix, scaleFactors);
    modelMatrix = modelMatrix * normalizedBaseTransform;

    worldTransform = modelMatrix;
    for (auto& mesh : meshes)
    {
        for (auto& primitive : mesh.primitives)
        {
            if (!primitive)
            {
                continue;
            }
            primitive->transform = modelMatrix;
            if (primitive->ObjectTransformUBOMapped)
            {
                ObjectTransform updated = primitive->buildObjectTransformData();
                memcpy(primitive->ObjectTransformUBOMapped, &updated, sizeof(updated));
            }
        }
    }
    recomputeBounds();
}

void Model::setPaintOverride(bool enabled, const glm::vec3& color)
{
    for (auto& mesh : meshes)
    {
        for (auto& primitive : mesh.primitives)
        {
            if (!primitive)
            {
                continue;
            }
            primitive->paintOverrideEnabled = enabled;
            primitive->paintOverrideColor = color;
            if (primitive->ObjectTransformUBOMapped)
            {
                ObjectTransform updated = primitive->buildObjectTransformData();
                memcpy(primitive->ObjectTransformUBOMapped, &updated, sizeof(updated));
            }
        }
    }
}

void Model::setAnimationPlaybackState(const std::string& clipName, bool playing, bool loop, float speed)
{
    if (fbxAnimationRuntime)
    {
        motive::animation::setFbxPlaybackState(*fbxAnimationRuntime, clipName, playing, loop, speed);
    }
}

void Model::updateAnimation(double deltaSeconds)
{
    if (!visible || !animated)
    {
        return;
    }
    if (fbxAnimationRuntime && engine)
    {
        vkQueueWaitIdle(engine->getGraphicsQueue());
        motive::animation::updateFbxAnimation(*this, *fbxAnimationRuntime, deltaSeconds);
    }
}

void Model::applyTransformToPrimitives(const glm::mat4 &transform)
{
    worldTransform = transform * worldTransform;
    for (auto &mesh : meshes)
    {
        for (auto &primitive : mesh.primitives)
        {
            if (!primitive)
            {
                continue;
            }

            primitive->transform = transform * primitive->transform;

            if (primitive->ObjectTransformUBOMapped)
            {
                ObjectTransform updated = primitive->buildObjectTransformData();
                memcpy(primitive->ObjectTransformUBOMapped, &updated, sizeof(updated));
            }
        }
    }
    recomputeBounds();
}

void Model::recomputeBounds()
{
    glm::vec3 localMin(std::numeric_limits<float>::max());
    glm::vec3 localMax(std::numeric_limits<float>::lowest());
    bool found = false;
    for (const auto &mesh : meshes)
    {
        for (const auto &primitive : mesh.primitives)
        {
            if (!primitive)
                continue;
            for (const auto &vertex : primitive->cpuVertices)
            {
                localMin = glm::min(localMin, vertex.pos);
                localMax = glm::max(localMax, vertex.pos);
                found = true;
            }
        }
    }
    if (!found)
    {
        boundsCenter = glm::vec3(0.0f);
        boundsRadius = 0.0f;
        return;
    }
    glm::vec3 localCenter = (localMin + localMax) * 0.5f;
    float localRadius = glm::length(localMax - localMin) * 0.5f;

    boundsCenter = glm::vec3(worldTransform * glm::vec4(localCenter, 1.0f));

    glm::vec3 scale;
    scale.x = glm::length(glm::vec3(worldTransform[0]));
    scale.y = glm::length(glm::vec3(worldTransform[1]));
    scale.z = glm::length(glm::vec3(worldTransform[2]));
    boundsRadius = localRadius * glm::max(scale.x, glm::max(scale.y, scale.z));
}

bool Model::computeProceduralBounds(glm::vec3 &minBounds, glm::vec3 &maxBounds) const
{
    bool found = false;
    for (const auto &mesh : meshes)
    {
        for (const auto &primitive : mesh.primitives)
        {
            if (!primitive)
            {
                continue;
            }

            for (const auto &vertex : primitive->cpuVertices)
            {
                minBounds = glm::min(minBounds, vertex.pos);
                maxBounds = glm::max(maxBounds, vertex.pos);
                found = true;
            }
        }
    }
    return found;
}
