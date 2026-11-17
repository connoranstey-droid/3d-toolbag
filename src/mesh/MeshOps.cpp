#include "mesh/MeshOps.h"

#include <algorithm>
#include <numeric>
#include <unordered_map>

namespace facet::ops {

void sortUnique(IndexList& l)
{
    std::sort(l.begin(), l.end());
    l.erase(std::unique(l.begin(), l.end()), l.end());
}

namespace {

struct UnionFind {
    std::vector<uint32_t> p;
    explicit UnionFind(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0u); }
    uint32_t find(uint32_t x)
    {
        while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; }
        return x;
    }
    void unite(uint32_t a, uint32_t b) { a = find(a); b = find(b); if (a != b) p[std::max(a, b)] = std::min(a, b); }
};

std::vector<uint8_t> flags(size_t n, const IndexList& items)
{
    std::vector<uint8_t> f(n, 0);
    for (uint32_t i : items)
        if (i < n) f[i] = 1;
    return f;
}

// Per-vertex uv value taken from any corner (for faces created around existing vertices).
std::vector<Vec2> anyCornerUV(const Mesh& m)
{
    std::vector<Vec2> out(m.numVerts());
    if (!m.hasUVs()) return out;
    for (uint32_t h = 0; h < m.numHalfEdges(); ++h) out[m.faceVerts[h]] = m.uvs[m.faceUVs[h]];
    return out;
}

// Rewrites every face through `remap`, removing repeated consecutive vertices and degenerate faces.
void remapFacesAndClean(Mesh& m, const std::vector<uint32_t>& remap)
{
    const bool uv = m.hasUVs();
    std::vector<uint32_t> newStart{0}, newVerts, newUV;
    newVerts.reserve(m.faceVerts.size());
    std::vector<std::pair<uint32_t, uint32_t>> poly;
    for (uint32_t f = 0; f < m.numFaces(); ++f) {
        poly.clear();
        for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
            uint32_t r = remap[m.faceVerts[h]];
            if (poly.empty() || poly.back().first != r) poly.push_back({r, uv ? m.faceUVs[h] : 0u});
        }
        while (poly.size() > 1 && poly.front().first == poly.back().first) poly.pop_back();
        if (poly.size() < 3) continue;
        for (auto [v, u] : poly) { newVerts.push_back(v); if (uv) newUV.push_back(u); }
        newStart.push_back(uint32_t(newVerts.size()));
    }
    if (uv) m.faceUVs = std::move(newUV);
    // Remap stale edges too so creases survive the rebuild.
    for (auto& e : m.edges)
        if (e.v0 != kInvalid) { e.v0 = remap[e.v0]; e.v1 = remap[e.v1]; }
    m.faceStart = std::move(newStart);
    m.faceVerts = std::move(newVerts);
    m.removeUnusedVertices();
    if (uv) m.weldUVs();
    m.buildTopology();
}

} // namespace

IndexList extrudeFaces(Mesh& m, const IndexList& faces, bool keepTogether, float offset)
{
    if (faces.empty()) return {};
    const auto sel = flags(m.numFaces(), faces);
    std::vector<uint32_t> newVerts(m.faceVerts);
    std::vector<uint32_t> sideFaces; // flattened quads
    std::vector<Vec3> capNormal(m.numVerts());
    std::vector<uint8_t> isCap(m.numVerts(), 0);

    auto oppositeSelected = [&](uint32_t h) {
        uint32_t t = m.heTwin[h];
        return t != kInvalid && sel[m.heFace[t]];
    };

    if (keepTogether) {
        std::vector<uint32_t> dup(m.numVerts(), kInvalid);
        auto dupOf = [&](uint32_t v) {
            if (dup[v] == kInvalid) dup[v] = m.addVertex(m.positions[v]);
            return dup[v];
        };
        for (uint32_t f : faces) {
            for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
                uint32_t v = m.faceVerts[h];
                bool needs = !oppositeSelected(h) || !oppositeSelected(m.hePrev(h));
                if (!needs)
                    for (uint32_t of : m.vertexFaces(v))
                        if (!sel[of]) { needs = true; break; }
                if (needs) dupOf(v);
            }
        }
        for (uint32_t f : faces) {
            for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
                uint32_t v = m.faceVerts[h];
                if (dup[v] != kInvalid) newVerts[h] = dup[v];
                if (!oppositeSelected(h)) {
                    uint32_t a = m.heFrom(h), b = m.heTo(h);
                    sideFaces.insert(sideFaces.end(), {a, b, dup[b], dup[a]});
                }
            }
        }
    } else {
        for (uint32_t f : faces) {
            uint32_t b0 = m.faceStart[f], n = m.faceSize(f);
            std::vector<uint32_t> d(n);
            for (uint32_t i = 0; i < n; ++i) d[i] = m.addVertex(m.positions[m.faceVerts[b0 + i]]);
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t j = (i + 1) % n;
                sideFaces.insert(sideFaces.end(), {m.faceVerts[b0 + i], m.faceVerts[b0 + j], d[j], d[i]});
                newVerts[b0 + i] = d[i];
            }
        }
    }

    capNormal.resize(m.numVerts());
    isCap.resize(m.numVerts(), 0);
    m.faceVerts = std::move(newVerts);
    if (offset != 0.0f) {
        for (uint32_t f : faces) {
            Vec3 n = m.faceNormalRaw(f);
            for (uint32_t v : m.faceVertices(f)) { capNormal[v] += n; isCap[v] = 1; }
        }
        for (uint32_t v = 0; v < m.numVerts(); ++v)
            if (isCap[v]) m.positions[v] += normalize(capNormal[v]) * offset;
    }
    if (m.hasUVs()) {
        const auto any = anyCornerUV(m);
        for (size_t i = 0; i < sideFaces.size(); i += 4) {
            const uint32_t q[4] = {sideFaces[i], sideFaces[i + 1], sideFaces[i + 2], sideFaces[i + 3]};
            const Vec2 u[4] = {any[q[0]], any[q[1]], any[q[2]], any[q[3]]};
            m.addFaceWithUVs(q, u);
        }
        m.weldUVs();
    } else {
        for (size_t i = 0; i < sideFaces.size(); i += 4) m.addFace({sideFaces[i], sideFaces[i + 1], sideFaces[i + 2], sideFaces[i + 3]});
    }
    m.buildTopology();
    return faces;
}

void deleteFaces(Mesh& m, const IndexList& faces)
{
    const auto sel = flags(m.numFaces(), faces);
    const bool uv = m.hasUVs();
    std::vector<uint32_t> ns{0}, nv, nu;
    for (uint32_t f = 0; f < m.numFaces(); ++f) {
        if (sel[f]) continue;
        for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
            nv.push_back(m.faceVerts[h]);
            if (uv) nu.push_back(m.faceUVs[h]);
        }
        ns.push_back(uint32_t(nv.size()));
    }
    m.faceStart = std::move(ns);
    m.faceVerts = std::move(nv);
    if (uv) { m.faceUVs = std::move(nu); m.compactUVs(); }
    m.removeUnusedVertices();
    m.buildTopology();
}

bool dissolveEdges(Mesh& m, const IndexList& edgeList, bool removeWinged)
{
    const uint32_t F = m.numFaces();
    std::vector<uint8_t> dissolved(m.numEdges(), 0);
    UnionFind uf(F);
    std::vector<uint32_t> touchedVerts;
    for (uint32_t e : edgeList) {
        if (e >= m.numEdges() || m.edges[e].faceCount != 2) continue;
        uint32_t h = m.edges[e].he, t = m.heTwin[h];
        if (t == kInvalid) continue;
        dissolved[e] = 1;
        uf.unite(m.heFace[h], m.heFace[t]);
        touchedVerts.push_back(m.edges[e].v0);
        touchedVerts.push_back(m.edges[e].v1);
    }
    if (touchedVerts.empty()) return false;

    // Group faces by region root.
    std::vector<uint32_t> regionSize(F, 0);
    for (uint32_t f = 0; f < F; ++f) regionSize[uf.find(f)]++;

    const bool uv = m.hasUVs();
    std::vector<uint32_t> ns{0}, nv, nu;
    auto emitFace = [&](uint32_t f) {
        for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
            nv.push_back(m.faceVerts[h]);
            if (uv) nu.push_back(m.faceUVs[h]);
        }
        ns.push_back(uint32_t(nv.size()));
    };
    std::vector<uint8_t> emitted(F, 0);
    std::vector<uint8_t> heUsed(m.numHalfEdges(), 0);
    for (uint32_t f = 0; f < F; ++f) {
        uint32_t root = uf.find(f);
        if (regionSize[root] == 1) {
            emitFace(f);
            continue;
        }
        if (emitted[root]) continue;
        emitted[root] = 1;
        // Collect region faces and boundary loops.
        std::vector<uint32_t> regionFaces;
        for (uint32_t g = f; g < F; ++g)
            if (uf.find(g) == root) regionFaces.push_back(g);
        std::vector<std::vector<uint32_t>> loops; // loops of corner (half-edge) ids
        bool ok = true;
        for (uint32_t rf : regionFaces) {
            for (uint32_t h = m.faceStart[rf]; h < m.faceStart[rf + 1] && ok; ++h) {
                if (dissolved[m.heEdge[h]] || heUsed[h]) continue;
                std::vector<uint32_t> loop;
                uint32_t cur = h;
                size_t guard = 0;
                do {
                    heUsed[cur] = 1;
                    loop.push_back(cur);
                    uint32_t g = m.heNext(cur);
                    size_t spin = 0;
                    while (dissolved[m.heEdge[g]]) {
                        g = m.heNext(m.heTwin[g]);
                        if (++spin > m.numHalfEdges()) { ok = false; break; }
                    }
                    cur = g;
                    if (++guard > m.numHalfEdges()) { ok = false; break; }
                } while (ok && cur != h);
                loops.push_back(std::move(loop));
            }
        }
        if (ok && loops.size() == 1 && loops[0].size() >= 3) {
            for (uint32_t h : loops[0]) {
                nv.push_back(m.faceVerts[h]);
                if (uv) nu.push_back(m.faceUVs[h]);
            }
            ns.push_back(uint32_t(nv.size()));
        } else {
            for (uint32_t rf : regionFaces) emitFace(rf);
        }
    }
    m.faceStart = std::move(ns);
    m.faceVerts = std::move(nv);
    if (uv) m.faceUVs = std::move(nu);
    std::vector<uint32_t> remap;
    m.removeUnusedVertices(&remap);
    m.buildTopology();

    if (removeWinged) {
        // Remove touched vertices left with exactly two edges (they add nothing to the shape).
        std::vector<uint8_t> drop(m.numVerts(), 0);
        bool any = false;
        for (uint32_t ov : touchedVerts) {
            uint32_t v = remap[ov];
            if (v == kInvalid) continue;
            if (m.vertexEdges(v).size() == 2) { drop[v] = 1; any = true; }
        }
        if (any) {
            std::vector<uint32_t> ns2{0}, nv2, nu2;
            for (uint32_t f = 0; f < m.numFaces(); ++f) {
                uint32_t keep = 0;
                for (uint32_t v : m.faceVertices(f)) keep += drop[v] ? 0 : 1;
                for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h)
                    if (!drop[m.faceVerts[h]] || keep < 3) {
                        nv2.push_back(m.faceVerts[h]);
                        if (uv) nu2.push_back(m.faceUVs[h]);
                    }
                ns2.push_back(uint32_t(nv2.size()));
            }
            m.faceStart = std::move(ns2);
            m.faceVerts = std::move(nv2);
            if (uv) m.faceUVs = std::move(nu2);
            m.removeUnusedVertices();
            m.buildTopology();
        }
    }
    if (uv) m.compactUVs();
    return true;
}

void deleteVertices(Mesh& m, const IndexList& verts)
{
    IndexList es;
    for (uint32_t v : verts)
        if (v < m.numVerts())
            for (uint32_t e : m.vertexEdges(v)) es.push_back(e);
    sortUnique(es);
    dissolveEdges(m, es, true);
}

uint32_t mergeToCenter(Mesh& m, const IndexList& verts)
{
    if (verts.size() < 2) return verts.empty() ? kInvalid : verts[0];
    Vec3 c;
    for (uint32_t v : verts) c += m.positions[v];
    c = c / float(verts.size());
    uint32_t target = *std::min_element(verts.begin(), verts.end());
    std::vector<uint32_t> remap(m.numVerts());
    std::iota(remap.begin(), remap.end(), 0u);
    for (uint32_t v : verts) remap[v] = target;
    m.positions[target] = c;
    remapFacesAndClean(m, remap);
    return target; // index may shift after compaction; callers reselect by position if needed
}

uint32_t mergeByDistance(Mesh& m, const IndexList& verts, float threshold)
{
    IndexList vs = verts;
    if (vs.empty()) { vs.resize(m.numVerts()); std::iota(vs.begin(), vs.end(), 0u); }
    const float cell = std::max(threshold, 1e-9f);
    auto key = [&](int64_t x, int64_t y, int64_t z) { return uint64_t((x * 73856093) ^ (y * 19349663) ^ (z * 83492791)); };
    std::unordered_multimap<uint64_t, uint32_t> grid;
    for (uint32_t v : vs) {
        const Vec3& p = m.positions[v];
        grid.emplace(key(int64_t(std::floor(p.x / cell)), int64_t(std::floor(p.y / cell)), int64_t(std::floor(p.z / cell))), v);
    }
    std::vector<uint32_t> remap(m.numVerts());
    std::iota(remap.begin(), remap.end(), 0u);
    std::vector<uint8_t> done(m.numVerts(), 0);
    uint32_t removed = 0;
    for (uint32_t v : vs) {
        if (done[v]) continue;
        done[v] = 1;
        const Vec3 p = m.positions[v];
        int64_t cx = int64_t(std::floor(p.x / cell)), cy = int64_t(std::floor(p.y / cell)), cz = int64_t(std::floor(p.z / cell));
        std::vector<uint32_t> cluster{v};
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    auto range = grid.equal_range(key(cx + dx, cy + dy, cz + dz));
                    for (auto it = range.first; it != range.second; ++it) {
                        uint32_t o = it->second;
                        if (!done[o] && length(m.positions[o] - p) <= threshold) { done[o] = 1; cluster.push_back(o); }
                    }
                }
        if (cluster.size() > 1) {
            Vec3 c;
            for (uint32_t o : cluster) c += m.positions[o];
            m.positions[v] = c / float(cluster.size());
            for (uint32_t o : cluster) remap[o] = v;
            removed += uint32_t(cluster.size() - 1);
        }
    }
    if (removed) remapFacesAndClean(m, remap);
    return removed;
}

void mergeInto(Mesh& m, uint32_t src, uint32_t dst)
{
    if (src == dst || src >= m.numVerts() || dst >= m.numVerts()) return;
    std::vector<uint32_t> remap(m.numVerts());
    std::iota(remap.begin(), remap.end(), 0u);
    remap[src] = dst;
    remapFacesAndClean(m, remap);
}

void setEdgesHard(Mesh& m, const IndexList& edgeList, bool hard)
{
    if (m.edgeHard.size() != m.edges.size()) m.edgeHard.assign(m.edges.size(), 0);
    if (edgeList.empty()) std::fill(m.edgeHard.begin(), m.edgeHard.end(), uint8_t(hard));
    else for (uint32_t e : edgeList) if (e < m.numEdges()) m.edgeHard[e] = hard;
    m.topologyId = nextVersionId();
    m.touchPositions();
}

void setNormalAngle(Mesh& m, const IndexList& edgeList, float degrees)
{
    if (m.edgeHard.size() != m.edges.size()) m.edgeHard.assign(m.edges.size(), 0);
    const float cosLimit = std::cos(radians(std::clamp(degrees, 0.0f, 180.0f)));
    auto apply = [&](uint32_t e) {
        uint32_t h = m.edges[e].he, t = m.heTwin[h];
        if (t == kInvalid) { m.edgeHard[e] = 0; return; }
        Vec3 a = m.faceNormal(m.heFace[h]), b = m.faceNormal(m.heFace[t]);
        m.edgeHard[e] = dot(a, b) < cosLimit ? 1 : 0;
    };
    if (edgeList.empty()) for (uint32_t e = 0; e < m.numEdges(); ++e) apply(e);
    else for (uint32_t e : edgeList) if (e < m.numEdges()) apply(e);
    m.topologyId = nextVersionId();
    m.touchPositions();
}

void collapseEdges(Mesh& m, const IndexList& edgeList)
{
    UnionFind uf(m.numVerts());
    for (uint32_t e : edgeList) uf.unite(m.edges[e].v0, m.edges[e].v1);
    std::vector<Vec3> sum(m.numVerts());
    std::vector<uint32_t> cnt(m.numVerts(), 0), remap(m.numVerts());
    for (uint32_t v = 0; v < m.numVerts(); ++v) {
        uint32_t r = uf.find(v);
        remap[v] = r;
        sum[r] += m.positions[v];
        cnt[r]++;
    }
    for (uint32_t v = 0; v < m.numVerts(); ++v)
        if (cnt[v] > 1) m.positions[v] = sum[v] / float(cnt[v]);
    remapFacesAndClean(m, remap);
}

// ---------------------------------------------------------------------------------------------
// Loops and rings

IndexList edgeLoop(const Mesh& m, uint32_t edge)
{
    IndexList out{edge};
    if (edge >= m.numEdges()) return {};
    std::vector<uint8_t> seen(m.numEdges(), 0);
    seen[edge] = 1;

    if (m.edges[edge].faceCount == 1) {
        // Border loop: follow border edges through each vertex.
        for (uint32_t startV : {m.edges[edge].v0, m.edges[edge].v1}) {
            uint32_t v = startV, cur = edge;
            while (true) {
                uint32_t next = kInvalid, borderCount = 0;
                for (uint32_t e : m.vertexEdges(v))
                    if (m.edges[e].faceCount == 1) {
                        ++borderCount;
                        if (e != cur) next = e;
                    }
                if (borderCount != 2 || next == kInvalid || seen[next]) break;
                seen[next] = 1;
                out.push_back(next);
                v = m.edgeOther(next, v);
                cur = next;
            }
        }
        return out;
    }

    uint32_t h0 = m.edges[edge].he;
    for (uint32_t start : {h0, m.heTwin[h0]}) {
        uint32_t h = start;
        while (h != kInvalid) {
            uint32_t b = m.heTo(h);
            if (m.vertexEdges(b).size() != 4 || m.isBorderVertex(b)) break;
            uint32_t n = m.heNext(h);
            uint32_t t = m.heTwin[n];
            if (t == kInvalid) break;
            uint32_t g = m.heNext(t);
            uint32_t e = m.heEdge[g];
            if (seen[e]) break;
            seen[e] = 1;
            out.push_back(e);
            h = g;
        }
    }
    return out;
}

IndexList edgeRing(const Mesh& m, uint32_t edge)
{
    if (edge >= m.numEdges()) return {};
    IndexList out{edge};
    std::vector<uint8_t> seen(m.numEdges(), 0);
    seen[edge] = 1;
    uint32_t h0 = m.edges[edge].he;
    for (uint32_t start : {h0, m.heTwin[h0]}) {
        uint32_t h = start;
        while (h != kInvalid && m.faceSize(m.heFace[h]) == 4) {
            uint32_t o = m.heNext(m.heNext(h));
            uint32_t e = m.heEdge[o];
            if (seen[e]) break;
            seen[e] = 1;
            out.push_back(e);
            h = m.heTwin[o];
        }
    }
    return out;
}

EdgeLoopPlan planEdgeLoop(const Mesh& m, uint32_t edge, float t)
{
    EdgeLoopPlan plan;
    if (edge >= m.numEdges()) return plan;
    t = std::clamp(t, 0.001f, 0.999f);
    std::vector<uint8_t> seenEdge(m.numEdges(), 0), seenFace(m.numFaces(), 0);
    auto addSplit = [&](uint32_t h, float tFromHeFrom) {
        uint32_t e = m.heEdge[h];
        if (seenEdge[e]) return;
        seenEdge[e] = 1;
        float te = m.heFrom(h) == m.edges[e].v0 ? tFromHeFrom : 1.0f - tFromHeFrom;
        plan.splits.push_back({e, te});
    };
    const uint32_t h0 = m.edges[edge].he;
    const float ta0 = m.heFrom(h0) == m.edges[edge].v0 ? t : 1.0f - t;
    addSplit(h0, ta0);
    // Walk h0's side, then the twin side with the fraction mirrored.
    struct Start { uint32_t h; float ta; };
    Start starts[2] = {{h0, ta0}, {m.heTwin[h0], 1.0f - ta0}};
    for (const Start& s : starts) {
        uint32_t h = s.h;
        float ta = s.ta;
        while (h != kInvalid) {
            uint32_t f = m.heFace[h];
            if (seenFace[f] || m.faceSize(f) != 4) break;
            seenFace[f] = 1;
            uint32_t o = m.heNext(m.heNext(h));
            addSplit(o, 1.0f - ta);
            plan.cuts.push_back({f, m.heEdge[h], m.heEdge[o]});
            h = m.heTwin[o];
            // Crossing into the neighbour flips direction: the point is at ta from twin's from-vertex.
        }
    }
    return plan;
}

IndexList insertEdgeLoop(Mesh& m, const EdgeLoopPlan& plan)
{
    if (!plan.valid()) return {};
    std::vector<uint32_t> splitVert(m.numEdges(), kInvalid);
    struct CreaseCarry { uint32_t a, m, b; float s; bool hard; };
    std::vector<CreaseCarry> carry;
    for (const auto& s : plan.splits) {
        const MeshEdge& e = m.edges[s.edge];
        splitVert[s.edge] = m.addVertex(lerp(m.positions[e.v0], m.positions[e.v1], s.t));
        if (m.edgeCrease[s.edge] > 0 || m.isHard(s.edge)) carry.push_back({e.v0, splitVert[s.edge], e.v1, m.edgeCrease[s.edge], m.isHard(s.edge)});
    }
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> cutOf;
    for (const auto& c : plan.cuts) cutOf[c.face] = {c.e0, c.e1};

    const bool uv = m.hasUVs();
    std::unordered_map<uint32_t, float> splitT;
    for (const auto& sp : plan.splits) splitT[sp.edge] = sp.t;
    std::vector<uint32_t> ns{0}, nv, nu;
    std::vector<std::pair<uint32_t, uint32_t>> newEdges;
    std::vector<uint32_t> poly, polyUV;
    for (uint32_t f = 0; f < m.numFaces(); ++f) {
        poly.clear();
        polyUV.clear();
        for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
            poly.push_back(m.faceVerts[h]);
            if (uv) polyUV.push_back(m.faceUVs[h]);
            uint32_t e = m.heEdge[h];
            uint32_t sv = splitVert[e];
            if (sv != kInvalid) {
                poly.push_back(sv);
                if (uv) {
                    float t = splitT[e];
                    if (m.heFrom(h) != m.edges[e].v0) t = 1.0f - t;
                    polyUV.push_back(uint32_t(m.uvs.size()));
                    m.uvs.push_back(lerp(m.cornerUV(h), m.cornerUV(m.heNext(h)), t));
                }
            }
        }
        auto emit = [&](size_t k) { nv.push_back(poly[k]); if (uv) nu.push_back(polyUV[k]); };
        auto it = cutOf.find(f);
        if (it == cutOf.end()) {
            for (size_t k = 0; k < poly.size(); ++k) emit(k);
            ns.push_back(uint32_t(nv.size()));
            continue;
        }
        uint32_t sa = splitVert[it->second.first], sb = splitVert[it->second.second];
        size_t i = std::find(poly.begin(), poly.end(), sa) - poly.begin();
        size_t j = std::find(poly.begin(), poly.end(), sb) - poly.begin();
        const size_t n = poly.size();
        for (size_t k = i;; k = (k + 1) % n) { emit(k); if (k == j) break; }
        ns.push_back(uint32_t(nv.size()));
        for (size_t k = j;; k = (k + 1) % n) { emit(k); if (k == i) break; }
        ns.push_back(uint32_t(nv.size()));
        newEdges.push_back({sa, sb});
    }
    m.faceStart = std::move(ns);
    m.faceVerts = std::move(nv);
    if (uv) { m.faceUVs = std::move(nu); m.weldUVs(); }
    m.buildTopology();
    for (const auto& c : carry) {
        if (c.s > 0) { m.setCrease(c.a, c.m, c.s); m.setCrease(c.m, c.b, c.s); }
        if (c.hard) { m.setHard(c.a, c.m, true); m.setHard(c.m, c.b, true); }
    }
    IndexList out;
    for (auto [a, b] : newEdges) {
        uint32_t e = m.findEdge(a, b);
        if (e != kInvalid) out.push_back(e);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------

IndexList fillHoles(Mesh& m, const IndexList& borderEdges)
{
    std::vector<uint8_t> want(m.numEdges(), borderEdges.empty() ? 1 : 0);
    for (uint32_t e : borderEdges)
        if (e < m.numEdges()) want[e] = 1;
    std::vector<uint8_t> used(m.numHalfEdges(), 0);
    std::vector<std::vector<uint32_t>> loops;
    for (uint32_t e = 0; e < m.numEdges(); ++e) {
        if (!want[e] || m.edges[e].faceCount != 1) continue;
        uint32_t h = m.edges[e].he;
        if (used[h]) continue;
        std::vector<uint32_t> loop;
        uint32_t cur = h;
        bool ok = true;
        while (true) {
            used[cur] = 1;
            loop.push_back(m.heFrom(cur));
            uint32_t b = m.heTo(cur), nxt = kInvalid;
            for (uint32_t oe : m.vertexEdges(b)) {
                if (m.edges[oe].faceCount != 1) continue;
                uint32_t oh = m.edges[oe].he;
                if (m.heFrom(oh) == b && !used[oh]) { nxt = oh; break; }
                if (oh == h && m.heFrom(oh) == b) { nxt = oh; break; }
            }
            if (nxt == h) break;
            if (nxt == kInvalid || loop.size() > m.numHalfEdges()) { ok = false; break; }
            cur = nxt;
        }
        if (ok && loop.size() >= 3) loops.push_back(std::move(loop));
    }
    IndexList out;
    const auto any = anyCornerUV(m);
    for (auto& l : loops) {
        std::reverse(l.begin(), l.end());
        if (m.hasUVs()) {
            std::vector<Vec2> u;
            for (uint32_t v : l) u.push_back(any[v]);
            out.push_back(m.addFaceWithUVs(l, u));
        } else {
            out.push_back(m.addFace(l));
        }
    }
    if (m.hasUVs()) m.weldUVs();
    m.buildTopology();
    return out;
}

void reverseFaces(Mesh& m, const IndexList& faces)
{
    if (faces.empty())
        for (uint32_t f = 0; f < m.numFaces(); ++f) m.reverseFace(f);
    else
        for (uint32_t f : faces) m.reverseFace(f);
    m.buildTopology();
}

void triangulate(Mesh& m, const IndexList& faces)
{
    auto sel = faces.empty() ? std::vector<uint8_t>(m.numFaces(), 1) : flags(m.numFaces(), faces);
    const bool uv = m.hasUVs();
    std::vector<uint32_t> ns{0}, nv, nu;
    for (uint32_t f = 0; f < m.numFaces(); ++f) {
        const uint32_t b = m.faceStart[f], n = m.faceSize(f);
        auto put = [&](uint32_t h) { nv.push_back(m.faceVerts[h]); if (uv) nu.push_back(m.faceUVs[h]); };
        if (!sel[f] || n <= 3) {
            for (uint32_t h = b; h < b + n; ++h) put(h);
            ns.push_back(uint32_t(nv.size()));
            continue;
        }
        for (uint32_t i = 1; i + 1 < n; ++i) {
            put(b); put(b + i); put(b + i + 1);
            ns.push_back(uint32_t(nv.size()));
        }
    }
    m.faceStart = std::move(ns);
    m.faceVerts = std::move(nv);
    if (uv) m.faceUVs = std::move(nu);
    m.buildTopology();
}

void mirror(Mesh& m, int axis, float mergeThreshold)
{
    const uint32_t V = m.numVerts(), F = m.numFaces();
    for (uint32_t v = 0; v < V; ++v)
        if (std::abs(m.positions[v][axis]) <= mergeThreshold) m.positions[v][axis] = 0.0f;
    std::vector<uint32_t> mirrorOf(V);
    for (uint32_t v = 0; v < V; ++v) {
        if (m.positions[v][axis] == 0.0f) { mirrorOf[v] = v; continue; }
        Vec3 p = m.positions[v];
        p[axis] = -p[axis];
        mirrorOf[v] = m.addVertex(p);
    }
    const bool uv = m.hasUVs();
    const uint32_t uvBase = uint32_t(m.uvs.size());
    if (uv) {
        std::vector<Vec2> copy = m.uvs; // mirrored half reuses (overlapping) uvs
        m.uvs.insert(m.uvs.end(), copy.begin(), copy.end());
    }
    std::vector<uint32_t> poly;
    for (uint32_t f = 0; f < F; ++f) {
        poly.clear();
        for (uint32_t h = m.faceStart[f + 1]; h-- > m.faceStart[f];) {
            poly.push_back(mirrorOf[m.faceVerts[h]]);
            if (uv) m.faceUVs.push_back(m.faceUVs[h] + uvBase);
        }
        m.faceVerts.insert(m.faceVerts.end(), poly.begin(), poly.end());
        m.faceStart.push_back(uint32_t(m.faceVerts.size()));
    }
    if (uv) m.weldUVs();
    // Mirror creases and hard edges.
    std::vector<std::tuple<uint32_t, uint32_t, float, bool>> cr;
    for (uint32_t e = 0; e < m.numEdges(); ++e)
        if (m.edgeCrease[e] > 0 || m.isHard(e)) cr.push_back({mirrorOf[m.edges[e].v0], mirrorOf[m.edges[e].v1], m.edgeCrease[e], m.isHard(e)});
    m.buildTopology();
    for (auto [a, b, s, hd] : cr) {
        if (s > 0) m.setCrease(a, b, s);
        if (hd) m.setHard(a, b, true);
    }
}

std::vector<Mesh> separate(const Mesh& m)
{
    UnionFind uf(m.numVerts());
    for (uint32_t f = 0; f < m.numFaces(); ++f) {
        auto fv = m.faceVertices(f);
        for (size_t i = 1; i < fv.size(); ++i) uf.unite(fv[0], fv[i]);
    }
    std::unordered_map<uint32_t, uint32_t> compOf;
    std::vector<Mesh> out;
    std::vector<std::vector<uint32_t>> localIndex;
    std::vector<uint32_t> local(m.numVerts(), kInvalid);
    const bool uv = m.hasUVs();
    for (uint32_t f = 0; f < m.numFaces(); ++f) {
        uint32_t root = uf.find(m.faceVerts[m.faceStart[f]]);
        auto [it, inserted] = compOf.emplace(root, uint32_t(out.size()));
        if (inserted) out.emplace_back();
        Mesh& dst = out[it->second];
        std::vector<uint32_t> poly;
        std::vector<Vec2> polyUV;
        for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
            uint32_t v = m.faceVerts[h];
            if (local[v] == kInvalid) local[v] = dst.addVertex(m.positions[v]);
            poly.push_back(local[v]);
            if (uv) polyUV.push_back(m.cornerUV(h));
        }
        if (uv) dst.addFaceWithUVs(poly, polyUV);
        else dst.addFace(poly);
    }
    for (Mesh& part : out) {
        if (uv) part.weldUVs();
        part.buildTopology();
    }
    // Carry creases.
    for (uint32_t e = 0; e < m.numEdges(); ++e) {
        if (m.edgeCrease[e] <= 0 && !m.isHard(e)) continue;
        uint32_t a = m.edges[e].v0, b = m.edges[e].v1;
        auto it = compOf.find(uf.find(a));
        if (it == compOf.end()) continue;
        if (m.edgeCrease[e] > 0) out[it->second].setCrease(local[a], local[b], m.edgeCrease[e]);
        if (m.isHard(e)) out[it->second].setHard(local[a], local[b], true);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Component conversion

IndexList convert(const Mesh& m, Comp from, const IndexList& items, Comp to)
{
    IndexList out;
    if (from == to) { out = items; sortUnique(out); return out; }
    switch (from) {
    case Comp::Vertex:
        for (uint32_t v : items) {
            if (v >= m.numVerts()) continue;
            if (to == Comp::Edge) for (uint32_t e : m.vertexEdges(v)) out.push_back(e);
            else for (uint32_t f : m.vertexFaces(v)) out.push_back(f);
        }
        if (to == Comp::Edge) {
            // Maya-style: only edges whose both ends are selected (when that yields anything).
            auto vs = flags(m.numVerts(), items);
            IndexList inner;
            for (uint32_t e : out)
                if (vs[m.edges[e].v0] && vs[m.edges[e].v1]) inner.push_back(e);
            if (!inner.empty()) out = std::move(inner);
        }
        break;
    case Comp::Edge:
        for (uint32_t e : items) {
            if (e >= m.numEdges()) continue;
            if (to == Comp::Vertex) { out.push_back(m.edges[e].v0); out.push_back(m.edges[e].v1); }
            else {
                uint32_t h = m.edges[e].he;
                out.push_back(m.heFace[h]);
                if (m.heTwin[h] != kInvalid) out.push_back(m.heFace[m.heTwin[h]]);
            }
        }
        break;
    case Comp::Face:
        for (uint32_t f : items) {
            if (f >= m.numFaces()) continue;
            for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h)
                out.push_back(to == Comp::Vertex ? m.faceVerts[h] : m.heEdge[h]);
        }
        break;
    }
    sortUnique(out);
    return out;
}

IndexList grow(const Mesh& m, Comp type, const IndexList& items)
{
    IndexList verts = convert(m, type, items, Comp::Vertex);
    IndexList out = items;
    if (type == Comp::Vertex) {
        for (uint32_t v : verts)
            for (uint32_t e : m.vertexEdges(v)) out.push_back(m.edgeOther(e, v));
    } else if (type == Comp::Edge) {
        for (uint32_t v : verts)
            for (uint32_t e : m.vertexEdges(v)) out.push_back(e);
    } else {
        for (uint32_t v : verts)
            for (uint32_t f : m.vertexFaces(v)) out.push_back(f);
    }
    sortUnique(out);
    return out;
}

IndexList shrink(const Mesh& m, Comp type, const IndexList& items)
{
    // Keep items whose every neighbour (via shared vertices) is also selected.
    auto sel = flags(type == Comp::Vertex ? m.numVerts() : type == Comp::Edge ? m.numEdges() : m.numFaces(), items);
    IndexList out;
    for (uint32_t i : items) {
        bool interior = true;
        IndexList vs = convert(m, type, {i}, Comp::Vertex);
        for (uint32_t v : vs) {
            if (type == Comp::Vertex) {
                for (uint32_t e : m.vertexEdges(v)) interior &= bool(sel[m.edgeOther(e, v)]);
                interior &= !m.isBorderVertex(v);
            } else if (type == Comp::Edge) {
                for (uint32_t e : m.vertexEdges(v)) interior &= bool(sel[e]);
            } else {
                for (uint32_t f : m.vertexFaces(v)) interior &= bool(sel[f]);
                interior &= !m.isBorderVertex(v);
            }
        }
        if (interior) out.push_back(i);
    }
    return out;
}

} // namespace facet::ops
