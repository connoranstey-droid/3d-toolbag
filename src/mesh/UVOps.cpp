#include "mesh/UVOps.h"
#include "util/Parallel.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>

namespace facet::uv {

namespace {

struct UF {
    std::vector<uint32_t> p;
    explicit UF(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0u); }
    uint32_t find(uint32_t x) { while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; } return x; }
    void unite(uint32_t a, uint32_t b) { a = find(a); b = find(b); if (a != b) p[std::max(a, b)] = std::min(a, b); }
};

IndexList allFaces(const Mesh& m, const IndexList& faces)
{
    if (!faces.empty()) return faces;
    IndexList f(m.numFaces());
    std::iota(f.begin(), f.end(), 0u);
    return f;
}

// Gives the faces their own fresh per-corner uvs computed by fn(face, corner) and welds.
template <class Fn>
void assignPerCorner(Mesh& m, const IndexList& faces, Fn fn)
{
    m.ensureUVs();
    for (uint32_t f : faces)
        for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
            m.faceUVs[h] = uint32_t(m.uvs.size());
            m.uvs.push_back(fn(f, h));
        }
    m.weldUVs(1e-6f);
    m.touchUVs();
}

void fitUnit(Mesh& m, const IndexList& uvIdx)
{
    if (uvIdx.empty()) return;
    Vec2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
    for (uint32_t u : uvIdx) {
        lo = {std::min(lo.x, m.uvs[u].x), std::min(lo.y, m.uvs[u].y)};
        hi = {std::max(hi.x, m.uvs[u].x), std::max(hi.y, m.uvs[u].y)};
    }
    float s = std::max(hi.x - lo.x, hi.y - lo.y);
    if (s < 1e-12f) s = 1;
    for (uint32_t u : uvIdx) m.uvs[u] = (m.uvs[u] - lo) / s;
}

float area2(Vec2 a, Vec2 b, Vec2 c) { return cross2(b - a, c - a); }

} // namespace

IndexList uvsOfFaces(const Mesh& m, const IndexList& faces)
{
    IndexList out;
    if (!m.hasUVs()) return out;
    for (uint32_t f : allFaces(m, faces))
        for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) out.push_back(m.faceUVs[h]);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void planar(Mesh& m, const IndexList& facesIn, Axis axis)
{
    IndexList faces = allFaces(m, facesIn);
    Vec3 n;
    if (axis == Axis::Best) {
        for (uint32_t f : faces) n += m.faceNormalRaw(f);
        n = length(n) > 1e-12f ? normalize(n) : Vec3{0, 1, 0};
    } else {
        n = axis == Axis::X ? Vec3{1, 0, 0} : axis == Axis::Y ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
    }
    Vec3 up = std::abs(n.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{0, 0, -1};
    Vec3 tu = normalize(cross(up, n)), tv = cross(n, tu);
    assignPerCorner(m, faces, [&](uint32_t, uint32_t h) {
        const Vec3& p = m.positions[m.faceVerts[h]];
        return Vec2{dot(p, tu), dot(p, tv)};
    });
    fitUnit(m, uvsOfFaces(m, faces));
}

void cylindrical(Mesh& m, const IndexList& facesIn)
{
    IndexList faces = allFaces(m, facesIn);
    AABB box;
    for (uint32_t f : faces)
        for (uint32_t v : m.faceVertices(f)) box.expand(m.positions[v]);
    Vec3 c = box.center();
    float h = std::max(box.size().y, 1e-6f);
    assignPerCorner(m, faces, [&](uint32_t f, uint32_t hc) {
        const Vec3 p = m.positions[m.faceVerts[hc]] - c;
        float u = std::atan2(-p.z, p.x) / (2 * kPi);
        if (u < 0) u += 1;
        // Seam fix: compare against the face centre's angle.
        Vec3 fc = m.faceCenter(f) - c;
        float uc = std::atan2(-fc.z, fc.x) / (2 * kPi);
        if (uc < 0) uc += 1;
        if (u - uc > 0.5f) u -= 1;
        if (uc - u > 0.5f) u += 1;
        return Vec2{u, (p.y + h / 2) / h};
    });
}

void spherical(Mesh& m, const IndexList& facesIn)
{
    IndexList faces = allFaces(m, facesIn);
    AABB box;
    for (uint32_t f : faces)
        for (uint32_t v : m.faceVertices(f)) box.expand(m.positions[v]);
    Vec3 c = box.center();
    assignPerCorner(m, faces, [&](uint32_t f, uint32_t hc) {
        Vec3 p = normalize(m.positions[m.faceVerts[hc]] - c);
        Vec3 fc = normalize(m.faceCenter(f) - c);
        float uc = std::atan2(-fc.z, fc.x) / (2 * kPi);
        if (uc < 0) uc += 1;
        float u = (std::abs(p.x) + std::abs(p.z) < 1e-6f) ? uc : std::atan2(-p.z, p.x) / (2 * kPi);
        if (u < 0) u += 1;
        if (u - uc > 0.5f) u -= 1;
        if (uc - u > 0.5f) u += 1;
        return Vec2{u, std::asin(std::clamp(p.y, -1.0f, 1.0f)) / kPi + 0.5f};
    });
}

void automatic(Mesh& m, const IndexList& facesIn)
{
    IndexList faces = allFaces(m, facesIn);
    static const Vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::vector<int> label(m.numFaces(), -1);
    for (uint32_t f : faces) {
        Vec3 n = m.faceNormal(f);
        int best = 0;
        for (int i = 1; i < 6; ++i)
            if (dot(n, dirs[i]) > dot(n, dirs[best])) best = i;
        label[f] = best;
    }
    // Connected clusters of equal label.
    UF uf(m.numFaces());
    for (uint32_t f : faces)
        for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
            uint32_t t = m.heTwin[h];
            if (t != kInvalid && label[m.heFace[t]] == label[f]) uf.unite(f, m.heFace[t]);
        }
    m.ensureUVs();
    for (uint32_t f : faces) {
        const Vec3 n = dirs[label[f]];
        Vec3 up = std::abs(n.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{0, 0, -1};
        Vec3 tu = normalize(cross(up, n)), tv = cross(n, tu);
        for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
            const Vec3& p = m.positions[m.faceVerts[h]];
            m.faceUVs[h] = uint32_t(m.uvs.size());
            // Offset each cluster far apart so welding never joins different clusters.
            float off = float(uf.find(f)) * 1000.0f;
            m.uvs.push_back({dot(p, tu) + off, dot(p, tv)});
        }
    }
    m.weldUVs(1e-6f);
    layout(m);
}

std::vector<uint32_t> shellIds(const Mesh& m, uint32_t* count)
{
    std::vector<uint32_t> id(m.uvs.size(), kInvalid);
    if (!m.hasUVs()) { if (count) *count = 0; return id; }
    UF uf(m.uvs.size());
    for (uint32_t f = 0; f < m.numFaces(); ++f)
        for (uint32_t h = m.faceStart[f] + 1; h < m.faceStart[f + 1]; ++h) uf.unite(m.faceUVs[m.faceStart[f]], m.faceUVs[h]);
    std::unordered_map<uint32_t, uint32_t> compact;
    for (uint32_t u = 0; u < m.uvs.size(); ++u) {
        auto [it, ins] = compact.emplace(uf.find(u), uint32_t(compact.size()));
        id[u] = it->second;
    }
    if (count) *count = uint32_t(compact.size());
    return id;
}

IndexList expandToShells(const Mesh& m, const IndexList& uvSel)
{
    auto ids = shellIds(m);
    std::vector<uint8_t> want(m.uvs.size(), 0);
    for (uint32_t u : uvSel)
        if (u < ids.size()) want[ids[u]] = 1;
    IndexList out;
    for (uint32_t u = 0; u < m.uvs.size(); ++u)
        if (want[ids[u]]) out.push_back(u);
    return out;
}

void cutEdges(Mesh& m, const IndexList& edges)
{
    if (!m.hasUVs()) return;
    std::vector<uint8_t> cut(m.numEdges(), 0);
    for (uint32_t e : edges)
        if (e < m.numEdges()) cut[e] = 1;
    // Corners stay connected across uncut edges where they already share a uv.
    UF uf(m.numHalfEdges());
    for (uint32_t e = 0; e < m.numEdges(); ++e) {
        if (cut[e]) continue;
        uint32_t h = m.edges[e].he, t = m.heTwin[h];
        if (t == kInvalid) continue;
        uint32_t ha = h, ta = m.heNext(t), hb = m.heNext(h), tb = t;
        if (m.faceUVs[ha] == m.faceUVs[ta]) uf.unite(ha, ta);
        if (m.faceUVs[hb] == m.faceUVs[tb]) uf.unite(hb, tb);
    }
    // Give every corner group its own uv index.
    std::unordered_map<uint32_t, uint32_t> firstRoot; // uv index -> root that keeps it
    std::unordered_map<uint32_t, uint32_t> rootUV;    // root -> new uv index
    for (uint32_t h = 0; h < m.numHalfEdges(); ++h) {
        uint32_t r = uf.find(h), u = m.faceUVs[h];
        auto it = rootUV.find(r);
        if (it != rootUV.end()) { m.faceUVs[h] = it->second; continue; }
        auto [fr, inserted] = firstRoot.emplace(u, r);
        if (inserted || fr->second == r) { rootUV[r] = u; continue; }
        uint32_t nu = uint32_t(m.uvs.size());
        m.uvs.push_back(m.uvs[u]);
        rootUV[r] = nu;
        m.faceUVs[h] = nu;
    }
    m.touchUVs();
}

void sewEdges(Mesh& m, const IndexList& edges)
{
    if (!m.hasUVs()) return;
    for (uint32_t e : edges) {
        if (e >= m.numEdges()) continue;
        uint32_t h = m.edges[e].he, t = m.heTwin[h];
        if (t == kInvalid) continue;
        uint32_t a1 = m.faceUVs[h], a2 = m.faceUVs[m.heNext(t)];
        uint32_t b1 = m.faceUVs[m.heNext(h)], b2 = m.faceUVs[t];
        if (a1 == a2 && b1 == b2) continue;
        auto ids = shellIds(m);
        if (ids[a1] != ids[a2]) {
            // Similarity transform taking segment (a2,b2) onto (a1,b1), applied to shell 2.
            Vec2 s1 = m.uvs[b1] - m.uvs[a1], s2 = m.uvs[b2] - m.uvs[a2];
            float l1 = s1.length(), l2 = s2.length();
            float scale = l2 > 1e-12f ? l1 / l2 : 1.0f;
            float ang = std::atan2(s1.y, s1.x) - std::atan2(s2.y, s2.x);
            float c = std::cos(ang) * scale, s = std::sin(ang) * scale;
            const Vec2 o2 = m.uvs[a2], o1 = m.uvs[a1];
            const uint32_t shell2 = ids[a2];
            for (uint32_t u = 0; u < m.uvs.size(); ++u)
                if (ids[u] == shell2) {
                    Vec2 d = m.uvs[u] - o2;
                    m.uvs[u] = o1 + Vec2{c * d.x - s * d.y, s * d.x + c * d.y};
                }
        }
        // Merge the edge's uvs (every corner using a2/b2 now uses a1/b1).
        for (uint32_t& u : m.faceUVs) {
            if (u == a2) u = a1;
            else if (u == b2) u = b1;
        }
    }
    m.compactUVs();
}

// ------------------------------------------------------------------------------------------
// LSCM

namespace {

struct Sparse { // row-major list of rows with 6 entries (3 complex coefficients * 2 unknowns)
    std::vector<uint32_t> col;
    std::vector<float> val;
    std::vector<uint32_t> rowStart{0};
    std::vector<float> rhs;
};

} // namespace

int unfold(Mesh& m, const IndexList& uvSel, int maxIterations)
{
    if (!m.hasUVs()) planar(m, {}, Axis::Best);
    uint32_t shellCount = 0;
    auto ids = shellIds(m, &shellCount);
    std::vector<uint8_t> want(shellCount, uvSel.empty() ? 1 : 0);
    for (uint32_t u : uvSel)
        if (u < ids.size()) want[ids[u]] = 1;

    // Corner lists per shell.
    std::vector<std::vector<uint32_t>> shellFaces(shellCount);
    for (uint32_t f = 0; f < m.numFaces(); ++f) shellFaces[ids[m.faceUVs[m.faceStart[f]]]].push_back(f);

    int solved = 0;
    for (uint32_t s = 0; s < shellCount; ++s) {
        if (!want[s] || shellFaces[s].empty()) continue;
        // Local numbering of the shell's uvs.
        std::unordered_map<uint32_t, uint32_t> local;
        std::vector<uint32_t> globalOf;
        std::vector<uint32_t> vertOf;
        for (uint32_t f : shellFaces[s])
            for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) {
                auto [it, ins] = local.emplace(m.faceUVs[h], uint32_t(globalOf.size()));
                if (ins) { globalOf.push_back(m.faceUVs[h]); vertOf.push_back(m.faceVerts[h]); }
            }
        const uint32_t n = uint32_t(globalOf.size());
        if (n < 3) continue;

        // Pins: two far-apart points (in 3D).
        uint32_t p0 = 0, p1 = 0;
        float best = -1;
        for (uint32_t i = 0; i < n; ++i) {
            float d = length2(m.positions[vertOf[i]] - m.positions[vertOf[0]]);
            if (d > best) { best = d; p0 = i; }
        }
        best = -1;
        for (uint32_t i = 0; i < n; ++i) {
            float d = length2(m.positions[vertOf[i]] - m.positions[vertOf[p0]]);
            if (d > best) { best = d; p1 = i; }
        }
        if (p0 == p1) continue;
        Vec2 pin0{0, 0}, pin1{std::sqrt(best), 0};

        // Unknown numbering: free vertices only, u then v interleaved.
        std::vector<int> freeIdx(n, -1);
        uint32_t nf = 0;
        for (uint32_t i = 0; i < n; ++i)
            if (i != p0 && i != p1) freeIdx[i] = int(nf++);
        Sparse A;
        float area3D = 0;
        auto addTri = [&](uint32_t c0, uint32_t c1, uint32_t c2) {
            const uint32_t L[3] = {local[m.faceUVs[c0]], local[m.faceUVs[c1]], local[m.faceUVs[c2]]};
            const Vec3 P0 = m.positions[m.faceVerts[c0]], P1 = m.positions[m.faceVerts[c1]], P2 = m.positions[m.faceVerts[c2]];
            Vec3 e1 = P1 - P0;
            float l1 = length(e1);
            Vec3 nrm = cross(e1, P2 - P0);
            float a = length(nrm) * 0.5f;
            if (a < 1e-14f || l1 < 1e-12f) return;
            area3D += a;
            e1 = e1 / l1;
            Vec3 e2 = normalize(cross(normalize(nrm), e1));
            const Vec2 x[3] = {{0, 0}, {l1, 0}, {dot(P2 - P0, e1), dot(P2 - P0, e2)}};
            const float w = 1.0f / std::sqrt(2.0f * a);
            Vec2 W[3];
            for (int j = 0; j < 3; ++j) W[j] = (x[(j + 2) % 3] - x[(j + 1) % 3]) * w;
            // Two rows: Re and Im of sum W_j * U_j.
            for (int row = 0; row < 2; ++row) {
                float rhs = 0;
                for (int j = 0; j < 3; ++j) {
                    float cu = row == 0 ? W[j].x : W[j].y;  // coefficient of u_j
                    float cv = row == 0 ? -W[j].y : W[j].x; // coefficient of v_j
                    uint32_t li = L[j];
                    if (freeIdx[li] < 0) {
                        Vec2 pv = li == p0 ? pin0 : pin1;
                        rhs -= cu * pv.x + cv * pv.y;
                    } else {
                        A.col.push_back(uint32_t(freeIdx[li]) * 2);
                        A.val.push_back(cu);
                        A.col.push_back(uint32_t(freeIdx[li]) * 2 + 1);
                        A.val.push_back(cv);
                    }
                }
                A.rowStart.push_back(uint32_t(A.col.size()));
                A.rhs.push_back(rhs);
            }
        };
        for (uint32_t f : shellFaces[s])
            for (uint32_t i = 1; i + 1 < m.faceSize(f); ++i) addTri(m.faceStart[f], m.faceStart[f] + i, m.faceStart[f] + i + 1);
        const size_t N = size_t(nf) * 2;
        if (N == 0 || A.rhs.empty()) continue;

        // Assemble the normal matrix N = A^T A (symmetric, sparse) and solve N x = A^T b with
        // conjugate gradients preconditioned by an incomplete Cholesky factorisation IC(0).
        const size_t rows = A.rowStart.size() - 1;
        std::vector<std::vector<std::pair<uint32_t, float>>> acc(N);
        for (size_t r = 0; r < rows; ++r)
            for (uint32_t a = A.rowStart[r]; a < A.rowStart[r + 1]; ++a)
                for (uint32_t b = A.rowStart[r]; b < A.rowStart[r + 1]; ++b) acc[A.col[a]].push_back({A.col[b], A.val[a] * A.val[b]});
        std::vector<uint32_t> nStart(N + 1, 0), nCol;
        std::vector<double> nVal;
        for (size_t i = 0; i < N; ++i) {
            auto& row = acc[i];
            std::sort(row.begin(), row.end(), [](auto& x, auto& y) { return x.first < y.first; });
            for (size_t k = 0; k < row.size();) {
                uint32_t c = row[k].first;
                double v = 0;
                while (k < row.size() && row[k].first == c) v += row[k++].second;
                nCol.push_back(c);
                nVal.push_back(v);
            }
            nStart[i + 1] = uint32_t(nCol.size());
            std::vector<std::pair<uint32_t, float>>().swap(row);
        }
        auto mulN = [&](const std::vector<double>& in, std::vector<double>& outv) {
            outv.resize(N);
            parallelFor(N, [&](size_t i) {
                double sum = 0;
                for (uint32_t k = nStart[i]; k < nStart[i + 1]; ++k) sum += nVal[k] * in[nCol[k]];
                outv[i] = sum;
            }, 2048);
        };
        // IC(0): L has the sparsity of the lower triangle of N (stored by rows, diagonal last).
        std::vector<uint32_t> lStart(N + 1, 0), lCol;
        std::vector<double> lVal;
        for (size_t i = 0; i < N; ++i) {
            for (uint32_t k = nStart[i]; k < nStart[i + 1]; ++k)
                if (nCol[k] <= i) { lCol.push_back(nCol[k]); lVal.push_back(nVal[k]); }
            lStart[i + 1] = uint32_t(lCol.size());
        }
        const std::vector<double> lOrig = lVal;
        auto factor = [&](double shift) {
            lVal = lOrig;
            for (size_t i = 0; i < N; ++i) {
                const uint32_t bi = lStart[i], ei = lStart[i + 1]; // last entry is the diagonal
                for (uint32_t a = bi; a + 1 < ei; ++a) {
                    const uint32_t k = lCol[a];
                    // dot(L[i, :k], L[k, :k]) via merge of sorted rows
                    double dotv = 0;
                    uint32_t p = bi, q = lStart[k];
                    const uint32_t qe = lStart[k + 1] - 1; // exclude diagonal of row k
                    while (p < a && q < qe) {
                        if (lCol[p] == lCol[q]) dotv += lVal[p++] * lVal[q++];
                        else if (lCol[p] < lCol[q]) ++p;
                        else ++q;
                    }
                    lVal[a] = (lVal[a] - dotv) / lVal[lStart[k + 1] - 1];
                }
                double d = lVal[ei - 1] * (1.0 + shift);
                for (uint32_t a = bi; a + 1 < ei; ++a) d -= lVal[a] * lVal[a];
                if (!(d > 1e-300)) return false;
                lVal[ei - 1] = std::sqrt(d);
            }
            return true;
        };
        bool haveIC = false;
        for (double shift : {0.0, 1e-4, 1e-3, 1e-2, 1e-1})
            if (factor(shift)) { haveIC = true; break; }
        std::vector<double> diagInv(N, 1.0);
        for (size_t i = 0; i < N; ++i)
            for (uint32_t k = nStart[i]; k < nStart[i + 1]; ++k)
                if (nCol[k] == i && nVal[k] > 1e-30) diagInv[i] = 1.0 / nVal[k];
        auto precond = [&](const std::vector<double>& r, std::vector<double>& z) {
            z = r;
            if (!haveIC) {
                for (size_t i = 0; i < N; ++i) z[i] *= diagInv[i];
                return;
            }
            for (size_t i = 0; i < N; ++i) { // forward: L y = r
                double v = z[i];
                for (uint32_t a = lStart[i]; a + 1 < lStart[i + 1]; ++a) v -= lVal[a] * z[lCol[a]];
                z[i] = v / lVal[lStart[i + 1] - 1];
            }
            for (size_t i = N; i-- > 0;) { // backward: L^T x = y
                z[i] /= lVal[lStart[i + 1] - 1];
                for (uint32_t a = lStart[i]; a + 1 < lStart[i + 1]; ++a) z[lCol[a]] -= lVal[a] * z[i];
            }
        };

        std::vector<double> x(N, 0.0);
        // Warm start from a planar projection relative to the pins.
        {
            Vec3 a3 = m.positions[vertOf[p0]], b3 = m.positions[vertOf[p1]];
            Vec3 ex = normalize(b3 - a3), nn;
            for (uint32_t f : shellFaces[s]) nn += m.faceNormalRaw(f);
            Vec3 ey = normalize(cross(normalize(nn), ex));
            for (uint32_t i = 0; i < n; ++i)
                if (freeIdx[i] >= 0) {
                    Vec3 d = m.positions[vertOf[i]] - a3;
                    x[size_t(freeIdx[i]) * 2] = dot(d, ex);
                    x[size_t(freeIdx[i]) * 2 + 1] = dot(d, ey);
                }
        }
        std::vector<double> b(N, 0.0);
        for (size_t r = 0; r < rows; ++r)
            for (uint32_t k = A.rowStart[r]; k < A.rowStart[r + 1]; ++k) b[A.col[k]] += double(A.val[k]) * A.rhs[r];
        std::vector<double> r(N), z, p, Ap;
        mulN(x, Ap);
        double bnorm = 0;
        for (size_t i = 0; i < N; ++i) { r[i] = b[i] - Ap[i]; bnorm += b[i] * b[i]; }
        bnorm = std::max(bnorm, 1e-30);
        precond(r, z);
        p = z;
        double rz = 0;
        for (size_t i = 0; i < N; ++i) rz += r[i] * z[i];
        int it = 0;
        for (; it < maxIterations; ++it) {
            mulN(p, Ap);
            double pAp = 0;
            for (size_t i = 0; i < N; ++i) pAp += p[i] * Ap[i];
            if (std::abs(pAp) < 1e-300) break;
            const double alpha = rz / pAp;
            double rr = 0;
            for (size_t i = 0; i < N; ++i) { x[i] += alpha * p[i]; r[i] -= alpha * Ap[i]; rr += r[i] * r[i]; }
            if (rr / bnorm < 1e-11) break;
            precond(r, z);
            double rzNew = 0;
            for (size_t i = 0; i < N; ++i) rzNew += r[i] * z[i];
            const double beta = rzNew / rz;
            rz = rzNew;
            for (size_t i = 0; i < N; ++i) p[i] = z[i] + beta * p[i];
        }

        // Write back; fix orientation; restore 3D area; keep the old shell centre.
        Vec2 oldC;
        for (uint32_t g : globalOf) oldC += m.uvs[g];
        oldC = oldC / float(n);
        std::vector<Vec2> res(n);
        for (uint32_t i = 0; i < n; ++i)
            res[i] = i == p0 ? pin0 : i == p1 ? pin1 : Vec2{float(x[size_t(freeIdx[i]) * 2]), float(x[size_t(freeIdx[i]) * 2 + 1])};
        float a2 = 0;
        for (uint32_t f : shellFaces[s])
            for (uint32_t i = 1; i + 1 < m.faceSize(f); ++i) {
                uint32_t b0 = m.faceStart[f];
                a2 += 0.5f * area2(res[local[m.faceUVs[b0]]], res[local[m.faceUVs[b0 + i]]], res[local[m.faceUVs[b0 + i + 1]]]);
            }
        float flipX = a2 < 0 ? -1.0f : 1.0f;
        float scale = std::abs(a2) > 1e-20f ? std::sqrt(area3D / std::abs(a2)) : 1.0f;
        Vec2 c;
        for (auto& r2 : res) { r2 = Vec2{r2.x * flipX, r2.y} * scale; c += r2; }
        c = c / float(n);
        for (uint32_t i = 0; i < n; ++i) m.uvs[globalOf[i]] = res[i] - c + oldC;
        ++solved;
    }
    m.touchUVs();
    return solved;
}

void layout(Mesh& m, float padding)
{
    if (!m.hasUVs()) return;
    uint32_t count = 0;
    auto ids = shellIds(m, &count);
    if (!count) return;
    std::vector<std::vector<uint32_t>> members(count);
    for (uint32_t u = 0; u < m.uvs.size(); ++u) members[ids[u]].push_back(u);
    std::vector<float> a3(count, 0), a2(count, 0);
    for (uint32_t f = 0; f < m.numFaces(); ++f) {
        uint32_t s = ids[m.faceUVs[m.faceStart[f]]];
        a3[s] += length(m.faceNormalRaw(f)) * 0.5f;
        for (uint32_t i = 1; i + 1 < m.faceSize(f); ++i) {
            uint32_t b0 = m.faceStart[f];
            a2[s] += std::abs(0.5f * area2(m.cornerUV(b0), m.cornerUV(b0 + i), m.cornerUV(b0 + i + 1)));
        }
    }
    struct Box { uint32_t shell; float w, h; Vec2 lo; };
    std::vector<Box> boxes;
    for (uint32_t s = 0; s < count; ++s) {
        auto& mem = members[s];
        // Uniform texel density.
        float k = a2[s] > 1e-20f ? std::sqrt(a3[s] / a2[s]) : 1.0f;
        for (uint32_t u : mem) m.uvs[u] = m.uvs[u] * k;
        // Rotation giving the smallest bounding box (coarse search).
        Vec2 c;
        for (uint32_t u : mem) c += m.uvs[u];
        c = c / float(mem.size());
        float bestA = 1e30f, bestAng = 0;
        for (int deg = 0; deg < 90; deg += 1) {
            float cs = std::cos(radians(float(deg))), sn = std::sin(radians(float(deg)));
            Vec2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
            for (uint32_t u : mem) {
                Vec2 d = m.uvs[u] - c, r{cs * d.x - sn * d.y, sn * d.x + cs * d.y};
                lo = {std::min(lo.x, r.x), std::min(lo.y, r.y)};
                hi = {std::max(hi.x, r.x), std::max(hi.y, r.y)};
            }
            float area = (hi.x - lo.x) * (hi.y - lo.y);
            if (area < bestA * 0.98f) { bestA = area; bestAng = float(deg); }
        }
        float cs = std::cos(radians(bestAng)), sn = std::sin(radians(bestAng));
        Vec2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
        for (uint32_t u : mem) {
            Vec2 d = m.uvs[u] - c;
            m.uvs[u] = {cs * d.x - sn * d.y, sn * d.x + cs * d.y};
            lo = {std::min(lo.x, m.uvs[u].x), std::min(lo.y, m.uvs[u].y)};
            hi = {std::max(hi.x, m.uvs[u].x), std::max(hi.y, m.uvs[u].y)};
        }
        // Keep shells landscape.
        if (hi.y - lo.y > hi.x - lo.x) {
            for (uint32_t u : mem) m.uvs[u] = {-m.uvs[u].y, m.uvs[u].x};
            lo = {1e30f, 1e30f};
            hi = {-1e30f, -1e30f};
            for (uint32_t u : mem) {
                lo = {std::min(lo.x, m.uvs[u].x), std::min(lo.y, m.uvs[u].y)};
                hi = {std::max(hi.x, m.uvs[u].x), std::max(hi.y, m.uvs[u].y)};
            }
        }
        boxes.push_back({s, hi.x - lo.x, hi.y - lo.y, lo});
    }
    // Shelf packing, tallest first.
    std::sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) { return a.h > b.h; });
    float total = 0, maxW = 0;
    for (auto& b : boxes) { total += (b.w + 1e-6f) * (b.h + 1e-6f); maxW = std::max(maxW, b.w); }
    // Try a few shelf widths and keep the squarest result.
    float bestScale = 0;
    std::vector<Vec2> bestPos;
    for (float f : {0.8f, 1.0f, 1.2f, 1.5f, 2.0f}) {
        float width = std::max(maxW, std::sqrt(total) * f);
        float gap = std::sqrt(total) * padding * 2.0f;
        std::vector<Vec2> pos(boxes.size());
        float x = 0, y = 0, rowH = 0, usedW = 0;
        for (size_t i = 0; i < boxes.size(); ++i) {
            if (x > 0 && x + boxes[i].w > width) { x = 0; y += rowH + gap; rowH = 0; }
            pos[i] = {x, y};
            x += boxes[i].w + gap;
            usedW = std::max(usedW, x);
            rowH = std::max(rowH, boxes[i].h);
        }
        float extent = std::max(usedW, y + rowH);
        float scale = extent > 0 ? 1.0f / extent : 1.0f;
        if (scale > bestScale) { bestScale = scale; bestPos = pos; }
    }
    const float inner = 1.0f - 2.0f * padding;
    for (size_t i = 0; i < boxes.size(); ++i)
        for (uint32_t u : members[boxes[i].shell])
            m.uvs[u] = Vec2{padding, padding} + (m.uvs[u] - boxes[i].lo + bestPos[i]) * (bestScale * inner);
    m.touchUVs();
}

void normalize(Mesh& m, const IndexList& uvSel)
{
    if (!m.hasUVs()) return;
    IndexList sel = uvSel;
    if (sel.empty()) { sel.resize(m.uvs.size()); std::iota(sel.begin(), sel.end(), 0u); }
    fitUnit(m, sel);
    m.touchUVs();
}

Vec2 center(const Mesh& m, const IndexList& uvSel)
{
    Vec2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
    for (uint32_t u : uvSel) {
        lo = {std::min(lo.x, m.uvs[u].x), std::min(lo.y, m.uvs[u].y)};
        hi = {std::max(hi.x, m.uvs[u].x), std::max(hi.y, m.uvs[u].y)};
    }
    return uvSel.empty() ? Vec2{0.5f, 0.5f} : (lo + hi) * 0.5f;
}

void flip(Mesh& m, const IndexList& uvSelIn, bool horizontal)
{
    IndexList sel = uvSelIn.empty() ? uvsOfFaces(m, {}) : uvSelIn;
    Vec2 c = center(m, sel);
    for (uint32_t u : sel) {
        if (horizontal) m.uvs[u].x = 2 * c.x - m.uvs[u].x;
        else m.uvs[u].y = 2 * c.y - m.uvs[u].y;
    }
    m.touchUVs();
}

void rotate(Mesh& m, const IndexList& uvSelIn, float degrees)
{
    IndexList sel = uvSelIn.empty() ? uvsOfFaces(m, {}) : uvSelIn;
    Vec2 c = center(m, sel);
    float cs = std::cos(radians(degrees)), sn = std::sin(radians(degrees));
    for (uint32_t u : sel) {
        Vec2 d = m.uvs[u] - c;
        m.uvs[u] = c + Vec2{cs * d.x - sn * d.y, sn * d.x + cs * d.y};
    }
    m.touchUVs();
}

Quality measure(const Mesh& m, const Mat4& world, const IndexList& facesIn)
{
    Quality q;
    if (!m.hasUVs()) return q;
    IndexList faces = allFaces(m, facesIn);
    auto ids = shellIds(m);
    std::unordered_map<uint32_t, std::pair<double, double>> shellArea; // shell -> (a3, a2)
    double A3 = 0, A2 = 0;
    struct Tri { float a3, a2, ratio; };
    std::vector<Tri> tris;
    tris.reserve(faces.size() * 2);
    for (uint32_t f : faces) {
        const uint32_t b = m.faceStart[f];
        float fa2 = 0;
        for (uint32_t i = 1; i + 1 < m.faceSize(f); ++i) {
            const uint32_t c[3] = {b, b + i, b + i + 1};
            Vec3 P[3];
            Vec2 U[3];
            for (int k = 0; k < 3; ++k) { P[k] = world.transformPoint(m.positions[m.faceVerts[c[k]]]); U[k] = m.cornerUV(c[k]); }
            Vec3 e1 = P[1] - P[0], e2 = P[2] - P[0];
            float a3 = 0.5f * length(cross(e1, e2));
            float a2 = 0.5f * cross2(U[1] - U[0], U[2] - U[0]);
            fa2 += a2;
            if (a3 < 1e-20f) continue;
            // Jacobian of the 3D->uv map in the triangle's own 2D frame.
            float l = length(e1);
            Vec3 ex = e1 / l, ey = normalize(cross(normalize(cross(e1, e2)), ex));
            float x2 = l, x3 = dot(e2, ex), y3 = dot(e2, ey);
            Vec2 du1 = U[1] - U[0], du2 = U[2] - U[0];
            // J * [x2 x3; 0 y3] = [du1 du2]
            float j00 = du1.x / x2, j10 = du1.y / x2;
            float j01 = (du2.x - j00 * x3) / y3, j11 = (du2.y - j10 * x3) / y3;
            float a = j00 * j00 + j10 * j10, c2 = j01 * j01 + j11 * j11, bb = j00 * j01 + j10 * j11;
            float tr = a + c2, det = std::sqrt(std::max(0.0f, (a - c2) * (a - c2) + 4 * bb * bb));
            float s1 = std::sqrt(std::max(0.0f, (tr + det) * 0.5f)), s2 = std::sqrt(std::max(0.0f, (tr - det) * 0.5f));
            tris.push_back({a3, std::abs(a2), s2 > 1e-20f ? s1 / s2 : 1e6f});
            A3 += a3;
            A2 += std::abs(a2);
            auto& sa = shellArea[ids[m.faceUVs[b]]];
            sa.first += a3;
            sa.second += std::abs(a2);
        }
        if (fa2 < 0) ++q.flippedFaces;
    }
    q.shells = uint32_t(shellArea.size());
    if (A3 <= 0 || A2 <= 0) return q;
    double ad = 0, ang = 0;
    const double norm = A2 / A3;
    for (const Tri& t : tris) {
        double s = (t.a2 / t.a3) / norm;
        ad += t.a3 * std::max(s, s > 1e-12 ? 1.0 / s : 1e6);
        ang += t.a3 * std::min(double(t.ratio), 1e3);
    }
    q.areaDistortion = float(ad / A3);
    q.angleDistortion = float(ang / A3);
    // Texel density spread between shells.
    double mean = 0, var = 0;
    for (auto& [id, sa] : shellArea) mean += sa.first * std::sqrt(sa.second / std::max(sa.first, 1e-30));
    mean /= A3;
    for (auto& [id, sa] : shellArea) {
        double d = std::sqrt(sa.second / std::max(sa.first, 1e-30)) - mean;
        var += sa.first * d * d;
    }
    q.densitySpread = mean > 0 ? float(std::sqrt(var / A3) / mean) : 0.0f;
    // Coverage / overlap on a 512^2 raster of the 0..1 tile (texel centres).
    const int R = 512;
    std::vector<uint16_t> cover(size_t(R) * R, 0);
    for (uint32_t f : faces) {
        const uint32_t b = m.faceStart[f];
        for (uint32_t i = 1; i + 1 < m.faceSize(f); ++i) {
            Vec2 a = m.cornerUV(b) * float(R), c = m.cornerUV(b + i) * float(R), d = m.cornerUV(b + i + 1) * float(R);
            float area = cross2(c - a, d - a);
            if (std::abs(area) < 1e-12f) continue;
            int x0 = std::max(0, int(std::floor(std::min({a.x, c.x, d.x})))), x1 = std::min(R - 1, int(std::ceil(std::max({a.x, c.x, d.x}))));
            int y0 = std::max(0, int(std::floor(std::min({a.y, c.y, d.y})))), y1 = std::min(R - 1, int(std::ceil(std::max({a.y, c.y, d.y}))));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    Vec2 p{x + 0.5f, y + 0.5f};
                    float w0 = cross2(c - p, d - p), w1 = cross2(d - p, a - p), w2 = cross2(a - p, c - p);
                    bool inside = area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0);
                    if (inside && cover[size_t(y) * R + x] < 60000) cover[size_t(y) * R + x]++;
                }
        }
    }
    size_t covered = 0, multi = 0;
    for (uint16_t c : cover) { covered += c > 0; multi += c > 1; }
    q.efficiency = float(covered) / float(R * R);
    q.overlap = covered ? float(multi) / float(covered) : 0.0f;
    return q;
}

float flippedRatio(const Mesh& m)
{
    if (!m.hasUVs() || !m.numFaces()) return 0;
    uint32_t flipped = 0;
    for (uint32_t f = 0; f < m.numFaces(); ++f) {
        float a = 0;
        uint32_t b0 = m.faceStart[f];
        for (uint32_t i = 1; i + 1 < m.faceSize(f); ++i) a += area2(m.cornerUV(b0), m.cornerUV(b0 + i), m.cornerUV(b0 + i + 1));
        flipped += a < 0;
    }
    return float(flipped) / float(m.numFaces());
}

} // namespace facet::uv
