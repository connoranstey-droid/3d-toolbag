#include "mesh/Subdivision.h"
#include "util/Parallel.h"

namespace facet {

static constexpr float kInfiniteCrease = 10.0f;

void subdivideTopology(const Mesh& p, Mesh& c)
{
    const uint32_t V = p.numVerts(), E = p.numEdges(), H = p.numHalfEdges();
    c.clear();
    c.positions.resize(V + E + p.numFaces());
    c.faceVerts.resize(size_t(H) * 4);
    c.faceStart.resize(size_t(H) + 1);
    parallelFor(H, [&](size_t hi) {
        uint32_t h = uint32_t(hi);
        uint32_t* q = &c.faceVerts[size_t(h) * 4];
        q[0] = p.faceVerts[h];
        q[1] = V + p.heEdge[h];
        q[2] = V + E + p.heFace[h];
        q[3] = V + p.heEdge[p.hePrev(h)];
        c.faceStart[h + 1] = uint32_t(h + 1) * 4;
    });
    c.faceStart[0] = 0;
    c.buildTopology();
    subdivideUVs(p, c);

    // Semi-sharp creases lose one unit of sharpness per level (Pixar / OpenSubdiv rule).
    if (p.hasCreases()) {
        for (uint32_t e = 0; e < E; ++e) {
            float s = p.edgeCrease[e];
            if (s <= 0.0f) continue;
            float cs = s >= kInfiniteCrease ? s : std::max(0.0f, s - 1.0f);
            if (cs <= 0.0f) continue;
            c.setCrease(p.edges[e].v0, V + e, cs);
            c.setCrease(V + e, p.edges[e].v1, cs);
        }
    }
}

void subdivideUVs(const Mesh& p, Mesh& c)
{
    if (!p.hasUVs()) { c.uvs.clear(); c.faceUVs.clear(); return; }
    const uint32_t H = p.numHalfEdges();
    std::vector<uint32_t> euv(H, kInvalid);
    c.uvs = p.uvs; // vertex corners keep their uv (linear rule)
    for (uint32_t h = 0; h < H; ++h) {
        uint32_t t = p.heTwin[h];
        bool shared = t != kInvalid && p.faceUVs[h] == p.faceUVs[p.heNext(t)] && p.faceUVs[p.heNext(h)] == p.faceUVs[t];
        if (shared && euv[t] != kInvalid) { euv[h] = euv[t]; continue; }
        euv[h] = uint32_t(c.uvs.size());
        c.uvs.push_back((p.cornerUV(h) + p.cornerUV(p.heNext(h))) * 0.5f);
    }
    const uint32_t fBase = uint32_t(c.uvs.size());
    for (uint32_t f = 0; f < p.numFaces(); ++f) {
        Vec2 a;
        for (uint32_t h = p.faceStart[f]; h < p.faceStart[f + 1]; ++h) a += p.cornerUV(h);
        c.uvs.push_back(a / float(p.faceSize(f)));
    }
    c.faceUVs.resize(size_t(H) * 4);
    for (uint32_t h = 0; h < H; ++h) {
        uint32_t* q = &c.faceUVs[size_t(h) * 4];
        q[0] = p.faceUVs[h];
        q[1] = euv[h];
        q[2] = fBase + p.heFace[h];
        q[3] = euv[p.hePrev(h)];
    }
    c.touchUVs();
}

void subdividePositions(const Mesh& p, Mesh& c)
{
    const uint32_t V = p.numVerts(), E = p.numEdges(), F = p.numFaces();
    const auto& P = p.positions;
    auto& C = c.positions;
    const bool creases = p.hasCreases();

    parallelFor(F, [&](size_t f) { C[V + E + f] = p.faceCenter(uint32_t(f)); });

    parallelFor(E, [&](size_t ei) {
        const MeshEdge& ed = p.edges[ei];
        Vec3 mid = (P[ed.v0] + P[ed.v1]) * 0.5f;
        if (ed.faceCount != 2) { C[V + ei] = mid; return; }
        float s = creases ? p.edgeCrease[ei] : 0.0f;
        if (s >= 1.0f) { C[V + ei] = mid; return; }
        uint32_t h = ed.he, t = p.heTwin[h];
        Vec3 smooth = (P[ed.v0] + P[ed.v1] + C[V + E + p.heFace[h]] +
                       C[V + E + (t != kInvalid ? p.heFace[t] : p.heFace[h])]) * 0.25f;
        C[V + ei] = s > 0.0f ? lerp(smooth, mid, s) : smooth;
    });

    parallelFor(V, [&](size_t vi) {
        const uint32_t v = uint32_t(vi);
        auto ve = p.vertexEdges(v);
        const uint32_t n = uint32_t(ve.size());
        if (n == 0) { C[v] = P[v]; return; }

        uint32_t sharpCount = 0, sharpA = kInvalid, sharpB = kInvalid;
        float sharpSum = 0.0f;
        bool border = false;
        for (uint32_t e : ve) {
            bool isBorder = p.edges[e].faceCount != 2;
            float s = isBorder ? kInfiniteCrease : (creases ? p.edgeCrease[e] : 0.0f);
            if (s > 0.0f) {
                border |= isBorder;
                sharpSum += std::min(s, kInfiniteCrease);
                if (sharpCount == 0) sharpA = p.edgeOther(e, v);
                else if (sharpCount == 1) sharpB = p.edgeOther(e, v);
                ++sharpCount;
            }
        }

        auto smoothRule = [&]() {
            Vec3 fsum, rsum;
            auto vf = p.vertexFaces(v);
            for (uint32_t f : vf) fsum += C[V + E + f];
            for (uint32_t e : ve) rsum += (P[v] + P[p.edgeOther(e, v)]) * 0.5f;
            Vec3 Fa = fsum / float(vf.size()), Ra = rsum / float(n);
            return (Fa + Ra * 2.0f + P[v] * float(int(n) - 3)) / float(n);
        };

        if (sharpCount < 2) { C[v] = smoothRule(); return; }
        Vec3 sharp;
        if (sharpCount == 2) {
            // Border corner (vertex used by a single face) stays pinned.
            if (border && p.vertexFaces(v).size() == 1) sharp = P[v];
            else sharp = (P[v] * 6.0f + P[sharpA] + P[sharpB]) * 0.125f;
        } else {
            sharp = P[v];
        }
        float avg = sharpSum / float(sharpCount);
        C[v] = (!border && avg < 1.0f) ? lerp(smoothRule(), sharp, avg) : sharp;
    });

    c.touchPositions();
}

Mesh subdivide(const Mesh& cage, int levels)
{
    Mesh cur = cage;
    for (int i = 0; i < levels; ++i) {
        Mesh next;
        subdivideTopology(cur, next);
        subdividePositions(cur, next);
        cur = std::move(next);
    }
    return cur;
}

const Mesh& SubdivSurface::evaluate(const Mesh& cage, int levels)
{
    levels = std::max(0, std::min(levels, 6));
    if (levels == 0) return cage;
    bool topoChanged = cage.topologyId != topoId_ || levels != levelCount_;
    if (topoChanged) {
        levels_.assign(levels, Mesh());
        const Mesh* parent = &cage;
        faceOrigin_.resize(cage.numFaces());
        for (uint32_t f = 0; f < cage.numFaces(); ++f) faceOrigin_[f] = f;
        for (int l = 0; l < levels; ++l) {
            subdivideTopology(*parent, levels_[l]);
            std::vector<uint32_t> next(levels_[l].numFaces());
            for (uint32_t cf = 0; cf < next.size(); ++cf) next[cf] = faceOrigin_[parent->heFace[cf]];
            faceOrigin_ = std::move(next);
            parent = &levels_[l];
        }
        topoId_ = cage.topologyId;
        levelCount_ = levels;
        posId_ = 0;
        uvId_ = cage.uvId;
    }
    if (cage.uvId != uvId_) {
        const Mesh* parent = &cage;
        for (int l = 0; l < levels; ++l) {
            subdivideUVs(*parent, levels_[l]);
            parent = &levels_[l];
        }
        uvId_ = cage.uvId;
    }
    if (cage.positionId != posId_) {
        const Mesh* parent = &cage;
        for (int l = 0; l < levels; ++l) {
            subdividePositions(*parent, levels_[l]);
            parent = &levels_[l];
        }
        posId_ = cage.positionId;
    }
    return levels_.back();
}

} // namespace facet
