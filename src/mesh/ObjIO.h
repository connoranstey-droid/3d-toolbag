#pragma once
#include "mesh/Mesh.h"

#include <string>
#include <vector>

namespace facet {

struct NamedMesh {
    std::string name;
    Mesh mesh;
    Mat4 xform; // used on export only
};

// Reads polygonal geometry from a Wavefront OBJ. Each `o`/`g` block becomes a mesh.
bool readObj(const std::string& path, std::vector<NamedMesh>& out, std::string* error = nullptr);
// Writes meshes (transformed by xform) into one OBJ file.
bool writeObj(const std::string& path, const std::vector<NamedMesh>& meshes, std::string* error = nullptr);

} // namespace facet
