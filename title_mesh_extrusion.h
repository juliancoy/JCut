#pragma once

#include <QVector>
#include <QVector2D>
#include <QVector3D>
#include <QString>

// A JCut-owned port of motive3d's FreeType-outline text extrusion. Coordinates
// are normalized to a one-unit glyph height and centered on the origin.
struct TitleMeshVertex {
    QVector3D position;
    QVector3D normal;
    QVector2D uv;
};

struct TitleMeshExtrusionOptions {
    QString fontFamily;
    bool bold = true;
    int pixelHeight = 96;
    qreal depth = 0.20;
    qreal bevelScale = 0.70;
};

QVector<TitleMeshVertex> buildExtrudedTitleMesh(const QString& text,
                                                const TitleMeshExtrusionOptions& options,
                                                QString* errorMessage = nullptr);
