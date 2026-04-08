#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif

#include "fbx_import.h"
#include "model_import_shared.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QImage>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

#include "stb_image.h"

namespace
{
glm::vec4 materialMapColor(const ufbx_material_map& map)
{
    switch (map.value_components)
    {
    case 4:
        return glm::vec4(static_cast<float>(map.value_vec4.x),
                         static_cast<float>(map.value_vec4.y),
                         static_cast<float>(map.value_vec4.z),
                         static_cast<float>(map.value_vec4.w));
    case 3:
        return glm::vec4(static_cast<float>(map.value_vec3.x),
                         static_cast<float>(map.value_vec3.y),
                         static_cast<float>(map.value_vec3.z),
                         1.0f);
    case 2:
        return glm::vec4(static_cast<float>(map.value_vec2.x),
                         static_cast<float>(map.value_vec2.y),
                         1.0f,
                         1.0f);
    case 1:
        return glm::vec4(static_cast<float>(map.value_real),
                         static_cast<float>(map.value_real),
                         static_cast<float>(map.value_real),
                         1.0f);
    default:
        return glm::vec4(1.0f);
    }
}

float materialMapScalar(const ufbx_material_map& map, float defaultValue = 1.0f)
{
    switch (map.value_components)
    {
    case 4:
        return static_cast<float>(map.value_vec4.x);
    case 3:
        return static_cast<float>(map.value_vec3.x);
    case 2:
        return static_cast<float>(map.value_vec2.x);
    case 1:
        return static_cast<float>(map.value_real);
    default:
        return defaultValue;
    }
}

bool shouldIgnoreScalarOnlyFbxTransparency(const ufbx_material* material)
{
    if (!material)
    {
        return true;
    }

    if (material->fbx.transparency_factor.texture)
    {
        return false;
    }

    if (material->fbx.transparency_color.has_value)
    {
        return false;
    }

    return material->fbx.transparency_factor.has_value;
}

bool shouldIgnoreScalarOnlyFbxPbrOpacity(const ufbx_material* material)
{
    if (!material)
    {
        return true;
    }

    if (material->pbr.opacity.texture)
    {
        return false;
    }

    return material->pbr.opacity.has_value;
}

float extractFbxMaterialAlpha(const ufbx_material* material)
{
    if (!material)
    {
        return 1.0f;
    }

    if (material->pbr.opacity.has_value)
    {
        if (shouldIgnoreScalarOnlyFbxPbrOpacity(material))
        {
            return 1.0f;
        }
        return std::clamp(materialMapScalar(material->pbr.opacity, 1.0f), 0.0f, 1.0f);
    }

    if (material->fbx.transparency_factor.has_value)
    {
        if (shouldIgnoreScalarOnlyFbxTransparency(material))
        {
            return 1.0f;
        }
        const float transparency = std::clamp(materialMapScalar(material->fbx.transparency_factor, 0.0f), 0.0f, 1.0f);
        return 1.0f - transparency;
    }

    if (material->fbx.transparency_color.has_value)
    {
        const glm::vec4 transparency = glm::clamp(materialMapColor(material->fbx.transparency_color), glm::vec4(0.0f), glm::vec4(1.0f));
        return 1.0f - transparency.r;
    }

    return 1.0f;
}

const ufbx_material* materialForFbxMesh(const ufbx_mesh* mesh, int materialIndex = 0)
{
    if (!mesh || mesh->materials.count == 0 || !mesh->materials.data)
    {
        return nullptr;
    }

    const int clampedIndex = materialIndex >= 0 && materialIndex < static_cast<int>(mesh->materials.count) ? materialIndex : 0;
    const ufbx_material* material = mesh->materials.data[clampedIndex];
    if (!material)
    {
        return nullptr;
    }
    return material;
}

int maxImportedTextureDimension()
{
    static int cached = []() {
        const char* env = std::getenv("MOTIVE_MAX_IMPORT_TEXTURE_DIM");
        if (!env || *env == '\0')
        {
            return 4096;
        }
        const int value = std::atoi(env);
        return value > 0 ? value : 4096;
    }();
    return cached;
}

void downscaleImportedImageIfNeeded(QImage& image, const QString& sourceLabel)
{
    if (image.isNull())
    {
        return;
    }
    const int maxDim = maxImportedTextureDimension();
    if (image.width() <= maxDim && image.height() <= maxDim)
    {
        return;
    }

    image = image.scaled(maxDim, maxDim, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    std::cout << "[Import] Downscaled oversized texture: "
              << sourceLabel.toStdString()
              << " -> " << image.width() << "x" << image.height()
              << " (maxDim=" << maxDim << ")" << std::endl;
}

}

const char* primitiveAlphaModeName(PrimitiveAlphaMode mode)
{
    switch (mode)
    {
    case PrimitiveAlphaMode::Opaque:
        return "opaque";
    case PrimitiveAlphaMode::Mask:
        return "mask";
    case PrimitiveAlphaMode::Blend:
        return "blend";
    }
    return "unknown";
}

const char* primitiveCullModeName(PrimitiveCullMode mode)
{
    switch (mode)
    {
    case PrimitiveCullMode::Back:
        return "back";
    case PrimitiveCullMode::Disabled:
        return "none";
    case PrimitiveCullMode::Front:
        return "front";
    }
    return "unknown";
}

PrimitiveAlphaMode classifyAlphaModeFromImage(const QImage& image,
                                              float uniformAlpha,
                                              bool hasOpacityImage)
{
    (void)hasOpacityImage;

    if (image.isNull())
    {
        if (uniformAlpha < 0.999f)
        {
            return PrimitiveAlphaMode::Blend;
        }
        return PrimitiveAlphaMode::Opaque;
    }

    int opaquePixels = 0;
    int transparentPixels = 0;
    int partialPixels = 0;
    const int pixelCount = image.width() * image.height();
    for (int y = 0; y < image.height(); ++y)
    {
        const uchar* scanline = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x)
        {
            const uchar alpha = scanline[x * 4 + 3];
            if (alpha >= 250)
            {
                ++opaquePixels;
            }
            else if (alpha <= 5)
            {
                ++transparentPixels;
            }
            else
            {
                ++partialPixels;
            }
        }
    }

    if (partialPixels == 0)
    {
        if (transparentPixels > 0)
        {
            return PrimitiveAlphaMode::Mask;
        }
        return PrimitiveAlphaMode::Opaque;
    }

    const float partialRatio = pixelCount > 0 ? static_cast<float>(partialPixels) / static_cast<float>(pixelCount) : 1.0f;
    const float transparentRatio = pixelCount > 0 ? static_cast<float>(transparentPixels) / static_cast<float>(pixelCount) : 0.0f;
    if (partialRatio <= 0.02f && transparentRatio >= 0.001f)
    {
        return PrimitiveAlphaMode::Mask;
    }

    return PrimitiveAlphaMode::Blend;
}

bool extractFbxMaterialColor(const ufbx_mesh* mesh, glm::vec4& outColor, int materialIndex)
{
    const ufbx_material* material = materialForFbxMesh(mesh, materialIndex);
    if (!material)
    {
        return false;
    }

    if (material->pbr.base_color.has_value)
    {
        outColor = materialMapColor(material->pbr.base_color);
        if (material->pbr.base_factor.has_value)
        {
            const float factor = static_cast<float>(material->pbr.base_factor.value_real);
            outColor.r *= factor;
            outColor.g *= factor;
            outColor.b *= factor;
        }
        outColor.a = extractFbxMaterialAlpha(material);
        outColor = glm::clamp(outColor, glm::vec4(0.0f), glm::vec4(1.0f));
        return true;
    }

    if (material->fbx.diffuse_color.has_value)
    {
        outColor = materialMapColor(material->fbx.diffuse_color);
        if (material->fbx.diffuse_factor.has_value)
        {
            const float factor = static_cast<float>(material->fbx.diffuse_factor.value_real);
            outColor.r *= factor;
            outColor.g *= factor;
            outColor.b *= factor;
        }
        outColor.a = extractFbxMaterialAlpha(material);
        outColor = glm::clamp(outColor, glm::vec4(0.0f), glm::vec4(1.0f));
        return true;
    }

    return false;
}

bool loadImageFromUfbxTexture(const ufbx_texture* texture,
                              const QString& sourceDirectory,
                              QImage& outImage,
                              QString* outResolvedLabel)
{
    if (!texture)
    {
        return false;
    }

    auto loadViaStbFromBytes = [&](const unsigned char* bytes, int size, const QString& sourceLabel) -> bool
    {
        if (!bytes || size <= 0)
        {
            return false;
        }
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* decoded = stbi_load_from_memory(bytes, size, &width, &height, &channels, STBI_rgb_alpha);
        if (!decoded)
        {
            return false;
        }

        QImage image(decoded, width, height, QImage::Format_RGBA8888);
        outImage = image.copy();
        stbi_image_free(decoded);
        if (!outImage.isNull())
        {
            downscaleImportedImageIfNeeded(outImage, sourceLabel);
            if (outResolvedLabel)
            {
                *outResolvedLabel = sourceLabel;
            }
            std::cout << "[FBX] Loaded texture via stb_image fallback: "
                      << sourceLabel.toStdString()
                      << " (" << width << "x" << height << ")" << std::endl;
            return true;
        }
        return false;
    };

    const QStringList declaredCandidates = {
        ufbxStringToQString(texture->filename),
        ufbxStringToQString(texture->absolute_filename),
        ufbxStringToQString(texture->relative_filename)
    };

    QStringList candidates;
    auto appendCandidate = [&](const QString& candidate)
    {
        if (candidate.isEmpty())
        {
            return;
        }
        if (!candidates.contains(candidate))
        {
            candidates.push_back(candidate);
        }
    };

    for (const QString& candidate : declaredCandidates)
    {
        appendCandidate(candidate);
        const QFileInfo info(candidate);
        const QString baseName = info.fileName();
        if (!sourceDirectory.isEmpty() && !baseName.isEmpty())
        {
            appendCandidate(QDir(sourceDirectory).filePath(baseName));
            appendCandidate(QDir(sourceDirectory).filePath(QStringLiteral("tex/%1").arg(baseName)));
        }
    }

    for (const QString& candidate : candidates)
    {
        QString resolvedCandidate = candidate;
        const QFileInfo candidateInfo(candidate);
        if (candidateInfo.isRelative() && !sourceDirectory.isEmpty())
        {
            resolvedCandidate = QDir(sourceDirectory).filePath(candidate);
        }

        QImage image(resolvedCandidate);
        if (!image.isNull())
        {
            outImage = image;
            downscaleImportedImageIfNeeded(outImage, resolvedCandidate);
            if (outResolvedLabel)
            {
                *outResolvedLabel = resolvedCandidate;
            }
            return true;
        }

        std::ifstream file(resolvedCandidate.toStdString(), std::ios::binary);
        if (!file)
        {
            continue;
        }
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),
                                         std::istreambuf_iterator<char>());
        if (bytes.empty())
        {
            continue;
        }
        if (loadViaStbFromBytes(bytes.data(),
                                static_cast<int>(bytes.size()),
                                resolvedCandidate))
        {
            return true;
        }
    }

    if (texture->content.data && texture->content.size > 0)
    {
        if (outImage.loadFromData(reinterpret_cast<const uchar*>(texture->content.data),
                                  static_cast<int>(texture->content.size)))
        {
            downscaleImportedImageIfNeeded(outImage, QStringLiteral("<embedded>"));
            if (outResolvedLabel)
            {
                *outResolvedLabel = QStringLiteral("<embedded>");
            }
            return true;
        }
        if (loadViaStbFromBytes(reinterpret_cast<const unsigned char*>(texture->content.data),
                                static_cast<int>(texture->content.size),
                                QStringLiteral("<embedded>")))
        {
            return true;
        }
    }

    return false;
}

QString sourceLabelForUfbxTexture(const ufbx_texture* texture)
{
    if (!texture)
    {
        return QString();
    }
    const QString absolute = ufbxStringToQString(texture->absolute_filename);
    if (!absolute.isEmpty())
    {
        return absolute;
    }
    const QString relative = ufbxStringToQString(texture->relative_filename);
    if (!relative.isEmpty())
    {
        return relative;
    }
    const QString filename = ufbxStringToQString(texture->filename);
    if (!filename.isEmpty())
    {
        return filename;
    }
    return texture->content.size > 0 ? QStringLiteral("<embedded>") : QString();
}

bool applyFbxBaseColorTexture(Mesh& targetMesh, const ufbx_mesh* sourceMesh, int materialIndex)
{
    if (targetMesh.primitives.empty() || !sourceMesh)
    {
        return false;
    }

    const ufbx_material* material = materialForFbxMesh(sourceMesh, materialIndex);
    if (!material)
    {
        return false;
    }

    const ufbx_texture* texture = nullptr;
    const ufbx_material_map* sourceMap = nullptr;
    const ufbx_material_map* opacityMap = nullptr;
    bool invertOpacityMap = false;
    if (material->pbr.base_color.texture)
    {
        texture = material->pbr.base_color.texture;
        sourceMap = &material->pbr.base_color;
    }
    else if (material->fbx.diffuse_color.texture)
    {
        texture = material->fbx.diffuse_color.texture;
        sourceMap = &material->fbx.diffuse_color;
    }

    if (!texture || !sourceMap)
    {
        return false;
    }

    if (material->pbr.opacity.texture ||
        (material->pbr.opacity.has_value && !shouldIgnoreScalarOnlyFbxPbrOpacity(material)))
    {
        opacityMap = &material->pbr.opacity;
    }
    else if (material->fbx.transparency_factor.texture || material->fbx.transparency_factor.has_value)
    {
        if (!shouldIgnoreScalarOnlyFbxTransparency(material))
        {
            opacityMap = &material->fbx.transparency_factor;
            invertOpacityMap = true;
        }
    }

    const QString sourceDirectory = QFileInfo(QString::fromStdString(targetMesh.model->name)).absolutePath();
    QImage image;
    QString resolvedTextureLabel;
    if (!loadImageFromUfbxTexture(texture, sourceDirectory, image, &resolvedTextureLabel) || image.isNull())
    {
        return false;
    }

    image = image.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull())
    {
        return false;
    }

    glm::vec4 factor = materialMapColor(*sourceMap);
    if (sourceMap == &material->pbr.base_color && material->pbr.base_factor.has_value)
    {
        const float scalar = static_cast<float>(material->pbr.base_factor.value_real);
        factor.r *= scalar;
        factor.g *= scalar;
        factor.b *= scalar;
    }
    if (sourceMap == &material->fbx.diffuse_color && material->fbx.diffuse_factor.has_value)
    {
        const float scalar = static_cast<float>(material->fbx.diffuse_factor.value_real);
        factor.r *= scalar;
        factor.g *= scalar;
        factor.b *= scalar;
    }
    factor.a = 1.0f;
    factor = glm::clamp(factor, glm::vec4(0.0f), glm::vec4(1.0f));

    float opacityScalar = 1.0f;
    QImage opacityImage;
    QString resolvedOpacityLabel;
    if (opacityMap)
    {
        opacityScalar = std::clamp(materialMapScalar(*opacityMap, 1.0f), 0.0f, 1.0f);
        if (invertOpacityMap)
        {
            opacityScalar = 1.0f - opacityScalar;
        }
        if (opacityMap->texture &&
            loadImageFromUfbxTexture(opacityMap->texture, sourceDirectory, opacityImage, &resolvedOpacityLabel) &&
            !opacityImage.isNull())
        {
            opacityImage = opacityImage.convertToFormat(QImage::Format_RGBA8888);
            if (opacityImage.size() != image.size())
            {
                opacityImage = opacityImage.scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            }
        }
    }

    for (int y = 0; y < image.height(); ++y)
    {
        uchar* scanline = image.scanLine(y);
        const uchar* opacityScanline = opacityImage.isNull() ? nullptr : opacityImage.constScanLine(y);
        for (int x = 0; x < image.width(); ++x)
        {
            uchar* pixel = scanline + (x * 4);
            pixel[0] = static_cast<uchar>(std::clamp((pixel[0] / 255.0f) * factor.r, 0.0f, 1.0f) * 255.0f);
            pixel[1] = static_cast<uchar>(std::clamp((pixel[1] / 255.0f) * factor.g, 0.0f, 1.0f) * 255.0f);
            pixel[2] = static_cast<uchar>(std::clamp((pixel[2] / 255.0f) * factor.b, 0.0f, 1.0f) * 255.0f);

            float alpha = 1.0f;
            if (!opacityImage.isNull())
            {
                alpha = (pixel[3] / 255.0f) * opacityScalar;
            }
            else if (opacityMap)
            {
                alpha = opacityScalar;
            }

            if (opacityScanline)
            {
                const uchar* opacityPixel = opacityScanline + (x * 4);
                float opacitySample = opacityPixel[0] / 255.0f;
                if (invertOpacityMap)
                {
                    opacitySample = 1.0f - opacitySample;
                }
                alpha *= opacitySample;
            }

            pixel[3] = static_cast<uchar>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
        }
    }

    Primitive* primitive = targetMesh.primitives.front().get();
    primitive->sourceMaterialIndex = materialIndex;
    primitive->sourceMaterialName = material && material->name.data
        ? QString::fromUtf8(material->name.data, static_cast<qsizetype>(material->name.length))
        : QString();
    primitive->sourceTextureLabel = !resolvedTextureLabel.isEmpty()
        ? resolvedTextureLabel
        : sourceLabelForUfbxTexture(texture);
    primitive->sourceOpacityScalar = opacityScalar;
    primitive->sourceHasOpacityTexture = !opacityImage.isNull();
    primitive->sourceOpacityInverted = invertOpacityMap;
    if (!resolvedOpacityLabel.isEmpty())
    {
        primitive->sourceOpacityTextureLabel = resolvedOpacityLabel;
    }
    else if (opacityMap && opacityMap->texture)
    {
        primitive->sourceOpacityTextureLabel = sourceLabelForUfbxTexture(opacityMap->texture);
    }
    else
    {
        primitive->sourceOpacityTextureLabel = QString();
    }

    if (primitive->primitiveDescriptorSet == VK_NULL_HANDLE)
    {
        primitive->createTextureFromPixelData(
            image.constBits(),
            static_cast<size_t>(image.sizeInBytes()),
            static_cast<uint32_t>(image.width()),
            static_cast<uint32_t>(image.height()),
            VK_FORMAT_R8G8B8A8_SRGB,
            static_cast<uint32_t>(image.bytesPerLine()));
    }
    else
    {
        primitive->updateTextureFromPixelData(
            image.constBits(),
            static_cast<size_t>(image.sizeInBytes()),
            static_cast<uint32_t>(image.width()),
            static_cast<uint32_t>(image.height()),
            VK_FORMAT_R8G8B8A8_SRGB,
            static_cast<uint32_t>(image.bytesPerLine()));
    }
    primitive->alphaMode = classifyAlphaModeFromImage(image, opacityMap ? opacityScalar : 1.0f, !opacityImage.isNull());
    primitive->alphaCutoff = 0.5f;
    std::cout << "[FBX] Material texture applied: mesh=\""
              << (sourceMesh && sourceMesh->name.data ? std::string(sourceMesh->name.data, sourceMesh->name.length) : std::string("<unnamed>"))
              << "\" size=" << image.width() << "x" << image.height()
              << " baseFactor=(" << factor.r << ", " << factor.g << ", " << factor.b << ", " << factor.a << ")"
              << " opacityScalar=" << opacityScalar
              << " opacityTexture=" << (!opacityImage.isNull() ? "yes" : "no")
              << " opacityTextureLabel=" << primitive->sourceOpacityTextureLabel.toStdString()
              << " invertOpacity=" << (invertOpacityMap ? "yes" : "no")
              << " alphaMode=" << primitiveAlphaModeName(primitive->alphaMode)
              << " cullMode=" << primitiveCullModeName(primitive->cullMode)
              << std::endl;
    return true;
}

void applyFbxMaterialColorFallback(Mesh& targetMesh, const ufbx_mesh* sourceMesh, int materialIndex)
{
    if (targetMesh.primitives.empty())
    {
        return;
    }

    glm::vec4 color(1.0f);
    if (!extractFbxMaterialColor(sourceMesh, color, materialIndex))
    {
        return;
    }

    const uint8_t rgba[4] = {
        static_cast<uint8_t>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f),
        static_cast<uint8_t>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f),
        static_cast<uint8_t>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f),
        static_cast<uint8_t>(std::clamp(color.a, 0.0f, 1.0f) * 255.0f),
    };

    Primitive* primitive = targetMesh.primitives.front().get();
    primitive->sourceMaterialIndex = materialIndex;
    const ufbx_material* material = materialForFbxMesh(sourceMesh, materialIndex);
    primitive->sourceMaterialName = material && material->name.data
        ? QString::fromUtf8(material->name.data, static_cast<qsizetype>(material->name.length))
        : QString();
    primitive->sourceTextureLabel = QString();
    if (primitive->primitiveDescriptorSet == VK_NULL_HANDLE)
    {
        primitive->createTextureFromPixelData(
            rgba,
            sizeof(rgba),
            1,
            1,
            VK_FORMAT_R8G8B8A8_SRGB);
    }
    else
    {
        primitive->updateTextureFromPixelData(
            rgba,
            sizeof(rgba),
            1,
            1,
            VK_FORMAT_R8G8B8A8_SRGB);
    }
    primitive->alphaMode = color.a < 0.999f ? PrimitiveAlphaMode::Blend : PrimitiveAlphaMode::Opaque;
    primitive->alphaCutoff = 0.5f;
    std::cout << "[FBX] Material color fallback: mesh=\""
              << (sourceMesh && sourceMesh->name.data ? std::string(sourceMesh->name.data, sourceMesh->name.length) : std::string("<unnamed>"))
              << "\" color=(" << color.r << ", " << color.g << ", " << color.b << ", " << color.a << ")"
              << " alphaMode=" << primitiveAlphaModeName(primitive->alphaMode)
              << " cullMode=" << primitiveCullModeName(primitive->cullMode)
              << std::endl;
}

std::vector<Vertex> buildVerticesFromFbxMesh(const ufbx_mesh* mesh, std::vector<uint32_t>* outVertexIndices)
{
    std::vector<Vertex> vertices;
    if (!mesh || !mesh->vertex_position.exists)
    {
        return vertices;
    }

    std::vector<uint32_t> triIndices(mesh->max_face_triangles * 3);
    vertices.reserve(mesh->num_triangles * 3);
    if (outVertexIndices)
    {
        outVertexIndices->clear();
        outVertexIndices->reserve(mesh->num_triangles * 3);
    }

    for (size_t faceIndex = 0; faceIndex < mesh->faces.count; ++faceIndex)
    {
        const ufbx_face face = mesh->faces.data[faceIndex];
        const uint32_t numTriangles = ufbx_triangulate_face(triIndices.data(), triIndices.size(), mesh, face);
        for (uint32_t tri = 0; tri < numTriangles * 3; ++tri)
        {
            const size_t index = triIndices[tri];
            Vertex vertex{};
            vertex.pos = toGlmVec3(ufbx_get_vertex_vec3(&mesh->vertex_position, index));
            vertex.normal = mesh->vertex_normal.exists
                ? toGlmVec3(ufbx_get_vertex_vec3(&mesh->vertex_normal, index))
                : glm::vec3(0.0f, 1.0f, 0.0f);
            vertex.texCoord = mesh->vertex_uv.exists
                ? toGlmVec2(ufbx_get_vertex_vec2(&mesh->vertex_uv, index))
                : glm::vec2(0.0f);
            vertices.push_back(vertex);
            if (outVertexIndices)
            {
                outVertexIndices->push_back(static_cast<uint32_t>(index));
            }
        }
    }

    return vertices;
}

std::vector<Vertex> buildVerticesFromFbxMeshPart(const ufbx_mesh* mesh,
                                                 const ufbx_mesh_part& part,
                                                 std::vector<uint32_t>* outVertexIndices)
{
    std::vector<Vertex> vertices;
    if (!mesh || !mesh->vertex_position.exists || part.face_indices.count == 0 || !part.face_indices.data)
    {
        return vertices;
    }

    std::vector<uint32_t> triIndices(mesh->max_face_triangles * 3);
    vertices.reserve(part.num_triangles * 3);
    if (outVertexIndices)
    {
        outVertexIndices->clear();
        outVertexIndices->reserve(part.num_triangles * 3);
    }

    for (size_t partFaceIndex = 0; partFaceIndex < part.face_indices.count; ++partFaceIndex)
    {
        const uint32_t faceIndex = part.face_indices.data[partFaceIndex];
        if (faceIndex >= mesh->faces.count)
        {
            continue;
        }

        const ufbx_face face = mesh->faces.data[faceIndex];
        const uint32_t numTriangles = ufbx_triangulate_face(triIndices.data(), triIndices.size(), mesh, face);
        for (uint32_t tri = 0; tri < numTriangles * 3; ++tri)
        {
            const size_t index = triIndices[tri];
            Vertex vertex{};
            vertex.pos = toGlmVec3(ufbx_get_vertex_vec3(&mesh->vertex_position, index));
            vertex.normal = mesh->vertex_normal.exists
                ? toGlmVec3(ufbx_get_vertex_vec3(&mesh->vertex_normal, index))
                : glm::vec3(0.0f, 1.0f, 0.0f);
            vertex.texCoord = mesh->vertex_uv.exists
                ? toGlmVec2(ufbx_get_vertex_vec2(&mesh->vertex_uv, index))
                : glm::vec2(0.0f);
            vertices.push_back(vertex);
            if (outVertexIndices)
            {
                outVertexIndices->push_back(static_cast<uint32_t>(index));
            }
        }
    }

    return vertices;
}

void getFbxSkinInfluences(const ufbx_mesh* mesh,
                          size_t index,
                          glm::uvec4& outJointIndices,
                          glm::vec4& outJointWeights,
                          uint32_t* outSkinDeformerIndex)
{
    outJointIndices = glm::uvec4(0u);
    outJointWeights = glm::vec4(0.0f);
    if (outSkinDeformerIndex)
    {
        *outSkinDeformerIndex = 0;
    }

    if (!mesh || mesh->skin_deformers.count == 0 || !mesh->vertex_indices.data || index >= mesh->vertex_indices.count)
    {
        return;
    }

    const uint32_t logicalVertex = mesh->vertex_indices.data[index];
    const ufbx_skin_deformer* skin = mesh->skin_deformers.data[0];
    if (!skin || logicalVertex >= skin->vertices.count)
    {
        return;
    }

    if (outSkinDeformerIndex)
    {
        *outSkinDeformerIndex = 0;
    }

    const ufbx_skin_vertex skinVertex = skin->vertices.data[logicalVertex];
    float weightSum = 0.0f;
    uint32_t writeIndex = 0;
    for (uint32_t i = 0; i < skinVertex.num_weights && writeIndex < 4; ++i)
    {
        const uint32_t weightIndex = skinVertex.weight_begin + i;
        if (weightIndex >= skin->weights.count)
        {
            break;
        }
        const ufbx_skin_weight skinWeight = skin->weights.data[weightIndex];
        const float weightValue = std::max(0.0f, static_cast<float>(skinWeight.weight));
        if (weightValue <= 0.0f)
        {
            continue;
        }
        outJointIndices[writeIndex] = skinWeight.cluster_index;
        outJointWeights[writeIndex] = weightValue;
        weightSum += weightValue;
        ++writeIndex;
    }

    if (weightSum > 0.0f)
    {
        outJointWeights /= weightSum;
    }
}

int materialIndexForFbxMeshPart(const ufbx_mesh* mesh, const ufbx_mesh_part& part)
{
    if (!mesh || part.face_indices.count == 0 || !part.face_indices.data ||
        !mesh->face_material.data || mesh->face_material.count != mesh->faces.count)
    {
        return 0;
    }

    const uint32_t firstFaceIndex = part.face_indices.data[0];
    if (firstFaceIndex >= mesh->face_material.count)
    {
        return 0;
    }

    return static_cast<int>(mesh->face_material.data[firstFaceIndex]);
}
