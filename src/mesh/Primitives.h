#pragma once
#include "mesh/Mesh.h"

namespace facet::prim {

// All primitives are centred at the origin, Y-up, with outward-facing CCW winding.
Mesh cube(float w = 1, float h = 1, float d = 1, int sx = 1, int sy = 1, int sz = 1);
Mesh plane(float w = 1, float h = 1, int sx = 1, int sy = 1);
Mesh sphere(float radius = 1, int axisDivs = 20, int heightDivs = 20);
Mesh cylinder(float radius = 1, float height = 2, int axisDivs = 20, int heightDivs = 1, bool caps = true);
Mesh cone(float radius = 1, float height = 2, int axisDivs = 20, int heightDivs = 1);
Mesh torus(float radius = 1, float sectionRadius = 0.5f, int axisDivs = 20, int heightDivs = 20);

// Welds vertices closer than eps (used to stitch generated patches).
void weld(Mesh& m, float eps = 1e-6f);

} // namespace facet::prim
