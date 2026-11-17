#include "mesh/Mesh.h"
#include "util/Parallel.h"

#include <atomic>
#include <cmath>
#include <unordered_map>

namespace facet {

uint64_t nextVersionId()
{
    static std::atomic<uint64_t> counter{1};
    return counter.fetch_add(1);
}

static inline uint64_t pairKey(uint32_t a, uint32_t b)
{
    if (a > b) std::swap(a, b);
    return (uint64_t(a) << 32) | b;
}

void Mesh::clear()
{
    positions.clear();
    faceStart.assign(1, 0);
    faceVerts.clear();
    uvs.clear();
    faceUVs.clear();
    heFace.clear(); heTwin.clear(); heEdge.clear();
    edges.clear(); edgeCrease.clear(); edgeHard.clear();
    vertEdgeStart.clear(); vertEdges.clear();
    vertFaceStart.clear(); vertFaces.clear();
}

uint32_t Mesh::addFace(std::span<const uint32_t> verts)
{
    if (hasUVs()) {
        // Keep the UV channel parallel: give new corners uvs copied from another corner of the same vertex.
        std::vector<uint32_t> any(numVerts(), kInvalid);
        for (size_t h = 0; h < faceVerts.size(); ++h) any[faceVerts[h]] = faceUVs[h];
        for (uint32_t v : verts) {
            Vec2 uv = (v < any.size() && any[v] != kInvalid) ? uvs[any[v]] : Vec2{};
            faceUVs.push_back(uint32_t(uvs.size()));
            uvs.push_back(uv);
        }
    }
    faceVerts.insert(faceVerts.end(), verts.begin(), verts.end());
    faceStart.push_back(static_cast<uint32_t>(faceVerts.size()));
    return numFaces() - 1;
}

uint32_t Mesh::addFaceWithUVs(std::span<const uint32_t> verts, std::span<const Vec2> uv)
{
    if (!hasUVs() && !faceVerts.empty()) ensureUVs();
    for (size_t i = 0; i < verts.size(); ++i) {
        faceUVs.push_back(uint32_t(uvs.size()));
        uvs.push_back(i < uv.size() ? uv[i] : Vec2{});
    }
    faceVerts.insert(faceVerts.end(), verts.begin(), verts.end());
    faceStart.push_back(static_cast<uint32_t>(faceVerts.size()));
    return numFaces() - 1;
}

void Mesh::ensureUVs()
{
    if (hasUVs() || faceVerts.empty()) return;
    uvs.assign(numVerts(), Vec2{});
    faceUVs = faceVerts;
    touchUVs();
}

void Mesh::weldUVs(float eps)
{
    if (!hasUVs()) return;
    const uint32_t H = numHalfEdges();
    // Group corners by vertex (counting sort), then merge equal uv values within each group.
    std::vector<uint32_t> start(numVerts() + 1, 0), order(H);
    for (uint32_t h = 0; h < H; ++h) start[faceVerts[h] + 1]++;
    for (uint32_t v = 0; v < numVerts(); ++v) start[v + 1] += start[v];
    {
        std::vector<uint32_t> fill(start.begin(), start.end() - 1);
        for (uint32_t h = 0; h < H; ++h) order[fill[faceVerts[h]]++] = h;
    }
    std::vector<uint32_t> reps;
    for (uint32_t v = 0; v < numVerts(); ++v) {
        reps.clear();
        for (uint32_t i = start[v]; i < start[v + 1]; ++i) {
            uint32_t h = order[i];
            const Vec2 uv = uvs[faceUVs[h]];
            bool merged = false;
            for (uint32_t r : reps)
                if (std::abs(uvs[r].x - uv.x) <= eps && std::abs(uvs[r].y - uv.y) <= eps) { faceUVs[h] = r; merged = true; break; }
            if (!merged) reps.push_back(faceUVs[h]);
        }
    }
    compactUVs();
}

void Mesh::compactUVs()
{
    if (faceUVs.empty()) { uvs.clear(); return; }
    std::vector<uint32_t> remap(uvs.size(), kInvalid);
    std::vector<Vec2> out;
    out.reserve(uvs.size());
    for (uint32_t& u : faceUVs) {
        if (remap[u] == kInvalid) { remap[u] = uint32_t(out.size()); out.push_back(uvs[u]); }
        u = remap[u];
    }
    uvs = std::move(out);
    touchUVs();
}

bool Mesh::isUVBorderEdge(uint32_t e) const
{
    if (!hasUVs()) return edges[e].faceCount != 2;
    uint32_t h = edges[e].he, t = heTwin[h];
    if (t == kInvalid) return true;
    // h: a->b, t: b->a. Same uv at a means faceUVs[h] == faceUVs[next(t)].
    return faceUVs[h] != faceUVs[heNext(t)] || faceUVs[heNext(h)] != faceUVs[t];
}

void Mesh::buildTopology()
{
    // Remember creases by vertex pair so edits that keep vertex ids keep creases.
    std::unordered_map<uint64_t, float> oldCreases;
    std::unordered_map<uint64_t, uint8_t> oldHard;
    if (edgeCrease.size() == edges.size()) {
        for (size_t e = 0; e < edges.size(); ++e)
            if (edgeCrease[e] > 0.0f && edges[e].v0 != kInvalid) oldCreases[pairKey(edges[e].v0, edges[e].v1)] = edgeCrease[e];
    }
    if (edgeHard.size() == edges.size()) {
        for (size_t e = 0; e < edges.size(); ++e)
            if (edgeHard[e] && edges[e].v0 != kInvalid) oldHard[pairKey(edges[e].v0, edges[e].v1)] = 1;
    }

    const uint32_t V = numVerts();
    const uint32_t F = numFaces();
    const uint32_t H = numHalfEdges();

    heFace.resize(H);
    for (uint32_t f = 0; f < F; ++f)
        for (uint32_t h = faceStart[f]; h < faceStart[f + 1]; ++h) heFace[h] = f;

    // Outgoing half-edges per vertex (CSR).
    std::vector<uint32_t> outStart(V + 1, 0), outHe(H);
    for (uint32_t h = 0; h < H; ++h) outStart[faceVerts[h] + 1]++;
    for (uint32_t v = 0; v < V; ++v) outStart[v + 1] += outStart[v];
    {
        std::vector<uint32_t> fill(outStart.begin(), outStart.end() - 1);
        for (uint32_t h = 0; h < H; ++h) outHe[fill[faceVerts[h]]++] = h;
    }

    heTwin.assign(H, kInvalid);
    heEdge.assign(H, kInvalid);
    edges.clear();
    edges.reserve(H / 2 + 16);
    for (uint32_t h = 0; h < H; ++h) {
        if (heEdge[h] != kInvalid) continue;
        const uint32_t a = heFrom(h), b = heTo(h);
        const uint32_t e = static_cast<uint32_t>(edges.size());
        edges.push_back({a, b, h, 1});
        heEdge[h] = e;
        // Opposite-direction half-edges (properly oriented neighbours).
        for (uint32_t i = outStart[b]; i < outStart[b + 1]; ++i) {
            uint32_t g = outHe[i];
            if (heEdge[g] == kInvalid && heTo(g) == a) {
                heEdge[g] = e;
                edges[e].faceCount++;
                if (heTwin[h] == kInvalid) { heTwin[h] = g; heTwin[g] = h; }
            }
        }
        // Same-direction duplicates (inconsistent winding): same edge, no twin link.
        for (uint32_t i = outStart[a]; i < outStart[a + 1]; ++i) {
            uint32_t g = outHe[i];
            if (g != h && heEdge[g] == kInvalid && heTo(g) == b) {
                heEdge[g] = e;
                edges[e].faceCount++;
            }
        }
    }
    const uint32_t E = numEdges();

    vertEdgeStart.assign(V + 1, 0);
    for (const auto& ed : edges) { vertEdgeStart[ed.v0 + 1]++; vertEdgeStart[ed.v1 + 1]++; }
    for (uint32_t v = 0; v < V; ++v) vertEdgeStart[v + 1] += vertEdgeStart[v];
    vertEdges.resize(vertEdgeStart[V]);
    {
        std::vector<uint32_t> fill(vertEdgeStart.begin(), vertEdgeStart.end() - 1);
        for (uint32_t e = 0; e < E; ++e) {
            vertEdges[fill[edges[e].v0]++] = e;
            vertEdges[fill[edges[e].v1]++] = e;
        }
    }

    vertFaceStart.assign(V + 1, 0);
    for (uint32_t h = 0; h < H; ++h) vertFaceStart[faceVerts[h] + 1]++;
    for (uint32_t v = 0; v < V; ++v) vertFaceStart[v + 1] += vertFaceStart[v];
    vertFaces.resize(H);
    {
        std::vector<uint32_t> fill(vertFaceStart.begin(), vertFaceStart.end() - 1);
        for (uint32_t h = 0; h < H; ++h) vertFaces[fill[faceVerts[h]]++] = heFace[h];
    }

    edgeHard.assign(E, 0);
    if (!oldHard.empty()) {
        for (uint32_t e = 0; e < E; ++e)
            if (oldHard.count(pairKey(edges[e].v0, edges[e].v1))) edgeHard[e] = 1;
    }
    edgeCrease.assign(E, 0.0f);
    if (!oldCreases.empty()) {
        for (uint32_t e = 0; e < E; ++e) {
            auto it = oldCreases.find(pairKey(edges[e].v0, edges[e].v1));
            if (it != oldCreases.end()) edgeCrease[e] = it->second;
        }
    }

    topologyId = nextVersionId();
    positionId = nextVersionId();
}

uint32_t Mesh::findEdge(uint32_t a, uint32_t b) const
{
    if (a >= numVerts()) return kInvalid;
    for (uint32_t e : vertexEdges(a))
        if (edgeOther(e, a) == b) return e;
    return kInvalid;
}

bool Mesh::isBorderVertex(uint32_t v) const
{
    for (uint32_t e : vertexEdges(v))
        if (edges[e].faceCount == 1) return true;
    return false;
}

uint32_t Mesh::halfEdgeOfEdgeInFace(uint32_t e, uint32_t f) const
{
    for (uint32_t h = faceStart[f]; h < faceStart[f + 1]; ++h)
        if (heEdge[h] == e) return h;
    return kInvalid;
}

Vec3 Mesh::faceCenter(uint32_t f) const
{
    Vec3 c;
    for (uint32_t v : faceVertices(f)) c += positions[v];
    return c / float(faceSize(f));
}

Vec3 Mesh::faceNormalRaw(uint32_t f) const
{
    Vec3 n;
    const uint32_t b = faceStart[f], e = faceStart[f + 1];
    for (uint32_t h = b; h < e; ++h) {
        const Vec3& p = positions[faceVerts[h]];
        const Vec3& q = positions[faceVerts[h + 1 < e ? h + 1 : b]];
        n.x += (p.y - q.y) * (p.z + q.z);
        n.y += (p.z - q.z) * (p.x + q.x);
        n.z += (p.x - q.x) * (p.y + q.y);
    }
    return n;
}

void Mesh::computeFaceNormals(std::vector<Vec3>& out) const
{
    out.resize(numFaces());
    parallelFor(numFaces(), [&](size_t f) { out[f] = faceNormalRaw(uint32_t(f)); });
}

void Mesh::computeVertexNormals(std::vector<Vec3>& out) const
{
    std::vector<Vec3> fn;
    computeFaceNormals(fn); // area weighted (unnormalized)
    out.resize(numVerts());
    parallelFor(numVerts(), [&](size_t v) {
        Vec3 n;
        for (uint32_t f : vertexFaces(uint32_t(v))) n += fn[f];
        out[v] = normalize(n);
    });
}

AABB Mesh::bounds() const
{
    AABB b;
    for (const Vec3& p : positions) b.expand(p);
    return b;
}

uint64_t Mesh::triangleCount() const
{
    uint64_t t = 0;
    for (uint32_t f = 0; f < numFaces(); ++f) t += faceSize(f) >= 3 ? faceSize(f) - 2 : 0;
    return t;
}

bool Mesh::hasHardEdges() const
{
    for (uint8_t h : edgeHard)
        if (h) return true;
    return false;
}

void Mesh::setHard(uint32_t a, uint32_t b, bool hard)
{
    uint32_t e = findEdge(a, b);
    if (e == kInvalid) return;
    if (edgeHard.size() != edges.size()) edgeHard.assign(edges.size(), 0);
    edgeHard[e] = hard ? 1 : 0;
}

void Mesh::computeCornerNormals(std::vector<Vec3>& out, std::vector<uint32_t>* groupOut) const
{
    const uint32_t H = numHalfEdges();
    std::vector<Vec3> fn;
    computeFaceNormals(fn);
    // Union corners across soft manifold edges (at both ends of the edge).
    std::vector<uint32_t> parent(H);
    for (uint32_t h = 0; h < H; ++h) parent[h] = h;
    auto find = [&](uint32_t x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    auto unite = [&](uint32_t a, uint32_t b) {
        a = find(a); b = find(b);
        if (a != b) parent[std::max(a, b)] = std::min(a, b);
    };
    const bool anyHard = hasHardEdges();
    for (uint32_t e = 0; e < numEdges(); ++e) {
        if (anyHard && edgeHard[e]) continue;
        uint32_t h = edges[e].he, t = heTwin[h];
        if (t == kInvalid) continue;
        if (faceVerts[h] == faceVerts[heNext(t)]) unite(h, heNext(t));
        if (faceVerts[heNext(h)] == faceVerts[t]) unite(heNext(h), t);
    }
    std::vector<Vec3> sum(H);
    if (normalWeightPower > 0.0f) {
        // fn is area * unit normal; scale by area^power (relative to the mean so magnitudes stay sane).
        double mean = 0;
        for (const Vec3& n : fn) mean += length(n);
        mean = fn.empty() ? 1.0 : std::max(mean / double(fn.size()), 1e-30);
        std::vector<Vec3> wn(fn.size());
        for (size_t f = 0; f < fn.size(); ++f) {
            float a = float(length(fn[f]) / mean);
            wn[f] = fn[f] * std::pow(std::max(a, 1e-12f), normalWeightPower);
        }
        for (uint32_t h = 0; h < H; ++h) sum[find(h)] += wn[heFace[h]];
    } else {
        for (uint32_t h = 0; h < H; ++h) sum[find(h)] += fn[heFace[h]];
    }
    out.resize(H);
    if (groupOut) groupOut->resize(H);
    for (uint32_t h = 0; h < H; ++h) {
        uint32_t r = find(h);
        out[h] = normalize(sum[r]);
        if (groupOut) (*groupOut)[h] = r;
    }
}

bool Mesh::hasCreases() const
{
    for (float c : edgeCrease)
        if (c > 0.0f) return true;
    return false;
}

void Mesh::setCrease(uint32_t a, uint32_t b, float s)
{
    uint32_t e = findEdge(a, b);
    if (e != kInvalid) {
        if (edgeCrease.size() != edges.size()) edgeCrease.assign(edges.size(), 0.0f);
        edgeCrease[e] = s;
    }
}

void Mesh::removeUnusedVertices(std::vector<uint32_t>* remapOut)
{
    std::vector<uint32_t> remap(numVerts(), kInvalid);
    for (uint32_t v : faceVerts) remap[v] = 0;
    uint32_t n = 0;
    for (uint32_t v = 0; v < numVerts(); ++v)
        if (remap[v] != kInvalid) {
            positions[n] = positions[v];
            remap[v] = n++;
        }
    positions.resize(n);
    for (uint32_t& v : faceVerts) v = remap[v];
    // Keep stale edges pointing at the new ids so buildTopology() preserves creases.
    for (auto& e : edges) {
        uint32_t a = e.v0 < remap.size() ? remap[e.v0] : kInvalid;
        uint32_t b = e.v1 < remap.size() ? remap[e.v1] : kInvalid;
        if (a == kInvalid || b == kInvalid) { e.v0 = e.v1 = kInvalid; }
        else { e.v0 = a; e.v1 = b; }
    }
    if (remapOut) *remapOut = std::move(remap);
}

void Mesh::append(const Mesh& other, const Mat4& xform)
{
    const bool wantUV = hasUVs() || other.hasUVs();
    if (wantUV) ensureUVs();
    const uint32_t base = numVerts();
    if (wantUV) {
        const uint32_t uvBase = uint32_t(uvs.size());
        if (other.hasUVs()) {
            uvs.insert(uvs.end(), other.uvs.begin(), other.uvs.end());
            for (uint32_t u : other.faceUVs) faceUVs.push_back(u + uvBase);
        } else {
            for (size_t i = 0; i < other.faceVerts.size(); ++i) { faceUVs.push_back(uint32_t(uvs.size())); uvs.push_back({}); }
        }
    }
    for (const Vec3& p : other.positions) positions.push_back(xform.transformPoint(p));
    for (uint32_t f = 0; f < other.numFaces(); ++f) {
        for (uint32_t v : other.faceVertices(f)) faceVerts.push_back(v + base);
        faceStart.push_back(static_cast<uint32_t>(faceVerts.size()));
    }
    // Carry creases and hard edges as stale edges so buildTopology() picks them up.
    if (edgeCrease.size() != edges.size()) edgeCrease.resize(edges.size(), 0.0f);
    if (edgeHard.size() != edges.size()) edgeHard.resize(edges.size(), 0);
    for (uint32_t e = 0; e < other.numEdges(); ++e) {
        float c = e < other.edgeCrease.size() ? other.edgeCrease[e] : 0.0f;
        uint8_t h = e < other.edgeHard.size() ? other.edgeHard[e] : 0;
        if (c > 0.0f || h) {
            edges.push_back({other.edges[e].v0 + base, other.edges[e].v1 + base, kInvalid, 0});
            edgeCrease.push_back(c);
            edgeHard.push_back(h);
        }
    }
}

void Mesh::reverseFace(uint32_t f)
{
    std::reverse(faceVerts.begin() + faceStart[f], faceVerts.begin() + faceStart[f + 1]);
    if (faceUVs.size() == faceVerts.size()) std::reverse(faceUVs.begin() + faceStart[f], faceUVs.begin() + faceStart[f + 1]);
}

} // namespace facet
