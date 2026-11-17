#pragma once
#include "mesh/Mesh.h"

#include <vector>

// Sculpt brushes operating on a mesh's vertices (object space).
namespace facet::sculpt {

enum class Brush { Draw, Smooth, Grab, Inflate, Flatten, Pinch, Crease, Mask };
const char* brushName(Brush b);

struct Dab {
    Vec3 center;          // object space
    float radius = 0.1f;  // object space
    float strength = 0.5f;
    bool invert = false;
};

// One brush stroke. Records the original position of every vertex it touches so the stroke can
// be undone as a single step. Vertices are looked up through a uniform grid built at begin().
class Stroke {
public:
    // mask (optional, per vertex 0..1): 1 = fully protected. symmetryAxis: -1 none, 0..2 = X/Y/Z.
    void begin(Mesh& mesh, std::vector<float>* mask, Brush brush, float radius, int symmetryAxis);
    void dab(const Dab& d);
    // Grab: moves the vertices captured at the first dab by an object-space offset from their start.
    void grab(const Vec3& offset);
    void end();

    const std::vector<uint32_t>& touched() const { return touched_; }
    const std::vector<Vec3>& before() const { return before_; }
    bool active() const { return mesh_ != nullptr; }
    Brush brush() const { return brush_; }

private:
    void gather(const Vec3& c, float r, std::vector<uint32_t>& out) const;
    void apply(const Dab& d);
    void remember(uint32_t v);
    Vec3 vertexNormal(uint32_t v) const;

    Mesh* mesh_ = nullptr;
    std::vector<float>* mask_ = nullptr;
    Brush brush_ = Brush::Draw;
    int sym_ = -1;
    float cell_ = 1.0f;
    std::vector<uint32_t> cellStart_, cellVerts_;
    Vec3 gridLo_;
    int gx_ = 1, gy_ = 1, gz_ = 1;
    std::vector<uint8_t> seen_;
    std::vector<uint32_t> touched_;
    std::vector<Vec3> before_;
    // Grab state
    bool grabbing_ = false;
    std::vector<uint32_t> grabIdx_;
    std::vector<float> grabW_;
    std::vector<Vec3> grabStart_;
    std::vector<int8_t> grabMirror_;
};

// Utilities
float falloff(float d, float r); // smooth 1 -> 0
void invertMask(std::vector<float>& mask);

} // namespace facet::sculpt
