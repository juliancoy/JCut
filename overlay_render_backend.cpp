#include "overlay_render_backend.h"

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
    const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
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
    const qreal maxWidth = layoutMetrics.width;
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
            transcriptOverlayLayoutForFrame(clip, timelineFrame, request.renderSyncMarkers, transcriptCache);
        if (overlayLayout.lines.isEmpty()) {
            continue;
        }

        const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
        const auto sectionsIt = transcriptCache.constFind(transcriptSectionsCacheKeyForClip(clip));
        const int64_t sourceFrame = transcriptFrameForClipAtTimelineSample(
            clip, frameToSamples(timelineFrame), request.renderSyncMarkers);
        const QVector<TranscriptSection> emptySections;
        const QVector<TranscriptSection>& sections =
            sectionsIt != transcriptCache.constEnd() ? sectionsIt.value() : emptySections;
        const QRectF bounds = transcriptOverlayRectInOutputSpace(
            clip, request.outputSize, transcriptPath, sections, sourceFrame);
        if (clip.transcriptOverlay.showBackground) {
            fillRoundedRectSoftware(&canvas, bounds, 14.0, QColor(0, 0, 0, 120));
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
        const QColor shadowColor(0, 0, 0, 200);
        const QColor highlightFillColor(QStringLiteral("#fff2a8"));
        const QColor highlightTextColor(QStringLiteral("#181818"));

        const QString speakerTitle = (clip.transcriptOverlay.showSpeakerTitle && hasTitleFace)
            ? transcriptSpeakerTitleForSourceFrame(transcriptPath, sections, sourceFrame).trimmed()
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

        const qreal shadowOffset = 5.0 * docScale;

        if (!speakerTitle.isEmpty() && titleFace) {
            const qreal titleWidth = speakerTitleWidth * docScale;
            const qreal titleX = textBounds.left() + qMax<qreal>(0.0, (textBounds.width() - titleWidth) / 2.0);
            const qreal titleBaseline = cursorY + (titleMetrics.ascender * docScale);
            drawGlyphRun(&canvas, titleFace, titleX + shadowOffset, titleBaseline + shadowOffset, speakerTitle, shadowColor);
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
                const bool active = wordIndex == line.activeWord;
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
                drawGlyphRun(&canvas, bodyFace, cursorX + shadowOffset, baseline + shadowOffset, word, shadowColor);
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
    return result;
}

} // namespace render_detail
