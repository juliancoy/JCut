#pragma once

#include <vector>

#include <QImage>
#include <QString>
#include <glm/glm.hpp>

#include "model.h"
#include "ufbx.h"

const char* primitiveAlphaModeName(PrimitiveAlphaMode mode);
const char* primitiveCullModeName(PrimitiveCullMode mode);

PrimitiveAlphaMode classifyAlphaModeFromImage(const QImage& image,
                                              float uniformAlpha,
                                              bool hasOpacityImage);

bool extractFbxMaterialColor(const ufbx_mesh* mesh, glm::vec4& outColor, int materialIndex = 0);

bool loadImageFromUfbxTexture(const ufbx_texture* texture,
                              const QString& sourceDirectory,
                              QImage& outImage,
                              QString* outResolvedLabel = nullptr);

QString sourceLabelForUfbxTexture(const ufbx_texture* texture);

bool applyFbxBaseColorTexture(Mesh& targetMesh, const ufbx_mesh* sourceMesh, int materialIndex = 0);
void applyFbxMaterialColorFallback(Mesh& targetMesh, const ufbx_mesh* sourceMesh, int materialIndex = 0);

std::vector<Vertex> buildVerticesFromFbxMesh(const ufbx_mesh* mesh, std::vector<uint32_t>* outVertexIndices = nullptr);
std::vector<Vertex> buildVerticesFromFbxMeshPart(const ufbx_mesh* mesh,
                                                 const ufbx_mesh_part& part,
                                                 std::vector<uint32_t>* outVertexIndices = nullptr);

void getFbxSkinInfluences(const ufbx_mesh* mesh,
                          size_t index,
                          glm::uvec4& outJointIndices,
                          glm::vec4& outJointWeights,
                          uint32_t* outSkinDeformerIndex = nullptr);

int materialIndexForFbxMeshPart(const ufbx_mesh* mesh, const ufbx_mesh_part& part);
