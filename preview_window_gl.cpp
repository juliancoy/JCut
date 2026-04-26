#include "preview.h"
#include "preview_debug.h"

#include "frame_handle.h"
#include "gl_frame_texture_shared.h"
#include "timeline_cache.h"
#include "playback_frame_pipeline.h"
#include "editor_shared.h"
#include "debug_controls.h"
#include "visual_effects_shader.h"
#include "polygon_triangulation.h"
#include "decoder_image_io.h"

#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QByteArray>
#include <QPainter>
#include <QJsonArray>
#include <QJsonObject>

using namespace editor;

namespace {
constexpr int64_t kMaxHeldPresentationFrameDelta = 8;

const char* overlayVertexShaderSource() {
    return R"(
        attribute vec2 a_position;
        attribute vec2 a_texCoord;
        uniform mat4 u_mvp;
        varying vec2 v_texCoord;
        void main() {
            v_texCoord = a_texCoord;
            gl_Position = u_mvp * vec4(a_position, 0.0, 1.0);
        }
    )";
}

const char* overlayFragmentShaderSource() {
    return R"(
        varying vec2 v_texCoord;
        uniform sampler2D u_texture;
        void main() {
            gl_FragColor = texture2D(u_texture, v_texCoord);
        }
    )";
}

void uploadCurveLutTexture(GLuint* textureId, const TimelineClip::GradingKeyframe& grade) {
    if (!textureId) {
        return;
    }
    const QVector<quint8> lutR = gradingCurveLut8(
        grade.curvePointsR, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    const QVector<quint8> lutG = gradingCurveLut8(
        grade.curvePointsG, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    const QVector<quint8> lutB = gradingCurveLut8(
        grade.curvePointsB, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    const QVector<quint8> lutL = gradingCurveLut8(
        grade.curvePointsLuma, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    if (lutR.isEmpty() || lutG.isEmpty() || lutB.isEmpty() || lutL.isEmpty()) {
        return;
    }
    QByteArray rgbaData;
    rgbaData.resize(TimelineClip::kGradingCurveLutSize * 4);
    for (int i = 0; i < TimelineClip::kGradingCurveLutSize; ++i) {
        rgbaData[i * 4 + 0] = static_cast<char>(lutR[i]);
        rgbaData[i * 4 + 1] = static_cast<char>(lutG[i]);
        rgbaData[i * 4 + 2] = static_cast<char>(lutB[i]);
        rgbaData[i * 4 + 3] = static_cast<char>(lutL[i]);
    }

    if (*textureId == 0) {
        glGenTextures(1, textureId);
    }
    glBindTexture(GL_TEXTURE_2D, *textureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA,
                 TimelineClip::kGradingCurveLutSize,
                 1,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 rgbaData.constData());
    glBindTexture(GL_TEXTURE_2D, 0);
}
}

void PreviewWindow::initializeGL() {
    m_glInitialized = true;
    initializeOpenGLFunctions();
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    m_shaderProgram = std::make_unique<QOpenGLShaderProgram>();
    if (!m_shaderProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, visualEffectsVertexShaderSource()) ||
        !m_shaderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, visualEffectsFragmentShaderSource()) ||
        !m_shaderProgram->link()) {
        qWarning() << "Failed to build preview shader program" << m_shaderProgram->log();
        m_shaderProgram.reset();
        return;
    }
    m_correctionMaskShaderProgram = std::make_unique<QOpenGLShaderProgram>();
    if (!m_correctionMaskShaderProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, correctionMaskVertexShaderSource()) ||
        !m_correctionMaskShaderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, correctionMaskFragmentShaderSource()) ||
        !m_correctionMaskShaderProgram->link()) {
        qWarning() << "Failed to build preview correction mask shader program"
                   << m_correctionMaskShaderProgram->log();
        m_correctionMaskShaderProgram.reset();
        return;
    }
    m_overlayShaderProgram = std::make_unique<QOpenGLShaderProgram>();
    if (!m_overlayShaderProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, overlayVertexShaderSource()) ||
        !m_overlayShaderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, overlayFragmentShaderSource()) ||
        !m_overlayShaderProgram->link()) {
        qWarning() << "Failed to build preview overlay shader program" << m_overlayShaderProgram->log();
        m_overlayShaderProgram.reset();
        return;
    }

    static const GLfloat kQuadVertices[] = {
        -0.5f, -0.5f, 0.0f, 0.0f,
         0.5f, -0.5f, 1.0f, 0.0f,
        -0.5f,  0.5f, 0.0f, 1.0f,
         0.5f,  0.5f, 1.0f, 1.0f,
    };

    m_quadBuffer.create();
    m_quadBuffer.bind();
    m_quadBuffer.allocate(kQuadVertices, sizeof(kQuadVertices));
    m_quadBuffer.release();
    m_polygonBuffer.create();

    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this]() {
        makeCurrent();
        releaseGlResources();
        doneCurrent();
    }, Qt::DirectConnection);
}

void PreviewWindow::resizeGL(int w, int h) { Q_UNUSED(w) Q_UNUSED(h) }

bool PreviewWindow::usingCpuFallback() const {
    return !context() || !isValid() || !m_shaderProgram || !m_correctionMaskShaderProgram;
}

void PreviewWindow::releaseGlResources() {
    if (m_glResourcesReleased) return;
    m_glResourcesReleased = true;

    if (!m_glInitialized || !context() || !context()->isValid()) {
        m_textureCache.clear();
        m_transcriptTextureCache.clear();
        m_overlayShaderProgram.reset();
        m_correctionMaskShaderProgram.reset();
        m_shaderProgram.reset();
        return;
    }
    for (auto it = m_textureCache.begin(); it != m_textureCache.end(); ++it) {
        editor::destroyGlTextureEntry(&it.value());
    }
    m_textureCache.clear();
    for (auto it = m_transcriptTextureCache.begin(); it != m_transcriptTextureCache.end(); ++it) {
        editor::destroyGlTextureEntry(&it.value());
    }
    m_transcriptTextureCache.clear();
    if (m_polygonBuffer.isCreated()) m_polygonBuffer.destroy();
    if (m_quadBuffer.isCreated()) m_quadBuffer.destroy();
    if (m_curveLutTextureId != 0) {
        glDeleteTextures(1, &m_curveLutTextureId);
        m_curveLutTextureId = 0;
    }
    m_overlayShaderProgram.reset();
    m_correctionMaskShaderProgram.reset();
    m_shaderProgram.reset();
}

GLuint PreviewWindow::textureForFrame(const FrameHandle& frame) {
    if (frame.isNull()) return 0;
    const QString key = editor::textureCacheKey(frame);
    const qint64 decodeTimestamp = frame.data() ? frame.data()->decodeTimestamp : 0;
    editor::GlTextureCacheEntry entry = m_textureCache.value(key);
    if (entry.textureId != 0 && entry.decodeTimestamp == decodeTimestamp) {
        entry.lastUsedMs = nowMs();
        m_textureCache.insert(key, entry);
        return entry.textureId;
    }
    editor::destroyGlTextureEntry(&entry);
    if (editor::uploadFrameToGlTextureEntry(frame, &entry)) {
        entry.decodeTimestamp = decodeTimestamp;
        entry.lastUsedMs = nowMs();
        m_textureCache.insert(key, entry);
        trimTextureCache();
        return entry.textureId;
    }
    return 0;
}

void PreviewWindow::trimTextureCache() {
    static constexpr int kMaxTextureCacheEntries = 180;
    editor::trimGlTextureCache(&m_textureCache, kMaxTextureCacheEntries);
}

void PreviewWindow::paintGL() {
    const qint64 renderStartMs = nowMs();
    m_lastPaintMs = renderStartMs;
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glViewport(0, 0, width() * devicePixelRatio(), height() * devicePixelRatio());
    if (m_clipCount > 0) {
        glClearColor(m_backgroundColor.redF(), m_backgroundColor.greenF(), m_backgroundColor.blueF(), 1.0f);
    } else {
        const float phase = static_cast<float>(m_currentFrame % 180) / 179.0f;
        const float motion = m_playing ? phase : 0.25f;
        glClearColor(0.08f + 0.12f * motion, 0.08f, 0.10f + 0.16f * (1.0f - motion), 1.0f);
    }
    glClear(GL_COLOR_BUFFER_BIT);

    QList<TimelineClip> activeClips = getActiveClips();
    const QRect safeRect = rect().adjusted(24, 24, -24, -24);
    const QRect compositeRect = scaledCanvasRect(previewCanvasBaseRect());
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    bool drewAnyFrame = false;
    bool waitingForFrame = false;
    if (m_viewMode == ViewMode::Video) {
        renderCompositedPreviewGL(compositeRect, activeClips, drewAnyFrame, waitingForFrame);
    }
    drawCompositedPreviewOverlay(&painter, safeRect, compositeRect, activeClips, drewAnyFrame, waitingForFrame);
    drawPreviewChrome(&painter, safeRect, activeClips.size());

    // Track render timing
    const qint64 renderEndMs = nowMs();
    m_lastRenderDurationMs = renderEndMs - renderStartMs;
    m_maxRenderDurationMs = qMax(m_maxRenderDurationMs, m_lastRenderDurationMs);
    m_totalRenderDurationMs += m_lastRenderDurationMs;
    ++m_renderCount;
    m_renderTimeHistory.push_back(m_lastRenderDurationMs);
    if (m_renderTimeHistory.size() > kRenderTimeHistorySize) {
        m_renderTimeHistory.pop_front();
    }

    if (m_playing || (m_cache && m_cache->pendingVisibleRequestCount() > 0) ||
        (m_decoder && m_decoder->pendingRequestCount() > 0)) {
        scheduleRepaint();
    }
}

PreviewWindow::PreviewOverlayInfo PreviewWindow::renderFrameLayerGL(const QRect& targetRect,
                                                                    const TimelineClip& clip,
                                                                    const FrameHandle& frame) {
    PreviewOverlayInfo overlayInfo;
    overlayInfo.kind = PreviewOverlayKind::VisualClip;
    if (!m_shaderProgram) {
        return overlayInfo;
    }

    const QString cacheKey = editor::textureCacheKey(frame);
    const GLuint textureId = textureForFrame(frame);
    if (textureId == 0) {
        return overlayInfo;
    }
    const editor::GlTextureCacheEntry entry = m_textureCache.value(cacheKey);

    const QRect fitted = fitRect(frame.size(), targetRect);
    const TimelineClip::TransformKeyframe transform =
        evaluateClipRenderTransformAtPosition(clip, m_currentFramePosition, m_outputSize);
    const QPointF previewScale = previewCanvasScale(targetRect);
    const QPointF center(fitted.center().x() + (transform.translationX * previewScale.x()),
                         fitted.center().y() + (transform.translationY * previewScale.y()));

    QMatrix4x4 projection;
    projection.ortho(0.0f, static_cast<float>(width()),
                     static_cast<float>(height()), 0.0f,
                     -1.0f, 1.0f);

    QMatrix4x4 model;
    model.translate(center.x(), center.y());
    model.rotate(transform.rotation, 0.0f, 0.0f, 1.0f);
    model.scale(fitted.width() * transform.scaleX, fitted.height() * transform.scaleY, 1.0f);

    EffectiveVisualEffects effects = evaluateEffectiveVisualEffectsAtPosition(
        clip, m_tracks, m_currentFramePosition, m_renderSyncMarkers);
    if (m_bypassGrading) {
        effects.grading = TimelineClip::GradingKeyframe{};
    }
    if (!m_correctionsEnabled) {
        effects.correctionPolygons.clear();
    }
    const TimelineClip::GradingKeyframe& grade = effects.grading;
    const qreal brightness = grade.brightness;
    const qreal contrast = grade.contrast;
    const qreal saturation = grade.saturation;
    const qreal opacity = grade.opacity;

    const bool hasCorrections =
        !effects.correctionPolygons.isEmpty() && m_correctionMaskShaderProgram && m_polygonBuffer.isCreated();
    if (hasCorrections) {
        glEnable(GL_STENCIL_TEST);
        glStencilMask(0xFF);
        glClear(GL_STENCIL_BUFFER_BIT);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glStencilFunc(GL_ALWAYS, 1, 0xFF);
        glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);

        m_correctionMaskShaderProgram->bind();
        m_correctionMaskShaderProgram->setUniformValue("u_mvp", projection * model);
        const int correctionPositionLoc = m_correctionMaskShaderProgram->attributeLocation("a_position");
        for (const TimelineClip::CorrectionPolygon& polygon : effects.correctionPolygons) {
            QVector<QPointF> triangleVertices;
            if (!editor::triangulatePolygon(polygon.pointsNormalized, &triangleVertices)) {
                continue;
            }
            QVector<GLfloat> vertices;
            vertices.reserve(triangleVertices.size() * 2);
            for (const QPointF& p : triangleVertices) {
                vertices.push_back(GLfloat(qBound<qreal>(0.0, p.x(), 1.0) - 0.5));
                vertices.push_back(GLfloat(qBound<qreal>(0.0, p.y(), 1.0) - 0.5));
            }
            m_polygonBuffer.bind();
            m_polygonBuffer.allocate(vertices.constData(), int(vertices.size() * sizeof(GLfloat)));
            m_correctionMaskShaderProgram->enableAttributeArray(correctionPositionLoc);
            m_correctionMaskShaderProgram->setAttributeBuffer(
                correctionPositionLoc, GL_FLOAT, 0, 2, 2 * sizeof(GLfloat));
            glDrawArrays(GL_TRIANGLES, 0, vertices.size() / 2);
            m_correctionMaskShaderProgram->disableAttributeArray(correctionPositionLoc);
            m_polygonBuffer.release();
        }
        m_correctionMaskShaderProgram->release();
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glStencilMask(0x00);
        glStencilFunc(GL_EQUAL, 0, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);

        // Explicitly clear alpha to 0 in masked regions (preview alpha mask contract).
        glStencilFunc(GL_EQUAL, 1, 0xFF);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        glDisable(GL_BLEND);
        m_correctionMaskShaderProgram->bind();
        m_correctionMaskShaderProgram->setUniformValue("u_mvp", projection * model);
        const int alphaClearPositionLoc = m_correctionMaskShaderProgram->attributeLocation("a_position");
        m_quadBuffer.bind();
        m_correctionMaskShaderProgram->enableAttributeArray(alphaClearPositionLoc);
        m_correctionMaskShaderProgram->setAttributeBuffer(
            alphaClearPositionLoc, GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_correctionMaskShaderProgram->disableAttributeArray(alphaClearPositionLoc);
        m_quadBuffer.release();
        m_correctionMaskShaderProgram->release();
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glStencilFunc(GL_EQUAL, 0, 0xFF);
    } else {
        glDisable(GL_STENCIL_TEST);
    }

    m_shaderProgram->bind();
    m_shaderProgram->setUniformValue("u_mvp", projection * model);
    m_shaderProgram->setUniformValue("u_brightness", GLfloat(brightness));
    m_shaderProgram->setUniformValue("u_contrast", GLfloat(contrast));
    m_shaderProgram->setUniformValue("u_saturation", GLfloat(saturation));
    m_shaderProgram->setUniformValue("u_opacity", GLfloat(opacity));
    m_shaderProgram->setUniformValue("u_shadows",
        QVector3D(grade.shadowsR, grade.shadowsG, grade.shadowsB));
    m_shaderProgram->setUniformValue("u_midtones",
        QVector3D(grade.midtonesR, grade.midtonesG, grade.midtonesB));
    m_shaderProgram->setUniformValue("u_highlights",
        QVector3D(grade.highlightsR, grade.highlightsG, grade.highlightsB));

    const qreal featherRadius = effects.maskFeather;
    const qreal featherGamma = effects.maskFeatherGamma;
    const QSize frameSize = frame.size();
    const GLfloat texelSizeX = frameSize.width() > 0 ? 1.0f / frameSize.width() : 0.0f;
    const GLfloat texelSizeY = frameSize.height() > 0 ? 1.0f / frameSize.height() : 0.0f;
    m_shaderProgram->setUniformValue("u_feather_radius", GLfloat(featherRadius));
    m_shaderProgram->setUniformValue("u_feather_gamma", GLfloat(featherGamma));
    m_shaderProgram->setUniformValue("u_texel_size", QVector2D(texelSizeX, texelSizeY));
    m_shaderProgram->setUniformValue("u_texture", 0);
    m_shaderProgram->setUniformValue("u_texture_uv", 1);
    m_shaderProgram->setUniformValue("u_curve_lut", 2);
    m_shaderProgram->setUniformValue("u_curve_enabled", 1.0f);
    m_shaderProgram->setUniformValue("u_texture_mode", entry.usesYuvTextures ? 1.0f : 0.0f);
    uploadCurveLutTexture(&m_curveLutTextureId, grade);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureId);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, entry.usesYuvTextures ? entry.auxTextureId : 0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_curveLutTextureId);
    m_quadBuffer.bind();
    const int positionLoc = m_shaderProgram->attributeLocation("a_position");
    const int texCoordLoc = m_shaderProgram->attributeLocation("a_texCoord");
    m_shaderProgram->enableAttributeArray(positionLoc);
    m_shaderProgram->enableAttributeArray(texCoordLoc);
    m_shaderProgram->setAttributeBuffer(positionLoc, GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
    m_shaderProgram->setAttributeBuffer(texCoordLoc, GL_FLOAT, 2 * sizeof(GLfloat), 2, 4 * sizeof(GLfloat));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_shaderProgram->disableAttributeArray(positionLoc);
    m_shaderProgram->disableAttributeArray(texCoordLoc);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_quadBuffer.release();
    m_shaderProgram->release();
    if (hasCorrections) {
        glStencilMask(0xFF);
        glDisable(GL_STENCIL_TEST);
    }

    QTransform overlayTransform;
    overlayTransform.translate(center.x(), center.y());
    overlayTransform.rotate(transform.rotation);
    overlayTransform.scale(transform.scaleX, transform.scaleY);
    overlayInfo.bounds = overlayTransform.mapRect(QRectF(-fitted.width() / 2.0,
                                                         -fitted.height() / 2.0,
                                                         fitted.width(),
                                                         fitted.height()));
    overlayInfo.clipTransform = overlayTransform;
    overlayInfo.clipPixelSize = QSizeF(fitted.width(), fitted.height());
    return overlayInfo;
}

void PreviewWindow::renderCompositedPreviewGL(const QRect& compositeRect,
                                              const QList<TimelineClip>& activeClips,
                                              bool& drewAnyFrame,
                                              bool& waitingForFrame) {
    m_overlayInfo.clear();
    m_paintOrder.clear();
    int usedPlaybackPipelineCount = 0;
    int presentationCount = 0;
    int exactCount = 0;
    int bestCount = 0;
    int heldCount = 0;
    int staleRejectedCount = 0;
    int nullCount = 0;
    int skippedZeroOpacityCount = 0;
    QJsonArray clipSelections;
    GLboolean previousScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    GLint previousScissorBox[4] = {0, 0, 0, 0};
    if (m_hideOutsideOutputWindow) {
        glGetIntegerv(GL_SCISSOR_BOX, previousScissorBox);
        glEnable(GL_SCISSOR_TEST);
        glScissor(compositeRect.left(),
                  height() - compositeRect.bottom() - 1,
                  compositeRect.width(),
                  compositeRect.height());
    }
    for (const TimelineClip& clip : activeClips) {
        if (!clipVisualPlaybackEnabled(clip, m_tracks)) {
            continue;
        }
        if (clip.mediaType == ClipMediaType::Title) {
            continue; // Title clips are drawn as text overlays, not decoded frames
        }
        if (!m_bypassGrading) {
            const EffectiveVisualEffects effects = evaluateEffectiveVisualEffectsAtPosition(
                clip, m_tracks, m_currentFramePosition, m_renderSyncMarkers);
            if (effects.grading.opacity <= 0.0001) {
                ++skippedZeroOpacityCount;
                clipSelections.append(QJsonObject{
                    {QStringLiteral("id"), clip.id},
                    {QStringLiteral("label"), clip.label},
                    {QStringLiteral("media_type"), clipMediaTypeLabel(clip.mediaType)},
                    {QStringLiteral("source_kind"), mediaSourceKindLabel(clip.sourceKind)},
                    {QStringLiteral("playback_pipeline"), false},
                    {QStringLiteral("local_frame"), static_cast<qint64>(sourceFrameForSample(clip, m_currentSample))},
                    {QStringLiteral("selection"), QStringLiteral("skipped_zero_opacity")},
                    {QStringLiteral("frame_storage"), QStringLiteral("none")}
                });
                continue;
            }
        }
        const int64_t localFrame = sourceFrameForSample(clip, m_currentSample);
        const bool usePlaybackPipeline =
            m_playing &&
            clip.sourceKind == MediaSourceKind::ImageSequence &&
            clip.mediaType != ClipMediaType::Image;
        const bool allowApproximateFrame =
            m_playing &&
            (usePlaybackPipeline || !m_cache ||
             m_cache->shouldAllowApproximatePreviewFrame(clip.id, localFrame, nowMs()));
        QString selection = QStringLiteral("none");
        const FrameHandle exactFrame = usePlaybackPipeline
                                           ? m_playbackPipeline->getFrame(clip.id, localFrame)
                                           : (m_cache ? m_cache->getCachedFrame(clip.id, localFrame) : FrameHandle());
        FrameHandle frame;
        if (usePlaybackPipeline) {
            ++usedPlaybackPipelineCount;
            frame = m_playbackPipeline->getPresentationFrame(clip.id, localFrame);
            if (!frame.isNull()) {
                ++presentationCount;
                selection = QStringLiteral("presentation");
            }
        } else {
            frame = exactFrame;
            if (frame.isNull() && m_cache && allowApproximateFrame) {
                frame = m_playing ? m_cache->getLatestCachedFrame(clip.id, localFrame)
                                  : m_cache->getBestCachedFrame(clip.id, localFrame);
                if (frame.isNull() && editor::debugPlaybackCacheFallbackEnabled()) {
                    frame = m_cache->getBestCachedFrame(clip.id, localFrame);
                }
            }
            if (!frame.isNull()) {
                if (!exactFrame.isNull() && frame == exactFrame) {
                    ++exactCount;
                    selection = QStringLiteral("exact");
                } else {
                    ++bestCount;
                    selection = m_playing ? QStringLiteral("latest") : QStringLiteral("best");
                }
            }
        }
        if (usePlaybackPipeline && frame.isNull()) {
            frame = !exactFrame.isNull() ? exactFrame
                                         : m_playbackPipeline->getBestFrame(clip.id, localFrame);
            if (!frame.isNull()) {
                if (!exactFrame.isNull() && frame == exactFrame) {
                    ++exactCount;
                    selection = QStringLiteral("exact");
                } else {
                    ++bestCount;
                    selection = QStringLiteral("best");
                }
            }
        }
        if (usePlaybackPipeline && frame.isNull() && m_cache) {
            const FrameHandle cacheExact = m_cache->getCachedFrame(clip.id, localFrame);
            frame = !cacheExact.isNull()
                        ? cacheExact
                        : (m_playing
                               ? m_cache->getLatestCachedFrame(clip.id, localFrame)
                               : m_cache->getBestCachedFrame(clip.id, localFrame));
            if (!frame.isNull()) {
                if (!cacheExact.isNull() && frame == cacheExact) {
                    ++exactCount;
                    selection = QStringLiteral("exact");
                } else {
                    ++bestCount;
                    selection = m_playing ? QStringLiteral("latest") : QStringLiteral("best");
                }
            }
        }
        if (usePlaybackPipeline) {
            if (!frame.isNull()) {
                m_lastPresentedFrames.insert(clip.id, frame);
            } else {
                const FrameHandle heldFrame = m_lastPresentedFrames.value(clip.id);
                if (!heldFrame.isNull() &&
                    qAbs(heldFrame.frameNumber() - localFrame) <= kMaxHeldPresentationFrameDelta) {
                    frame = heldFrame;
                    ++heldCount;
                    selection = QStringLiteral("held");
                } else {
                    m_lastPresentedFrames.remove(clip.id);
                }
            }
        }
        if (isFrameTooStaleForPlayback(clip, localFrame, frame)) {
            frame = FrameHandle();
            selection = QStringLiteral("stale");
            ++staleRejectedCount;
            m_lastPresentedFrames.remove(clip.id);
        }
        playbackFrameSelectionTrace(QStringLiteral("PreviewWindow::renderCompositedPreviewGL.select"),
                                    clip,
                                    localFrame,
                                    exactFrame,
                                    frame,
                                    m_currentFramePosition);
        if (frame.isNull()) {
            if (usePlaybackPipeline && m_playbackPipeline && m_playing) {
                static constexpr int kMaxVisibleBacklog = 4;
                if (m_playbackPipeline->pendingVisibleRequestCount() < kMaxVisibleBacklog) {
                    m_lastFrameRequestMs = nowMs();
                    m_playbackPipeline->requestFramesForSample(
                        m_currentSample,
                        [this]() {
                            QMetaObject::invokeMethod(this, [this]() {
                                scheduleRepaint();
                            }, Qt::QueuedConnection);
                        });
                }
            } else if (m_cache &&
                       (!m_cache->isVisibleRequestPending(clip.id, localFrame) ||
                        m_cache->shouldForceVisibleRequestRetry(clip.id, localFrame, 250))) {
                m_lastFrameRequestMs = nowMs();
                m_cache->requestFrame(
                    clip.id,
                    localFrame,
                    [this](FrameHandle delivered) {
                        Q_UNUSED(delivered)
                        QMetaObject::invokeMethod(this, [this]() {
                            scheduleRepaint();
                        }, Qt::QueuedConnection);
                    });
            }

            // For static images, try to load them synchronously as a last resort
            if (clip.mediaType == ClipMediaType::Image && !clip.filePath.isEmpty()) {
                // Try to load the image synchronously
                QImage image = editor::loadSingleImageFile(clip.filePath);
                if (!image.isNull()) {
                    // Create a frame handle from the loaded image
                    frame = FrameHandle::createCpuFrame(image, localFrame, clip.filePath);
                    // Store it in cache for future use
                    if (m_cache) {
                        m_cache->requestFrame(clip.id, localFrame, [](FrameHandle){});
                    }
                    selection = QStringLiteral("sync-loaded");
                }
            }
            
            if (frame.isNull()) {
                ++nullCount;
                selection = QStringLiteral("null");
                waitingForFrame = true;
                clipSelections.append(QJsonObject{
                    {QStringLiteral("id"), clip.id},
                    {QStringLiteral("label"), clip.label},
                    {QStringLiteral("media_type"), clipMediaTypeLabel(clip.mediaType)},
                    {QStringLiteral("source_kind"), mediaSourceKindLabel(clip.sourceKind)},
                    {QStringLiteral("playback_pipeline"), usePlaybackPipeline},
                    {QStringLiteral("local_frame"), static_cast<qint64>(localFrame)},
                    {QStringLiteral("selection"), selection},
                    {QStringLiteral("frame_storage"), QStringLiteral("none")}
                });
                continue;
            }
        }
        clipSelections.append(QJsonObject{
            {QStringLiteral("id"), clip.id},
            {QStringLiteral("label"), clip.label},
            {QStringLiteral("media_type"), clipMediaTypeLabel(clip.mediaType)},
            {QStringLiteral("source_kind"), mediaSourceKindLabel(clip.sourceKind)},
            {QStringLiteral("playback_pipeline"), usePlaybackPipeline},
            {QStringLiteral("local_frame"), static_cast<qint64>(localFrame)},
            {QStringLiteral("frame_number"), static_cast<qint64>(frame.frameNumber())},
            {QStringLiteral("selection"), selection},
            {QStringLiteral("frame_storage"),
             frame.hasHardwareFrame() ? QStringLiteral("hardware")
                                      : (frame.hasCpuImage() ? QStringLiteral("cpu")
                                                             : QStringLiteral("unknown"))}
        });

        PreviewOverlayInfo info = renderFrameLayerGL(compositeRect, clip, frame);
        if (!info.bounds.isEmpty()) {
            const QRectF bounds = info.bounds;
            constexpr qreal kHandleSize = 12.0;
            info.rightHandle = QRectF(bounds.right() - kHandleSize,
                                      bounds.center().y() - kHandleSize,
                                      kHandleSize,
                                      kHandleSize * 2.0);
            info.bottomHandle = QRectF(bounds.center().x() - kHandleSize,
                                       bounds.bottom() - kHandleSize,
                                       kHandleSize * 2.0,
                                       kHandleSize);
            info.cornerHandle = QRectF(bounds.right() - kHandleSize * 1.5,
                                       bounds.bottom() - kHandleSize * 1.5,
                                       kHandleSize * 1.5,
                                       kHandleSize * 1.5);
            m_overlayInfo.insert(clip.id, info);
            m_paintOrder.push_back(clip.id);
        }
        drewAnyFrame = true;
    }
    if (m_hideOutsideOutputWindow) {
        glScissor(previousScissorBox[0], previousScissorBox[1],
                  previousScissorBox[2], previousScissorBox[3]);
        if (!previousScissorEnabled) {
            glDisable(GL_SCISSOR_TEST);
        }
    }
    for (const TimelineClip& clip : activeClips) {
        if (clipShowsTranscriptOverlay(clip)) {
            drawTranscriptOverlayGL(clip, compositeRect);
        }
    }
    m_lastFrameSelectionStats = QJsonObject{
        {QStringLiteral("path"), QStringLiteral("gl")},
        {QStringLiteral("active_visual_clips"), activeClips.size()},
        {QStringLiteral("use_playback_pipeline_clips"), usedPlaybackPipelineCount},
        {QStringLiteral("presentation"), presentationCount},
        {QStringLiteral("exact"), exactCount},
        {QStringLiteral("best"), bestCount},
        {QStringLiteral("held"), heldCount},
        {QStringLiteral("stale_rejected"), staleRejectedCount},
        {QStringLiteral("null"), nullCount},
        {QStringLiteral("skipped_zero_opacity"), skippedZeroOpacityCount},
        {QStringLiteral("clips"), clipSelections}
    };
}
