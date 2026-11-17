#include "mesh/Primitives.h"

#include <cmath>
#include <unordered_map>

namespace facet::prim {

void weld(Mesh& m, float eps)
{
    const float inv = 1.0f / std::max(eps, 1e-12f);
    std::unordered_multimap<uint64_t, uint32_t> cells;
    std::vector<uint32_t> remap(m.numVerts());
    std::vector<Vec3> out;
    auto keyOf = [&](const Vec3& p, int dx, int dy, int dz) {
        int64_t x = int64_t(std::floor(p.x * inv)) + dx, y = int64_t(std::floor(p.y * inv)) + dy, z = int64_t(std::floor(p.z * inv)) + dz;
        return uint64_t((x * 73856093) ^ (y * 19349663) ^ (z * 83492791));
    };
    for (uint32_t v = 0; v < m.numVerts(); ++v) {
        const Vec3& p = m.positions[v];
        uint32_t found = kInvalid;
        for (int dx = -1; dx <= 1 && found == kInvalid; ++dx)
            for (int dy = -1; dy <= 1 && found == kInvalid; ++dy)
                for (int dz = -1; dz <= 1 && found == kInvalid; ++dz) {
                    auto range = cells.equal_range(keyOf(p, dx, dy, dz));
                    for (auto it = range.first; it != range.second; ++it)
                        if (length(out[it->second] - p) <= eps) { found = it->second; break; }
                }
        if (found == kInvalid) {
            found = uint32_t(out.size());
            out.push_back(p);
            cells.emplace(keyOf(p, 0, 0, 0), found);
        }
        remap[v] = found;
    }
    m.positions = std::move(out);
    for (uint32_t& v : m.faceVerts) v = remap[v];
}

// Adds a grid patch spanning origin + u*[0,1] + v*[0,1]; faces wind CCW around u x v.
static void addGrid(Mesh& m, Vec3 origin, Vec3 u, Vec3 v, int su, int sv)
{
    const uint32_t base = m.numVerts();
    for (int j = 0; j <= sv; ++j)
        for (int i = 0; i <= su; ++i) m.addVertex(origin + u * (float(i) / su) + v * (float(j) / sv));
    auto idx = [&](int i, int j) { return base + uint32_t(j * (su + 1) + i); };
    for (int j = 0; j < sv; ++j)
        for (int i = 0; i < su; ++i) m.addFace({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1), idx(i, j + 1)});
}

Mesh cube(float w, float h, float d, int sx, int sy, int sz)
{
    sx = std::max(1, sx); sy = std::max(1, sy); sz = std::max(1, sz);
    Mesh m;
    const float x = w / 2, y = h / 2, z = d / 2;
    addGrid(m, {-x, -y, z}, {w, 0, 0}, {0, h, 0}, sx, sy);   // +Z
    addGrid(m, {x, -y, -z}, {-w, 0, 0}, {0, h, 0}, sx, sy);  // -Z
    addGrid(m, {x, -y, z}, {0, 0, -d}, {0, h, 0}, sz, sy);   // +X
    addGrid(m, {-x, -y, -z}, {0, 0, d}, {0, h, 0}, sz, sy);  // -X
    addGrid(m, {-x, y, z}, {w, 0, 0}, {0, 0, -d}, sx, sz);   // +Y
    addGrid(m, {-x, -y, -z}, {w, 0, 0}, {0, 0, d}, sx, sz);  // -Y
    weld(m, 1e-5f * std::max({w, h, d}));
    m.buildTopology();
    return m;
}

Mesh plane(float w, float h, int sx, int sy)
{
    sx = std::max(1, sx); sy = std::max(1, sy);
    Mesh m;
    addGrid(m, {-w / 2, 0, h / 2}, {w, 0, 0}, {0, 0, -h}, sx, sy);
    m.buildTopology();
    return m;
}

Mesh sphere(float r, int sa, int sh)
{
    sa = std::max(3, sa); sh = std::max(2, sh);
    Mesh m;
    const uint32_t top = m.addVertex({0, r, 0});
    for (int j = 1; j < sh; ++j) {
        float phi = kPi * float(j) / sh;
        for (int i = 0; i < sa; ++i) {
            float th = 2 * kPi * float(i) / sa;
            m.addVertex({r * std::sin(phi) * std::cos(th), r * std::cos(phi), -r * std::sin(phi) * std::sin(th)});
        }
    }
    const uint32_t bot = m.addVertex({0, -r, 0});
    auto ring = [&](int j, int i) { return 1 + uint32_t((j - 1) * sa + ((i % sa + sa) % sa)); };
    for (int i = 0; i < sa; ++i) m.addFace({top, ring(1, i), ring(1, i + 1)});
    for (int j = 1; j < sh - 1; ++j)
        for (int i = 0; i < sa; ++i) m.addFace({ring(j, i), ring(j + 1, i), ring(j + 1, i + 1), ring(j, i + 1)});
    for (int i = 0; i < sa; ++i) m.addFace({ring(sh - 1, i), bot, ring(sh - 1, i + 1)});
    m.buildTopology();
    return m;
}

Mesh cylinder(float r, float height, int sa, int sh, bool caps)
{
    sa = std::max(3, sa); sh = std::max(1, sh);
    Mesh m;
    for (int j = 0; j <= sh; ++j) {
        float y = -height / 2 + height * float(j) / sh;
        for (int i = 0; i < sa; ++i) {
            float th = 2 * kPi * float(i) / sa;
            m.addVertex({r * std::cos(th), y, -r * std::sin(th)});
        }
    }
    auto idx = [&](int j, int i) { return uint32_t(j * sa + (i % sa)); };
    for (int j = 0; j < sh; ++j)
        for (int i = 0; i < sa; ++i) m.addFace({idx(j, i), idx(j, i + 1), idx(j + 1, i + 1), idx(j + 1, i)});
    if (caps) {
        std::vector<uint32_t> topCap, botCap;
        for (int i = 0; i < sa; ++i) topCap.push_back(idx(sh, i));
        for (int i = sa - 1; i >= 0; --i) botCap.push_back(idx(0, i));
        m.addFace(topCap);
        m.addFace(botCap);
    }
    m.buildTopology();
    return m;
}

Mesh cone(float r, float height, int sa, int sh)
{
    sa = std::max(3, sa); sh = std::max(1, sh);
    Mesh m;
    for (int j = 0; j < sh; ++j) {
        float t = float(j) / sh;
        float y = -height / 2 + height * t, rr = r * (1 - t);
        for (int i = 0; i < sa; ++i) {
            float th = 2 * kPi * float(i) / sa;
            m.addVertex({rr * std::cos(th), y, -rr * std::sin(th)});
        }
    }
    const uint32_t apex = m.addVertex({0, height / 2, 0});
    auto idx = [&](int j, int i) { return uint32_t(j * sa + (i % sa)); };
    for (int j = 0; j < sh - 1; ++j)
        for (int i = 0; i < sa; ++i) m.addFace({idx(j, i), idx(j, i + 1), idx(j + 1, i + 1), idx(j + 1, i)});
    for (int i = 0; i < sa; ++i) m.addFace({idx(sh - 1, i), idx(sh - 1, i + 1), apex});
    std::vector<uint32_t> cap;
    for (int i = sa - 1; i >= 0; --i) cap.push_back(idx(0, i));
    m.addFace(cap);
    m.buildTopology();
    return m;
}

Mesh torus(float R, float r, int sa, int sh)
{
    sa = std::max(3, sa); sh = std::max(3, sh);
    Mesh m;
    for (int i = 0; i < sa; ++i) {
        float th = 2 * kPi * float(i) / sa;
        Vec3 dir{std::cos(th), 0, -std::sin(th)};
        for (int j = 0; j < sh; ++j) {
            float ph = 2 * kPi * float(j) / sh;
            m.addVertex(dir * (R + r * std::cos(ph)) + Vec3{0, r * std::sin(ph), 0});
        }
    }
    auto idx = [&](int i, int j) { return uint32_t((i % sa) * sh + (j % sh)); };
    for (int i = 0; i < sa; ++i)
        for (int j = 0; j < sh; ++j) m.addFace({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1), idx(i, j + 1)});
    m.buildTopology();
    return m;
}

} // namespace facet::prim
