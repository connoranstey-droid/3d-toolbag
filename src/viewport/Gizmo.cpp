#include "viewport/Gizmo.h"
#include "render/Renderer.h"

namespace facet {

namespace {

const Vec4 kAxisColor[3] = {{0.95f, 0.18f, 0.18f, 1}, {0.25f, 0.9f, 0.25f, 1}, {0.25f, 0.4f, 1.0f, 1}};
const Vec4 kActive{1.0f, 0.95f, 0.1f, 1};
const Vec4 kCenter{0.35f, 0.85f, 1.0f, 1};

void addCone(std::vector<Vec3>& tris, const Vec3& base, const Vec3& dir, float len, float radius)
{
    Vec3 a = std::abs(dir.y) < 0.9f ? normalize(cross(dir, {0, 1, 0})) : normalize(cross(dir, {1, 0, 0}));
    Vec3 b = cross(dir, a);
    Vec3 tip = base + dir * len;
    const int n = 14;
    for (int i = 0; i < n; ++i) {
        float t0 = 2 * kPi * i / n, t1 = 2 * kPi * (i + 1) / n;
        Vec3 p0 = base + (a * std::cos(t0) + b * std::sin(t0)) * radius;
        Vec3 p1 = base + (a * std::cos(t1) + b * std::sin(t1)) * radius;
        tris.insert(tris.end(), {p0, p1, tip, p1, p0, base});
    }
}

void addCube(std::vector<Vec3>& tris, const Vec3& c, const Vec3 ax[3], float h)
{
    for (int i = 0; i < 3; ++i)
        for (int s = -1; s <= 1; s += 2) {
            Vec3 n = ax[i] * (h * s), u = ax[(i + 1) % 3] * h, v = ax[(i + 2) % 3] * h;
            Vec3 p0 = c + n - u - v, p1 = c + n + u - v, p2 = c + n + u + v, p3 = c + n - u + v;
            tris.insert(tris.end(), {p0, p1, p2, p0, p2, p3});
        }
}

void addRing(std::vector<Vec3>& lines, const Vec3& c, const Vec3& u, const Vec3& v, float r, const Vec3* cullDir)
{
    const int n = 72;
    for (int i = 0; i < n; ++i) {
        float t0 = 2 * kPi * i / n, t1 = 2 * kPi * (i + 1) / n;
        Vec3 p0 = c + (u * std::cos(t0) + v * std::sin(t0)) * r;
        Vec3 p1 = c + (u * std::cos(t1) + v * std::sin(t1)) * r;
        if (cullDir && dot((p0 + p1) * 0.5f - c, *cullDir) > r * 0.05f) continue; // hide back half
        lines.insert(lines.end(), {p0, p1});
    }
}

float distToSegment(Vec2 p, Vec2 a, Vec2 b)
{
    Vec2 ab = b - a;
    float l2 = dot(ab, ab);
    float t = l2 > 0 ? std::clamp(dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
    return (p - (a + ab * t)).length();
}

bool inTriangle(Vec2 p, Vec2 a, Vec2 b, Vec2 c)
{
    auto s = [](Vec2 p1, Vec2 p2, Vec2 p3) { return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y); };
    float d1 = s(p, a, b), d2 = s(p, b, c), d3 = s(p, c, a);
    bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

} // namespace

std::vector<HandleGeom> buildGizmo(Tool tool, const GizmoFrame& f, const GizmoView& v)
{
    std::vector<HandleGeom> out;
    const float s = f.size;
    const Vec3 o = f.origin;
    auto axisVisible = [&](int i) {
        Vec2 a = v.toScreen(o), b = v.toScreen(o + f.axis[i] * s);
        return (b - a).length() > 8.0f;
    };
    static const Handle axH[3] = {Handle::X, Handle::Y, Handle::Z};

    if (tool == Tool::Move) {
        // Plane handles first so axis lines draw on top.
        static const Handle plH[3] = {Handle::YZ, Handle::XZ, Handle::XY};
        for (int i = 0; i < 3; ++i) {
            int a = (i + 1) % 3, b = (i + 2) % 3;
            if (std::abs(dot(f.axis[i], v.forward)) < 0.15f) continue; // plane seen edge-on
            HandleGeom g{plH[i], {}, {}, kAxisColor[i]};
            g.color.w = 0.55f;
            Vec3 p0 = o + (f.axis[a] + f.axis[b]) * (0.22f * s), da = f.axis[a] * (0.16f * s), db = f.axis[b] * (0.16f * s);
            g.tris = {p0, p0 + da, p0 + da + db, p0, p0 + da + db, p0 + db};
            g.lines = {p0, p0 + da, p0 + da, p0 + da + db, p0 + da + db, p0 + db, p0 + db, p0};
            out.push_back(std::move(g));
        }
        for (int i = 0; i < 3; ++i) {
            if (!axisVisible(i)) continue;
            HandleGeom g{axH[i], {o, o + f.axis[i] * s}, {}, kAxisColor[i]};
            addCone(g.tris, o + f.axis[i] * s, f.axis[i], s * 0.22f, s * 0.06f);
            out.push_back(std::move(g));
        }
        HandleGeom c{Handle::Center, {}, {}, kCenter};
        float h = s * 0.07f;
        Vec3 r = v.right * h, u = v.up * h;
        c.lines = {o - r - u, o + r - u, o + r - u, o + r + u, o + r + u, o - r + u, o - r + u, o - r - u};
        out.push_back(std::move(c));
    } else if (tool == Tool::Rotate) {
        Vec3 toCam = v.ortho ? -v.forward : normalize(v.eye - o);
        Vec3 cull = -toCam;
        for (int i = 0; i < 3; ++i) {
            HandleGeom g{axH[i], {}, {}, kAxisColor[i]};
            addRing(g.lines, o, f.axis[(i + 1) % 3], f.axis[(i + 2) % 3], s, &cull);
            out.push_back(std::move(g));
        }
        HandleGeom vr{Handle::View, {}, {}, kCenter};
        addRing(vr.lines, o, v.right, v.up, s * 1.18f, nullptr);
        out.push_back(std::move(vr));
        HandleGeom ball{Handle::None, {}, {}, {0.6f, 0.6f, 0.6f, 0.5f}};
        addRing(ball.lines, o, v.right, v.up, s, nullptr);
        out.push_back(std::move(ball));
    } else if (tool == Tool::Scale) {
        for (int i = 0; i < 3; ++i) {
            if (!axisVisible(i)) continue;
            HandleGeom g{axH[i], {o, o + f.axis[i] * s}, {}, kAxisColor[i]};
            addCube(g.tris, o + f.axis[i] * s, f.axis, s * 0.055f);
            out.push_back(std::move(g));
        }
        HandleGeom c{Handle::Center, {}, {}, kCenter};
        addCube(c.tris, o, f.axis, s * 0.075f);
        out.push_back(std::move(c));
    }
    return out;
}

Handle hitGizmo(Tool tool, const GizmoFrame& f, const GizmoView& v, Vec2 mouse)
{
    auto geoms = buildGizmo(tool, f, v);
    Handle best = Handle::None;
    float bestD = 9.0f;
    // Centre handle wins inside its footprint.
    for (const auto& g : geoms) {
        if (g.handle == Handle::None) continue;
        for (size_t i = 0; i + 2 < g.tris.size(); i += 3)
            if (inTriangle(mouse, v.toScreen(g.tris[i]), v.toScreen(g.tris[i + 1]), v.toScreen(g.tris[i + 2]))) {
                if (g.handle == Handle::Center) return Handle::Center;
                if (bestD > 0.5f) { best = g.handle; bestD = 0.5f; }
            }
        for (size_t i = 0; i + 1 < g.lines.size(); i += 2) {
            float d = distToSegment(mouse, v.toScreen(g.lines[i]), v.toScreen(g.lines[i + 1]));
            if (g.handle == Handle::Center && d < 9.0f) return Handle::Center;
            if (d < bestD) { bestD = d; best = g.handle; }
        }
    }
    if (tool == Tool::Move && best == Handle::None && (v.toScreen(f.origin) - mouse).length() < 10.0f) return Handle::Center;
    return best;
}

void drawGizmo(Renderer& r, Tool tool, const GizmoFrame& f, const GizmoView& v, Handle hover, Handle active)
{
    auto* gl = r.gl();
    gl->glEnable(GL_BLEND);
    gl->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl->glDisable(GL_DEPTH_TEST);
    for (const auto& g : buildGizmo(tool, f, v)) {
        Vec4 c = g.color;
        if (g.handle != Handle::None && (g.handle == active || (active == Handle::None && g.handle == hover))) c = Vec4{kActive.x, kActive.y, kActive.z, c.w};
        if (!g.tris.empty()) r.drawLines(g.tris, c, v.viewProj, false, GL_TRIANGLES);
        if (!g.lines.empty()) r.drawLines(g.lines, Vec4{c.x, c.y, c.z, 1.0f}, v.viewProj, false, GL_LINES);
    }
    gl->glEnable(GL_DEPTH_TEST);
    gl->glDisable(GL_BLEND);
}

float closestOnLine(const Vec3& p, const Vec3& d, const Ray& ray)
{
    Vec3 w = p - ray.origin;
    float a = dot(d, d), b = dot(d, ray.dir), c = dot(ray.dir, ray.dir);
    float dd = dot(d, w), e = dot(ray.dir, w);
    float den = a * c - b * b;
    if (std::abs(den) < 1e-9f) return 0.0f;
    return (b * e - c * dd) / den;
}

bool rayPlane(const Ray& ray, const Vec3& p, const Vec3& n, Vec3& hit)
{
    float den = dot(ray.dir, n);
    if (std::abs(den) < 1e-8f) return false;
    float t = dot(p - ray.origin, n) / den;
    hit = ray.origin + ray.dir * t;
    return true;
}

} // namespace facet
