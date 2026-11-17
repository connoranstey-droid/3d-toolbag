#include "mesh/Mesh.h"
#include "util/Parallel.h"

#include <atomic>
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
    heFace.clear(); heTwin.clear(); heEdge.clear();
    edges.clear(); edgeCrease.clear();
    vertEdgeStart.clear(); vertEdges.clear();
    vertFaceStart.clear(); vertFaces.clear();
}

uint32_t Mesh::addFace(std::span<const uint32_t> verts)
{
    faceVerts.insert(faceVerts.end(), verts.begin(), verts.end());
    faceStart.push_back(static_cast<uint32_t>(faceVerts.size()));
    return numFaces() - 1;
}

void Mesh::buildTopology()
{
    // Remember creases by vertex pair so edits that keep vertex ids keep creases.
    std::unordered_map<uint64_t, float> oldCreases;
    if (edgeCrease.size() == edges.size()) {
        for (size_t e = 0; e < edges.size(); ++e)
            if (edgeCrease[e] > 0.0f && edges[e].v0 != kInvalid) oldCreases[pairKey(edges[e].v0, edges[e].v1)] = edgeCrease[e];
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
    const uint32_t base = numVerts();
    for (const Vec3& p : other.positions) positions.push_back(xform.transformPoint(p));
    for (uint32_t f = 0; f < other.numFaces(); ++f) {
        for (uint32_t v : other.faceVertices(f)) faceVerts.push_back(v + base);
        faceStart.push_back(static_cast<uint32_t>(faceVerts.size()));
    }
    // Carry creases as stale edges so buildTopology() picks them up.
    if (other.edgeCrease.size() == other.edges.size()) {
        if (edgeCrease.size() != edges.size()) edgeCrease.assign(edges.size(), 0.0f);
        for (uint32_t e = 0; e < other.numEdges(); ++e)
            if (other.edgeCrease[e] > 0.0f) {
                edges.push_back({other.edges[e].v0 + base, other.edges[e].v1 + base, kInvalid, 0});
                edgeCrease.push_back(other.edgeCrease[e]);
            }
    }
}

void Mesh::reverseFace(uint32_t f)
{
    std::reverse(faceVerts.begin() + faceStart[f], faceVerts.begin() + faceStart[f + 1]);
}

} // namespace facet
