#pragma once
#include "mesh/Mesh.h"

#include <vector>

namespace facet {

// One Catmull-Clark step. The child's vertex order is [parent verts | edge points | face points],
// and child face c (one quad per parent corner) derives from parent face heFace[c].
void subdivideTopology(const Mesh& parent, Mesh& child);
void subdividePositions(const Mesh& parent, Mesh& child);

// Bakes `levels` Catmull-Clark steps into a new mesh (Mesh > Smooth).
Mesh subdivide(const Mesh& cage, int levels);

// Cached multi-level subdivision for smooth-preview display. Topology is refined only when the
// cage topology changes; vertex drags only re-run the (parallel) position evaluation.
class SubdivSurface {
public:
    const Mesh& evaluate(const Mesh& cage, int levels);
    // Final-level face -> originating cage face.
    const std::vector<uint32_t>& faceOrigin() const { return faceOrigin_; }
    void invalidate() { topoId_ = posId_ = 0; }

private:
    std::vector<Mesh> levels_;
    std::vector<uint32_t> faceOrigin_;
    uint64_t topoId_ = 0, posId_ = 0;
    int levelCount_ = -1;
};

} // namespace facet
