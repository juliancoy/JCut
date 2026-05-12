#pragma once

#include <QVector>
#include <QString>

#include <memory>
#include <vector>

namespace jcut::identity_resolution {

QString findArcFaceModelFile(const QString& fileName);
double cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b);
bool averageEmbeddings(const QVector<std::vector<float>>& vectors, std::vector<float>* out);

class ArcFaceNcnnEmbedder {
public:
    ArcFaceNcnnEmbedder();
    ~ArcFaceNcnnEmbedder();
    ArcFaceNcnnEmbedder(const ArcFaceNcnnEmbedder&) = delete;
    ArcFaceNcnnEmbedder& operator=(const ArcFaceNcnnEmbedder&) = delete;

    bool initialize(const QString& paramPath, const QString& binPath, QString* error);
    bool embedFaceCrop(const QString& imagePath, std::vector<float>* outEmbedding) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace jcut::identity_resolution
