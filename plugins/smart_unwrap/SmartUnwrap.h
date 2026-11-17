#pragma once
#include "mesh/Mesh.h"

#include <vector>

namespace facet::smart {

struct Options {
    float angleLimit = 66.0f;   // max normal deviation inside a shell (degrees)
    bool hardEdgeSeams = true;  // hard edges always become seams
    bool keepSeams = false;     // existing UV borders stay seams
    float maxStretch = 1.1f;    // split shells whose area distortion exceeds this
    int maxSplitDepth = 8;
    float minShellFraction = 0.002f; // shells smaller than this fraction of the area merge into a neighbour
    int textureSize = 2048;
    float paddingPx = 8.0f;
    bool packTogether = true;   // one shared tile & texel density for all selected meshes
    bool rotateShells = true;
};

struct Target {
    Mesh* mesh;
    Mat4 world;
    std::vector<uint32_t> faces; // empty = all
};

struct Report {
    int shells = 0;
    int splits = 0;
    int merges = 0;
    float texelDensity = 0;     // texture pixels per world unit
    float efficiency = 0;       // packed shell area / tile area
};

Report unwrap(std::vector<Target>& targets, const Options& opt);

} // namespace facet::smart
