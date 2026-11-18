#pragma once
#include "scene/Scene.h"

#include <QString>

// High-level, undoable modeling commands. Each returns a short status message for the help line.
namespace facet::modeling {

QString createPrimitive(Scene& s, const QString& type);
QString deleteSelection(Scene& s);
QString duplicate(Scene& s);
QString extrude(Scene& s, bool keepFacesTogether);
QString mergeVertices(Scene& s, float threshold);
QString mergeToCenter(Scene& s);
QString collapse(Scene& s);
QString fillHole(Scene& s);
QString smooth(Scene& s, int levels);
QString triangulate(Scene& s);
QString reverseNormals(Scene& s);
QString mirror(Scene& s, int axis);
QString combine(Scene& s);
QString separate(Scene& s);
QString setCrease(Scene& s, float value);
// Shading: hard / soft edges (selected edges in edge/face/vertex mode, whole objects in object mode).
QString hardenEdges(Scene& s);
QString softenEdges(Scene& s);
QString setNormalAngle(Scene& s, float degrees);
// Weighted normals (0 clears): large faces dominate shared normals.
QString setWeightedNormals(Scene& s, float power);
// Sculpt mask helpers
QString invertMask(Scene& s);
QString clearMask(Scene& s);
QString toggleIsolate(Scene& s);
QString freezeTransforms(Scene& s);
QString centerPivot(Scene& s);
QString resetTransforms(Scene& s);
QString deleteHistory(Scene& s);
QString setSmoothDisplay(Scene& s, int mode);
QString changeSmoothLevel(Scene& s, int delta);
QString hideSelected(Scene& s);
QString showAll(Scene& s);

// Selection
QString selectAll(Scene& s);
QString invertSelection(Scene& s);
QString growSelection(Scene& s);
QString shrinkSelection(Scene& s);
QString selectEdgeLoop(Scene& s);
QString selectEdgeRing(Scene& s);
QString convertSelection(Scene& s, SelectMode to);
QString setComponentMode(Scene& s, SelectMode m);

// New modeling tools
QString bevel(Scene& s, float offset, int segments, bool round);
QString bridge(Scene& s, int divisions);

// UVs
enum class Projection { PlanarX, PlanarY, PlanarZ, PlanarBest, Cylindrical, Spherical, Automatic };
QString projectUVs(Scene& s, Projection p);
QString cutUVs(Scene& s);
QString sewUVs(Scene& s);
QString unfoldUVs(Scene& s);
QString layoutUVs(Scene& s);
QString normalizeUVs(Scene& s);
QString flipUVs(Scene& s, bool horizontal);
QString rotateUVs(Scene& s, float degrees);

// Retopology
QString toggleLive(Scene& s);
QString conform(Scene& s);

QString importFbx(Scene& s, const QString& path);
QString exportFbx(Scene& s, const QString& path, bool selectedOnly);
QString importFile(Scene& s, const QString& path); // by extension (.obj / .fbx)
QString importObj(Scene& s, const QString& path);
QString exportObj(Scene& s, const QString& path, bool selectedOnly);

} // namespace facet::modeling
