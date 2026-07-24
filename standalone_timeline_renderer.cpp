#include "standalone_timeline_renderer.h"
#include "editor_grading_core.h"
#include "editor_runtime.h"
#include "editor_timeline_mapping_core.h"
#include "face_artifact_core.h"
#include "ffmpeg_hardware_device_core.h"
#include "image_sequence_directory.h"
#include "mask_frame_map_core.h"
#include "title_3d_projection_core.h"
#include "transcript_cut_session_core.h"
#include "transcript_overlay_core.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

namespace {

using jcut::EditorClip;
using jcut::EditorDocumentCore;
using jcut::DecoderPolicySettingsCore;
using jcut::EffectiveDecoderPolicyCore;
using jcut::FfmpegHardwareDeviceSetup;
using jcut::ImageSequenceDirectoryInfo;
using jcut::core::ImageBuffer;
using jcut::core::PixelFormat;
using jcut::core::SizeI;
using jcut::effectiveDecoderPolicyCore;
using jcut::createFfmpegHardwareDeviceForDecoder;
using jcut::ffmpegHardwareDeviceOrder;
using jcut::selectFfmpegHardwarePixelFormat;
using jcut::probeImageSequenceDirectory;

struct AvFormatContextDeleter {
    void operator()(AVFormatContext* value) const
    {
        if (value) {
            avformat_close_input(&value);
        }
    }
};

struct AvCodecContextDeleter {
    void operator()(AVCodecContext* value) const
    {
        avcodec_free_context(&value);
    }
};

struct AvFrameDeleter {
    void operator()(AVFrame* value) const
    {
        av_frame_free(&value);
    }
};

struct AvPacketDeleter {
    void operator()(AVPacket* value) const
    {
        av_packet_free(&value);
    }
};

struct SwsContextDeleter {
    void operator()(SwsContext* value) const
    {
        sws_freeContext(value);
    }
};

ImageBuffer makeSolidImage(
    SizeI size,
    std::uint8_t red,
    std::uint8_t green,
    std::uint8_t blue,
    std::uint8_t alpha = 255)
{
    ImageBuffer image;
    image.format = PixelFormat::Rgba8;
    image.size = size;
    image.strideBytes = size.width * 4;
    image.bytes.resize(static_cast<std::size_t>(image.strideBytes * size.height));
    for (int y = 0; y < size.height; ++y) {
        for (int x = 0; x < size.width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(y * image.strideBytes + x * 4);
            image.bytes[offset + 0] = red;
            image.bytes[offset + 1] = green;
            image.bytes[offset + 2] = blue;
            image.bytes[offset + 3] = alpha;
        }
    }
    return image;
}

void cropImageToAlphaBounds(
    ImageBuffer* image,
    int* offsetX,
    int* offsetY)
{
    if (offsetX) *offsetX = 0;
    if (offsetY) *offsetY = 0;
    if (!image || image->empty() ||
        image->format != PixelFormat::Rgba8) {
        return;
    }
    int left = image->size.width;
    int top = image->size.height;
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < image->size.height; ++y) {
        for (int x = 0; x < image->size.width; ++x) {
            const std::size_t alphaOffset =
                static_cast<std::size_t>(
                    y * image->strideBytes + x * 4 + 3);
            if (image->bytes[alphaOffset] == 0) {
                continue;
            }
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x);
            bottom = std::max(bottom, y);
        }
    }
    if (right < left || bottom < top) {
        *image = {};
        return;
    }
    if (left == 0 && top == 0 &&
        right == image->size.width - 1 &&
        bottom == image->size.height - 1) {
        return;
    }
    ImageBuffer cropped;
    cropped.format = PixelFormat::Rgba8;
    cropped.size = {
        right - left + 1,
        bottom - top + 1};
    cropped.strideBytes =
        cropped.size.width * 4;
    cropped.bytes.resize(
        static_cast<std::size_t>(
            cropped.strideBytes *
            cropped.size.height));
    for (int y = 0; y < cropped.size.height; ++y) {
        std::copy_n(
            image->bytes.begin() +
                static_cast<std::ptrdiff_t>(
                    (top + y) * image->strideBytes +
                    left * 4),
            cropped.strideBytes,
            cropped.bytes.begin() +
                static_cast<std::ptrdiff_t>(
                    y * cropped.strideBytes));
    }
    *image = std::move(cropped);
    if (offsetX) *offsetX = left;
    if (offsetY) *offsetY = top;
}

struct FreeTypeLibrary {
    FT_Library value = nullptr;
    FreeTypeLibrary() { FT_Init_FreeType(&value); }
    ~FreeTypeLibrary() { if (value) FT_Done_FreeType(value); }
};

FT_Library freeTypeLibrary()
{
    static FreeTypeLibrary library;
    return library.value;
}

std::string resolveFontPath(const jcut::EditorTitleKeyframe& title)
{
    static const bool initialized = FcInit();
    (void)initialized;
    FcPattern* pattern = FcPatternCreate();
    if (pattern) {
        const std::string family = title.fontFamily.empty()
            ? "DejaVu Sans" : title.fontFamily;
        FcPatternAddString(pattern, FC_FAMILY,
            reinterpret_cast<const FcChar8*>(family.c_str()));
        FcPatternAddInteger(pattern, FC_WEIGHT,
            title.bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
        FcPatternAddInteger(pattern, FC_SLANT,
            title.italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
        FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        FcResult result = FcResultNoMatch;
        FcPattern* match = FcFontMatch(nullptr, pattern, &result);
        std::string path;
        if (match) {
            FcChar8* file = nullptr;
            if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
                path = reinterpret_cast<const char*>(file);
            }
            FcPatternDestroy(match);
        }
        FcPatternDestroy(pattern);
        if (!path.empty()) return path;
    }
    for (const char* fallback : {
             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
             "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf"}) {
        if (std::filesystem::exists(fallback)) return fallback;
    }
    return {};
}

std::vector<std::uint32_t> utf8Codepoints(const std::string& text)
{
    std::vector<std::uint32_t> result;
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<std::uint8_t>(text[index]);
        std::uint32_t value = 0xfffd;
        std::size_t length = 1;
        if (first < 0x80) value = first;
        else if ((first & 0xe0) == 0xc0 && index + 1 < text.size()) { value = first & 0x1f; length = 2; }
        else if ((first & 0xf0) == 0xe0 && index + 2 < text.size()) { value = first & 0x0f; length = 3; }
        else if ((first & 0xf8) == 0xf0 && index + 3 < text.size()) { value = first & 0x07; length = 4; }
        bool valid = length > 1;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto continuation = static_cast<std::uint8_t>(text[index + offset]);
            if ((continuation & 0xc0) != 0x80) {
                valid = false;
                length = 1;
                value = 0xfffd;
                break;
            }
            value = (value << 6) | (continuation & 0x3f);
        }
        result.push_back(length == 1 && first < 0x80 ? first : (valid ? value : 0xfffd));
        index += length;
    }
    return result;
}

std::uint8_t parseHexByte(const std::string& value, std::size_t offset)
{
    try {
        return static_cast<std::uint8_t>(std::stoul(value.substr(offset, 2), nullptr, 16));
    } catch (...) {
        return 255;
    }
}

void blendTitlePixel(ImageBuffer* image, int x, int y,
                     std::uint8_t red, std::uint8_t green, std::uint8_t blue,
                     float sourceAlpha)
{
    if (!image || x < 0 || y < 0 || x >= image->size.width || y >= image->size.height) return;
    const std::size_t offset = static_cast<std::size_t>(y * image->strideBytes + x * 4);
    const float destinationAlpha = image->bytes[offset + 3] / 255.0f;
    const float outputAlpha = sourceAlpha + destinationAlpha * (1.0f - sourceAlpha);
    for (int channel = 0; channel < 3; ++channel) {
        const float color = channel == 0 ? red : (channel == 1 ? green : blue);
        const float premultiplied = color * sourceAlpha +
            image->bytes[offset + channel] * destinationAlpha * (1.0f - sourceAlpha);
        image->bytes[offset + channel] = static_cast<std::uint8_t>(
            outputAlpha > 0.0f ? premultiplied / outputAlpha + 0.5f : 0.0f);
    }
    image->bytes[offset + 3] = static_cast<std::uint8_t>(
        std::clamp(outputAlpha * 255.0f + 0.5f, 0.0f, 255.0f));
}

ImageBuffer renderTitleImage(const jcut::EditorTitleKeyframe& title, SizeI size,
                             const ImageBuffer* textPattern = nullptr,
                             const ImageBuffer* framePattern = nullptr,
                             const ImageBuffer* logo = nullptr)
{
    ImageBuffer image = makeSolidImage(size, 0, 0, 0);
    std::fill(image.bytes.begin(), image.bytes.end(), 0);
    if (title.text.empty() || title.opacity <= 0.0 || !freeTypeLibrary()) return image;
    const std::string fontPath = resolveFontPath(title);
    FT_Face face = nullptr;
    if (fontPath.empty() || FT_New_Face(freeTypeLibrary(), fontPath.c_str(), 0, &face) != 0 || !face) {
        return image;
    }
    struct FaceGuard { FT_Face value; ~FaceGuard() { if (value) FT_Done_Face(value); } } guard{face};
    FT_Select_Charmap(face, FT_ENCODING_UNICODE);

    std::vector<std::vector<std::uint32_t>> lines(1);
    for (const std::uint32_t codepoint : utf8Codepoints(title.text)) {
        if (codepoint == '\n') lines.emplace_back();
        else if (codepoint != '\r') lines.back().push_back(codepoint);
    }
    const auto setPixelSize = [&](double requested) {
        return FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(std::clamp(
            static_cast<int>(std::lround(requested)), 1, 1000))) == 0;
    };
    if (!setPixelSize(title.fontSize)) return image;
    const auto measureLine = [&](const std::vector<std::uint32_t>& line) {
        int width = 0;
        FT_UInt previousGlyph = 0;
        for (const std::uint32_t codepoint : line) {
            const FT_UInt glyph = FT_Get_Char_Index(face, codepoint);
            if (FT_HAS_KERNING(face) && previousGlyph && glyph) {
                FT_Vector kerning{};
                FT_Get_Kerning(face, previousGlyph, glyph, FT_KERNING_DEFAULT, &kerning);
                width += static_cast<int>(kerning.x >> 6);
            }
            if (FT_Load_Glyph(face, glyph, FT_LOAD_DEFAULT) == 0) {
                width += static_cast<int>(face->glyph->advance.x >> 6);
            }
            previousGlyph = glyph;
        }
        return width;
    };
    if (title.autoFitToOutput) {
        int maximumWidth = 0;
        for (const auto& line : lines) maximumWidth = std::max(maximumWidth, measureLine(line));
        const int initialLineHeight = std::max(
            1, static_cast<int>(face->size->metrics.height >> 6));
        const double widthScale = maximumWidth > 0
            ? size.width * 0.90 / maximumWidth : 1.0;
        const double heightScale = size.height * 0.86 /
            std::max(1, initialLineHeight * static_cast<int>(lines.size()));
        const double fitScale = std::min({1.0, widthScale, heightScale});
        if (!setPixelSize(title.fontSize * fitScale)) return image;
    }

    const int lineHeight = std::max(1, static_cast<int>(face->size->metrics.height >> 6));
    const int ascender = static_cast<int>(face->size->metrics.ascender >> 6);
    const int totalHeight = lineHeight * static_cast<int>(lines.size());
    std::vector<int> lineWidths;
    lineWidths.reserve(lines.size());
    int maximumWidth = 0;
    for (const auto& line : lines) {
        lineWidths.push_back(measureLine(line));
        maximumWidth = std::max(maximumWidth, lineWidths.back());
    }

    struct TitleColor { std::uint8_t r, g, b; double a; };
    const auto colorFor = [](const std::string& value, TitleColor fallback) {
        if (value.empty() || value[0] != '#') return fallback;
        if (value.size() == 7) {
            return TitleColor{parseHexByte(value, 1), parseHexByte(value, 3),
                              parseHexByte(value, 5), fallback.a};
        }
        if (value.size() == 9) {
            return TitleColor{parseHexByte(value, 3), parseHexByte(value, 5),
                              parseHexByte(value, 7), parseHexByte(value, 1) / 255.0};
        }
        return fallback;
    };
    const TitleColor textColor = colorFor(title.color, {255, 255, 255, 1.0});
    const double titleOpacity = std::clamp(title.opacity, 0.0, 1.0);
    const double centerX = (size.width - 1) * 0.5 + title.translationX;
    const double centerY = (size.height - 1) * 0.5 + title.translationY;
    const jcut::Title3DProjectionCore titleProjection{
        title.vulkan3DEnabled,
        title.vulkan3DYawDegrees,
        title.vulkan3DPitchDegrees,
        title.vulkan3DRollDegrees,
        title.vulkan3DDepth,
        title.vulkan3DScale,
    };
    const auto transformPoint = [&](double x,
                                    double y,
                                    double elementCenterX,
                                    double elementCenterY) {
        const jcut::TitleProjectedPointCore point =
            jcut::projectTitle3DPointCore(
                x,
                y,
                elementCenterX,
                elementCenterY,
                centerX,
                centerY,
                size.width,
                size.height,
                titleProjection);
        if (!point.valid) {
            return std::pair<int, int>{
                std::numeric_limits<int>::min(),
                std::numeric_limits<int>::min()};
        }
        return std::pair<int, int>{
            static_cast<int>(std::lround(point.x)),
            static_cast<int>(std::lround(point.y))};
    };
    const auto materialColor = [&](TitleColor base, const std::string& style,
                                   double patternScale, int x, int y,
                                   std::uint8_t coverage,
                                   const ImageBuffer* pattern) {
        const double scale = std::max(0.1, patternScale);
        const auto brighten = [](std::uint8_t value, double factor, double add) {
            return static_cast<std::uint8_t>(std::clamp(value * factor + add, 0.0, 255.0));
        };
        if (style == "neon") {
            const double glow = std::clamp((coverage / 255.0 - 0.02) / 0.93, 0.0, 1.0);
            TitleColor bright{
                brighten(base.r, 1.85, 25.5), brighten(base.g, 1.85, 45.9),
                brighten(base.b, 1.85, 61.2), base.a};
            base.r = static_cast<std::uint8_t>(base.r * 0.55 * (1.0 - glow) + bright.r * glow);
            base.g = static_cast<std::uint8_t>(base.g * 0.55 * (1.0 - glow) + bright.g * glow);
            base.b = static_cast<std::uint8_t>(base.b * 0.55 * (1.0 - glow) + bright.b * glow);
        } else if (style == "diagonal_stripes") {
            const double p = (x + y) / scale * 0.075;
            const bool stripe = p - std::floor(p) >= 0.48;
            const double factor = stripe ? 1.25 : 0.48;
            const double add = stripe ? 20.4 : 0.0;
            base.r = brighten(base.r, factor, add);
            base.g = brighten(base.g, factor, add);
            base.b = brighten(base.b, factor, add);
        } else if (style == "image_pattern" && pattern && !pattern->empty()) {
            const int sampleX = std::clamp(
                static_cast<int>(std::floor(std::abs(x) / scale)) % pattern->size.width,
                0, pattern->size.width - 1);
            const int sampleY = std::clamp(
                static_cast<int>(std::floor(std::abs(y) / scale)) % pattern->size.height,
                0, pattern->size.height - 1);
            const std::size_t offset = static_cast<std::size_t>(
                sampleY * pattern->strideBytes + sampleX * 4);
            base.r = static_cast<std::uint8_t>(
                base.r * 0.28 + pattern->bytes[offset] * 0.72);
            base.g = static_cast<std::uint8_t>(
                base.g * 0.28 + pattern->bytes[offset + 1] * 0.72);
            base.b = static_cast<std::uint8_t>(
                base.b * 0.28 + pattern->bytes[offset + 2] * 0.72);
        } else if (style == "grid" || style == "image_pattern") {
            const double px = x / scale * (style == "grid" ? 0.075 : 0.035);
            const double py = y / scale * (style == "grid" ? 0.075 : 0.035);
            const double gx = std::abs((px - std::floor(px)) - 0.5);
            const double gy = std::abs((py - std::floor(py)) - 0.5);
            const bool line = std::min(gx, gy) < (style == "grid" ? 0.055 : 0.16);
            const double factor = line ? 1.35 : (style == "grid" ? 0.62 : 0.42);
            const double add = line ? 20.0 : 0.0;
            base.r = brighten(base.r, factor, add);
            base.g = brighten(base.g, factor, add);
            base.b = brighten(base.b, factor, add);
        }
        return base;
    };
    const auto blendMaterialPixel = [&](int x, int y, TitleColor base,
                                        const std::string& material,
                                        double materialScale,
                                        std::uint8_t coverage, double opacity,
                                        const ImageBuffer* pattern = nullptr) {
        base = materialColor(base, material, materialScale, x, y, coverage, pattern);
        blendTitlePixel(&image, x, y, base.r, base.g, base.b,
            static_cast<float>(coverage / 255.0 * base.a * opacity));
    };
    const auto fillMaterialRect = [&](int left, int top, int width, int height,
                                      TitleColor color, const std::string& material,
                                      double patternScale, double opacity,
                                      const ImageBuffer* pattern = nullptr) {
        const double elementCenterX =
            left + static_cast<double>(width) * 0.5;
        const double elementCenterY =
            top + static_cast<double>(height) * 0.5;
        for (int y = top; y < top + height; ++y) {
            for (int x = left; x < left + width; ++x) {
                const auto [targetX, targetY] =
                    transformPoint(
                        x, y, elementCenterX, elementCenterY);
                blendMaterialPixel(
                    targetX,
                    targetY,
                    color,
                    material,
                    patternScale,
                    255,
                    opacity,
                    pattern);
            }
        }
    };

    const double windowContentWidth = title.windowWidth > 0.0
        ? std::max(0.0, title.windowWidth - title.windowPadding * 2.0) : 0.0;
    const int contentWidth = static_cast<int>(std::ceil(
        std::max<double>(maximumWidth, windowContentWidth)));
    const int padding = static_cast<int>(std::lround(std::clamp(
        title.windowPadding, 0.0, 400.0)));
    const int windowLeft = static_cast<int>(std::lround(centerX - contentWidth * 0.5)) - padding;
    const int windowTop = static_cast<int>(std::lround(centerY - totalHeight * 0.5)) - padding;
    const int windowWidth = contentWidth + padding * 2;
    const int windowHeight = totalHeight + padding * 2;
    if (title.windowEnabled) {
        const TitleColor color = colorFor(title.windowColor, {0, 0, 0, 1.0});
        fillMaterialRect(windowLeft, windowTop, windowWidth, windowHeight,
                         color, "solid", 1.0,
                         titleOpacity * std::clamp(title.windowOpacity, 0.0, 1.0));
    }
    if (title.windowFrameEnabled && title.windowFrameWidth > 0.0) {
        const int gap = static_cast<int>(std::lround(std::clamp(
            title.windowFrameGap, 0.0, 200.0)));
        const int frameWidth = std::max(1, static_cast<int>(std::lround(
            std::clamp(title.windowFrameWidth, 0.0, 120.0))));
        const TitleColor color = colorFor(title.windowFrameColor, {255, 255, 255, 1.0});
        const double opacity = titleOpacity * std::clamp(title.windowFrameOpacity, 0.0, 1.0);
        const int left = windowLeft - gap;
        const int top = windowTop - gap;
        const int width = windowWidth + gap * 2;
        const int height = windowHeight + gap * 2;
        fillMaterialRect(left, top, width, frameWidth, color,
            title.windowFrameMaterialStyle, title.windowFramePatternScale, opacity,
            framePattern);
        fillMaterialRect(left, top + height - frameWidth, width, frameWidth, color,
            title.windowFrameMaterialStyle, title.windowFramePatternScale, opacity,
            framePattern);
        fillMaterialRect(left, top, frameWidth, height, color,
            title.windowFrameMaterialStyle, title.windowFramePatternScale, opacity,
            framePattern);
        fillMaterialRect(left + width - frameWidth, top, frameWidth, height, color,
            title.windowFrameMaterialStyle, title.windowFramePatternScale, opacity,
            framePattern);
    }

    if (logo && !logo->empty()) {
        const int logoLeft = windowLeft - logo->size.width -
            std::max(8, static_cast<int>(std::lround(padding * 0.45)));
        const int logoTop = static_cast<int>(std::lround(centerY - logo->size.height * 0.5));
        const double logoCenterX =
            logoLeft + static_cast<double>(logo->size.width) * 0.5;
        const double logoCenterY =
            logoTop + static_cast<double>(logo->size.height) * 0.5;
        for (int y = 0; y < logo->size.height; ++y) {
            for (int x = 0; x < logo->size.width; ++x) {
                const std::size_t sourceOffset = static_cast<std::size_t>(
                    y * logo->strideBytes + x * 4);
                const auto [targetX, targetY] =
                    transformPoint(
                        logoLeft + x,
                        logoTop + y,
                        logoCenterX,
                        logoCenterY);
                blendTitlePixel(
                    &image, targetX, targetY,
                    logo->bytes[sourceOffset], logo->bytes[sourceOffset + 1],
                    logo->bytes[sourceOffset + 2],
                    logo->bytes[sourceOffset + 3] / 255.0f *
                        static_cast<float>(titleOpacity));
            }
        }
    }

    const auto drawTextPass = [&](double offsetX, double offsetY,
                                  TitleColor color, const std::string& material,
                                  double patternScale, double opacity) {
        for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
            double cursorX = centerX - lineWidths[lineIndex] * 0.5 + offsetX;
            const double baseline = centerY - totalHeight * 0.5 + ascender +
                static_cast<double>(lineIndex) * lineHeight + offsetY;
            FT_UInt previousGlyph = 0;
            for (const std::uint32_t codepoint : lines[lineIndex]) {
            const FT_UInt glyph = FT_Get_Char_Index(face, codepoint);
            if (FT_HAS_KERNING(face) && previousGlyph && glyph) {
                FT_Vector kerning{};
                FT_Get_Kerning(face, previousGlyph, glyph, FT_KERNING_DEFAULT, &kerning);
                    cursorX += static_cast<double>(kerning.x >> 6);
            }
            if (FT_Load_Glyph(face, glyph, FT_LOAD_DEFAULT) != 0 ||
                FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
                previousGlyph = glyph;
                continue;
            }
            const FT_Bitmap& bitmap = face->glyph->bitmap;
                const double originX = cursorX + face->glyph->bitmap_left;
                const double originY = baseline - face->glyph->bitmap_top;
                const double glyphCenterX =
                    originX + static_cast<double>(bitmap.width) * 0.5;
                const double glyphCenterY =
                    originY + static_cast<double>(bitmap.rows) * 0.5;
            for (unsigned int y = 0; y < bitmap.rows; ++y) {
                for (unsigned int x = 0; x < bitmap.width; ++x) {
                    const std::uint8_t coverage = bitmap.buffer[
                        y * static_cast<unsigned int>(std::abs(bitmap.pitch)) + x];
                        if (coverage == 0) continue;
                        const auto [targetX, targetY] = transformPoint(
                            originX + x,
                            originY + y,
                            glyphCenterX,
                            glyphCenterY);
                        blendMaterialPixel(targetX, targetY, color, material,
                                           patternScale, coverage, opacity,
                                           material == "image_pattern" ? textPattern : nullptr);
                }
            }
                cursorX += static_cast<double>(face->glyph->advance.x >> 6);
            previousGlyph = glyph;
            }
        }
    };
    if (title.dropShadowEnabled && title.dropShadowOpacity > 0.0) {
        const TitleColor shadow = colorFor(title.dropShadowColor, {0, 0, 0, 1.0});
        drawTextPass(title.dropShadowOffsetX, title.dropShadowOffsetY,
                     shadow, "solid", 1.0,
                     titleOpacity * std::clamp(title.dropShadowOpacity, 0.0, 1.0));
    }
    if (title.vulkan3DExtrudeEnabled && title.textExtrudeMode != "none" &&
        title.vulkan3DExtrudeDepth > 0.001) {
        const bool stacked = title.textExtrudeMode == "stacked_copies";
        const double depth = std::clamp(title.vulkan3DExtrudeDepth, 0.0, 2.0);
        const int sparseLayers = std::clamp(
            static_cast<int>(std::ceil(depth * 24.0)), 1, 12);
        const double totalDepth = stacked
            ? std::clamp(depth * 8.0, 0.6, 4.5) * sparseLayers
            : std::clamp(depth * 18.0, 1.0, 36.0);
        const int layers = stacked ? sparseLayers
            : std::clamp(static_cast<int>(std::ceil(totalDepth / 0.65)), 2, 64);
        TitleColor side = textColor;
        side.r = static_cast<std::uint8_t>(side.r * 100 / 175);
        side.g = static_cast<std::uint8_t>(side.g * 100 / 175);
        side.b = static_cast<std::uint8_t>(side.b * 100 / 175);
        for (int layer = layers; layer >= 1; --layer) {
            const double offset = totalDepth * layer / layers;
            drawTextPass(offset, offset * 0.58, side, title.textMaterialStyle,
                         title.textPatternScale, titleOpacity);
        }
    }
    drawTextPass(0.0, 0.0, textColor, title.textMaterialStyle,
                 title.textPatternScale, titleOpacity);
    return image;
}

void blendRgbaPixel(ImageBuffer* image, int x, int y,
                    std::uint8_t red, std::uint8_t green,
                    std::uint8_t blue, std::uint8_t alpha);

struct RgbaColor {
    std::uint8_t red = 255;
    std::uint8_t green = 255;
    std::uint8_t blue = 255;
    double alpha = 1.0;
};

RgbaColor parseColor(std::string value, RgbaColor fallback)
{
    if (value.empty() || value[0] != '#') return fallback;
    try {
        if (value.size() == 7) {
            fallback.red = parseHexByte(value, 1);
            fallback.green = parseHexByte(value, 3);
            fallback.blue = parseHexByte(value, 5);
        } else if (value.size() == 9) {
            fallback.alpha = parseHexByte(value, 1) / 255.0;
            fallback.red = parseHexByte(value, 3);
            fallback.green = parseHexByte(value, 5);
            fallback.blue = parseHexByte(value, 7);
        }
    } catch (...) {
        return fallback;
    }
    return fallback;
}

double measureGlyphRun(FT_Face face, const std::string& text)
{
    if (!face) return 0.0;
    double width = 0.0;
    FT_UInt previous = 0;
    for (std::uint32_t codepoint : utf8Codepoints(text)) {
        const FT_UInt glyph = FT_Get_Char_Index(face, codepoint);
        if (FT_HAS_KERNING(face) && previous && glyph) {
            FT_Vector kerning{};
            FT_Get_Kerning(face, previous, glyph, FT_KERNING_DEFAULT, &kerning);
            width += static_cast<double>(kerning.x >> 6);
        }
        if (FT_Load_Glyph(face, glyph, FT_LOAD_DEFAULT) == 0) {
            width += static_cast<double>(face->glyph->advance.x >> 6);
        }
        previous = glyph;
    }
    return width;
}

void drawGlyphRun(ImageBuffer* image, FT_Face face, double x, double baseline,
                  const std::string& text, const RgbaColor& color)
{
    if (!image || !face || color.alpha <= 0.0) return;
    double cursor = x;
    FT_UInt previous = 0;
    for (std::uint32_t codepoint : utf8Codepoints(text)) {
        const FT_UInt glyph = FT_Get_Char_Index(face, codepoint);
        if (FT_HAS_KERNING(face) && previous && glyph) {
            FT_Vector kerning{};
            FT_Get_Kerning(face, previous, glyph, FT_KERNING_DEFAULT, &kerning);
            cursor += static_cast<double>(kerning.x >> 6);
        }
        if (FT_Load_Glyph(face, glyph, FT_LOAD_DEFAULT) != 0 ||
            FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
            previous = glyph;
            continue;
        }
        const FT_Bitmap& bitmap = face->glyph->bitmap;
        const int originX = static_cast<int>(std::lround(cursor)) + face->glyph->bitmap_left;
        const int originY = static_cast<int>(std::lround(baseline)) - face->glyph->bitmap_top;
        for (unsigned int row = 0; row < bitmap.rows; ++row) {
            for (unsigned int column = 0; column < bitmap.width; ++column) {
                const std::uint8_t coverage = bitmap.buffer[
                    static_cast<std::size_t>(row * bitmap.pitch + column)];
                if (coverage == 0) continue;
                blendRgbaPixel(
                    image, originX + static_cast<int>(column),
                    originY + static_cast<int>(row),
                    color.red, color.green, color.blue,
                    static_cast<std::uint8_t>(std::clamp(
                        color.alpha * coverage + 0.5, 0.0, 255.0)));
            }
        }
        cursor += static_cast<double>(face->glyph->advance.x >> 6);
        previous = glyph;
    }
}

void fillOverlayRect(ImageBuffer* image, int left, int top, int width, int height,
                     const RgbaColor& color)
{
    if (!image || width <= 0 || height <= 0 || color.alpha <= 0.0) return;
    for (int y = std::max(0, top); y < std::min(image->size.height, top + height); ++y) {
        for (int x = std::max(0, left); x < std::min(image->size.width, left + width); ++x) {
            blendRgbaPixel(image, x, y, color.red, color.green, color.blue,
                           static_cast<std::uint8_t>(std::clamp(
                               color.alpha * 255.0 + 0.5, 0.0, 255.0)));
        }
    }
}

void fillRoundedOverlayRect(ImageBuffer* image, int left, int top, int width,
                            int height, double radius, const RgbaColor& color)
{
    if (!image || width <= 0 || height <= 0 || color.alpha <= 0.0) return;
    const double boundedRadius = std::clamp(
        radius, 0.0, std::min(width, height) * 0.5);
    if (boundedRadius < 0.5) {
        fillOverlayRect(image, left, top, width, height, color);
        return;
    }
    const double rightCenter = left + width - boundedRadius;
    const double bottomCenter = top + height - boundedRadius;
    for (int y = std::max(0, top); y < std::min(image->size.height, top + height); ++y) {
        for (int x = std::max(0, left); x < std::min(image->size.width, left + width); ++x) {
            const double centerX = x + 0.5;
            const double centerY = y + 0.5;
            const double nearestX = std::clamp(
                centerX, left + boundedRadius, rightCenter);
            const double nearestY = std::clamp(
                centerY, top + boundedRadius, bottomCenter);
            const double dx = centerX - nearestX;
            const double dy = centerY - nearestY;
            if (dx * dx + dy * dy <= boundedRadius * boundedRadius) {
                blendRgbaPixel(image, x, y, color.red, color.green, color.blue,
                    static_cast<std::uint8_t>(std::clamp(
                        color.alpha * 255.0 + 0.5, 0.0, 255.0)));
            }
        }
    }
}

void drawTranscriptOverlay(ImageBuffer* image,
                           const jcut::EditorTranscriptOverlayState& settings,
                           const jcut::TranscriptOverlayLayoutCore& layout)
{
    if (!image || image->empty() || layout.lines.empty()) return;
    const double boxWidth = std::max(160.0, settings.boxWidth);
    const double boxHeight = std::max(80.0, settings.boxHeight);
    const double translationX = !settings.useManualPlacement && layout.speakerLocationValid
        ? (std::clamp(layout.speakerLocationX, 0.0, 1.0) - 0.5) * image->size.width
        : std::clamp(settings.translationX, -1.0, 1.0) * image->size.width * 0.5;
    const double translationY = !settings.useManualPlacement && layout.speakerLocationValid
        ? (std::clamp(layout.speakerLocationY, 0.0, 1.0) - 0.5) * image->size.height
        : std::clamp(settings.translationY, -1.0, 1.0) * image->size.height * 0.5;
    const int left = static_cast<int>(std::lround(
        image->size.width * 0.5 + translationX - boxWidth * 0.5));
    const int top = static_cast<int>(std::lround(
        image->size.height * 0.5 + translationY - boxHeight * 0.5));
    if (settings.showBackground) {
        const double radius = std::clamp(settings.backgroundCornerRadius, 0.0, 128.0);
        const int frameGap = static_cast<int>(std::lround(
            std::clamp(settings.backgroundFrameGap, 0.0, 200.0)));
        const int frameWidth = static_cast<int>(std::lround(
            std::clamp(settings.backgroundFrameWidth, 0.0, 120.0)));
        if (settings.backgroundFrameEnabled && frameWidth > 0) {
            RgbaColor frame = parseColor(
                settings.backgroundFrameColor, {255, 255, 255, 1.0});
            frame.alpha *= std::clamp(settings.backgroundFrameOpacity, 0.0, 1.0);
            fillRoundedOverlayRect(
                image, left + frameGap, top + frameGap,
                static_cast<int>(std::lround(boxWidth)) - frameGap * 2,
                static_cast<int>(std::lround(boxHeight)) - frameGap * 2,
                radius, frame);
        }
        RgbaColor background = parseColor(settings.backgroundColor, {0, 0, 0, 1.0});
        background.alpha *= std::clamp(settings.backgroundOpacity, 0.0, 1.0);
        const int backgroundInset = settings.backgroundFrameEnabled
            ? frameGap + frameWidth : 0;
        fillRoundedOverlayRect(
            image, left + backgroundInset, top + backgroundInset,
            static_cast<int>(std::lround(boxWidth)) - backgroundInset * 2,
            static_cast<int>(std::lround(boxHeight)) - backgroundInset * 2,
            std::max(0.0, radius - backgroundInset), background);
    }

    jcut::EditorTitleKeyframe fontRequest;
    fontRequest.fontFamily = settings.fontFamily;
    fontRequest.bold = settings.bold;
    fontRequest.italic = settings.italic;
    const std::string fontPath = resolveFontPath(fontRequest);
    FT_Face face = nullptr;
    if (fontPath.empty() || !freeTypeLibrary() ||
        FT_New_Face(freeTypeLibrary(), fontPath.c_str(), 0, &face) != 0 || !face) return;
    struct FaceGuard { FT_Face value; ~FaceGuard() { if (value) FT_Done_Face(value); } } guard{face};
    FT_Select_Charmap(face, FT_ENCODING_UNICODE);
    const int pixelSize = std::max(12, settings.fontPointSize);
    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixelSize)) != 0) return;
    const double lineHeight = std::max(1L, face->size->metrics.height >> 6);
    const double ascender = face->size->metrics.ascender >> 6;
    const double spaceWidth = measureGlyphRun(face, " ");
    const double padding = std::clamp(settings.backgroundPadding, 0.0, 400.0);
    const std::string speakerTitle = settings.showSpeakerTitle
        ? (!layout.speakerTitle.empty() ? layout.speakerTitle : layout.speakerId)
        : std::string{};
    const int speakerPixelSize = std::max(
        1, static_cast<int>(std::lround(pixelSize * 0.62)));
    FT_Face speakerFace = nullptr;
    struct OptionalFaceGuard {
        FT_Face value = nullptr;
        ~OptionalFaceGuard() { if (value) FT_Done_Face(value); }
    } speakerGuard;
    double speakerLineHeight = 0.0;
    double speakerAscender = 0.0;
    if (!speakerTitle.empty()) {
        jcut::EditorTitleKeyframe speakerFont = fontRequest;
        speakerFont.bold = true;
        speakerFont.italic = false;
        const std::string speakerFontPath = resolveFontPath(speakerFont);
        if (!speakerFontPath.empty() &&
            FT_New_Face(freeTypeLibrary(), speakerFontPath.c_str(), 0, &speakerFace) == 0 &&
            speakerFace) {
            speakerGuard.value = speakerFace;
            FT_Select_Charmap(speakerFace, FT_ENCODING_UNICODE);
            if (FT_Set_Pixel_Sizes(
                    speakerFace, 0, static_cast<FT_UInt>(speakerPixelSize)) == 0) {
                speakerLineHeight = std::max(1L, speakerFace->size->metrics.height >> 6);
                speakerAscender = speakerFace->size->metrics.ascender >> 6;
            } else {
                speakerFace = nullptr;
            }
        }
    }
    const double titleGap = speakerFace ? pixelSize * 0.30 : 0.0;
    const double contentHeight = lineHeight * layout.lines.size() +
        speakerLineHeight + titleGap;
    double cursorY = top + std::max(padding, (boxHeight - contentHeight) * 0.5);
    RgbaColor textColor = parseColor(settings.textColor, {255, 255, 255, 1.0});
    textColor.alpha *= std::clamp(settings.textOpacity, 0.0, 1.0);
    RgbaColor highlight = parseColor(settings.highlightColor, {255, 242, 168, 1.0});
    RgbaColor highlightText = parseColor(
        settings.highlightTextColor, {24, 24, 24, 1.0});
    RgbaColor shadow = parseColor(settings.shadowColor, {0, 0, 0, 1.0});
    shadow.alpha *= std::clamp(settings.shadowOpacity, 0.0, 1.0);
    RgbaColor outline = parseColor(settings.textOutlineColor, {0, 0, 0, 1.0});
    outline.alpha *= std::clamp(settings.textOutlineOpacity, 0.0, 1.0);
    const int outlineRadius = settings.textOutlineEnabled
        ? static_cast<int>(std::ceil(std::clamp(settings.textOutlineWidth, 0.0, 24.0)))
        : 0;
    const auto drawOutline = [&](FT_Face runFace, double x, double baseline,
                                 const std::string& text) {
        for (int dy = -outlineRadius; dy <= outlineRadius; ++dy) {
            for (int dx = -outlineRadius; dx <= outlineRadius; ++dx) {
                if ((dx == 0 && dy == 0) ||
                    dx * dx + dy * dy > outlineRadius * outlineRadius) continue;
                drawGlyphRun(image, runFace, x + dx, baseline + dy, text, outline);
            }
        }
    };
    const auto drawExtrusion = [&](FT_Face runFace, double x, double baseline,
                                   const std::string& text, const RgbaColor& color) {
        if (settings.textExtrudeMode == "none" || settings.textExtrudeDepth <= 0.001) return;
        const bool stacked = settings.textExtrudeMode == "stacked_copies";
        const double depth = std::clamp(settings.textExtrudeDepth, 0.0, 2.0);
        const int sparseLayers = std::clamp(
            static_cast<int>(std::ceil(depth * 24.0)), 1, 12);
        const double totalDepth = stacked
            ? std::clamp(depth * 8.0, 0.6, 4.5) * sparseLayers
            : std::clamp(depth * 18.0, 1.0, 36.0);
        const int layers = stacked ? sparseLayers
            : std::clamp(static_cast<int>(std::ceil(totalDepth / 0.65)), 2, 64);
        RgbaColor side = color;
        side.red = static_cast<std::uint8_t>(side.red * 100 / 175);
        side.green = static_cast<std::uint8_t>(side.green * 100 / 175);
        side.blue = static_cast<std::uint8_t>(side.blue * 100 / 175);
        for (int layer = layers; layer >= 1; --layer) {
            const double offset = totalDepth * layer / layers;
            drawGlyphRun(image, runFace, x + offset, baseline + offset * 0.58,
                         text, side);
        }
    };
    if (speakerFace) {
        const double width = measureGlyphRun(speakerFace, speakerTitle);
        const double x = left + std::max(padding, (boxWidth - width) * 0.5);
        const double baseline = cursorY + speakerAscender;
        if (settings.showShadow) {
            drawGlyphRun(image, speakerFace,
                x + settings.shadowOffsetX, baseline + settings.shadowOffsetY,
                speakerTitle, shadow);
        }
        drawExtrusion(speakerFace, x, baseline, speakerTitle, textColor);
        drawOutline(speakerFace, x, baseline, speakerTitle);
        drawGlyphRun(image, speakerFace, x, baseline, speakerTitle, textColor);
        cursorY += speakerLineHeight + titleGap;
    }
    for (const jcut::TranscriptOverlayLineCore& line : layout.lines) {
        std::vector<double> widths;
        widths.reserve(line.words.size());
        double lineWidth = 0.0;
        for (const std::string& word : line.words) {
            const double width = measureGlyphRun(face, word);
            widths.push_back(width);
            lineWidth += width;
        }
        if (line.words.size() > 1) lineWidth += spaceWidth * (line.words.size() - 1);
        double cursorX = left + std::max(padding, (boxWidth - lineWidth) * 0.5);
        const double baseline = cursorY + ascender;
        for (std::size_t wordIndex = 0; wordIndex < line.words.size(); ++wordIndex) {
            const bool active = settings.highlightCurrentWord &&
                static_cast<int>(wordIndex) == line.activeWord;
            if (active) {
                const int paddingX = std::max(1, static_cast<int>(std::lround(pixelSize * 0.18)));
                const int paddingY = std::max(1, static_cast<int>(std::lround(pixelSize * 0.02)));
                fillRoundedOverlayRect(
                    image,
                    static_cast<int>(std::floor(cursorX)) - paddingX,
                    static_cast<int>(std::floor(baseline - ascender)) - paddingY,
                    static_cast<int>(std::ceil(widths[wordIndex])) + paddingX * 2,
                    static_cast<int>(std::ceil(lineHeight)) + paddingY * 2,
                    pixelSize * 0.28, highlight);
            }
            const RgbaColor glyphColor = active ? highlightText : textColor;
            if (settings.showShadow && !active) {
                drawGlyphRun(image, face,
                             cursorX + settings.shadowOffsetX,
                             baseline + settings.shadowOffsetY,
                             line.words[wordIndex], shadow);
            }
            drawExtrusion(face, cursorX, baseline,
                          line.words[wordIndex], glyphColor);
            drawOutline(face, cursorX, baseline, line.words[wordIndex]);
            drawGlyphRun(image, face, cursorX, baseline,
                         line.words[wordIndex], glyphColor);
            cursorX += widths[wordIndex] +
                (wordIndex + 1 < line.words.size() ? spaceWidth : 0.0);
        }
        cursorY += lineHeight;
    }
}

std::vector<std::uint8_t> maskPlaneFromImage(const ImageBuffer& mask, SizeI size)
{
    std::vector<std::uint8_t> plane(static_cast<std::size_t>(size.width * size.height), 0);
    if (mask.empty()) return plane;
    for (int y = 0; y < size.height; ++y) {
        const int sourceY = std::clamp(
            static_cast<int>((static_cast<std::int64_t>(y) * mask.size.height) /
                             std::max(1, size.height)), 0, mask.size.height - 1);
        for (int x = 0; x < size.width; ++x) {
            const int sourceX = std::clamp(
                static_cast<int>((static_cast<std::int64_t>(x) * mask.size.width) /
                                 std::max(1, size.width)), 0, mask.size.width - 1);
            const std::size_t sourceOffset = static_cast<std::size_t>(
                sourceY * mask.strideBytes + sourceX * 4);
            const int luma = (77 * mask.bytes[sourceOffset] +
                              150 * mask.bytes[sourceOffset + 1] +
                              29 * mask.bytes[sourceOffset + 2]) >> 8;
            plane[static_cast<std::size_t>(y * size.width + x)] =
                static_cast<std::uint8_t>(std::min<int>(
                    luma, mask.bytes[sourceOffset + 3]));
        }
    }
    return plane;
}

void morphMaskPlane(std::vector<std::uint8_t>* plane, SizeI size,
                    int radius, bool dilate)
{
    if (!plane || radius <= 0 || plane->empty()) return;
    radius = std::min(radius, 128);
    const std::vector<std::uint8_t> source = *plane;
    for (int y = 0; y < size.height; ++y) {
        for (int x = 0; x < size.width; ++x) {
            int value = dilate ? 0 : 255;
            for (int sampleY = std::max(0, y - radius);
                 sampleY <= std::min(size.height - 1, y + radius); ++sampleY) {
                for (int sampleX = std::max(0, x - radius);
                     sampleX <= std::min(size.width - 1, x + radius); ++sampleX) {
                    const int sample = source[static_cast<std::size_t>(
                        sampleY * size.width + sampleX)];
                    value = dilate ? std::max(value, sample) : std::min(value, sample);
                }
            }
            (*plane)[static_cast<std::size_t>(y * size.width + x)] =
                static_cast<std::uint8_t>(value);
        }
    }
}

double shapeMaskFeather(double value, double gamma, int falloff)
{
    const double t = std::clamp(value, 0.0, 1.0);
    switch (std::clamp(falloff, 0, 5)) {
    case 1: return t;
    case 2: return t * t * (3.0 - 2.0 * t);
    case 3: return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
    case 4: return 0.5 - 0.5 * std::cos(t * 3.14159265358979323846);
    case 5: {
        constexpr double k = 4.0;
        const double low = std::exp(-k);
        return (std::exp(-k * (1.0 - t) * (1.0 - t)) - low) / (1.0 - low);
    }
    default: return std::pow(t, 1.0 / std::max(0.01, gamma));
    }
}

void blurMaskPlane(std::vector<std::uint8_t>* plane, SizeI size,
                   int radius, double gamma, int falloff)
{
    if (!plane || radius <= 0 || plane->empty()) return;
    radius = std::min(radius, 128);
    const std::vector<std::uint8_t> source = *plane;
    for (int y = 0; y < size.height; ++y) {
        for (int x = 0; x < size.width; ++x) {
            std::int64_t sum = 0;
            int count = 0;
            for (int sampleY = std::max(0, y - radius);
                 sampleY <= std::min(size.height - 1, y + radius); ++sampleY) {
                for (int sampleX = std::max(0, x - radius);
                     sampleX <= std::min(size.width - 1, x + radius); ++sampleX) {
                    sum += source[static_cast<std::size_t>(
                        sampleY * size.width + sampleX)];
                    ++count;
                }
            }
            const double value = count > 0
                ? static_cast<double>(sum) / (count * 255.0) : 0.0;
            (*plane)[static_cast<std::size_t>(y * size.width + x)] =
                static_cast<std::uint8_t>(std::clamp(
                    shapeMaskFeather(value, gamma, falloff) * 255.0 + 0.5,
                    0.0, 255.0));
        }
    }
}

bool pointInsidePolygon(double x, double y,
                        const std::vector<jcut::EditorPoint>& points)
{
    bool inside = false;
    for (std::size_t current = 0, previous = points.size() - 1;
         current < points.size(); previous = current++) {
        const double currentY = points[current].y;
        const double previousY = points[previous].y;
        if ((currentY > y) != (previousY > y)) {
            const double crossingX =
                (points[previous].x - points[current].x) *
                    (y - currentY) /
                    (previousY - currentY) + points[current].x;
            if (x < crossingX) inside = !inside;
        }
    }
    return inside;
}

void applyCorrectionPolygons(std::vector<std::uint8_t>* plane, SizeI size,
                             const EditorClip& clip, int localFrame)
{
    if (!plane) return;
    for (const jcut::EditorCorrectionPolygon& polygon : clip.correctionPolygons) {
        if (!polygon.enabled || polygon.pointsNormalized.size() < 3 ||
            localFrame < polygon.startFrame ||
            (polygon.endFrame >= 0 && localFrame > polygon.endFrame)) continue;
        for (int y = 0; y < size.height; ++y) {
            for (int x = 0; x < size.width; ++x) {
                if (pointInsidePolygon(
                        (x + 0.5) / std::max(1, size.width),
                        (y + 0.5) / std::max(1, size.height),
                        polygon.pointsNormalized)) {
                    (*plane)[static_cast<std::size_t>(y * size.width + x)] = 0;
                }
            }
        }
    }
}

void applyClipGrade(ImageBuffer* image,
                    const jcut::EditorGradingKeyframe& grade,
                    bool forceOpaque);

void blendRgbaPixel(ImageBuffer* image, int x, int y,
                    std::uint8_t red, std::uint8_t green,
                    std::uint8_t blue, std::uint8_t alpha)
{
    if (!image || x < 0 || y < 0 || x >= image->size.width || y >= image->size.height || alpha == 0) {
        return;
    }
    const std::size_t offset = static_cast<std::size_t>(y * image->strideBytes + x * 4);
    const float sourceAlpha = alpha / 255.0f;
    const float destinationAlpha = image->bytes[offset + 3] / 255.0f;
    const float outputAlpha = sourceAlpha + destinationAlpha * (1.0f - sourceAlpha);
    for (int channel = 0; channel < 3; ++channel) {
        const float source = channel == 0 ? red : (channel == 1 ? green : blue);
        const float premultiplied = source * sourceAlpha +
            image->bytes[offset + channel] * destinationAlpha * (1.0f - sourceAlpha);
        image->bytes[offset + channel] = static_cast<std::uint8_t>(
            outputAlpha > 0.0f ? premultiplied / outputAlpha + 0.5f : 0.0f);
    }
    image->bytes[offset + 3] = static_cast<std::uint8_t>(
        std::clamp(outputAlpha * 255.0f + 0.5f, 0.0f, 255.0f));
}

void applyMaskPlaneToImage(ImageBuffer* image,
                           const ImageBuffer& original,
                           const std::vector<std::uint8_t>& mask,
                           const EditorClip& clip)
{
    if (!image || image->empty() || mask.empty()) return;
    const double opacity = std::clamp(clip.maskOpacity, 0.0, 1.0);
    if (clip.maskShowOnly) {
        for (int y = 0; y < image->size.height; ++y) {
            for (int x = 0; x < image->size.width; ++x) {
                const std::size_t pixel = static_cast<std::size_t>(y * image->size.width + x);
                const std::size_t offset = static_cast<std::size_t>(y * image->strideBytes + x * 4);
                const std::uint8_t value = mask[pixel];
                image->bytes[offset] = value;
                image->bytes[offset + 1] = value;
                image->bytes[offset + 2] = value;
                image->bytes[offset + 3] = value;
            }
        }
        return;
    }

    const bool generatedMatte = clip.clipRole == "mask_matte";
    const bool drawsMaskedForeground = generatedMatte ||
        clip.maskForegroundLayerEnabled || clip.maskRepeatEnabled ||
        clip.maskGradeEnabled;
    if (!drawsMaskedForeground && !clip.maskDropShadowEnabled) return;

    ImageBuffer foreground = generatedMatte || original.empty() ? *image : original;
    if (clip.maskGradeEnabled) {
        jcut::EditorGradingKeyframe maskGrade;
        maskGrade.brightness = clip.maskGradeBrightness;
        maskGrade.contrast = clip.maskGradeContrast;
        maskGrade.saturation = clip.maskGradeSaturation;
        maskGrade.curvePointsR = clip.maskGradeCurvePointsR;
        maskGrade.curvePointsG = clip.maskGradeCurvePointsG;
        maskGrade.curvePointsB = clip.maskGradeCurvePointsB;
        maskGrade.curvePointsLuma = clip.maskGradeCurvePointsLuma;
        maskGrade.curveSmoothingEnabled = clip.maskGradeCurveSmoothingEnabled;
        applyClipGrade(&foreground, maskGrade, false);
    } else if (!generatedMatte && clip.maskForegroundLayerEnabled) {
        foreground = *image;
    }

    if (generatedMatte) {
        std::fill(image->bytes.begin(), image->bytes.end(), 0);
    }
    if (clip.maskDropShadowEnabled && clip.maskDropShadowOpacity > 0.0 &&
        (generatedMatte || clip.maskForegroundLayerEnabled)) {
        std::vector<std::uint8_t> shadowMask = mask;
        blurMaskPlane(&shadowMask, image->size,
            static_cast<int>(std::lround(std::clamp(
                clip.maskDropShadowRadius, 0.0, 200.0))), 1.0, 1);
        const int offsetX = static_cast<int>(std::lround(clip.maskDropShadowOffsetX));
        const int offsetY = static_cast<int>(std::lround(clip.maskDropShadowOffsetY));
        for (int y = 0; y < image->size.height; ++y) {
            for (int x = 0; x < image->size.width; ++x) {
                const std::uint8_t alpha = static_cast<std::uint8_t>(
                    shadowMask[static_cast<std::size_t>(y * image->size.width + x)] *
                    std::clamp(clip.maskDropShadowOpacity, 0.0, 1.0) + 0.5);
                blendRgbaPixel(image, x + offsetX, y + offsetY, 0, 0, 0, alpha);
            }
        }
    }

    const int repeatCount = clip.maskRepeatEnabled
        ? std::clamp(clip.effectRows, 1, 96) : 1;
    const double centerIndex = (repeatCount - 1) * 0.5;
    for (int repeatIndex = 0; repeatIndex < repeatCount; ++repeatIndex) {
        const double offsetIndex = repeatIndex - centerIndex;
        const int offsetX = static_cast<int>(std::lround(
            clip.maskRepeatEnabled ? clip.maskRepeatDeltaX * offsetIndex : 0.0));
        const int offsetY = static_cast<int>(std::lround(
            clip.maskRepeatEnabled ? clip.maskRepeatDeltaY * offsetIndex : 0.0));
        for (int y = 0; y < image->size.height; ++y) {
            for (int x = 0; x < image->size.width; ++x) {
                const std::size_t pixel = static_cast<std::size_t>(y * image->size.width + x);
                const std::size_t sourceOffset = static_cast<std::size_t>(
                    y * foreground.strideBytes + x * 4);
                const std::uint8_t alpha = static_cast<std::uint8_t>(
                    foreground.bytes[sourceOffset + 3] *
                    (mask[pixel] / 255.0) * opacity + 0.5);
                blendRgbaPixel(
                    image, x + offsetX, y + offsetY,
                    foreground.bytes[sourceOffset],
                    foreground.bytes[sourceOffset + 1],
                    foreground.bytes[sourceOffset + 2],
                    alpha);
            }
        }
    }
}

void blitImage(const ImageBuffer& source, ImageBuffer* destination, int offsetX, int offsetY)
{
    if (!destination || source.empty() || destination->empty()) {
        return;
    }
    for (int y = 0; y < source.size.height; ++y) {
        const int destY = y + offsetY;
        if (destY < 0 || destY >= destination->size.height) {
            continue;
        }
        for (int x = 0; x < source.size.width; ++x) {
            const int destX = x + offsetX;
            if (destX < 0 || destX >= destination->size.width) {
                continue;
            }
            const std::size_t sourceOffset = static_cast<std::size_t>(y * source.strideBytes + x * 4);
            const std::size_t destOffset =
                static_cast<std::size_t>(destY * destination->strideBytes + destX * 4);
            const float sourceAlpha =
                static_cast<float>(source.bytes[sourceOffset + 3]) / 255.0f;
            const float destinationAlpha =
                static_cast<float>(destination->bytes[destOffset + 3]) / 255.0f;
            const float outputAlpha = sourceAlpha +
                destinationAlpha * (1.0f - sourceAlpha);
            for (int channel = 0; channel < 3; ++channel) {
                const float sourceValue = source.bytes[sourceOffset + channel];
                const float destinationValue = destination->bytes[destOffset + channel];
                const float premultiplied = sourceValue * sourceAlpha +
                    destinationValue * destinationAlpha * (1.0f - sourceAlpha);
                destination->bytes[destOffset + channel] =
                    static_cast<std::uint8_t>(std::clamp(
                        outputAlpha > 0.0f ? premultiplied / outputAlpha : 0.0f,
                        0.0f, 255.0f));
            }
            destination->bytes[destOffset + 3] =
                static_cast<std::uint8_t>(std::clamp(
                    outputAlpha * 255.0f, 0.0f, 255.0f));
        }
    }
}

void blitTransformedImage(
    const ImageBuffer& source,
    ImageBuffer* destination,
    int offsetX,
    int offsetY,
    const jcut::EditorTransformKeyframe& transform);

std::array<double, 4> sampleImageBilinear(
    const ImageBuffer& image, double u, double v);

ImageBuffer resizeImageNearest(const ImageBuffer& source, SizeI size)
{
    if (source.empty() || !size.valid()) return {};
    ImageBuffer resized = makeSolidImage(size, 0, 0, 0);
    std::fill(resized.bytes.begin(), resized.bytes.end(), 0);
    for (int y = 0; y < size.height; ++y) {
        const int sourceY = std::min(
            source.size.height - 1,
            static_cast<int>((static_cast<std::int64_t>(y) * source.size.height) /
                             size.height));
        for (int x = 0; x < size.width; ++x) {
            const int sourceX = std::min(
                source.size.width - 1,
                static_cast<int>((static_cast<std::int64_t>(x) * source.size.width) /
                                 size.width));
            const std::size_t sourceOffset = static_cast<std::size_t>(
                sourceY * source.strideBytes + sourceX * 4);
            const std::size_t destinationOffset = static_cast<std::size_t>(
                y * resized.strideBytes + x * 4);
            std::copy_n(
                source.bytes.begin() + static_cast<std::ptrdiff_t>(sourceOffset),
                4,
                resized.bytes.begin() + static_cast<std::ptrdiff_t>(destinationOffset));
        }
    }
    return resized;
}

ImageBuffer renderSourceTilingEffect(const ImageBuffer& source,
                                    const EditorClip& clip,
                                    int localFrame,
                                    SizeI outputSize)
{
    ImageBuffer result = makeSolidImage(outputSize, 0, 0, 0);
    std::fill(result.bytes.begin(), result.bytes.end(), 0);
    if (source.empty()) return result;

    constexpr double kTwoPi = 6.28318530717958647692;
    const int count = std::clamp(clip.effectRows, 1, 96);
    const double scale = std::clamp(clip.effectScale, 0.1, 8.0);
    const double spacing = std::clamp(clip.tilingSpacing, 0.1, 8.0);
    const double speed = std::clamp(clip.effectSpeed, -8.0, 8.0);
    const double aspect = static_cast<double>(source.size.width) /
        std::max(1, source.size.height);
    const double minDimension = std::max(1, std::min(outputSize.width, outputSize.height));
    const double tileWidth = std::max(2.0, outputSize.width * scale / count);
    const double tileHeight = std::max(2.0, tileWidth / std::max(0.001, aspect));
    const double stepX = std::max(1.0, tileWidth * spacing);
    const double stepY = std::max(1.0, tileHeight * spacing);
    const double centerX = outputSize.width * 0.5;
    const double centerY = outputSize.height * 0.5;
    int emitted = 0;
    auto addDraw = [&](double x, double y, double width = -1.0, double height = -1.0) {
        if (++emitted > 4096) return;
        const int drawWidth = std::max(1, static_cast<int>(std::lround(
            width > 0.0 ? width : tileWidth)));
        const int drawHeight = std::max(1, static_cast<int>(std::lround(
            height > 0.0 ? height : tileHeight)));
        const ImageBuffer tile = resizeImageNearest(source, {drawWidth, drawHeight});
        blitImage(tile, &result,
                  static_cast<int>(std::lround(x)),
                  static_cast<int>(std::lround(y)));
    };

    if (clip.tilingPattern == "encircle") {
        const double radius = minDimension * 0.34 * spacing;
        const double phase = localFrame * speed * 0.018;
        for (int i = 0; i < count; ++i) {
            const double angle = phase + kTwoPi * i / count;
            addDraw(centerX + std::cos(angle) * radius - tileWidth * 0.5,
                    centerY + std::sin(angle) * radius - tileHeight * 0.5);
        }
    } else if (clip.tilingPattern == "spiral_xy" ||
               clip.tilingPattern == "spiral_xz" ||
               clip.tilingPattern == "spiral_yz") {
        const double maxRadius = minDimension * 0.46 * spacing;
        const double phase = localFrame * speed * 0.014;
        for (int i = 0; i < count; ++i) {
            const double t = count <= 1 ? 0.0 : static_cast<double>(i) / (count - 1);
            const double angle = phase + kTwoPi * 1.61803398875 * i;
            const double u = std::cos(angle) * maxRadius * t;
            const double v = std::sin(angle) * maxRadius * t;
            double x = centerX + u;
            double y = centerY + v;
            double sizeMultiplier = 1.0;
            if (clip.tilingPattern == "spiral_xz") {
                y = centerY + (t - 0.5) * outputSize.height * 0.68;
                sizeMultiplier = std::clamp(0.76 + v / std::max(1.0, maxRadius) * 0.34,
                                            0.45, 1.18);
            } else if (clip.tilingPattern == "spiral_yz") {
                x = centerX + (t - 0.5) * outputSize.width * 0.68;
                y = centerY + u;
                sizeMultiplier = std::clamp(0.76 + v / std::max(1.0, maxRadius) * 0.34,
                                            0.45, 1.18);
            }
            addDraw(x - tileWidth * sizeMultiplier * 0.5,
                    y - tileHeight * sizeMultiplier * 0.5,
                    tileWidth * sizeMultiplier,
                    tileHeight * sizeMultiplier);
        }
    } else if (clip.tilingPattern == "diamond") {
        addDraw(centerX - tileWidth * 0.5, centerY - tileHeight * 0.5);
        for (int ring = 1; emitted < count && ring <= 10; ++ring) {
            const double dx = stepX * ring;
            const double dy = stepY * ring;
            const std::pair<double, double> points[] = {
                {centerX, centerY - dy}, {centerX + dx, centerY},
                {centerX, centerY + dy}, {centerX - dx, centerY}};
            for (const auto& [x, y] : points) {
                if (emitted >= count) break;
                addDraw(x - tileWidth * 0.5, y - tileHeight * 0.5);
            }
        }
    } else {
        const double phaseXRaw = std::fmod(localFrame * speed * tileWidth * 0.015, stepX);
        const double phaseYRaw = std::fmod(localFrame * speed * tileHeight * 0.006, stepY);
        const double phaseX = phaseXRaw < 0.0 ? phaseXRaw + stepX : phaseXRaw;
        const double phaseY = phaseYRaw < 0.0 ? phaseYRaw + stepY : phaseYRaw;
        const double startY = clip.tilingWrap ? -tileHeight + phaseY : 0.0;
        const double endY = clip.tilingWrap ? outputSize.height + tileHeight
                                           : outputSize.height - tileHeight + 1.0;
        int row = 0;
        for (double y = startY; y < endY && emitted < 4096; y += stepY, ++row) {
            const double rowOffset = clip.effectAlternateDirection && (row % 2)
                ? stepX * 0.5 : 0.0;
            const double startX = clip.tilingWrap
                ? -tileWidth + phaseX - rowOffset : -rowOffset;
            const double endX = clip.tilingWrap
                ? outputSize.width + tileWidth : count * stepX;
            for (double x = startX; x < endX && emitted < 4096; x += stepX) {
                addDraw(x, y);
            }
        }
    }
    return result;
}

bool isGeneratedRepeatEffect(std::string_view preset)
{
    return preset == "news_logo_ticker" ||
        preset == "alternating_motion_background" ||
        preset == "directional_trim_ticker" ||
        preset == "person_orbit" ||
        preset == "freeze_pattern" ||
        preset == "step_repeat";
}

ImageBuffer renderGeneratedRepeatEffect(const ImageBuffer& source,
                                       const EditorClip& clip,
                                       int localFrame,
                                       SizeI outputSize)
{
    ImageBuffer result = makeSolidImage(outputSize, 0, 0, 0);
    std::fill(result.bytes.begin(), result.bytes.end(), 0);
    if (source.empty()) return result;
    constexpr double kTwoPi = 6.28318530717958647692;
    const int count = std::clamp(clip.effectRows, 1, 96);
    const double scale = std::clamp(clip.effectScale, 0.1, 8.0);
    const double speed = std::clamp(clip.effectSpeed, -8.0, 8.0);
    const double aspect = static_cast<double>(source.size.width) /
        std::max(1, source.size.height);
    int emitted = 0;
    auto draw = [&](double x, double y, double width, double height) {
        if (++emitted > 4096) return;
        const ImageBuffer tile = resizeImageNearest(source, {
            std::max(1, static_cast<int>(std::lround(width))),
            std::max(1, static_cast<int>(std::lround(height)))});
        blitImage(tile, &result,
                  static_cast<int>(std::lround(x)),
                  static_cast<int>(std::lround(y)));
    };

    if (clip.effectPreset == "news_logo_ticker" ||
        clip.effectPreset == "alternating_motion_background" ||
        clip.effectPreset == "directional_trim_ticker") {
        const double rowHeight = static_cast<double>(outputSize.height) / count;
        double baseCoverage = 0.78;
        double baseSpacing = 1.35;
        double phaseScale = 0.08;
        if (clip.effectPreset == "alternating_motion_background") {
            baseCoverage = 1.08;
            baseSpacing = 1.02;
        } else if (clip.effectPreset == "directional_trim_ticker") {
            baseCoverage = 0.92;
            baseSpacing = 0.74;
            phaseScale = 0.18;
        }
        const double tileHeight = std::max(2.0, rowHeight * baseCoverage * scale);
        const double trimPulse = clip.effectPreset == "directional_trim_ticker"
            ? 0.58 + 0.42 * std::abs(std::sin(
                  localFrame * std::max(0.1, std::abs(speed)) * 0.12))
            : 1.0;
        const double tileWidth = std::max(2.0, tileHeight * aspect * trimPulse);
        const double spacing = tileWidth * baseSpacing;
        for (int row = 0; row < count; ++row) {
            const double direction = clip.effectAlternateDirection && (row % 2) ? -1.0 : 1.0;
            double phase = std::fmod(
                localFrame * speed * direction * rowHeight * phaseScale +
                    row * spacing * 0.37,
                spacing);
            if ((clip.effectPreset == "alternating_motion_background" ||
                 clip.effectPreset == "directional_trim_ticker") && phase < 0.0) {
                phase += spacing;
            }
            const double y = (row + 0.5) * rowHeight - tileHeight * 0.5;
            for (double x = -spacing + phase;
                 x < outputSize.width + spacing && emitted < 4096;
                 x += spacing) {
                draw(x, y, tileWidth, tileHeight);
            }
        }
    } else if (clip.effectPreset == "person_orbit") {
        const double tileHeight = std::max(
            4.0, std::min(outputSize.width, outputSize.height) * 0.072 * scale);
        const double tileWidth = tileHeight * aspect;
        const double centerX = outputSize.width * 0.5;
        const double centerY = outputSize.height * 0.5;
        const double radiusX = outputSize.width * 0.28;
        const double radiusY = outputSize.height * 0.24;
        const double phase = localFrame * speed * 0.025;
        for (int index = 0; index < count; ++index) {
            const double angle = phase + kTwoPi * index / count;
            draw(centerX + std::cos(angle) * radiusX - tileWidth * 0.5,
                 centerY + std::sin(angle) * radiusY - tileHeight * 0.5,
                 tileWidth, tileHeight);
        }
    } else if (clip.effectPreset == "freeze_pattern") {
        const int columns = std::clamp(
            static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count)))), 1, 12);
        const int rows = std::clamp(
            static_cast<int>(std::ceil(static_cast<double>(count) / columns)), 1, 12);
        const double cellWidth = static_cast<double>(outputSize.width) / columns;
        const double cellHeight = static_cast<double>(outputSize.height) / rows;
        const double tileHeight = std::max(2.0, cellHeight * 0.86 * scale);
        const double tileWidth = std::max(
            2.0, std::min(cellWidth * 0.92, tileHeight * aspect));
        const int activeStep = static_cast<int>(std::floor(
            localFrame * std::max(0.1, std::abs(speed)) / 8.0));
        for (int index = 0; index < count; ++index) {
            const int column = index % columns;
            const int row = index / columns;
            if (row >= rows) break;
            const double jitter = ((activeStep + index * 3) % 5 - 2) *
                std::min(cellWidth, cellHeight) * 0.025;
            draw((column + 0.5) * cellWidth - tileWidth * 0.5 + jitter,
                 (row + 0.5) * cellHeight - tileHeight * 0.5 - jitter,
                 tileWidth, tileHeight);
        }
    } else if (clip.effectPreset == "step_repeat") {
        const double tileHeight = std::max(
            4.0,
            std::min(outputSize.width, outputSize.height) *
                std::clamp(0.12 * scale, 0.03, 0.75));
        const double tileWidth = std::max(4.0, tileHeight * aspect);
        const double stepX = static_cast<double>(outputSize.width) / (count + 1);
        const double stepY = outputSize.height * 0.18;
        const int snappedStep = static_cast<int>(std::floor(
            localFrame * std::max(0.1, std::abs(speed)) / 6.0));
        for (int index = 0; index < count; ++index) {
            const int sequenced = (snappedStep + index) % count;
            const double x = speed < 0.0
                ? outputSize.width - (sequenced + 1) * stepX - tileWidth * 0.5
                : (sequenced + 1) * stepX - tileWidth * 0.5;
            const double y = outputSize.height * 0.5 - tileHeight * 0.5 +
                std::sin(index * 1.57079632679) * stepY;
            draw(x, y, tileWidth, tileHeight);
        }
    }
    return result;
}

ImageBuffer renderVulkan3DSynthEffect(const ImageBuffer& source,
                                     const EditorClip& clip,
                                     int localFrame,
                                     SizeI outputSize)
{
    ImageBuffer result = makeSolidImage(outputSize, 0, 0, 0);
    std::fill(result.bytes.begin(), result.bytes.end(), 0);
    if (source.empty()) return result;
    struct SynthNode {
        double x = 0.0;
        double y = 0.0;
        double width = 0.0;
        double height = 0.0;
        double rotation = 0.0;
        double opacity = 1.0;
        double cameraDepth = 0.0;
    };
    constexpr double kTwoPi = 6.28318530717958647692;
    const int count = std::clamp(clip.effectRows, 1, 96);
    const double aspect = static_cast<double>(source.size.width) /
        std::max(1, source.size.height);
    const double scale = std::clamp(clip.effectScale, 0.1, 8.0);
    const double speed = std::clamp(clip.effectSpeed, -8.0, 8.0);
    const double phase = localFrame * speed * 0.018;
    const double outputAspect = static_cast<double>(outputSize.width) /
        std::max(1, outputSize.height);
    const double perspective = 1.0 / std::tan(44.0 * 3.14159265358979323846 / 360.0);
    std::vector<SynthNode> nodes;
    nodes.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        const double t = count <= 1 ? 0.0 : static_cast<double>(index) / (count - 1);
        const double angle = phase + kTwoPi * 2.0 * t;
        const double lane = -0.88 + 1.76 * ((index + 0.5) / count);
        const double radius = 1.28 * std::clamp(scale, 0.35, 3.5);
        const double worldX = std::sin(angle) * radius;
        const double worldY = lane + std::sin(angle * 1.7 + phase * 2.3) * 0.34;
        const double worldZ = std::cos(angle) * 1.12;
        const double cameraDepth = 5.4 - worldZ;
        const double ndcX = perspective / outputAspect * worldX / cameraDepth;
        const double ndcY = perspective * worldY / cameraDepth;
        const double centerX = (ndcX + 1.0) * 0.5 * outputSize.width;
        const double centerY = (1.0 - (ndcY + 1.0) * 0.5) * outputSize.height;
        const double depthScale = std::clamp(5.4 / std::max(0.001, cameraDepth), 0.46, 1.58);
        const double pulse = 0.82 + 0.18 * std::sin(phase * 3.0 + index * 0.9);
        const double tileHeight = std::max(
            4.0, std::min(outputSize.width, outputSize.height) * 0.13 *
                     scale * depthScale * pulse);
        const double tileWidth = std::max(4.0, tileHeight * aspect);
        nodes.push_back({
            centerX - tileWidth * 0.5,
            centerY - tileHeight * 0.5,
            tileWidth,
            tileHeight,
            (clip.effectAlternateDirection ? worldZ : -worldZ) * 32.0 +
                std::sin(angle + phase) * 9.0,
            std::clamp(0.56 + depthScale * 0.32 + worldZ * 0.18, 0.22, 1.0),
            cameraDepth});
    }
    std::sort(nodes.begin(), nodes.end(), [](const SynthNode& left, const SynthNode& right) {
        return left.cameraDepth > right.cameraDepth;
    });
    for (const SynthNode& node : nodes) {
        ImageBuffer tile = resizeImageNearest(source, {
            std::max(1, static_cast<int>(std::lround(node.width))),
            std::max(1, static_cast<int>(std::lround(node.height)))});
        for (int y = 0; y < tile.size.height; ++y) {
            for (int x = 0; x < tile.size.width; ++x) {
                const std::size_t alphaOffset = static_cast<std::size_t>(
                    y * tile.strideBytes + x * 4 + 3);
                tile.bytes[alphaOffset] = static_cast<std::uint8_t>(std::clamp(
                    std::lround(tile.bytes[alphaOffset] * node.opacity), 0L, 255L));
            }
        }
        jcut::EditorTransformKeyframe transform;
        transform.rotation = node.rotation;
        blitTransformedImage(
            tile, &result,
            static_cast<int>(std::lround(node.x)),
            static_cast<int>(std::lround(node.y)),
            transform);
    }
    return result;
}

ImageBuffer renderEdgeStretchEffect(const ImageBuffer& source,
                                    const EditorClip& clip,
                                    SizeI outputSize)
{
    ImageBuffer result = makeSolidImage(outputSize, 0, 0, 0);
    std::fill(result.bytes.begin(), result.bytes.end(), 0);
    if (source.empty()) return result;
    const double left = (outputSize.width - source.size.width) * 0.5;
    const double top = (outputSize.height - source.size.height) * 0.5;
    const double centerX = outputSize.width * 0.5;
    const double centerY = outputSize.height * 0.5;
    const double halfWidth = source.size.width * 0.5;
    const double halfHeight = source.size.height * 0.5;
    const bool legacyProgressivePreset =
        clip.effectPreset == "progressive_edge_stretch";
    const bool progressive =
        legacyProgressivePreset ||
        clip.edgeFillEffect == "progressive_edge_stretch";
    const bool bidirectional =
        !legacyProgressivePreset &&
        clip.edgeFillEffect == "progressive_bidirectional_edge_stretch";
    const bool tile = !legacyProgressivePreset && clip.edgeFillEffect == "tile";
    const bool mirror = !legacyProgressivePreset && clip.edgeFillEffect == "mirror";
    const double bandPixels = std::clamp(
        static_cast<double>(legacyProgressivePreset
                                ? clip.effectRows
                                : clip.edgeFillPixels),
        1.0, 512.0);
    const double power = std::max(
        0.25,
        legacyProgressivePreset ? clip.effectScale : clip.edgeFillPower);
    const double fillOpacity = legacyProgressivePreset
        ? 1.0
        : std::clamp(clip.edgeFillOpacity, 0.0, 1.0);
    const double fillBrightness = legacyProgressivePreset
        ? 0.0
        : std::clamp(clip.edgeFillBrightness, -1.0, 1.0);
    const double fillSaturation = legacyProgressivePreset
        ? 1.0
        : std::clamp(clip.edgeFillSaturation, 0.0, 3.0);
    if (bidirectional) {
        blitImage(source, &result,
                  static_cast<int>(std::lround(left)),
                  static_cast<int>(std::lround(top)));
    }
    for (int y = 0; y < outputSize.height; ++y) {
        for (int x = 0; x < outputSize.width; ++x) {
            if (x + 0.5 >= left && x + 0.5 <= left + source.size.width &&
                y + 0.5 >= top && y + 0.5 <= top + source.size.height &&
                !bidirectional) {
                continue;
            }
            const double dx = x + 0.5 - centerX;
            const double dy = y + 0.5 - centerY;
            const double ratioX = std::abs(dx) / std::max(0.5, halfWidth);
            const double ratioY = std::abs(dy) / std::max(0.5, halfHeight);
            const double radialScale = bidirectional
                ? std::pow(std::pow(ratioX, 4.0) + std::pow(ratioY, 4.0), 0.25)
                : std::max(ratioX, ratioY);
            const double outsideScale = std::max(1.0, radialScale);
            const double mediaScale = 1.0 / outsideScale;
            const double edgeX = centerX + dx * mediaScale;
            const double edgeY = centerY + dy * mediaScale;
            const double canvasScaleX = std::abs(dx) < 0.0001
                ? std::numeric_limits<double>::infinity()
                : (dx > 0.0 ? (outputSize.width - centerX) / dx : -centerX / dx);
            const double canvasScaleY = std::abs(dy) < 0.0001
                ? std::numeric_limits<double>::infinity()
                : (dy > 0.0 ? (outputSize.height - centerY) / dy : -centerY / dy);
            const double canvasScale = std::min(canvasScaleX, canvasScaleY);
            const double fill = std::clamp(
                (1.0 - mediaScale) /
                    std::max(0.0001, canvasScale - mediaScale),
                0.0, 1.0);
            const double scan = progressive ? std::pow(fill, power) : 0.0;
            double sourceX = edgeX - left;
            double sourceY = edgeY - top;
            double overlayAlpha = 1.0;
            double bidirectionalPolarX = 0.0;
            double bidirectionalPolarY = 0.0;
            double bidirectionalSampleRadius = 0.0;
            if (tile || mirror) {
                const auto wrap = [](double value) {
                    return value - std::floor(value);
                };
                const auto mirrorWrap = [&](double value) {
                    const double wrapped = wrap(std::abs(value) * 0.5) * 2.0;
                    return wrapped <= 1.0 ? wrapped : 2.0 - wrapped;
                };
                const double u = (x + 0.5 - left) / source.size.width;
                const double v = (y + 0.5 - top) / source.size.height;
                sourceX = (tile ? wrap(u) : mirrorWrap(u)) * source.size.width - 0.5;
                sourceY = (tile ? wrap(v) : mirrorWrap(v)) * source.size.height - 0.5;
            } else if (bidirectional && radialScale > 0.000001) {
                const double normalizedX = dx / std::max(0.5, halfWidth);
                const double normalizedY = dy / std::max(0.5, halfHeight);
                const double polarLength =
                    std::max(0.000001, std::hypot(normalizedX, normalizedY));
                bidirectionalPolarX = normalizedX / polarLength;
                bidirectionalPolarY = normalizedY / polarLength;
                const double directionX = normalizedX / radialScale;
                const double directionY = normalizedY / radialScale;
                const double boundaryPixelRadius = std::hypot(
                    directionX * halfWidth,
                    directionY * halfHeight);
                const double coreRadius = std::clamp(
                    1.0 - bandPixels / std::max(1.0, boundaryPixelRadius),
                    0.05,
                    0.995);
                if (radialScale <= coreRadius) {
                    continue;
                }
                const double canvasRadius = std::max(
                    radialScale,
                    radialScale * canvasScale);
                const double stretch = std::clamp(
                    (radialScale - coreRadius) /
                        std::max(0.0001, canvasRadius - coreRadius),
                    0.0,
                    1.0);
                const double sampleRadius =
                    coreRadius + (1.0 - coreRadius) * std::pow(stretch, power);
                bidirectionalSampleRadius = sampleRadius;
                sourceX =
                    (0.5 + directionX * sampleRadius * 0.5) * source.size.width - 0.5;
                sourceY =
                    (0.5 + directionY * sampleRadius * 0.5) * source.size.height - 0.5;
                const double featherWidth =
                    std::max(0.002, (1.0 - coreRadius) * 0.25);
                const double featherT = std::clamp(
                    (radialScale - coreRadius) / featherWidth,
                    0.0,
                    1.0);
                overlayAlpha = featherT * featherT * (3.0 - 2.0 * featherT);
            } else if (ratioX >= ratioY) {
                sourceX = dx < 0.0
                    ? scan * bandPixels
                    : source.size.width - 1.0 - scan * bandPixels;
            } else {
                sourceY = dy < 0.0
                    ? scan * bandPixels
                    : source.size.height - 1.0 - scan * bandPixels;
            }
            std::array<double, 4> color = sampleImageBilinear(
                source,
                (sourceX + 0.5) / source.size.width,
                (sourceY + 0.5) / source.size.height);
            const double minClipPixels =
                std::min(source.size.width, source.size.height);
            if (bidirectional && minClipPixels < 128.0) {
                const double angularStep = std::clamp(
                    2.0 / std::max(1.0, minClipPixels),
                    0.0,
                    0.18);
                const auto sampleRing = [&](double angle) {
                    const double cosine = std::cos(angle);
                    const double sine = std::sin(angle);
                    const double polarX =
                        cosine * bidirectionalPolarX - sine * bidirectionalPolarY;
                    const double polarY =
                        sine * bidirectionalPolarX + cosine * bidirectionalPolarY;
                    const double norm = std::pow(
                        std::pow(std::abs(polarX), 4.0) +
                            std::pow(std::abs(polarY), 4.0),
                        0.25);
                    return sampleImageBilinear(
                        source,
                        0.5 + (polarX / norm) * bidirectionalSampleRadius * 0.5,
                        0.5 + (polarY / norm) * bidirectionalSampleRadius * 0.5);
                };
                const auto positive = sampleRing(angularStep);
                const auto negative = sampleRing(-angularStep);
                const auto positiveWide = sampleRing(2.0 * angularStep);
                const auto negativeWide = sampleRing(-2.0 * angularStep);
                for (int channel = 0; channel < 4; ++channel) {
                    color[channel] =
                        color[channel] * 0.4 +
                        (positive[channel] + negative[channel]) * 0.2 +
                        (positiveWide[channel] + negativeWide[channel]) * 0.1;
                }
            }
            const double luma =
                color[0] * 0.2126 + color[1] * 0.7152 + color[2] * 0.0722;
            std::uint8_t adjustedColor[3]{};
            for (int channel = 0; channel < 3; ++channel) {
                const double adjusted =
                    (luma + (color[channel] - luma) * fillSaturation) +
                    fillBrightness;
                adjustedColor[channel] = static_cast<std::uint8_t>(
                    std::clamp(std::lround(adjusted * 255.0), 0L, 255L));
            }
            const std::uint8_t alpha = static_cast<std::uint8_t>(
                std::clamp(
                    std::lround(color[3] * fillOpacity * overlayAlpha * 255.0),
                    0L,
                    255L));
            if (bidirectional) {
                blendRgbaPixel(
                    &result,
                    x,
                    y,
                    adjustedColor[0],
                    adjustedColor[1],
                    adjustedColor[2],
                    alpha);
            } else {
                const std::size_t offset = static_cast<std::size_t>(
                    y * result.strideBytes + x * 4);
                result.bytes[offset] = adjustedColor[0];
                result.bytes[offset + 1] = adjustedColor[1];
                result.bytes[offset + 2] = adjustedColor[2];
                result.bytes[offset + 3] = alpha;
            }
        }
    }
    if (!bidirectional) {
        blitImage(source, &result,
                  static_cast<int>(std::lround(left)),
                  static_cast<int>(std::lround(top)));
    }
    return result;
}

void squaredDistanceTransform1D(const std::vector<double>& input,
                                std::vector<double>* output)
{
    const int size = static_cast<int>(input.size());
    if (!output || size == 0) return;
    output->resize(input.size());
    std::vector<int> locations(static_cast<std::size_t>(size));
    std::vector<double> boundaries(static_cast<std::size_t>(size + 1));
    int envelope = 0;
    locations[0] = 0;
    boundaries[0] = -std::numeric_limits<double>::infinity();
    boundaries[1] = std::numeric_limits<double>::infinity();
    for (int q = 1; q < size; ++q) {
        double intersection = 0.0;
        do {
            const int location = locations[envelope];
            intersection = ((input[q] + q * q) -
                            (input[location] + location * location)) /
                (2.0 * (q - location));
            if (intersection <= boundaries[envelope]) --envelope;
            else break;
        } while (envelope >= 0);
        envelope = std::max(0, envelope + 1);
        locations[envelope] = q;
        boundaries[envelope] = intersection;
        boundaries[envelope + 1] = std::numeric_limits<double>::infinity();
    }
    envelope = 0;
    for (int q = 0; q < size; ++q) {
        while (boundaries[envelope + 1] < q) ++envelope;
        const double delta = q - locations[envelope];
        (*output)[q] = delta * delta + input[locations[envelope]];
    }
}

std::vector<double> maskDistanceField(const std::vector<std::uint8_t>& mask, SizeI size)
{
    constexpr double kFar = 1.0e12;
    std::vector<double> horizontal(mask.size(), kFar);
    std::vector<double> result(mask.size(), kFar);
    std::vector<double> input;
    std::vector<double> output;
    input.resize(static_cast<std::size_t>(std::max(size.width, size.height)));
    for (int y = 0; y < size.height; ++y) {
        input.resize(static_cast<std::size_t>(size.width));
        for (int x = 0; x < size.width; ++x) {
            input[static_cast<std::size_t>(x)] =
                mask[static_cast<std::size_t>(y * size.width + x)] > 127 ? 0.0 : kFar;
        }
        squaredDistanceTransform1D(input, &output);
        std::copy(output.begin(), output.end(),
                  horizontal.begin() + static_cast<std::ptrdiff_t>(y * size.width));
    }
    for (int x = 0; x < size.width; ++x) {
        input.resize(static_cast<std::size_t>(size.height));
        for (int y = 0; y < size.height; ++y) {
            input[static_cast<std::size_t>(y)] =
                horizontal[static_cast<std::size_t>(y * size.width + x)];
        }
        squaredDistanceTransform1D(input, &output);
        for (int y = 0; y < size.height; ++y) {
            result[static_cast<std::size_t>(y * size.width + x)] =
                std::sqrt(std::max(0.0, output[static_cast<std::size_t>(y)]));
        }
    }
    return result;
}

void applySpeakerMaskEffect(ImageBuffer* image,
                            const std::vector<std::uint8_t>& mask,
                            const EditorClip& clip,
                            int localFrame,
                            const std::array<RgbaColor, 3>& palette)
{
    if (!image || image->empty() || mask.size() !=
        static_cast<std::size_t>(image->size.width * image->size.height)) return;
    const std::vector<double> distance = maskDistanceField(mask, image->size);
    const double radius = std::clamp(static_cast<double>(clip.effectRows), 1.0, 8.0);
    const double opacity = std::clamp(clip.effectScale, 0.0, 1.0);
    const double cycle = localFrame * std::clamp(clip.effectSpeed, -8.0, 8.0) * 0.04;
    const double spacing = std::max(1.0, clip.tilingSpacing * 8.0);
    for (int y = 0; y < image->size.height; ++y) {
        for (int x = 0; x < image->size.width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y * image->size.width + x);
            const double centerMask = mask[index] / 255.0;
            const double normalized = std::clamp(distance[index] - (radius - 1.0), 0.0, 1.0);
            const double dilation = 1.0 - normalized * normalized * (3.0 - 2.0 * normalized);
            const double amount = dilation * (1.0 - centerMask) * opacity;
            if (amount <= 0.0) continue;
            int colorIndex = 0;
            if (clip.effectPreset == "speaker_mask_dilation") {
                colorIndex = static_cast<int>(std::floor(x / spacing + cycle)) % 3;
            } else if (clip.effectPreset == "speaker_mask_dilation_pulse") {
                colorIndex = static_cast<int>(std::floor((x + y) / spacing + cycle)) % 3;
            } else {
                colorIndex = static_cast<int>(std::floor(
                    distance[index] / std::max(1.0, spacing * 0.25) + cycle)) % 3;
            }
            if (colorIndex < 0) colorIndex += 3;
            const std::size_t offset = static_cast<std::size_t>(
                y * image->strideBytes + x * 4);
            for (int channel = 0; channel < 3; ++channel) {
                const double original = image->bytes[offset + channel] / 255.0;
                image->bytes[offset + channel] = static_cast<std::uint8_t>(std::clamp(
                    std::lround((original * (1.0 - amount) +
                                 (channel == 0
                                      ? palette[colorIndex].red
                                      : (channel == 1
                                           ? palette[colorIndex].green
                                           : palette[colorIndex].blue)) /
                                     255.0 * amount) * 255.0),
                    0L, 255L));
            }
            image->bytes[offset + 3] = std::max(
                image->bytes[offset + 3],
                static_cast<std::uint8_t>(std::clamp(
                    std::lround(amount * 255.0), 0L, 255L)));
        }
    }
}

bool isSpeakerMaskEffect(std::string_view preset)
{
    return preset == "speaker_mask_dilation" ||
        preset == "speaker_mask_dilation_pulse" ||
        preset == "speaker_mask_dilation_rings";
}

void applyDifferenceMatte(ImageBuffer* current,
                          const ImageBuffer& reference,
                          double thresholdValue,
                          double softnessValue)
{
    if (!current || current->empty() || reference.empty() ||
        current->size.width != reference.size.width ||
        current->size.height != reference.size.height) return;
    const double threshold = std::clamp(thresholdValue, 0.0, 1.0);
    const double softness = std::max(0.00001, std::clamp(softnessValue, 0.0, 1.0));
    const double edge0 = threshold - softness;
    const double edge1 = threshold + softness;
    for (int y = 0; y < current->size.height; ++y) {
        for (int x = 0; x < current->size.width; ++x) {
            const std::size_t currentOffset = static_cast<std::size_t>(
                y * current->strideBytes + x * 4);
            const std::size_t referenceOffset = static_cast<std::size_t>(
                y * reference.strideBytes + x * 4);
            double difference = 0.0;
            for (int channel = 0; channel < 3; ++channel) {
                difference = std::max(
                    difference,
                    std::abs(static_cast<double>(current->bytes[currentOffset + channel]) -
                             reference.bytes[referenceOffset + channel]) / 255.0);
            }
            const double normalized = std::clamp(
                (difference - edge0) / std::max(0.00001, edge1 - edge0), 0.0, 1.0);
            const double matte = normalized * normalized * (3.0 - 2.0 * normalized);
            const std::uint8_t value = static_cast<std::uint8_t>(
                std::clamp(std::lround(matte * 255.0), 0L, 255L));
            current->bytes[currentOffset] = value;
            current->bytes[currentOffset + 1] = value;
            current->bytes[currentOffset + 2] = value;
        }
    }
}

double fractValue(double value)
{
    return value - std::floor(value);
}

double mirroredCoordinate(double value)
{
    const double wrapped = std::fmod(std::abs(value), 2.0);
    return wrapped <= 1.0 ? wrapped : 2.0 - wrapped;
}

std::array<double, 4> sampleImageBilinear(const ImageBuffer& image, double u, double v)
{
    u = std::clamp(u, 0.0, 1.0);
    v = std::clamp(v, 0.0, 1.0);
    const double sourceX = u * image.size.width - 0.5;
    const double sourceY = v * image.size.height - 0.5;
    const int x0 = std::clamp(static_cast<int>(std::floor(sourceX)), 0, image.size.width - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(sourceY)), 0, image.size.height - 1);
    const int x1 = std::min(image.size.width - 1, x0 + 1);
    const int y1 = std::min(image.size.height - 1, y0 + 1);
    const double tx = std::clamp(sourceX - std::floor(sourceX), 0.0, 1.0);
    const double ty = std::clamp(sourceY - std::floor(sourceY), 0.0, 1.0);
    std::array<double, 4> result{};
    for (int channel = 0; channel < 4; ++channel) {
        auto value = [&](int x, int y) {
            return image.bytes[static_cast<std::size_t>(
                y * image.strideBytes + x * 4 + channel)] / 255.0;
        };
        const double top = value(x0, y0) * (1.0 - tx) + value(x1, y0) * tx;
        const double bottom = value(x0, y1) * (1.0 - tx) + value(x1, y1) * tx;
        result[channel] = top * (1.0 - ty) + bottom * ty;
    }
    return result;
}

double luma(const std::array<double, 4>& color)
{
    return color[0] * 0.2126 + color[1] * 0.7152 + color[2] * 0.0722;
}

bool isStandalonePixelEffect(std::string_view preset)
{
    static constexpr std::array<std::string_view, 20> kPresets = {
        "mirror_ring", "kaleidoscope", "quad_mirror", "infinite_mirror",
        "tessellation", "hexagonal_prism", "droste", "polar_tunnel",
        "tiny_planet", "twirl_vortex", "ripple_shockwave", "displacement_map",
        "glass_refraction", "slit_scan", "pixel_sorting", "datamosh_glitch",
        "rgb_split", "halftone_mosaic", "sobel_edges", "neon_glow"};
    return std::find(kPresets.begin(), kPresets.end(), preset) != kPresets.end();
}

ImageBuffer renderStandalonePixelEffect(const ImageBuffer& source,
                                        const EditorClip& clip,
                                        int localFrame,
                                        SizeI outputSize)
{
    const ImageBuffer input = resizeImageNearest(source, outputSize);
    ImageBuffer output = makeSolidImage(outputSize, 0, 0, 0);
    if (input.empty()) return output;
    constexpr double kTwoPi = 6.28318530718;
    const double strength = std::clamp(clip.effectScale, 0.1, 8.0);
    const double radius = std::clamp(static_cast<double>(clip.effectRows), 1.0, 4.0);
    const double effectFrame = localFrame * std::clamp(clip.effectSpeed, -8.0, 8.0);
    for (int y = 0; y < outputSize.height; ++y) {
        for (int x = 0; x < outputSize.width; ++x) {
            const double u = (x + 0.5) / outputSize.width;
            const double v = (y + 0.5) / outputSize.height;
            double sampleU = u;
            double sampleV = v;
            const double px = u - 0.5;
            const double py = v - 0.5;
            const double distance = std::hypot(px, py);
            const double angle = std::atan2(py, px);
            if (clip.effectPreset == "mirror_ring") {
                constexpr double sectors = 12.0;
                const double wedge = kTwoPi / sectors;
                const double folded = std::abs(
                    std::fmod(angle + wedge * 0.5 + kTwoPi * 8.0, wedge) - wedge * 0.5);
                const double radial = mirroredCoordinate(distance * 2.0) * 0.5;
                sampleU = 0.5 + std::cos(folded) * radial;
                sampleV = 0.5 + std::sin(folded) * radial;
            } else if (clip.effectPreset == "tessellation") {
                const double tileX = u * 6.0;
                const double tileY = v * 6.0;
                const double cellX = std::floor(tileX);
                const double cellY = std::floor(tileY);
                double localX = fractValue(tileX);
                double localY = fractValue(tileY);
                if (std::fmod(cellX + cellY, 2.0) > 0.5) localX = 1.0 - localX;
                if (localX + localY > 1.0) {
                    const double oldX = localX;
                    localX = 1.0 - localY;
                    localY = 1.0 - oldX;
                }
                sampleU = localX + 0.5 * localY;
                sampleV = 0.86602540378 * localY;
            } else if (clip.effectPreset == "kaleidoscope") {
                const double wedge = kTwoPi / 16.0;
                const double folded = std::abs(
                    std::fmod(angle + wedge * 0.5 + kTwoPi * 8.0, wedge) - wedge * 0.5);
                sampleU = 0.5 + std::cos(folded) * distance;
                sampleV = 0.5 + std::sin(folded) * distance;
            } else if (clip.effectPreset == "hexagonal_prism") {
                const double gridX = u * 5.0 - v * 2.5;
                const double gridY = v * 5.0 * 0.8660254;
                const double hx = std::abs(fractValue(gridX) - 0.5);
                const double hy = std::abs(fractValue(gridY) - 0.5);
                sampleU = 0.5 + (hx + hy * 0.5) * 0.85;
                sampleV = 0.5 + hy * 0.85;
            } else if (clip.effectPreset == "droste") {
                const double r = std::max(distance, 0.0001);
                const double a = angle + std::log(r) * 0.7;
                const double rr = std::exp(fractValue(std::log(r) / std::log(2.0)) *
                                           std::log(2.0)) * 0.24;
                sampleU = 0.5 + std::cos(a) * rr;
                sampleV = 0.5 + std::sin(a) * rr;
            } else if (clip.effectPreset == "polar_tunnel") {
                sampleU = fractValue(angle / kTwoPi + 0.5);
                sampleV = fractValue(0.16 / std::max(distance, 0.025));
            } else if (clip.effectPreset == "tiny_planet") {
                sampleU = fractValue(angle / kTwoPi + 0.5);
                sampleV = std::clamp(1.15 - distance * 1.75, 0.0, 1.0);
            } else if (clip.effectPreset == "infinite_mirror") {
                const double band = fractValue(-std::log2(
                    std::max(std::max(std::abs(px), std::abs(py)), 0.001)));
                const double length = std::hypot(px + 0.0001, py + 0.0001);
                sampleU = 0.5 + (px + 0.0001) / length * band * 0.48;
                sampleV = 0.5 + (py + 0.0001) / length * band * 0.48;
            } else if (clip.effectPreset == "quad_mirror") {
                sampleU = 0.5 + std::abs(px);
                sampleV = 0.5 + std::abs(py);
            } else if (clip.effectPreset == "slit_scan") {
                const double band = std::floor(v * 96.0);
                sampleU = mirroredCoordinate(u + std::sin(band * 0.37) * 0.22);
            } else if (clip.effectPreset == "displacement_map") {
                sampleU = u + std::sin(v * 31.0) * 0.025;
                sampleV = v + std::cos(u * 27.0) * 0.025;
            } else if (clip.effectPreset == "twirl_vortex") {
                const double smooth = std::clamp(distance / 0.72, 0.0, 1.0);
                const double smoothstep = smooth * smooth * (3.0 - 2.0 * smooth);
                const double a = angle + (1.0 - smoothstep) * 5.0;
                sampleU = 0.5 + std::cos(a) * distance;
                sampleV = 0.5 + std::sin(a) * distance;
            } else if (clip.effectPreset == "ripple_shockwave") {
                const double length = std::hypot(px + 0.0001, py + 0.0001);
                const double displacement = std::sin(distance * 70.0) * 0.018;
                sampleU = u + (px + 0.0001) / length * displacement;
                sampleV = v + (py + 0.0001) / length * displacement;
            } else if (clip.effectPreset == "pixel_sorting") {
                const double column = std::floor(u * 160.0);
                sampleV = fractValue(v + fractValue(std::sin(column * 91.73) * 43758.5) * 0.45);
            } else if (clip.effectPreset == "datamosh_glitch") {
                const double blockX = std::floor(u * 24.0);
                const double blockY = std::floor(v * 14.0);
                const double hash = fractValue(std::sin(blockX * 12.9898 + blockY * 78.233) *
                                               43758.5453);
                if (hash >= 0.68) sampleU = mirroredCoordinate(u + (hash - 0.5) * 0.35);
            } else if (clip.effectPreset == "halftone_mosaic") {
                sampleU = (std::floor(u * 120.0) + 0.5) / 120.0;
                sampleV = (std::floor(v * 120.0) + 0.5) / 120.0;
            } else if (clip.effectPreset == "glass_refraction") {
                const double cellX = std::floor(u * 18.0);
                const double cellY = std::floor(v * 18.0);
                const double localX = fractValue(u * 18.0) - 0.5;
                const double localY = fractValue(v * 18.0) - 0.5;
                const double hash = fractValue(std::sin(cellX * 41.7 + cellY * 289.1) * 43758.5);
                const double length = std::hypot(localX + 0.001, localY + 0.001);
                sampleU = u + (localX + 0.001) / length * (hash - 0.5) * 0.035;
                sampleV = v + (localY + 0.001) / length * (hash - 0.5) * 0.035;
            }

            std::array<double, 4> color = sampleImageBilinear(input, sampleU, sampleV);
            if (clip.effectPreset == "rgb_split") {
                const double radialU = px * 0.035;
                const double radialV = py * 0.035;
                color[0] = sampleImageBilinear(input, u + radialU, v + radialV)[0];
                color[2] = sampleImageBilinear(input, u - radialU, v - radialV)[2];
            } else if (clip.effectPreset == "halftone_mosaic") {
                const double luminance = luma(color);
                const double dotX = fractValue(u * 120.0) - 0.5;
                const double dotY = fractValue(v * 120.0) - 0.5;
                const double start = std::sqrt(std::max(0.0, luminance)) * 0.48;
                const double normalized = std::clamp(
                    (std::hypot(dotX, dotY) - start) / 0.08, 0.0, 1.0);
                const double ink = 1.0 - normalized * normalized * (3.0 - 2.0 * normalized);
                color[0] = color[1] = color[2] = ink;
            } else if (clip.effectPreset == "sobel_edges" ||
                       clip.effectPreset == "neon_glow") {
                const double du = radius / input.size.width;
                const double dv = radius / input.size.height;
                auto lumAt = [&](int dx, int dy) {
                    return luma(sampleImageBilinear(input, u + dx * du, v + dy * dv));
                };
                const double tl = lumAt(-1, -1), tc = lumAt(0, -1), tr = lumAt(1, -1);
                const double ml = lumAt(-1, 0), mr = lumAt(1, 0);
                const double bl = lumAt(-1, 1), bc = lumAt(0, 1), br = lumAt(1, 1);
                const double gx = -tl - 2.0 * ml - bl + tr + 2.0 * mr + br;
                const double gy = -tl - 2.0 * tc - tr + bl + 2.0 * bc + br;
                const double edge = std::clamp(std::hypot(gx, gy) * strength, 0.0, 1.0);
                if (clip.effectPreset == "sobel_edges") {
                    color[0] = color[1] = color[2] = edge;
                } else {
                    for (int channel = 0; channel < 3; ++channel) {
                        const double neon = 0.5 + 0.5 * std::cos(
                            (channel == 0 ? 0.0 : (channel == 1 ? 2.1 : 4.2)) +
                            edge * 8.0 + effectFrame * 0.03);
                        color[channel] = std::clamp(
                            color[channel] * 0.18 + neon * edge * strength, 0.0, 1.0);
                    }
                }
            }
            const std::size_t offset = static_cast<std::size_t>(
                y * output.strideBytes + x * 4);
            for (int channel = 0; channel < 4; ++channel) {
                output.bytes[offset + channel] = static_cast<std::uint8_t>(
                    std::clamp(std::lround(color[channel] * 255.0), 0L, 255L));
            }
        }
    }
    return output;
}

void blitTransformedImage(
    const ImageBuffer& source,
    ImageBuffer* destination,
    int offsetX,
    int offsetY,
    const jcut::EditorTransformKeyframe& transform)
{
    if (!destination || source.empty() || destination->empty()) {
        return;
    }
    const bool identity =
        std::abs(transform.translationX) < 0.0001 &&
        std::abs(transform.translationY) < 0.0001 &&
        std::abs(transform.rotation) < 0.0001 &&
        std::abs(transform.scaleX - 1.0) < 0.0001 &&
        std::abs(transform.scaleY - 1.0) < 0.0001;
    if (identity) {
        blitImage(source, destination, offsetX, offsetY);
        return;
    }

    ImageBuffer transformed;
    transformed.format = PixelFormat::Rgba8;
    transformed.size = destination->size;
    transformed.strideBytes = transformed.size.width * 4;
    transformed.bytes.assign(
        static_cast<std::size_t>(
            transformed.strideBytes * transformed.size.height),
        0);
    const double radians = transform.rotation *
        3.14159265358979323846 / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const double centerX = offsetX + source.size.width * 0.5 +
        transform.translationX;
    const double centerY = offsetY + source.size.height * 0.5 +
        transform.translationY;
    for (int y = 0; y < transformed.size.height; ++y) {
        for (int x = 0; x < transformed.size.width; ++x) {
            const double translatedX = x + 0.5 - centerX;
            const double translatedY = y + 0.5 - centerY;
            const double unrotatedX =
                cosine * translatedX + sine * translatedY;
            const double unrotatedY =
                -sine * translatedX + cosine * translatedY;
            const int sourceX = static_cast<int>(std::floor(
                unrotatedX / transform.scaleX + source.size.width * 0.5));
            const int sourceY = static_cast<int>(std::floor(
                unrotatedY / transform.scaleY + source.size.height * 0.5));
            if (sourceX < 0 || sourceX >= source.size.width ||
                sourceY < 0 || sourceY >= source.size.height) {
                continue;
            }
            const std::size_t sourceOffset = static_cast<std::size_t>(
                sourceY * source.strideBytes + sourceX * 4);
            const std::size_t destinationOffset = static_cast<std::size_t>(
                y * transformed.strideBytes + x * 4);
            std::copy_n(
                source.bytes.data() + sourceOffset, 4,
                transformed.bytes.data() + destinationOffset);
        }
    }
    blitImage(transformed, destination, 0, 0);
}

struct HslColor {
    double hue = 0.0;
    double saturation = 0.0;
    double lightness = 0.0;
};

HslColor rgbToHsl(double red, double green, double blue)
{
    const double maximum = std::max({red, green, blue});
    const double minimum = std::min({red, green, blue});
    HslColor result;
    result.lightness = (maximum + minimum) * 0.5;
    if (maximum == minimum) {
        return result;
    }
    const double delta = maximum - minimum;
    result.saturation = result.lightness > 0.5
        ? delta / (2.0 - maximum - minimum)
        : delta / (maximum + minimum);
    if (maximum == red) {
        result.hue = (green - blue) / delta + (green < blue ? 6.0 : 0.0);
    } else if (maximum == green) {
        result.hue = (blue - red) / delta + 2.0;
    } else {
        result.hue = (red - green) / delta + 4.0;
    }
    result.hue /= 6.0;
    return result;
}

double hueChannel(double p, double q, double value)
{
    if (value < 0.0) value += 1.0;
    if (value > 1.0) value -= 1.0;
    if (value < 1.0 / 6.0) return p + (q - p) * 6.0 * value;
    if (value < 0.5) return q;
    if (value < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - value) * 6.0;
    return p;
}

void hslToRgb(const HslColor& hsl, double* red, double* green, double* blue)
{
    if (hsl.saturation <= 0.0) {
        *red = *green = *blue = hsl.lightness;
        return;
    }
    const double q = hsl.lightness < 0.5
        ? hsl.lightness * (1.0 + hsl.saturation)
        : hsl.lightness + hsl.saturation - hsl.lightness * hsl.saturation;
    const double p = 2.0 * hsl.lightness - q;
    *red = hueChannel(p, q, hsl.hue + 1.0 / 3.0);
    *green = hueChannel(p, q, hsl.hue);
    *blue = hueChannel(p, q, hsl.hue - 1.0 / 3.0);
}

bool identityCurve(const std::vector<jcut::EditorPoint>& points)
{
    return jcut::editorGradingCurveIsIdentity(points);
}

void applyClipGrade(ImageBuffer* image,
                    const jcut::EditorGradingKeyframe& grade,
                    bool forceOpaque)
{
    if (!image || image->empty()) {
        return;
    }
    const bool curvesEnabled =
        !identityCurve(grade.curvePointsR) ||
        !identityCurve(grade.curvePointsG) ||
        !identityCurve(grade.curvePointsB) ||
        !identityCurve(grade.curvePointsLuma);
    const std::vector<std::uint8_t> curveR = curvesEnabled
        ? jcut::editorGradingCurveLut8(
              grade.curvePointsR, jcut::kEditorGradingCurveLutSize,
              grade.curveSmoothingEnabled)
        : std::vector<std::uint8_t>{};
    const std::vector<std::uint8_t> curveG = curvesEnabled
        ? jcut::editorGradingCurveLut8(
              grade.curvePointsG, jcut::kEditorGradingCurveLutSize,
              grade.curveSmoothingEnabled)
        : std::vector<std::uint8_t>{};
    const std::vector<std::uint8_t> curveB = curvesEnabled
        ? jcut::editorGradingCurveLut8(
              grade.curvePointsB, jcut::kEditorGradingCurveLutSize,
              grade.curveSmoothingEnabled)
        : std::vector<std::uint8_t>{};
    const std::vector<std::uint8_t> curveLuma = curvesEnabled
        ? jcut::editorGradingCurveLut8(
              grade.curvePointsLuma, jcut::kEditorGradingCurveLutSize,
              grade.curveSmoothingEnabled)
        : std::vector<std::uint8_t>{};

    for (int y = 0; y < image->size.height; ++y) {
        for (int x = 0; x < image->size.width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(
                y * image->strideBytes + x * 4);
            double red = image->bytes[offset] / 255.0;
            double green = image->bytes[offset + 1] / 255.0;
            double blue = image->bytes[offset + 2] / 255.0;
            const double luma = red * 0.2126 + green * 0.7152 + blue * 0.0722;
            const double shadowWeight = std::pow(1.0 - luma, 2.0);
            const double midtoneWeight = 1.0 - std::abs(luma - 0.5) * 2.0;
            const double highlightWeight = std::pow(luma, 2.0);
            red *= 1.0 + grade.shadowsR * shadowWeight;
            green *= 1.0 + grade.shadowsG * shadowWeight;
            blue *= 1.0 + grade.shadowsB * shadowWeight;
            red = std::pow(std::max(0.0, red),
                1.0 / std::max(0.01, 1.0 + grade.midtonesR * midtoneWeight));
            green = std::pow(std::max(0.0, green),
                1.0 / std::max(0.01, 1.0 + grade.midtonesG * midtoneWeight));
            blue = std::pow(std::max(0.0, blue),
                1.0 / std::max(0.01, 1.0 + grade.midtonesB * midtoneWeight));
            red += grade.highlightsR * highlightWeight;
            green += grade.highlightsG * highlightWeight;
            blue += grade.highlightsB * highlightWeight;
            red = std::clamp(red, 0.0, 1.0);
            green = std::clamp(green, 0.0, 1.0);
            blue = std::clamp(blue, 0.0, 1.0);
            if (curvesEnabled) {
                red = curveR[static_cast<std::size_t>(std::clamp(
                    static_cast<int>(red * 255.0 + 0.5), 0, 255))] / 255.0;
                green = curveG[static_cast<std::size_t>(std::clamp(
                    static_cast<int>(green * 255.0 + 0.5), 0, 255))] / 255.0;
                blue = curveB[static_cast<std::size_t>(std::clamp(
                    static_cast<int>(blue * 255.0 + 0.5), 0, 255))] / 255.0;
                const double mappedInput = red * 0.2126 + green * 0.7152 + blue * 0.0722;
                const double mappedLuma = curveLuma[static_cast<std::size_t>(
                    std::clamp(static_cast<int>(mappedInput * 255.0 + 0.5), 0, 255))] / 255.0;
                if (mappedInput > 0.0001) {
                    const double scale = mappedLuma / mappedInput;
                    red *= scale;
                    green *= scale;
                    blue *= scale;
                } else {
                    red = green = blue = mappedLuma;
                }
            }
            red = std::clamp(((red - 0.5) * grade.contrast) + 0.5 + grade.brightness, 0.0, 1.0);
            green = std::clamp(((green - 0.5) * grade.contrast) + 0.5 + grade.brightness, 0.0, 1.0);
            blue = std::clamp(((blue - 0.5) * grade.contrast) + 0.5 + grade.brightness, 0.0, 1.0);
            HslColor hsl = rgbToHsl(red, green, blue);
            hsl.saturation = std::clamp(hsl.saturation * grade.saturation, 0.0, 1.0);
            hslToRgb(hsl, &red, &green, &blue);
            image->bytes[offset] = static_cast<std::uint8_t>(red * 255.0 + 0.5);
            image->bytes[offset + 1] = static_cast<std::uint8_t>(green * 255.0 + 0.5);
            image->bytes[offset + 2] = static_cast<std::uint8_t>(blue * 255.0 + 0.5);
            const double opacity = forceOpaque
                ? 1.0
                : std::clamp(grade.opacity, 0.0, 1.0);
            image->bytes[offset + 3] = static_cast<std::uint8_t>(
                image->bytes[offset + 3] * opacity + 0.5);
        }
    }
}

double streamFps(const AVStream* stream)
{
    if (!stream) {
        return 30.0;
    }
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
        return av_q2d(stream->avg_frame_rate);
    }
    if (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0) {
        return av_q2d(stream->r_frame_rate);
    }
    return 30.0;
}

std::int64_t ptsForFrameIndex(int frameIndex, const AVStream* stream)
{
    const double fps = std::max(0.001, streamFps(stream));
    const double seconds = static_cast<double>(std::max(0, frameIndex)) / fps;
    const double ticks = seconds / av_q2d(stream->time_base);
    return static_cast<std::int64_t>(std::llround(ticks));
}

struct ActiveVisualClip {
    const EditorClip* clip = nullptr;
    const jcut::EditorTrack* track = nullptr;
    int trackOrder = 0;
};

std::vector<ActiveVisualClip> activeVisualClips(
    const EditorDocumentCore& document,
    double timelineFrame)
{
    std::unordered_map<int, std::pair<int, const jcut::EditorTrack*>> trackOrder;
    for (std::size_t i = 0; i < document.tracks.size(); ++i) {
        trackOrder.emplace(
            document.tracks[i].id,
            std::make_pair(static_cast<int>(i), &document.tracks[i]));
    }
    std::vector<ActiveVisualClip> result;
    for (const EditorClip& clip : document.clips) {
        const double clipStart = static_cast<double>(clip.startFrame);
        const double clipEnd = static_cast<double>(clip.startFrame + std::max(1, clip.durationFrames));
        const auto track = trackOrder.find(clip.trackId);
        const jcut::EditorTrack* trackState =
            track == trackOrder.end() ? nullptr : track->second.second;
        const bool titleClip = clip.mediaKind == "title";
        if (timelineFrame < clipStart || timelineFrame >= clipEnd ||
            (!titleClip && clip.sourcePath.empty()) || !clip.videoEnabled ||
            clip.mediaKind == "audio" ||
            (trackState && trackState->visualMode == 2)) {
            continue;
        }
        result.push_back({
            &clip,
            trackState,
            track == trackOrder.end() ? 0 : track->second.first});
    }
    std::stable_sort(result.begin(), result.end(),
        [](const ActiveVisualClip& left, const ActiveVisualClip& right) {
            return left.trackOrder < right.trackOrder;
        });
    return result;
}

bool nearlyEqual(double left, double right)
{
    return std::abs(left - right) <= 1.0e-9;
}

std::string hardwareDirectIneligibilityReason(
    const EditorDocumentCore& document,
    const std::vector<ActiveVisualClip>& activeClips,
    double timelineFrame)
{
    const ActiveVisualClip* base = nullptr;
    for (const ActiveVisualClip& active :
         activeClips) {
        if (active.clip->mediaKind == "title") {
            continue;
        }
        if (!base) base = &active;
    }
    if (!base) {
        return "hardware-direct preview requires one active decoded visual clip";
    }
    const ActiveVisualClip& active = *base;
    const EditorClip& clip = *active.clip;
    if (clip.mediaKind == "image" ||
        clip.clipRole != "media" ||
        (active.track && active.track->generatedChildTrack)) {
        return "active clip is not an ordinary decoded video layer";
    }
    for (const ActiveVisualClip& candidate :
         activeClips) {
        if (&candidate == base ||
            candidate.trackOrder >
                base->trackOrder) {
            continue;
        }
        if (candidate.clip->mediaKind == "title") {
            return "title layer at or below the decoded video requires ordered composition";
        }
        return "multiple decoded visual layers at or below the hardware base require ordered composition";
    }
    if (clip.effectPreset != "none" ||
        clip.maskEnabled ||
        !clip.correctionPolygons.empty() ||
        clip.maskDropShadowEnabled ||
        clip.maskForegroundLayerEnabled ||
        clip.maskRepeatEnabled) {
        return "clip effects, masks, or corrections require composition";
    }
    (void)document;
    (void)timelineFrame;
    return {};
}

const ActiveVisualClip* hardwareDirectBaseClip(
    const std::vector<ActiveVisualClip>& activeClips)
{
    const auto found = std::find_if(
        activeClips.begin(),
        activeClips.end(),
        [](const ActiveVisualClip& active) {
            return active.clip &&
                active.clip->mediaKind != "title";
        });
    return found == activeClips.end()
        ? nullptr
        : &*found;
}

int sourceFrameForClip(const EditorDocumentCore& document,
                       const EditorClip& clip,
                       int localTimelineFrame)
{
    const std::int64_t timelineFrame =
        static_cast<std::int64_t>(clip.startFrame) +
        std::max(0, localTimelineFrame);
    const std::int64_t sourceFrame =
        jcut::editorClipSourceFrame(
            document, clip, timelineFrame);
    return static_cast<int>(std::min<std::int64_t>(
        sourceFrame, std::numeric_limits<int>::max()));
}

int effectFrameForClip(const EditorDocumentCore& document,
                       const EditorClip& clip,
                       int localTimelineFrame)
{
    auto adjustedDuration = [&](const EditorClip& candidate, int throughLocalFrame) {
        std::int64_t duration = std::clamp<std::int64_t>(
            throughLocalFrame, 0, std::max(0, candidate.durationFrames));
        if (!candidate.effectSkipAwareTiming) return duration;
        const std::int64_t timelineLimit = candidate.startFrame + duration;
        for (const jcut::EditorRenderSyncMarker& marker : document.renderSyncMarkers) {
            if (marker.clipId != candidate.persistentId ||
                marker.frame < candidate.startFrame || marker.frame >= timelineLimit) continue;
            const int magnitude = std::max(1, marker.count);
            duration += marker.skipFrame ? magnitude : -magnitude;
        }
        return std::max<std::int64_t>(0, duration);
    };
    const std::string continuityKey = !clip.linkedSourceClipId.empty()
        ? "linked:" + clip.linkedSourceClipId
        : "source:" + clip.sourcePath;
    std::int64_t elapsed = 0;
    std::vector<const EditorClip*> matching;
    for (const EditorClip& candidate : document.clips) {
        const std::string candidateKey = !candidate.linkedSourceClipId.empty()
            ? "linked:" + candidate.linkedSourceClipId
            : "source:" + candidate.sourcePath;
        if (candidate.trackId == clip.trackId &&
            candidate.effectPreset == clip.effectPreset &&
            candidateKey == continuityKey) {
            matching.push_back(&candidate);
        }
    }
    std::sort(matching.begin(), matching.end(), [](const EditorClip* left, const EditorClip* right) {
        if (left->startFrame == right->startFrame) return left->persistentId < right->persistentId;
        return left->startFrame < right->startFrame;
    });
    for (const EditorClip* candidate : matching) {
        if (candidate == &clip || candidate->id == clip.id) {
            elapsed += adjustedDuration(clip, localTimelineFrame);
            break;
        }
        if (candidate->startFrame < clip.startFrame) {
            elapsed += adjustedDuration(*candidate, candidate->durationFrames);
        }
    }
    return static_cast<int>(std::min<std::int64_t>(
        elapsed, std::numeric_limits<int>::max()));
}

std::string resolveClipPath(const EditorClip& clip, const std::string& rootDirectory)
{
    std::filesystem::path resolvedPath(clip.sourcePath);
    if (resolvedPath.is_relative() && !rootDirectory.empty()) {
        resolvedPath = std::filesystem::path(rootDirectory) / resolvedPath;
    }
    return resolvedPath.lexically_normal().string();
}

class MediaSource {
public:
    explicit MediaSource(
        std::string path,
        DecoderPolicySettingsCore decoderPolicy = {})
        : m_path(std::move(path))
        , m_decoderPolicy(decoderPolicy)
        , m_effectivePolicy(effectiveDecoderPolicyCore(
              decoderPolicy,
              false,
              false,
              static_cast<int>(std::max(
                  1U, std::thread::hardware_concurrency()))))
    {
        ImageSequenceDirectoryInfo sequence =
            probeImageSequenceDirectory(std::filesystem::path(m_path));
        m_sequenceFramePaths = std::move(sequence.framePaths);
    }

    bool open(std::string* errorOut)
    {
        if (m_opened) {
            return true;
        }

        AVFormatContext* rawFormatContext = nullptr;
        if (avformat_open_input(&rawFormatContext, m_path.c_str(), nullptr, nullptr) < 0) {
            if (errorOut) {
                *errorOut = "failed to open media source";
            }
            return false;
        }
        m_formatContext.reset(rawFormatContext);
        if (avformat_find_stream_info(m_formatContext.get(), nullptr) < 0) {
            if (errorOut) {
                *errorOut = "failed to read media stream info";
            }
            return false;
        }

        m_videoStreamIndex =
            av_find_best_stream(m_formatContext.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (m_videoStreamIndex < 0) {
            if (errorOut) {
                *errorOut = "no video stream available";
            }
            return false;
        }

        m_stream = m_formatContext->streams[m_videoStreamIndex];
        const AVCodec* codec = avcodec_find_decoder(m_stream->codecpar->codec_id);
        if (!codec) {
            if (errorOut) {
                *errorOut = "no decoder available for source stream";
            }
            return false;
        }

        const auto allocateCodecContext = [&]() {
            m_codecContext.reset(avcodec_alloc_context3(codec));
            return m_codecContext &&
                avcodec_parameters_to_context(
                    m_codecContext.get(), m_stream->codecpar) >= 0;
        };
        if (!allocateCodecContext()) {
            if (errorOut) {
                *errorOut = "failed to initialize decoder context";
            }
            return false;
        }
        const bool h26x =
            m_stream->codecpar->codec_id == AV_CODEC_ID_H264 ||
            m_stream->codecpar->codec_id == AV_CODEC_ID_HEVC;
        const int logicalCpuCount = static_cast<int>(std::max(
            1U, std::thread::hardware_concurrency()));
        bool hardwareEnabled = false;
        if (!m_hardwareDisabled &&
            m_decoderPolicy.decodePreference !=
            jcut::DecodePreferenceCore::Software) {
            FfmpegHardwareDeviceSetup hardware =
                createFfmpegHardwareDeviceForDecoder(
                    codec,
                    ffmpegHardwareDeviceOrder(
                        m_decoderPolicy.hardwareDevice));
            if (hardware.deviceContext) {
                m_codecContext->hw_device_ctx = hardware.deviceContext;
                hardware.deviceContext = nullptr;
                m_hwPixFmt = hardware.hardwarePixelFormat;
                m_hardwareDeviceLabel = std::move(hardware.deviceLabel);
                m_codecContext->get_format =
                    selectFfmpegHardwarePixelFormat;
                m_codecContext->opaque = reinterpret_cast<void*>(
                    static_cast<std::intptr_t>(m_hwPixFmt));
                hardwareEnabled = true;
            } else {
                m_hardwareFallbackReason = std::move(hardware.error);
            }
        }
        const auto configureDecodePolicy = [&](bool usingHardware) {
            m_effectivePolicy = effectiveDecoderPolicyCore(
                m_decoderPolicy,
                usingHardware,
                false,
                logicalCpuCount);
            if (!h26x || usingHardware) {
                m_effectivePolicy.softwareThreadCount = 1;
                m_effectivePolicy.softwareThreadType = 0;
            }
            m_codecContext->thread_count =
                h26x && !usingHardware
                ? m_effectivePolicy.softwareThreadCount
                : 1;
            m_codecContext->thread_type =
                m_effectivePolicy.softwareThreadType == 1
                ? FF_THREAD_SLICE
                : (m_effectivePolicy.softwareThreadType == 3
                    ? FF_THREAD_FRAME | FF_THREAD_SLICE
                    : 0);
            if (m_decoderPolicy.deterministic ||
                usingHardware) {
                m_codecContext->flags2 &= ~AV_CODEC_FLAG2_FAST;
            } else if (m_codecContext->thread_type != 0) {
                m_codecContext->flags2 |= AV_CODEC_FLAG2_FAST;
            }
        };
        configureDecodePolicy(hardwareEnabled);
        int openResult =
            avcodec_open2(m_codecContext.get(), codec, nullptr);
        if (openResult < 0 && hardwareEnabled) {
            m_hardwareFallbackReason =
                "hardware decoder initialization failed; using software";
            m_hardwareDeviceLabel.clear();
            m_hwPixFmt = AV_PIX_FMT_NONE;
            hardwareEnabled = false;
            if (!allocateCodecContext()) {
                if (errorOut) {
                    *errorOut = "failed to reinitialize software decoder";
                }
                return false;
            }
            configureDecodePolicy(false);
            openResult =
                avcodec_open2(m_codecContext.get(), codec, nullptr);
        }
        m_hardwareAccelerated = hardwareEnabled && openResult >= 0;
        if (openResult < 0) {
            if (errorOut) {
                *errorOut = "failed to initialize decoder context";
            }
            return false;
        }

        m_packet.reset(av_packet_alloc());
        m_frame.reset(av_frame_alloc());
        m_bestFrame.reset(av_frame_alloc());
        if (!m_packet || !m_frame || !m_bestFrame) {
            if (errorOut) {
                *errorOut = "failed to allocate ffmpeg frame buffers";
            }
            return false;
        }

        m_opened = true;
        return true;
    }

    bool decodeScaledFrame(int frameIndex,
                           SizeI outputSize,
                           ImageBuffer* imageOut,
                           std::string* errorOut)
    {
        if (!imageOut) {
            return false;
        }
        if (!m_sequenceFramePaths.empty()) {
            const int sequenceFrameIndex = std::clamp(
                frameIndex,
                0,
                static_cast<int>(m_sequenceFramePaths.size()) - 1);
            if (!m_sequenceFrameSource ||
                sequenceFrameIndex != m_sequenceFrameSourceIndex) {
                m_sequenceFrameSource = std::make_unique<MediaSource>(
                    m_sequenceFramePaths[static_cast<std::size_t>(
                        sequenceFrameIndex)].string(),
                    m_decoderPolicy);
                m_sequenceFrameSourceIndex = sequenceFrameIndex;
            }
            return m_sequenceFrameSource->decodeScaledFrame(
                0, outputSize, imageOut, errorOut);
        }
        if (!ensureDecodedFrame(frameIndex, errorOut)) {
            return false;
        }

        if (scaleFrame(
                m_bestFrame.get(), outputSize, imageOut, errorOut)) {
            return true;
        }
        if (m_hardwareAccelerated) {
            m_hardwareDecodeFailed = true;
            if (reopenWithSoftwareDecoder(errorOut)) {
                return decodeScaledFrame(
                    frameIndex, outputSize, imageOut, errorOut);
            }
        }
        return false;
    }

    std::shared_ptr<const jcut::core::FramePayloadCore> decodeHardwareFrame(
        int frameIndex,
        std::string* errorOut)
    {
        if (!m_sequenceFramePaths.empty()) {
            if (errorOut) {
                *errorOut = "image sequences do not expose hardware decoder surfaces";
            }
            return {};
        }
        if (!ensureDecodedFrame(frameIndex, errorOut)) {
            return {};
        }
        if (!m_hardwareAccelerated ||
            m_hwPixFmt == AV_PIX_FMT_NONE ||
            !m_bestFrame ||
            m_bestFrame->format != m_hwPixFmt) {
            if (errorOut) {
                *errorOut = m_hardwareFallbackReason.empty()
                    ? "decoder did not produce a retained hardware frame"
                    : m_hardwareFallbackReason;
            }
            return {};
        }

        int softwarePixelFormat = AV_PIX_FMT_NONE;
        if (m_bestFrame->hw_frames_ctx && m_bestFrame->hw_frames_ctx->data) {
            const auto* framesContext =
                reinterpret_cast<const AVHWFramesContext*>(
                    m_bestFrame->hw_frames_ctx->data);
            softwarePixelFormat = framesContext
                ? framesContext->sw_format
                : AV_PIX_FMT_NONE;
        }
        if (softwarePixelFormat == AV_PIX_FMT_NONE && m_codecContext) {
            softwarePixelFormat = m_codecContext->sw_pix_fmt;
        }

        auto payload = std::make_shared<jcut::core::FramePayloadCore>();
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        payload->setIdentity(
            frameIndex,
            m_path,
            std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
        if (!payload->cloneHardwareFrame(
                m_bestFrame.get(), softwarePixelFormat)) {
            if (errorOut) {
                *errorOut = "failed to retain decoded hardware frame";
            }
            return {};
        }
        return payload;
    }

    const EffectiveDecoderPolicyCore& effectivePolicy() const noexcept
    {
        return m_effectivePolicy;
    }

    bool hardwareAccelerated() const noexcept
    {
        return m_hardwareAccelerated;
    }

    const std::string& hardwareDeviceLabel() const noexcept
    {
        return m_hardwareDeviceLabel;
    }

    const std::string& hardwareFallbackReason() const noexcept
    {
        return m_hardwareFallbackReason;
    }

    std::string codecName() const
    {
        return m_codecContext && m_codecContext->codec
            ? m_codecContext->codec->name
            : std::string{};
    }

private:
    bool ensureDecodedFrame(int frameIndex, std::string* errorOut)
    {
        if (!open(errorOut)) {
            return false;
        }
        if (m_lastDecodedFrameIndex < 0 || frameIndex < m_lastDecodedFrameIndex) {
            const std::int64_t targetPts =
                ptsForFrameIndex(frameIndex, m_stream);
            av_seek_frame(
                m_formatContext.get(),
                m_videoStreamIndex,
                targetPts,
                AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(m_codecContext.get());
            m_lastDecodedFrameIndex = -1;
            av_frame_unref(m_bestFrame.get());
            m_haveBestFrame = false;
        }
        while (!m_haveBestFrame || m_lastDecodedFrameIndex < frameIndex) {
            if (!readNextFrame(errorOut)) {
                if (m_hardwareDecodeFailed &&
                    reopenWithSoftwareDecoder(errorOut)) {
                    return ensureDecodedFrame(frameIndex, errorOut);
                }
                break;
            }
        }
        if (!m_haveBestFrame) {
            if (errorOut) {
                *errorOut = "failed to decode timeline frame";
            }
            return false;
        }
        return true;
    }

    bool reopenWithSoftwareDecoder(std::string* errorOut)
    {
        if (m_hardwareDisabled) return false;
        m_hardwareDisabled = true;
        m_hardwareDecodeFailed = false;
        m_hardwareAccelerated = false;
        m_hardwareDeviceLabel.clear();
        m_hardwareFallbackReason =
            "hardware frame decode failed; reopened with software";
        m_hwPixFmt = AV_PIX_FMT_NONE;
        m_packet.reset();
        m_frame.reset();
        m_bestFrame.reset();
        m_codecContext.reset();
        m_formatContext.reset();
        m_stream = nullptr;
        m_videoStreamIndex = -1;
        m_lastDecodedFrameIndex = -1;
        m_haveBestFrame = false;
        m_opened = false;
        return open(errorOut);
    }

    bool readNextFrame(std::string* errorOut)
    {
        while (av_read_frame(m_formatContext.get(), m_packet.get()) >= 0) {
            if (m_packet->stream_index != m_videoStreamIndex) {
                av_packet_unref(m_packet.get());
                continue;
            }
            const int sendResult = avcodec_send_packet(m_codecContext.get(), m_packet.get());
            av_packet_unref(m_packet.get());
            if (sendResult < 0) {
                if (m_hardwareAccelerated) {
                    m_hardwareDecodeFailed = true;
                    if (errorOut) {
                        *errorOut = "hardware packet decode failed";
                    }
                    return false;
                }
                continue;
            }
            while (true) {
                const int receiveResult =
                    avcodec_receive_frame(
                        m_codecContext.get(), m_frame.get());
                if (receiveResult == AVERROR(EAGAIN) ||
                    receiveResult == AVERROR_EOF) {
                    break;
                }
                if (receiveResult < 0) {
                    if (m_hardwareAccelerated) {
                        m_hardwareDecodeFailed = true;
                        if (errorOut) {
                            *errorOut =
                                "hardware frame decode failed";
                        }
                        return false;
                    }
                    break;
                }
                av_frame_unref(m_bestFrame.get());
                av_frame_ref(m_bestFrame.get(), m_frame.get());
                m_haveBestFrame = true;
                ++m_lastDecodedFrameIndex;
                av_frame_unref(m_frame.get());
                return true;
            }
        }
        if (errorOut && errorOut->empty()) {
            *errorOut = "failed to decode timeline frame";
        }
        return false;
    }

    bool scaleFrame(const AVFrame* sourceFrame,
                    SizeI outputSize,
                    ImageBuffer* imageOut,
                    std::string* errorOut) const
    {
        std::unique_ptr<AVFrame, AvFrameDeleter> transferredFrame;
        if (m_hwPixFmt != AV_PIX_FMT_NONE &&
            sourceFrame->format == m_hwPixFmt) {
            transferredFrame.reset(av_frame_alloc());
            if (!transferredFrame ||
                av_hwframe_transfer_data(
                    transferredFrame.get(), sourceFrame, 0) < 0) {
                if (errorOut) {
                    *errorOut =
                        "failed to transfer hardware frame to system memory";
                }
                return false;
            }
            av_frame_copy_props(transferredFrame.get(), sourceFrame);
            sourceFrame = transferredFrame.get();
        }
        const double scale = std::min(
            static_cast<double>(outputSize.width) / std::max(1, sourceFrame->width),
            static_cast<double>(outputSize.height) / std::max(1, sourceFrame->height));
        const int targetWidth = std::max(1, static_cast<int>(std::lround(sourceFrame->width * scale)));
        const int targetHeight = std::max(1, static_cast<int>(std::lround(sourceFrame->height * scale)));

        std::unique_ptr<SwsContext, SwsContextDeleter> scaleContext(
            sws_getContext(sourceFrame->width,
                           sourceFrame->height,
                           static_cast<AVPixelFormat>(sourceFrame->format),
                           targetWidth,
                           targetHeight,
                           AV_PIX_FMT_RGBA,
                           SWS_BILINEAR,
                           nullptr,
                           nullptr,
                           nullptr));
        if (!scaleContext) {
            if (errorOut) {
                *errorOut = "failed to create scale context";
            }
            return false;
        }

        ImageBuffer scaled;
        scaled.format = PixelFormat::Rgba8;
        scaled.size = {targetWidth, targetHeight};
        scaled.strideBytes = targetWidth * 4;
        scaled.bytes.resize(static_cast<std::size_t>(scaled.strideBytes * targetHeight));

        std::uint8_t* destinationData[4] = {scaled.bytes.data(), nullptr, nullptr, nullptr};
        int destinationLinesize[4] = {scaled.strideBytes, 0, 0, 0};
        if (sws_scale(scaleContext.get(),
                      sourceFrame->data,
                      sourceFrame->linesize,
                      0,
                      sourceFrame->height,
                      destinationData,
                      destinationLinesize) <= 0) {
            if (errorOut) {
                *errorOut = "failed to scale decoded frame";
            }
            return false;
        }

        *imageOut = std::move(scaled);
        return true;
    }

    std::string m_path;
    DecoderPolicySettingsCore m_decoderPolicy;
    EffectiveDecoderPolicyCore m_effectivePolicy;
    AVPixelFormat m_hwPixFmt = AV_PIX_FMT_NONE;
    bool m_hardwareAccelerated = false;
    bool m_hardwareDisabled = false;
    bool m_hardwareDecodeFailed = false;
    std::string m_hardwareDeviceLabel;
    std::string m_hardwareFallbackReason;
    std::vector<std::filesystem::path> m_sequenceFramePaths;
    int m_sequenceFrameSourceIndex = -1;
    std::unique_ptr<MediaSource> m_sequenceFrameSource;
    bool m_opened = false;
    int m_videoStreamIndex = -1;
    int m_lastDecodedFrameIndex = -1;
    bool m_haveBestFrame = false;
    AVStream* m_stream = nullptr;
    std::unique_ptr<AVFormatContext, AvFormatContextDeleter> m_formatContext;
    std::unique_ptr<AVCodecContext, AvCodecContextDeleter> m_codecContext;
    std::unique_ptr<AVPacket, AvPacketDeleter> m_packet;
    std::unique_ptr<AVFrame, AvFrameDeleter> m_frame;
    std::unique_ptr<AVFrame, AvFrameDeleter> m_bestFrame;
};

} // namespace

namespace jcut::standalone_render {

class StandaloneMediaFrameDecoder::Impl {
public:
    Impl(std::string path, DecoderPolicySettingsCore policy)
        : source(std::move(path), policy)
    {
    }

    MediaSource source;
};

StandaloneMediaFrameDecoder::StandaloneMediaFrameDecoder(
    std::string path,
    DecoderPolicySettingsCore policy)
    : m_impl(std::make_unique<Impl>(std::move(path), policy))
{
}

StandaloneMediaFrameDecoder::~StandaloneMediaFrameDecoder() = default;

StandaloneDecodedFrameResult StandaloneMediaFrameDecoder::decodeFrame(
    int frameIndex,
    core::SizeI outputSize)
{
    StandaloneDecodedFrameResult result;
    if (!m_impl) {
        result.message = "media decoder is unavailable";
        return result;
    }
    if (!outputSize.valid()) {
        outputSize = {640, 360};
    }
    result.success = m_impl->source.decodeScaledFrame(
        std::max(0, frameIndex),
        outputSize,
        &result.image,
        &result.message);
    if (result.success && result.image.empty()) {
        result.success = false;
        result.message = "decoded source frame is empty";
    } else if (result.success) {
        result.message = "decoded standalone media frame";
    }
    return result;
}

StandaloneMediaInfo probeStandaloneMedia(const std::string& path)
{
    StandaloneMediaInfo result;
    if (path.empty()) {
        result.message = "media path is empty";
        return result;
    }

    const ImageSequenceDirectoryInfo sequence =
        probeImageSequenceDirectory(std::filesystem::path(path));
    if (sequence.detected()) {
        result.probed = true;
        result.hasVideo = true;
        result.hasAudio = false;
        result.videoFps = kImageSequenceFramesPerSecond;
        result.sourceDurationFrames = sequence.frameCount();
        result.durationFrames = sequence.frameCount();
        if (!sequence.framePaths.empty()) {
            const StandaloneMediaInfo firstFrame = probeStandaloneMedia(
                sequence.framePaths.front().string());
            result.frameSize = firstFrame.frameSize;
        }
        result.mediaKind = "video";
        result.message = "image sequence directory probed";
        return result;
    }

    AVFormatContext* rawFormatContext = nullptr;
    if (avformat_open_input(&rawFormatContext, path.c_str(), nullptr, nullptr) < 0) {
        result.message = "failed to open media source";
        return result;
    }
    std::unique_ptr<AVFormatContext, AvFormatContextDeleter> formatContext(
        rawFormatContext);
    if (avformat_find_stream_info(formatContext.get(), nullptr) < 0) {
        result.message = "failed to read media stream info";
        return result;
    }

    AVStream* firstVideoStream = nullptr;
    for (unsigned int index = 0; index < formatContext->nb_streams; ++index) {
        AVStream* stream = formatContext->streams[index];
        if (!stream || !stream->codecpar) {
            continue;
        }
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            result.hasVideo = true;
            if (!firstVideoStream) {
                firstVideoStream = stream;
            }
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            result.hasAudio = true;
            if (result.audioStreamIndex < 0) {
                result.audioStreamIndex = static_cast<int>(index);
            }
        }
    }

    result.probed = result.hasVideo || result.hasAudio;
    result.mediaKind = result.hasVideo
        ? "video"
        : (result.hasAudio ? "audio" : "unknown");
    if (firstVideoStream) {
        result.videoFps = streamFps(firstVideoStream);
        if (firstVideoStream->codecpar &&
            firstVideoStream->codecpar->width > 0 &&
            firstVideoStream->codecpar->height > 0) {
            result.frameSize = {
                firstVideoStream->codecpar->width,
                firstVideoStream->codecpar->height};
        }
    }
    double durationSeconds = 0.0;
    if (firstVideoStream && firstVideoStream->duration > 0) {
        durationSeconds =
            static_cast<double>(firstVideoStream->duration) *
            av_q2d(firstVideoStream->time_base);
    } else if (formatContext->duration > 0) {
        durationSeconds =
            static_cast<double>(formatContext->duration) /
            static_cast<double>(AV_TIME_BASE);
    }
    if (durationSeconds > 0.0) {
        if (result.hasVideo) {
            const double sourceFps =
                result.videoFps > 0.001 ? result.videoFps : 30.0;
            result.sourceDurationFrames = static_cast<std::int64_t>(
                std::llround(durationSeconds * sourceFps));
        }
        result.durationFrames = static_cast<std::int64_t>(
            std::llround(durationSeconds * 30.0));
    }
    result.message = result.probed
        ? "media streams probed"
        : "no audio or video streams found";
    return result;
}

StandaloneDecodeBenchmarkResult benchmarkStandaloneMediaDecode(
    const std::string& path,
    const DecoderPolicySettingsCore& policy,
    int maxFrames)
{
    StandaloneDecodeBenchmarkResult result;
    result.requestedPreference = policy.decodePreference;
    const StandaloneMediaInfo media = probeStandaloneMedia(path);
    if (!media.probed || !media.hasVideo) {
        result.message = media.message.empty()
            ? "benchmark source has no video"
            : media.message;
        return result;
    }
    const int frameCount = std::clamp(
        static_cast<int>(std::min<std::int64_t>(
            std::max<std::int64_t>(1, media.durationFrames),
            std::numeric_limits<int>::max())),
        1,
        std::clamp(maxFrames, 1, 1000));
    MediaSource source(path, policy);
    const auto started = std::chrono::steady_clock::now();
    for (int frame = 0; frame < frameCount; ++frame) {
        ImageBuffer image;
        std::string error;
        if (source.decodeScaledFrame(
                frame, {640, 360}, &image, &error) &&
            !image.empty()) {
            ++result.framesDecoded;
        } else {
            ++result.failedFrames;
            if (result.message.empty()) result.message = std::move(error);
        }
    }
    result.elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
    result.codecName = source.codecName();
    result.effectivePreference =
        source.effectivePolicy().effectivePreference;
    result.softwareThreadCount =
        source.effectivePolicy().softwareThreadCount;
    result.hardwareAccelerated = source.hardwareAccelerated();
    result.hardwareDeviceLabel = source.hardwareDeviceLabel();
    result.hardwareFallbackReason = source.hardwareFallbackReason();
    result.framesPerSecond = result.framesDecoded > 0
        ? static_cast<double>(result.framesDecoded) * 1000.0 /
            static_cast<double>(std::max<std::int64_t>(1, result.elapsedMs))
        : 0.0;
    result.success = result.framesDecoded > 0;
    if (result.success) {
        result.message = "standalone decode benchmark completed";
    } else if (result.message.empty()) {
        result.message = "standalone decode benchmark decoded no frames";
    }
    return result;
}

StandaloneHardwareFrameResult retainStandaloneHardwareFrame(
    const std::string& path,
    int frameIndex,
    const DecoderPolicySettingsCore& policy)
{
    StandaloneHardwareFrameResult result;
    MediaSource source(path, policy);
    std::string error;
    result.frame = source.decodeHardwareFrame(
        std::max(0, frameIndex), &error);
    result.effectivePreference =
        source.effectivePolicy().effectivePreference;
    result.hardwareAccelerated = source.hardwareAccelerated();
    result.hardwareDeviceLabel = source.hardwareDeviceLabel();
    result.hardwareFallbackReason = source.hardwareFallbackReason();
    result.success = static_cast<bool>(result.frame);
    result.message = result.success
        ? "retained standalone hardware frame"
        : (error.empty()
            ? "standalone hardware frame unavailable"
            : std::move(error));
    return result;
}

StandaloneDecodedFrameResult decodeStandaloneMediaFrame(
    const std::string& path,
    int frameIndex,
    core::SizeI outputSize,
    const DecoderPolicySettingsCore& policy)
{
    if (path.empty()) {
        StandaloneDecodedFrameResult result;
        result.message = "decode source path is empty";
        return result;
    }
    StandaloneMediaFrameDecoder decoder(path, policy);
    return decoder.decodeFrame(frameIndex, outputSize);
}

std::size_t probeUnknownAudioPresence(EditorDocumentCore* document,
                                      const std::string& rootDirectory)
{
    if (!document) {
        return 0;
    }

    std::unordered_map<std::string, StandaloneMediaInfo> probeByPath;
    std::size_t resolvedClipCount = 0;
    for (EditorClip& clip : document->clips) {
        if (clip.audioPresenceKnown ||
            !mediaKindMayContainAudio(clip.mediaKind, clip.sourcePath)) {
            continue;
        }

        std::filesystem::path resolvedPath(clip.sourcePath);
        if (resolvedPath.is_relative() && !rootDirectory.empty()) {
            resolvedPath = std::filesystem::path(rootDirectory) / resolvedPath;
        }
        const std::string probePath = resolvedPath.lexically_normal().string();
        if (probePath.empty()) {
            continue;
        }

        auto [probeIt, inserted] = probeByPath.try_emplace(probePath);
        if (inserted) {
            probeIt->second = probeStandaloneMedia(probePath);
        }
        const StandaloneMediaInfo& info = probeIt->second;
        if (!info.probed) {
            continue;
        }

        clip.audioPresenceKnown = true;
        clip.hasAudio = info.hasAudio;
        ++resolvedClipCount;
        for (EditorMediaItem& mediaItem : document->mediaItems) {
            if (mediaItem.id == clip.sourcePath) {
                mediaItem.audioPresenceKnown = true;
                mediaItem.hasAudio = info.hasAudio;
            }
        }
    }
    return resolvedClipCount;
}

class TimelineRenderer::Impl {
public:
    TimelineRenderResult renderFrame(const TimelineRenderRequest& request)
    {
        TimelineRenderResult result;
        result.requestedDecodePreference =
            request.decoderPolicy.decodePreference;
        result.effectiveDecodePreference =
            effectiveDecoderPolicyCore(
                request.decoderPolicy,
                false,
                false,
                static_cast<int>(std::max(
                    1U, std::thread::hardware_concurrency())))
                .effectivePreference;
        if (!request.outputSize.valid()) {
            result.message = "invalid preview output size";
            return result;
        }
        if (!(request.decoderPolicy == m_decoderPolicy)) {
            m_sources.clear();
            m_decoderPolicy = request.decoderPolicy;
        }

        const std::vector<ActiveVisualClip> activeClips =
            activeVisualClips(request.document, request.timelineFrame);
        if (request.preferHardwareFrame) {
            result.hardwareDirectFallbackReason =
                hardwareDirectIneligibilityReason(
                    request.document,
                    activeClips,
                    request.timelineFrame);
            result.hardwareDirectEligible =
                result.hardwareDirectFallbackReason.empty();
            if (result.hardwareDirectEligible) {
                const ActiveVisualClip* directBase =
                    hardwareDirectBaseClip(activeClips);
                if (!directBase) {
                    result.hardwareDirectEligible = false;
                    result.hardwareDirectFallbackReason =
                        "hardware-direct base video is unavailable";
                } else {
                const EditorClip& clip = *directBase->clip;
                const int localFrame = std::max(
                    0,
                    static_cast<int>(std::floor(request.timelineFrame)) -
                        clip.startFrame);
                const int sourceFrame = sourceFrameForClip(
                    request.document, clip, localFrame);
                result.sourcePath =
                    resolveClipPath(clip, request.rootDirectory);
                MediaSource& mediaSource =
                    sourceForPath(result.sourcePath);
                std::string handoffError;
                std::shared_ptr<const jcut::core::FramePayloadCore> payload =
                    mediaSource.decodeHardwareFrame(
                        sourceFrame, &handoffError);
                applyDecodeStatus(mediaSource, &result);
                bool hardwarePayloadWasValid = false;
                if (payload && payload->size().valid()) {
                    hardwarePayloadWasValid = true;
                    const core::SizeI hardwareFrameSize =
                        payload->size();
                    result.hardwareFrame = std::move(payload);
                    result.hardwarePresentationTransformValid = true;
                    result.hardwarePresentationTransform =
                        resolvedPresentationTransform(
                            request,
                            clip,
                            result.sourcePath,
                            sourceFrame,
                            hardwareFrameSize);
                    result.hardwarePresentationGrade =
                        jcut::evaluateEditorClipGradingAtLocalFrame(
                            clip, localFrame);
                    const bool gradingEnabled =
                        clip.gradingPreviewEnabled &&
                        (!directBase->track ||
                         directBase->track->
                             gradingPreviewEnabled);
                    if (!gradingEnabled) {
                        const double opacity =
                            result.hardwarePresentationGrade.opacity;
                        result.hardwarePresentationGrade = {};
                        result.hardwarePresentationGrade.opacity =
                            opacity;
                    }
                    const bool forceOpaque =
                        directBase->track &&
                        directBase->track->
                            visualMode == 1;
                    result.hardwarePresentationOpacity =
                        forceOpaque
                        ? 1.0
                        : std::clamp(
                            result.
                                hardwarePresentationGrade.
                                opacity,
                            0.0,
                            1.0);
                    if (forceOpaque) {
                        result.hardwarePresentationGrade.
                            opacity = 1.0;
                    }
                    std::string overlayError;
                    result.hardwareOverlayImage =
                        renderHardwareOverlayLayer(
                            request,
                            directBase->trackOrder,
                            &overlayError);
                    if (!overlayError.empty()) {
                        result.hardwareFrame.reset();
                        result.
                            hardwarePresentationTransformValid =
                                false;
                        result.hardwareDirectFallbackReason =
                            std::move(overlayError);
                    } else {
                    cropImageToAlphaBounds(
                        &result.hardwareOverlayImage,
                        &result.hardwareOverlayX,
                        &result.hardwareOverlayY);
                    result.success = true;
                    result.message =
                        "retained hardware frame ready for Vulkan handoff";
                    return result;
                    }
                }
                if (!hardwarePayloadWasValid &&
                    !payload) {
                    result.hardwareDirectFallbackReason =
                        handoffError.empty()
                        ? "decoder did not return a hardware frame"
                        : std::move(handoffError);
                } else if (!hardwarePayloadWasValid) {
                    result.hardwareDirectFallbackReason =
                        "decoded hardware frame has invalid dimensions";
                }
                }
            }
            if (!request.allowCpuFallback) {
                result.message =
                    result.hardwareDirectFallbackReason.empty()
                    ? "hardware-direct preview unavailable"
                    : result.hardwareDirectFallbackReason;
                return result;
            }
        } else if (!request.allowCpuFallback) {
            result.message = "CPU preview fallback disabled";
            return result;
        }

        ImageBuffer canvas = request.transparentBackground
            ? makeSolidImage(
                request.outputSize, 0, 0, 0, 0)
            : makeSolidImage(
                request.outputSize, 12, 14, 18);

        for (const ActiveVisualClip& active : activeClips) {
            const EditorClip& clip = *active.clip;
            const int localFrame = std::max(
                0,
                static_cast<int>(std::floor(request.timelineFrame)) -
                    clip.startFrame);
            ImageBuffer decoded;
            int sourceFrame = localFrame;
            const int effectFrame = effectFrameForClip(
                request.document, clip, localFrame);
            if (clip.mediaKind == "title") {
                const ImageBuffer titleLayer =
                    renderTitleClipLayer(
                        request, active);
                if (!titleLayer.empty()) {
                    blitImage(
                        titleLayer, &canvas, 0, 0);
                }
                continue;
            } else {
                result.sourcePath = resolveClipPath(clip, request.rootDirectory);
                MediaSource& mediaSource = sourceForPath(result.sourcePath);
                std::string decodeError;
                sourceFrame = sourceFrameForClip(
                    request.document, clip, localFrame);
                if (!mediaSource.decodeScaledFrame(
                        sourceFrame, request.outputSize, &decoded, &decodeError)) {
                    applyDecodeStatus(mediaSource, &result);
                    result.success = false;
                    result.message = decodeError.empty()
                        ? "timeline render failed"
                        : decodeError;
                    result.image = std::move(canvas);
                    return result;
                }
                applyDecodeStatus(mediaSource, &result);
            }

            jcut::EditorGradingKeyframe grade =
                jcut::evaluateEditorClipGradingAtLocalFrame(clip, localFrame);
            const bool gradingEnabled = clip.gradingPreviewEnabled &&
                (!active.track || active.track->gradingPreviewEnabled);
            if (!gradingEnabled) {
                const double opacity = grade.opacity;
                grade = {};
                grade.opacity = opacity;
            }
            const ImageBuffer ungraded = decoded;
            applyClipGrade(
                &decoded, grade,
                active.track && active.track->visualMode == 1);
            const bool dynamicVisualSource = clip.mediaKind != "title" &&
                clip.mediaKind != "image";
            if (dynamicVisualSource && clip.effectPreset == "difference_matte") {
                ImageBuffer reference;
                std::string referenceError;
                const int referenceFrame = std::max(
                    0, sourceFrame - std::clamp(clip.differenceReferenceFrames, 1, 300));
                if (sourceForPath(result.sourcePath).decodeScaledFrame(
                        referenceFrame, request.outputSize, &reference, &referenceError)) {
                    applyDifferenceMatte(
                        &decoded, reference,
                        clip.differenceThreshold,
                        clip.differenceSoftness);
                }
            }
            if (clip.mediaKind != "title" &&
                clip.effectPreset != "difference_matte" &&
                (clip.maskEnabled || clip.clipRole == "mask_matte")) {
                std::filesystem::path maskDirectory(clip.maskFramesDir);
                if (maskDirectory.is_relative() && !request.rootDirectory.empty()) {
                    maskDirectory = std::filesystem::path(request.rootDirectory) / maskDirectory;
                }
                const auto maskPath = clip.maskEnabled && !clip.maskFramesDir.empty()
                    ? jcut::masks::maskFramePathForSourceFrameCore(
                          maskDirectory.lexically_normal(),
                          std::filesystem::path(result.sourcePath),
                          sourceFrame)
                    : std::nullopt;
                ImageBuffer maskImage;
                bool maskReady = false;
                if (maskPath) {
                    std::string maskError;
                    maskReady = sourceForPath(maskPath->string()).decodeScaledFrame(
                        0, decoded.size, &maskImage, &maskError);
                }
                if (maskReady) {
                    std::vector<std::uint8_t> mask =
                        maskPlaneFromImage(maskImage, decoded.size);
                    applyCorrectionPolygons(&mask, decoded.size, clip, localFrame);
                    if (clip.maskInvert) {
                        for (std::uint8_t& value : mask) value = 255 - value;
                    }
                    morphMaskPlane(&mask, decoded.size,
                        static_cast<int>(std::lround(std::max(0.0, clip.maskErode))), false);
                    morphMaskPlane(&mask, decoded.size,
                        static_cast<int>(std::lround(std::max(0.0, clip.maskDilate))), true);
                    blurMaskPlane(&mask, decoded.size,
                        static_cast<int>(std::lround(std::max(
                            std::max(0.0, clip.maskFeather),
                            std::max(0.0, clip.maskBlur)))),
                        clip.maskFeatherGamma,
                        clip.maskFeatherFalloff);
                    if (isSpeakerMaskEffect(clip.effectPreset)) {
                        const jcut::TranscriptOverlayLayoutCore speaker =
                            transcriptLayout(
                                request.document,
                                clip,
                                result.sourcePath,
                                sourceFrame,
                                request.timelineFrame -
                                    static_cast<double>(clip.startFrame),
                                request.rootDirectory);
                        const std::array<RgbaColor, 3> palette = {{
                            parseColor(
                                speaker.speakerPrimaryColor,
                                {255, 0, 0, 1.0}),
                            parseColor(
                                speaker.speakerSecondaryColor,
                                {0, 255, 0, 1.0}),
                            parseColor(
                                speaker.speakerAccentColor,
                                {255, 255, 0, 1.0}),
                        }};
                        applySpeakerMaskEffect(
                            &decoded, mask, clip, effectFrame, palette);
                    } else {
                        applyMaskPlaneToImage(&decoded, ungraded, mask, clip);
                    }
                } else if (clip.clipRole == "mask_matte") {
                    for (int y = 0; y < decoded.size.height; ++y) {
                        for (int x = 0; x < decoded.size.width; ++x) {
                            decoded.bytes[static_cast<std::size_t>(
                                y * decoded.strideBytes + x * 4 + 3)] = 0;
                        }
                    }
                }
            }
            if (dynamicVisualSource && clip.effectPreset == "temporal_echo") {
                const int echoCount = std::clamp(clip.temporalEchoCount, 1, 12);
                const int echoSpacing = std::clamp(
                    clip.temporalEchoSpacingFrames, 1, 120);
                const double echoDecay = std::clamp(clip.temporalEchoDecay, 0.0, 1.0);
                for (int echoIndex = 1; echoIndex <= echoCount; ++echoIndex) {
                    ImageBuffer echo;
                    std::string echoError;
                    const int echoFrame = std::max(
                        0, sourceFrame - echoIndex * echoSpacing);
                    if (!sourceForPath(result.sourcePath).decodeScaledFrame(
                            echoFrame, request.outputSize, &echo, &echoError)) {
                        continue;
                    }
                    jcut::EditorGradingKeyframe echoGrade = grade;
                    echoGrade.opacity = std::clamp(
                        grade.opacity * std::pow(echoDecay, echoIndex), 0.0, 1.0);
                    applyClipGrade(
                        &echo, echoGrade,
                        active.track && active.track->visualMode == 1);
                    blitImage(echo, &decoded, 0, 0);
                }
            }
            if (clip.mediaKind != "title" && clip.effectPreset == "source_tile") {
                decoded = renderSourceTilingEffect(
                    decoded, clip, effectFrame, request.outputSize);
            } else if (clip.mediaKind != "title" &&
                       isGeneratedRepeatEffect(clip.effectPreset)) {
                decoded = renderGeneratedRepeatEffect(
                    decoded, clip, effectFrame, request.outputSize);
            } else if (clip.mediaKind != "title" &&
                       isStandalonePixelEffect(clip.effectPreset)) {
                decoded = renderStandalonePixelEffect(
                    decoded, clip, effectFrame, request.outputSize);
            } else if (clip.mediaKind != "title" &&
                       clip.effectPreset == "vulkan_3d_synth") {
                decoded = renderVulkan3DSynthEffect(
                    decoded, clip, effectFrame, request.outputSize);
            }
            if (clip.mediaKind != "title" &&
                (clip.edgeFillEffect != "none" ||
                 clip.effectPreset == "progressive_edge_stretch")) {
                decoded = renderEdgeStretchEffect(
                    decoded, clip, request.outputSize);
            }
            const int offsetX = clip.mediaKind == "title" ? 0 :
                (request.outputSize.width - decoded.size.width) / 2;
            const int offsetY = clip.mediaKind == "title" ? 0 :
                (request.outputSize.height - decoded.size.height) / 2;
            const jcut::EditorTransformKeyframe transform =
                resolvedPresentationTransform(
                    request,
                    clip,
                    result.sourcePath,
                    sourceFrame,
                    decoded.size);
            blitTransformedImage(
                decoded, &canvas, offsetX, offsetY, transform);
        }

        const ImageBuffer transcriptOverlay =
            renderTranscriptOverlayLayer(request);
        if (!transcriptOverlay.empty()) {
            blitImage(
                transcriptOverlay, &canvas, 0, 0);
        }

        result.success = true;
        result.message = activeClips.empty()
            ? "no active visual clip"
            : (activeClips.size() == 1
            ? "timeline frame rendered"
            : "timeline layers rendered");
        result.image = std::move(canvas);
        return result;
    }

private:
    static void applyDecodeStatus(
        const MediaSource& source,
        TimelineRenderResult* result)
    {
        if (!result) return;
        result->effectiveDecodePreference =
            source.effectivePolicy().effectivePreference;
        result->hardwareAccelerated = source.hardwareAccelerated();
        result->hardwareDeviceLabel = source.hardwareDeviceLabel();
        result->hardwareFallbackReason =
            source.hardwareFallbackReason();
    }

    struct TranscriptCacheEntry {
        jcut::TranscriptFileStamp stamp;
        std::vector<jcut::TranscriptRow> rows;
        std::optional<jcut::TranscriptDocumentCore> document;
        bool valid = false;
    };

    jcut::TranscriptOverlayLayoutCore transcriptLayout(
        const EditorDocumentCore& document,
        const EditorClip& clip,
        const std::string& resolvedSourcePath,
        int sourceFrame,
        double localTimelineFrame,
        const std::string& rootDirectory)
    {
        jcut::TranscriptSourceSpec source;
        source.sourcePath = resolvedSourcePath;
        source.audioSourceMode = clip.audioSourceMode;
        source.audioSourceStatus = clip.audioSourceStatus;
        source.audioStreamIndex = clip.audioStreamIndex;
        source.sourceRootPath = rootDirectory;
        if (!clip.audioSourcePath.empty()) {
            std::filesystem::path audioPath(clip.audioSourcePath);
            if (audioPath.is_relative() && !rootDirectory.empty()) {
                audioPath = std::filesystem::path(rootDirectory) / audioPath;
            }
            source.audioSourcePath = audioPath.lexically_normal().string();
        }
        const jcut::TranscriptCutCatalog catalog =
            jcut::discoverTranscriptCutCatalog(source, false);
        std::string activePath = clip.transcriptActiveCutPath.empty()
            ? catalog.workingPath : clip.transcriptActiveCutPath;
        if (!activePath.empty()) {
            std::filesystem::path path(activePath);
            if (path.is_relative() && !rootDirectory.empty()) {
                path = std::filesystem::path(rootDirectory) / path;
            }
            activePath = path.lexically_normal().string();
        }
        if (activePath.empty()) return {};
        const jcut::TranscriptFileStamp stamp = jcut::inspectTranscriptFile(activePath);
        TranscriptCacheEntry& cache = m_transcripts[activePath];
        if (!cache.valid || cache.stamp != stamp) {
            jcut::TranscriptCutSessionOptions sessionOptions;
            sessionOptions.requestedActivePath = activePath;
            sessionOptions.ensureEditable = false;
            sessionOptions.timing.framesPerSecond =
                std::isfinite(clip.sourceFps) && clip.sourceFps > 0.0
                    ? clip.sourceFps : 30.0;
            // Keep the cached projection on the persisted word boundaries.
            // Overlay padding/offset belongs to the frame-specific layout pass
            // below, matching the Qt renderer and avoiding applying it twice.
            sessionOptions.timing.prependMilliseconds = 0;
            sessionOptions.timing.postpendMilliseconds = 0;
            sessionOptions.timing.offsetMilliseconds = 0;
            const jcut::TranscriptCutSession session =
                jcut::loadTranscriptCutSession(source, sessionOptions);
            cache.stamp = stamp;
            cache.rows = session.ok() ? session.rows : std::vector<jcut::TranscriptRow>{};
            cache.document = session.ok() ? session.activeDocument : std::nullopt;
            cache.valid = session.ok();
        }
        if (!cache.valid) return {};
        const double estimatedLineHeight = std::max(
            12.0, clip.transcriptOverlay.fontPointSize * 1.55);
        const double usableHeight = std::max(
            estimatedLineHeight,
            clip.transcriptOverlay.boxHeight - 28.0 -
                (clip.transcriptOverlay.showShadow
                     ? clip.transcriptOverlay.fontPointSize * 0.16 : 0.0) -
                clip.transcriptOverlay.fontPointSize * 0.08);
        const int fittedLines = std::max(
            1, static_cast<int>(std::floor(usableHeight / estimatedLineHeight)));
        const double estimatedCharacterWidth = std::max(
            6.0, clip.transcriptOverlay.fontPointSize * 0.62);
        const int fittedCharacters = std::max(
            1, static_cast<int>(std::floor(
                std::max(estimatedCharacterWidth,
                         clip.transcriptOverlay.boxWidth - 36.0) /
                estimatedCharacterWidth)));
        jcut::TranscriptOverlayLayoutOptions layoutOptions;
        layoutOptions.timing.framesPerSecond =
            std::isfinite(clip.sourceFps) && clip.sourceFps > 0.0
                ? clip.sourceFps : 30.0;
        layoutOptions.timing.prependMilliseconds =
            document.exportRequest.transcriptPrependMs;
        layoutOptions.timing.postpendMilliseconds =
            document.exportRequest.transcriptPostpendMs;
        layoutOptions.timing.offsetMilliseconds =
            document.exportRequest.transcriptOffsetMs;
        layoutOptions.maxLines = std::min(
            std::max(1, clip.transcriptOverlay.maxLines), fittedLines);
        layoutOptions.maxCharsPerLine = std::min(
            std::max(1, clip.transcriptOverlay.maxCharsPerLine), fittedCharacters);
        layoutOptions.autoScroll = clip.transcriptOverlay.autoScroll;
        jcut::TranscriptOverlayLayoutCore layout =
            jcut::transcriptOverlayLayoutForRows(
                cache.rows, sourceFrame, layoutOptions);
        if (cache.document && !layout.speakerId.empty()) {
            const auto profiles =
                cache.document->speakerProfiles();
            const auto profile = std::find_if(
                profiles.begin(),
                profiles.end(),
                [&](const jcut::TranscriptSpeakerProfileCore& candidate) {
                    return candidate.id == layout.speakerId;
                });
            if (profile != profiles.end()) {
                layout.speakerPrimaryColor =
                    profile->primaryColor;
                layout.speakerSecondaryColor =
                    profile->secondaryColor;
                layout.speakerAccentColor =
                    profile->accentColor;
            }
            if (!clip.transcriptOverlay.useManualPlacement) {
                const jcut::TranscriptSpeakerLocationCore location =
                    cache.document->speakerLocation(
                        layout.speakerId, sourceFrame);
                layout.speakerLocationX = location.x;
                layout.speakerLocationY = location.y;
                layout.speakerLocationValid = location.valid;
            }
            const double sourceFps =
                std::isfinite(clip.sourceFps) && clip.sourceFps > 0.0
                ? clip.sourceFps : 30.0;
            const std::int64_t transcriptFrame =
                std::max<std::int64_t>(
                    0,
                    static_cast<std::int64_t>(std::floor(
                        (static_cast<double>(sourceFrame) / sourceFps) *
                        30.0)));
            const std::string clipId =
                clip.persistentId.empty()
                ? std::to_string(clip.id)
                : clip.persistentId;
            std::vector<jcut::SpeakerTrackAssignmentCore>
                assignments;
            if (clip.speakerFramingManualTrackId >= 0 ||
                !clip.speakerFramingManualStreamId.empty()) {
                assignments.push_back({
                    clip.speakerFramingManualTrackId,
                    layout.speakerId,
                    clip.speakerFramingManualStreamId,
                    sourceFrame});
            } else {
                assignments =
                    jcut::transcriptSpeakerTrackAssignmentsAtFrame(
                        cache.document->root(),
                        clipId,
                        layout.speakerId,
                        sourceFrame);
            }
            double sectionRotationDegrees = 0.0;
            for (const auto& assignment : assignments) {
                sectionRotationDegrees = assignment.rotationDegrees;
                const jcut::FaceTrackingSampleCore sample =
                    jcut::sampleFaceContinuityTrack(
                        activePath,
                        clipId,
                        assignment.trackId,
                        assignment.streamId,
                        sourceFrame,
                        clip.speakerFramingMinConfidence,
                        clip.sourceInFrame,
                        localTimelineFrame,
                        clip.speakerFramingGapHoldFrames,
                        clip.speakerFramingCenterSmoothingFrames,
                        clip.speakerFramingZoomSmoothingFrames,
                        clip.speakerFramingSmoothingMode,
                        clip.speakerFramingCenterSmoothingStrength,
                        clip.speakerFramingZoomSmoothingStrength);
                if (!sample.valid) continue;
                layout.speakerTrackingX = sample.x;
                layout.speakerTrackingY = sample.y;
                layout.speakerTrackingBoxSize = sample.box;
                layout.speakerTrackingRotationDegrees =
                    assignment.rotationDegrees;
                layout.speakerTrackingValid = true;
                break;
            }
            if (!layout.speakerTrackingValid &&
                std::abs(sectionRotationDegrees) > 0.000001) {
                layout.speakerTrackingRotationDegrees =
                    sectionRotationDegrees;
                layout.speakerTrackingCenterRotationFallback = true;
            }
            if (!layout.speakerTrackingValid) {
                const jcut::TranscriptSpeakerTrackingSampleCore tracking =
                    cache.document->speakerTrackingSample(
                        layout.speakerId,
                        transcriptFrame,
                        clip.speakerFramingMinConfidence);
                layout.speakerTrackingX = tracking.x;
                layout.speakerTrackingY = tracking.y;
                layout.speakerTrackingBoxSize = tracking.boxSize;
                layout.speakerTrackingValid = tracking.valid;
                if (tracking.valid) {
                    layout.speakerTrackingCenterRotationFallback =
                        false;
                    layout.speakerTrackingRotationDegrees = 0.0;
                }
            }
        }
        return layout;
    }

    ImageBuffer renderTitleClipLayer(
        const TimelineRenderRequest& request,
        const ActiveVisualClip& active)
    {
        if (!active.clip ||
            active.clip->mediaKind != "title") {
            return {};
        }
        const EditorClip& clip = *active.clip;
        const int localFrame = std::max(
            0,
            static_cast<int>(
                std::floor(request.timelineFrame)) -
                clip.startFrame);
        const jcut::EditorTitleKeyframe title =
            jcut::evaluateEditorClipTitleAtLocalFrame(
                clip, localFrame);
        ImageBuffer textPattern;
        ImageBuffer framePattern;
        ImageBuffer logo;
        const auto decodeTitleAsset =
            [&](const std::string& storedPath,
                SizeI requestedSize,
                ImageBuffer* imageOut) {
                if (storedPath.empty() || !imageOut) {
                    return;
                }
                std::filesystem::path path(storedPath);
                if (path.is_relative() &&
                    !request.rootDirectory.empty()) {
                    path =
                        std::filesystem::path(
                            request.rootDirectory) /
                        path;
                }
                std::string ignoredError;
                sourceForPath(
                    path.lexically_normal().string()).
                    decodeScaledFrame(
                        0,
                        requestedSize,
                        imageOut,
                        &ignoredError);
            };
        if (title.textMaterialStyle ==
            "image_pattern") {
            decodeTitleAsset(
                title.textPatternImagePath,
                {96, 96},
                &textPattern);
        }
        if (title.windowFrameMaterialStyle ==
            "image_pattern") {
            decodeTitleAsset(
                title.windowFramePatternImagePath,
                {96, 96},
                &framePattern);
        }
        const int logoSize = std::clamp(
            static_cast<int>(
                std::lround(title.fontSize * 1.35)),
            34,
            140);
        decodeTitleAsset(
            title.logoPath,
            {logoSize, logoSize},
            &logo);
        ImageBuffer decoded = renderTitleImage(
            title,
            request.outputSize,
            textPattern.empty() ? nullptr : &textPattern,
            framePattern.empty() ? nullptr : &framePattern,
            logo.empty() ? nullptr : &logo);
        jcut::EditorGradingKeyframe grade =
            jcut::evaluateEditorClipGradingAtLocalFrame(
                clip, localFrame);
        const bool gradingEnabled =
            clip.gradingPreviewEnabled &&
            (!active.track ||
             active.track->gradingPreviewEnabled);
        if (!gradingEnabled) {
            const double opacity = grade.opacity;
            grade = {};
            grade.opacity = opacity;
        }
        applyClipGrade(
            &decoded,
            grade,
            active.track &&
                active.track->visualMode == 1);
        ImageBuffer layer = makeSolidImage(
            request.outputSize, 0, 0, 0, 0);
        const jcut::EditorTransformKeyframe transform =
            resolvedPresentationTransform(
                request,
                clip,
                {},
                localFrame,
                decoded.size);
        blitTransformedImage(
            decoded,
            &layer,
            0,
            0,
            transform);
        return layer;
    }

    ImageBuffer renderTranscriptOverlayLayer(
        const TimelineRenderRequest& request)
    {
        ImageBuffer overlay;
        for (const EditorClip& clip :
             request.document.clips) {
            const double clipStart =
                static_cast<double>(clip.startFrame);
            const double clipEnd =
                static_cast<double>(
                    clip.startFrame +
                    std::max(1, clip.durationFrames));
            if (!clip.transcriptOverlay.enabled ||
                request.timelineFrame < clipStart ||
                request.timelineFrame >= clipEnd ||
                (clip.mediaKind != "audio" &&
                 !clip.hasAudio)) {
                continue;
            }
            const int localFrame = std::max(
                0,
                static_cast<int>(
                    std::floor(request.timelineFrame)) -
                    clip.startFrame);
            const int sourceFrame =
                sourceFrameForClip(
                    request.document,
                    clip,
                    localFrame);
            const std::string sourcePath =
                resolveClipPath(
                    clip,
                    request.rootDirectory);
            const jcut::TranscriptOverlayLayoutCore layout =
                transcriptLayout(
                    request.document,
                    clip,
                    sourcePath,
                    sourceFrame,
                    request.timelineFrame -
                        static_cast<double>(
                            clip.startFrame),
                    request.rootDirectory);
            if (layout.lines.empty()) {
                continue;
            }
            if (overlay.empty()) {
                overlay = makeSolidImage(
                    request.outputSize,
                    0,
                    0,
                    0,
                    0);
            }
            drawTranscriptOverlay(
                &overlay,
                clip.transcriptOverlay,
                layout);
        }
        return overlay;
    }

    ImageBuffer renderHardwareOverlayLayer(
        const TimelineRenderRequest& request,
        int baseTrackOrder,
        std::string* error)
    {
        ImageBuffer overlay;
        std::unordered_set<int> upperTrackIds;
        for (std::size_t index = 0;
             index < request.document.tracks.size();
             ++index) {
            if (static_cast<int>(index) >
                baseTrackOrder) {
                upperTrackIds.insert(
                    request.document.tracks[index].id);
            }
        }
        if (!upperTrackIds.empty()) {
            TimelineRenderRequest upperRequest =
                request;
            upperRequest.preferHardwareFrame = false;
            upperRequest.allowCpuFallback = true;
            upperRequest.transparentBackground = true;
            auto& upperClips =
                upperRequest.document.clips;
            upperClips.erase(
                std::remove_if(
                    upperClips.begin(),
                    upperClips.end(),
                    [&](EditorClip& clip) {
                        clip.transcriptOverlay.enabled =
                            false;
                        return !upperTrackIds.contains(
                            clip.trackId);
                    }),
                upperClips.end());
            if (!upperClips.empty()) {
                TimelineRenderResult upperResult =
                    renderFrame(upperRequest);
                if (!upperResult.success) {
                    if (error) {
                        *error =
                            upperResult.message.empty()
                            ? "upper visual layer composition failed"
                            : "upper visual layer composition failed: " +
                                upperResult.message;
                    }
                    return {};
                }
                overlay =
                    std::move(upperResult.image);
            }
        }
        const ImageBuffer transcriptLayer =
            renderTranscriptOverlayLayer(request);
        if (!transcriptLayer.empty()) {
            if (overlay.empty()) {
                overlay = makeSolidImage(
                    request.outputSize,
                    0,
                    0,
                    0,
                    0);
            }
            blitImage(
                transcriptLayer,
                &overlay,
                0,
                0);
        }
        return overlay;
    }

    jcut::EditorTransformKeyframe resolvedPresentationTransform(
        const TimelineRenderRequest& request,
        const EditorClip& clip,
        const std::string& resolvedSourcePath,
        int sourceFrame,
        core::SizeI decodedSize)
    {
        jcut::EditorTransformKeyframe transform =
            jcut::evaluateEditorClipRenderTransformAtTimelineFrame(
                request.document,
                clip,
                static_cast<std::int64_t>(
                    std::floor(request.timelineFrame)));
        const double localTimelineFrame =
            request.timelineFrame -
            static_cast<double>(clip.startFrame);
        bool bakedSpeakerFramingApplied = false;
        jcut::EditorTransformKeyframe resolvedSpeakerFraming =
            jcut::evaluateEditorClipBakedSpeakerFramingAtLocalFrame(
                clip,
                localTimelineFrame,
                decodedSize.width,
                decodedSize.height,
                request.outputSize.width,
                request.outputSize.height,
                &bakedSpeakerFramingApplied);
        bool speakerFramingApplied =
            bakedSpeakerFramingApplied;
        if (!speakerFramingApplied &&
            clip.speakerFramingEnabled &&
            clip.mediaKind != "title") {
            const jcut::TranscriptOverlayLayoutCore layout =
                transcriptLayout(
                    request.document,
                    clip,
                    resolvedSourcePath,
                    sourceFrame,
                    localTimelineFrame,
                    request.rootDirectory);
            if (layout.speakerTrackingValid) {
                resolvedSpeakerFraming =
                    jcut::
                        evaluateEditorClipSpeakerFramingForFaceBoxAtLocalFrame(
                            clip,
                            localTimelineFrame,
                            layout.speakerTrackingX,
                            layout.speakerTrackingY,
                            layout.speakerTrackingBoxSize,
                            layout.
                                speakerTrackingRotationDegrees,
                            decodedSize.width,
                            decodedSize.height,
                            request.outputSize.width,
                            request.outputSize.height,
                            &speakerFramingApplied);
            } else if (
                layout.speakerTrackingCenterRotationFallback &&
                clip.speakerFramingKeyframes.empty()) {
                const std::int64_t framingLocalFrame =
                    std::max<std::int64_t>(
                        0,
                        static_cast<std::int64_t>(
                            std::floor(localTimelineFrame)));
                bool enabledAtFrame =
                    clip.speakerFramingEnabled;
                for (const auto& keyframe :
                     clip.speakerFramingEnabledKeyframes) {
                    if (keyframe.frame >
                        framingLocalFrame) {
                        break;
                    }
                    enabledAtFrame = keyframe.enabled;
                }
                double targetBox = -1.0;
                for (const auto& keyframe :
                     clip.speakerFramingTargetKeyframes) {
                    if (keyframe.frame >
                        framingLocalFrame) {
                        break;
                    }
                    targetBox = keyframe.scaleX;
                }
                if (enabledAtFrame && targetBox > 0.0) {
                    resolvedSpeakerFraming = {};
                    resolvedSpeakerFraming.frame =
                        framingLocalFrame;
                    resolvedSpeakerFraming.rotation =
                        layout.
                            speakerTrackingRotationDegrees;
                    speakerFramingApplied = true;
                }
            }
        }
        if (speakerFramingApplied &&
            !clip.sourceTransformLocked) {
            transform.frame =
                resolvedSpeakerFraming.frame;
            transform.translationX =
                clip.baseTranslationX +
                resolvedSpeakerFraming.translationX;
            transform.translationY =
                clip.baseTranslationY +
                resolvedSpeakerFraming.translationY;
            transform.rotation =
                clip.baseRotation +
                resolvedSpeakerFraming.rotation;
            transform.scaleX =
                clip.baseScaleX *
                resolvedSpeakerFraming.scaleX;
            transform.scaleY =
                clip.baseScaleY *
                resolvedSpeakerFraming.scaleY;
        }
        return transform;
    }

    MediaSource& sourceForPath(const std::string& path)
    {
        auto it = m_sources.find(path);
        if (it == m_sources.end()) {
            it = m_sources.emplace(
                path,
                std::make_unique<MediaSource>(path, m_decoderPolicy)).first;
        }
        return *it->second;
    }

    std::unordered_map<std::string, std::unique_ptr<MediaSource>> m_sources;
    std::unordered_map<std::string, TranscriptCacheEntry> m_transcripts;
    DecoderPolicySettingsCore m_decoderPolicy;
};

TimelineRenderer::TimelineRenderer()
    : m_impl(std::make_unique<Impl>())
{
}

TimelineRenderer::~TimelineRenderer() = default;

TimelineRenderResult TimelineRenderer::renderFrame(const TimelineRenderRequest& request)
{
    return m_impl->renderFrame(request);
}

TimelineRenderResult renderTimelineFrame(const TimelineRenderRequest& request)
{
    TimelineRenderer renderer;
    return renderer.renderFrame(request);
}

} // namespace jcut::standalone_render
