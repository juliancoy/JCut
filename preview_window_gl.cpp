#include "preview.h"
#include "preview_debug.h"

#include "frame_handle.h"
#include "gl_frame_texture_shared.h"
#include "timeline_cache.h"
#include "playback_frame_pipeline.h"
#include "editor_shared.h"

#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QPainter>
#include <QJsonArray>
#include <QJsonObject>

using namespace editor;

void PreviewWindow::initializeGL() {
    m_glInitialized = true;
    initializeOpenGLFunctions();
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    static const char* kVertexShader = R"(
        attribute vec2 a_position;
        attribute vec2 a_texCoord;
        uniform mat4 u_mvp;
        varying vec2 v_texCoord;
        void main() {
            v_texCoord = a_texCoord;
            gl_Position = u_mvp * vec4(a_position, 0.0, 1.0);
        }
    )";

    static const char* kFragmentShader = R"(
        uniform sampler2D u_texture;
        uniform sampler2D u_texture_uv;
        uniform float u_texture_mode;
        uniform float u_brightness;
        uniform float u_contrast;
        uniform float u_saturation;
        uniform float u_opacity;
        uniform float u_feather_radius;
        uniform float u_feather_gamma;
        uniform vec2 u_texel_size;
        // Shadows/Midtones/Highlights (Lift/Gamma/Gain)
        uniform vec3 u_shadows;      // Lift - affects dark areas
        uniform vec3 u_midtones;     // Gamma - affects mid-range
        uniform vec3 u_highlights;   // Gain - affects bright areas
        varying vec2 v_texCoord;
        
        // Smooth curve for tone range blending
        float smoothShadows(float luma) {
            return pow(1.0 - luma, 2.0);
        }
        float smoothMidtones(float luma) {
            return 1.0 - abs(luma - 0.5) * 2.0;
        }
        float smoothHighlights(float luma) {
            return pow(luma, 2.0);
        }
        
        void main() {
            vec4 color;
            float sourceAlpha;
            vec3 rgb;
            if (u_texture_mode > 0.5) {
                float y = texture2D(u_texture, v_texCoord).r;
                vec2 uv = texture2D(u_texture_uv, v_texCoord).rg - vec2(0.5, 0.5);
                rgb = vec3(y + 1.4020 * uv.y,
                           y - 0.344136 * uv.x - 0.714136 * uv.y,
                           y + 1.7720 * uv.x);
                rgb = clamp(rgb, 0.0, 1.0);
                sourceAlpha = 1.0;
            } else {
                color = texture2D(u_texture, v_texCoord);
                sourceAlpha = color.a;
                rgb = color.rgb;
                if (sourceAlpha > 0.0001) rgb /= sourceAlpha;
                else rgb = vec3(0.0);
            }
            
            // Apply mask feather (box blur on alpha)
            if (u_feather_radius > 0.0 && sourceAlpha > 0.0) {
                float alphaSum = 0.0;
                int sampleCount = 0;
                int radius = int(ceil(u_feather_radius));
                for (int dy = -radius; dy <= radius; dy++) {
                    for (int dx = -radius; dx <= radius; dx++) {
                        vec2 offset = vec2(float(dx), float(dy)) * u_texel_size;
                        vec2 sampleCoord = clamp(v_texCoord + offset, 0.0, 1.0);
                        if (u_texture_mode > 0.5) {
                            alphaSum += 1.0;
                        } else {
                            alphaSum += texture2D(u_texture, sampleCoord).a;
                        }
                        sampleCount++;
                    }
                }
                float blurredAlpha = alphaSum / float(sampleCount);
                // Apply gamma curve to feather (1.0 = linear, <1.0 = sharper edges, >1.0 = softer edges)
                sourceAlpha = pow(blurredAlpha, 1.0 / max(0.01, u_feather_gamma));
            }
            
            // Calculate luminance for tone-based grading
            float luminance = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
            
            // Apply Shadows/Lift (multiply, affects dark areas)
            float shadowWeight = smoothShadows(luminance);
            rgb *= (1.0 + u_shadows * shadowWeight);
            
            // Apply Midtones/Gamma (power curve, affects mid-range)
            float midtoneWeight = smoothMidtones(luminance);
            vec3 midtoneAdjust = u_midtones * midtoneWeight;
            rgb = pow(rgb, vec3(1.0) / (vec3(1.0) + midtoneAdjust));
            
            // Apply Highlights/Gain (addition, affects bright areas)
            float highlightWeight = smoothHighlights(luminance);
            rgb += u_highlights * highlightWeight;
            
            // Basic grading
            rgb = ((rgb - 0.5) * u_contrast) + 0.5 + vec3(u_brightness);
            rgb = mix(vec3(luminance), rgb, u_saturation);
            rgb = clamp(rgb, 0.0, 1.0);
            color.a = clamp(sourceAlpha * u_opacity, 0.0, 1.0);
            color.rgb = rgb * color.a;
            gl_FragColor = color;
        }
    )";

    m_shaderProgram = std::make_unique<QOpenGLShaderProgram>();
    if (!m_shaderProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader) ||
        !m_shaderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader) ||
        !m_shaderProgram->link()) {
        qWarning() << "Failed to build preview shader program" << m_shaderProgram->log();
        m_shaderProgram.reset();
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

    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this]() {
        makeCurrent();
        releaseGlResources();
        doneCurrent();
    }, Qt::DirectConnection);
}

void PreviewWindow::resizeGL(int w, int h) { Q_UNUSED(w) Q_UNUSED(h) }

bool PreviewWindow::usingCpuFallback() const { return !context() || !isValid() || !m_shaderProgram; }

void PreviewWindow::releaseGlResources() {
    if (m_glResourcesReleased) return;
    m_glResourcesReleased = true;

    if (!m_glInitialized || !context() || !context()->isValid()) {
        m_textureCache.clear();
        m_shaderProgram.reset();
        return;
    }
    for (auto it = m_textureCache.begin(); it != m_textureCache.end(); ++it) {
        editor::destroyGlTextureEntry(&it.value());
    }
    m_textureCache.clear();
    if (m_quadBuffer.isCreated()) m_quadBuffer.destroy();
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
    renderCompositedPreviewGL(compositeRect, activeClips, drewAnyFrame, waitingForFrame);
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

QRectF PreviewWindow::renderFrameLayerGL(const QRect& targetRect, const TimelineClip& clip, const FrameHandle& frame) {
    if (!m_shaderProgram) {
        return QRectF();
    }

    const QString cacheKey = editor::textureCacheKey(frame);
    const GLuint textureId = textureForFrame(frame);
    if (textureId == 0) {
        return QRectF();
    }
    const editor::GlTextureCacheEntry entry = m_textureCache.value(cacheKey);

    const QRect fitted = fitRect(frame.size(), targetRect);
    const TimelineClip::TransformKeyframe transform = evaluateClipTransformAtPosition(clip, m_currentFramePosition);
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

    const TimelineClip::GradingKeyframe grade =
        m_bypassGrading ? TimelineClip::GradingKeyframe{} : evaluateClipGradingAtPosition(clip, m_currentFramePosition);
    const qreal brightness = grade.brightness;
    const qreal contrast = grade.contrast;
    const qreal saturation = grade.saturation;
    const qreal opacity = grade.opacity;

    m_shaderProgram->bind();
    m_shaderProgram->setUniformValue("u_mvp", projection * model);
    m_shaderProgram->setUniformValue("u_brightness", GLfloat(brightness));
    m_shaderProgram->setUniformValue("u_contrast", GLfloat(contrast));
    m_shaderProgram->setUniformValue("u_saturation", GLfloat(saturation));
    m_shaderProgram->setUniformValue("u_opacity", GLfloat(opacity));
    
    // Shadows/Midtones/Highlights uniforms
    m_shaderProgram->setUniformValue("u_shadows", 
        QVector3D(grade.shadowsR, grade.shadowsG, grade.shadowsB));
    m_shaderProgram->setUniformValue("u_midtones", 
        QVector3D(grade.midtonesR, grade.midtonesG, grade.midtonesB));
    m_shaderProgram->setUniformValue("u_highlights", 
        QVector3D(grade.highlightsR, grade.highlightsG, grade.highlightsB));
    
    // Mask feather uniforms
    const qreal featherRadius = clip.maskFeather;
    const qreal featherGamma = clip.maskFeatherGamma;
    const QSize frameSize = frame.size();
    const GLfloat texelSizeX = frameSize.width() > 0 ? 1.0f / frameSize.width() : 0.0f;
    const GLfloat texelSizeY = frameSize.height() > 0 ? 1.0f / frameSize.height() : 0.0f;
    m_shaderProgram->setUniformValue("u_feather_radius", GLfloat(featherRadius));
    m_shaderProgram->setUniformValue("u_feather_gamma", GLfloat(featherGamma));
    m_shaderProgram->setUniformValue("u_texel_size", QVector2D(texelSizeX, texelSizeY));
    
    m_shaderProgram->setUniformValue("u_texture", 0);
    m_shaderProgram->setUniformValue("u_texture_uv", 1);
    m_shaderProgram->setUniformValue("u_texture_mode", entry.usesYuvTextures ? 1.0f : 0.0f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureId);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, entry.usesYuvTextures ? entry.auxTextureId : 0);
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
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_quadBuffer.release();
    m_shaderProgram->release();

    QTransform overlayTransform;
    overlayTransform.translate(center.x(), center.y());
    overlayTransform.rotate(transform.rotation);
    overlayTransform.scale(transform.scaleX, transform.scaleY);
    return overlayTransform.mapRect(QRectF(-fitted.width() / 2.0,
                                           -fitted.height() / 2.0,
                                           fitted.width(),
                                           fitted.height()));
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
        if (!clipVisualPlaybackEnabled(clip)) {
            continue;
        }
        if (clip.mediaType == ClipMediaType::Title) {
            continue; // Title clips are drawn as text overlays, not decoded frames
        }
        if (!m_bypassGrading) {
            const TimelineClip::GradingKeyframe grade =
                evaluateClipGradingAtPosition(clip, m_currentFramePosition);
            if (grade.opacity <= 0.0001) {
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
            frame = exactFrame.isNull() && m_cache
                        ? (m_playing
                               ? m_cache->getLatestCachedFrame(clip.id, localFrame)
                               : m_cache->getBestCachedFrame(clip.id, localFrame))
                        : exactFrame;
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
                frame = m_lastPresentedFrames.value(clip.id);
                if (!frame.isNull()) {
                    ++heldCount;
                    selection = QStringLiteral("held");
                }
            }
        }
        playbackFrameSelectionTrace(QStringLiteral("PreviewWindow::renderCompositedPreviewGL.select"),
                                    clip,
                                    localFrame,
                                    exactFrame,
                                    frame,
                                    m_currentFramePosition);
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

        const QRectF bounds = renderFrameLayerGL(compositeRect, clip, frame);
        if (!bounds.isEmpty()) {
            PreviewOverlayInfo info;
            info.kind = PreviewOverlayKind::VisualClip;
            info.bounds = bounds;
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
    m_lastFrameSelectionStats = QJsonObject{
        {QStringLiteral("path"), QStringLiteral("gl")},
        {QStringLiteral("active_visual_clips"), activeClips.size()},
        {QStringLiteral("use_playback_pipeline_clips"), usedPlaybackPipelineCount},
        {QStringLiteral("presentation"), presentationCount},
        {QStringLiteral("exact"), exactCount},
        {QStringLiteral("best"), bestCount},
        {QStringLiteral("held"), heldCount},
        {QStringLiteral("null"), nullCount},
        {QStringLiteral("skipped_zero_opacity"), skippedZeroOpacityCount},
        {QStringLiteral("clips"), clipSelections}
    };
}
