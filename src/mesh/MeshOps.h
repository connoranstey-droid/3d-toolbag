#pragma once
#include "mesh/Mesh.h"

#include <vector>

namespace facet::ops {

using IndexList = std::vector<uint32_t>;

// ---- topology-changing operations (all rebuild topology before returning) ----

// Extrudes faces. Returns the cap faces (same indices as the input faces).
IndexList extrudeFaces(Mesh& m, const IndexList& faces, bool keepTogether = true, float offset = 0.0f);
void deleteFaces(Mesh& m, const IndexList& faces);
// Dissolves edges, merging adjacent faces; optionally removes leftover 2-valence vertices.
bool dissolveEdges(Mesh& m, const IndexList& edges, bool removeWinged = true);
void deleteVertices(Mesh& m, const IndexList& verts);
// Collapses all given vertices into one at their centroid. Returns the surviving vertex.
uint32_t mergeToCenter(Mesh& m, const IndexList& verts);
// Merges vertices within `threshold` of each other. Returns number of vertices removed.
uint32_t mergeByDistance(Mesh& m, const IndexList& verts, float threshold);
// Collapses each edge (or each connected edge group) to its centre.
void collapseEdges(Mesh& m, const IndexList& edges);

struct EdgeLoopPlan {
    struct Split { uint32_t edge; float t; }; // t measured from edges[edge].v0
    struct Cut { uint32_t face, e0, e1; };
    std::vector<Split> splits;
    std::vector<Cut> cuts;
    bool valid() const { return !cuts.empty(); }
};
// Plans an edge loop across the quad ring through `edge`; t in (0,1) from edges[edge].v0.
EdgeLoopPlan planEdgeLoop(const Mesh& m, uint32_t edge, float t);
// Applies the plan. Returns the new loop's edges.
IndexList insertEdgeLoop(Mesh& m, const EdgeLoopPlan& plan);

// Caps holes bordered by the given border edges (all holes when empty). Returns new faces.
IndexList fillHoles(Mesh& m, const IndexList& borderEdges = {});
void reverseFaces(Mesh& m, const IndexList& faces = {});
void triangulate(Mesh& m, const IndexList& faces = {});
// Mirrors geometry across the local axis plane (0=X,1=Y,2=Z), welding the seam.
void mirror(Mesh& m, int axis, float mergeThreshold = 1e-3f);
std::vector<Mesh> separate(const Mesh& m);

// Merges vertex `src` into `dst` (dst keeps its position).
void mergeInto(Mesh& m, uint32_t src, uint32_t dst);

struct BevelOptions {
    float offset = 0.1f;  // world-ish distance from the original edge
    int segments = 1;
    bool round = true;    // circular-ish profile when segments > 1
};
// Bevels/chamfers manifold edges. Returns the new faces (edge strips and corner patches).
IndexList bevelEdges(Mesh& m, const IndexList& edges, const BevelOptions& opt);

// Bridges exactly two border edge chains (open or closed, equal length). Returns new faces.
IndexList bridgeEdges(Mesh& m, const IndexList& borderEdges, int divisions = 0);
// Deletes two face regions and bridges their boundary loops.
IndexList bridgeFaces(Mesh& m, const IndexList& faces, int divisions = 0);

// Splits edge (a,b) at t (from a). Returns the new vertex (kInvalid if no such edge).
uint32_t splitEdge(Mesh& m, uint32_t a, uint32_t b, float t);
// Splits a face containing both (non-adjacent) vertices. Returns false if none.
bool splitFace(Mesh& m, uint32_t va, uint32_t vb);

// Multi-Cut: a polyline of points on edges (b != kInvalid, t from a) or on vertices (b == kInvalid).
struct CutPoint { uint32_t a = kInvalid, b = kInvalid; float t = 0.0f; };
// Cuts consecutive points through shared faces. Returns the new edges.
IndexList multiCut(Mesh& m, const std::vector<CutPoint>& points);

// ---- component helpers ----
IndexList edgeLoop(const Mesh& m, uint32_t edge);
IndexList edgeRing(const Mesh& m, uint32_t edge);

enum class Comp { Vertex, Edge, Face };
IndexList convert(const Mesh& m, Comp from, const IndexList& items, Comp to);
IndexList grow(const Mesh& m, Comp type, const IndexList& items);
IndexList shrink(const Mesh& m, Comp type, const IndexList& items);
void sortUnique(IndexList& l);

} // namespace facet::ops
