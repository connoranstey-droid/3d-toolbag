#pragma once
#include "math/Math.h"
#include "scene/Scene.h"

#include <functional>
#include <vector>

namespace facet {

class Renderer;

enum class Handle { None, X, Y, Z, XY, YZ, XZ, Center, View };

struct GizmoFrame {
    Vec3 origin;
    Vec3 axis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    float size = 1.0f; // world length of an axis handle
};

struct GizmoView {
    Mat4 viewProj;
    Vec3 forward, right, up, eye;
    bool ortho = false;
    std::function<Vec2(const Vec3&)> toScreen; // world -> logical pixels (top-left origin)
};

struct HandleGeom {
    Handle handle;
    std::vector<Vec3> lines; // GL_LINES pairs
    std::vector<Vec3> tris;  // filled triangles
    Vec4 color;
};

std::vector<HandleGeom> buildGizmo(Tool tool, const GizmoFrame& f, const GizmoView& v);
Handle hitGizmo(Tool tool, const GizmoFrame& f, const GizmoView& v, Vec2 mouse);
void drawGizmo(Renderer& r, Tool tool, const GizmoFrame& f, const GizmoView& v, Handle hover, Handle active);

// Ray/line helpers used by manipulator drags.
// Parameter t along line (p + d t) closest to the ray.
float closestOnLine(const Vec3& p, const Vec3& d, const Ray& ray);
bool rayPlane(const Ray& ray, const Vec3& p, const Vec3& n, Vec3& hit);

} // namespace facet
