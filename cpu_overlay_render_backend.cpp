#include "cpu_overlay_render_backend.h"

#include "render_internal.h"
#include "titles.h"

#include <QFileInfo>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

#include <cmath>
#include <memory>

namespace render_detail {

TitleLayoutMetrics measureOverlayTitleLayout(const EvaluatedTitle& title, qreal fontScale);

QImage OverlayImage::asQImageView() const
{
    if (isNull()) {
        return QImage();
    }
    return QImage(reinterpret_cast<const uchar*>(rgbaPremultiplied.constData()),
                  width,
                  height,
                  width * 4,
                  QImage::Format_RGBA8888_Premultiplied);
}

namespace {

OverlayImage makeOverlayImage(const QSize& size)
{
    if (!size.isValid()) {
        return {};
    }
    OverlayImage overlay;
    overlay.width = size.width();
    overlay.height = size.height();
    overlay.rgbaPremultiplied = QByteArray(overlay.width * overlay.height * 4, '\0');
    return overlay;
}

QString transcriptSectionsCacheKeyForClip(const TimelineClip& clip)
{
    const QString transcriptPath = activeTranscriptPathForClip(clip);
    const QFileInfo info(transcriptPath);
    const qint64 mtimeMs = info.exists() ? info.lastModified().toMSecsSinceEpoch() : -1;
    return clip.filePath + QLatin1Char('|') + transcriptPath + QLatin1Char('|') +
           QString::number(mtimeMs);
}

struct FtLibraryHolder {
    FT_Library library = nullptr;

    FtLibraryHolder()
    {
        FT_Init_FreeType(&library);
    }

    ~FtLibraryHolder()
    {
        if (library) {
            FT_Done_FreeType(library);
        }
    }
};

struct FontMetricsData {
    qreal ascender = 0.0;
    qreal descender = 0.0;
    qreal lineHeight = 0.0;
    qreal height = 0.0;
};

struct TranscriptRenderLineMetrics {
    QVector<qreal> wordWidths;
    qreal totalWidth = 0.0;
};

struct SpeakerLabelLine {
    QString text;
    bool name = false;
};

QColor colorWithOpacity(QColor color, qreal opacity)
{
    if (!color.isValid()) {
        color = QColor(Qt::black);
    }
    color.setAlphaF(qBound<qreal>(0.0, opacity, 1.0));
    return color;
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
            const QPointF offset(std::round(std::cos(angle) * ringRadius),
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

FtLibraryHolder& ftLibraryHolder()
{
    static FtLibraryHolder holder;
    return holder;
}

QString fallbackFontPath()
{
    static const QStringList candidates = {
        QStringLiteral("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
        QStringLiteral("/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf"),
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return QString();
}

QString resolveFontPath(const QString& family, bool bold, bool italic)
{
    static bool fcInitialized = FcInit();
    Q_UNUSED(fcInitialized);

    FcPattern* pattern = FcPatternCreate();
    if (!pattern) {
        return fallbackFontPath();
    }

    const QByteArray familyUtf8 = (family.isEmpty() ? kDefaultFontFamily : family).toUtf8();
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(familyUtf8.constData()));
    FcPatternAddInteger(pattern, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
    FcPatternAddInteger(pattern, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);

    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pattern, &result);
    QString resolved;
    if (match) {
        FcChar8* file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
            resolved = QString::fromUtf8(reinterpret_cast<const char*>(file));
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pattern);

    return resolved.isEmpty() ? fallbackFontPath() : resolved;
}

bool loadFontFace(const QString& family,
                  bool bold,
                  bool italic,
                  int pixelSize,
                  FT_Face* face,
                  FontMetricsData* metrics)
{
    if (!face) {
        return false;
    }
    *face = nullptr;
    const QString fontPath = resolveFontPath(family, bold, italic);
    if (fontPath.isEmpty()) {
        return false;
    }
    FT_Library library = ftLibraryHolder().library;
    if (!library) {
        return false;
    }
    const QByteArray pathUtf8 = fontPath.toUtf8();
    if (FT_New_Face(library, pathUtf8.constData(), 0, face) != 0 || !*face) {
        return false;
    }
    if (FT_Select_Charmap(*face, FT_ENCODING_UNICODE) != 0) {
        FT_Done_Face(*face);
        *face = nullptr;
        return false;
    }
    if (FT_Set_Pixel_Sizes(*face, 0, qMax(1, pixelSize)) != 0) {
        FT_Done_Face(*face);
        *face = nullptr;
        return false;
    }
    if (metrics) {
        metrics->ascender = static_cast<qreal>((*face)->size->metrics.ascender) / 64.0;
        metrics->descender = static_cast<qreal>(-(*face)->size->metrics.descender) / 64.0;
        metrics->lineHeight = static_cast<qreal>((*face)->size->metrics.height) / 64.0;
        metrics->height = metrics->ascender + metrics->descender;
    }
    return true;
}

void blendPixel(OverlayImage* image, int x, int y, const QColor& src, int alpha)
{
    if (!image || image->isNull() || x < 0 || y < 0 || x >= image->width || y >= image->height || alpha <= 0) {
        return;
    }
    const int offset = ((y * image->width) + x) * 4;
    const uchar* bits = reinterpret_cast<const uchar*>(image->rgbaPremultiplied.constData());
    const QColor dst = QColor::fromRgb(bits[offset + 0], bits[offset + 1], bits[offset + 2], bits[offset + 3]);
    const qreal sa = qBound<qreal>(0.0, static_cast<qreal>(alpha) / 255.0, 1.0);
    const qreal da = dst.alphaF();
    const qreal outA = sa + (da * (1.0 - sa));
    char* outBits = image->rgbaPremultiplied.data();
    if (outA <= 0.0) {
        outBits[offset + 0] = 0;
        outBits[offset + 1] = 0;
        outBits[offset + 2] = 0;
        outBits[offset + 3] = 0;
        return;
    }
    const qreal r = ((src.redF() * sa) + (dst.redF() * da * (1.0 - sa))) / outA;
    const qreal g = ((src.greenF() * sa) + (dst.greenF() * da * (1.0 - sa))) / outA;
    const qreal b = ((src.blueF() * sa) + (dst.blueF() * da * (1.0 - sa))) / outA;
    const QColor blended = QColor::fromRgbF(r, g, b, outA);
    outBits[offset + 0] = static_cast<char>(blended.red());
    outBits[offset + 1] = static_cast<char>(blended.green());
    outBits[offset + 2] = static_cast<char>(blended.blue());
    outBits[offset + 3] = static_cast<char>(blended.alpha());
}

void fillRectSoftware(OverlayImage* image, const QRectF& rect, const QColor& color)
{
    if (!image || image->isNull() || rect.isEmpty() || color.alpha() <= 0) {
        return;
    }
    const int left = qMax(0, static_cast<int>(std::floor(rect.left())));
    const int top = qMax(0, static_cast<int>(std::floor(rect.top())));
    const int right = qMin(image->width - 1, static_cast<int>(std::ceil(rect.right())));
    const int bottom = qMin(image->height - 1, static_cast<int>(std::ceil(rect.bottom())));
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            blendPixel(image, x, y, color, color.alpha());
        }
    }
}

void fillRoundedRectSoftware(OverlayImage* image, const QRectF& rect, qreal radius, const QColor& color)
{
    if (!image || image->isNull() || rect.isEmpty() || color.alpha() <= 0) {
        return;
    }
    const int left = qMax(0, static_cast<int>(std::floor(rect.left())));
    const int top = qMax(0, static_cast<int>(std::floor(rect.top())));
    const int right = qMin(image->width - 1, static_cast<int>(std::ceil(rect.right())));
    const int bottom = qMin(image->height - 1, static_cast<int>(std::ceil(rect.bottom())));
    const qreal r = qMax<qreal>(0.0, radius);
    const qreal cxLeft = rect.left() + r;
    const qreal cxRight = rect.right() - r;
    const qreal cyTop = rect.top() + r;
    const qreal cyBottom = rect.bottom() - r;
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            const qreal px = static_cast<qreal>(x) + 0.5;
            const qreal py = static_cast<qreal>(y) + 0.5;
            bool inside = true;
            if (r > 0.0 && px < cxLeft && py < cyTop) {
                inside = std::hypot(px - cxLeft, py - cyTop) <= r;
            } else if (r > 0.0 && px > cxRight && py < cyTop) {
                inside = std::hypot(px - cxRight, py - cyTop) <= r;
            } else if (r > 0.0 && px < cxLeft && py > cyBottom) {
                inside = std::hypot(px - cxLeft, py - cyBottom) <= r;
            } else if (r > 0.0 && px > cxRight && py > cyBottom) {
                inside = std::hypot(px - cxRight, py - cyBottom) <= r;
            }
            if (inside) {
                blendPixel(image, x, y, color, color.alpha());
            }
        }
    }
}

void strokeRectSoftware(OverlayImage* image, const QRectF& rect, qreal width, const QColor& color)
{
    if (!image || image->isNull() || rect.isEmpty() || color.alpha() <= 0 || width <= 0.0) {
        return;
    }
    const qreal half = width * 0.5;
    fillRectSoftware(image, QRectF(rect.left() - half, rect.top() - half, rect.width() + width, width), color);
    fillRectSoftware(image, QRectF(rect.left() - half, rect.bottom() - half, rect.width() + width, width), color);
    fillRectSoftware(image, QRectF(rect.left() - half, rect.top() + half, width, qMax<qreal>(0.0, rect.height() - width)), color);
    fillRectSoftware(image, QRectF(rect.right() - half, rect.top() + half, width, qMax<qreal>(0.0, rect.height() - width)), color);
}

qreal measureTextWidth(FT_Face face, const QString& text)
{
    if (!face || text.isEmpty()) {
        return 0.0;
    }
    qreal width = 0.0;
    FT_UInt previousGlyph = 0;
    const QList<uint> codepoints = text.toUcs4();
    for (uint codepoint : codepoints) {
        const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
        if (glyphIndex == 0 && codepoint != ' ') {
            continue;
        }
        if (FT_HAS_KERNING(face) && previousGlyph && glyphIndex) {
            FT_Vector delta{};
            FT_Get_Kerning(face, previousGlyph, glyphIndex, FT_KERNING_DEFAULT, &delta);
            width += static_cast<qreal>(delta.x) / 64.0;
        }
        if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_DEFAULT) != 0) {
            previousGlyph = 0;
            continue;
        }
        width += static_cast<qreal>(face->glyph->advance.x) / 64.0;
        previousGlyph = glyphIndex;
    }
    return width;
}

qreal drawGlyphRun(OverlayImage* image,
                   FT_Face face,
                   qreal x,
                   qreal baseline,
                   const QString& text,
                   const QColor& color)
{
    if (!image || image->isNull() || !face || text.isEmpty() || color.alpha() <= 0) {
        return 0.0;
    }
    qreal cursor = x;
    FT_UInt previousGlyph = 0;
    const QList<uint> codepoints = text.toUcs4();
    for (uint codepoint : codepoints) {
        const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
        if (FT_HAS_KERNING(face) && previousGlyph && glyphIndex) {
            FT_Vector delta{};
            FT_Get_Kerning(face, previousGlyph, glyphIndex, FT_KERNING_DEFAULT, &delta);
            cursor += static_cast<qreal>(delta.x) / 64.0;
        }
        if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_DEFAULT) != 0) {
            previousGlyph = 0;
            continue;
        }
        if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
            cursor += static_cast<qreal>(face->glyph->advance.x) / 64.0;
            previousGlyph = glyphIndex;
            continue;
        }
        const FT_GlyphSlot glyph = face->glyph;
        const int glyphX = static_cast<int>(std::floor(cursor + glyph->bitmap_left));
        const int glyphY = static_cast<int>(std::floor(baseline - glyph->bitmap_top));
        const FT_Bitmap& bitmap = glyph->bitmap;
        for (int row = 0; row < bitmap.rows; ++row) {
            const unsigned char* src = bitmap.buffer + (row * bitmap.pitch);
            for (int col = 0; col < bitmap.width; ++col) {
                const int alpha = static_cast<int>(src[col]);
                if (alpha > 0) {
                    blendPixel(image, glyphX + col, glyphY + row, color, (alpha * color.alpha()) / 255);
                }
            }
        }
        cursor += static_cast<qreal>(glyph->advance.x) / 64.0;
        previousGlyph = glyphIndex;
    }
    return cursor - x;
}

TranscriptRenderLineMetrics measureTranscriptLine(const TranscriptOverlayLine& line,
                                                  FT_Face face)
{
    TranscriptRenderLineMetrics result;
    result.wordWidths.reserve(line.words.size());
    const qreal spaceWidth = measureTextWidth(face, QStringLiteral(" "));
    for (int i = 0; i < line.words.size(); ++i) {
        const qreal width = measureTextWidth(face, line.words.at(i));
        result.wordWidths.push_back(width);
        result.totalWidth += width;
        if (i + 1 < line.words.size()) {
            result.totalWidth += spaceWidth;
        }
    }
    return result;
}

QStringList wrapTextToWidth(FT_Face face, const QString& text, qreal maxWidth)
{
    QStringList result;
    const QString trimmed = text.simplified();
    if (!face || trimmed.isEmpty() || maxWidth <= 0.0) {
        return result;
    }
    const QStringList words = trimmed.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    auto fitToken = [face, maxWidth](const QString& token) {
        if (measureTextWidth(face, token) <= maxWidth) {
            return token;
        }
        QString clipped = token;
        const QString ellipsis = QStringLiteral("...");
        while (!clipped.isEmpty() &&
               measureTextWidth(face, clipped + ellipsis) > maxWidth) {
            clipped.chop(1);
        }
        return clipped.isEmpty() ? ellipsis : clipped + ellipsis;
    };
    QString current;
    for (const QString& word : words) {
        const QString fittedWord = fitToken(word);
        const QString candidate = current.isEmpty() ? fittedWord : current + QLatin1Char(' ') + fittedWord;
        if (measureTextWidth(face, candidate) <= maxWidth || current.isEmpty()) {
            current = candidate;
            continue;
        }
        result.push_back(current);
        current = fittedWord;
    }
    if (!current.isEmpty()) {
        result.push_back(current);
    }
    return result;
}

OverlayImage renderSpeakerLabelOverlayImageSoftware(const QSize& imageSize,
                                                    const SpeakerLabelOverlaySpec& spec)
{
    if (!imageSize.isValid() ||
        (!spec.showName && !spec.showOrganization)) {
        return {};
    }

    OverlayImage overlay = makeOverlayImage(imageSize);
    const QString name = spec.name.trimmed();
    const QString organization = spec.organization.trimmed();
    if ((spec.showName && name.isEmpty()) &&
        (spec.showOrganization && organization.isEmpty())) {
        return overlay;
    }

    const qreal base = qMax<qreal>(1.0, qMin(imageSize.width(), imageSize.height()));
    const qreal nameScale = qBound<qreal>(0.25, spec.nameTextScale, 3.0);
    const qreal orgScale = qBound<qreal>(0.25, spec.organizationTextScale, 3.0);
    const int namePixelSize = qBound(8, static_cast<int>(std::round(base * 0.042 * nameScale)), 160);
    const int orgPixelSize = qBound(8, static_cast<int>(std::round(base * 0.030 * orgScale)), 140);
    FT_Face nameFace = nullptr;
    FontMetricsData nameMetrics;
    if (!loadFontFace(spec.fontFamily, true, false, namePixelSize, &nameFace, &nameMetrics)) {
        return overlay;
    }
    auto nameFaceGuard = std::unique_ptr<std::remove_pointer_t<FT_Face>, void(*)(FT_Face)>(nameFace, [](FT_Face value) {
        if (value) {
            FT_Done_Face(value);
        }
    });

    FT_Face orgFace = nullptr;
    FontMetricsData orgMetrics;
    if (!loadFontFace(spec.fontFamily, false, false, orgPixelSize, &orgFace, &orgMetrics)) {
        return overlay;
    }
    auto orgFaceGuard = std::unique_ptr<std::remove_pointer_t<FT_Face>, void(*)(FT_Face)>(orgFace, [](FT_Face value) {
        if (value) {
            FT_Done_Face(value);
        }
    });

    const qreal maxTextWidth = qMax<qreal>(180.0, imageSize.width() * 0.72);
    QVector<SpeakerLabelLine> lines;
    if (spec.showName && !name.isEmpty()) {
        const QStringList wrapped = wrapTextToWidth(nameFace, name, maxTextWidth);
        for (const QString& line : wrapped) {
            lines.push_back(SpeakerLabelLine{line, true});
        }
    }
    if (spec.showOrganization && !organization.isEmpty()) {
        const QStringList wrapped = wrapTextToWidth(orgFace, organization, maxTextWidth);
        for (const QString& line : wrapped) {
            lines.push_back(SpeakerLabelLine{line, false});
        }
    }
    if (lines.isEmpty()) {
        return overlay;
    }

    const qreal paddingX = qMax<qreal>(18.0, base * 0.028);
    const qreal paddingY = qMax<qreal>(10.0, base * 0.018);
    const qreal lineGap = qMax<qreal>(4.0, base * 0.008);
    const auto drawBlock = [&](const QVector<SpeakerLabelLine>& blockLines, qreal verticalPosition) {
        if (blockLines.isEmpty()) {
            return;
        }
        qreal contentWidth = 0.0;
        qreal contentHeight = 0.0;
        for (int i = 0; i < blockLines.size(); ++i) {
            const SpeakerLabelLine& line = blockLines.at(i);
            contentWidth = qMax(contentWidth, measureTextWidth(line.name ? nameFace : orgFace, line.text));
            contentHeight += line.name ? nameMetrics.lineHeight : orgMetrics.lineHeight;
            if (i + 1 < blockLines.size()) {
                contentHeight += lineGap;
            }
        }

        const qreal cardWidth = qMin<qreal>(
            imageSize.width() - 32.0,
            qMax<qreal>(220.0, contentWidth + (paddingX * 2.0)));
        const qreal cardHeight = contentHeight + (paddingY * 2.0);
        const qreal clampedCenterY = qBound<qreal>(
            cardHeight * 0.5,
            imageSize.height() * qBound<qreal>(0.0, verticalPosition, 1.0),
            imageSize.height() - (cardHeight * 0.5));
        const QRectF cardRect((imageSize.width() - cardWidth) * 0.5,
                              clampedCenterY - (cardHeight * 0.5),
                              cardWidth,
                              cardHeight);
        if (!cardRect.isValid()) {
            return;
        }

        const qreal radius = qBound<qreal>(0.0, spec.backgroundCornerRadius, 128.0);
        fillRoundedRectSoftware(&overlay, cardRect, radius, spec.backgroundColor);
        const qreal borderWidth = qBound<qreal>(0.0, spec.borderWidth, 16.0);
        if (borderWidth > 0.0) {
            strokeRectSoftware(
                &overlay,
                cardRect.adjusted(borderWidth * 0.5,
                                  borderWidth * 0.5,
                                  -borderWidth * 0.5,
                                  -borderWidth * 0.5),
                borderWidth,
                spec.borderColor);
        }

        qreal y = cardRect.top() + paddingY;
        for (int i = 0; i < blockLines.size(); ++i) {
            const SpeakerLabelLine& line = blockLines.at(i);
            FT_Face face = line.name ? nameFace : orgFace;
            const FontMetricsData& metrics = line.name ? nameMetrics : orgMetrics;
            const qreal width = measureTextWidth(face, line.text);
            const qreal x = cardRect.left() + qMax<qreal>(paddingX, (cardRect.width() - width) * 0.5);
            const qreal baseline = y + metrics.ascender;
            if (spec.showShadow) {
                drawGlyphRun(&overlay, face, x + 2.0, baseline + 2.0, line.text, spec.shadowColor);
            }
            drawGlyphRun(&overlay,
                         face,
                         x,
                         baseline,
                         line.text,
                         line.name ? spec.nameColor : spec.organizationColor);
            y += metrics.lineHeight + lineGap;
        }
    };

    QVector<SpeakerLabelLine> nameLines;
    QVector<SpeakerLabelLine> organizationLines;
    for (const SpeakerLabelLine& line : lines) {
        (line.name ? nameLines : organizationLines).push_back(line);
    }
    drawBlock(nameLines, spec.nameVerticalPosition);
    drawBlock(organizationLines, spec.organizationVerticalPosition);

    return overlay;
}

OverlayImage renderTitleOverlayImageSoftware(const QSize& imageSize,
                                             const EvaluatedTitle& title,
                                             const QSize& outputSize)
{
    if (!imageSize.isValid()) {
        return {};
    }
    OverlayImage titleImage = makeOverlayImage(imageSize);
    if (!title.valid || title.text.isEmpty() || title.opacity <= 0.001) {
        return titleImage;
    }

    const qreal scaleX = outputSize.width() > 0
        ? static_cast<qreal>(imageSize.width()) / outputSize.width()
        : 1.0;
    const qreal scaleY = outputSize.height() > 0
        ? static_cast<qreal>(imageSize.height()) / outputSize.height()
        : 1.0;
    const qreal fontScale = qMin(scaleX, scaleY);

    FT_Face face = nullptr;
    FontMetricsData metrics;
    const int pixelSize = qMax(1, static_cast<int>(std::round(title.fontSize * fontScale)));
    if (!loadFontFace(title.fontFamily, title.bold, title.italic, pixelSize, &face, &metrics)) {
        return titleImage;
    }
    auto faceGuard = std::unique_ptr<std::remove_pointer_t<FT_Face>, void(*)(FT_Face)>(face, [](FT_Face value) {
        if (value) {
            FT_Done_Face(value);
        }
    });

    QColor textColor = title.color;
    textColor.setAlphaF(title.opacity);

    const qreal centerX = (static_cast<qreal>(imageSize.width()) - 1.0) * 0.5 + title.x * scaleX;
    const qreal centerY = (static_cast<qreal>(imageSize.height()) - 1.0) * 0.5 + title.y * scaleY;
    const TitleLayoutMetrics layoutMetrics = measureOverlayTitleLayout(title, fontScale);
    const QStringList lines = title.text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    const qreal requestedWindowContentWidth =
        title.windowWidth > 0.0 ? qMax<qreal>(0.0, title.windowWidth * fontScale - (title.windowPadding * fontScale * 2.0)) : 0.0;
    const qreal maxWidth = qMax(layoutMetrics.width, requestedWindowContentWidth);
    const qreal totalHeight = layoutMetrics.height;
    const qreal topY = centerY - (totalHeight / 2.0);
    const qreal windowPaddingPx = qMax<qreal>(0.0, title.windowPadding * fontScale);

    const QRectF windowRect(centerX - (maxWidth / 2.0) - windowPaddingPx,
                            topY - windowPaddingPx,
                            maxWidth + (windowPaddingPx * 2.0),
                            totalHeight + (windowPaddingPx * 2.0));

    if (title.windowEnabled) {
        QColor windowColor = title.windowColor;
        windowColor.setAlphaF(qBound<qreal>(0.0, title.opacity * title.windowOpacity, 1.0));
        fillRectSoftware(&titleImage, windowRect, windowColor);
    }

    if (title.windowFrameEnabled) {
        const qreal frameGapPx = qMax<qreal>(0.0, title.windowFrameGap * fontScale);
        const qreal frameWidthPx = qMax<qreal>(1.0, title.windowFrameWidth * fontScale);
        QColor frameColor = title.windowFrameColor;
        frameColor.setAlphaF(qBound<qreal>(0.0, title.opacity * title.windowFrameOpacity, 1.0));
        strokeRectSoftware(&titleImage,
                           windowRect.adjusted(-frameGapPx, -frameGapPx, frameGapPx, frameGapPx),
                           frameWidthPx,
                           frameColor);
    }

    if (title.dropShadowEnabled) {
        QColor shadowColor = title.dropShadowColor;
        shadowColor.setAlphaF(qBound<qreal>(0.0, title.opacity * title.dropShadowOpacity, 1.0));
        for (int i = 0; i < lines.size(); ++i) {
            const QString& line = lines.at(i);
            const qreal lineWidth = measureTextWidth(face, line);
            const qreal x = centerX - (lineWidth / 2.0) + (title.dropShadowOffsetX * scaleX);
            const qreal y = topY + (i * metrics.lineHeight) + metrics.ascender + (title.dropShadowOffsetY * scaleY);
            drawGlyphRun(&titleImage, face, x, y, line, shadowColor);
        }
    }

    for (int i = 0; i < lines.size(); ++i) {
        const QString& line = lines.at(i);
        const qreal lineWidth = measureTextWidth(face, line);
        const qreal x = centerX - (lineWidth / 2.0);
        const qreal y = topY + (i * metrics.lineHeight) + metrics.ascender;
        drawGlyphRun(&titleImage, face, x, y, line, textColor);
    }

    return titleImage;
}

OverlayImage renderTranscriptOverlayImageSoftware(const QSize& imageSize,
                                                  const RenderRequest& request,
                                                  int64_t timelineFrame,
                                                  const QVector<TimelineClip>& orderedClips,
                                                  QHash<QString, QVector<TranscriptSection>>& transcriptCache)
{
    if (!imageSize.isValid()) {
        return {};
    }
    OverlayImage canvas = makeOverlayImage(imageSize);

    for (const TimelineClip& clip : orderedClips) {
        if (timelineFrame < clip.startFrame || timelineFrame >= clip.startFrame + clip.durationFrames) {
            continue;
        }
        const TranscriptOverlayLayout overlayLayout =
            transcriptOverlayLayoutForFrame(
                clip,
                timelineFrame,
                request.renderSyncMarkers,
                transcriptCache,
                TranscriptOverlayTiming{
                    request.transcriptPrependMs, request.transcriptPostpendMs, request.transcriptOffsetMs});
        if (overlayLayout.lines.isEmpty()) {
            continue;
        }

        const QString transcriptPath = activeTranscriptPathForClip(clip);
        const auto sectionsIt = transcriptCache.constFind(transcriptSectionsCacheKeyForClip(clip));
        const int64_t sourceFrame = transcriptFrameForClipAtTimelineSample(
            clip, frameToSamples(timelineFrame), request.renderSyncMarkers);
        const QVector<TranscriptSection> emptySections;
        const QVector<TranscriptSection>& sections =
            sectionsIt != transcriptCache.constEnd() ? sectionsIt.value() : emptySections;
        const QRectF bounds = transcriptOverlayRectInOutputSpace(
            clip, request.outputSize, transcriptPath, sections, sourceFrame);
        if (clip.transcriptOverlay.showBackground) {
            QColor backgroundColor = clip.transcriptOverlay.backgroundColor.isValid()
                ? clip.transcriptOverlay.backgroundColor
                : QColor(Qt::black);
            backgroundColor.setAlphaF(qBound<qreal>(0.0, clip.transcriptOverlay.backgroundOpacity, 1.0));
            fillRoundedRectSoftware(&canvas,
                                    bounds,
                                    qBound<qreal>(0.0, clip.transcriptOverlay.backgroundCornerRadius, 128.0),
                                    backgroundColor);
        }

        FT_Face bodyFace = nullptr;
        FontMetricsData bodyMetrics;
        if (!loadFontFace(clip.transcriptOverlay.fontFamily,
                          clip.transcriptOverlay.bold,
                          clip.transcriptOverlay.italic,
                          clip.transcriptOverlay.fontPointSize,
                          &bodyFace,
                          &bodyMetrics)) {
            continue;
        }
        auto bodyFaceGuard = std::unique_ptr<std::remove_pointer_t<FT_Face>, void(*)(FT_Face)>(bodyFace, [](FT_Face value) {
            if (value) {
                FT_Done_Face(value);
            }
        });

        FT_Face titleFace = nullptr;
        FontMetricsData titleMetrics;
        const int titlePixelSize = qMax(1, static_cast<int>(std::round(clip.transcriptOverlay.fontPointSize * 0.62)));
        const bool hasTitleFace =
            loadFontFace(clip.transcriptOverlay.fontFamily, true, false, titlePixelSize, &titleFace, &titleMetrics);
        auto titleFaceGuard = std::unique_ptr<std::remove_pointer_t<FT_Face>, void(*)(FT_Face)>(titleFace, [](FT_Face value) {
            if (value) {
                FT_Done_Face(value);
            }
        });

        const QRectF textBounds = bounds.adjusted(18.0, 14.0, -18.0, -14.0);
        const QColor textColor = clip.transcriptOverlay.textColor.isValid()
            ? clip.transcriptOverlay.textColor
            : QColor(Qt::white);
        const QColor shadowColor = colorWithOpacity(clip.transcriptOverlay.shadowColor,
                                                    clip.transcriptOverlay.shadowOpacity);
        const QColor highlightFillColor = clip.transcriptOverlay.highlightColor.isValid()
            ? clip.transcriptOverlay.highlightColor
            : QColor(QStringLiteral("#fff2a8"));
        const QColor highlightTextColor = clip.transcriptOverlay.highlightTextColor.isValid()
            ? clip.transcriptOverlay.highlightTextColor
            : QColor(QStringLiteral("#181818"));

        const QString speakerTitle = (clip.transcriptOverlay.showSpeakerTitle && hasTitleFace)
            ? transcriptSpeakerTitleForSourceFrame(
                  transcriptPath,
                  sections,
                  sourceFrame,
                  TranscriptOverlayTiming{
                      request.transcriptPrependMs, request.transcriptPostpendMs, request.transcriptOffsetMs}).trimmed()
            : QString();

        QVector<TranscriptRenderLineMetrics> lineMetrics;
        lineMetrics.reserve(overlayLayout.lines.size());
        qreal contentWidth = 0.0;
        for (const TranscriptOverlayLine& line : overlayLayout.lines) {
            TranscriptRenderLineMetrics metrics = measureTranscriptLine(line, bodyFace);
            contentWidth = qMax(contentWidth, metrics.totalWidth);
            lineMetrics.push_back(std::move(metrics));
        }
        const qreal speakerTitleWidth = speakerTitle.isEmpty() ? 0.0 : measureTextWidth(titleFace, speakerTitle);
        contentWidth = qMax(contentWidth, speakerTitleWidth);

        const qreal titleGap = speakerTitle.isEmpty() ? 0.0 : clip.transcriptOverlay.fontPointSize * 0.30;
        const qreal contentHeight =
            (speakerTitle.isEmpty() ? 0.0 : titleMetrics.lineHeight + titleGap) +
            (bodyMetrics.lineHeight * overlayLayout.lines.size());
        const qreal widthScale = contentWidth > textBounds.width() ? textBounds.width() / contentWidth : 1.0;
        const qreal heightScale = contentHeight > textBounds.height() ? textBounds.height() / contentHeight : 1.0;
        const qreal docScale = qMin(widthScale, heightScale);
        const qreal scaledContentHeight = contentHeight * docScale;
        qreal cursorY = textBounds.top() + qMax<qreal>(0.0, (textBounds.height() - scaledContentHeight) / 2.0);

        const qreal shadowOffsetX = clip.transcriptOverlay.shadowOffsetX * docScale;
        const qreal shadowOffsetY = clip.transcriptOverlay.shadowOffsetY * docScale;
        const QColor outlineColor = colorWithOpacity(clip.transcriptOverlay.textOutlineColor,
                                                     clip.transcriptOverlay.textOutlineOpacity);
        const QVector<QPointF> outlineOffsets = clip.transcriptOverlay.textOutlineEnabled
            ? dilationOffsets(qMax<qreal>(0.0, clip.transcriptOverlay.textOutlineWidth) * docScale)
            : QVector<QPointF>{};
        auto drawDilatedGlyphRun = [&](FT_Face face, qreal x, qreal baseline, const QString& text) {
            for (const QPointF& offset : outlineOffsets) {
                drawGlyphRun(&canvas, face, x + offset.x(), baseline + offset.y(), text, outlineColor);
            }
        };

        if (!speakerTitle.isEmpty() && titleFace) {
            const qreal titleWidth = speakerTitleWidth * docScale;
            const qreal titleX = textBounds.left() + qMax<qreal>(0.0, (textBounds.width() - titleWidth) / 2.0);
            const qreal titleBaseline = cursorY + (titleMetrics.ascender * docScale);
            if (clip.transcriptOverlay.showShadow) {
                drawGlyphRun(&canvas,
                             titleFace,
                             titleX + shadowOffsetX,
                             titleBaseline + shadowOffsetY,
                             speakerTitle,
                             shadowColor);
            }
            drawDilatedGlyphRun(titleFace, titleX, titleBaseline, speakerTitle);
            drawGlyphRun(&canvas, titleFace, titleX, titleBaseline, speakerTitle, textColor);
            cursorY += (titleMetrics.lineHeight + titleGap) * docScale;
        }

        const qreal bodySpaceWidth = measureTextWidth(bodyFace, QStringLiteral(" ")) * docScale;
        for (int lineIndex = 0; lineIndex < overlayLayout.lines.size(); ++lineIndex) {
            const TranscriptOverlayLine& line = overlayLayout.lines.at(lineIndex);
            const TranscriptRenderLineMetrics& metrics = lineMetrics.at(lineIndex);
            const qreal lineWidth = metrics.totalWidth * docScale;
            qreal cursorX = textBounds.left() + qMax<qreal>(0.0, (textBounds.width() - lineWidth) / 2.0);
            const qreal baseline = cursorY + (bodyMetrics.ascender * docScale);
            const qreal lineHeight = bodyMetrics.lineHeight * docScale;
            const qreal textHeight = bodyMetrics.height * docScale;
            const qreal ascent = bodyMetrics.ascender * docScale;
            for (int wordIndex = 0; wordIndex < line.words.size(); ++wordIndex) {
                const QString& word = line.words.at(wordIndex);
                const qreal wordWidth = metrics.wordWidths.at(wordIndex) * docScale;
                const bool active = clip.transcriptOverlay.highlightCurrentWord &&
                                    wordIndex == line.activeWord;
                if (active) {
                    const qreal padX = 0.18 * clip.transcriptOverlay.fontPointSize * docScale;
                    const qreal padY = 0.02 * clip.transcriptOverlay.fontPointSize * docScale;
                    const QRectF highlight(cursorX - padX,
                                           baseline - ascent - padY,
                                           wordWidth + (padX * 2.0),
                                           textHeight + (padY * 2.0));
                    fillRoundedRectSoftware(&canvas,
                                            highlight,
                                            0.28 * clip.transcriptOverlay.fontPointSize * docScale,
                                            highlightFillColor);
                }
                const QColor glyphColor = active ? highlightTextColor : textColor;
                if (clip.transcriptOverlay.showShadow && !active) {
                    drawGlyphRun(&canvas,
                                 bodyFace,
                                 cursorX + shadowOffsetX,
                                 baseline + shadowOffsetY,
                                 word,
                                 shadowColor);
                }
                drawDilatedGlyphRun(bodyFace, cursorX, baseline, word);
                drawGlyphRun(&canvas, bodyFace, cursorX, baseline, word, glyphColor);
                cursorX += wordWidth;
                if (wordIndex + 1 < line.words.size()) {
                    cursorX += bodySpaceWidth;
                }
            }
            cursorY += lineHeight;
        }
    }

    return canvas;
}

class QtOverlayRenderBackend final : public OverlayRenderBackend {
public:
    OverlayImage renderTitleOverlay(const QSize& imageSize,
                                    const EvaluatedTitle& title,
                                    const QSize& outputSize) override
    {
        return renderTitleOverlayImageSoftware(imageSize, title, outputSize);
    }

    OverlayImage renderTranscriptOverlay(const QSize& imageSize,
                                         const RenderRequest& request,
                                         int64_t timelineFrame,
                                         const QVector<TimelineClip>& orderedClips,
                                         QHash<QString, QVector<TranscriptSection>>& transcriptCache) override
    {
        return renderTranscriptOverlayImageSoftware(
            imageSize, request, timelineFrame, orderedClips, transcriptCache);
    }

    OverlayImage renderSpeakerLabelOverlay(const QSize& imageSize,
                                           const SpeakerLabelOverlaySpec& spec) override
    {
        return renderSpeakerLabelOverlayImageSoftware(imageSize, spec);
    }
};

OverlayRenderBackend* g_overlayRenderBackendOverride = nullptr;

QtOverlayRenderBackend& defaultOverlayRenderBackend()
{
    static QtOverlayRenderBackend backend;
    return backend;
}

} // namespace

OverlayRenderBackend& overlayRenderBackend()
{
    if (g_overlayRenderBackendOverride) {
        return *g_overlayRenderBackendOverride;
    }
    return defaultOverlayRenderBackend();
}

void setOverlayRenderBackendForTesting(OverlayRenderBackend* backend)
{
    g_overlayRenderBackendOverride = backend;
}

TitleLayoutMetrics measureOverlayTitleLayout(const EvaluatedTitle& title, qreal fontScale)
{
    TitleLayoutMetrics result;
    const QStringList lines = title.text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    result.lineCount = qMax(1, lines.size());
    FT_Face face = nullptr;
    FontMetricsData metrics;
    const int pixelSize = qMax(1, static_cast<int>(std::round(title.fontSize * qMax<qreal>(0.001, fontScale))));
    if (!loadFontFace(title.fontFamily, title.bold, title.italic, pixelSize, &face, &metrics)) {
        return result;
    }
    auto faceGuard = std::unique_ptr<std::remove_pointer_t<FT_Face>, void(*)(FT_Face)>(face, [](FT_Face value) {
        if (value) {
            FT_Done_Face(value);
        }
    });
    result.lineHeight = metrics.lineHeight;
    result.height = result.lineHeight * result.lineCount;
    for (const QString& line : lines) {
        result.width = qMax(result.width, measureTextWidth(face, line));
    }
    if (title.windowWidth > 0.0) {
        result.width = qMax(result.width,
                            title.windowWidth * qMax<qreal>(0.001, fontScale) -
                                (qMax<qreal>(0.0, title.windowPadding) * qMax<qreal>(0.001, fontScale) * 2.0));
    }
    return result;
}

} // namespace render_detail
