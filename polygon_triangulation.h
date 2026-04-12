#pragma once

#include <QPointF>
#include <QVector>
#include <QtMath>

namespace editor {

inline qreal polygonSignedArea(const QVector<QPointF>& points) {
    if (points.size() < 3) {
        return 0.0;
    }
    qreal area = 0.0;
    for (int i = 0; i < points.size(); ++i) {
        const QPointF& a = points[i];
        const QPointF& b = points[(i + 1) % points.size()];
        area += (a.x() * b.y()) - (b.x() * a.y());
    }
    return area * 0.5;
}

inline qreal orient2d(const QPointF& a, const QPointF& b, const QPointF& c) {
    return (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
}

inline bool pointInTriangle(const QPointF& p,
                            const QPointF& a,
                            const QPointF& b,
                            const QPointF& c,
                            const bool ccw) {
    const qreal o1 = orient2d(a, b, p);
    const qreal o2 = orient2d(b, c, p);
    const qreal o3 = orient2d(c, a, p);
    if (ccw) {
        return o1 >= 0.0 && o2 >= 0.0 && o3 >= 0.0;
    }
    return o1 <= 0.0 && o2 <= 0.0 && o3 <= 0.0;
}

inline QVector<QPointF> normalizedPolygonWithoutDuplicateClosure(const QVector<QPointF>& points) {
    QVector<QPointF> cleaned;
    cleaned.reserve(points.size());
    for (const QPointF& p : points) {
        if (cleaned.isEmpty() || cleaned.constLast() != p) {
            cleaned.push_back(p);
        }
    }
    if (cleaned.size() >= 2 && cleaned.constFirst() == cleaned.constLast()) {
        cleaned.removeLast();
    }
    return cleaned;
}

inline bool triangulatePolygon(const QVector<QPointF>& polygonPoints,
                               QVector<QPointF>* triangleVerticesOut) {
    if (!triangleVerticesOut) {
        return false;
    }
    triangleVerticesOut->clear();

    QVector<QPointF> points = normalizedPolygonWithoutDuplicateClosure(polygonPoints);
    if (points.size() < 3) {
        return false;
    }

    const qreal area = polygonSignedArea(points);
    if (qAbs(area) < 1e-8) {
        return false;
    }
    const bool ccw = area > 0.0;

    QVector<int> indices;
    indices.reserve(points.size());
    for (int i = 0; i < points.size(); ++i) {
        indices.push_back(i);
    }

    triangleVerticesOut->reserve((points.size() - 2) * 3);
    int guard = 0;
    const int maxGuard = points.size() * points.size();
    while (indices.size() > 3 && guard < maxGuard) {
        bool earFound = false;
        for (int i = 0; i < indices.size(); ++i) {
            const int prevIdx = indices[(i - 1 + indices.size()) % indices.size()];
            const int currIdx = indices[i];
            const int nextIdx = indices[(i + 1) % indices.size()];
            const QPointF& a = points[prevIdx];
            const QPointF& b = points[currIdx];
            const QPointF& c = points[nextIdx];

            const qreal corner = orient2d(a, b, c);
            if (ccw ? (corner <= 1e-8) : (corner >= -1e-8)) {
                continue;
            }

            bool containsOther = false;
            for (int j = 0; j < indices.size(); ++j) {
                const int testIdx = indices[j];
                if (testIdx == prevIdx || testIdx == currIdx || testIdx == nextIdx) {
                    continue;
                }
                if (pointInTriangle(points[testIdx], a, b, c, ccw)) {
                    containsOther = true;
                    break;
                }
            }
            if (containsOther) {
                continue;
            }

            triangleVerticesOut->push_back(a);
            triangleVerticesOut->push_back(b);
            triangleVerticesOut->push_back(c);
            indices.removeAt(i);
            earFound = true;
            break;
        }
        if (!earFound) {
            break;
        }
        ++guard;
    }

    if (indices.size() == 3) {
        triangleVerticesOut->push_back(points[indices[0]]);
        triangleVerticesOut->push_back(points[indices[1]]);
        triangleVerticesOut->push_back(points[indices[2]]);
    }

    return triangleVerticesOut->size() >= 3;
}

}  // namespace editor
