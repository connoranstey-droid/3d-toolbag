#pragma once
#include "mesh/Mesh.h"

#include <string>
#include <vector>

namespace facet {

struct FbxMesh {
    std::string name;
    Mesh mesh;
    Vec3 translate, rotate, scale{1, 1, 1}; // object transform (rotate: degrees, XYZ order)
};

// Reads binary (any 7.x) and ASCII FBX: polygon geometry, first UV set, Lcl transforms,
// pre-rotation and model parenting (parents are baked into each object's transform).
bool readFbx(const std::string& path, std::vector<FbxMesh>& out, std::string* error = nullptr);
// Writes binary FBX 7.4 (Maya 2014+/Blender/Unity/Unreal compatible): geometry, normals,
// UVs ("map1"), a lambert material and per-object transforms.
bool writeFbx(const std::string& path, const std::vector<FbxMesh>& meshes, std::string* error = nullptr);

} // namespace facet
