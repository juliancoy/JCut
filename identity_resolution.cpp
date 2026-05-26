#include "identity_resolution.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QThread>

#include <cmath>

#if JCUT_HAVE_NCNN
#include <net.h>
#endif

namespace jcut::identity_resolution {

namespace {
bool l2Normalize(std::vector<float>* values)
{
    if (!values || values->empty()) {
        return false;
    }
    double sumSq = 0.0;
    for (float v : *values) {
        sumSq += static_cast<double>(v) * static_cast<double>(v);
    }
    if (sumSq <= 0.0) {
        return false;
    }
    const float inv = static_cast<float>(1.0 / std::sqrt(sumSq));
    for (float& v : *values) {
        v *= inv;
    }
    return true;
}
} // namespace

QString findArcFaceModelFile(const QString& fileName)
{
    const QStringList roots{
        QDir::currentPath(),
        QCoreApplication::applicationDirPath(),
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral(".."))
    };
    for (const QString& root : roots) {
        const QString candidate = QDir(root).absoluteFilePath(QStringLiteral("assets/models/%1").arg(fileName));
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return QDir::current().absoluteFilePath(QStringLiteral("assets/models/%1").arg(fileName));
}

double cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size() || a.empty()) {
        return -1.0;
    }
    double dot = 0.0;
    double na = 0.0;
    double nb = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double av = static_cast<double>(a[i]);
        const double bv = static_cast<double>(b[i]);
        dot += av * bv;
        na += av * av;
        nb += bv * bv;
    }
    if (na <= 0.0 || nb <= 0.0) {
        return -1.0;
    }
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

bool averageEmbeddings(const QVector<std::vector<float>>& vectors, std::vector<float>* out)
{
    if (!out || vectors.isEmpty() || vectors.first().empty()) {
        return false;
    }
    const size_t dim = vectors.first().size();
    std::vector<float> sum(dim, 0.0f);
    int used = 0;
    for (const std::vector<float>& v : vectors) {
        if (v.size() != dim) {
            continue;
        }
        for (size_t i = 0; i < dim; ++i) {
            sum[i] += v[i];
        }
        ++used;
    }
    if (used <= 0) {
        return false;
    }
    const float inv = 1.0f / static_cast<float>(used);
    for (float& x : sum) {
        x *= inv;
    }
    if (!l2Normalize(&sum)) {
        return false;
    }
    *out = std::move(sum);
    return true;
}

struct ArcFaceNcnnEmbedder::Impl {
#if JCUT_HAVE_NCNN
    ncnn::Net net;
#endif
};

ArcFaceNcnnEmbedder::ArcFaceNcnnEmbedder()
    : m_impl(std::make_unique<Impl>())
{
}

ArcFaceNcnnEmbedder::~ArcFaceNcnnEmbedder() = default;

bool ArcFaceNcnnEmbedder::initialize(const QString& paramPath, const QString& binPath, QString* error)
{
#if !JCUT_HAVE_NCNN
    Q_UNUSED(paramPath)
    Q_UNUSED(binPath)
    if (error) {
        *error = QStringLiteral("Build missing NCNN support for ArcFace embedding.");
    }
    return false;
#else
    if (!QFileInfo::exists(paramPath) || !QFileInfo::exists(binPath)) {
        if (error) {
            *error = QStringLiteral("Missing ArcFace model files: %1 and %2").arg(paramPath, binPath);
        }
        return false;
    }
    m_impl->net.opt.use_vulkan_compute = false;
    m_impl->net.opt.num_threads = qMax(1, QThread::idealThreadCount() / 2);
    if (m_impl->net.load_param(paramPath.toLocal8Bit().constData()) != 0) {
        if (error) {
            *error = QStringLiteral("Failed to load ArcFace param: %1").arg(paramPath);
        }
        return false;
    }
    if (m_impl->net.load_model(binPath.toLocal8Bit().constData()) != 0) {
        if (error) {
            *error = QStringLiteral("Failed to load ArcFace bin: %1").arg(binPath);
        }
        return false;
    }
    return true;
#endif
}

bool ArcFaceNcnnEmbedder::embedFaceCrop(const QString& imagePath, std::vector<float>* outEmbedding) const
{
#if !JCUT_HAVE_NCNN
    Q_UNUSED(imagePath)
    Q_UNUSED(outEmbedding)
    return false;
#else
    if (!outEmbedding) {
        return false;
    }
    const QImage src(imagePath);
    if (src.isNull()) {
        return false;
    }
    const QImage rgb = src.convertToFormat(QImage::Format_RGB888);
    if (rgb.isNull()) {
        return false;
    }
    ncnn::Mat input = ncnn::Mat::from_pixels_resize(
        rgb.constBits(), ncnn::Mat::PIXEL_RGB, rgb.width(), rgb.height(), 112, 112);
    // The bundled NCNN param has preprocessing layers at the graph input:
    // subtract 127.5, then multiply by 1/128. Applying it here too collapses identity scores.

    ncnn::Extractor ex = m_impl->net.create_extractor();
    ex.set_light_mode(true);
    if (ex.input("data", input) != 0) {
        return false;
    }
    ncnn::Mat feat;
    if (ex.extract("fc1", feat) != 0) {
        if (ex.extract("output", feat) != 0) {
            return false;
        }
    }
    if (feat.empty() || feat.w <= 0) {
        return false;
    }
    outEmbedding->assign(reinterpret_cast<const float*>(feat.data),
                         reinterpret_cast<const float*>(feat.data) + feat.w);
    return l2Normalize(outEmbedding);
#endif
}

} // namespace jcut::identity_resolution
