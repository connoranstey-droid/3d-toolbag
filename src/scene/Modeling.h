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

QString importObj(Scene& s, const QString& path);
QString exportObj(Scene& s, const QString& path, bool selectedOnly);

} // namespace facet::modeling
