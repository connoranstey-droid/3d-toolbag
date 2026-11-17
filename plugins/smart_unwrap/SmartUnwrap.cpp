// Smart Unwrap: curvature-aware shell segmentation + LSCM + distortion-driven splitting +
// uniform world-space texel density + skyline packing with pixel padding.
#include "SmartUnwrap.h"

#include "mesh/UVOps.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <numeric>
#include <queue>
#include <unordered_map>

namespace facet::smart {

namespace {

struct Chart {
    size_t target;
    std::vector<uint32_t> faces;
};

struct MeshInfo {
    std::vector<Vec3> normal;   // world-space unit face normals
    std::vector<float> area;    // world-space face areas
    std::vector<uint8_t> inSet; // faces being unwrapped
    std::vector<uint8_t> barrier; // per edge: may not be crossed
};

MeshInfo analyse(const Target& t, const Options& opt)
{
    const Mesh& m = *t.mesh;
    MeshInfo mi;
    mi.normal.resize(m.numFaces());
    mi.area.resize(m.numFaces());
    for (uint32_t f = 0; f < m.numFaces(); ++f) {
        Vec3 n;
        auto fv = m.faceVertices(f);
        for (size_t i = 0; i < fv.size(); ++i) {
            Vec3 p = t.world.transformPoint(m.positions[fv[i]]), q = t.world.transformPoint(m.positions[fv[(i + 1) % fv.size()]]);
            n += cross(p, q);
        }
        // Area as the sum of fan triangles (matches how uv area is measured, also for non-planar n-gons).
        float a = 0;
        Vec3 p0 = t.world.transformPoint(m.positions[fv[0]]);
        for (size_t i = 1; i + 1 < fv.size(); ++i)
            a += 0.5f * length(cross(t.world.transformPoint(m.positions[fv[i]]) - p0, t.world.transformPoint(m.positions[fv[i + 1]]) - p0));
        mi.area[f] = a;
        mi.normal[f] = normalize(n);
    }
    mi.inSet.assign(m.numFaces(), t.faces.empty() ? 1 : 0);
    for (uint32_t f : t.faces)
        if (f < m.numFaces()) mi.inSet[f] = 1;
    mi.barrier.assign(m.numEdges(), 0);
    for (uint32_t e = 0; e < m.numEdges(); ++e) {
        if (m.edges[e].faceCount != 2) mi.barrier[e] = 1;
        else if (opt.hardEdgeSeams && m.isHard(e)) mi.barrier[e] = 1;
        else if (opt.keepSeams && m.hasUVs() && m.isUVBorderEdge(e)) mi.barrier[e] = 1;
    }
    return mi;
}

// Best-first region growing: a face joins the chart while its normal stays within angleLimit of the
// chart's running average normal.
std::vector<std::vector<uint32_t>> growCharts(const Mesh& m, const MeshInfo& mi, float angleLimit)
{
    const float cosLimit = std::cos(radians(angleLimit));
    std::vector<int> chartOf(m.numFaces(), -1);
    std::vector<uint32_t> order;
    for (uint32_t f = 0; f < m.numFaces(); ++f)
        if (mi.inSet[f]) order.push_back(f);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return mi.area[a] > mi.area[b]; });
    std::vector<std::vector<uint32_t>> charts;
    for (uint32_t seed : order) {
        if (chartOf[seed] >= 0) continue;
        const int id = int(charts.size());
        charts.emplace_back();
        Vec3 sum = mi.normal[seed] * mi.area[seed];
        using Item = std::pair<float, uint32_t>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
        pq.push({0.0f, seed});
        while (!pq.empty()) {
            auto [dev, f] = pq.top();
            pq.pop();
            if (chartOf[f] >= 0) continue;
            Vec3 avg = normalize(sum);
            if (f != seed && dot(mi.normal[f], avg) < cosLimit) continue;
            chartOf[f] = id;
            charts.back().push_back(f);
            sum += mi.normal[f] * mi.area[f];
            avg = normalize(sum);
            for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
                uint32_t t = m.heTwin[h];
                if (t == kInvalid || mi.barrier[m.heEdge[h]]) continue;
                uint32_t g = m.heFace[t];
                if (!mi.inSet[g] || chartOf[g] >= 0) continue;
                float d = 1.0f - dot(mi.normal[g], avg);
                if (dot(mi.normal[g], avg) >= cosLimit) pq.push({d, g});
            }
        }
    }
    return charts;
}

// Merges tiny charts into the neighbour they share the longest (non-barrier) boundary with.
int mergeTiny(const Mesh& m, const MeshInfo& mi, std::vector<std::vector<uint32_t>>& charts, float minFraction)
{
    float total = 0;
    for (auto& c : charts)
        for (uint32_t f : c) total += mi.area[f];
    std::vector<int> chartOf(m.numFaces(), -1);
    for (size_t i = 0; i < charts.size(); ++i)
        for (uint32_t f : charts[i]) chartOf[f] = int(i);
    int merges = 0;
    for (size_t i = 0; i < charts.size(); ++i) {
        if (charts[i].empty()) continue;
        float a = 0;
        for (uint32_t f : charts[i]) a += mi.area[f];
        if (a >= total * minFraction && charts[i].size() > 2) continue;
        std::unordered_map<int, float> shared;
        for (uint32_t f : charts[i])
            for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
                uint32_t t = m.heTwin[h];
                if (t == kInvalid || mi.barrier[m.heEdge[h]]) continue;
                int o = chartOf[m.heFace[t]];
                if (o >= 0 && o != int(i)) shared[o] += length(m.positions[m.heFrom(h)] - m.positions[m.heTo(h)]);
            }
        int best = -1;
        float bestLen = 0;
        for (auto [o, len] : shared)
            if (len > bestLen) { bestLen = len; best = o; }
        if (best < 0) continue;
        for (uint32_t f : charts[i]) { chartOf[f] = best; charts[size_t(best)].push_back(f); }
        charts[i].clear();
        ++merges;
    }
    charts.erase(std::remove_if(charts.begin(), charts.end(), [](auto& c) { return c.empty(); }), charts.end());
    return merges;
}

// Connected components (through non-barrier edges) of a face subset.
std::vector<std::vector<uint32_t>> components(const Mesh& m, const MeshInfo& mi, const std::vector<uint32_t>& faces)
{
    std::unordered_map<uint32_t, int> local;
    for (size_t i = 0; i < faces.size(); ++i) local[faces[i]] = int(i);
    std::vector<int> comp(faces.size(), -1);
    std::vector<std::vector<uint32_t>> out;
    for (size_t i = 0; i < faces.size(); ++i) {
        if (comp[i] >= 0) continue;
        out.emplace_back();
        std::vector<uint32_t> stack{uint32_t(i)};
        comp[i] = int(out.size() - 1);
        while (!stack.empty()) {
            uint32_t k = stack.back();
            stack.pop_back();
            uint32_t f = faces[k];
            out.back().push_back(f);
            for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
                uint32_t t = m.heTwin[h];
                if (t == kInvalid || mi.barrier[m.heEdge[h]]) continue;
                auto it = local.find(m.heFace[t]);
                if (it != local.end() && comp[size_t(it->second)] < 0) {
                    comp[size_t(it->second)] = comp[i];
                    stack.push_back(uint32_t(it->second));
                }
            }
        }
    }
    return out;
}

// Gives a chart its own uv indices. Corners share a uv only when they are connected around their
// vertex through edges *inside* the chart that are not barriers, so a chart never folds over an
// edge it did not grow across.
std::vector<uint32_t> assignChartUVs(Mesh& m, const MeshInfo& mi, const std::vector<uint32_t>& faces)
{
    std::unordered_map<uint32_t, uint32_t> parent; // corner -> parent corner
    std::unordered_map<uint32_t, uint8_t> inChart;
    for (uint32_t f : faces) inChart[f] = 1;
    for (uint32_t f : faces)
        for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) parent[h] = h;
    auto find = [&](uint32_t x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    auto unite = [&](uint32_t a, uint32_t b) {
        a = find(a); b = find(b);
        if (a != b) parent[std::max(a, b)] = std::min(a, b);
    };
    for (uint32_t f : faces)
        for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
            uint32_t t = m.heTwin[h];
            if (t == kInvalid || mi.barrier[m.heEdge[h]] || !inChart.count(m.heFace[t])) continue;
            unite(h, m.heNext(t));
            unite(m.heNext(h), t);
        }
    std::unordered_map<uint32_t, uint32_t> uvOf; // root corner -> uv
    std::vector<uint32_t> list;
    for (uint32_t f : faces)
        for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
            auto [it, ins] = uvOf.emplace(find(h), uint32_t(m.uvs.size()));
            if (ins) { m.uvs.push_back({}); list.push_back(it->second); }
            m.faceUVs[h] = it->second;
        }
    return list;
}

struct ChartQuality { float stretch; int flipped; float a2, a3; float angle; };

ChartQuality evaluate(const Mesh& m, const MeshInfo& mi, const std::vector<uint32_t>& faces)
{
    ChartQuality q{1, 0, 0, 0, 1};
    std::vector<float> fa2(faces.size());
    for (size_t i = 0; i < faces.size(); ++i) {
        uint32_t f = faces[i], b = m.faceStart[f];
        float a = 0;
        for (uint32_t k = 1; k + 1 < m.faceSize(f); ++k) a += 0.5f * cross2(m.cornerUV(b + k) - m.cornerUV(b), m.cornerUV(b + k + 1) - m.cornerUV(b));
        fa2[i] = a;
        q.a2 += std::abs(a);
        q.a3 += mi.area[f];
        if (a <= 0) ++q.flipped;
    }
    if (q.a2 <= 0 || q.a3 <= 0) { q.stretch = 1e9f; return q; }
    double s = 0;
    const double norm = q.a2 / q.a3;
    for (size_t i = 0; i < faces.size(); ++i) {
        double r = (std::abs(fa2[i]) / std::max(mi.area[faces[i]], 1e-30f)) / norm;
        s += mi.area[faces[i]] * std::max(r, r > 1e-9 ? 1.0 / r : 1e9);
    }
    q.stretch = float(s / q.a3);
    // Angle distortion (singular-value ratio of the 3D->uv Jacobian), area weighted.
    double ang = 0, wsum = 0;
    for (uint32_t f : faces) {
        const uint32_t b = m.faceStart[f];
        for (uint32_t k = 1; k + 1 < m.faceSize(f); ++k) {
            const uint32_t c[3] = {b, b + k, b + k + 1};
            Vec3 P0 = m.positions[m.faceVerts[c[0]]], e1 = m.positions[m.faceVerts[c[1]]] - P0, e2 = m.positions[m.faceVerts[c[2]]] - P0;
            float l = length(e1), a3 = 0.5f * length(cross(e1, e2));
            if (l < 1e-20f || a3 < 1e-20f) continue;
            Vec3 ex = e1 / l, ey = normalize(cross(normalize(cross(e1, e2)), ex));
            float x3 = dot(e2, ex), y3 = dot(e2, ey);
            Vec2 du1 = m.cornerUV(c[1]) - m.cornerUV(c[0]), du2 = m.cornerUV(c[2]) - m.cornerUV(c[0]);
            float j00 = du1.x / l, j10 = du1.y / l, j01 = (du2.x - j00 * x3) / y3, j11 = (du2.y - j10 * x3) / y3;
            float A = j00 * j00 + j10 * j10, C = j01 * j01 + j11 * j11, B = j00 * j01 + j10 * j11;
            float tr = A + C, dd = std::sqrt(std::max(0.0f, (A - C) * (A - C) + 4 * B * B));
            float s1 = std::sqrt(std::max(0.0f, (tr + dd) * 0.5f)), s2 = std::sqrt(std::max(0.0f, (tr - dd) * 0.5f));
            ang += a3 * std::min(s2 > 1e-20f ? double(s1 / s2) : 1e3, 1e3);
            wsum += a3;
        }
    }
    q.angle = wsum > 0 ? float(ang / wsum) : 1.0f;
    return q;
}

// Splits a face set in two by 2-means on normals (falls back to a spatial split).
std::pair<std::vector<uint32_t>, std::vector<uint32_t>> split(const Mesh& m, const MeshInfo& mi, const std::vector<uint32_t>& faces)
{
    Vec3 avg;
    for (uint32_t f : faces) avg += mi.normal[f] * mi.area[f];
    avg = normalize(avg);
    uint32_t a = faces[0];
    for (uint32_t f : faces)
        if (dot(mi.normal[f], avg) < dot(mi.normal[a], avg)) a = f;
    uint32_t b = faces[0];
    for (uint32_t f : faces)
        if (dot(mi.normal[f], mi.normal[a]) < dot(mi.normal[b], mi.normal[a])) b = f;
    Vec3 ca = mi.normal[a], cb = mi.normal[b];
    std::vector<uint32_t> A, B;
    for (int it = 0; it < 6; ++it) {
        A.clear(); B.clear();
        Vec3 sa, sb;
        for (uint32_t f : faces) {
            if (dot(mi.normal[f], ca) >= dot(mi.normal[f], cb)) { A.push_back(f); sa += mi.normal[f] * mi.area[f]; }
            else { B.push_back(f); sb += mi.normal[f] * mi.area[f]; }
        }
        if (A.empty() || B.empty()) break;
        ca = normalize(sa);
        cb = normalize(sb);
    }
    if (A.empty() || B.empty() || dot(ca, cb) > 0.999f) {
        // Nearly flat but stretched (e.g. a long folded strip): split along the longest spatial axis.
        AABB box;
        for (uint32_t f : faces) box.expand(m.faceCenter(f));
        Vec3 sz = box.size();
        int axis = sz.x > sz.y ? (sz.x > sz.z ? 0 : 2) : (sz.y > sz.z ? 1 : 2);
        std::vector<std::pair<float, uint32_t>> keyed;
        for (uint32_t f : faces) keyed.push_back({m.faceCenter(f)[axis], f});
        std::sort(keyed.begin(), keyed.end());
        A.clear(); B.clear();
        for (size_t i = 0; i < keyed.size(); ++i) (i < keyed.size() / 2 ? A : B).push_back(keyed[i].second);
    }
    return {A, B};
}

// Unfolds a chart; splits it recursively while it is flipped or too stretched.
void solveChart(Mesh& m, const MeshInfo& mi, std::vector<uint32_t> faces, const Options& opt, int depth,
                std::vector<std::vector<uint32_t>>& done, int& splits)
{
    auto uvs = assignChartUVs(m, mi, faces);
    // Planar start along the chart's mean normal, then LSCM.
    Vec3 n;
    for (uint32_t f : faces) n += mi.normal[f] * mi.area[f];
    n = normalize(n);
    Vec3 up = std::abs(n.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    Vec3 tu = normalize(cross(up, n)), tv = cross(n, tu);
    for (uint32_t f : faces)
        for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
            Vec3 p = m.positions[m.faceVerts[h]];
            m.uvs[m.faceUVs[h]] = {dot(p, tu), dot(p, tv)};
        }
    if (faces.size() > 1) uv::unfold(m, uvs, 4000);
    ChartQuality q = evaluate(m, mi, faces);
    if ((q.flipped > 0 || q.stretch > opt.maxStretch || q.angle > 1.0f + (opt.maxStretch - 1.0f) * 1.5f) && depth < opt.maxSplitDepth &&
        faces.size() > 1) {
        auto [A, B] = split(m, mi, faces);
        if (!A.empty() && !B.empty()) {
            ++splits;
            for (auto* part : {&A, &B})
                for (auto& comp : components(m, mi, *part)) solveChart(m, mi, comp, opt, depth + 1, done, splits);
            return;
        }
    }
    done.push_back(std::move(faces));
}

// ---- packing ----

struct Rect {
    size_t chart;
    float w, h;
    bool rotated = false;
    float x = 0, y = 0;
};

// Skyline bottom-left packer into a bin of width W. Returns the used height.
float skylinePack(std::vector<Rect>& rects, float W, float pad, bool allowRotate)
{
    struct Seg { float x, y, w; };
    std::vector<Seg> sky{{0, 0, W}};
    float top = 0;
    for (Rect& r : rects) {
        float bestY = 1e30f, bestX = 0;
        bool bestRot = false;
        size_t bestI = 0;
        for (int rot = 0; rot < (allowRotate ? 2 : 1); ++rot) {
            float rw = (rot ? r.h : r.w) + pad, rh = (rot ? r.w : r.h) + pad;
            for (size_t i = 0; i < sky.size(); ++i) {
                float x = sky[i].x;
                if (x + rw > W + 1e-6f) break;
                float y = 0, span = 0;
                for (size_t j = i; j < sky.size() && span < rw; ++j) {
                    y = std::max(y, sky[j].y);
                    span += sky[j].w;
                }
                if (span + 1e-6f < rw) continue;
                if (y + rh < bestY + 1e-9f && (y + rh < bestY - 1e-9f || x < bestX)) { bestY = y + rh; bestX = x; bestRot = rot; bestI = i; }
            }
        }
        if (bestY >= 1e29f) { // wider than the bin: put it on top
            bestRot = false;
            bestX = 0;
            bestY = top + r.h + pad;
            bestI = 0;
        }
        r.rotated = bestRot;
        r.x = bestX + pad * 0.5f;
        float rw = (bestRot ? r.h : r.w) + pad, rh = (bestRot ? r.w : r.h) + pad;
        r.y = bestY - rh + pad * 0.5f;
        top = std::max(top, bestY);
        // Update the skyline: replace [bestX, bestX+rw) with height bestY.
        std::vector<Seg> ns;
        for (const Seg& s : sky) {
            float s0 = s.x, s1 = s.x + s.w, c0 = bestX, c1 = bestX + rw;
            if (s1 <= c0 || s0 >= c1) { ns.push_back(s); continue; }
            if (s0 < c0) ns.push_back({s0, s.y, c0 - s0});
            if (s1 > c1) ns.push_back({c1, s.y, s1 - c1});
        }
        ns.push_back({bestX, bestY, rw});
        std::sort(ns.begin(), ns.end(), [](const Seg& a, const Seg& b) { return a.x < b.x; });
        // Merge equal-height neighbours.
        sky.clear();
        for (const Seg& s : ns) {
            if (!sky.empty() && std::abs(sky.back().y - s.y) < 1e-9f && std::abs(sky.back().x + sky.back().w - s.x) < 1e-6f) sky.back().w += s.w;
            else sky.push_back(s);
        }
        (void)bestI;
    }
    return top;
}

// ---- shape-aware raster packing ----
//
// Each shell is rasterised (conservatively, then dilated by the padding) onto a G x G grid covering
// the 0..1 tile. Shells are placed largest-first at the lowest, then left-most free position, trying
// four 90-degree rotations. A binary search finds the largest uniform scale (texel density) at which
// every shell fits.

constexpr int G = 256;
constexpr int W64 = G / 64;
using Row = std::array<uint64_t, W64>;

struct Mask {
    int w = 0, h = 0;
    std::vector<Row> rows;                 // unshifted
    std::vector<std::vector<Row>> shifted; // [x][row]
    Vec2 lo;                               // local (rotated) coords of cell (0,0) corner, before dilation offset
    int dil = 0;
};

struct ShellGeom {
    std::vector<std::array<Vec2, 3>> tris; // local coords (already at world density, min-area rotated)
    float area = 0;
};

inline void setBit(Row& r, int x) { r[size_t(x >> 6)] |= uint64_t(1) << (x & 63); }

Row shiftRow(const Row& r, int x)
{
    Row o{};
    const int ws = x >> 6, bs = x & 63;
    for (int i = W64 - 1; i >= 0; --i) {
        uint64_t v = 0;
        int src = i - ws;
        if (src >= 0) v = r[size_t(src)] << bs;
        if (bs && src - 1 >= 0) v |= r[size_t(src - 1)] >> (64 - bs);
        o[size_t(i)] = v;
    }
    return o;
}

Vec2 rot90(Vec2 p, int r)
{
    switch (r & 3) {
    case 1: return {-p.y, p.x};
    case 2: return {-p.x, -p.y};
    case 3: return {p.y, -p.x};
    default: return p;
    }
}

bool buildMask(const ShellGeom& sg, int rot, float cellsPerUnit, int dil, Mask& m)
{
    Vec2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
    for (auto& t : sg.tris)
        for (const Vec2& p : t) {
            Vec2 q = rot90(p, rot);
            lo = {std::min(lo.x, q.x), std::min(lo.y, q.y)};
            hi = {std::max(hi.x, q.x), std::max(hi.y, q.y)};
        }
    m.lo = lo;
    m.dil = dil;
    m.w = int(std::ceil((hi.x - lo.x) * cellsPerUnit)) + 1 + 2 * dil;
    m.h = int(std::ceil((hi.y - lo.y) * cellsPerUnit)) + 1 + 2 * dil;
    if (m.w > G || m.h > G) return false;
    std::vector<uint8_t> grid(size_t(m.w) * m.h, 0);
    auto mark = [&](float x, float y) {
        int cx = int(std::floor(x)), cy = int(std::floor(y));
        if (cx >= 0 && cy >= 0 && cx < m.w && cy < m.h) grid[size_t(cy) * m.w + cx] = 1;
    };
    for (auto& t : sg.tris) {
        Vec2 c[3];
        for (int k = 0; k < 3; ++k) c[k] = (rot90(t[size_t(k)], rot) - lo) * cellsPerUnit + Vec2{float(dil), float(dil)};
        // Edges (supercover by half-cell sampling) and vertices: conservative for slivers.
        for (int k = 0; k < 3; ++k) {
            Vec2 a = c[k], b = c[(k + 1) % 3];
            int n = int(std::ceil((b - a).length() * 2.0f)) + 1;
            for (int i = 0; i <= n; ++i) { Vec2 p = lerp(a, b, float(i) / n); mark(p.x, p.y); }
        }
        // Interior cell centres.
        float area = cross2(c[1] - c[0], c[2] - c[0]);
        if (std::abs(area) < 1e-9f) continue;
        int x0 = int(std::floor(std::min({c[0].x, c[1].x, c[2].x}))), x1 = int(std::ceil(std::max({c[0].x, c[1].x, c[2].x})));
        int y0 = int(std::floor(std::min({c[0].y, c[1].y, c[2].y}))), y1 = int(std::ceil(std::max({c[0].y, c[1].y, c[2].y})));
        for (int y = std::max(0, y0); y <= std::min(m.h - 1, y1); ++y)
            for (int x = std::max(0, x0); x <= std::min(m.w - 1, x1); ++x) {
                Vec2 p{x + 0.5f, y + 0.5f};
                float w0 = cross2(c[1] - p, c[2] - p), w1 = cross2(c[2] - p, c[0] - p), w2 = cross2(c[0] - p, c[1] - p);
                bool in = area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0);
                if (in) grid[size_t(y) * m.w + x] = 1;
            }
    }
    // Dilate by the padding (square structuring element).
    if (dil > 0) {
        std::vector<uint8_t> out(grid.size(), 0);
        for (int y = 0; y < m.h; ++y)
            for (int x = 0; x < m.w; ++x) {
                if (!grid[size_t(y) * m.w + x]) continue;
                for (int dy = -dil; dy <= dil; ++dy)
                    for (int dx = -dil; dx <= dil; ++dx) {
                        int xx = x + dx, yy = y + dy;
                        if (xx >= 0 && yy >= 0 && xx < m.w && yy < m.h) out[size_t(yy) * m.w + xx] = 1;
                    }
            }
        grid.swap(out);
    }
    m.rows.assign(size_t(m.h), Row{});
    for (int y = 0; y < m.h; ++y)
        for (int x = 0; x < m.w; ++x)
            if (grid[size_t(y) * m.w + x]) setBit(m.rows[size_t(y)], x);
    m.shifted.assign(size_t(G - m.w + 1), {});
    for (int x = 0; x <= G - m.w; ++x) {
        auto& sh = m.shifted[size_t(x)];
        sh.resize(size_t(m.h));
        for (int y = 0; y < m.h; ++y) sh[size_t(y)] = shiftRow(m.rows[size_t(y)], x);
    }
    return true;
}

struct RasterPlacement { int rot = 0, x = 0, y = 0; Mask mask; };

// Tries to place all shells at `cellsPerUnit`. Fills placements on success.
bool rasterFit(const std::vector<ShellGeom>& shells, const std::vector<size_t>& order, float cellsPerUnit, int dil, bool rotate,
               std::vector<RasterPlacement>& out)
{
    std::vector<Row> occ(size_t(G), Row{});
    out.assign(shells.size(), {});
    for (size_t idx : order) {
        int bestY = G, bestX = G, bestR = -1;
        Mask bestMask;
        for (int r = 0; r < (rotate ? 4 : 1); ++r) {
            Mask mk;
            if (!buildMask(shells[idx], r, cellsPerUnit, dil, mk)) continue;
            bool found = false;
            for (int y = 0; y + mk.h <= G && y <= bestY && !found; ++y) {
                for (int x = 0; x + mk.w <= G; ++x) {
                    if (y == bestY && x >= bestX) break;
                    const auto& sh = mk.shifted[size_t(x)];
                    bool clash = false;
                    for (int ry = 0; ry < mk.h && !clash; ++ry) {
                        const Row& o = occ[size_t(y + ry)];
                        const Row& s = sh[size_t(ry)];
                        for (int k = 0; k < W64; ++k)
                            if (o[size_t(k)] & s[size_t(k)]) { clash = true; break; }
                    }
                    if (!clash) {
                        if (y < bestY || (y == bestY && x < bestX)) { bestY = y; bestX = x; bestR = r; bestMask = mk; }
                        found = true;
                        break;
                    }
                }
            }
        }
        if (bestR < 0) return false;
        for (int ry = 0; ry < bestMask.h; ++ry) {
            const Row& s = bestMask.shifted[size_t(bestX)][size_t(ry)];
            for (int k = 0; k < W64; ++k) occ[size_t(bestY + ry)][size_t(k)] |= s[size_t(k)];
        }
        bestMask.shifted.clear();
        out[idx] = {bestR, bestX, bestY, std::move(bestMask)};
    }
    return true;
}

} // namespace

Report unwrap(std::vector<Target>& targets, const Options& opt)
{
    // Work in world space so shells are conformal and densities uniform after the object transform
    // (e.g. non-uniform scale). Original positions are restored afterwards.
    std::vector<std::vector<Vec3>> saved;
    std::vector<uint64_t> savedPosId;
    for (auto& t : targets) {
        saved.push_back(t.mesh->positions);
        savedPosId.push_back(t.mesh->positionId);
        for (auto& p : t.mesh->positions) p = t.world.transformPoint(p);
    }
    struct Restore {
        std::vector<Target>& t;
        std::vector<std::vector<Vec3>>& s;
        std::vector<uint64_t>& id;
        ~Restore()
        {
            for (size_t i = 0; i < t.size(); ++i) { t[i].mesh->positions = std::move(s[i]); t[i].mesh->positionId = id[i]; }
        }
    } restore{targets, saved, savedPosId};
    for (auto& t : targets) t.world = Mat4(); // positions are already in world space
    const bool timing = std::getenv("FACET_UNWRAP_TIMING") != nullptr;
    auto clk = std::chrono::steady_clock::now();
    auto lap = [&](const char* what) {
        if (!timing) return;
        auto now = std::chrono::steady_clock::now();
        std::fprintf(stderr, "  [smart unwrap] %-10s %8.1f ms\n", what, std::chrono::duration<double, std::milli>(now - clk).count());
        clk = now;
    };
    Report rep;
    std::vector<MeshInfo> infos;
    std::vector<Chart> charts;
    for (size_t ti = 0; ti < targets.size(); ++ti) {
        Mesh& m = *targets[ti].mesh;
        if (!m.topologyValid()) m.buildTopology();
        m.ensureUVs();
        infos.push_back(analyse(targets[ti], opt));
        const MeshInfo& mi = infos.back();
        auto grown = growCharts(m, mi, opt.angleLimit);
        rep.merges += mergeTiny(m, mi, grown, opt.minShellFraction);
        lap("charts");
        for (auto& g : grown)
            for (auto& comp : components(m, mi, g)) {
                std::vector<std::vector<uint32_t>> done;
                solveChart(m, mi, comp, opt, 0, done, rep.splits);
                for (auto& d : done) charts.push_back({ti, std::move(d)});
            }
        lap("unfold");
    }

    // Uniform world-space texel density: scale every chart so uv area == world area,
    // then rotate it into its tightest bounding box.
    struct Placed { std::vector<uint32_t> uvs; Vec2 lo; float w, h; };
    std::vector<Placed> placed(charts.size());
    for (size_t c = 0; c < charts.size(); ++c) {
        Mesh& m = *targets[charts[c].target].mesh;
        const MeshInfo& mi = infos[charts[c].target];
        ChartQuality q = evaluate(m, mi, charts[c].faces);
        float k = q.a2 > 1e-30f ? std::sqrt(q.a3 / q.a2) : 1.0f;
        std::vector<uint32_t>& uvs = placed[c].uvs;
        for (uint32_t f : charts[c].faces)
            for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) uvs.push_back(m.faceUVs[h]);
        std::sort(uvs.begin(), uvs.end());
        uvs.erase(std::unique(uvs.begin(), uvs.end()), uvs.end());
        Vec2 ctr;
        for (uint32_t u : uvs) { m.uvs[u] = m.uvs[u] * k; ctr += m.uvs[u]; }
        ctr = ctr / float(uvs.size());
        float bestAng = 0, bestA = 1e30f;
        for (int deg = 0; deg < 90; ++deg) {
            float cs = std::cos(radians(float(deg))), sn = std::sin(radians(float(deg)));
            Vec2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
            for (uint32_t u : uvs) {
                Vec2 d = m.uvs[u] - ctr, r{cs * d.x - sn * d.y, sn * d.x + cs * d.y};
                lo = {std::min(lo.x, r.x), std::min(lo.y, r.y)};
                hi = {std::max(hi.x, r.x), std::max(hi.y, r.y)};
            }
            float a = (hi.x - lo.x) * (hi.y - lo.y);
            if (a < bestA * 0.999f) { bestA = a; bestAng = float(deg); }
        }
        float cs = std::cos(radians(bestAng)), sn = std::sin(radians(bestAng));
        Vec2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
        for (uint32_t u : uvs) {
            Vec2 d = m.uvs[u] - ctr;
            m.uvs[u] = {cs * d.x - sn * d.y, sn * d.x + cs * d.y};
            lo = {std::min(lo.x, m.uvs[u].x), std::min(lo.y, m.uvs[u].y)};
            hi = {std::max(hi.x, m.uvs[u].x), std::max(hi.y, m.uvs[u].y)};
        }
        placed[c].lo = lo;
        placed[c].w = hi.x - lo.x;
        placed[c].h = hi.y - lo.y;
    }

    lap("density");
    // Pack: groups are either everything (shared density) or one group per mesh.
    std::vector<std::vector<size_t>> groups;
    if (opt.packTogether) {
        groups.emplace_back(charts.size());
        std::iota(groups[0].begin(), groups[0].end(), size_t(0));
    } else {
        groups.resize(targets.size());
        for (size_t c = 0; c < charts.size(); ++c) groups[charts[c].target].push_back(c);
    }
    float densitySum = 0, effSum = 0;
    int groupCount = 0;
    for (auto& grp : groups) {
        if (grp.empty()) continue;
        float total = 0, maxSide = 0;
        for (size_t c : grp) { total += placed[c].w * placed[c].h; maxSide = std::max({maxSide, placed[c].w, placed[c].h}); }
        // Padding is defined in texels, so it depends on the final scale: iterate a few times.
        float side = std::max(std::sqrt(total / 0.7f), maxSide);
        std::vector<Rect> best;
        float bestSide = 1e30f;
        for (int iter = 0; iter < 3; ++iter) {
            const float pad = opt.paddingPx / float(std::max(1, opt.textureSize)) * side;
            for (float f : {0.85f, 0.95f, 1.0f, 1.05f, 1.15f, 1.3f, 1.5f}) {
                float W = std::max(std::sqrt(total) * f, maxSide + pad);
                std::vector<Rect> rects;
                for (size_t c : grp) rects.push_back({c, placed[c].w, placed[c].h});
                std::sort(rects.begin(), rects.end(), [](const Rect& a, const Rect& b) { return std::max(a.w, a.h) > std::max(b.w, b.h); });
                float H = skylinePack(rects, W, pad, opt.rotateShells);
                float s = std::max(W, H);
                if (s < bestSide) { bestSide = s; best = rects; }
            }
            side = bestSide;
        }
        // Shape-aware raster packing: binary search the largest density (cells per world unit) that fits.
        std::vector<ShellGeom> geoms(grp.size());
        float shellArea = 0;
        for (size_t gi = 0; gi < grp.size(); ++gi) {
            const Chart& ch = charts[grp[gi]];
            const Mesh& m = *targets[ch.target].mesh;
            for (uint32_t f : ch.faces) {
                const uint32_t b = m.faceStart[f];
                for (uint32_t k = 1; k + 1 < m.faceSize(f); ++k) {
                    geoms[gi].tris.push_back({m.cornerUV(b), m.cornerUV(b + k), m.cornerUV(b + k + 1)});
                    geoms[gi].area += 0.5f * std::abs(cross2(m.cornerUV(b + k) - m.cornerUV(b), m.cornerUV(b + k + 1) - m.cornerUV(b)));
                }
            }
            shellArea += geoms[gi].area;
        }
        std::vector<size_t> order(grp.size());
        std::iota(order.begin(), order.end(), size_t(0));
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return geoms[a].area > geoms[b].area; });
        const int dil = int(std::ceil(opt.paddingPx / float(std::max(1, opt.textureSize)) * G * 0.5f));
        float lo = float(G) / bestSide * 0.9f, hi = float(G) / std::sqrt(std::max(shellArea, 1e-20f));
        std::vector<RasterPlacement> rp, bestRP;
        float bestCells = 0;
        if (rasterFit(geoms, order, lo, dil, opt.rotateShells, rp)) { bestRP = rp; bestCells = lo; }
        for (int it = 0; it < 9 && bestCells > 0; ++it) {
            float mid = 0.5f * (lo + hi);
            if (rasterFit(geoms, order, mid, dil, opt.rotateShells, rp)) { bestRP = rp; bestCells = mid; lo = mid; }
            else hi = mid;
        }
        if (bestCells > float(G) / bestSide) {
            for (size_t gi = 0; gi < grp.size(); ++gi) {
                const Chart& ch = charts[grp[gi]];
                Mesh& m = *targets[ch.target].mesh;
                const RasterPlacement& p = bestRP[gi];
                for (uint32_t u : placed[grp[gi]].uvs) {
                    Vec2 q = (rot90(m.uvs[u], p.rot) - p.mask.lo) * bestCells + Vec2{float(p.mask.dil + p.x), float(p.mask.dil + p.y)};
                    m.uvs[u] = q / float(G);
                }
            }
            densitySum += float(opt.textureSize) * bestCells / float(G);
            effSum += shellArea * bestCells * bestCells / float(G * G);
        } else {
            // Raster packing did not beat the box packer: keep the skyline result.
            for (const Rect& r : best) {
                const Chart& ch = charts[r.chart];
                Mesh& m = *targets[ch.target].mesh;
                for (uint32_t u : placed[r.chart].uvs) {
                    Vec2 d = m.uvs[u] - placed[r.chart].lo;
                    if (r.rotated) d = {placed[r.chart].h - d.y, d.x};
                    m.uvs[u] = Vec2{r.x + d.x, r.y + d.y} / bestSide;
                }
            }
            densitySum += float(opt.textureSize) / bestSide;
            effSum += shellArea / (bestSide * bestSide);
        }
        ++groupCount;
    }
    lap("pack");
    for (auto& t : targets) {
        t.mesh->compactUVs();
        t.mesh->touchUVs();
    }
    rep.shells = int(charts.size());
    rep.texelDensity = groupCount ? densitySum / groupCount : 0;
    rep.efficiency = groupCount ? effSum / groupCount : 0;
    return rep;
}

} // namespace facet::smart
