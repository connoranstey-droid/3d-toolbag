#pragma once
#include "mesh/Mesh.h"

#include <vector>

namespace facet {

// Triangle BVH over a mesh (local space) for ray casts and closest-point queries.
// Used to snap retopology to a "live" high-poly surface.
class TriBVH {
public:
    struct Hit {
        bool hit = false;
        float t = 0;
        uint32_t face = kInvalid;
        Vec3 point, normal;
    };
    void build(const Mesh& m);
    bool empty() const { return nodes_.empty(); }
    Hit raycast(const Ray& ray) const;
    Hit closest(const Vec3& p, float maxDist = 1e30f) const;
    uint64_t topologyId = 0, positionId = 0;

private:
    struct Node {
        AABB box;
        uint32_t left = 0, right = 0;   // children (inner)
        uint32_t start = 0, count = 0;  // triangle range (leaf when count > 0)
    };
    uint32_t buildNode(uint32_t start, uint32_t end, int depth);
    std::vector<Node> nodes_;
    std::vector<Vec3> v0_, v1_, v2_;
    std::vector<uint32_t> face_, order_;
    std::vector<Vec3> centroid_;
};

} // namespace facet
