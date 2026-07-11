#include "title_mesh_extrusion.h"

#include <QHash>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <fontconfig/fontconfig.h>

#include "../motive3d/external/mapbox/earcut.hpp"

namespace {

using Ring = QVector<QVector2D>;
using Point = std::array<double, 2>;

struct Contour {
    Ring points;
    double area = 0.0;
};

struct OutlineState {
    QVector<Contour> contours;
    Ring current;
    QVector2D pen;
    QVector2D minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    QVector2D maximum{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    qreal flatness = 0.5;
};

double signedArea(const Ring& ring)
{
    double result = 0.0;
    for (int i = 0; i < ring.size(); ++i) {
        const QVector2D& a = ring.at(i);
        const QVector2D& b = ring.at((i + 1) % ring.size());
        result += static_cast<double>(a.x()) * b.y() - static_cast<double>(b.x()) * a.y();
    }
    return result * 0.5;
}

void addPoint(OutlineState* state, const QVector2D& point)
{
    state->current.push_back(point);
    state->minimum.setX(qMin(state->minimum.x(), point.x()));
    state->minimum.setY(qMin(state->minimum.y(), point.y()));
    state->maximum.setX(qMax(state->maximum.x(), point.x()));
    state->maximum.setY(qMax(state->maximum.y(), point.y()));
}

QVector2D pointFromFt(const FT_Vector* point, const QVector2D& pen)
{
    return {static_cast<float>(point->x) / 64.0f + pen.x(),
            static_cast<float>(point->y) / 64.0f + pen.y()};
}

void finishContour(OutlineState* state)
{
    if (state->current.size() > 2 &&
        (state->current.constFirst() - state->current.constLast()).lengthSquared() < 1e-6f) {
        state->current.removeLast();
    }
    if (state->current.size() >= 3) {
        const double area = signedArea(state->current);
        if (std::abs(area) > 1e-6) state->contours.push_back({std::move(state->current), area});
    }
    state->current.clear();
}

int moveTo(const FT_Vector* to, void* user)
{
    auto* state = static_cast<OutlineState*>(user);
    finishContour(state);
    addPoint(state, pointFromFt(to, state->pen));
    return 0;
}

int lineTo(const FT_Vector* to, void* user)
{
    auto* state = static_cast<OutlineState*>(user);
    addPoint(state, pointFromFt(to, state->pen));
    return 0;
}

int conicTo(const FT_Vector* control, const FT_Vector* to, void* user)
{
    auto* state = static_cast<OutlineState*>(user);
    if (state->current.isEmpty()) return 0;
    const QVector2D p0 = state->current.constLast();
    const QVector2D p1 = pointFromFt(control, state->pen);
    const QVector2D p2 = pointFromFt(to, state->pen);
    const int steps = qBound(8, qCeil(qMax((p2 - p0).length() * 0.12f,
                                           (p1 - (p0 + p2) * 0.5f).length()) / state->flatness), 96);
    for (int i = 1; i <= steps; ++i) {
        const qreal t = static_cast<qreal>(i) / steps;
        addPoint(state, p0 * ((1 - t) * (1 - t)) + p1 * (2 * (1 - t) * t) + p2 * (t * t));
    }
    return 0;
}

int cubicTo(const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to, void* user)
{
    auto* state = static_cast<OutlineState*>(user);
    if (state->current.isEmpty()) return 0;
    const QVector2D p0 = state->current.constLast();
    const QVector2D p1 = pointFromFt(c1, state->pen);
    const QVector2D p2 = pointFromFt(c2, state->pen);
    const QVector2D p3 = pointFromFt(to, state->pen);
    const int steps = qBound(10, qCeil(qMax((p3 - p0).length() * 0.16f,
                                            qMax((p1 - p0).length(), (p2 - p3).length())) / state->flatness), 128);
    for (int i = 1; i <= steps; ++i) {
        const qreal t = static_cast<qreal>(i) / steps;
        const qreal u = 1 - t;
        addPoint(state, p0 * (u * u * u) + p1 * (3 * u * u * t) +
                            p2 * (3 * u * t * t) + p3 * (t * t * t));
    }
    return 0;
}

bool containsPoint(const QVector2D& point, const Ring& ring)
{
    bool inside = false;
    for (int i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        const QVector2D& a = ring.at(i);
        const QVector2D& b = ring.at(j);
        if ((a.y() > point.y()) != (b.y() > point.y()) &&
            point.x() < (b.x() - a.x()) * (point.y() - a.y()) / (b.y() - a.y() + 1e-12f) + a.x()) {
            inside = !inside;
        }
    }
    return inside;
}

QVector2D centroid(const Ring& ring)
{
    QVector2D c;
    for (const QVector2D& point : ring) c += point;
    return ring.isEmpty() ? c : c / ring.size();
}

void addQuad(QVector<TitleMeshVertex>* vertices,
             const QVector3D& a, const QVector3D& b, const QVector3D& c, const QVector3D& d,
             const QVector3D& normal)
{
    const QVector3D cross = QVector3D::crossProduct(b - a, c - a);
    const bool forward = QVector3D::dotProduct(cross, normal) >= 0.0f;
    const QVector3D p0 = forward ? a : b;
    const QVector3D p1 = forward ? b : a;
    const QVector3D p2 = forward ? c : d;
    const QVector3D p3 = forward ? d : c;
    vertices->append({p0, normal, {0.5f, 0.5f}});
    vertices->append({p1, normal, {0.5f, 0.5f}});
    vertices->append({p2, normal, {0.5f, 0.5f}});
    vertices->append({p0, normal, {0.5f, 0.5f}});
    vertices->append({p2, normal, {0.5f, 0.5f}});
    vertices->append({p3, normal, {0.5f, 0.5f}});
}

QString resolveFont(const QString& family, bool bold)
{
    FcPattern* pattern = FcPatternCreate();
    if (!pattern) return {};
    const QByteArray name = (family.trimmed().isEmpty() ? QStringLiteral("DejaVu Sans") : family).toUtf8();
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(name.constData()));
    FcPatternAddInteger(pattern, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pattern, &result);
    FcPatternDestroy(pattern);
    if (!match) return {};
    FcChar8* file = nullptr;
    const QString path = FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch
        ? QString::fromUtf8(reinterpret_cast<const char*>(file)) : QString();
    FcPatternDestroy(match);
    return path;
}

} // namespace

QVector<TitleMeshVertex> buildExtrudedTitleMesh(const QString& text,
                                                const TitleMeshExtrusionOptions& options,
                                                QString* errorMessage)
{
    QVector<TitleMeshVertex> vertices;
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    const QString fontPath = resolveFont(options.fontFamily, options.bold);
    if (text.trimmed().isEmpty() || fontPath.isEmpty() || FT_Init_FreeType(&library) != 0 ||
        FT_New_Face(library, fontPath.toUtf8().constData(), 0, &face) != 0) {
        if (errorMessage) *errorMessage = QStringLiteral("title_mesh_font_load_failed");
        if (library) FT_Done_FreeType(library);
        return vertices;
    }
    FT_Set_Pixel_Sizes(face, 0, qMax(8, options.pixelHeight));
    OutlineState state;
    state.flatness = qBound<qreal>(0.20, 96.0 / qMax(8, options.pixelHeight), 0.60);
    FT_Outline_Funcs callbacks{moveTo, lineTo, conicTo, cubicTo, 0, 0};
    FT_UInt previous = 0;
    for (uint codepoint : text.toUcs4()) {
        if (codepoint == '\n') {
            state.pen.setX(0.0f);
            state.pen.setY(state.pen.y() - static_cast<float>(face->size->metrics.height) / 64.0f);
            previous = 0;
            continue;
        }
        const FT_UInt glyph = FT_Get_Char_Index(face, codepoint);
        if (FT_HAS_KERNING(face) && previous && glyph) {
            FT_Vector kerning{};
            if (FT_Get_Kerning(face, previous, glyph, FT_KERNING_DEFAULT, &kerning) == 0)
                state.pen.setX(state.pen.x() + kerning.x / 64.0f);
        }
        if (FT_Load_Glyph(face, glyph, FT_LOAD_NO_BITMAP) == 0 && face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
            FT_Outline_Decompose(&face->glyph->outline, &callbacks, &state);
            finishContour(&state);
        }
        state.pen.setX(state.pen.x() + face->glyph->advance.x / 64.0f);
        previous = glyph;
    }
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    if (state.contours.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("title_mesh_empty_outline");
        return vertices;
    }
    const qreal width = qMax<qreal>(1e-3, state.maximum.x() - state.minimum.x());
    const qreal height = qMax<qreal>(1e-3, state.maximum.y() - state.minimum.y());
    const qreal scale = 1.0 / height;
    for (Contour& contour : state.contours) {
        for (QVector2D& point : contour.points) {
            point = QVector2D(static_cast<float>((point.x() - state.minimum.x()) * scale - width * scale * 0.5),
                              static_cast<float>(0.5 - (point.y() - state.minimum.y()) * scale));
        }
        contour.area = signedArea(contour.points);
    }
    int seed = 0;
    for (int i = 1; i < state.contours.size(); ++i)
        if (std::abs(state.contours.at(i).area) > std::abs(state.contours.at(seed).area)) seed = i;
    const bool outerCcw = state.contours.at(seed).area > 0.0;
    QVector<int> outers, holes;
    for (int i = 0; i < state.contours.size(); ++i)
        (state.contours.at(i).area > 0.0) == outerCcw ? outers.push_back(i) : holes.push_back(i);
    const qreal halfDepth = qMax<qreal>(0.0, options.depth) * 0.5;
    const qreal bevelInset = qMin<qreal>(0.03, qMax<qreal>(0.0, options.depth * 0.22 * qBound<qreal>(0.0, options.bevelScale, 2.0)));
    const qreal bevelZ = qMin<qreal>(halfDepth * .85, qMax<qreal>(0.001, bevelInset * .75));
    const float frontZ = static_cast<float>(halfDepth);
    const float backZ = -frontZ;
    const float frontBevelZ = static_cast<float>(halfDepth - bevelZ);
    const float backBevelZ = static_cast<float>(-halfDepth + bevelZ);
    for (int outerIndex : outers) {
        std::vector<std::vector<Point>> polygon;
        QVector<QVector2D> flat;
        auto addRing = [&](Ring ring, bool ccw) {
            if ((signedArea(ring) > 0.0) != ccw) std::reverse(ring.begin(), ring.end());
            std::vector<Point> points; points.reserve(ring.size());
            for (const QVector2D& point : ring) { points.push_back({point.x(), point.y()}); flat.push_back(point); }
            polygon.push_back(std::move(points));
        };
        addRing(state.contours.at(outerIndex).points, true);
        for (int holeIndex : holes)
            if (containsPoint(centroid(state.contours.at(holeIndex).points), state.contours.at(outerIndex).points))
                addRing(state.contours.at(holeIndex).points, false);
        const std::vector<uint32_t> indices = mapbox::earcut<uint32_t>(polygon);
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            const QVector2D a = flat.at(indices[i]), b = flat.at(indices[i + 1]), c = flat.at(indices[i + 2]);
            vertices.append({{a.x(), a.y(), frontZ}, {0, 0, 1}, {0, 0}});
            vertices.append({{b.x(), b.y(), frontZ}, {0, 0, 1}, {1, 0}});
            vertices.append({{c.x(), c.y(), frontZ}, {0, 0, 1}, {0, 1}});
            vertices.append({{c.x(), c.y(), backZ}, {0, 0, -1}, {0, 0}});
            vertices.append({{b.x(), b.y(), backZ}, {0, 0, -1}, {1, 0}});
            vertices.append({{a.x(), a.y(), backZ}, {0, 0, -1}, {0, 1}});
        }
    }
    for (const Contour& contour : state.contours) {
        const bool ccw = contour.area > 0.0;
        Ring inset; inset.reserve(contour.points.size());
        for (int i = 0; i < contour.points.size(); ++i) {
            const QVector2D before = contour.points.at((i + contour.points.size() - 1) % contour.points.size());
            const QVector2D current = contour.points.at(i);
            const QVector2D after = contour.points.at((i + 1) % contour.points.size());
            QVector2D e0 = current - before, e1 = after - current;
            if (e0.lengthSquared() < 1e-8f || e1.lengthSquared() < 1e-8f) { inset.push_back(current); continue; }
            e0.normalize(); e1.normalize();
            QVector2D n0(e0.y(), -e0.x()), n1(e1.y(), -e1.x());
            if (!ccw) { n0 = -n0; n1 = -n1; }
            QVector2D normal = n0 + n1;
            if (normal.lengthSquared() < 1e-8f) normal = n1;
            normal.normalize();
            const qreal miter = 1.0 / qBound<qreal>(.35, QVector2D::dotProduct(normal, n1), 1.0);
            inset.push_back(current + normal * qMin(bevelInset * miter, qMin(e0.length(), e1.length()) * .45f));
        }
        for (int i = 0; i < contour.points.size(); ++i) {
            const QVector2D a = contour.points.at(i), b = contour.points.at((i + 1) % contour.points.size());
            const QVector2D ai = inset.at(i), bi = inset.at((i + 1) % inset.size());
            QVector3D normal(b.y() - a.y(), -(b.x() - a.x()), 0); normal.normalize(); if (!ccw) normal = -normal;
            const QVector3D frontBevel(normal.x(), normal.y(), .70f);
            const QVector3D backBevel(normal.x(), normal.y(), -.70f);
            addQuad(&vertices, {a.x(),a.y(),frontZ}, {b.x(),b.y(),frontZ}, {bi.x(),bi.y(),frontBevelZ}, {ai.x(),ai.y(),frontBevelZ}, frontBevel.normalized());
            addQuad(&vertices, {ai.x(),ai.y(),backBevelZ}, {bi.x(),bi.y(),backBevelZ}, {bi.x(),bi.y(),frontBevelZ}, {ai.x(),ai.y(),frontBevelZ}, normal);
            addQuad(&vertices, {a.x(),a.y(),backZ}, {b.x(),b.y(),backZ}, {bi.x(),bi.y(),backBevelZ}, {ai.x(),ai.y(),backBevelZ}, backBevel.normalized());
        }
    }
    return vertices;
}
