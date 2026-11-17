#pragma once
#include "mesh/Mesh.h"

#include <vector>

// UV unwrapping toolkit. "faces" / "uvSel" arguments: empty means the whole mesh.
namespace facet::uv {

using IndexList = std::vector<uint32_t>;
enum class Axis { X, Y, Z, Best };

void planar(Mesh& m, const IndexList& faces, Axis axis = Axis::Best);
void cylindrical(Mesh& m, const IndexList& faces);
void spherical(Mesh& m, const IndexList& faces);
// Splits faces into clusters by dominant normal direction, projects each, then lays out.
void automatic(Mesh& m, const IndexList& faces);

// Cut: separates UVs along edges (makes them UV borders). Sew: moves the second shell onto the
// first and merges the edge's uvs.
void cutEdges(Mesh& m, const IndexList& edges);
void sewEdges(Mesh& m, const IndexList& edges);

// Shell id for every uv index (connected through faces).
std::vector<uint32_t> shellIds(const Mesh& m, uint32_t* shellCount = nullptr);
// All uv indices in the shells touched by `uvSel`.
IndexList expandToShells(const Mesh& m, const IndexList& uvSel);
// UV indices used by faces.
IndexList uvsOfFaces(const Mesh& m, const IndexList& faces);

// Least Squares Conformal Maps unfold of the shells touched by uvSel (all if empty).
// Keeps each shell's 3D surface area. Returns number of shells solved.
int unfold(Mesh& m, const IndexList& uvSel = {}, int maxIterations = 3000);
// Scales shells to a common texel density, rotates for a tight box and packs into 0..1.
void layout(Mesh& m, float padding = 0.01f);
// Fits the selected uvs (whole mesh if empty) into 0..1 keeping aspect.
void normalize(Mesh& m, const IndexList& uvSel = {});
void flip(Mesh& m, const IndexList& uvSel, bool horizontal);
void rotate(Mesh& m, const IndexList& uvSel, float degrees);
Vec2 center(const Mesh& m, const IndexList& uvSel);
// Ratio of flipped (negative-area) faces in uv space: 0 = none.
float flippedRatio(const Mesh& m);

} // namespace facet::uv
