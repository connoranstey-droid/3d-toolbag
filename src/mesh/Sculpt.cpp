#include "mesh/Sculpt.h"
#include "util/Parallel.h"

#include <algorithm>
#include <cmath>

namespace facet::sculpt {

const char* brushName(Brush b)
{
    switch (b) {
    case Brush::Draw: return "Draw";
    case Brush::Smooth: return "Smooth";
    case Brush::Grab: return "Grab";
    case Brush::Inflate: return "Inflate";
    case Brush::Flatten: return "Flatten";
    case Brush::Pinch: return "Pinch";
    case Brush::Crease: return "Crease";
    case Brush::Mask: return "Mask";
    }
    return "?";
}

float falloff(float d, float r)
{
    if (d >= r) return 0.0f;
    float s = 1.0f - d / r;
    return s * s * (3.0f - 2.0f * s); // smoothstep: 1 at the centre, 0 at the rim, flat at both ends
}

void invertMask(std::vector<float>& mask)
{
    for (float& m : mask) m = 1.0f - std::clamp(m, 0.0f, 1.0f);
}

void Stroke::begin(Mesh& mesh, std::vector<float>* mask, Brush brush, float radius, int symmetryAxis)
{
    mesh_ = &mesh;
    mask_ = mask && mask->size() == mesh.numVerts() ? mask : nullptr;
    brush_ = brush;
    sym_ = symmetryAxis;
    touched_.clear();
    before_.clear();
    seen_.assign(mesh.numVerts(), 0);
    grabbing_ = false;
    grabIdx_.clear();
    // Uniform grid over the vertices (cell ~ brush radius, but bounded so huge meshes stay sparse enough).
    AABB box = mesh.bounds();
    Vec3 size = box.size();
    float maxSide = std::max({size.x, size.y, size.z, 1e-6f});
    cell_ = std::max(radius, maxSide / 256.0f);
    gridLo_ = box.lo;
    gx_ = std::max(1, int(size.x / cell_) + 1);
    gy_ = std::max(1, int(size.y / cell_) + 1);
    gz_ = std::max(1, int(size.z / cell_) + 1);
    const size_t cells = size_t(gx_) * gy_ * gz_;
    cellStart_.assign(cells + 1, 0);
    cellVerts_.resize(mesh.numVerts());
    auto cellOf = [&](const Vec3& p) {
        int x = std::clamp(int((p.x - gridLo_.x) / cell_), 0, gx_ - 1), y = std::clamp(int((p.y - gridLo_.y) / cell_), 0, gy_ - 1),
            z = std::clamp(int((p.z - gridLo_.z) / cell_), 0, gz_ - 1);
        return (size_t(z) * gy_ + y) * gx_ + x;
    };
    std::vector<uint32_t> cellIdx(mesh.numVerts());
    for (uint32_t v = 0; v < mesh.numVerts(); ++v) {
        cellIdx[v] = uint32_t(cellOf(mesh.positions[v]));
        cellStart_[cellIdx[v] + 1]++;
    }
    for (size_t c = 0; c < cells; ++c) cellStart_[c + 1] += cellStart_[c];
    std::vector<uint32_t> fill(cellStart_.begin(), cellStart_.end() - 1);
    for (uint32_t v = 0; v < mesh.numVerts(); ++v) cellVerts_[fill[cellIdx[v]]++] = v;
}

void Stroke::gather(const Vec3& c, float r, std::vector<uint32_t>& out) const
{
    out.clear();
    const Mesh& m = *mesh_;
    // Vertices drift during a stroke; widen the search by one cell to stay safe.
    int x0 = std::max(0, int((c.x - r - gridLo_.x) / cell_) - 1), x1 = std::min(gx_ - 1, int((c.x + r - gridLo_.x) / cell_) + 1);
    int y0 = std::max(0, int((c.y - r - gridLo_.y) / cell_) - 1), y1 = std::min(gy_ - 1, int((c.y + r - gridLo_.y) / cell_) + 1);
    int z0 = std::max(0, int((c.z - r - gridLo_.z) / cell_) - 1), z1 = std::min(gz_ - 1, int((c.z + r - gridLo_.z) / cell_) + 1);
    const float r2 = r * r;
    for (int z = z0; z <= z1; ++z)
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                size_t cidx = (size_t(z) * gy_ + y) * gx_ + x;
                for (uint32_t k = cellStart_[cidx]; k < cellStart_[cidx + 1]; ++k) {
                    uint32_t v = cellVerts_[k];
                    if (length2(m.positions[v] - c) <= r2) out.push_back(v);
                }
            }
}

void Stroke::remember(uint32_t v)
{
    if (seen_[v]) return;
    seen_[v] = 1;
    touched_.push_back(v);
    before_.push_back(mesh_->positions[v]);
}

Vec3 Stroke::vertexNormal(uint32_t v) const
{
    Vec3 n;
    for (uint32_t f : mesh_->vertexFaces(v)) n += mesh_->faceNormalRaw(f);
    return normalize(n);
}

void Stroke::dab(const Dab& d)
{
    if (!mesh_) return;
    apply(d);
    if (sym_ >= 0 && sym_ <= 2) {
        Dab m = d;
        m.center[sym_] = -m.center[sym_];
        if (std::abs(d.center[sym_]) > d.radius * 0.05f || brush_ == Brush::Grab) apply(m);
    }
    mesh_->touchPositions();
}

void Stroke::apply(const Dab& d)
{
    Mesh& m = *mesh_;
    std::vector<uint32_t> verts;
    gather(d.center, d.radius, verts);
    if (verts.empty()) return;
    auto weight = [&](uint32_t v) {
        float w = falloff(length(m.positions[v] - d.center), d.radius);
        if (mask_ && brush_ != Brush::Mask) w *= 1.0f - std::clamp((*mask_)[v], 0.0f, 1.0f);
        return w;
    };

    if (brush_ == Brush::Mask) {
        if (!mask_) return;
        for (uint32_t v : verts) {
            float w = falloff(length(m.positions[v] - d.center), d.radius) * d.strength;
            float& mk = (*mask_)[v];
            mk = std::clamp(mk + (d.invert ? -w : w), 0.0f, 1.0f);
        }
        return;
    }
    if (brush_ == Brush::Grab) {
        if (!grabbing_) {
            // Capture on the first dab (and its mirror when symmetry is on).
            for (uint32_t v : verts) {
                float w = weight(v);
                if (w <= 0) continue;
                grabIdx_.push_back(v);
                grabW_.push_back(w);
                grabStart_.push_back(m.positions[v]);
                grabMirror_.push_back(int8_t(sym_ >= 0 && d.center[sym_] * m.positions[v][sym_] < 0 ? 1 : 0));
                remember(v);
            }
            if (sym_ < 0 || std::abs(d.center[sym_]) <= d.radius * 0.05f) grabbing_ = true;
            else grabbing_ = d.center[sym_] < 0; // first call is the primary dab, second the mirror
        }
        return;
    }

    // Area plane (centre + normal) of the affected region.
    Vec3 areaN, areaC;
    float wsum = 0;
    for (uint32_t v : verts) {
        float w = falloff(length(m.positions[v] - d.center), d.radius);
        areaN += vertexNormal(v) * w;
        areaC += m.positions[v] * w;
        wsum += w;
    }
    if (wsum <= 0) return;
    areaN = normalize(areaN);
    areaC = areaC / wsum;
    const float sign = d.invert ? -1.0f : 1.0f;
    const float amount = d.strength * d.radius * 0.12f;

    std::vector<Vec3> out(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) {
        const uint32_t v = verts[i];
        const Vec3 p = m.positions[v];
        const float w = weight(v);
        Vec3 q = p;
        switch (brush_) {
        case Brush::Draw: q = p + areaN * (amount * w * sign); break;
        case Brush::Inflate: q = p + vertexNormal(v) * (amount * w * sign); break;
        case Brush::Smooth: {
            auto ve = m.vertexEdges(v);
            if (ve.empty()) break;
            Vec3 avg;
            for (uint32_t e : ve) avg += m.positions[m.edgeOther(e, v)];
            avg = avg / float(ve.size());
            q = lerp(p, avg, std::clamp(w * d.strength, 0.0f, 1.0f));
            break;
        }
        case Brush::Flatten: {
            float dist = dot(p - areaC, areaN);
            q = p - areaN * (dist * std::clamp(w * d.strength, 0.0f, 1.0f) * sign);
            break;
        }
        case Brush::Pinch: {
            Vec3 to = d.center - p;
            to = to - areaN * dot(to, areaN); // tangential
            q = p + to * std::clamp(w * d.strength * 0.3f * sign, -1.0f, 1.0f);
            break;
        }
        case Brush::Crease: {
            Vec3 to = d.center - p;
            to = to - areaN * dot(to, areaN);
            q = p + to * std::clamp(w * d.strength * 0.25f, 0.0f, 1.0f) - areaN * (amount * 0.6f * w * sign);
            break;
        }
        default: break;
        }
        out[i] = q;
    }
    for (size_t i = 0; i < verts.size(); ++i) {
        remember(verts[i]);
        m.positions[verts[i]] = out[i];
    }
}

void Stroke::grab(const Vec3& offset)
{
    if (!mesh_ || brush_ != Brush::Grab) return;
    for (size_t k = 0; k < grabIdx_.size(); ++k) {
        Vec3 o = offset;
        if (grabMirror_[k] && sym_ >= 0) o[sym_] = -o[sym_];
        mesh_->positions[grabIdx_[k]] = grabStart_[k] + o * grabW_[k];
    }
    mesh_->touchPositions();
}

void Stroke::end()
{
    mesh_ = nullptr;
    mask_ = nullptr;
    grabbing_ = false;
}

} // namespace facet::sculpt
