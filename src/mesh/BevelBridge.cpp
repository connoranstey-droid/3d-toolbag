// Bevel, Bridge, edge/face splitting and Multi-Cut.
#include "mesh/MeshOps.h"

#include <algorithm>
#include <map>
#include <unordered_map>

namespace facet::ops {

namespace {

// Collects faces as (vertex, uv value) polygons, then writes them back in one go.
struct FaceBuilder {
    std::vector<uint32_t> start{0}, verts;
    std::vector<Vec2> uv;
    void add(const std::vector<uint32_t>& v, const std::vector<Vec2>& u)
    {
        verts.insert(verts.end(), v.begin(), v.end());
        for (size_t i = 0; i < v.size(); ++i) uv.push_back(i < u.size() ? u[i] : Vec2{});
        start.push_back(uint32_t(verts.size()));
    }
    uint32_t count() const { return uint32_t(start.size() - 1); }
    void commit(Mesh& m, bool withUV)
    {
        m.faceStart = std::move(start);
        m.faceVerts = std::move(verts);
        if (withUV) {
            m.uvs = std::move(uv);
            m.faceUVs.resize(m.faceVerts.size());
            for (uint32_t i = 0; i < m.faceUVs.size(); ++i) m.faceUVs[i] = i;
        } else {
            m.uvs.clear();
            m.faceUVs.clear();
        }
    }
};

Vec3 bezier(const Vec3& a, const Vec3& c, const Vec3& b, float t)
{
    float u = 1 - t;
    return a * (u * u) + c * (2 * u * t) + b * (t * t);
}

uint64_t key2(uint32_t a, uint32_t b) { return (uint64_t(a) << 32) | b; }

} // namespace

// ------------------------------------------------------------------------------------------
// Bevel

IndexList bevelEdges(Mesh& m, const IndexList& edgeList, const BevelOptions& opt)
{
    const float d = opt.offset;
    const int segs = std::max(1, opt.segments);
    if (d <= 0) return {};
    std::vector<uint8_t> bev(m.numEdges(), 0);
    bool any = false;
    for (uint32_t e : edgeList)
        if (e < m.numEdges() && m.edges[e].faceCount == 2 && m.heTwin[m.edges[e].he] != kInvalid) { bev[e] = 1; any = true; }
    if (!any) return {};

    const bool uv = m.hasUVs();
    const std::vector<Vec3> P = m.positions; // original positions
    const uint32_t V0 = m.numVerts();
    std::vector<uint8_t> affected(m.numVerts(), 0);
    for (uint32_t e = 0; e < m.numEdges(); ++e)
        if (bev[e]) affected[m.edges[e].v0] = affected[m.edges[e].v1] = 1;
    auto edgeLen = [&](uint32_t e) { return length(P[m.edges[e].v0] - P[m.edges[e].v1]); };
    auto maxSlide = [&](uint32_t e) {
        const auto& ed = m.edges[e];
        bool both = affected[ed.v0] && affected[ed.v1];
        return edgeLen(e) * (both ? 0.48f : 0.92f);
    };
    auto cuv = [&](uint32_t h) { return uv ? m.cornerUV(h) : Vec2{}; };

    // 1. New point per corner touching a bevelled edge.
    const uint32_t H = m.numHalfEdges();
    std::vector<uint32_t> cornerPt(H, kInvalid);
    std::vector<Vec2> cornerUVv(H);
    struct Slide { uint32_t idx; Vec3 dir; float sum; int n; };
    std::unordered_map<uint64_t, Slide> slides; // key (edge, vertex)
    auto slideAt = [&](uint32_t e, uint32_t v) -> Slide* {
        auto it = slides.find(key2(e, v));
        return it == slides.end() ? nullptr : &it->second;
    };
    for (uint32_t h = 0; h < H; ++h) {
        const uint32_t v = m.faceVerts[h];
        if (!affected[v]) continue;
        const uint32_t hp = m.hePrev(h), hn = m.heNext(h);
        const uint32_t next = m.faceVerts[hn], prev = m.faceVerts[hp];
        const uint32_t eout = m.heEdge[h], ein = m.heEdge[hp];
        Vec3 uo = P[next] - P[v], ui = P[prev] - P[v];
        float lo = length(uo), li = length(ui);
        if (lo < 1e-12f || li < 1e-12f) { cornerPt[h] = v; cornerUVv[h] = cuv(h); continue; }
        uo = uo / lo;
        ui = ui / li;
        float sinT = std::max(length(cross(uo, ui)), 0.05f);
        const bool bo = bev[eout], bi = bev[ein];
        if (!bo && !bi) {
            cornerPt[h] = v;
            cornerUVv[h] = cuv(h);
        } else if (bo && bi) {
            // Point at distance d from both bevelled edges, inside the face.
            const float cosT = dot(uo, ui);
            const float rawSin = length(cross(uo, ui));
            Vec3 np;
            float a, b;
            if (rawSin < 0.2f && cosT < 0) {
                // Nearly collinear edges: offset straight into the face.
                Vec3 fn = m.faceNormal(m.heFace[h]);
                Vec3 inward = normalize(cross(fn, uo));
                np = P[v] + inward * std::min(d, 0.48f * std::min(lo, li));
                a = dot(np - P[v], uo);
                b = dot(np - P[v], ui);
            } else {
                // p = v + t (uo + ui), t = d / sin(theta). The travel along each edge is t (1 + cos theta);
                // clamp that (not t itself) so near-straight corners keep their full inset.
                float t = d / sinT;
                const float travel = std::max(1e-6f, 1.0f + cosT);
                t = std::min({t, lo * 0.48f / travel, li * 0.48f / travel});
                a = b = t;
                np = P[v] + (uo + ui) * t;
            }
            cornerPt[h] = m.addVertex(np);
            cornerUVv[h] = cuv(h) + (cuv(hn) - cuv(h)) * (std::max(a, 0.0f) / lo) + (cuv(hp) - cuv(h)) * (std::max(b, 0.0f) / li);
        } else {
            // Slide along the single non-bevelled edge of this corner.
            const uint32_t se = bo ? ein : eout;
            const Vec3 dir = bo ? ui : uo;
            const float len = bo ? li : lo;
            float dist = std::min(d / sinT, maxSlide(se));
            auto [it, inserted] = slides.try_emplace(key2(se, v), Slide{0, dir, 0.0f, 0});
            if (inserted) it->second.idx = m.addVertex(P[v]);
            it->second.sum += dist;
            it->second.n += 1;
            cornerPt[h] = it->second.idx;
            const uint32_t other = bo ? hp : hn;
            cornerUVv[h] = cuv(h) + (cuv(other) - cuv(h)) * (dist / len);
        }
    }
    for (auto& [k, sl] : slides) m.positions[sl.idx] = P[uint32_t(k & 0xffffffffu)] + sl.dir * (sl.sum / float(sl.n));

    // 2. Profile points between two corner points around vertex v (shared by strips and caps).
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::vector<uint32_t>> profiles;
    std::unordered_map<uint32_t, Vec2> pointUV;
    auto profile = [&](uint32_t pa, uint32_t pb, uint32_t v) {
        // Returns interior points ordered from pa to pb.
        uint32_t lo = std::min(pa, pb), hi = std::max(pa, pb);
        auto& mids = profiles[{lo, hi, v}];
        if (mids.empty() && segs > 1) {
            for (int k = 1; k < segs; ++k) {
                float t = float(k) / segs;
                Vec3 p = opt.round ? bezier(m.positions[lo], P[v], m.positions[hi], t) : lerp(m.positions[lo], m.positions[hi], t);
                mids.push_back(m.addVertex(p));
            }
        }
        std::vector<uint32_t> out = mids;
        if (pa != lo) std::reverse(out.begin(), out.end());
        return out;
    };

    // Corner of face f at vertex v on edge e (the corner whose half-edge touches v).
    auto cornerOnEdge = [&](uint32_t h, uint32_t v) { return m.faceVerts[h] == v ? h : m.heNext(h); };

    // 3. Rotation around each affected vertex.
    auto rotNext = [&](uint32_t c) { uint32_t t = m.heTwin[m.hePrev(c)]; return t; };
    auto rotPrev = [&](uint32_t c) { uint32_t t = m.heTwin[c]; return t == kInvalid ? kInvalid : m.heNext(t); };
    std::vector<uint32_t> someCorner(V0, kInvalid);
    for (uint32_t h = 0; h < H; ++h) someCorner[m.faceVerts[h]] = h;

    struct Ring { std::vector<uint32_t> corners; bool closed = true; };
    auto ringOf = [&](uint32_t v) {
        Ring r;
        uint32_t start = someCorner[v];
        if (start == kInvalid) return r;
        uint32_t c = start;
        size_t guard = 0;
        while (true) { // walk back to a border (or around)
            uint32_t p = rotPrev(c);
            if (p == kInvalid) { r.closed = false; start = c; break; }
            if (p == start || ++guard > 64) break;
            c = p;
        }
        c = start;
        guard = 0;
        do {
            r.corners.push_back(c);
            c = rotNext(c);
        } while (c != kInvalid && c != start && ++guard < 64);
        if (c == kInvalid) r.closed = false;
        return r;
    };

    // Cap sequence around v: points in rotation order including profile points across bevelled edges.
    std::vector<uint8_t> splice(V0, 0);
    std::vector<std::vector<uint32_t>> capSeq(V0);
    std::vector<uint32_t> spliceCorner(V0, kInvalid);
    for (uint32_t v = 0; v < V0; ++v) {
        if (!affected[v]) continue;
        Ring r = ringOf(v);
        std::vector<uint32_t> seq;
        uint32_t anchors = 0, anchorCorner = kInvalid;
        for (size_t i = 0; i < r.corners.size(); ++i) {
            uint32_t c = r.corners[i];
            uint32_t pt = cornerPt[c];
            if (pt == v) { ++anchors; anchorCorner = c; }
            seq.push_back(pt);
            if (pointUV.find(pt) == pointUV.end()) pointUV[pt] = cornerUVv[c];
            bool last = i + 1 == r.corners.size();
            if (last && !r.closed) break;
            uint32_t nc = last ? r.corners[0] : r.corners[i + 1];
            uint32_t x = m.heEdge[m.hePrev(c)];
            if (bev[x]) {
                auto mids = profile(pt, cornerPt[nc], v);
                seq.insert(seq.end(), mids.begin(), mids.end());
            }
        }
        // Remove consecutive duplicates (merged slide points).
        std::vector<uint32_t> clean;
        for (uint32_t p : seq)
            if (clean.empty() || clean.back() != p) clean.push_back(p);
        while (clean.size() > 1 && clean.front() == clean.back()) clean.pop_back();
        if (anchors == 1 && r.closed) {
            uint32_t c = anchorCorner;
            if (slideAt(m.heEdge[m.hePrev(c)], v) && slideAt(m.heEdge[c], v)) {
                splice[v] = 1;
                spliceCorner[v] = c;
                // Rotate so the sequence starts at v; the path is everything after it.
                auto it = std::find(clean.begin(), clean.end(), v);
                std::rotate(clean.begin(), it, clean.end());
            }
        }
        capSeq[v] = std::move(clean);
        if (!r.closed && anchors == 0) capSeq[v].clear(); // open border fan: no cap needed
    }

    // 4. Rebuild faces.
    FaceBuilder fb;
    for (uint32_t f = 0; f < m.numFaces(); ++f) {
        std::vector<uint32_t> poly;
        std::vector<Vec2> pu;
        for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
            const uint32_t v = m.faceVerts[h];
            if (!affected[v] || cornerPt[h] != v) {
                poly.push_back(affected[v] ? cornerPt[h] : v);
                pu.push_back(affected[v] ? cornerUVv[h] : cuv(h));
                continue;
            }
            // Anchor corner: weave in slide points that landed on its edges.
            const uint32_t hp = m.hePrev(h), hn = m.heNext(h);
            Slide* sIn = slideAt(m.heEdge[hp], v);
            Slide* sOut = slideAt(m.heEdge[h], v);
            auto slideUV = [&](Slide* s, uint32_t other) {
                float len = length(P[m.faceVerts[other]] - P[v]);
                return cuv(h) + (cuv(other) - cuv(h)) * (len > 0 ? (s->sum / s->n) / len : 0.0f);
            };
            if (splice[v] && spliceCorner[v] == h) {
                const auto& path = capSeq[v]; // starts with v
                for (size_t i = 1; i < path.size(); ++i) {
                    poly.push_back(path[i]);
                    if (i == 1) pu.push_back(slideUV(sIn, hp));
                    else if (i + 1 == path.size()) pu.push_back(slideUV(sOut, hn));
                    else pu.push_back(cuv(h));
                }
                continue;
            }
            if (sIn) { poly.push_back(sIn->idx); pu.push_back(slideUV(sIn, hp)); }
            poly.push_back(v);
            pu.push_back(cuv(h));
            if (sOut) { poly.push_back(sOut->idx); pu.push_back(slideUV(sOut, hn)); }
        }
        fb.add(poly, pu);
    }
    IndexList created;
    // Edge strips.
    for (uint32_t e = 0; e < m.numEdges(); ++e) {
        if (!bev[e]) continue;
        const uint32_t h = m.edges[e].he, t = m.heTwin[h];
        const uint32_t a = m.faceVerts[h], b = m.faceVerts[m.heNext(h)];
        const uint32_t a1 = cornerOnEdge(h, a), a2 = cornerOnEdge(t, a), b1 = cornerOnEdge(h, b), b2 = cornerOnEdge(t, b);
        std::vector<uint32_t> Ra{cornerPt[a1]}, Rb{cornerPt[b1]};
        std::vector<Vec2> Ua{cornerUVv[a1]}, Ub{cornerUVv[b1]};
        auto ma = profile(cornerPt[a1], cornerPt[a2], a), mb = profile(cornerPt[b1], cornerPt[b2], b);
        for (int k = 1; k < segs; ++k) {
            float tt = float(k) / segs;
            Ra.push_back(ma[size_t(k - 1)]);
            Rb.push_back(mb[size_t(k - 1)]);
            Ua.push_back(lerp(cornerUVv[a1], cornerUVv[a2], tt));
            Ub.push_back(lerp(cornerUVv[b1], cornerUVv[b2], tt));
        }
        Ra.push_back(cornerPt[a2]);
        Rb.push_back(cornerPt[b2]);
        Ua.push_back(cornerUVv[a2]);
        Ub.push_back(cornerUVv[b2]);
        for (int k = 0; k < segs; ++k) {
            created.push_back(fb.count());
            fb.add({Rb[size_t(k)], Ra[size_t(k)], Ra[size_t(k + 1)], Rb[size_t(k + 1)]},
                   {Ub[size_t(k)], Ua[size_t(k)], Ua[size_t(k + 1)], Ub[size_t(k + 1)]});
        }
    }
    // Corner caps.
    for (uint32_t v = 0; v < V0; ++v) {
        if (!affected[v] || splice[v]) continue;
        const auto& seq = capSeq[v];
        if (seq.size() < 3) continue;
        std::vector<uint32_t> sorted = seq;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) continue; // degenerate (pass-through)
        std::vector<Vec2> u;
        for (uint32_t p : seq) u.push_back(pointUV.count(p) ? pointUV[p] : Vec2{});
        created.push_back(fb.count());
        fb.add(seq, u);
    }
    fb.commit(m, uv);
    m.removeUnusedVertices();
    if (uv) m.weldUVs(1e-5f);
    m.buildTopology();
    // Like Maya's polyBevel smoothing angle: edges sharper than 30 degrees go hard. For rounded
    // multi-segment bevels the whole bevel surface (strips + corner patches) shades smoothly and only
    // its boundary with the original faces may be hard.
    {
        std::vector<uint8_t> isNew(m.numFaces(), 0);
        for (uint32_t f : created) isNew[f] = 1;
        const bool rounded = opt.round && segs > 1;
        const float cosLimit = std::cos(radians(30.0f));
        if (m.edgeHard.size() != m.edges.size()) m.edgeHard.assign(m.edges.size(), 0);
        for (uint32_t f : created)
            for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
                uint32_t e = m.heEdge[h], t = m.heTwin[h];
                if (t == kInvalid) continue;
                const uint32_t g = m.heFace[t];
                const bool sharp = dot(m.faceNormal(f), m.faceNormal(g)) < cosLimit;
                m.edgeHard[e] = sharp && (!rounded || !isNew[g]) ? 1 : 0;
            }
        m.topologyId = nextVersionId();
    }
    return created;
}

// ------------------------------------------------------------------------------------------
// Bridge

namespace {
struct Chain { std::vector<uint32_t> verts; bool closed = false; };

std::vector<Chain> borderChains(const Mesh& m, const IndexList& edges)
{
    std::vector<uint8_t> sel(m.numEdges(), 0);
    for (uint32_t e : edges)
        if (e < m.numEdges() && m.edges[e].faceCount == 1) sel[e] = 1;
    // Border half-edge per selected edge; index by from-vertex.
    std::unordered_map<uint32_t, uint32_t> outOf, inOf;
    for (uint32_t e = 0; e < m.numEdges(); ++e)
        if (sel[e]) {
            uint32_t h = m.edges[e].he;
            outOf[m.heFrom(h)] = h;
            inOf[m.heTo(h)] = h;
        }
    std::vector<uint8_t> used(m.numEdges(), 0);
    std::vector<Chain> chains;
    for (uint32_t e = 0; e < m.numEdges(); ++e) {
        if (!sel[e] || used[e]) continue;
        // Walk back to the chain start.
        uint32_t h = m.edges[e].he, start = h;
        size_t guard = 0;
        while (inOf.count(m.heFrom(h)) && ++guard < m.numHalfEdges()) {
            uint32_t p = inOf[m.heFrom(h)];
            if (p == start) break;
            h = p;
        }
        Chain c;
        uint32_t first = h;
        while (true) {
            used[m.heEdge[h]] = 1;
            c.verts.push_back(m.heFrom(h));
            auto it = outOf.find(m.heTo(h));
            if (it == outOf.end()) { c.verts.push_back(m.heTo(h)); break; }
            if (it->second == first) { c.closed = true; break; }
            if (used[m.heEdge[it->second]]) { c.verts.push_back(m.heTo(h)); break; }
            h = it->second;
        }
        chains.push_back(std::move(c));
    }
    return chains;
}
} // namespace

IndexList bridgeEdges(Mesh& m, const IndexList& borderEdges, int divisions)
{
    auto chains = borderChains(m, borderEdges);
    if (chains.size() != 2 || chains[0].closed != chains[1].closed || chains[0].verts.size() != chains[1].verts.size()) return {};
    const auto& A = chains[0].verts;
    const auto& C = chains[1].verts;
    const size_t n = A.size();
    const bool closed = chains[0].closed;
    // map[i] = index into C matched with A[i]; orientation is forced by border directions (C runs backwards).
    std::vector<uint32_t> map(n);
    if (closed) {
        size_t bestS = 0;
        float best = 1e30f;
        for (size_t s = 0; s < n; ++s) {
            float sum = 0;
            for (size_t i = 0; i < n; ++i) sum += length2(m.positions[A[i]] - m.positions[C[(s + n - i) % n]]);
            if (sum < best) { best = sum; bestS = s; }
        }
        for (size_t i = 0; i < n; ++i) map[i] = uint32_t((bestS + n - i) % n);
    } else {
        for (size_t i = 0; i < n; ++i) map[i] = uint32_t(n - 1 - i);
    }
    const auto any = [&] {
        std::vector<Vec2> out(m.numVerts());
        if (m.hasUVs())
            for (uint32_t h = 0; h < m.numHalfEdges(); ++h) out[m.faceVerts[h]] = m.cornerUV(h);
        return out;
    }();
    // Rings: ring 0 = A, ring D+1 = C (matched), intermediate rings interpolated.
    const int D = std::max(0, divisions);
    std::vector<std::vector<uint32_t>> rings(size_t(D + 2), std::vector<uint32_t>(n));
    std::vector<std::vector<Vec2>> ringUV(size_t(D + 2), std::vector<Vec2>(n));
    for (size_t i = 0; i < n; ++i) {
        rings[0][i] = A[i];
        rings[size_t(D + 1)][i] = C[map[i]];
        ringUV[0][i] = any[A[i]];
        ringUV[size_t(D + 1)][i] = any[C[map[i]]];
        for (int r = 1; r <= D; ++r) {
            float t = float(r) / (D + 1);
            rings[size_t(r)][i] = m.addVertex(lerp(m.positions[A[i]], m.positions[C[map[i]]], t));
            ringUV[size_t(r)][i] = lerp(ringUV[0][i], ringUV[size_t(D + 1)][i], t);
        }
    }
    IndexList created;
    const size_t segs = closed ? n : n - 1;
    for (int r = 0; r <= D; ++r)
        for (size_t i = 0; i < segs; ++i) {
            size_t j = (i + 1) % n;
            const auto& R0 = rings[size_t(r)];
            const auto& R1 = rings[size_t(r + 1)];
            const uint32_t q[4] = {R0[j], R0[i], R1[i], R1[j]};
            const Vec2 u[4] = {ringUV[size_t(r)][j], ringUV[size_t(r)][i], ringUV[size_t(r + 1)][i], ringUV[size_t(r + 1)][j]};
            if (m.hasUVs()) created.push_back(m.addFaceWithUVs(q, u));
            else created.push_back(m.addFace(std::span<const uint32_t>(q, 4)));
        }
    if (m.hasUVs()) m.weldUVs();
    m.buildTopology();
    return created;
}

IndexList bridgeFaces(Mesh& m, const IndexList& faces, int divisions)
{
    std::vector<uint8_t> sel(m.numFaces(), 0);
    for (uint32_t f : faces)
        if (f < m.numFaces()) sel[f] = 1;
    std::vector<std::pair<uint32_t, uint32_t>> boundary;
    for (uint32_t f : faces)
        for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
            uint32_t t = m.heTwin[h];
            if (t != kInvalid && !sel[m.heFace[t]]) boundary.push_back({m.heFrom(h), m.heTo(h)});
        }
    // Delete the faces, keeping a vertex remap so the boundary pairs can be found again.
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
    std::vector<uint32_t> remap;
    m.removeUnusedVertices(&remap);
    m.buildTopology();
    IndexList edges;
    for (auto [a, b] : boundary) {
        if (remap[a] == kInvalid || remap[b] == kInvalid) continue;
        uint32_t e = m.findEdge(remap[a], remap[b]);
        if (e != kInvalid) edges.push_back(e);
    }
    return bridgeEdges(m, edges, divisions);
}

// ------------------------------------------------------------------------------------------
// Splitting / Multi-Cut

uint32_t splitEdge(Mesh& m, uint32_t a, uint32_t b, float t)
{
    uint32_t e = m.findEdge(a, b);
    if (e == kInvalid) return kInvalid;
    const bool uv = m.hasUVs();
    const uint32_t nv = m.addVertex(lerp(m.positions[a], m.positions[b], t));
    std::vector<uint32_t> ns{0}, verts, uvi;
    for (uint32_t f = 0; f < m.numFaces(); ++f) {
        for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
            verts.push_back(m.faceVerts[h]);
            if (uv) uvi.push_back(m.faceUVs[h]);
            uint32_t from = m.faceVerts[h], to = m.faceVerts[m.heNext(h)];
            if ((from == a && to == b) || (from == b && to == a)) {
                verts.push_back(nv);
                if (uv) {
                    float tt = from == a ? t : 1.0f - t;
                    uvi.push_back(uint32_t(m.uvs.size()));
                    m.uvs.push_back(lerp(m.cornerUV(h), m.cornerUV(m.heNext(h)), tt));
                }
            }
        }
        ns.push_back(uint32_t(verts.size()));
    }
    m.faceStart = std::move(ns);
    m.faceVerts = std::move(verts);
    if (uv) { m.faceUVs = std::move(uvi); m.weldUVs(); }
    float crease = m.edgeCrease.size() == m.edges.size() ? m.edgeCrease[e] : 0.0f;
    bool hard = m.isHard(e);
    m.buildTopology();
    if (crease > 0) { m.setCrease(a, nv, crease); m.setCrease(nv, b, crease); }
    if (hard) { m.setHard(a, nv, true); m.setHard(nv, b, true); }
    return nv;
}

bool splitFace(Mesh& m, uint32_t va, uint32_t vb)
{
    if (va == vb || va >= m.numVerts() || vb >= m.numVerts()) return false;
    for (uint32_t f : m.vertexFaces(va)) {
        uint32_t b0 = m.faceStart[f], n = m.faceSize(f);
        int ia = -1, ib = -1;
        for (uint32_t i = 0; i < n; ++i) {
            if (m.faceVerts[b0 + i] == va) ia = int(i);
            if (m.faceVerts[b0 + i] == vb) ib = int(i);
        }
        if (ia < 0 || ib < 0) continue;
        int diff = (ib - ia + int(n)) % int(n);
        if (diff == 1 || diff == int(n) - 1) return false; // already connected
        const bool uv = m.hasUVs();
        std::vector<uint32_t> p1, p2, u1, u2;
        for (int k = ia;; k = (k + 1) % int(n)) {
            p1.push_back(m.faceVerts[b0 + uint32_t(k)]);
            if (uv) u1.push_back(m.faceUVs[b0 + uint32_t(k)]);
            if (k == ib) break;
        }
        for (int k = ib;; k = (k + 1) % int(n)) {
            p2.push_back(m.faceVerts[b0 + uint32_t(k)]);
            if (uv) u2.push_back(m.faceUVs[b0 + uint32_t(k)]);
            if (k == ia) break;
        }
        std::vector<uint32_t> ns{0}, verts, uvs;
        for (uint32_t g = 0; g < m.numFaces(); ++g) {
            if (g == f) {
                verts.insert(verts.end(), p1.begin(), p1.end());
                uvs.insert(uvs.end(), u1.begin(), u1.end());
                ns.push_back(uint32_t(verts.size()));
                verts.insert(verts.end(), p2.begin(), p2.end());
                uvs.insert(uvs.end(), u2.begin(), u2.end());
            } else {
                for (uint32_t h = m.faceStart[g]; h < m.faceStart[g + 1]; ++h) {
                    verts.push_back(m.faceVerts[h]);
                    if (uv) uvs.push_back(m.faceUVs[h]);
                }
            }
            ns.push_back(uint32_t(verts.size()));
        }
        m.faceStart = std::move(ns);
        m.faceVerts = std::move(verts);
        if (uv) m.faceUVs = std::move(uvs);
        m.buildTopology();
        return true;
    }
    return false;
}

IndexList multiCut(Mesh& m, const std::vector<CutPoint>& points)
{
    std::vector<uint32_t> verts;
    for (const CutPoint& p : points) {
        uint32_t v = p.a;
        if (p.b != kInvalid) {
            if (p.t <= 0.001f) v = p.a;
            else if (p.t >= 0.999f) v = p.b;
            else v = splitEdge(m, p.a, p.b, p.t);
        }
        if (v == kInvalid) continue;
        if (verts.empty() || verts.back() != v) verts.push_back(v);
    }
    IndexList out;
    for (size_t i = 0; i + 1 < verts.size(); ++i) {
        if (splitFace(m, verts[i], verts[i + 1])) {
            uint32_t e = m.findEdge(verts[i], verts[i + 1]);
            if (e != kInvalid) out.push_back(e);
        } else if (uint32_t e = m.findEdge(verts[i], verts[i + 1]); e != kInvalid) {
            out.push_back(e);
        }
    }
    return out;
}

} // namespace facet::ops
