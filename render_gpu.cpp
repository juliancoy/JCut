#include "render_internal.h"
#include "visual_effects_shader.h"
#include "polygon_triangulation.h"
#include "titles.h"

#include <QByteArray>

namespace render_detail {

namespace {
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

QVector<TimelineClip> sortedVisualClips(const QVector<TimelineClip>& clips,
                                        const QVector<TimelineTrack>& tracks) {
    QVector<TimelineClip> visual;
    for (const TimelineClip& clip : clips) {
        if (clipVisualPlaybackEnabled(clip, tracks)) {
            visual.push_back(clip);
        }
    }
    std::sort(visual.begin(), visual.end(), [](const TimelineClip& a, const TimelineClip& b) {
        if (a.trackIndex == b.trackIndex) {
            return clipTimelineStartSamples(a) < clipTimelineStartSamples(b);
        }
        return a.trackIndex > b.trackIndex;
    });
    return visual;
}

class OffscreenGpuRendererPrivate : protected QOpenGLFunctions {
public:
    OffscreenGpuRendererPrivate()
        : m_quadBuffer(QOpenGLBuffer::VertexBuffer)
        , m_polygonBuffer(QOpenGLBuffer::VertexBuffer) {}

    ~OffscreenGpuRendererPrivate() {
        releaseResources();
    }

    bool initialize(const QSize& outputSize, QString* errorMessage) {
        m_outputSize = outputSize;

        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setProfile(QSurfaceFormat::CompatibilityProfile);
        format.setVersion(2, 0);

        m_surface = std::make_unique<QOffscreenSurface>();
        m_surface->setFormat(format);
        m_surface->create();
        if (!m_surface->isValid()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create offscreen OpenGL surface.");
            }
            return false;
        }

        m_context = std::make_unique<QOpenGLContext>();
        m_context->setFormat(format);
        if (!m_context->create()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create OpenGL context for render export.");
            }
            return false;
        }
        if (!m_context->makeCurrent(m_surface.get())) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to activate OpenGL context for render export.");
            }
            return false;
        }

        initializeOpenGLFunctions();
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        m_shaderProgram = std::make_unique<QOpenGLShaderProgram>();
        if (!m_shaderProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, editor::visualEffectsVertexShaderSource()) ||
            !m_shaderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, editor::visualEffectsFragmentShaderSource()) ||
            !m_shaderProgram->link()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to build offscreen render shader pipeline.");
            }
            return false;
        }
        m_correctionMaskShaderProgram = std::make_unique<QOpenGLShaderProgram>();
        if (!m_correctionMaskShaderProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, editor::correctionMaskVertexShaderSource()) ||
            !m_correctionMaskShaderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, editor::correctionMaskFragmentShaderSource()) ||
            !m_correctionMaskShaderProgram->link()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to build correction mask shader pipeline.");
            }
            return false;
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

        QOpenGLFramebufferObjectFormat fboFormat;
        fboFormat.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        fboFormat.setInternalTextureFormat(GL_RGBA8);
        m_fbo = std::make_unique<QOpenGLFramebufferObject>(m_outputSize, fboFormat);
        if (!m_fbo->isValid()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create offscreen framebuffer for render export.");
            }
            return false;
        }

        QOpenGLFramebufferObjectFormat yFboFormat;
        yFboFormat.setAttachment(QOpenGLFramebufferObject::NoAttachment);
        yFboFormat.setInternalTextureFormat(GL_R8);
        m_nv12YFbo = std::make_unique<QOpenGLFramebufferObject>(m_outputSize.width(), m_outputSize.height(), yFboFormat);
        if (!m_nv12YFbo->isValid()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create NV12 luma framebuffer for render export.");
            }
            return false;
        }

        QOpenGLFramebufferObjectFormat uvFboFormat;
        uvFboFormat.setAttachment(QOpenGLFramebufferObject::NoAttachment);
        uvFboFormat.setInternalTextureFormat(GL_RG8);
        m_nv12UvFbo = std::make_unique<QOpenGLFramebufferObject>(qMax(1, m_outputSize.width() / 2),
                                                                 qMax(1, m_outputSize.height() / 2),
                                                                 uvFboFormat);
        if (!m_nv12UvFbo->isValid()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create NV12 chroma framebuffer for render export.");
            }
            return false;
        }

        static const char* kNv12VertexShader = R"(
            attribute vec2 a_position;
            attribute vec2 a_texCoord;
            varying vec2 v_texCoord;
            void main() {
                v_texCoord = a_texCoord;
                gl_Position = vec4(a_position * 2.0, 0.0, 1.0);
            }
        )";

        static const char* kNv12YFragmentShader = R"(
            uniform sampler2D u_texture;
            varying vec2 v_texCoord;
            void main() {
                vec2 coord = vec2(v_texCoord.x, 1.0 - v_texCoord.y);
                vec3 rgb = texture2D(u_texture, coord).rgb;
                float y = dot(rgb, vec3(0.2578125, 0.50390625, 0.09765625)) + 0.0625;
                gl_FragColor = vec4(y, 0.0, 0.0, 1.0);
            }
        )";

        static const char* kNv12UvFragmentShader = R"(
            uniform sampler2D u_texture;
            uniform vec2 u_texel_size;
            varying vec2 v_texCoord;
            vec3 sampleRgb(vec2 coord) {
                return texture2D(u_texture, coord).rgb;
            }
            void main() {
                vec2 base = vec2(v_texCoord.x, 1.0 - v_texCoord.y);
                vec2 offset = u_texel_size * 0.5;
                vec3 rgb0 = sampleRgb(base + vec2(-offset.x, -offset.y));
                vec3 rgb1 = sampleRgb(base + vec2( offset.x, -offset.y));
                vec3 rgb2 = sampleRgb(base + vec2(-offset.x,  offset.y));
                vec3 rgb3 = sampleRgb(base + vec2( offset.x,  offset.y));
                vec3 rgb = (rgb0 + rgb1 + rgb2 + rgb3) * 0.25;
                float u = dot(rgb, vec3(-0.1484375, -0.2890625, 0.4375)) + 0.5;
                float v = dot(rgb, vec3(0.4375, -0.3671875, -0.0703125)) + 0.5;
                gl_FragColor = vec4(u, v, 0.0, 1.0);
            }
        )";

        m_nv12YShaderProgram = std::make_unique<QOpenGLShaderProgram>();
        if (!m_nv12YShaderProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, kNv12VertexShader) ||
            !m_nv12YShaderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, kNv12YFragmentShader) ||
            !m_nv12YShaderProgram->link()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to build NV12 luma conversion shader.");
            }
            return false;
        }

        m_nv12UvShaderProgram = std::make_unique<QOpenGLShaderProgram>();
        if (!m_nv12UvShaderProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, kNv12VertexShader) ||
            !m_nv12UvShaderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, kNv12UvFragmentShader) ||
            !m_nv12UvShaderProgram->link()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to build NV12 chroma conversion shader.");
            }
            return false;
        }

        return true;
    }

    QImage renderFrame(const RenderRequest& request,
                       int64_t timelineFrame,
                       QHash<QString, editor::DecoderContext*>& decoders,
                       editor::AsyncDecoder* asyncDecoder,
                       QHash<RenderAsyncFrameKey, editor::FrameHandle>* asyncFrameCache,
                       const QVector<TimelineClip>& orderedClips,
                       QHash<QString, RenderClipStageStats>* clipStageStats,
                       qint64* decodeMs,
                       qint64* textureMs,
                       qint64* compositeMs,
                       qint64* readbackMs,
                       QJsonArray* skippedClips,
                       QJsonObject* skippedReasonCounts) {
        if (!m_context || !m_surface || !m_fbo || !m_shaderProgram) {
            return QImage();
        }
        if (!m_context->makeCurrent(m_surface.get())) {
            return QImage();
        }

        m_fbo->bind();
        glViewport(0, 0, m_outputSize.width(), m_outputSize.height());
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        for (const TimelineClip& clip : orderedClips) {
            if (timelineFrame < clip.startFrame || timelineFrame >= clip.startFrame + clip.durationFrames) {
                continue;
            }

            EffectiveVisualEffects effects = evaluateEffectiveVisualEffectsAtPosition(
                clip, request.tracks, static_cast<qreal>(timelineFrame), request.renderSyncMarkers);
            if (!request.correctionsEnabled) {
                effects.correctionPolygons.clear();
            }
            const TimelineClip::GradingKeyframe& grade = effects.grading;
            if (grade.opacity <= 0.0001) {
                recordRenderSkip(skippedClips, skippedReasonCounts, clip, QStringLiteral("zero_opacity"), timelineFrame);
                continue;
            }

            // Handle title clips specially - they don't have video files
            if (clip.mediaType == ClipMediaType::Title) {
                if (clip.titleKeyframes.isEmpty()) {
                    // No title keyframes to render
                    continue;
                }
                
                const int64_t localFrame = timelineFrame - clip.startFrame;
                const EvaluatedTitle evaluatedTitle = evaluateTitleAtLocalFrame(clip, localFrame);
                const EvaluatedTitle title =
                    composeTitleWithOpacity(evaluatedTitle, static_cast<qreal>(grade.opacity));
                if (!title.valid || title.text.isEmpty() || title.opacity <= 0.001) {
                    continue;
                }
                
                QElapsedTimer decodeTimer;
                decodeTimer.start();
                
                // Render title to an image
                QImage titleImage(m_outputSize, QImage::Format_ARGB32_Premultiplied);
                titleImage.fill(Qt::transparent);
                QPainter titlePainter(&titleImage);
                titlePainter.setRenderHint(QPainter::Antialiasing, true);
                drawTitleOverlay(&titlePainter, QRect(QPoint(0, 0), m_outputSize), title, m_outputSize);
                titlePainter.end();
                
                const qint64 decodeElapsed = decodeTimer.elapsed();
                if (decodeMs) {
                    *decodeMs += decodeElapsed;
                }
                
                QElapsedTimer textureTimer;
                textureTimer.start();
                
                // Create texture from title image
                GLuint titleTextureId = 0;
                glGenTextures(1, &titleTextureId);
                glBindTexture(GL_TEXTURE_2D, titleTextureId);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                
                // Convert QImage to RGBA format for OpenGL
                // Feed title overlays as straight-alpha RGBA and skip shader unpremultiply.
                QImage rgbaImage = titleImage.convertToFormat(QImage::Format_RGBA8888);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rgbaImage.width(), rgbaImage.height(), 0,
                            GL_RGBA, GL_UNSIGNED_BYTE, rgbaImage.constBits());
                
                const qint64 textureElapsed = textureTimer.elapsed();
                if (textureMs) {
                    *textureMs += textureElapsed;
                }
                
                // Use a dummy texture entry for title
                editor::GlTextureCacheEntry titleTextureEntry;
                titleTextureEntry.textureId = titleTextureId;
                titleTextureEntry.auxTextureId = 0;
                titleTextureEntry.usesYuvTextures = false;
                titleTextureEntry.decodeTimestamp = 0;
                titleTextureEntry.lastUsedMs = QDateTime::currentMSecsSinceEpoch();
                
                // Render title texture with transformations
                const QRect fitted = fitRect(m_outputSize, m_outputSize); // Title fills output
                const TimelineClip::TransformKeyframe transform =
                    evaluateClipRenderTransformAtPosition(
                        clip, static_cast<qreal>(timelineFrame), m_outputSize);
                const QPointF center(fitted.center().x() + transform.translationX,
                                     fitted.center().y() + transform.translationY);

                QMatrix4x4 projection;
                projection.ortho(0.0f, static_cast<float>(m_outputSize.width()),
                                 static_cast<float>(m_outputSize.height()), 0.0f,
                                 -1.0f, 1.0f);

                QMatrix4x4 model;
                model.translate(center.x(), center.y());
                model.rotate(transform.rotation, 0.0f, 0.0f, 1.0f);
                model.scale(fitted.width() * transform.scaleX, fitted.height() * transform.scaleY, 1.0f);

                QElapsedTimer compositeTimer;
                compositeTimer.start();
                
                m_shaderProgram->bind();
                m_shaderProgram->setUniformValue("u_mvp", projection * model);
                m_shaderProgram->setUniformValue("u_brightness", 0.0f); // No brightness adjustment for titles
                m_shaderProgram->setUniformValue("u_contrast", 1.0f);   // No contrast adjustment for titles
                m_shaderProgram->setUniformValue("u_saturation", 1.0f); // No saturation adjustment for titles
                m_shaderProgram->setUniformValue("u_opacity", 1.0f);
                // Neutral color grading for titles: avoid tint/brightening drift.
                m_shaderProgram->setUniformValue("u_shadows", QVector3D(0.0f, 0.0f, 0.0f));
                m_shaderProgram->setUniformValue("u_midtones", QVector3D(0.0f, 0.0f, 0.0f));
                m_shaderProgram->setUniformValue("u_highlights", QVector3D(0.0f, 0.0f, 0.0f));
                m_shaderProgram->setUniformValue("u_feather_radius", 0.0f);
                m_shaderProgram->setUniformValue("u_feather_gamma", 1.0f);
                m_shaderProgram->setUniformValue("u_texel_size", QVector2D(0.0f, 0.0f));
                m_shaderProgram->setUniformValue("u_texture", 0);
                m_shaderProgram->setUniformValue("u_texture_uv", 1);
                m_shaderProgram->setUniformValue("u_curve_lut", 2);
                m_shaderProgram->setUniformValue("u_curve_enabled", 1.0f);
                m_shaderProgram->setUniformValue("u_texture_mode", 0.0f); // RGB texture mode
                m_shaderProgram->setUniformValue("u_unpremultiply_input", 0.0f);
                TimelineClip::GradingKeyframe identityGrade;
                identityGrade.curvePointsR = defaultGradingCurvePoints();
                identityGrade.curvePointsG = defaultGradingCurvePoints();
                identityGrade.curvePointsB = defaultGradingCurvePoints();
                identityGrade.curvePointsLuma = defaultGradingCurvePoints();
                uploadCurveLutTexture(&m_curveLutTextureId, identityGrade);

                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, titleTextureEntry.textureId);
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, 0);
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
                m_quadBuffer.release();
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, 0);
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, 0);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, 0);
                
                // Clean up title texture
                glDeleteTextures(1, &titleTextureId);
                
                const qint64 compositeElapsed = compositeTimer.elapsed();
                if (compositeMs) {
                    *compositeMs += compositeElapsed;
                }
                accumulateClipStageStats(clipStageStats,
                                         clip,
                                         decodeElapsed,
                                         textureElapsed,
                                         compositeElapsed);
                continue;
            }

            // Regular video/image clip processing
            const QString path = clip.filePath;
            const int64_t localFrame =
                sourceFrameForClipAtTimelinePosition(clip, static_cast<qreal>(timelineFrame), request.renderSyncMarkers);
            QElapsedTimer decodeTimer;
            decodeTimer.start();
            const editor::FrameHandle frame =
                decodeRenderFrame(path, localFrame, decoders, asyncDecoder, asyncFrameCache);
            const qint64 decodeElapsed = decodeTimer.elapsed();
            if (decodeMs) {
                *decodeMs += decodeElapsed;
            }
            if (frame.isNull() || (!frame.hasCpuImage() && !frameUsesCudaZeroCopyCandidate(frame))) {
                recordRenderSkip(skippedClips,
                                 skippedReasonCounts,
                                 clip,
                                 frame.isNull() ? QStringLiteral("frame_null_or_decoder_init_failed")
                                                : QStringLiteral("no_cpu_image"),
                                 timelineFrame,
                                 localFrame);
                continue;
            }

            QElapsedTimer textureTimer;
            textureTimer.start();
            editor::GlTextureCacheEntry* textureEntry = textureForFrame(frame);
            const qint64 textureElapsed = textureTimer.elapsed();
            if (textureMs) {
                *textureMs += textureElapsed;
            }
            if (!textureEntry || textureEntry->textureId == 0) {
                recordRenderSkip(skippedClips, skippedReasonCounts, clip, QStringLiteral("texture_upload_failed"), timelineFrame, localFrame);
                continue;
            }

            const QRect fitted = fitRect(frame.size(), m_outputSize);
            const TimelineClip::TransformKeyframe transform =
                evaluateClipRenderTransformAtPosition(
                    clip, static_cast<qreal>(timelineFrame), m_outputSize);
            const QPointF center(fitted.center().x() + transform.translationX,
                                 fitted.center().y() + transform.translationY);

            QMatrix4x4 projection;
            projection.ortho(0.0f, static_cast<float>(m_outputSize.width()),
                             static_cast<float>(m_outputSize.height()), 0.0f,
                             -1.0f, 1.0f);

            QMatrix4x4 model;
            model.translate(center.x(), center.y());
            model.rotate(transform.rotation, 0.0f, 0.0f, 1.0f);
            model.scale(fitted.width() * transform.scaleX, fitted.height() * transform.scaleY, 1.0f);

            QElapsedTimer compositeTimer;
            compositeTimer.start();
            const bool hasCorrections =
                !effects.correctionPolygons.isEmpty() &&
                m_correctionMaskShaderProgram &&
                m_polygonBuffer.isCreated();
            if (hasCorrections) {
                glEnable(GL_STENCIL_TEST);
                glStencilMask(0xFF);
                glClear(GL_STENCIL_BUFFER_BIT);
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                glStencilFunc(GL_ALWAYS, 1, 0xFF);
                glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);

                m_correctionMaskShaderProgram->bind();
                m_correctionMaskShaderProgram->setUniformValue("u_mvp", projection * model);
                const int correctionPositionLoc =
                    m_correctionMaskShaderProgram->attributeLocation("a_position");
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
                    m_polygonBuffer.allocate(vertices.constData(),
                                             int(vertices.size() * sizeof(GLfloat)));
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
            } else {
                glDisable(GL_STENCIL_TEST);
            }

            m_shaderProgram->bind();
            m_shaderProgram->setUniformValue("u_mvp", projection * model);
            m_shaderProgram->setUniformValue("u_brightness", GLfloat(grade.brightness));
            m_shaderProgram->setUniformValue("u_contrast", GLfloat(grade.contrast));
            m_shaderProgram->setUniformValue("u_saturation", GLfloat(grade.saturation));
            m_shaderProgram->setUniformValue("u_opacity", GLfloat(grade.opacity));
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
            m_shaderProgram->setUniformValue("u_texture_mode", textureEntry->usesYuvTextures ? 1.0f : 0.0f);
            m_shaderProgram->setUniformValue("u_unpremultiply_input", 1.0f);
            uploadCurveLutTexture(&m_curveLutTextureId, grade);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, textureEntry->textureId);
            glActiveTexture(GL_TEXTURE1);
            if (textureEntry->usesYuvTextures && textureEntry->auxTextureId != 0) {
                glBindTexture(GL_TEXTURE_2D, textureEntry->auxTextureId);
            } else {
                glBindTexture(GL_TEXTURE_2D, 0);
            }
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
            m_quadBuffer.release();
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, 0);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, 0);
            if (hasCorrections) {
                glStencilMask(0xFF);
                glDisable(GL_STENCIL_TEST);
            }

            const qint64 compositeElapsed = compositeTimer.elapsed();
            if (compositeMs) {
                *compositeMs += compositeElapsed;
            }
            accumulateClipStageStats(clipStageStats,
                                     clip,
                                     decodeElapsed,
                                     textureElapsed,
                                     compositeElapsed);
        }

        QImage image(m_outputSize, QImage::Format_ARGB32_Premultiplied);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        QElapsedTimer readbackTimer;
        readbackTimer.start();
        glReadPixels(0, 0, m_outputSize.width(), m_outputSize.height(), GL_BGRA, GL_UNSIGNED_BYTE, image.bits());
        if (readbackMs) {
            *readbackMs += readbackTimer.elapsed();
        }
        m_fbo->release();
        trimTextureCache();
        return image.mirrored();
    }

    bool convertLastFrameToNv12(AVFrame* frame,
                                qint64* nv12ConvertMs,
                                qint64* readbackMs) {
        if (!frame || !m_context || !m_surface || !m_fbo || !m_nv12YFbo || !m_nv12UvFbo ||
            !m_nv12YShaderProgram || !m_nv12UvShaderProgram) {
            return false;
        }
        if (!m_context->makeCurrent(m_surface.get())) {
            return false;
        }

        const GLuint sourceTextureId = m_fbo->texture();
        if (!sourceTextureId) {
            return false;
        }

        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);

        QElapsedTimer nv12Timer;
        nv12Timer.start();

        m_nv12YFbo->bind();
        glViewport(0, 0, m_outputSize.width(), m_outputSize.height());
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        m_nv12YShaderProgram->bind();
        m_nv12YShaderProgram->setUniformValue("u_texture", 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sourceTextureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        m_quadBuffer.bind();
        int positionLoc = m_nv12YShaderProgram->attributeLocation("a_position");
        int texCoordLoc = m_nv12YShaderProgram->attributeLocation("a_texCoord");
        m_nv12YShaderProgram->enableAttributeArray(positionLoc);
        m_nv12YShaderProgram->enableAttributeArray(texCoordLoc);
        m_nv12YShaderProgram->setAttributeBuffer(positionLoc, GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
        m_nv12YShaderProgram->setAttributeBuffer(texCoordLoc, GL_FLOAT, 2 * sizeof(GLfloat), 2, 4 * sizeof(GLfloat));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_nv12YShaderProgram->disableAttributeArray(positionLoc);
        m_nv12YShaderProgram->disableAttributeArray(texCoordLoc);
        m_quadBuffer.release();
        m_nv12YShaderProgram->release();
        m_nv12YFbo->release();

        m_nv12UvFbo->bind();
        glViewport(0, 0, qMax(1, m_outputSize.width() / 2), qMax(1, m_outputSize.height() / 2));
        glClear(GL_COLOR_BUFFER_BIT);
        m_nv12UvShaderProgram->bind();
        m_nv12UvShaderProgram->setUniformValue("u_texture", 0);
        m_nv12UvShaderProgram->setUniformValue("u_texel_size",
                                               QVector2D(1.0f / qMax(1, m_outputSize.width()),
                                                         1.0f / qMax(1, m_outputSize.height())));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sourceTextureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        m_quadBuffer.bind();
        positionLoc = m_nv12UvShaderProgram->attributeLocation("a_position");
        texCoordLoc = m_nv12UvShaderProgram->attributeLocation("a_texCoord");
        m_nv12UvShaderProgram->enableAttributeArray(positionLoc);
        m_nv12UvShaderProgram->enableAttributeArray(texCoordLoc);
        m_nv12UvShaderProgram->setAttributeBuffer(positionLoc, GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
        m_nv12UvShaderProgram->setAttributeBuffer(texCoordLoc, GL_FLOAT, 2 * sizeof(GLfloat), 2, 4 * sizeof(GLfloat));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_nv12UvShaderProgram->disableAttributeArray(positionLoc);
        m_nv12UvShaderProgram->disableAttributeArray(texCoordLoc);
        m_quadBuffer.release();
        m_nv12UvShaderProgram->release();
        m_nv12UvFbo->release();
        if (nv12ConvertMs) {
            *nv12ConvertMs += nv12Timer.elapsed();
        }

        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        QElapsedTimer readbackTimer;
        readbackTimer.start();
        m_nv12YFbo->bind();
        glPixelStorei(GL_PACK_ROW_LENGTH, frame->linesize[0]);
        glReadPixels(0, 0, frame->width, frame->height, GL_RED, GL_UNSIGNED_BYTE, frame->data[0]);
        m_nv12YFbo->release();
        m_nv12UvFbo->bind();
        glPixelStorei(GL_PACK_ROW_LENGTH, frame->linesize[1] / 2);
        glReadPixels(0, 0, qMax(1, frame->width / 2), qMax(1, frame->height / 2), GL_RG, GL_UNSIGNED_BYTE, frame->data[1]);
        m_nv12UvFbo->release();
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        if (readbackMs) {
            *readbackMs += readbackTimer.elapsed();
        }
        return true;
    }

private:
    void releaseResources() {
        if (!m_context || !m_surface) {
            return;
        }
        if (m_context->makeCurrent(m_surface.get())) {
            for (auto it = m_textureCache.begin(); it != m_textureCache.end(); ++it) {
                editor::destroyGlTextureEntry(&it.value());
            }
            m_textureCache.clear();
            for (auto it = m_reusableTextureCache.begin(); it != m_reusableTextureCache.end(); ++it) {
                editor::destroyGlTextureEntry(&it.value());
            }
            m_reusableTextureCache.clear();
            m_fbo.reset();
            m_nv12YFbo.reset();
            m_nv12UvFbo.reset();
            if (m_curveLutTextureId != 0) {
                glDeleteTextures(1, &m_curveLutTextureId);
                m_curveLutTextureId = 0;
            }
            if (m_quadBuffer.isCreated()) {
                m_quadBuffer.destroy();
            }
            if (m_polygonBuffer.isCreated()) {
                m_polygonBuffer.destroy();
            }
            m_shaderProgram.reset();
            m_correctionMaskShaderProgram.reset();
            m_nv12YShaderProgram.reset();
            m_nv12UvShaderProgram.reset();
            m_context->doneCurrent();
        }
    }

    editor::GlTextureCacheEntry* textureForFrame(const editor::FrameHandle& frame) {
        if (editor::shouldUseReusableTextureCache(frame)) {
            const QString reusableKey = editor::reusableTextureCacheKey(frame);
            editor::GlTextureCacheEntry& reusableEntry = m_reusableTextureCache[reusableKey];
            if (!editor::uploadFrameToGlTextureEntry(frame, &reusableEntry)) {
                editor::destroyGlTextureEntry(&reusableEntry);
                return nullptr;
            }
            return &reusableEntry;
        }

        const QString key = editor::textureCacheKey(frame);
        const qint64 decodeTimestamp = frame.data() ? frame.data()->decodeTimestamp : 0;
        auto it = m_textureCache.find(key);
        if (it != m_textureCache.end() &&
            it->textureId != 0 &&
            it->decodeTimestamp == decodeTimestamp) {
            it->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
            return &it.value();
        }
        editor::GlTextureCacheEntry entry = it != m_textureCache.end()
                                                ? it.value()
                                                : editor::GlTextureCacheEntry{};
        editor::destroyGlTextureEntry(&entry);
        if (!editor::uploadFrameToGlTextureEntry(frame, &entry)) {
            editor::destroyGlTextureEntry(&entry);
            return nullptr;
        }
        entry.decodeTimestamp = decodeTimestamp;
        m_textureCache.insert(key, entry);
        return &m_textureCache[key];
    }

    void trimTextureCache() {
        static constexpr int kMaxTextureCacheEntries = 180;
        editor::trimGlTextureCache(&m_textureCache, kMaxTextureCacheEntries);

        static constexpr int kMaxReusableTextureCacheEntries = 32;
        editor::trimGlTextureCache(&m_reusableTextureCache, kMaxReusableTextureCacheEntries);
    }

    QSize m_outputSize;
    std::unique_ptr<QOffscreenSurface> m_surface;
    std::unique_ptr<QOpenGLContext> m_context;
    std::unique_ptr<QOpenGLFramebufferObject> m_fbo;
    std::unique_ptr<QOpenGLFramebufferObject> m_nv12YFbo;
    std::unique_ptr<QOpenGLFramebufferObject> m_nv12UvFbo;
    std::unique_ptr<QOpenGLShaderProgram> m_shaderProgram;
    std::unique_ptr<QOpenGLShaderProgram> m_correctionMaskShaderProgram;
    std::unique_ptr<QOpenGLShaderProgram> m_nv12YShaderProgram;
    std::unique_ptr<QOpenGLShaderProgram> m_nv12UvShaderProgram;
    QOpenGLBuffer m_quadBuffer;
    QOpenGLBuffer m_polygonBuffer;
    GLuint m_curveLutTextureId = 0;
    QHash<QString, editor::GlTextureCacheEntry> m_textureCache;
    QHash<QString, editor::GlTextureCacheEntry> m_reusableTextureCache;
};

// Public class implementation using PIMPL
OffscreenGpuRenderer::OffscreenGpuRenderer()
    : d(std::make_unique<OffscreenGpuRendererPrivate>()) {}

OffscreenGpuRenderer::~OffscreenGpuRenderer() = default;

bool OffscreenGpuRenderer::initialize(const QSize& outputSize, QString* errorMessage) {
    return d->initialize(outputSize, errorMessage);
}

QImage OffscreenGpuRenderer::renderFrame(const RenderRequest& request,
                                          int64_t timelineFrame,
                                          QHash<QString, editor::DecoderContext*>& decoders,
                                          editor::AsyncDecoder* asyncDecoder,
                                          QHash<RenderAsyncFrameKey, editor::FrameHandle>* asyncFrameCache,
                                          const QVector<TimelineClip>& orderedClips,
                                          QHash<QString, RenderClipStageStats>* clipStageStats,
                                          qint64* decodeMs,
                                          qint64* textureMs,
                                          qint64* compositeMs,
                                          qint64* readbackMs,
                                          QJsonArray* skippedClips,
                                          QJsonObject* skippedReasonCounts) {
    return d->renderFrame(request, timelineFrame, decoders, asyncDecoder, asyncFrameCache,
                          orderedClips, clipStageStats, decodeMs, textureMs, compositeMs,
                          readbackMs, skippedClips, skippedReasonCounts);
}

bool OffscreenGpuRenderer::convertLastFrameToNv12(AVFrame* frame,
                                                  qint64* nv12ConvertMs,
                                                  qint64* readbackMs) {
    return d->convertLastFrameToNv12(frame, nv12ConvertMs, readbackMs);
}

bool OffscreenGpuRenderer::convertLastFrameToYuv420p(AVFrame* frame,
                                                     qint64* convertMs,
                                                     qint64* readbackMs)
{
    Q_UNUSED(frame);
    Q_UNUSED(convertMs);
    Q_UNUSED(readbackMs);
    return false;
}

bool OffscreenGpuRenderer::copyLastFrameToBgra(AVFrame* frame,
                                               qint64* readbackMs)
{
    Q_UNUSED(frame);
    Q_UNUSED(readbackMs);
    return false;
}

QString OffscreenGpuRenderer::backendId() const
{
    return QStringLiteral("opengl");
}

QImage renderTimelineFrame(const RenderRequest& request,
                           int64_t timelineFrame,
                           QHash<QString, editor::DecoderContext*>& decoders,
                           editor::AsyncDecoder* asyncDecoder,
                           QHash<RenderAsyncFrameKey, editor::FrameHandle>* asyncFrameCache,
                           const QVector<TimelineClip>& orderedClips,
                           QHash<QString, RenderClipStageStats>* clipStageStats,
                           QJsonArray* skippedClips,
                           QJsonObject* skippedReasonCounts) {
    QImage canvas(request.outputSize, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(QColor(QStringLiteral("#000000")));

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    for (const TimelineClip& clip : orderedClips) {
        if (timelineFrame < clip.startFrame || timelineFrame >= clip.startFrame + clip.durationFrames) {
            continue;
        }

        EffectiveVisualEffects effects = evaluateEffectiveVisualEffectsAtFrame(
            clip, request.tracks, timelineFrame, request.renderSyncMarkers);
        if (!request.correctionsEnabled) {
            effects.correctionPolygons.clear();
        }
        if (effects.grading.opacity <= 0.0001) {
            recordRenderSkip(skippedClips, skippedReasonCounts, clip, QStringLiteral("zero_opacity"), timelineFrame);
            continue;
        }

        // Handle title clips specially - they don't have video files to decode
        if (clip.mediaType == ClipMediaType::Title) {
            if (clip.titleKeyframes.isEmpty()) {
                // No title keyframes to render
                continue;
            }
            
            const int64_t localFrame = timelineFrame - clip.startFrame;
            const EvaluatedTitle evaluatedTitle = evaluateTitleAtLocalFrame(clip, localFrame);
            const EvaluatedTitle title =
                composeTitleWithOpacity(evaluatedTitle, static_cast<qreal>(effects.grading.opacity));
            if (!title.valid || title.text.isEmpty() || title.opacity <= 0.001) {
                continue;
            }
            
            QElapsedTimer compositeTimer;
            compositeTimer.start();
            
            // Draw title overlay directly onto the canvas
            // Note: We use the full canvas rect for title rendering
            drawTitleOverlay(&painter, QRect(QPoint(0, 0), request.outputSize), title, request.outputSize);
            
            accumulateClipStageStats(clipStageStats, clip, 0, 0, compositeTimer.elapsed());
            continue;
        }

        // Regular video/image clip processing
        const QString path = clip.filePath;
        const int64_t localFrame =
            sourceFrameForClipAtTimelinePosition(clip, static_cast<qreal>(timelineFrame), request.renderSyncMarkers);
        QElapsedTimer decodeTimer;
        decodeTimer.start();
        const editor::FrameHandle frame =
            decodeRenderFrame(path, localFrame, decoders, asyncDecoder, asyncFrameCache);
        const qint64 decodeElapsed = decodeTimer.elapsed();
        if (frame.isNull()) {
            recordRenderSkip(skippedClips,
                             skippedReasonCounts,
                             clip,
                             QStringLiteral("frame_null_or_decoder_init_failed"),
                             timelineFrame,
                             localFrame);
            continue;
        }
        if (!frame.hasCpuImage()) {
            recordRenderSkip(skippedClips, skippedReasonCounts, clip, QStringLiteral("no_cpu_image"), timelineFrame, localFrame);
            continue;
        }

        QImage graded = applyEffectiveClipVisualEffectsToImage(frame.cpuImage(), effects);
        
        const QRect fitted = fitRect(graded.size(), request.outputSize);
        const TimelineClip::TransformKeyframe transform =
            evaluateClipRenderTransformAtPosition(
                clip, static_cast<qreal>(timelineFrame), request.outputSize);

        QElapsedTimer compositeTimer;
        compositeTimer.start();
        painter.save();
        painter.translate(fitted.center().x() + transform.translationX,
                          fitted.center().y() + transform.translationY);
        painter.rotate(transform.rotation);
        painter.scale(transform.scaleX, transform.scaleY);
        const QRectF drawRect(-fitted.width() / 2.0,
                              -fitted.height() / 2.0,
                              fitted.width(),
                              fitted.height());
        painter.drawImage(drawRect, graded);
        painter.restore();
        accumulateClipStageStats(clipStageStats, clip, decodeElapsed, 0, compositeTimer.elapsed());
    }

    return canvas;
}

bool encodeFrame(AVCodecContext* codecCtx,
                 AVStream* stream,
                 AVFormatContext* formatCtx,
                 AVFrame* frame,
                 QString* errorMessage) {
    AVFrame* submittedFrame = frame;
    AVFrame* hwFrame = nullptr;
    if (frame &&
        codecCtx &&
        codecCtx->pix_fmt == AV_PIX_FMT_CUDA &&
        codecCtx->hw_frames_ctx &&
        frame->format != AV_PIX_FMT_CUDA) {
        hwFrame = av_frame_alloc();
        if (!hwFrame) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to allocate CUDA encoder frame.");
            }
            return false;
        }
        hwFrame->format = AV_PIX_FMT_CUDA;
        hwFrame->width = frame->width;
        hwFrame->height = frame->height;
        hwFrame->pts = frame->pts;
        int ret = av_hwframe_get_buffer(codecCtx->hw_frames_ctx, hwFrame, 0);
        if (ret < 0) {
            av_frame_free(&hwFrame);
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to allocate CUDA hardware frame: %1")
                                    .arg(avErrToString(ret));
            }
            return false;
        }
        ret = av_hwframe_transfer_data(hwFrame, frame, 0);
        if (ret < 0) {
            av_frame_free(&hwFrame);
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to upload frame to CUDA encoder surface: %1")
                                    .arg(avErrToString(ret));
            }
            return false;
        }
        submittedFrame = hwFrame;
    }

    int ret = avcodec_send_frame(codecCtx, submittedFrame);
    av_frame_free(&hwFrame);
    if (ret < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to send frame to encoder: %1").arg(avErrToString(ret));
        }
        return false;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate output packet.");
        }
        return false;
    }

    while (true) {
        ret = avcodec_receive_packet(codecCtx, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            av_packet_free(&packet);
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to receive encoded packet: %1").arg(avErrToString(ret));
            }
            return false;
        }

        av_packet_rescale_ts(packet, codecCtx->time_base, stream->time_base);
        packet->stream_index = stream->index;
        ret = av_interleaved_write_frame(formatCtx, packet);
        av_packet_unref(packet);
        if (ret < 0) {
            av_packet_free(&packet);
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to write output packet: %1").arg(avErrToString(ret));
            }
            return false;
        }
    }

    av_packet_free(&packet);
    return true;
}

} // namespace render_detail
