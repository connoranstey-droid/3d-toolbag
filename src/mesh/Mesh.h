#pragma once
#include "math/Math.h"

#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace facet {

constexpr uint32_t kInvalid = 0xFFFFFFFFu;

// Globally unique, monotonically increasing ids. Two meshes that carry the same
// (topologyId, positionId) pair hold identical data, which lets GPU and
// subdivision caches be keyed purely on ids, independent of object identity.
uint64_t nextVersionId();

struct MeshEdge {
    uint32_t v0 = kInvalid, v1 = kInvalid;
    uint32_t he = kInvalid;    // one half-edge lying on this edge
    uint32_t faceCount = 0;    // 1 = border, 2 = manifold, >2 = non-manifold
};

// Polygon mesh with arbitrary n-gon faces and derived half-edge adjacency.
//
// Faces are stored as a CSR list: face f uses corners [faceStart[f], faceStart[f+1]).
// Every corner doubles as a half-edge: half-edge h runs from faceVerts[h] to the
// next corner's vertex in the same face. After editing faces call buildTopology().
class Mesh {
public:
    std::vector<Vec3> positions;
    std::vector<uint32_t> faceStart{0};
    std::vector<uint32_t> faceVerts;

    // Face-varying UVs: faceUVs[corner] indexes uvs. Corners that share a vertex *and* a uv index
    // are connected in UV space; different indices at one vertex form a UV seam.
    std::vector<Vec2> uvs;
    std::vector<uint32_t> faceUVs;

    // ---- derived topology (buildTopology) ----
    std::vector<uint32_t> heFace;
    std::vector<uint32_t> heTwin;   // kInvalid on borders
    std::vector<uint32_t> heEdge;
    std::vector<MeshEdge> edges;
    std::vector<float> edgeCrease;  // subdivision sharpness per edge (0 = smooth)
    std::vector<uint32_t> vertEdgeStart, vertEdges;
    std::vector<uint32_t> vertFaceStart, vertFaces;

    uint64_t topologyId = 0;
    uint64_t positionId = 0;
    uint64_t uvId = 0;

    bool hasUVs() const { return !uvs.empty() && faceUVs.size() == faceVerts.size() && !faceVerts.empty(); }
    Vec2 cornerUV(uint32_t h) const { return uvs[faceUVs[h]]; }
    void touchUVs() { uvId = nextVersionId(); }
    // Adds one fresh uv entry per corner (helper for ops that create faces).
    uint32_t addFaceWithUVs(std::span<const uint32_t> verts, std::span<const Vec2> uv);
    // Merges uv indices of corners that share a vertex and (nearly) the same uv value.
    void weldUVs(float eps = 1e-6f);
    // Drops uv entries no corner uses.
    void compactUVs();
    // Creates an all-zero UV set (one uv per vertex) if the mesh has none.
    void ensureUVs();
    // UV seams: true if the edge's two sides use different uv indices at either end (or it is a border).
    bool isUVBorderEdge(uint32_t e) const;

    uint32_t numVerts() const { return static_cast<uint32_t>(positions.size()); }
    uint32_t numFaces() const { return static_cast<uint32_t>(faceStart.size() - 1); }
    uint32_t numEdges() const { return static_cast<uint32_t>(edges.size()); }
    uint32_t numHalfEdges() const { return static_cast<uint32_t>(faceVerts.size()); }
    uint32_t faceSize(uint32_t f) const { return faceStart[f + 1] - faceStart[f]; }

    uint32_t heNext(uint32_t h) const
    {
        uint32_t f = heFace[h];
        return h + 1 < faceStart[f + 1] ? h + 1 : faceStart[f];
    }
    uint32_t hePrev(uint32_t h) const
    {
        uint32_t f = heFace[h];
        return h > faceStart[f] ? h - 1 : faceStart[f + 1] - 1;
    }
    uint32_t heFrom(uint32_t h) const { return faceVerts[h]; }
    uint32_t heTo(uint32_t h) const { return faceVerts[heNext(h)]; }

    std::span<const uint32_t> faceVertices(uint32_t f) const
    {
        return {faceVerts.data() + faceStart[f], faceSize(f)};
    }
    std::span<const uint32_t> vertexEdges(uint32_t v) const
    {
        return {vertEdges.data() + vertEdgeStart[v], vertEdgeStart[v + 1] - vertEdgeStart[v]};
    }
    std::span<const uint32_t> vertexFaces(uint32_t v) const
    {
        return {vertFaces.data() + vertFaceStart[v], vertFaceStart[v + 1] - vertFaceStart[v]};
    }
    uint32_t edgeOther(uint32_t e, uint32_t v) const { return edges[e].v0 == v ? edges[e].v1 : edges[e].v0; }

    void clear();
    uint32_t addVertex(const Vec3& p)
    {
        positions.push_back(p);
        return numVerts() - 1;
    }
    uint32_t addFace(std::span<const uint32_t> verts);
    uint32_t addFace(std::initializer_list<uint32_t> verts) { return addFace(std::span<const uint32_t>(verts.begin(), verts.size())); }

    // Rebuilds adjacency. Existing edge creases are carried over by vertex pair.
    void buildTopology();
    void touchPositions() { positionId = nextVersionId(); }
    bool topologyValid() const { return heFace.size() == faceVerts.size() && vertEdgeStart.size() == positions.size() + 1; }

    uint32_t findEdge(uint32_t a, uint32_t b) const;
    bool isBorderEdge(uint32_t e) const { return edges[e].faceCount == 1; }
    bool isBorderVertex(uint32_t v) const;
    uint32_t halfEdgeOfEdgeInFace(uint32_t e, uint32_t f) const;

    Vec3 faceCenter(uint32_t f) const;
    Vec3 faceNormalRaw(uint32_t f) const; // Newell normal, length = 2 * area
    Vec3 faceNormal(uint32_t f) const { return normalize(faceNormalRaw(f)); }
    Vec3 edgeMidpoint(uint32_t e) const { return (positions[edges[e].v0] + positions[edges[e].v1]) * 0.5f; }
    void computeFaceNormals(std::vector<Vec3>& out) const;
    void computeVertexNormals(std::vector<Vec3>& out) const;
    AABB bounds() const;
    uint64_t triangleCount() const;

    bool hasCreases() const;
    void setCrease(uint32_t a, uint32_t b, float s);

    // Drops vertices referenced by no face. remapOut (if given) maps old -> new (kInvalid = removed).
    void removeUnusedVertices(std::vector<uint32_t>* remapOut = nullptr);
    // Appends another mesh (transformed). Call buildTopology() afterwards.
    void append(const Mesh& other, const Mat4& xform);
    void reverseFace(uint32_t f);
};

} // namespace facet
