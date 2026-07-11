#include "vulkan_text_renderer.h"

#include "preview_view_transform.h"
#include "titles.h"
#include "transcript_overlay_cache_key.h"
#include "vulkan_clear_helpers.h"

#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QMatrix4x4>
#include <QMutex>
#include <QMutexLocker>
#include <QVulkanFunctions>
#include <QVector3D>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

QByteArray erodeGlyphAlpha(const FT_Bitmap& bitmap, int radius);

namespace {

using jcut::vulkan::clearRect;
using jcut::vulkan::clearRectFromQRect;

constexpr int kAtlasSize = 2048;
constexpr int kGlyphPadding = 2;
constexpr uint32_t kTextPushSize = sizeof(VulkanTextPipeline::Push);

struct FreeTypeLibrary {
    FT_Library library = nullptr;
    FreeTypeLibrary() { FT_Init_FreeType(&library); }
    ~FreeTypeLibrary()
    {
        if (library) {
            FT_Done_FreeType(library);
        }
    }
};

FreeTypeLibrary& ftLibraryHolder()
{
    static FreeTypeLibrary holder;
    return holder;
}

struct ResolvedFontFace {
    QString path;
    int faceIndex = 0;
};

QString normalizedFontFamily(const QString& family)
{
    const QString trimmed = family.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("DejaVu Sans") : trimmed;
}

ResolvedFontFace resolveFontFaceUncached(const QString& family, bool bold)
{
    FcPattern* pattern = FcPatternCreate();
    if (!pattern) {
        return {};
    }
    const QByteArray familyUtf8 = normalizedFontFamily(family).toUtf8();
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(familyUtf8.constData()));
    FcPatternAddInteger(pattern, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
    FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pattern, &result);
    FcPatternDestroy(pattern);
    if (!match) {
        return {};
    }
    FcChar8* file = nullptr;
    ResolvedFontFace resolved;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
        resolved.path = QString::fromUtf8(reinterpret_cast<const char*>(file));
    }
    int faceIndex = 0;
    if (FcPatternGetInteger(match, FC_INDEX, 0, &faceIndex) == FcResultMatch) {
        resolved.faceIndex = qMax(0, faceIndex);
    }
    FcPatternDestroy(match);
    return resolved;
}

ResolvedFontFace cachedResolvedFontFace(const QString& family, bool bold)
{
    static QMutex mutex;
    static QHash<QString, ResolvedFontFace> cache;
    const QString key = normalizedFontFamily(family) + QLatin1Char('|') +
        (bold ? QLatin1String("bold") : QLatin1String("regular"));
    {
        QMutexLocker locker(&mutex);
        const auto it = cache.constFind(key);
        if (it != cache.constEnd()) {
            return it.value();
        }
    }

    const ResolvedFontFace resolved = resolveFontFaceUncached(family, bold);
    {
        QMutexLocker locker(&mutex);
        cache.insert(key, resolved);
    }
    return resolved;
}

struct FaceGuard {
    FT_Face face = nullptr;
    ~FaceGuard()
    {
        if (face) {
            FT_Done_Face(face);
        }
    }
};

bool loadFace(const QString& family, bool bold, int pixelSize, FaceGuard* guard)
{
    if (!guard || !ftLibraryHolder().library || pixelSize <= 0) {
        return false;
    }
    const ResolvedFontFace resolved = cachedResolvedFontFace(family, bold);
    if (resolved.path.isEmpty()) {
        return false;
    }
    const QByteArray pathUtf8 = resolved.path.toUtf8();
    if (FT_New_Face(ftLibraryHolder().library, pathUtf8.constData(), resolved.faceIndex, &guard->face) != 0 ||
        !guard->face) {
        return false;
    }
    if (FT_Select_Charmap(guard->face, FT_ENCODING_UNICODE) != 0 ||
        FT_Set_Pixel_Sizes(guard->face, 0, pixelSize) != 0) {
        return false;
    }
    return true;
}

QString rectKey(const QRectF& rect)
{
    return QString::number(rect.x(), 'f', 3) + QLatin1Char(',') +
        QString::number(rect.y(), 'f', 3) + QLatin1Char(',') +
        QString::number(rect.width(), 'f', 3) + QLatin1Char(',') +
        QString::number(rect.height(), 'f', 3);
}

QString speakerLabelLayoutKey(const QSize& outputSize, const render_detail::SpeakerLabelOverlaySpec& spec)
{
    const QString material =
        QStringLiteral("speaker-label-layout-v1|") +
        QString::number(outputSize.width()) + QLatin1Char('x') + QString::number(outputSize.height()) + QLatin1Char('|') +
        spec.name + QLatin1Char('|') +
        spec.organization + QLatin1Char('|') +
        QString::number(spec.showName ? 1 : 0) + QLatin1Char('|') +
        QString::number(spec.showOrganization ? 1 : 0) + QLatin1Char('|') +
        QString::number(spec.nameTextScale, 'f', 4) + QLatin1Char('|') +
        QString::number(spec.organizationTextScale, 'f', 4) + QLatin1Char('|') +
        QString::number(spec.nameVerticalPosition, 'f', 4) + QLatin1Char('|') +
        QString::number(spec.organizationVerticalPosition, 'f', 4) + QLatin1Char('|') +
        spec.fontFamily + QLatin1Char('|') +
        QString::number(static_cast<quint32>(spec.nameColor.rgba())) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(spec.organizationColor.rgba())) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(spec.backgroundColor.rgba())) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(spec.borderColor.rgba())) + QLatin1Char('|') +
        QString::number(spec.backgroundCornerRadius, 'f', 2) + QLatin1Char('|') +
        QString::number(spec.borderWidth, 'f', 2) + QLatin1Char('|') +
        QString::number(spec.showShadow ? 1 : 0) + QLatin1Char('|') +
        QString::number(static_cast<int>(spec.textExtrudeMode)) + QLatin1Char('|') +
        QString::number(spec.textExtrudeDepth, 'f', 3) + QLatin1Char('|') +
        QString::number(spec.textExtrudeBevelScale, 'f', 3) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(spec.shadowColor.rgba()));
    return QString::fromLatin1(QCryptographicHash::hash(material.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QString transcriptOverlayLayoutKey(const QSize& outputSize,
                                   const TimelineClip& clip,
                                   const TranscriptOverlayLayout& layout,
                                   const QRectF& outputRect,
                                   const QString& speakerTitle)
{
    QString layoutMaterial;
    for (const TranscriptOverlayLine& line : layout.lines) {
        layoutMaterial += line.words.join(QLatin1Char(' '));
        layoutMaterial += QLatin1Char('#');
        layoutMaterial += QString::number(line.activeWord);
        layoutMaterial += QLatin1Char('|');
    }
    const QString material =
        QStringLiteral("transcript-layout-v2|") +
        clip.id + QLatin1Char('|') +
        QString::number(outputSize.width()) + QLatin1Char('x') + QString::number(outputSize.height()) + QLatin1Char('|') +
        rectKey(outputRect) + QLatin1Char('|') +
        speakerTitle + QLatin1Char('|') +
        QString::number(static_cast<int>(clip.transcriptOverlay.textExtrudeMode)) + QLatin1Char('|') +
        QString::number(clip.transcriptOverlay.textExtrudeDepth, 'f', 3) + QLatin1Char('|') +
        QString::number(clip.transcriptOverlay.textExtrudeBevelScale, 'f', 3) + QLatin1Char('|') +
        transcriptOverlayStyleCacheMaterial(clip) + QLatin1Char('|') +
        layoutMaterial;
    return QString::fromLatin1(QCryptographicHash::hash(material.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QString transcriptOverlayAtlasKey(const TimelineClip& clip,
                                  const TranscriptOverlayLayout& layout,
                                  const QString& speakerTitle)
{
    QString glyphMaterial;
    for (const TranscriptOverlayLine& line : layout.lines) {
        glyphMaterial += line.words.join(QLatin1Char(' '));
        glyphMaterial += QLatin1Char('|');
    }
    const auto& overlay = clip.transcriptOverlay;
    const int bodyPixelSize = qMax(1, static_cast<int>(std::round(overlay.fontPointSize)));
    const int titlePixelSize = qMax(1, static_cast<int>(std::round(overlay.fontPointSize * 0.62)));
    const QString material =
        QStringLiteral("transcript-atlas-v1|") +
        overlay.fontFamily + QLatin1Char('|') +
        QString::number(bodyPixelSize) + QLatin1Char('|') +
        QString::number(titlePixelSize) + QLatin1Char('|') +
        QString::number(overlay.bold ? 1 : 0) + QLatin1Char('|') +
        QString::number(static_cast<int>(overlay.textExtrudeMode)) + QLatin1Char('|') +
        QString::number(overlay.textExtrudeBevelScale, 'f', 3) + QLatin1Char('|') +
        speakerTitle.trimmed() + QLatin1Char('|') +
        glyphMaterial;
    return QString::fromLatin1(QCryptographicHash::hash(material.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QString glyphKey(uint codepoint, bool bold, int pixelSize)
{
    return QString::number(codepoint) + QLatin1Char('|') +
        (bold ? QLatin1String("b") : QLatin1String("r")) + QLatin1Char('|') +
        QString::number(pixelSize);
}

qreal measureText(FT_Face face, const QString& text)
{
    if (!face || text.isEmpty()) {
        return 0.0;
    }
    qreal width = 0.0;
    FT_UInt previous = 0;
    for (uint codepoint : text.toUcs4()) {
        const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
        if (FT_HAS_KERNING(face) && previous && glyphIndex) {
            FT_Vector delta{};
            FT_Get_Kerning(face, previous, glyphIndex, FT_KERNING_DEFAULT, &delta);
            width += static_cast<qreal>(delta.x) / 64.0;
        }
        if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_DEFAULT) == 0) {
            width += static_cast<qreal>(face->glyph->advance.x) / 64.0;
        }
        previous = glyphIndex;
    }
    return width;
}

QStringList wrapText(FT_Face face, const QString& text, qreal maxWidth)
{
    const QString normalized = text.simplified();
    if (normalized.isEmpty()) {
        return {};
    }
    const QStringList words = normalized.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList lines;
    QString current;
    for (const QString& word : words) {
        const QString candidate = current.isEmpty() ? word : current + QLatin1Char(' ') + word;
        if (!current.isEmpty() && measureText(face, candidate) > maxWidth) {
            lines.push_back(current);
            current = word;
        } else {
            current = candidate;
        }
    }
    if (!current.isEmpty()) {
        lines.push_back(current);
    }
    return lines;
}

void blendAtlasPixel(render_detail::OverlayImage* image, int x, int y, unsigned char alpha)
{
    if (!image || x < 0 || y < 0 || x >= image->width || y >= image->height) {
        return;
    }
    const int index = ((y * image->width) + x) * 4;
    image->rgbaPremultiplied[index + 0] = static_cast<char>(255);
    image->rgbaPremultiplied[index + 1] = static_cast<char>(255);
    image->rgbaPremultiplied[index + 2] = static_cast<char>(255);
    image->rgbaPremultiplied[index + 3] = static_cast<char>(
        std::max<int>(static_cast<unsigned char>(image->rgbaPremultiplied[index + 3]), alpha));
}

QImage defaultPatternTile(const QColor& base, int size)
{
    QImage image(size, size, QImage::Format_RGBA8888);
    const QColor hi = base.isValid() ? base.lighter(145) : QColor(Qt::white);
    const QColor lo = base.isValid() ? base.darker(165) : QColor(80, 160, 220);
    for (int y = 0; y < size; ++y) {
        auto* row = image.scanLine(y);
        for (int x = 0; x < size; ++x) {
            const bool stripe = ((x + y) / qMax(2, size / 10)) % 2 == 0;
            const bool dot = ((x / qMax(2, size / 8)) + (y / qMax(2, size / 8))) % 3 == 0;
            QColor c = stripe ? hi : lo;
            if (dot) {
                c = c.lighter(128);
            }
            row[x * 4 + 0] = static_cast<uchar>(c.red());
            row[x * 4 + 1] = static_cast<uchar>(c.green());
            row[x * 4 + 2] = static_cast<uchar>(c.blue());
            row[x * 4 + 3] = 255;
        }
    }
    return image;
}

QRectF packPatternTile(render_detail::OverlayImage* atlas,
                       const QString& imagePath,
                       const QColor& fallbackColor,
                       int slot)
{
    if (!atlas || atlas->width <= 0 || atlas->height <= 0 ||
        atlas->rgbaPremultiplied.size() < atlas->width * atlas->height * 4) {
        return QRectF(0.0, 0.0, 1.0 / kAtlasSize, 1.0 / kAtlasSize);
    }
    constexpr int kPatternSize = 192;
    constexpr int kPatternGap = 12;
    const int x0 = atlas->width - kPatternSize - kPatternGap;
    const int y0 = atlas->height - ((slot + 1) * (kPatternSize + kPatternGap));
    if (x0 < 0 || y0 < 0) {
        return QRectF(0.0, 0.0, 1.0 / kAtlasSize, 1.0 / kAtlasSize);
    }
    QImage source;
    if (!imagePath.trimmed().isEmpty()) {
        source.load(imagePath.trimmed());
    }
    if (source.isNull()) {
        source = defaultPatternTile(fallbackColor, kPatternSize);
    }
    QImage tile = source.convertToFormat(QImage::Format_RGBA8888)
        .scaled(kPatternSize, kPatternSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    for (int y = 0; y < tile.height(); ++y) {
        const uchar* src = tile.constScanLine(y);
        for (int x = 0; x < tile.width(); ++x) {
            const int dstIndex = (((y0 + y) * atlas->width) + (x0 + x)) * 4;
            atlas->rgbaPremultiplied[dstIndex + 0] = static_cast<char>(src[x * 4 + 0]);
            atlas->rgbaPremultiplied[dstIndex + 1] = static_cast<char>(src[x * 4 + 1]);
            atlas->rgbaPremultiplied[dstIndex + 2] = static_cast<char>(src[x * 4 + 2]);
            atlas->rgbaPremultiplied[dstIndex + 3] = static_cast<char>(src[x * 4 + 3]);
        }
    }
    return QRectF(static_cast<qreal>(x0) / atlas->width,
                  static_cast<qreal>(y0) / atlas->height,
                  static_cast<qreal>(kPatternSize) / atlas->width,
                  static_cast<qreal>(kPatternSize) / atlas->height);
}

QRectF packLogoTile(render_detail::OverlayImage* atlas, const QString& imagePath)
{
    if (!atlas || imagePath.trimmed().isEmpty() || atlas->width <= 0 || atlas->height <= 0 ||
        atlas->rgbaPremultiplied.size() < atlas->width * atlas->height * 4) {
        return {};
    }
    constexpr int kLogoSize = 160;
    constexpr int kPatternSize = 192;
    constexpr int kPatternGap = 12;
    const int x0 = atlas->width - kLogoSize - kPatternGap;
    const int y0 = atlas->height - (2 * (kPatternSize + kPatternGap)) - kLogoSize - kPatternGap;
    if (x0 < 0 || y0 < 0) {
        return {};
    }
    QImage source(imagePath.trimmed());
    if (source.isNull()) {
        return {};
    }
    QImage tile = source.convertToFormat(QImage::Format_RGBA8888)
        .scaled(kLogoSize, kLogoSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const int dx = x0 + ((kLogoSize - tile.width()) / 2);
    const int dy = y0 + ((kLogoSize - tile.height()) / 2);
    for (int y = 0; y < tile.height(); ++y) {
        const uchar* src = tile.constScanLine(y);
        for (int x = 0; x < tile.width(); ++x) {
            const int dstIndex = (((dy + y) * atlas->width) + (dx + x)) * 4;
            atlas->rgbaPremultiplied[dstIndex + 0] = static_cast<char>(src[x * 4 + 0]);
            atlas->rgbaPremultiplied[dstIndex + 1] = static_cast<char>(src[x * 4 + 1]);
            atlas->rgbaPremultiplied[dstIndex + 2] = static_cast<char>(src[x * 4 + 2]);
            atlas->rgbaPremultiplied[dstIndex + 3] = static_cast<char>(src[x * 4 + 3]);
        }
    }
    return QRectF(static_cast<qreal>(x0) / atlas->width,
                  static_cast<qreal>(y0) / atlas->height,
                  static_cast<qreal>(kLogoSize) / atlas->width,
                  static_cast<qreal>(kLogoSize) / atlas->height);
}

VkClearValue clearValueForColor(const QColor& color)
{
    VkClearValue value{};
    value.color.float32[0] = static_cast<float>(color.redF() * color.alphaF());
    value.color.float32[1] = static_cast<float>(color.greenF() * color.alphaF());
    value.color.float32[2] = static_cast<float>(color.blueF() * color.alphaF());
    value.color.float32[3] = static_cast<float>(color.alphaF());
    return value;
}

QColor colorWithOpacity(QColor color, qreal opacity)
{
    if (!color.isValid()) {
        color = QColor(Qt::black);
    }
    color.setAlphaF(qBound<qreal>(0.0, opacity, 1.0));
    return color;
}

int materialStyleId(TitleMaterialStyle style)
{
    switch (style) {
    case TitleMaterialStyle::Neon:
        return 1;
    case TitleMaterialStyle::DiagonalStripes:
        return 2;
    case TitleMaterialStyle::Grid:
        return 3;
    case TitleMaterialStyle::ImagePattern:
        return 4;
    case TitleMaterialStyle::Solid:
    default:
        return 0;
    }
}

QVector<qreal> textExtrusionLayerOffsets(TextExtrudeMode mode, qreal depth)
{
    QVector<qreal> offsets;
    if (mode == TextExtrudeMode::None || depth <= 0.001) return offsets;
    const bool stacked = mode == TextExtrudeMode::StackedCopies;
    const int sparseLayers = qBound(1, static_cast<int>(std::ceil(depth * 24.0)), 12);
    const qreal totalDepth = stacked
        ? qBound<qreal>(0.6, depth * 8.0, 4.5) * sparseLayers
        : qBound<qreal>(1.0, depth * 18.0, 36.0);
    const int layers = stacked
        ? sparseLayers
        : qBound(2, static_cast<int>(std::ceil(totalDepth / 0.65)), 64);
    offsets.reserve(layers);
    for (int layer = layers; layer >= 1; --layer) {
        offsets.push_back(totalDepth * static_cast<qreal>(layer) / layers);
    }
    return offsets;
}

QVector<QPointF> dilationOffsets(qreal radius)
{
    QVector<QPointF> offsets;
    const int rings = qBound(0, static_cast<int>(std::ceil(radius)), 24);
    if (rings <= 0) {
        return offsets;
    }

    constexpr qreal kPi = 3.14159265358979323846;
    constexpr int kSamplesPerRing = 16;
    offsets.reserve(rings * kSamplesPerRing);
    auto containsOffset = [&offsets](const QPointF& candidate) {
        for (const QPointF& offset : offsets) {
            if (qFuzzyCompare(offset.x() + 1.0, candidate.x() + 1.0) &&
                qFuzzyCompare(offset.y() + 1.0, candidate.y() + 1.0)) {
                return true;
            }
        }
        return false;
    };

    for (int ring = 1; ring <= rings; ++ring) {
        const qreal ringRadius = qMin<qreal>(radius, ring);
        for (int sample = 0; sample < kSamplesPerRing; ++sample) {
            const qreal angle = (2.0 * kPi * sample) / kSamplesPerRing;
            QPointF offset(std::round(std::cos(angle) * ringRadius),
                           std::round(std::sin(angle) * ringRadius));
            if ((qFuzzyIsNull(offset.x()) && qFuzzyIsNull(offset.y())) ||
                containsOffset(offset)) {
                continue;
            }
            offsets.push_back(offset);
        }
    }
    return offsets;
}

void mvpForScreenRect(const QRectF& rect, const QSize& swapSize, float outMvp[16])
{
    const float fullW = static_cast<float>(std::max(1, swapSize.width()));
    const float fullH = static_cast<float>(std::max(1, swapSize.height()));
    const float halfW = static_cast<float>(std::max<qreal>(1.0, rect.width())) * 0.5f;
    const float halfH = static_cast<float>(std::max<qreal>(1.0, rect.height())) * 0.5f;
    const float cx = static_cast<float>(rect.center().x());
    const float cy = static_cast<float>(rect.center().y());
    const float m[16] = {
        (2.0f * halfW) / fullW, 0.f, 0.f, 0.f,
        0.f, (2.0f * halfH) / fullH, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        (2.0f * cx / fullW) - 1.0f,
        (2.0f * cy / fullH) - 1.0f,
        0.f,
        1.f
    };
    std::copy(std::begin(m), std::end(m), outMvp);
}

void mvpForTitle3DRect(const QRectF& rect,
                       const QPointF& titleCenter,
                       const QSize& swapSize,
                       const EvaluatedTitle& title,
                       float outMvp[16])
{
    const qreal w = qMax<qreal>(1.0, swapSize.width());
    const qreal h = qMax<qreal>(1.0, swapSize.height());
    const qreal aspect = w / h;
    const float cameraDistance = 5.2f;
    const float fovDegrees = 43.0f;
    const qreal halfViewH = std::tan((fovDegrees * M_PI / 180.0) * 0.5) * cameraDistance;
    const qreal halfViewW = halfViewH * aspect;
    auto screenToWorld = [&](const QPointF& point) {
        const qreal ndcX = (2.0 * point.x() / w) - 1.0;
        const qreal ndcY = (2.0 * point.y() / h) - 1.0;
        return QVector3D(static_cast<float>(ndcX * halfViewW),
                         static_cast<float>(ndcY * halfViewH),
                         0.0f);
    };
    const QVector3D centerWorld = screenToWorld(titleCenter);
    const QPointF localCenter = rect.center() - titleCenter;
    const QVector3D localWorld(static_cast<float>((localCenter.x() / w) * 2.0 * halfViewW),
                               static_cast<float>((localCenter.y() / h) * 2.0 * halfViewH),
                               0.0f);
    const qreal titleScale = qBound<qreal>(0.05, title.vulkan3DScale, 4.0);
    const QVector3D halfSize(static_cast<float>((rect.width() / w) * halfViewW * titleScale),
                             static_cast<float>((rect.height() / h) * halfViewH * titleScale),
                             1.0f);

    QMatrix4x4 projection;
    projection.perspective(fovDegrees, static_cast<float>(aspect), 0.1f, 32.0f);
    QMatrix4x4 view;
    view.lookAt(QVector3D(0.0f, 0.0f, cameraDistance),
                QVector3D(0.0f, 0.0f, 0.0f),
                QVector3D(0.0f, 1.0f, 0.0f));
    QMatrix4x4 model;
    model.translate(centerWorld);
    model.rotate(static_cast<float>(title.vulkan3DYawDegrees), 0.0f, 1.0f, 0.0f);
    model.rotate(static_cast<float>(title.vulkan3DPitchDegrees), 1.0f, 0.0f, 0.0f);
    model.rotate(static_cast<float>(title.vulkan3DRollDegrees), 0.0f, 0.0f, 1.0f);
    model.translate(localWorld.x(),
                    localWorld.y(),
                    static_cast<float>(qBound<qreal>(-3.0, title.vulkan3DDepth, 3.0)));
    model.scale(halfSize);

    const QMatrix4x4 mvp = projection * view * model;
    std::copy(mvp.constData(), mvp.constData() + 16, outMvp);
}

void mvpForTitleMesh(const QPointF& center, qreal pixelHeight, const QSize& swapSize,
                     const EvaluatedTitle& title, float outMvp[16])
{
    const qreal w = qMax<qreal>(1.0, swapSize.width());
    const qreal h = qMax<qreal>(1.0, swapSize.height());
    const qreal aspect = w / h;
    constexpr float cameraDistance = 5.2f;
    constexpr float fovDegrees = 43.0f;
    const qreal halfViewH = std::tan((fovDegrees * M_PI / 180.0) * .5) * cameraDistance;
    const qreal halfViewW = halfViewH * aspect;
    const QVector3D centerWorld(static_cast<float>(((2.0 * center.x() / w) - 1.0) * halfViewW),
                                static_cast<float>(((2.0 * center.y() / h) - 1.0) * halfViewH), 0.0f);
    QMatrix4x4 projection; projection.perspective(fovDegrees, static_cast<float>(aspect), .1f, 32.0f);
    QMatrix4x4 view; view.lookAt(QVector3D(0, 0, cameraDistance), QVector3D(0, 0, 0), QVector3D(0, 1, 0));
    QMatrix4x4 model;
    model.translate(centerWorld);
    model.rotate(static_cast<float>(title.vulkan3DYawDegrees), 0, 1, 0);
    model.rotate(static_cast<float>(title.vulkan3DPitchDegrees), 1, 0, 0);
    model.rotate(static_cast<float>(title.vulkan3DRollDegrees), 0, 0, 1);
    model.translate(0, 0, static_cast<float>(qBound<qreal>(-3.0, title.vulkan3DDepth, 3.0)));
    const qreal scale = (pixelHeight / h) * 2.0 * halfViewH * qBound<qreal>(.05, title.vulkan3DScale, 4.0);
    model.scale(static_cast<float>(scale));
    const QMatrix4x4 mvp = projection * view * model;
    std::copy(mvp.constData(), mvp.constData() + 16, outMvp);
}

} // namespace

VulkanTextPipeline::~VulkanTextPipeline()
{
    destroy();
}

bool VulkanTextPipeline::initialize(VkDevice device,
                                    QVulkanDeviceFunctions* funcs,
                                    VkRenderPass renderPass,
                                    VkDescriptorSetLayout descriptorSetLayout,
                                    QString* errorMessage)
{
    destroy();
    Q_UNUSED(funcs);
    if (!device || renderPass == VK_NULL_HANDLE || descriptorSetLayout == VK_NULL_HANDLE) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Vulkan text pipeline init received invalid handles.");
        }
        return false;
    }
    m_device = device;

    const QString shaderDir = QStringLiteral(JCUT_VULKAN_SHADER_DIR);
    m_vertShader = createShaderModule(shaderDir + QStringLiteral("/text.vert.spv"), errorMessage);
    m_fragShader = createShaderModule(shaderDir + QStringLiteral("/text.frag.spv"), errorMessage);
    m_meshVertShader = createShaderModule(shaderDir + QStringLiteral("/title_mesh.vert.spv"), errorMessage);
    m_meshFragShader = createShaderModule(shaderDir + QStringLiteral("/title_mesh.frag.spv"), errorMessage);
    if (m_vertShader == VK_NULL_HANDLE || m_fragShader == VK_NULL_HANDLE ||
        m_meshVertShader == VK_NULL_HANDLE || m_meshFragShader == VK_NULL_HANDLE) {
        destroy();
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = kTextPushSize;
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create Vulkan text pipeline layout.");
        }
        destroy();
        return false;
    }

    VkPipelineShaderStageCreateInfo shaderStages[2]{};
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = m_vertShader;
    shaderStages[0].pName = "main";
    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = m_fragShader;
    shaderStages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;
    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create Vulkan text graphics pipeline.");
        }
        destroy();
        return false;
    }
    VkVertexInputBindingDescription meshBinding{};
    meshBinding.binding = 0;
    meshBinding.stride = sizeof(TitleMeshVertex);
    meshBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription meshAttributes[3]{};
    meshAttributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(TitleMeshVertex, position))};
    meshAttributes[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(TitleMeshVertex, normal))};
    meshAttributes[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(TitleMeshVertex, uv))};
    VkPipelineVertexInputStateCreateInfo meshVertexInput{};
    meshVertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    meshVertexInput.vertexBindingDescriptionCount = 1;
    meshVertexInput.pVertexBindingDescriptions = &meshBinding;
    meshVertexInput.vertexAttributeDescriptionCount = 3;
    meshVertexInput.pVertexAttributeDescriptions = meshAttributes;
    VkPipelineInputAssemblyStateCreateInfo meshAssembly{};
    meshAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    meshAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineShaderStageCreateInfo meshStages[2] = {shaderStages[0], shaderStages[1]};
    meshStages[0].module = m_meshVertShader;
    meshStages[1].module = m_meshFragShader;
    VkGraphicsPipelineCreateInfo meshPipelineInfo = pipelineInfo;
    meshPipelineInfo.pStages = meshStages;
    meshPipelineInfo.pVertexInputState = &meshVertexInput;
    meshPipelineInfo.pInputAssemblyState = &meshAssembly;
    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &meshPipelineInfo, nullptr, &m_meshPipeline) != VK_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("Failed to create Vulkan title mesh pipeline.");
        destroy();
        return false;
    }

    m_ready = true;
    return true;
}

void VulkanTextPipeline::destroy()
{
    if (!m_device) {
        m_device = VK_NULL_HANDLE;
        m_funcs = nullptr;
        m_ready = false;
        return;
    }
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_meshPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_meshPipeline, nullptr); m_meshPipeline = VK_NULL_HANDLE; }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_vertShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(m_device, m_vertShader, nullptr);
        m_vertShader = VK_NULL_HANDLE;
    }
    if (m_fragShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(m_device, m_fragShader, nullptr);
        m_fragShader = VK_NULL_HANDLE;
    }
    if (m_meshVertShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_meshVertShader, nullptr); m_meshVertShader = VK_NULL_HANDLE; }
    if (m_meshFragShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_meshFragShader, nullptr); m_meshFragShader = VK_NULL_HANDLE; }
    m_ready = false;
    m_device = VK_NULL_HANDLE;
    m_funcs = nullptr;
}

VkShaderModule VulkanTextPipeline::createShaderModule(const QString& path, QString* errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to open Vulkan text shader module: %1").arg(path);
        }
        return VK_NULL_HANDLE;
    }
    const QByteArray code = file.readAll();
    if (code.isEmpty() || (code.size() % 4) != 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Invalid Vulkan text shader module: %1").arg(path);
        }
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = static_cast<size_t>(code.size());
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.constData());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_device, &createInfo, nullptr, &module) != VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create Vulkan text shader module: %1").arg(path);
        }
        return VK_NULL_HANDLE;
    }
    return module;
}

void VulkanTextPipeline::bindAndDraw(VkCommandBuffer commandBuffer,
                                     const VkViewport& viewport,
                                     const VkRect2D& scissor,
                                     VkDescriptorSet descriptorSet,
                                     const Push& push) const
{
    if (!m_ready || commandBuffer == VK_NULL_HANDLE || descriptorSet == VK_NULL_HANDLE) {
        return;
    }
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdBindDescriptorSets(commandBuffer,
                                     VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     m_pipelineLayout,
                                     0,
                                     1,
                                     &descriptorSet,
                                     0,
                                     nullptr);
    vkCmdPushConstants(commandBuffer,
                                m_pipelineLayout,
                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                0,
                                sizeof(Push),
                                &push);
    vkCmdDraw(commandBuffer, 4, 1, 0, 0);
}

void VulkanTextPipeline::bindAndDrawMesh(VkCommandBuffer commandBuffer,
                                         const VkViewport& viewport,
                                         const VkRect2D& scissor,
                                         VkDescriptorSet descriptorSet,
                                         VkBuffer vertexBuffer,
                                         uint32_t vertexCount,
                                         const Push& push) const
{
    if (!m_ready || m_meshPipeline == VK_NULL_HANDLE || commandBuffer == VK_NULL_HANDLE ||
        descriptorSet == VK_NULL_HANDLE || vertexBuffer == VK_NULL_HANDLE || vertexCount == 0) return;
    VkDeviceSize offset = 0;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_meshPipeline);
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                            0, 1, &descriptorSet, 0, nullptr);
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &offset);
    vkCmdPushConstants(commandBuffer, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &push);
    vkCmdDraw(commandBuffer, vertexCount, 1, 0, 0);
}

VulkanTextRenderer::~VulkanTextRenderer()
{
    destroy();
}

bool VulkanTextRenderer::initialize(VkPhysicalDevice physicalDevice,
                                    VkDevice device,
                                    QVulkanDeviceFunctions* funcs,
                                    VkRenderPass renderPass,
                                    QString* errorMessage)
{
    destroy();
    if (!physicalDevice || !device || renderPass == VK_NULL_HANDLE) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Invalid Vulkan text renderer initialization handles.");
        }
        return false;
    }
    m_physicalDevice = physicalDevice;
    m_device = device;
    m_funcs = funcs;
    m_atlasResources = std::make_unique<VulkanResources>();
    if (!m_atlasResources->initialize(physicalDevice, device, funcs)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to initialize Vulkan text atlas resources.");
        }
        destroy();
        return false;
    }
    m_pipeline = std::make_unique<VulkanTextPipeline>();
    if (!m_pipeline->initialize(device, funcs, renderPass, m_atlasResources->descriptorSetLayout(), errorMessage)) {
        destroy();
        return false;
    }
    return true;
}

void VulkanTextRenderer::destroy()
{
    if (m_titleMeshBuffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, m_titleMeshBuffer, nullptr);
    if (m_titleMeshMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_titleMeshMemory, nullptr);
    m_titleMeshBuffer = VK_NULL_HANDLE;
    m_titleMeshMemory = VK_NULL_HANDLE;
    m_titleMeshBufferCapacity = 0;
    m_titleMeshBufferKey.clear();
    m_pipeline.reset();
    m_atlasResources.reset();
    m_uploadedAtlasKey.clear();
    m_speakerLayoutCache = SpeakerLayoutCache{};
    m_transcriptLayoutCache = TranscriptLayoutCache{};
    m_titleLayoutCache = TitleLayoutCache{};
    m_physicalDevice = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_funcs = nullptr;
}

uint32_t VulkanTextRenderer::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & properties) == properties) return i;
    return UINT32_MAX;
}

bool VulkanTextRenderer::ensureTitleMeshBuffer(VkDeviceSize bytes)
{
    if (m_titleMeshBuffer != VK_NULL_HANDLE && m_titleMeshBufferCapacity >= bytes) return true;
    if (m_titleMeshBuffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, m_titleMeshBuffer, nullptr);
    if (m_titleMeshMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_titleMeshMemory, nullptr);
    m_titleMeshBuffer = VK_NULL_HANDLE; m_titleMeshMemory = VK_NULL_HANDLE; m_titleMeshBufferCapacity = 0;
    m_titleMeshBufferKey.clear();
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = qMax<VkDeviceSize>(bytes, 1);
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &info, nullptr, &m_titleMeshBuffer) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(m_device, m_titleMeshBuffer, &requirements);
    const uint32_t type = findMemoryType(requirements.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    if (vkAllocateMemory(m_device, &allocation, nullptr, &m_titleMeshMemory) != VK_SUCCESS ||
        vkBindBufferMemory(m_device, m_titleMeshBuffer, m_titleMeshMemory, 0) != VK_SUCCESS) return false;
    m_titleMeshBufferCapacity = requirements.size;
    return true;
}

bool VulkanTextRenderer::isReady() const
{
    return m_atlasResources && m_atlasResources->isReady() && m_pipeline && m_pipeline->isReady();
}

bool VulkanTextRenderer::beginFrameUploads(size_t frameSlot, size_t frameSlotCount)
{
    return m_atlasResources && m_atlasResources->beginFrameUploads(frameSlot, frameSlotCount);
}

bool VulkanTextRenderer::fail(const QString& reason) const
{
    m_lastFailureReason = reason;
    return false;
}

VulkanTextLayoutDebug VulkanTextRenderer::buildSpeakerLabelLayoutForTesting(
    const QSize& outputSize,
    const render_detail::SpeakerLabelOverlaySpec& spec) const
{
    Atlas atlas;
    QVector<LaidOutGlyph> glyphs;
    QVector<QRectF> cards;
    VulkanTextLayoutDebug debug;
    debug.valid = buildAtlasAndLayout(outputSize, spec, &atlas, &glyphs, &cards);
    debug.atlasKey = atlas.key;
    debug.layoutKey = speakerLabelLayoutKey(outputSize, spec);
    debug.atlasSize = QSize(atlas.image.width, atlas.image.height);
    debug.glyphAtlasEntryCount = atlas.glyphs.size();
    debug.glyphDrawCount = glyphs.size();
    debug.cardCount = cards.size();
    debug.cards = cards;
    debug.glyphRects.reserve(glyphs.size());
    debug.glyphColors.reserve(glyphs.size());
    for (const LaidOutGlyph& glyph : glyphs) {
        debug.glyphRects.push_back(glyph.rect);
        debug.glyphColors.push_back(glyph.color);
    }
    return debug;
}

VulkanTextLayoutDebug VulkanTextRenderer::buildTranscriptOverlayLayoutForTesting(
    const QSize& outputSize,
    const TimelineClip& clip,
    const TranscriptOverlayLayout& layout,
    const QRectF& outputRect,
    const QString& speakerTitle) const
{
    Atlas atlas;
    QVector<LaidOutGlyph> glyphs;
    QVector<TranscriptBackground> backgrounds;
    QVector<TranscriptHighlight> highlights;
    VulkanTextLayoutDebug debug;
    debug.valid = buildTranscriptAtlasAndLayout(
        outputSize, clip, layout, outputRect, speakerTitle, &atlas, &glyphs, &backgrounds, &highlights);
    debug.atlasKey = atlas.key;
    debug.layoutKey = transcriptOverlayLayoutKey(outputSize, clip, layout, outputRect, speakerTitle);
    debug.atlasSize = QSize(atlas.image.width, atlas.image.height);
    debug.glyphAtlasEntryCount = atlas.glyphs.size();
    debug.glyphDrawCount = glyphs.size();
    debug.backgroundCount = backgrounds.size();
    debug.highlightCount = highlights.size();
    debug.backgrounds.reserve(backgrounds.size());
    for (const TranscriptBackground& background : backgrounds) {
        debug.backgrounds.push_back(background.rect);
    }
    debug.highlights.reserve(highlights.size());
    for (const TranscriptHighlight& highlight : highlights) {
        debug.highlights.push_back(highlight.rect);
    }
    debug.glyphRects.reserve(glyphs.size());
    debug.glyphColors.reserve(glyphs.size());
    for (const LaidOutGlyph& glyph : glyphs) {
        debug.glyphRects.push_back(glyph.rect);
        debug.glyphColors.push_back(glyph.color);
    }
    return debug;
}

bool VulkanTextRenderer::buildTitleAtlasAndLayout(const QSize& outputSize,
                                                  const EvaluatedTitle& title,
                                                  Atlas* atlas,
                                                  QVector<LaidOutGlyph>* glyphs,
                                                  QVector<TranscriptBackground>* backgrounds,
                                                  QPointF* center) const
{
    if (!atlas || !glyphs || !backgrounds || !center || !outputSize.isValid() ||
        !title.valid || title.text.trimmed().isEmpty()) {
        return fail(QStringLiteral("title_invalid_input"));
    }

    const int pixelSize = qBound(8, static_cast<int>(std::round(title.fontSize)), 220);
    FaceGuard face;
    if (!loadFace(title.fontFamily, title.bold, pixelSize, &face)) {
        return fail(QStringLiteral("title_font_load_failed"));
    }

    QStringList lines = title.text.split(QLatin1Char('\n'));
    lines.erase(std::remove_if(lines.begin(), lines.end(), [](const QString& line) {
        return line.trimmed().isEmpty();
    }), lines.end());
    if (lines.isEmpty()) {
        return fail(QStringLiteral("title_empty_lines"));
    }

    atlas->image.width = kAtlasSize;
    atlas->image.height = kAtlasSize;
    atlas->image.rgbaPremultiplied = QByteArray(kAtlasSize * kAtlasSize * 4, char(0));
    blendAtlasPixel(&atlas->image, 0, 0, 255);
    atlas->solidUv = QRectF(0.0, 0.0, 1.0 / kAtlasSize, 1.0 / kAtlasSize);
    atlas->textPatternUv = packPatternTile(&atlas->image,
                                           title.textPatternImagePath,
                                           title.color,
                                           0);
    atlas->framePatternUv = packPatternTile(&atlas->image,
                                            title.windowFramePatternImagePath,
                                            title.windowFrameColor,
                                            1);
    atlas->logoUv = packLogoTile(&atlas->image, title.logoPath);
    int penX = kGlyphPadding;
    int penY = kGlyphPadding;
    int rowHeight = 0;

    auto addGlyph = [&](uint codepoint) -> bool {
        const QString key = glyphKey(codepoint, title.bold, pixelSize);
        if (atlas->glyphs.contains(key)) {
            return true;
        }
        const FT_UInt glyphIndex = FT_Get_Char_Index(face.face, codepoint);
        if (FT_Load_Glyph(face.face, glyphIndex, FT_LOAD_DEFAULT) != 0 ||
            FT_Render_Glyph(face.face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
            return fail(QStringLiteral("title_glyph_render_failed"));
        }
        const FT_GlyphSlot slot = face.face->glyph;
        const FT_Bitmap& bitmap = slot->bitmap;
        const int width = static_cast<int>(bitmap.width);
        const int height = static_cast<int>(bitmap.rows);
        const int erosionRadius = title.vulkan3DExtrudeEnabled &&
                title.textExtrudeMode == TextExtrudeMode::ErodedSolid
            ? qBound(1, qRound(1.0 + title.vulkan3DBevelScale * 1.5), 4) : 0;
        const int packedWidth = width + (erosionRadius > 0 ? width + kGlyphPadding : 0);
        if (penX + packedWidth + kGlyphPadding >= kAtlasSize) {
            penX = kGlyphPadding;
            penY += rowHeight + kGlyphPadding;
            rowHeight = 0;
        }
        if (penY + height + kGlyphPadding >= kAtlasSize) {
            return fail(QStringLiteral("title_atlas_full"));
        }
        for (int row = 0; row < height; ++row) {
            for (int col = 0; col < width; ++col) {
                const unsigned char alpha = bitmap.buffer[row * bitmap.pitch + col];
                blendAtlasPixel(&atlas->image, penX + col, penY + row, alpha);
            }
        }
        Glyph glyph;
        glyph.uv = QRectF(static_cast<qreal>(penX) / kAtlasSize,
                          static_cast<qreal>(penY) / kAtlasSize,
                          static_cast<qreal>(width) / kAtlasSize,
                          static_cast<qreal>(height) / kAtlasSize);
        if (erosionRadius > 0) {
            const QByteArray eroded = erodeGlyphAlpha(bitmap, erosionRadius);
            const int erodedX = penX + width + kGlyphPadding;
            for (int row = 0; row < height; ++row) {
                for (int col = 0; col < width; ++col) {
                    blendAtlasPixel(&atlas->image, erodedX + col, penY + row,
                                    static_cast<unsigned char>(eroded.at(row * width + col)));
                }
            }
            glyph.erodedUv = QRectF(static_cast<qreal>(erodedX) / kAtlasSize,
                                    static_cast<qreal>(penY) / kAtlasSize,
                                    static_cast<qreal>(width) / kAtlasSize,
                                    static_cast<qreal>(height) / kAtlasSize);
        }
        glyph.size = QSize(width, height);
        glyph.bearing = QPointF(slot->bitmap_left, slot->bitmap_top);
        glyph.advance = static_cast<qreal>(slot->advance.x) / 64.0;
        atlas->glyphs.insert(key, glyph);
        penX += packedWidth + kGlyphPadding;
        rowHeight = std::max(rowHeight, height);
        return true;
    };
    for (const QString& line : lines) {
        for (uint codepoint : line.toUcs4()) {
            if (!addGlyph(codepoint)) {
                return false;
            }
        }
    }

    const qreal lineHeight = static_cast<qreal>(face.face->size->metrics.height) / 64.0;
    const qreal ascender = static_cast<qreal>(face.face->size->metrics.ascender) / 64.0;
    const qreal lineGap = qMax<qreal>(2.0, pixelSize * 0.08);
    qreal contentWidth = 0.0;
    for (const QString& line : lines) {
        contentWidth = qMax(contentWidth, measureText(face.face, line));
    }
    if (title.windowWidth > 0.0) {
        contentWidth = qMax(contentWidth, title.windowWidth - (qMax<qreal>(0.0, title.windowPadding) * 2.0));
    }
    const qreal contentHeight = lines.size() * lineHeight + qMax(0, lines.size() - 1) * lineGap;
    *center = QPointF((outputSize.width() - 1) * 0.5 + title.x,
                      (outputSize.height() - 1) * 0.5 + title.y);
    const qreal padding = qMax<qreal>(0.0, title.windowPadding);
    const QRectF contentRect(center->x() - contentWidth * 0.5,
                             center->y() - contentHeight * 0.5,
                             contentWidth,
                             contentHeight);
    if (title.windowEnabled && title.windowOpacity > 0.0) {
        QColor bg = title.windowColor.isValid() ? title.windowColor : QColor(Qt::black);
        bg.setAlphaF(qBound<qreal>(0.0, title.windowOpacity * title.opacity, 1.0));
        backgrounds->push_back(TranscriptBackground{
            contentRect.adjusted(-padding, -padding, padding, padding),
            atlas->solidUv,
            bg,
            qMax<qreal>(0.0, padding * 0.45),
            0,
            1.0,
            atlas->solidUv});
    }
    if (title.windowFrameEnabled && title.windowFrameOpacity > 0.0 && title.windowFrameWidth > 0.0) {
        QColor frame = title.windowFrameColor.isValid() ? title.windowFrameColor : QColor(Qt::white);
        frame.setAlphaF(qBound<qreal>(0.0, title.windowFrameOpacity * title.opacity, 1.0));
        const QRectF outer = contentRect.adjusted(-padding - title.windowFrameGap,
                                                  -padding - title.windowFrameGap,
                                                  padding + title.windowFrameGap,
                                                  padding + title.windowFrameGap);
        const qreal fw = qMax<qreal>(1.0, title.windowFrameWidth);
        const int material = materialStyleId(title.windowFrameMaterialStyle);
        const qreal patternScale = qBound<qreal>(0.10, title.windowFramePatternScale, 8.0);
        backgrounds->push_back(TranscriptBackground{QRectF(outer.left(), outer.top(), outer.width(), fw), atlas->solidUv, frame, 0.0, material, patternScale, atlas->framePatternUv});
        backgrounds->push_back(TranscriptBackground{QRectF(outer.left(), outer.bottom() - fw, outer.width(), fw), atlas->solidUv, frame, 0.0, material, patternScale, atlas->framePatternUv});
        backgrounds->push_back(TranscriptBackground{QRectF(outer.left(), outer.top(), fw, outer.height()), atlas->solidUv, frame, 0.0, material, patternScale, atlas->framePatternUv});
        backgrounds->push_back(TranscriptBackground{QRectF(outer.right() - fw, outer.top(), fw, outer.height()), atlas->solidUv, frame, 0.0, material, patternScale, atlas->framePatternUv});
    }
    if (!atlas->logoUv.isEmpty()) {
        const qreal logoSize = qBound<qreal>(34.0, pixelSize * 1.35, 140.0);
        const QRectF logoRect(contentRect.left() - padding - logoSize - qMax<qreal>(8.0, padding * 0.45),
                              center->y() - logoSize * 0.5,
                              logoSize,
                              logoSize);
        QColor logoTint(Qt::white);
        logoTint.setAlphaF(qBound<qreal>(0.0, title.opacity, 1.0));
        backgrounds->push_back(TranscriptBackground{logoRect, atlas->logoUv, logoTint, logoSize * 0.12, 5, 1.0, atlas->logoUv});
    }

    qreal y = contentRect.top();
    for (const QString& line : lines) {
        qreal cursor = center->x() - measureText(face.face, line) * 0.5;
        const qreal baseline = y + ascender;
        FT_UInt previous = 0;
        for (uint codepoint : line.toUcs4()) {
            const FT_UInt glyphIndex = FT_Get_Char_Index(face.face, codepoint);
            if (FT_HAS_KERNING(face.face) && previous && glyphIndex) {
                FT_Vector delta{};
                FT_Get_Kerning(face.face, previous, glyphIndex, FT_KERNING_DEFAULT, &delta);
                cursor += static_cast<qreal>(delta.x) / 64.0;
            }
            const Glyph glyph = atlas->glyphs.value(glyphKey(codepoint, title.bold, pixelSize));
            const QRectF glyphRect(cursor + glyph.bearing.x(),
                                   baseline - glyph.bearing.y(),
                                   glyph.size.width(),
                                   glyph.size.height());
            if (!glyphRect.isEmpty()) {
                if (title.dropShadowEnabled && title.dropShadowOpacity > 0.0) {
                    QColor shadow = title.dropShadowColor.isValid() ? title.dropShadowColor : QColor(Qt::black);
                    shadow.setAlphaF(qBound<qreal>(0.0, title.dropShadowOpacity * title.opacity, 1.0));
                    glyphs->push_back(LaidOutGlyph{
                        glyphRect.translated(title.dropShadowOffsetX, title.dropShadowOffsetY),
                        glyph.uv,
                        shadow,
                        0,
                        1.0,
                        atlas->solidUv,
                        QRectF(),
                        false});
                }
                QColor color = title.color.isValid() ? title.color : QColor(Qt::white);
                color.setAlphaF(qBound<qreal>(0.0, color.alphaF() * title.opacity, 1.0));
                glyphs->push_back(LaidOutGlyph{
                    glyphRect,
                    glyph.uv,
                    color,
                    materialStyleId(title.textMaterialStyle),
                    qBound<qreal>(0.10, title.textPatternScale, 8.0),
                    atlas->textPatternUv,
                    glyph.erodedUv,
                    true});
            }
            cursor += glyph.advance;
            previous = glyphIndex;
        }
        y += lineHeight + lineGap;
    }

    const QString keyMaterial =
        QStringLiteral("title-vktext-v2|") + title.fontFamily + QLatin1Char('|') +
        QString::number(pixelSize) + QLatin1Char('|') +
        QString::number(title.bold ? 1 : 0) + QLatin1Char('|') +
        title.text + QLatin1Char('|') +
        title.logoPath + QLatin1Char('|') +
        QString::number(title.vulkan3DExtrudeEnabled ? 1 : 0) + QLatin1Char('|') +
        QString::number(static_cast<int>(title.textExtrudeMode)) + QLatin1Char('|') +
        QString::number(title.vulkan3DBevelScale, 'f', 3) + QLatin1Char('|') +
        title.textPatternImagePath + QLatin1Char('|') +
        title.windowFramePatternImagePath;
    atlas->key = QString::fromLatin1(QCryptographicHash::hash(keyMaterial.toUtf8(), QCryptographicHash::Sha1).toHex());
    return !glyphs->isEmpty() || !backgrounds->isEmpty();
}

bool VulkanTextRenderer::buildAtlasAndLayout(const QSize& outputSize,
                                             const render_detail::SpeakerLabelOverlaySpec& spec,
                                             Atlas* atlas,
                                             QVector<LaidOutGlyph>* glyphs,
                                             QVector<QRectF>* cards) const
{
    if (!atlas || !glyphs || !cards || !outputSize.isValid()) {
        return fail(QStringLiteral("speaker_invalid_input"));
    }
    const QString name = spec.name.trimmed();
    const QString organization = spec.organization.trimmed();
    if ((spec.showName && name.isEmpty()) && (spec.showOrganization && organization.isEmpty())) {
        return fail(QStringLiteral("speaker_empty_text"));
    }

    const qreal base = qMax<qreal>(1.0, qMin(outputSize.width(), outputSize.height()));
    const int namePixelSize = qBound(8, static_cast<int>(std::round(base * 0.042 * qBound<qreal>(0.25, spec.nameTextScale, 3.0))), 160);
    const int orgPixelSize = qBound(8, static_cast<int>(std::round(base * 0.030 * qBound<qreal>(0.25, spec.organizationTextScale, 3.0))), 140);

    FaceGuard nameFace;
    FaceGuard orgFace;
    if (!loadFace(spec.fontFamily, true, namePixelSize, &nameFace) ||
        !loadFace(spec.fontFamily, false, orgPixelSize, &orgFace)) {
        return fail(QStringLiteral("speaker_font_load_failed"));
    }

    struct Line {
        QString text;
        bool name = false;
    };
    QVector<Line> nameLines;
    QVector<Line> orgLines;
    const qreal maxTextWidth = qMax<qreal>(180.0, outputSize.width() * 0.72);
    if (spec.showName && !name.isEmpty()) {
        for (const QString& line : wrapText(nameFace.face, name, maxTextWidth)) {
            nameLines.push_back(Line{line, true});
        }
    }
    if (spec.showOrganization && !organization.isEmpty()) {
        for (const QString& line : wrapText(orgFace.face, organization, maxTextWidth)) {
            orgLines.push_back(Line{line, false});
        }
    }
    if (nameLines.isEmpty() && orgLines.isEmpty()) {
        return false;
    }

    atlas->image.width = kAtlasSize;
    atlas->image.height = kAtlasSize;
    atlas->image.rgbaPremultiplied = QByteArray(kAtlasSize * kAtlasSize * 4, char(0));
    blendAtlasPixel(&atlas->image, 0, 0, 255);
    atlas->solidUv = QRectF(0.0, 0.0, 1.0 / kAtlasSize, 1.0 / kAtlasSize);
    int penX = kGlyphPadding;
    int penY = kGlyphPadding;
    int rowHeight = 0;

    auto addGlyph = [&](FT_Face face, uint codepoint, bool bold, int pixelSize) -> bool {
        const QString key = glyphKey(codepoint, bold, pixelSize);
        if (atlas->glyphs.contains(key)) {
            return true;
        }
        const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
        if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_DEFAULT) != 0 ||
            FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
            return fail(QStringLiteral("speaker_glyph_render_failed"));
        }
        const FT_GlyphSlot slot = face->glyph;
        const FT_Bitmap& bitmap = slot->bitmap;
        const int width = static_cast<int>(bitmap.width);
        const int height = static_cast<int>(bitmap.rows);
        const int erosionRadius = spec.textExtrudeMode == TextExtrudeMode::ErodedSolid
            ? qBound(1, qRound(1.0 + spec.textExtrudeBevelScale * 1.5), 4) : 0;
        const int packedWidth = width + (erosionRadius > 0 ? width + kGlyphPadding : 0);
        if (penX + packedWidth + kGlyphPadding >= kAtlasSize) {
            penX = kGlyphPadding;
            penY += rowHeight + kGlyphPadding;
            rowHeight = 0;
        }
        if (penY + height + kGlyphPadding >= kAtlasSize) {
            return fail(QStringLiteral("speaker_atlas_full"));
        }
        for (int row = 0; row < height; ++row) {
            for (int col = 0; col < width; ++col) {
                const unsigned char alpha = bitmap.buffer[row * bitmap.pitch + col];
                blendAtlasPixel(&atlas->image, penX + col, penY + row, alpha);
            }
        }
        Glyph glyph;
        glyph.uv = QRectF(static_cast<qreal>(penX) / kAtlasSize,
                          static_cast<qreal>(penY) / kAtlasSize,
                          static_cast<qreal>(width) / kAtlasSize,
                          static_cast<qreal>(height) / kAtlasSize);
        if (erosionRadius > 0) {
            const QByteArray eroded = erodeGlyphAlpha(bitmap, erosionRadius);
            const int erodedX = penX + width + kGlyphPadding;
            for (int row = 0; row < height; ++row) {
                for (int col = 0; col < width; ++col) {
                    blendAtlasPixel(&atlas->image, erodedX + col, penY + row,
                                    static_cast<unsigned char>(eroded.at(row * width + col)));
                }
            }
            glyph.erodedUv = QRectF(static_cast<qreal>(erodedX) / kAtlasSize,
                                    static_cast<qreal>(penY) / kAtlasSize,
                                    static_cast<qreal>(width) / kAtlasSize,
                                    static_cast<qreal>(height) / kAtlasSize);
        }
        glyph.size = QSize(width, height);
        glyph.bearing = QPointF(slot->bitmap_left, slot->bitmap_top);
        glyph.advance = static_cast<qreal>(slot->advance.x) / 64.0;
        atlas->glyphs.insert(key, glyph);
        penX += packedWidth + kGlyphPadding;
        rowHeight = std::max(rowHeight, height);
        return true;
    };

    auto addLineGlyphs = [&](const QVector<Line>& lines) -> bool {
        for (const Line& line : lines) {
            for (uint codepoint : line.text.toUcs4()) {
                FT_Face face = line.name ? nameFace.face : orgFace.face;
                if (!addGlyph(face, codepoint, line.name, line.name ? namePixelSize : orgPixelSize)) {
                    return false;
                }
            }
        }
        return true;
    };
    if (!addLineGlyphs(nameLines) || !addLineGlyphs(orgLines)) {
        return fail(m_lastFailureReason.isEmpty()
                        ? QStringLiteral("speaker_add_glyph_failed")
                        : m_lastFailureReason);
    }

    auto lineHeight = [](FT_Face face) {
        return static_cast<qreal>(face->size->metrics.height) / 64.0;
    };
    auto ascender = [](FT_Face face) {
        return static_cast<qreal>(face->size->metrics.ascender) / 64.0;
    };

    const qreal paddingX = qMax<qreal>(18.0, base * 0.028);
    const qreal paddingY = qMax<qreal>(10.0, base * 0.018);
    const qreal lineGap = qMax<qreal>(4.0, base * 0.008);
    const auto layoutBlock = [&](const QVector<Line>& lines, qreal verticalPosition) {
        if (lines.isEmpty()) {
            return;
        }
        qreal contentWidth = 0.0;
        qreal contentHeight = 0.0;
        for (int i = 0; i < lines.size(); ++i) {
            FT_Face face = lines.at(i).name ? nameFace.face : orgFace.face;
            contentWidth = qMax(contentWidth, measureText(face, lines.at(i).text));
            contentHeight += lineHeight(face);
            if (i + 1 < lines.size()) {
                contentHeight += lineGap;
            }
        }
        const qreal cardWidth = qMin<qreal>(outputSize.width() - 32.0,
                                            qMax<qreal>(220.0, contentWidth + paddingX * 2.0));
        const qreal cardHeight = contentHeight + paddingY * 2.0;
        const qreal centerY = qBound<qreal>(cardHeight * 0.5,
                                            outputSize.height() * qBound<qreal>(0.0, verticalPosition, 1.0),
                                            outputSize.height() - cardHeight * 0.5);
        const QRectF cardRect((outputSize.width() - cardWidth) * 0.5,
                              centerY - cardHeight * 0.5,
                              cardWidth,
                              cardHeight);
        cards->push_back(cardRect);
        qreal y = cardRect.top() + paddingY;
        for (const Line& line : lines) {
            FT_Face face = line.name ? nameFace.face : orgFace.face;
            const int pixelSize = line.name ? namePixelSize : orgPixelSize;
            const qreal width = measureText(face, line.text);
            qreal cursor = cardRect.left() + qMax<qreal>(paddingX, (cardRect.width() - width) * 0.5);
            const qreal baseline = y + ascender(face);
            FT_UInt previous = 0;
            for (uint codepoint : line.text.toUcs4()) {
                const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
                if (FT_HAS_KERNING(face) && previous && glyphIndex) {
                    FT_Vector delta{};
                    FT_Get_Kerning(face, previous, glyphIndex, FT_KERNING_DEFAULT, &delta);
                    cursor += static_cast<qreal>(delta.x) / 64.0;
                }
                const QString key = glyphKey(codepoint, line.name, pixelSize);
                const Glyph glyph = atlas->glyphs.value(key);
                const QRectF glyphRect(cursor + glyph.bearing.x(),
                                       baseline - glyph.bearing.y(),
                                       glyph.size.width(),
                                       glyph.size.height());
                if (!glyphRect.isEmpty()) {
                    if (spec.showShadow) {
                        glyphs->push_back(
                            LaidOutGlyph{glyphRect.translated(2.0, 2.0), glyph.uv, spec.shadowColor});
                    }
                    glyphs->push_back(LaidOutGlyph{glyphRect, glyph.uv, line.name ? spec.nameColor : spec.organizationColor});
                }
                cursor += glyph.advance;
                previous = glyphIndex;
            }
            y += lineHeight(face) + lineGap;
        }
    };

    layoutBlock(nameLines, spec.nameVerticalPosition);
    layoutBlock(orgLines, spec.organizationVerticalPosition);

    const QString keyMaterial =
        QStringLiteral("speaker-label-vktext|") + spec.fontFamily + QLatin1Char('|') +
        QString::number(namePixelSize) + QLatin1Char('|') +
        QString::number(orgPixelSize) + QLatin1Char('|') +
        name + QLatin1Char('|') + organization + QLatin1Char('|') +
        QString::number(spec.nameTextScale, 'f', 3) + QLatin1Char('|') +
        QString::number(spec.organizationTextScale, 'f', 3) + QLatin1Char('|') +
        QString::number(spec.showShadow ? 1 : 0) + QLatin1Char('|') +
        QString::number(static_cast<int>(spec.textExtrudeMode)) + QLatin1Char('|') +
        QString::number(spec.textExtrudeBevelScale, 'f', 3) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(spec.shadowColor.rgba()));
    atlas->key = QString::fromLatin1(QCryptographicHash::hash(keyMaterial.toUtf8(), QCryptographicHash::Sha1).toHex());
    return !glyphs->isEmpty() ? true : fail(QStringLiteral("speaker_empty_glyph_layout"));
}

bool VulkanTextRenderer::buildTranscriptAtlasAndLayout(const QSize& outputSize,
                                                       const TimelineClip& clip,
                                                       const TranscriptOverlayLayout& layout,
                                                       const QRectF& outputRect,
                                                       const QString& speakerTitle,
                                                       Atlas* atlas,
                                                       QVector<LaidOutGlyph>* glyphs,
                                                       QVector<TranscriptBackground>* backgrounds,
                                                       QVector<TranscriptHighlight>* highlights) const
{
    if (!atlas || !glyphs || !backgrounds || !highlights) {
        return fail(QStringLiteral("transcript_invalid_output"));
    }
    if (!buildTranscriptAtlas(clip, layout, speakerTitle, atlas)) {
        return false;
    }
    return buildTranscriptLayout(outputSize,
                                 clip,
                                 layout,
                                 outputRect,
                                 speakerTitle,
                                 *atlas,
                                 glyphs,
                                 backgrounds,
                                 highlights);
}

bool VulkanTextRenderer::buildTranscriptAtlas(const TimelineClip& clip,
                                              const TranscriptOverlayLayout& layout,
                                              const QString& speakerTitle,
                                              Atlas* atlas) const
{
    if (!atlas || layout.lines.isEmpty()) {
        return fail(QStringLiteral("transcript_empty_layout"));
    }
    const int bodyPixelSize = qMax(1, static_cast<int>(std::round(clip.transcriptOverlay.fontPointSize)));
    const int titlePixelSize = qMax(1, static_cast<int>(std::round(clip.transcriptOverlay.fontPointSize * 0.62)));
    FaceGuard bodyFace;
    if (!loadFace(clip.transcriptOverlay.fontFamily,
                  clip.transcriptOverlay.bold,
                  bodyPixelSize,
                  &bodyFace)) {
        return fail(QStringLiteral("transcript_font_load_failed"));
    }
    FaceGuard titleFace;
    const bool hasTitle = !speakerTitle.trimmed().isEmpty() &&
        loadFace(clip.transcriptOverlay.fontFamily, true, titlePixelSize, &titleFace);

    atlas->image.width = kAtlasSize;
    atlas->image.height = kAtlasSize;
    atlas->image.rgbaPremultiplied = QByteArray(kAtlasSize * kAtlasSize * 4, char(0));
    blendAtlasPixel(&atlas->image, 0, 0, 255);
    atlas->solidUv = QRectF(0.0, 0.0, 1.0 / kAtlasSize, 1.0 / kAtlasSize);
    int penX = kGlyphPadding;
    int penY = kGlyphPadding;
    int rowHeight = 0;

    auto addGlyph = [&](FT_Face face, uint codepoint, bool bold, int pixelSize) -> bool {
        const QString key = glyphKey(codepoint, bold, pixelSize);
        if (atlas->glyphs.contains(key)) {
            return true;
        }
        const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
        if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_DEFAULT) != 0 ||
            FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
            return fail(QStringLiteral("transcript_glyph_render_failed"));
        }
        const FT_GlyphSlot slot = face->glyph;
        const FT_Bitmap& bitmap = slot->bitmap;
        const int width = static_cast<int>(bitmap.width);
        const int height = static_cast<int>(bitmap.rows);
        const int erosionRadius =
            clip.transcriptOverlay.textExtrudeMode == TextExtrudeMode::ErodedSolid
            ? qBound(1, qRound(1.0 + clip.transcriptOverlay.textExtrudeBevelScale * 1.5), 4)
            : 0;
        const int packedWidth = width + (erosionRadius > 0 ? width + kGlyphPadding : 0);
        if (penX + packedWidth + kGlyphPadding >= kAtlasSize) {
            penX = kGlyphPadding;
            penY += rowHeight + kGlyphPadding;
            rowHeight = 0;
        }
        if (penY + height + kGlyphPadding >= kAtlasSize) {
            return fail(QStringLiteral("transcript_atlas_full"));
        }
        for (int row = 0; row < height; ++row) {
            for (int col = 0; col < width; ++col) {
                const unsigned char alpha = bitmap.buffer[row * bitmap.pitch + col];
                blendAtlasPixel(&atlas->image, penX + col, penY + row, alpha);
            }
        }
        Glyph glyph;
        glyph.uv = QRectF(static_cast<qreal>(penX) / kAtlasSize,
                          static_cast<qreal>(penY) / kAtlasSize,
                          static_cast<qreal>(width) / kAtlasSize,
                          static_cast<qreal>(height) / kAtlasSize);
        if (erosionRadius > 0) {
            const QByteArray eroded = erodeGlyphAlpha(bitmap, erosionRadius);
            const int erodedX = penX + width + kGlyphPadding;
            for (int row = 0; row < height; ++row) {
                for (int col = 0; col < width; ++col) {
                    blendAtlasPixel(&atlas->image, erodedX + col, penY + row,
                                    static_cast<unsigned char>(eroded.at(row * width + col)));
                }
            }
            glyph.erodedUv = QRectF(static_cast<qreal>(erodedX) / kAtlasSize,
                                    static_cast<qreal>(penY) / kAtlasSize,
                                    static_cast<qreal>(width) / kAtlasSize,
                                    static_cast<qreal>(height) / kAtlasSize);
        }
        glyph.size = QSize(width, height);
        glyph.bearing = QPointF(slot->bitmap_left, slot->bitmap_top);
        glyph.advance = static_cast<qreal>(slot->advance.x) / 64.0;
        atlas->glyphs.insert(key, glyph);
        penX += packedWidth + kGlyphPadding;
        rowHeight = std::max(rowHeight, height);
        return true;
    };

    if (hasTitle) {
        for (uint codepoint : speakerTitle.toUcs4()) {
            if (!addGlyph(titleFace.face, codepoint, true, titlePixelSize)) {
                return fail(m_lastFailureReason.isEmpty()
                                ? QStringLiteral("transcript_add_title_glyph_failed")
                                : m_lastFailureReason);
            }
        }
    }
    for (const TranscriptOverlayLine& line : layout.lines) {
        for (const QString& word : line.words) {
            for (uint codepoint : word.toUcs4()) {
                if (!addGlyph(bodyFace.face, codepoint, clip.transcriptOverlay.bold, bodyPixelSize)) {
                    return fail(m_lastFailureReason.isEmpty()
                                    ? QStringLiteral("transcript_add_body_glyph_failed")
                                    : m_lastFailureReason);
                }
            }
        }
    }

    atlas->key = transcriptOverlayAtlasKey(clip, layout, speakerTitle);
    return !atlas->glyphs.isEmpty();
}

bool VulkanTextRenderer::buildTranscriptLayout(const QSize& outputSize,
                                               const TimelineClip& clip,
                                               const TranscriptOverlayLayout& layout,
                                               const QRectF& outputRect,
                                               const QString& speakerTitle,
                                               const Atlas& atlas,
                                               QVector<LaidOutGlyph>* glyphs,
                                               QVector<TranscriptBackground>* backgrounds,
                                               QVector<TranscriptHighlight>* highlights) const
{
    if (!glyphs || !backgrounds || !highlights ||
        !outputSize.isValid() || layout.lines.isEmpty() || outputRect.isEmpty() || atlas.key.isEmpty()) {
        return fail(QStringLiteral("transcript_layout_invalid_input"));
    }
    const int bodyPixelSize = qMax(1, static_cast<int>(std::round(clip.transcriptOverlay.fontPointSize)));
    const int titlePixelSize = qMax(1, static_cast<int>(std::round(clip.transcriptOverlay.fontPointSize * 0.62)));
    FaceGuard bodyFace;
    if (!loadFace(clip.transcriptOverlay.fontFamily,
                  clip.transcriptOverlay.bold,
                  bodyPixelSize,
                  &bodyFace)) {
        return fail(QStringLiteral("transcript_layout_font_load_failed"));
    }
    FaceGuard titleFace;
    const bool hasTitle = !speakerTitle.trimmed().isEmpty() &&
        loadFace(clip.transcriptOverlay.fontFamily, true, titlePixelSize, &titleFace);

    auto lineHeight = [](FT_Face face) {
        return static_cast<qreal>(face->size->metrics.height) / 64.0;
    };
    auto ascender = [](FT_Face face) {
        return static_cast<qreal>(face->size->metrics.ascender) / 64.0;
    };
    const qreal padding = qBound<qreal>(0.0, clip.transcriptOverlay.backgroundPadding, 400.0);
    const QRectF textBounds = outputRect.adjusted(padding, padding, -padding, -padding);
    if (textBounds.isEmpty()) {
        return fail(QStringLiteral("transcript_text_bounds_empty"));
    }
    if (clip.transcriptOverlay.showBackground) {
        if (clip.transcriptOverlay.backgroundFrameEnabled &&
            clip.transcriptOverlay.backgroundFrameWidth > 0.0) {
            QColor frameColor = clip.transcriptOverlay.backgroundFrameColor.isValid()
                ? clip.transcriptOverlay.backgroundFrameColor : QColor(Qt::white);
            frameColor.setAlphaF(clip.transcriptOverlay.backgroundFrameOpacity);
            const qreal inset = qMax<qreal>(0.0, clip.transcriptOverlay.backgroundFrameGap);
            backgrounds->push_back(TranscriptBackground{
                outputRect.adjusted(inset, inset, -inset, -inset), atlas.solidUv, frameColor,
                qBound<qreal>(0.0, clip.transcriptOverlay.backgroundCornerRadius, 128.0)});
        }
        QColor backgroundColor = clip.transcriptOverlay.backgroundColor.isValid()
            ? clip.transcriptOverlay.backgroundColor
            : QColor(Qt::black);
        backgroundColor.setAlphaF(qBound<qreal>(0.0, clip.transcriptOverlay.backgroundOpacity, 1.0));
        const qreal frameInset = clip.transcriptOverlay.backgroundFrameEnabled
            ? qMax<qreal>(0.0, clip.transcriptOverlay.backgroundFrameGap + clip.transcriptOverlay.backgroundFrameWidth)
            : 0.0;
        backgrounds->push_back(TranscriptBackground{
            outputRect.adjusted(frameInset, frameInset, -frameInset, -frameInset),
            atlas.solidUv,
            backgroundColor,
            qBound<qreal>(0.0, clip.transcriptOverlay.backgroundCornerRadius, 128.0)});
    }

    QVector<qreal> lineWidths;
    lineWidths.reserve(layout.lines.size());
    qreal contentWidth = 0.0;
    for (const TranscriptOverlayLine& line : layout.lines) {
        qreal width = 0.0;
        for (int wordIndex = 0; wordIndex < line.words.size(); ++wordIndex) {
            width += measureText(bodyFace.face, line.words.at(wordIndex));
            if (wordIndex + 1 < line.words.size()) {
                width += measureText(bodyFace.face, QStringLiteral(" "));
            }
        }
        lineWidths.push_back(width);
        contentWidth = qMax(contentWidth, width);
    }
    const qreal titleWidth = hasTitle ? measureText(titleFace.face, speakerTitle) : 0.0;
    contentWidth = qMax(contentWidth, titleWidth);
    const qreal titleGap = hasTitle ? clip.transcriptOverlay.fontPointSize * 0.30 : 0.0;
    const qreal contentHeight =
        (hasTitle ? lineHeight(titleFace.face) + titleGap : 0.0) +
        (lineHeight(bodyFace.face) * layout.lines.size());
    const qreal docScale = qMin(contentWidth > textBounds.width() ? textBounds.width() / contentWidth : 1.0,
                                contentHeight > textBounds.height() ? textBounds.height() / contentHeight : 1.0);
    qreal cursorY = textBounds.top();
    const QColor shadowColor = colorWithOpacity(clip.transcriptOverlay.shadowColor,
                                                clip.transcriptOverlay.shadowOpacity);
    const qreal shadowOffsetX = clip.transcriptOverlay.shadowOffsetX * docScale;
    const qreal shadowOffsetY = clip.transcriptOverlay.shadowOffsetY * docScale;
    const QColor outlineColor = colorWithOpacity(clip.transcriptOverlay.textOutlineColor,
                                                 clip.transcriptOverlay.textOutlineOpacity);
    const QVector<QPointF> outlineOffsets = clip.transcriptOverlay.textOutlineEnabled
        ? dilationOffsets(qMax<qreal>(0.0, clip.transcriptOverlay.textOutlineWidth) * docScale)
        : QVector<QPointF>{};

    auto emitRun = [&](FT_Face face,
                       const QString& text,
                       qreal x,
                       qreal baseline,
                       bool bold,
                       int pixelSize,
                       const QColor& color) {
        qreal cursor = x;
        FT_UInt previous = 0;
        for (uint codepoint : text.toUcs4()) {
            const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
            if (FT_HAS_KERNING(face) && previous && glyphIndex) {
                FT_Vector delta{};
                FT_Get_Kerning(face, previous, glyphIndex, FT_KERNING_DEFAULT, &delta);
                cursor += (static_cast<qreal>(delta.x) / 64.0) * docScale;
            }
            const QString key = glyphKey(codepoint, bold, pixelSize);
            if (!atlas.glyphs.contains(key)) {
                continue;
            }
            const Glyph glyph = atlas.glyphs.value(key);
            const QRectF glyphRect(cursor + (glyph.bearing.x() * docScale),
                                   baseline - (glyph.bearing.y() * docScale),
                                   glyph.size.width() * docScale,
                                   glyph.size.height() * docScale);
            if (!glyphRect.isEmpty()) {
                glyphs->push_back(LaidOutGlyph{glyphRect, glyph.uv, color});
            }
            cursor += glyph.advance * docScale;
            previous = glyphIndex;
        }
    };

    auto emitDilatedRun = [&](FT_Face face,
                              const QString& text,
                              qreal x,
                              qreal baseline,
                              bool bold,
                              int pixelSize) {
        if (outlineOffsets.isEmpty()) {
            return;
        }
        for (const QPointF& offset : outlineOffsets) {
            emitRun(face,
                    text,
                    x + offset.x(),
                    baseline + offset.y(),
                    bold,
                    pixelSize,
                    outlineColor);
        }
    };

    QColor textColor = clip.transcriptOverlay.textColor.isValid()
        ? clip.transcriptOverlay.textColor
        : QColor(Qt::white);
    textColor.setAlphaF(textColor.alphaF() * qBound<qreal>(0.0, clip.transcriptOverlay.textOpacity, 1.0));
    const QColor highlightFillColor = clip.transcriptOverlay.highlightColor.isValid()
        ? clip.transcriptOverlay.highlightColor
        : QColor(QStringLiteral("#fff2a8"));
    const QColor highlightTextColor = clip.transcriptOverlay.highlightTextColor.isValid()
        ? clip.transcriptOverlay.highlightTextColor
        : QColor(QStringLiteral("#181818"));

    if (hasTitle) {
        const qreal x = textBounds.left() + qMax<qreal>(0.0, (textBounds.width() - (titleWidth * docScale)) / 2.0);
        const qreal baseline = cursorY + ascender(titleFace.face) * docScale;
        if (clip.transcriptOverlay.showShadow) {
            emitRun(titleFace.face,
                    speakerTitle,
                    x + shadowOffsetX,
                    baseline + shadowOffsetY,
                    true,
                    titlePixelSize,
                    shadowColor);
        }
        emitDilatedRun(titleFace.face, speakerTitle, x, baseline, true, titlePixelSize);
        emitRun(titleFace.face, speakerTitle, x, baseline, true, titlePixelSize, textColor);
        cursorY += (lineHeight(titleFace.face) + titleGap) * docScale;
    }

    const qreal spaceWidth = measureText(bodyFace.face, QStringLiteral(" ")) * docScale;
    const qreal bodyLineHeight = lineHeight(bodyFace.face) * docScale;
    const qreal bodyAscender = ascender(bodyFace.face) * docScale;
    const qreal bodyTextHeight =
        (static_cast<qreal>(bodyFace.face->size->metrics.height) / 64.0) * docScale;
    for (int lineIndex = 0; lineIndex < layout.lines.size(); ++lineIndex) {
        const TranscriptOverlayLine& line = layout.lines.at(lineIndex);
        qreal cursorX = textBounds.left() + qMax<qreal>(0.0, (textBounds.width() - (lineWidths.at(lineIndex) * docScale)) / 2.0);
        const qreal baseline = cursorY + bodyAscender;
        for (int wordIndex = 0; wordIndex < line.words.size(); ++wordIndex) {
            const QString& word = line.words.at(wordIndex);
            const qreal wordWidth = measureText(bodyFace.face, word) * docScale;
            const bool active = clip.transcriptOverlay.highlightCurrentWord &&
                                wordIndex == line.activeWord;
            if (active) {
                const qreal padX = 0.18 * clip.transcriptOverlay.fontPointSize * docScale;
                const qreal padY = 0.02 * clip.transcriptOverlay.fontPointSize * docScale;
                highlights->push_back(TranscriptHighlight{
                    QRectF(cursorX - padX,
                           baseline - bodyAscender - padY,
                           wordWidth + padX * 2.0,
                           bodyTextHeight + padY * 2.0),
                    highlightFillColor});
            }
            const QColor glyphColor = active ? highlightTextColor : textColor;
            if (clip.transcriptOverlay.showShadow && !active) {
                emitRun(bodyFace.face,
                        word,
                        cursorX + shadowOffsetX,
                        baseline + shadowOffsetY,
                        clip.transcriptOverlay.bold,
                        bodyPixelSize,
                        shadowColor);
            }
            emitDilatedRun(bodyFace.face,
                           word,
                           cursorX,
                           baseline,
                           clip.transcriptOverlay.bold,
                           bodyPixelSize);
            emitRun(bodyFace.face,
                    word,
                    cursorX,
                    baseline,
                    clip.transcriptOverlay.bold,
                    bodyPixelSize,
                    glyphColor);
            cursorX += wordWidth;
            if (wordIndex + 1 < line.words.size()) {
                cursorX += spaceWidth;
            }
        }
        cursorY += bodyLineHeight;
    }

    return !glyphs->isEmpty() ? true : fail(QStringLiteral("transcript_empty_glyph_layout"));
}

bool VulkanTextRenderer::ensureAtlasUploaded(VkCommandBuffer commandBuffer, const Atlas& atlas)
{
    if (!m_atlasResources || atlas.key.isEmpty()) {
        return fail(QStringLiteral("text_atlas_invalid"));
    }
    if (m_uploadedAtlasKey == atlas.key) {
        return true;
    }
    if (!m_atlasResources->uploadImageTexture(commandBuffer, atlas.image)) {
        return fail(QStringLiteral("text_atlas_upload_failed"));
    }
    m_uploadedAtlasKey = atlas.key;
    return true;
}

const VulkanTextRenderer::SpeakerLayoutCache* VulkanTextRenderer::speakerLabelLayout(
    const QSize& outputSize,
    const render_detail::SpeakerLabelOverlaySpec& spec) const
{
    const QString layoutKey = speakerLabelLayoutKey(outputSize, spec);
    if (m_speakerLayoutCache.valid && m_speakerLayoutCache.layoutKey == layoutKey) {
        return &m_speakerLayoutCache;
    }

    SpeakerLayoutCache rebuilt;
    rebuilt.layoutKey = layoutKey;
    rebuilt.valid = buildAtlasAndLayout(outputSize,
                                        spec,
                                        &rebuilt.atlas,
                                        &rebuilt.glyphs,
                                        &rebuilt.cards);
    if (!rebuilt.valid) {
        m_speakerLayoutCache = SpeakerLayoutCache{};
        return nullptr;
    }
    m_speakerLayoutCache = rebuilt;
    return &m_speakerLayoutCache;
}

const VulkanTextRenderer::TranscriptLayoutCache* VulkanTextRenderer::transcriptOverlayLayout(
    const QSize& outputSize,
    const TimelineClip& clip,
    const TranscriptOverlayLayout& layout,
    const QRectF& outputRect,
    const QString& speakerTitle) const
{
    const QString layoutKey = transcriptOverlayLayoutKey(outputSize, clip, layout, outputRect, speakerTitle);
    if (m_transcriptLayoutCache.valid && m_transcriptLayoutCache.layoutKey == layoutKey) {
        return &m_transcriptLayoutCache;
    }
    const QString atlasKey = transcriptOverlayAtlasKey(clip, layout, speakerTitle);

    TranscriptLayoutCache rebuilt;
    rebuilt.layoutKey = layoutKey;
    rebuilt.atlasKey = atlasKey;
    if (m_transcriptLayoutCache.valid && m_transcriptLayoutCache.atlasKey == atlasKey) {
        rebuilt.atlas = m_transcriptLayoutCache.atlas;
        rebuilt.valid = buildTranscriptLayout(outputSize,
                                              clip,
                                              layout,
                                              outputRect,
                                              speakerTitle,
                                              rebuilt.atlas,
                                              &rebuilt.glyphs,
                                              &rebuilt.backgrounds,
                                              &rebuilt.highlights);
    } else {
        rebuilt.valid = buildTranscriptAtlasAndLayout(outputSize,
                                                      clip,
                                                      layout,
                                                      outputRect,
                                                      speakerTitle,
                                                      &rebuilt.atlas,
                                                      &rebuilt.glyphs,
                                                      &rebuilt.backgrounds,
                                                      &rebuilt.highlights);
        rebuilt.atlasKey = rebuilt.atlas.key;
    }
    if (!rebuilt.valid) {
        m_transcriptLayoutCache = TranscriptLayoutCache{};
        return nullptr;
    }
    m_transcriptLayoutCache = rebuilt;
    return &m_transcriptLayoutCache;
}

const VulkanTextRenderer::TitleLayoutCache* VulkanTextRenderer::titleOverlayLayout(
    const QSize& outputSize,
    const EvaluatedTitle& title) const
{
    const QString layoutMaterial =
        QStringLiteral("title-layout-v1|") +
        QString::number(outputSize.width()) + QLatin1Char('x') + QString::number(outputSize.height()) + QLatin1Char('|') +
        title.text + QLatin1Char('|') +
        title.fontFamily + QLatin1Char('|') +
        QString::number(title.fontSize, 'f', 3) + QLatin1Char('|') +
        QString::number(title.x, 'f', 3) + QLatin1Char('|') +
        QString::number(title.y, 'f', 3) + QLatin1Char('|') +
        QString::number(title.opacity, 'f', 3) + QLatin1Char('|') +
        QString::number(title.bold ? 1 : 0) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(title.color.rgba())) + QLatin1Char('|') +
        title.logoPath + QLatin1Char('|') +
        title.textPatternImagePath + QLatin1Char('|') +
        QString::number(static_cast<int>(title.textMaterialStyle)) + QLatin1Char('|') +
        QString::number(title.textPatternScale, 'f', 3) + QLatin1Char('|') +
        QString::number(title.windowEnabled ? 1 : 0) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(title.windowColor.rgba())) + QLatin1Char('|') +
        QString::number(title.windowOpacity, 'f', 3) + QLatin1Char('|') +
        QString::number(title.windowPadding, 'f', 3) + QLatin1Char('|') +
        QString::number(title.windowFrameEnabled ? 1 : 0) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(title.windowFrameColor.rgba())) + QLatin1Char('|') +
        QString::number(title.windowFrameOpacity, 'f', 3) + QLatin1Char('|') +
        QString::number(title.windowFrameWidth, 'f', 3) + QLatin1Char('|') +
        QString::number(title.windowFrameGap, 'f', 3) + QLatin1Char('|') +
        title.windowFramePatternImagePath + QLatin1Char('|') +
        QString::number(static_cast<int>(title.windowFrameMaterialStyle)) + QLatin1Char('|') +
        QString::number(title.windowFramePatternScale, 'f', 3) + QLatin1Char('|') +
        QString::number(title.vulkan3DExtrudeEnabled ? 1 : 0) + QLatin1Char('|') +
        QString::number(static_cast<int>(title.textExtrudeMode)) + QLatin1Char('|') +
        QString::number(title.vulkan3DExtrudeDepth, 'f', 3) + QLatin1Char('|') +
        QString::number(title.vulkan3DBevelScale, 'f', 3) + QLatin1Char('|') +
        QString::number(title.dropShadowEnabled ? 1 : 0) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(title.dropShadowColor.rgba())) + QLatin1Char('|') +
        QString::number(title.dropShadowOpacity, 'f', 3) + QLatin1Char('|') +
        QString::number(title.dropShadowOffsetX, 'f', 3) + QLatin1Char('|') +
        QString::number(title.dropShadowOffsetY, 'f', 3);
    const QString layoutKey =
        QString::fromLatin1(QCryptographicHash::hash(layoutMaterial.toUtf8(), QCryptographicHash::Sha1).toHex());
    if (m_titleLayoutCache.valid && m_titleLayoutCache.layoutKey == layoutKey) {
        return &m_titleLayoutCache;
    }

    TitleLayoutCache rebuilt;
    rebuilt.layoutKey = layoutKey;
    rebuilt.valid = buildTitleAtlasAndLayout(outputSize,
                                             title,
                                             &rebuilt.atlas,
                                             &rebuilt.glyphs,
                                             &rebuilt.backgrounds,
                                             &rebuilt.center);
    if (rebuilt.valid && title.vulkan3DExtrudeEnabled &&
        title.textExtrudeMode != TextExtrudeMode::None &&
        title.vulkan3DExtrudeDepth > 0.001) {
        rebuilt.meshVertices = buildExtrudedTitleMesh(
            title.text,
            TitleMeshExtrusionOptions{title.fontFamily, title.bold,
                                      qBound(24, qRound(title.fontSize * 2.0), 256),
                                      title.vulkan3DExtrudeDepth,
                                      title.textExtrudeMode == TextExtrudeMode::ErodedSolid
                                          ? title.vulkan3DBevelScale : 0.0},
            nullptr);
    }
    rebuilt.atlasKey = rebuilt.atlas.key;
    if (!rebuilt.valid) {
        m_titleLayoutCache = TitleLayoutCache{};
        return nullptr;
    }
    m_titleLayoutCache = rebuilt;
    return &m_titleLayoutCache;
}

void VulkanTextRenderer::drawGlyph(VkCommandBuffer commandBuffer,
                                   const QSize& swapSize,
                                   const QRectF& rect,
                                   const QRectF& uv,
                                   const QColor& color)
{
    if (!isReady() || !color.isValid() || color.alpha() <= 0 || rect.isEmpty()) {
        return;
    }
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(std::max(1, swapSize.width()));
    viewport.height = static_cast<float>(std::max(1, swapSize.height()));
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {static_cast<uint32_t>(std::max(1, swapSize.width())),
                      static_cast<uint32_t>(std::max(1, swapSize.height()))};

    VulkanTextPipeline::Push push{};
    mvpForScreenRect(rect, swapSize, push.mvp);
    push.uvRect[0] = static_cast<float>(uv.x());
    push.uvRect[1] = static_cast<float>(uv.y());
    push.uvRect[2] = static_cast<float>(uv.width());
    push.uvRect[3] = static_cast<float>(uv.height());
    push.color[0] = static_cast<float>(color.redF());
    push.color[1] = static_cast<float>(color.greenF());
    push.color[2] = static_cast<float>(color.blueF());
    push.color[3] = static_cast<float>(color.alphaF());
    push.material[0] = 0.0f;
    push.material[1] = 1.0f;
    push.material[2] = 0.0f;
    push.material[3] = 0.0f;
    push.patternRect[0] = static_cast<float>(uv.x());
    push.patternRect[1] = static_cast<float>(uv.y());
    push.patternRect[2] = static_cast<float>(uv.width());
    push.patternRect[3] = static_cast<float>(uv.height());
    m_pipeline->bindAndDraw(commandBuffer, viewport, scissor, m_atlasResources->descriptorSet(), push);
}

void VulkanTextRenderer::drawGlyphWithMvp(VkCommandBuffer commandBuffer,
                                          const QSize& swapSize,
                                          const float mvp[16],
                                          const QRectF& uv,
                                          const QColor& color,
                                          int materialStyle,
                                          qreal patternScale,
                                          const QRectF& patternUv)
{
    if (!isReady() || !color.isValid() || color.alpha() <= 0 || !mvp) {
        return;
    }
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(std::max(1, swapSize.width()));
    viewport.height = static_cast<float>(std::max(1, swapSize.height()));
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {static_cast<uint32_t>(std::max(1, swapSize.width())),
                      static_cast<uint32_t>(std::max(1, swapSize.height()))};

    VulkanTextPipeline::Push push{};
    std::copy(mvp, mvp + 16, push.mvp);
    push.uvRect[0] = static_cast<float>(uv.x());
    push.uvRect[1] = static_cast<float>(uv.y());
    push.uvRect[2] = static_cast<float>(uv.width());
    push.uvRect[3] = static_cast<float>(uv.height());
    push.color[0] = static_cast<float>(color.redF());
    push.color[1] = static_cast<float>(color.greenF());
    push.color[2] = static_cast<float>(color.blueF());
    push.color[3] = static_cast<float>(color.alphaF());
    push.material[0] = static_cast<float>(qBound(0, materialStyle, 4));
    push.material[1] = static_cast<float>(qBound<qreal>(0.10, patternScale, 8.0));
    push.material[2] = 0.0f;
    push.material[3] = 0.0f;
    const QRectF pattern = patternUv.isEmpty() ? uv : patternUv;
    push.patternRect[0] = static_cast<float>(pattern.x());
    push.patternRect[1] = static_cast<float>(pattern.y());
    push.patternRect[2] = static_cast<float>(pattern.width());
    push.patternRect[3] = static_cast<float>(pattern.height());
    m_pipeline->bindAndDraw(commandBuffer, viewport, scissor, m_atlasResources->descriptorSet(), push);
}

bool VulkanTextRenderer::drawTitleMesh(VkCommandBuffer commandBuffer,
                                       const QSize& swapSize,
                                       const QSize& outputSize,
                                       const QRectF& outputTargetRect,
                                       const EvaluatedTitle& title,
                                       const TitleLayoutCache& layout)
{
    if (layout.meshVertices.isEmpty()) return false;
    EvaluatedTitle meshTitle = title;
    if (std::abs(meshTitle.vulkan3DYawDegrees) < 0.001 &&
        std::abs(meshTitle.vulkan3DPitchDegrees) < 0.001 &&
        std::abs(meshTitle.vulkan3DRollDegrees) < 0.001) {
        // A front-on real mesh has no visible side wall. Keep authored camera
        // rotations intact, but give extrusion-only titles a readable default.
        meshTitle.vulkan3DYawDegrees = -32.0;
        meshTitle.vulkan3DPitchDegrees = 8.0;
    }
    const QString meshBufferKey = layout.layoutKey + QLatin1Char('|') +
        QString::number(meshTitle.vulkan3DYawDegrees, 'f', 3) + QLatin1Char('|') +
        QString::number(meshTitle.vulkan3DPitchDegrees, 'f', 3);
    QVector<TitleMeshVertex> orderedVertices;
    if (m_titleMeshBufferKey != meshBufferKey) {
        orderedVertices = layout.meshVertices;
        // The overlay render passes do not universally expose a depth attachment.
        // Order transparent mesh triangles back-to-front in camera space so bevels
        // and side walls remain correct in both preview and export.
        const float yaw = qDegreesToRadians(static_cast<float>(meshTitle.vulkan3DYawDegrees));
        const float pitch = qDegreesToRadians(static_cast<float>(meshTitle.vulkan3DPitchDegrees));
        const float cy = std::cos(yaw), sy = std::sin(yaw);
        const float cx = std::cos(pitch), sx = std::sin(pitch);
        QVector<int> triangles;
        triangles.reserve(orderedVertices.size() / 3);
        for (int i = 0; i + 2 < orderedVertices.size(); i += 3) triangles.push_back(i);
        const auto cameraZ = [&](int index) {
            const QVector3D p = (orderedVertices.at(index).position + orderedVertices.at(index + 1).position +
                                 orderedVertices.at(index + 2).position) / 3.0f;
            const float zAfterYaw = -sy * p.x() + cy * p.z();
            return sx * p.y() + cx * zAfterYaw;
        };
        std::stable_sort(triangles.begin(), triangles.end(), [&](int left, int right) {
            return cameraZ(left) < cameraZ(right);
        });
        QVector<TitleMeshVertex> sorted;
        sorted.reserve(orderedVertices.size());
        for (int index : triangles) {
            sorted.append(orderedVertices.at(index));
            sorted.append(orderedVertices.at(index + 1));
            sorted.append(orderedVertices.at(index + 2));
        }
        orderedVertices = std::move(sorted);
    }
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(layout.meshVertices.size() * sizeof(TitleMeshVertex));
    if (!ensureTitleMeshBuffer(bytes)) return false;
    if (m_titleMeshBufferKey != meshBufferKey) {
        void* mapped = nullptr;
        if (vkMapMemory(m_device, m_titleMeshMemory, 0, bytes, 0, &mapped) != VK_SUCCESS || !mapped) return false;
        std::memcpy(mapped, orderedVertices.constData(), static_cast<size_t>(bytes));
        vkUnmapMemory(m_device, m_titleMeshMemory);
        m_titleMeshBufferKey = meshBufferKey;
    }
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(qMax(1, swapSize.width())),
                        static_cast<float>(qMax(1, swapSize.height())), 0.0f, 1.0f};
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {static_cast<uint32_t>(qMax(1, swapSize.width())), static_cast<uint32_t>(qMax(1, swapSize.height()))};
    const qreal scaleX = outputTargetRect.width() / qMax<qreal>(1.0, outputSize.width());
    const qreal scaleY = outputTargetRect.height() / qMax<qreal>(1.0, outputSize.height());
    const QPointF center(outputTargetRect.left() + layout.center.x() * scaleX,
                         outputTargetRect.top() + layout.center.y() * scaleY);
    VulkanTextPipeline::Push push{};
    mvpForTitleMesh(center, meshTitle.fontSize * scaleY, swapSize, meshTitle, push.mvp);
    const QColor color = title.color.isValid() ? title.color : QColor(Qt::white);
    push.color[0] = color.redF(); push.color[1] = color.greenF(); push.color[2] = color.blueF();
    push.color[3] = qBound<qreal>(0.0, color.alphaF() * title.opacity, 1.0);
    push.material[2] = qDegreesToRadians(static_cast<float>(meshTitle.vulkan3DYawDegrees));
    push.material[3] = qDegreesToRadians(static_cast<float>(meshTitle.vulkan3DPitchDegrees));
    push.uvRect[3] = qDegreesToRadians(static_cast<float>(meshTitle.vulkan3DRollDegrees));
    m_pipeline->bindAndDrawMesh(commandBuffer, viewport, scissor, m_atlasResources->descriptorSet(),
                                m_titleMeshBuffer, static_cast<uint32_t>(layout.meshVertices.size()), push);
    return true;
}

void VulkanTextRenderer::drawSolidRoundedRect(VkCommandBuffer commandBuffer,
                                              const QSize& swapSize,
                                              const QRectF& rect,
                                              qreal radius,
                                              const QRectF& uv,
                                              const QColor& color)
{
    if (!isReady() || !color.isValid() || color.alpha() <= 0 || rect.isEmpty()) {
        return;
    }
    const QRect bounded = rect.normalized().toAlignedRect().intersected(
        QRect(0, 0, std::max(1, swapSize.width()), std::max(1, swapSize.height())));
    if (bounded.isEmpty()) {
        return;
    }
    const int r = std::max(0, std::min({
        static_cast<int>(std::round(radius)),
        bounded.width() / 2,
        bounded.height() / 2
    }));
    auto drawSpan = [&](int x, int y, int w, int h) {
        if (w <= 0 || h <= 0) {
            return;
        }
        drawGlyph(commandBuffer, swapSize, QRectF(x, y, w, h), uv, color);
    };
    const int x = bounded.x();
    const int y = bounded.y();
    const int w = bounded.width();
    const int h = bounded.height();
    if (r <= 0) {
        drawSpan(x, y, w, h);
        return;
    }
    drawSpan(x + r, y, w - (2 * r), h);
    drawSpan(x, y + r, r, h - (2 * r));
    drawSpan(x + w - r, y + r, r, h - (2 * r));
    for (int dy = 0; dy < r; ++dy) {
        const double yCenter = static_cast<double>(r) - static_cast<double>(dy) - 0.5;
        const int dx = static_cast<int>(std::floor(
            std::sqrt(std::max(0.0, static_cast<double>(r * r) - (yCenter * yCenter)))));
        const int inset = std::max(0, r - dx);
        drawSpan(x + inset, y + dy, w - (2 * inset), 1);
        drawSpan(x + inset, y + h - dy - 1, w - (2 * inset), 1);
    }
}

bool VulkanTextRenderer::drawSpeakerLabel(VkCommandBuffer commandBuffer,
                                          const QSize& swapSize,
                                          const QSize& outputSize,
                                          const QRectF& outputTargetRect,
                                          const render_detail::SpeakerLabelOverlaySpec& spec)
{
    m_lastFailureReason.clear();
    if (!isReady() || commandBuffer == VK_NULL_HANDLE || !swapSize.isValid() || !outputSize.isValid()) {
        return fail(QStringLiteral("speaker_draw_invalid_state"));
    }
    const SpeakerLayoutCache* layout = speakerLabelLayout(outputSize, spec);
    if (!layout || !ensureAtlasUploaded(commandBuffer, layout->atlas)) {
        return fail(m_lastFailureReason.isEmpty()
                        ? QStringLiteral("speaker_draw_atlas_unavailable")
                        : m_lastFailureReason);
    }

    const qreal scaleX = outputTargetRect.width() / qMax<qreal>(1.0, outputSize.width());
    const qreal scaleY = outputTargetRect.height() / qMax<qreal>(1.0, outputSize.height());
    auto mapRect = [&](const QRectF& rect) {
        return QRectF(outputTargetRect.left() + rect.left() * scaleX,
                      outputTargetRect.top() + rect.top() * scaleY,
                      rect.width() * scaleX,
                      rect.height() * scaleY);
    };

    for (const QRectF& card : layout->cards) {
        const QRectF mapped = mapRect(card);
        const qreal scale = qMin(scaleX, scaleY);
        const qreal borderWidth = qBound<qreal>(0.0, spec.borderWidth, 16.0) * scale;
        const qreal radius = qBound<qreal>(0.0, spec.backgroundCornerRadius, 128.0) * scale;
        if (borderWidth > 0.0 && spec.borderColor.alpha() > 0) {
            drawSolidRoundedRect(commandBuffer,
                                 swapSize,
                                 mapped,
                                 radius,
                                 layout->atlas.solidUv,
                                 spec.borderColor);
        }
        const QRectF backgroundRect = borderWidth > 0.0
            ? mapped.adjusted(borderWidth, borderWidth, -borderWidth, -borderWidth)
            : mapped;
        drawSolidRoundedRect(commandBuffer,
                             swapSize,
                             backgroundRect,
                             qMax<qreal>(0.0, radius - borderWidth),
                             layout->atlas.solidUv,
                             spec.backgroundColor);
    }
    const QVector<qreal> speakerExtrusion = textExtrusionLayerOffsets(
        spec.textExtrudeMode, spec.textExtrudeDepth);
    for (int layerIndex = 0; layerIndex < speakerExtrusion.size(); ++layerIndex) {
        const int layer = speakerExtrusion.size() - layerIndex;
        for (const LaidOutGlyph& glyph : layout->glyphs) {
            QColor side = glyph.color.darker(175);
            const QRectF uv = spec.textExtrudeMode == TextExtrudeMode::ErodedSolid &&
                    layer <= qRound(1.0 + spec.textExtrudeBevelScale * 2.0) &&
                    !glyph.erodedUv.isEmpty()
                ? glyph.erodedUv : glyph.uv;
            const qreal offset = speakerExtrusion.at(layerIndex);
            drawGlyph(commandBuffer, swapSize,
                      mapRect(glyph.rect.translated(offset, offset * 0.58)), uv, side);
        }
    }
    for (const LaidOutGlyph& glyph : layout->glyphs) {
        drawGlyph(commandBuffer, swapSize, mapRect(glyph.rect), glyph.uv, glyph.color);
    }
    return true;
}

bool VulkanTextRenderer::prepareSpeakerLabelAtlas(VkCommandBuffer commandBuffer,
                                                  const QSize& outputSize,
                                                  const render_detail::SpeakerLabelOverlaySpec& spec)
{
    m_lastFailureReason.clear();
    if (!isReady() || commandBuffer == VK_NULL_HANDLE || !outputSize.isValid()) {
        return fail(QStringLiteral("speaker_prepare_invalid_state"));
    }
    const SpeakerLayoutCache* layout = speakerLabelLayout(outputSize, spec);
    if (!layout) {
        return fail(m_lastFailureReason.isEmpty()
                        ? QStringLiteral("speaker_layout_failed")
                        : m_lastFailureReason);
    }
    if (!ensureAtlasUploaded(commandBuffer, layout->atlas)) {
        return fail(m_lastFailureReason.isEmpty()
                        ? QStringLiteral("speaker_atlas_upload_failed")
                        : m_lastFailureReason);
    }
    return true;
}

bool VulkanTextRenderer::drawTranscriptOverlay(VkCommandBuffer commandBuffer,
                                               const QSize& swapSize,
                                               const QSize& outputSize,
                                               const QRectF& outputTargetRect,
                                               const TimelineClip& clip,
                                               const TranscriptOverlayLayout& layout,
                                               const QRectF& outputRect,
                                               const QString& speakerTitle)
{
    m_lastFailureReason.clear();
    if (!isReady() || commandBuffer == VK_NULL_HANDLE || !swapSize.isValid() ||
        !outputSize.isValid() || layout.lines.isEmpty() || outputRect.isEmpty()) {
        return fail(QStringLiteral("transcript_draw_invalid_state"));
    }
    const TranscriptLayoutCache* cachedLayout =
        transcriptOverlayLayout(outputSize, clip, layout, outputRect, speakerTitle);
    if (!cachedLayout || !ensureAtlasUploaded(commandBuffer, cachedLayout->atlas)) {
        return fail(m_lastFailureReason.isEmpty()
                        ? QStringLiteral("transcript_draw_atlas_unavailable")
                        : m_lastFailureReason);
    }

    const qreal scaleX = outputTargetRect.width() / qMax<qreal>(1.0, outputSize.width());
    const qreal scaleY = outputTargetRect.height() / qMax<qreal>(1.0, outputSize.height());
    auto mapRect = [&](const QRectF& rect) {
        return QRectF(outputTargetRect.left() + rect.left() * scaleX,
                      outputTargetRect.top() + rect.top() * scaleY,
                      rect.width() * scaleX,
                      rect.height() * scaleY);
    };

    for (const TranscriptBackground& background : cachedLayout->backgrounds) {
        drawSolidRoundedRect(commandBuffer,
                             swapSize,
                             mapRect(background.rect),
                             background.radius * qMin(scaleX, scaleY),
                             cachedLayout->atlas.solidUv,
                             background.color);
    }
    for (const TranscriptHighlight& highlight : cachedLayout->highlights) {
        clearRect(m_funcs,
                  commandBuffer,
                  clearValueForColor(highlight.color.isValid()
                                         ? highlight.color
                                         : QColor(QStringLiteral("#fff2a8"))),
                  clearRectFromQRect(mapRect(highlight.rect), swapSize));
    }
    const auto& textSettings = clip.transcriptOverlay;
    const QVector<qreal> transcriptExtrusion = textExtrusionLayerOffsets(
        textSettings.textExtrudeMode, textSettings.textExtrudeDepth);
    for (int layerIndex = 0; layerIndex < transcriptExtrusion.size(); ++layerIndex) {
        const int layer = transcriptExtrusion.size() - layerIndex;
        for (const LaidOutGlyph& glyph : cachedLayout->glyphs) {
            QColor side = glyph.color.darker(175);
            const QRectF uv = textSettings.textExtrudeMode == TextExtrudeMode::ErodedSolid &&
                    layer <= qRound(1.0 + textSettings.textExtrudeBevelScale * 2.0) &&
                    !glyph.erodedUv.isEmpty()
                ? glyph.erodedUv : glyph.uv;
            const qreal offset = transcriptExtrusion.at(layerIndex);
            drawGlyph(commandBuffer, swapSize,
                      mapRect(glyph.rect.translated(offset, offset * 0.58)), uv, side);
        }
    }
    for (const LaidOutGlyph& glyph : cachedLayout->glyphs) {
        drawGlyph(commandBuffer, swapSize, mapRect(glyph.rect), glyph.uv, glyph.color);
    }
    return true;
}

bool VulkanTextRenderer::prepareTranscriptOverlayAtlas(VkCommandBuffer commandBuffer,
                                                       const QSize& outputSize,
                                                       const TimelineClip& clip,
                                                       const TranscriptOverlayLayout& layout,
                                                       const QRectF& outputRect,
                                                       const QString& speakerTitle)
{
    m_lastFailureReason.clear();
    if (!isReady() || commandBuffer == VK_NULL_HANDLE || !outputSize.isValid() ||
        layout.lines.isEmpty() || outputRect.isEmpty()) {
        return fail(QStringLiteral("transcript_prepare_invalid_state"));
    }
    const TranscriptLayoutCache* cachedLayout =
        transcriptOverlayLayout(outputSize, clip, layout, outputRect, speakerTitle);
    if (!cachedLayout) {
        return fail(m_lastFailureReason.isEmpty()
                        ? QStringLiteral("transcript_layout_failed")
                        : m_lastFailureReason);
    }
    if (!ensureAtlasUploaded(commandBuffer, cachedLayout->atlas)) {
        return fail(m_lastFailureReason.isEmpty()
                        ? QStringLiteral("transcript_atlas_upload_failed")
                        : m_lastFailureReason);
    }
    return true;
}

bool VulkanTextRenderer::prepareTitleOverlayAtlas(VkCommandBuffer commandBuffer,
                                                  const QSize& outputSize,
                                                  const EvaluatedTitle& title)
{
    m_lastFailureReason.clear();
    if (!isReady() || commandBuffer == VK_NULL_HANDLE || !outputSize.isValid() ||
        !title.valid || title.text.trimmed().isEmpty()) {
        return fail(QStringLiteral("title_prepare_invalid_state"));
    }
    const TitleLayoutCache* layout = titleOverlayLayout(outputSize, title);
    if (!layout) {
        return fail(m_lastFailureReason.isEmpty()
                        ? QStringLiteral("title_layout_failed")
                        : m_lastFailureReason);
    }
    if (!ensureAtlasUploaded(commandBuffer, layout->atlas)) {
        return fail(m_lastFailureReason.isEmpty()
                        ? QStringLiteral("title_atlas_upload_failed")
                        : m_lastFailureReason);
    }
    return true;
}

bool VulkanTextRenderer::drawTitleOverlay3D(VkCommandBuffer commandBuffer,
                                            const QSize& swapSize,
                                            const QSize& outputSize,
                                            const QRectF& outputTargetRect,
                                            const EvaluatedTitle& title)
{
    m_lastFailureReason.clear();
    if (!isReady() || commandBuffer == VK_NULL_HANDLE || !swapSize.isValid() ||
        !outputSize.isValid() || outputTargetRect.isEmpty() || !title.valid ||
        title.text.trimmed().isEmpty() || title.opacity <= 0.001) {
        return fail(QStringLiteral("title_draw_invalid_state"));
    }
    const TitleLayoutCache* layout = titleOverlayLayout(outputSize, title);
    if (!layout || !ensureAtlasUploaded(commandBuffer, layout->atlas)) {
        return fail(m_lastFailureReason.isEmpty()
                        ? QStringLiteral("title_draw_atlas_unavailable")
                        : m_lastFailureReason);
    }

    const qreal scaleX = outputTargetRect.width() / qMax<qreal>(1.0, outputSize.width());
    const qreal scaleY = outputTargetRect.height() / qMax<qreal>(1.0, outputSize.height());
    auto mapPoint = [&](const QPointF& point) {
        return QPointF(outputTargetRect.left() + point.x() * scaleX,
                       outputTargetRect.top() + point.y() * scaleY);
    };
    auto mapRect = [&](const QRectF& rect) {
        return QRectF(outputTargetRect.left() + rect.left() * scaleX,
                      outputTargetRect.top() + rect.top() * scaleY,
                      rect.width() * scaleX,
                      rect.height() * scaleY);
    };

    const QPointF mappedCenter = mapPoint(layout->center);
    for (const TranscriptBackground& background : layout->backgrounds) {
        float mvp[16];
        mvpForTitle3DRect(mapRect(background.rect), mappedCenter, swapSize, title, mvp);
        drawGlyphWithMvp(commandBuffer,
                         swapSize,
                         mvp,
                         background.uv.isEmpty() ? layout->atlas.solidUv : background.uv,
                         background.color,
                         background.materialStyle,
                         background.patternScale,
                         background.patternUv);
    }
    const bool meshDrawn = title.vulkan3DExtrudeEnabled &&
        title.textExtrudeMode != TextExtrudeMode::None &&
        drawTitleMesh(commandBuffer, swapSize, outputSize, outputTargetRect, title, *layout);
    if (!meshDrawn && title.vulkan3DExtrudeEnabled && title.vulkan3DExtrudeDepth > 0.001) {
        // Build a continuous sidewall. Large gaps between a small number of
        // translated glyph copies read as repeated text rather than solid
        // geometry, especially at the maximum depth.
        const bool stackedCopies = title.textExtrudeMode == TextExtrudeMode::StackedCopies;
        const QVector<qreal> layerOffsets = textExtrusionLayerOffsets(
            title.textExtrudeMode, title.vulkan3DExtrudeDepth);
        const int layers = layerOffsets.size();
        const int bevelLayers = qBound(1, qRound(1.0 + title.vulkan3DBevelScale * 2.0), qMax(1, layers));
        for (int layerIndex = 0; layerIndex < layers; ++layerIndex) {
            const int layer = layers - layerIndex;
            const qreal faceT = static_cast<qreal>(layer) / qMax<qreal>(1.0, layers);
            const qreal layerOffset = layerOffsets.at(layerIndex);
            for (const LaidOutGlyph& glyph : layout->glyphs) {
                if (!glyph.extrudable) continue;
                QColor side = glyph.color;
                // Keep the side wall opaque enough to read as geometry. The
                // previous translucent near-black copies disappeared into the
                // dark lower-third window and looked like an ordinary shadow.
                const qreal light = qBound<qreal>(0.34,
                    0.62 - faceT * 0.20 + title.vulkan3DBevelScale * 0.08,
                    0.76);
                side.setRedF(qBound<qreal>(0.0, side.redF() * light, 1.0));
                side.setGreenF(qBound<qreal>(0.0, side.greenF() * light, 1.0));
                side.setBlueF(qBound<qreal>(0.0, side.blueF() * qMin<qreal>(0.82, light + 0.08), 1.0));
                side.setAlphaF(qBound<qreal>(0.58,
                                             side.alphaF() * (0.76 + title.vulkan3DBevelScale * 0.10),
                                             0.96));
                const QRectF extrudedRect =
                    mapRect(glyph.rect.translated(layerOffset, layerOffset * 0.58));
                const QRectF sideUv = !stackedCopies &&
                        layer <= bevelLayers && !glyph.erodedUv.isEmpty()
                    ? glyph.erodedUv : glyph.uv;
                float mvp[16];
                mvpForTitle3DRect(extrudedRect, mappedCenter, swapSize, title, mvp);
                drawGlyphWithMvp(commandBuffer,
                                 swapSize,
                                 mvp,
                                 sideUv,
                                 side,
                                 0,
                                 1.0,
                                 layout->atlas.solidUv);
            }
        }
    }
    for (const LaidOutGlyph& glyph : layout->glyphs) {
        if (meshDrawn && glyph.extrudable) continue;
        float mvp[16];
        mvpForTitle3DRect(mapRect(glyph.rect), mappedCenter, swapSize, title, mvp);
        drawGlyphWithMvp(commandBuffer,
                         swapSize,
                         mvp,
                         glyph.uv,
                         glyph.color,
                         glyph.materialStyle,
                         glyph.patternScale,
                         glyph.patternUv);
    }
    return true;
}
QByteArray erodeGlyphAlpha(const FT_Bitmap& bitmap, int radius)
{
    const int width = static_cast<int>(bitmap.width);
    const int height = static_cast<int>(bitmap.rows);
    if (width <= 0 || height <= 0) return {};
    QByteArray current(width * height, char(0));
    for (int y = 0; y < height; ++y) {
        const unsigned char* row = bitmap.buffer + y * bitmap.pitch;
        for (int x = 0; x < width; ++x) current[y * width + x] = static_cast<char>(row[x]);
    }
    QByteArray next(current.size(), char(0));
    for (int pass = 0; pass < qMax(0, radius); ++pass) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                unsigned char minimum = 255;
                for (int oy = -1; oy <= 1; ++oy) {
                    for (int ox = -1; ox <= 1; ++ox) {
                        const int sx = x + ox;
                        const int sy = y + oy;
                        const unsigned char sample = (sx < 0 || sy < 0 || sx >= width || sy >= height)
                            ? 0 : static_cast<unsigned char>(current.at(sy * width + sx));
                        minimum = qMin(minimum, sample);
                    }
                }
                next[y * width + x] = static_cast<char>(minimum);
            }
        }
        current.swap(next);
    }
    return current;
}
