#include "mesh/BVH.h"

#include <algorithm>

namespace facet {

void TriBVH::build(const Mesh& m)
{
    nodes_.clear();
    v0_.clear(); v1_.clear(); v2_.clear(); face_.clear(); centroid_.clear();
    for (uint32_t f = 0; f < m.numFaces(); ++f) {
        auto fv = m.faceVertices(f);
        for (size_t i = 1; i + 1 < fv.size(); ++i) {
            v0_.push_back(m.positions[fv[0]]);
            v1_.push_back(m.positions[fv[i]]);
            v2_.push_back(m.positions[fv[i + 1]]);
            face_.push_back(f);
        }
    }
    const uint32_t n = uint32_t(v0_.size());
    topologyId = m.topologyId;
    positionId = m.positionId;
    if (!n) return;
    centroid_.resize(n);
    for (uint32_t i = 0; i < n; ++i) centroid_[i] = (v0_[i] + v1_[i] + v2_[i]) / 3.0f;
    order_.resize(n);
    for (uint32_t i = 0; i < n; ++i) order_[i] = i;
    nodes_.reserve(size_t(n) * 2 / 3 + 1);
    buildNode(0, n, 0);
    // Reorder triangles into leaf order.
    auto reorder = [&](std::vector<Vec3>& a) {
        std::vector<Vec3> b(a.size());
        for (uint32_t i = 0; i < n; ++i) b[i] = a[order_[i]];
        a = std::move(b);
    };
    reorder(v0_); reorder(v1_); reorder(v2_);
    std::vector<uint32_t> f2(n);
    for (uint32_t i = 0; i < n; ++i) f2[i] = face_[order_[i]];
    face_ = std::move(f2);
    centroid_.clear();
    centroid_.shrink_to_fit();
}

uint32_t TriBVH::buildNode(uint32_t start, uint32_t end, int depth)
{
    const uint32_t idx = uint32_t(nodes_.size());
    nodes_.emplace_back();
    AABB box, cbox;
    for (uint32_t i = start; i < end; ++i) {
        uint32_t t = order_[i];
        box.expand(v0_[t]); box.expand(v1_[t]); box.expand(v2_[t]);
        cbox.expand(centroid_[t]);
    }
    nodes_[idx].box = box;
    if (end - start <= 4 || depth > 48) {
        nodes_[idx].start = start;
        nodes_[idx].count = end - start;
        return idx;
    }
    Vec3 ext = cbox.size();
    int axis = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
    uint32_t mid = (start + end) / 2;
    std::nth_element(order_.begin() + start, order_.begin() + mid, order_.begin() + end,
                     [&](uint32_t a, uint32_t b) { return centroid_[a][axis] < centroid_[b][axis]; });
    uint32_t l = buildNode(start, mid, depth + 1);
    uint32_t r = buildNode(mid, end, depth + 1);
    nodes_[idx].left = l;
    nodes_[idx].right = r;
    return idx;
}

static bool rayBox(const Ray& r, const Vec3& inv, const AABB& b, float tmax, float& tnear)
{
    float t0 = 0, t1 = tmax;
    for (int a = 0; a < 3; ++a) {
        float tn = (b.lo[a] - r.origin[a]) * inv[a], tf = (b.hi[a] - r.origin[a]) * inv[a];
        if (tn > tf) std::swap(tn, tf);
        t0 = std::max(t0, tn);
        t1 = std::min(t1, tf);
        if (t0 > t1) return false;
    }
    tnear = t0;
    return true;
}

TriBVH::Hit TriBVH::raycast(const Ray& ray) const
{
    Hit best;
    if (nodes_.empty()) return best;
    best.t = 1e30f;
    Vec3 inv{1.0f / (std::abs(ray.dir.x) > 1e-12f ? ray.dir.x : 1e-12f), 1.0f / (std::abs(ray.dir.y) > 1e-12f ? ray.dir.y : 1e-12f),
             1.0f / (std::abs(ray.dir.z) > 1e-12f ? ray.dir.z : 1e-12f)};
    uint32_t stack[128];
    int sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const Node& nd = nodes_[stack[--sp]];
        float tn;
        if (!rayBox(ray, inv, nd.box, best.t, tn)) continue;
        if (nd.count) {
            for (uint32_t i = nd.start; i < nd.start + nd.count; ++i) {
                // Moller-Trumbore, two-sided.
                Vec3 e1 = v1_[i] - v0_[i], e2 = v2_[i] - v0_[i];
                Vec3 pv = cross(ray.dir, e2);
                float det = dot(e1, pv);
                if (std::abs(det) < 1e-14f) continue;
                float id = 1.0f / det;
                Vec3 tv = ray.origin - v0_[i];
                float u = dot(tv, pv) * id;
                if (u < 0 || u > 1) continue;
                Vec3 qv = cross(tv, e1);
                float v = dot(ray.dir, qv) * id;
                if (v < 0 || u + v > 1) continue;
                float t = dot(e2, qv) * id;
                if (t > 1e-6f && t < best.t) {
                    best.hit = true;
                    best.t = t;
                    best.face = face_[i];
                    best.normal = normalize(cross(e1, e2));
                }
            }
        } else if (sp < 126) {
            stack[sp++] = nd.left;
            stack[sp++] = nd.right;
        }
    }
    if (best.hit) best.point = ray.origin + ray.dir * best.t;
    return best;
}

// Closest point on triangle (Ericson, Real-Time Collision Detection 5.1.5).
static Vec3 closestOnTri(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c)
{
    Vec3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    Vec3 bp = p - b;
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    Vec3 cp = p - c;
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    float den = 1.0f / (va + vb + vc);
    return a + ab * (vb * den) + ac * (vc * den);
}

static float boxDist2(const AABB& b, const Vec3& p)
{
    float d = 0;
    for (int a = 0; a < 3; ++a) {
        float v = p[a] < b.lo[a] ? b.lo[a] - p[a] : p[a] > b.hi[a] ? p[a] - b.hi[a] : 0;
        d += v * v;
    }
    return d;
}

TriBVH::Hit TriBVH::closest(const Vec3& p, float maxDist) const
{
    Hit best;
    if (nodes_.empty()) return best;
    float bestD = maxDist * maxDist;
    uint32_t stack[128];
    int sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const Node& nd = nodes_[stack[--sp]];
        if (boxDist2(nd.box, p) > bestD) continue;
        if (nd.count) {
            for (uint32_t i = nd.start; i < nd.start + nd.count; ++i) {
                Vec3 q = closestOnTri(p, v0_[i], v1_[i], v2_[i]);
                float d = length2(q - p);
                if (d < bestD) {
                    bestD = d;
                    best.hit = true;
                    best.point = q;
                    best.face = face_[i];
                    best.normal = normalize(cross(v1_[i] - v0_[i], v2_[i] - v0_[i]));
                }
            }
        } else if (sp < 126) {
            // Visit the nearer child last (popped first).
            uint32_t a = nd.left, b = nd.right;
            if (boxDist2(nodes_[a].box, p) < boxDist2(nodes_[b].box, p)) std::swap(a, b);
            stack[sp++] = a;
            stack[sp++] = b;
        }
    }
    if (best.hit) best.t = std::sqrt(bestD);
    return best;
}

} // namespace facet
