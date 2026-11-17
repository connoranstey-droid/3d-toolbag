// Minimal self-contained test runner for the geometry core (no Qt required).
#include "mesh/Mesh.h"
#include "mesh/MeshOps.h"
#include "mesh/ObjIO.h"
#include "mesh/Primitives.h"
#include "mesh/Subdivision.h"
#include "mesh/UVOps.h"
#include "mesh/BVH.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

using namespace facet;
using ops::IndexList;

static int g_fail = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            ++g_fail;                                                                  \
        }                                                                              \
    } while (0)
#define CHECK_EQ(a, b)                                                                                          \
    do {                                                                                                        \
        auto _a = (a); auto _b = (b);                                                                           \
        if (!(_a == _b)) {                                                                                      \
            std::printf("  FAILED %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b,               \
                        (long long)_a, (long long)_b);                                                          \
            ++g_fail;                                                                                           \
        }                                                                                                       \
    } while (0)

static int euler(const Mesh& m) { return int(m.numVerts()) - int(m.numEdges()) + int(m.numFaces()); }

static bool manifoldClosed(const Mesh& m)
{
    for (const auto& e : m.edges)
        if (e.faceCount != 2) return false;
    for (uint32_t h = 0; h < m.numHalfEdges(); ++h) {
        uint32_t t = m.heTwin[h];
        if (t == kInvalid || m.heTwin[t] != h || m.heFrom(t) != m.heTo(h)) return false;
    }
    return true;
}

static bool outwardNormals(const Mesh& m)
{
    Vec3 c = m.bounds().center();
    for (uint32_t f = 0; f < m.numFaces(); ++f)
        if (dot(m.faceNormalRaw(f), m.faceCenter(f) - c) <= 0) return false;
    return true;
}

static void testPrimitives()
{
    Mesh c = prim::cube();
    CHECK_EQ(c.numVerts(), 8u); CHECK_EQ(c.numEdges(), 12u); CHECK_EQ(c.numFaces(), 6u);
    CHECK(manifoldClosed(c)); CHECK(outwardNormals(c));
    Mesh c3 = prim::cube(2, 2, 2, 3, 3, 3);
    CHECK_EQ(c3.numFaces(), 54u); CHECK_EQ(euler(c3), 2); CHECK(manifoldClosed(c3)); CHECK(outwardNormals(c3));
    Mesh s = prim::sphere(1, 20, 20);
    CHECK_EQ(euler(s), 2); CHECK(manifoldClosed(s)); CHECK(outwardNormals(s));
    Mesh cy = prim::cylinder();
    CHECK_EQ(euler(cy), 2); CHECK(manifoldClosed(cy)); CHECK(outwardNormals(cy));
    Mesh co = prim::cone();
    CHECK_EQ(euler(co), 2); CHECK(manifoldClosed(co)); CHECK(outwardNormals(co));
    Mesh t = prim::torus();
    CHECK_EQ(euler(t), 0); CHECK(manifoldClosed(t));
    Mesh p = prim::plane(1, 1, 4, 4);
    CHECK_EQ(p.numFaces(), 16u); CHECK_EQ(euler(p), 1);
    CHECK(p.faceNormal(0).y > 0.99f);
}

static void testSubdivision()
{
    Mesh c = prim::cube();
    Mesh s1 = subdivide(c, 1);
    CHECK_EQ(s1.numFaces(), 24u); CHECK_EQ(s1.numVerts(), 26u); CHECK(manifoldClosed(s1)); CHECK(outwardNormals(s1));
    Mesh s3 = subdivide(c, 3);
    CHECK_EQ(s3.numFaces(), 384u); CHECK_EQ(euler(s3), 2);
    // Smooth cube shrinks toward a sphere-ish shape; corners pull in.
    float maxc = 0;
    for (auto& p : s3.positions) maxc = std::max(maxc, std::abs(p.x));
    CHECK(maxc < 0.5f && maxc > 0.3f);
    // Infinitely sharp creases on every edge keep the cube's shape.
    Mesh hard = c;
    for (uint32_t e = 0; e < hard.numEdges(); ++e) hard.edgeCrease[e] = 10.0f;
    Mesh hs = subdivide(hard, 2);
    float mx = 0;
    for (auto& p : hs.positions) mx = std::max(mx, std::abs(p.x));
    CHECK(std::abs(mx - 0.5f) < 1e-5f);
    CHECK(hs.hasCreases());
    // Open plane: border corners stay pinned.
    Mesh pl = prim::plane(2, 2, 2, 2);
    Mesh ps = subdivide(pl, 2);
    bool corner = false;
    for (auto& p : ps.positions) corner |= (std::abs(p.x + 1) < 1e-6f && std::abs(p.z - 1) < 1e-6f);
    CHECK(corner);
    // Cached evaluation tracks position edits.
    SubdivSurface surf;
    Mesh cage = prim::cube();
    const Mesh& a = surf.evaluate(cage, 2);
    Vec3 before = a.positions[0];
    for (auto& p : cage.positions) p = p * 2.0f;
    cage.touchPositions();
    const Mesh& b = surf.evaluate(cage, 2);
    CHECK(std::abs(b.positions[0].x - before.x * 2.0f) < 1e-5f);
    CHECK_EQ(surf.faceOrigin().size(), size_t(96));
    CHECK(surf.faceOrigin()[95] < 6u);
}

static void testExtrude()
{
    Mesh c = prim::cube();
    auto caps = ops::extrudeFaces(c, {4}, true, 0.5f);
    CHECK_EQ(c.numFaces(), 10u); CHECK_EQ(c.numVerts(), 12u); CHECK(manifoldClosed(c)); CHECK_EQ(euler(c), 2);
    CHECK(outwardNormals(c));
    // Two adjacent faces kept together share the middle wall.
    Mesh g = prim::cube(1, 1, 1, 2, 1, 1);
    uint32_t topA = kInvalid, topB = kInvalid;
    for (uint32_t f = 0; f < g.numFaces(); ++f)
        if (g.faceNormal(f).y > 0.9f) (topA == kInvalid ? topA : topB) = f;
    ops::extrudeFaces(g, {topA, topB}, true, 0.3f);
    CHECK(manifoldClosed(g)); CHECK_EQ(euler(g), 2);
    CHECK_EQ(g.numFaces(), 10u + 6u);
    // Individual extrusion.
    Mesh g2 = prim::cube(1, 1, 1, 2, 1, 1);
    topA = topB = kInvalid;
    for (uint32_t f = 0; f < g2.numFaces(); ++f)
        if (g2.faceNormal(f).y > 0.9f) (topA == kInvalid ? topA : topB) = f;
    ops::extrudeFaces(g2, {topA, topB}, false, 0.3f);
    CHECK_EQ(g2.numFaces(), 10u + 8u);
    // Plane extrusion creates walls along open borders.
    Mesh p = prim::plane();
    ops::extrudeFaces(p, {0}, true, 1.0f);
    CHECK_EQ(p.numFaces(), 5u);
}

static void testLoops()
{
    Mesh c = prim::cube(1, 1, 1, 3, 3, 3);
    uint32_t e0 = kInvalid;
    for (uint32_t e = 0; e < c.numEdges() && e0 == kInvalid; ++e)
        if (c.vertexEdges(c.edges[e].v0).size() == 4 && c.vertexEdges(c.edges[e].v1).size() == 4) e0 = e;
    auto loop = ops::edgeLoop(c, e0);
    CHECK_EQ(loop.size(), size_t(12));
    auto ring = ops::edgeRing(c, e0);
    CHECK_EQ(ring.size(), size_t(12));
    Mesh t = prim::torus(1, 0.5f, 12, 8);
    size_t tl = ops::edgeLoop(t, 0).size();
    CHECK(tl == 12 || tl == 8);
    // Insert an edge loop around a cube.
    Mesh cu = prim::cube();
    auto plan = ops::planEdgeLoop(cu, 0, 0.5f);
    CHECK_EQ(plan.cuts.size(), size_t(4));
    auto newEdges = ops::insertEdgeLoop(cu, plan);
    CHECK_EQ(newEdges.size(), size_t(4));
    CHECK_EQ(cu.numFaces(), 10u); CHECK_EQ(cu.numVerts(), 12u); CHECK(manifoldClosed(cu)); CHECK(outwardNormals(cu));
    // Loop through a cylinder side stops at n-gon caps.
    Mesh cy = prim::cylinder(1, 2, 8, 1, true);
    uint32_t vertical = kInvalid;
    for (uint32_t e = 0; e < cy.numEdges(); ++e)
        if (std::abs(cy.positions[cy.edges[e].v0].y - cy.positions[cy.edges[e].v1].y) > 1.0f) { vertical = e; break; }
    auto p2 = ops::planEdgeLoop(cy, vertical, 0.25f);
    CHECK_EQ(p2.cuts.size(), size_t(8));
    ops::insertEdgeLoop(cy, p2);
    CHECK(manifoldClosed(cy)); CHECK_EQ(euler(cy), 2); CHECK_EQ(cy.numFaces(), 18u);
    // Border loop on a plane.
    Mesh pl = prim::plane(1, 1, 3, 3);
    uint32_t be = kInvalid;
    for (uint32_t e = 0; e < pl.numEdges(); ++e) if (pl.isBorderEdge(e)) { be = e; break; }
    CHECK_EQ(ops::edgeLoop(pl, be).size(), size_t(12));
}

static void testDeleteMerge()
{
    Mesh c = prim::cube();
    ops::deleteFaces(c, {0});
    CHECK_EQ(c.numFaces(), 5u);
    uint32_t border = 0;
    for (auto& e : c.edges) border += e.faceCount == 1;
    CHECK_EQ(border, 4u);
    auto filled = ops::fillHoles(c);
    CHECK_EQ(filled.size(), size_t(1));
    CHECK(manifoldClosed(c)); CHECK(outwardNormals(c));

    // Dissolve the middle edge on a 2x1 plane -> one quad (winged verts removed).
    Mesh p = prim::plane(2, 1, 2, 1);
    uint32_t mid = kInvalid;
    for (uint32_t e = 0; e < p.numEdges(); ++e) if (p.edges[e].faceCount == 2) mid = e;
    CHECK(ops::dissolveEdges(p, {mid}));
    CHECK_EQ(p.numFaces(), 1u); CHECK_EQ(p.numVerts(), 4u);

    // Merge by distance welds coincident verts from a split cube.
    Mesh a = prim::cube();
    Mesh b = prim::cube();
    a.append(b, Mat4::identity());
    a.buildTopology();
    CHECK_EQ(a.numVerts(), 16u);
    ops::mergeByDistance(a, {}, 1e-4f);
    CHECK_EQ(a.numVerts(), 8u);

    Mesh q = prim::plane(1, 1, 2, 2);
    ops::mergeToCenter(q, {0, 1});
    CHECK_EQ(q.numVerts(), 8u); CHECK_EQ(q.numFaces(), 4u);

    Mesh dv = prim::plane(1, 1, 2, 2);
    uint32_t center = kInvalid;
    for (uint32_t v = 0; v < dv.numVerts(); ++v) if (dv.vertexEdges(v).size() == 4) center = v;
    ops::deleteVertices(dv, {center});
    CHECK_EQ(dv.numFaces(), 1u); CHECK_EQ(dv.numVerts(), 4u);
}

static void testMirrorSeparate()
{
    Mesh h = prim::cube();
    // Remove the -X face and shift so the open side lies on the X=0 plane.
    uint32_t negX = kInvalid;
    for (uint32_t f = 0; f < h.numFaces(); ++f) if (h.faceNormal(f).x < -0.9f) negX = f;
    ops::deleteFaces(h, {negX});
    for (auto& p : h.positions) p.x += 0.5f;
    h.touchPositions();
    ops::mirror(h, 0, 1e-4f);
    CHECK_EQ(h.numFaces(), 10u); CHECK_EQ(h.numVerts(), 12u); CHECK(manifoldClosed(h)); CHECK(outwardNormals(h));

    Mesh two = prim::cube();
    two.append(prim::sphere(), Mat4::translate({3, 0, 0}));
    two.buildTopology();
    auto parts = ops::separate(two);
    CHECK_EQ(parts.size(), size_t(2));
    CHECK_EQ(parts[0].numFaces() + parts[1].numFaces(), two.numFaces());
}

static void testConvertGrow()
{
    Mesh p = prim::plane(1, 1, 3, 3);
    uint32_t centerFace = 4;
    auto v = ops::convert(p, ops::Comp::Face, {centerFace}, ops::Comp::Vertex);
    CHECK_EQ(v.size(), size_t(4));
    auto e = ops::convert(p, ops::Comp::Face, {centerFace}, ops::Comp::Edge);
    CHECK_EQ(e.size(), size_t(4));
    auto g = ops::grow(p, ops::Comp::Face, {centerFace});
    CHECK_EQ(g.size(), size_t(9));
    auto s = ops::shrink(p, ops::Comp::Face, g);
    CHECK_EQ(s.size(), size_t(1));
}

static void testObj()
{
    std::vector<NamedMesh> out{{"box", prim::cube(), Mat4::translate({1, 0, 0})}, {"ball", prim::sphere(1, 8, 6), Mat4()}};
    std::string path = "/tmp/facet_test.obj";
    CHECK(writeObj(path, out));
    std::vector<NamedMesh> in;
    CHECK(readObj(path, in));
    CHECK_EQ(in.size(), size_t(2));
    CHECK_EQ(in[0].mesh.numFaces(), 6u);
    CHECK(std::abs(in[0].mesh.bounds().center().x - 1.0f) < 1e-5f);
    CHECK_EQ(in[1].mesh.numVerts(), out[1].mesh.numVerts());
}

static bool uvValid(const Mesh& m)
{
    if (!m.hasUVs()) return false;
    for (uint32_t u : m.faceUVs)
        if (u >= m.uvs.size()) return false;
    return true;
}

static void testUVsThroughOps()
{
    Mesh c = prim::cube();
    CHECK(uvValid(c));
    CHECK_EQ(c.uvs.size(), size_t(14)); // Maya's cube has 14 uvs
    ops::extrudeFaces(c, {0}, true, 0.3f);
    CHECK(uvValid(c));
    auto plan = ops::planEdgeLoop(c, 0, 0.5f);
    ops::insertEdgeLoop(c, plan);
    CHECK(uvValid(c));
    ops::deleteFaces(c, {1});
    ops::fillHoles(c);
    CHECK(uvValid(c));
    ops::triangulate(c);
    CHECK(uvValid(c));
    Mesh s1 = subdivide(prim::sphere(1, 8, 6), 2);
    CHECK(uvValid(s1));
    // Subdivided plane keeps a single connected UV grid: (2*4+1)^2 uvs for a 2x2 plane at level 2.
    Mesh p = subdivide(prim::plane(1, 1, 2, 2), 2);
    CHECK_EQ(p.uvs.size(), size_t(81));
    // OBJ keeps uvs.
    std::vector<NamedMesh> out{{"box", prim::cube(), Mat4()}};
    writeObj("/tmp/facet_uv.obj", out);
    std::vector<NamedMesh> in;
    readObj("/tmp/facet_uv.obj", in);
    CHECK(in.size() == 1 && uvValid(in[0].mesh) && in[0].mesh.uvs.size() == 14);
}

static void testBevel()
{
    // Single cube edge, one segment: +1 strip face, end faces gain a vertex (no caps).
    Mesh c = prim::cube();
    uint32_t e = 0;
    auto f = ops::bevelEdges(c, {e}, {0.1f, 1, false});
    CHECK_EQ(c.numFaces(), 7u); CHECK_EQ(c.numVerts(), 10u); CHECK(manifoldClosed(c)); CHECK_EQ(euler(c), 2);
    CHECK(outwardNormals(c)); CHECK(uvValid(c));
    // All 12 edges: 6 faces + 12 strips + 8 corner triangles.
    Mesh all = prim::cube();
    IndexList es;
    for (uint32_t i = 0; i < all.numEdges(); ++i) es.push_back(i);
    ops::bevelEdges(all, es, {0.1f, 1, false});
    CHECK_EQ(all.numFaces(), 26u); CHECK_EQ(all.numVerts(), 24u); CHECK(manifoldClosed(all)); CHECK_EQ(euler(all), 2);
    CHECK(outwardNormals(all));
    // Rounded, 3 segments, all edges.
    Mesh r = prim::cube();
    ops::bevelEdges(r, es, {0.15f, 3, true});
    CHECK(manifoldClosed(r)); CHECK_EQ(euler(r), 2); CHECK(outwardNormals(r));
    CHECK_EQ(r.numFaces(), 6u + 12u * 3u + 8u);
    // Edge loop around a cylinder (loop through valence-4 vertices).
    Mesh cy = prim::cylinder(1, 2, 12, 2, true);
    uint32_t ring = kInvalid;
    for (uint32_t i = 0; i < cy.numEdges(); ++i) {
        auto& ed = cy.edges[i];
        if (std::abs(cy.positions[ed.v0].y) < 1e-4f && std::abs(cy.positions[ed.v1].y) < 1e-4f) { ring = i; break; }
    }
    auto loop = ops::edgeLoop(cy, ring);
    CHECK_EQ(loop.size(), size_t(12));
    ops::bevelEdges(cy, loop, {0.2f, 2, true});
    CHECK(manifoldClosed(cy)); CHECK_EQ(euler(cy), 2); CHECK_EQ(cy.numFaces(), 24u + 2u + 24u);
    // Cube corner with two bevelled edges.
    Mesh two = prim::cube();
    uint32_t v0 = 0;
    IndexList e2(two.vertexEdges(v0).begin(), two.vertexEdges(v0).begin() + 2);
    ops::bevelEdges(two, e2, {0.1f, 2, true});
    CHECK(manifoldClosed(two)); CHECK_EQ(euler(two), 2); CHECK(outwardNormals(two));
}

static void testBridgeCut()
{
    // Two separate planes facing each other -> bridge their borders into a tube.
    Mesh a = prim::plane(1, 1, 2, 2);
    Mesh b = prim::plane(1, 1, 2, 2);
    Mesh m = a;
    m.append(b, Mat4::translate({0, 2, 0}) * Mat4::rotateX(180));
    m.buildTopology();
    IndexList border;
    for (uint32_t e = 0; e < m.numEdges(); ++e) if (m.isBorderEdge(e)) border.push_back(e);
    auto made = ops::bridgeEdges(m, border, 2);
    CHECK_EQ(made.size(), size_t(8 * 3));
    CHECK(manifoldClosed(m)); CHECK_EQ(euler(m), 2);
    // Bridge faces: two opposite cube faces -> through-hole (genus 1).
    Mesh c = prim::cube(1, 1, 1, 3, 3, 3);
    uint32_t fz = kInvalid, fnz = kInvalid;
    for (uint32_t f = 0; f < c.numFaces(); ++f) {
        Vec3 ctr = c.faceCenter(f), n = c.faceNormal(f);
        if (std::abs(ctr.x) < 0.01f && std::abs(ctr.y) < 0.01f && n.z > 0.9f) fz = f;
        if (std::abs(ctr.x) < 0.01f && std::abs(ctr.y) < 0.01f && n.z < -0.9f) fnz = f;
    }
    ops::bridgeFaces(c, {fz, fnz}, 1);
    CHECK(manifoldClosed(c)); CHECK_EQ(euler(c), 0);
    // Multi-cut across a quad: edge midpoint to opposite edge midpoint.
    Mesh q = prim::plane(1, 1, 1, 1);
    auto& e0 = q.edges[0];
    uint32_t opp = kInvalid;
    for (uint32_t e = 1; e < q.numEdges(); ++e)
        if (q.edges[e].v0 != e0.v0 && q.edges[e].v0 != e0.v1 && q.edges[e].v1 != e0.v0 && q.edges[e].v1 != e0.v1) opp = e;
    auto cut = ops::multiCut(q, {{e0.v0, e0.v1, 0.5f}, {q.edges[opp].v0, q.edges[opp].v1, 0.5f}});
    CHECK_EQ(cut.size(), size_t(1)); CHECK_EQ(q.numFaces(), 2u); CHECK_EQ(q.numVerts(), 6u);
    CHECK(uvValid(q));
    // Corner-to-corner cut (diagonal).
    Mesh d = prim::plane(1, 1, 1, 1);
    CHECK(ops::splitFace(d, 0, 3) || ops::splitFace(d, 1, 2));
    CHECK_EQ(d.numFaces(), 2u);
    // Target weld.
    Mesh w = prim::plane(1, 1, 2, 1);
    ops::mergeInto(w, 0, 1);
    CHECK_EQ(w.numVerts(), 5u);
}

static void testUVTools()
{
    // Cylinder: cylindrical projection then unfold keeps it unflipped.
    Mesh cy = prim::cylinder(1, 2, 16, 4, false);
    uv::cylindrical(cy, {});
    CHECK(uvValid(cy));
    CHECK(uv::flippedRatio(cy) < 0.01f);
    // Cut a vertical seam on a closed sphere's hemisphere -> one shell, unfold flat without flips.
    Mesh s = prim::sphere(1, 24, 12);
    // Keep the top half: delete faces below the equator.
    IndexList lower;
    for (uint32_t f = 0; f < s.numFaces(); ++f) if (s.faceCenter(f).y < 0) lower.push_back(f);
    ops::deleteFaces(s, lower);
    uv::planar(s, {}, uv::Axis::Y);
    uint32_t shells = 0;
    uv::shellIds(s, &shells);
    CHECK_EQ(shells, 1u);
    int solved = uv::unfold(s);
    CHECK_EQ(solved, 1);
    std::printf("  hemisphere LSCM flipped ratio %.3f\n", uv::flippedRatio(s));
    CHECK(uv::flippedRatio(s) < 0.01f);
    // Area roughly preserved (hemisphere area = 2*pi).
    float a2 = 0;
    for (uint32_t f = 0; f < s.numFaces(); ++f)
        for (uint32_t i = 1; i + 1 < s.faceSize(f); ++i) {
            uint32_t b = s.faceStart[f];
            a2 += 0.5f * std::abs(cross2(s.cornerUV(b + i) - s.cornerUV(b), s.cornerUV(b + i + 1) - s.cornerUV(b)));
        }
    CHECK(std::abs(a2 - 2 * kPi) / (2 * kPi) < 0.1f);
    // Cut seams: cube -> cut every edge => 6 shells; sew one edge back => 5.
    Mesh c = prim::cube();
    IndexList all;
    for (uint32_t e = 0; e < c.numEdges(); ++e) all.push_back(e);
    uv::cutEdges(c, all);
    uv::shellIds(c, &shells);
    CHECK_EQ(shells, 6u);
    CHECK_EQ(c.uvs.size(), size_t(24));
    uv::sewEdges(c, {0});
    uv::shellIds(c, &shells);
    CHECK_EQ(shells, 5u);
    // Layout packs into 0..1 without overlap of shell boxes.
    uv::layout(c);
    for (const Vec2& t : c.uvs) CHECK(t.x >= -1e-4f && t.x <= 1.0001f && t.y >= -1e-4f && t.y <= 1.0001f);
    // Automatic mapping of a torus: no flips, inside 0..1.
    Mesh t = prim::torus();
    uv::automatic(t, {});
    CHECK(uvValid(t));
    for (const Vec2& q : t.uvs) CHECK(q.x >= -1e-4f && q.x <= 1.0001f && q.y >= -1e-4f && q.y <= 1.0001f);
    // Spherical projection valid.
    Mesh sp = prim::sphere();
    uv::spherical(sp, {});
    CHECK(uvValid(sp) && uv::flippedRatio(sp) < 0.05f);
}

static void testBVH()
{
    Mesh s = prim::sphere(2, 64, 32);
    TriBVH bvh;
    bvh.build(s);
    auto h = bvh.raycast({{0, 0, 10}, {0, 0, -1}});
    CHECK(h.hit && std::abs(h.point.z - 2.0f) < 0.02f);
    auto miss = bvh.raycast({{5, 5, 10}, {0, 0, -1}});
    CHECK(!miss.hit);
    auto c = bvh.closest({0, 5, 0});
    CHECK(c.hit && std::abs(c.point.y - 2.0f) < 0.02f && std::abs(c.t - 3.0f) < 0.02f);
    auto c2 = bvh.closest({0.3f, 0.2f, 0.1f});
    CHECK(c2.hit && std::abs(length(c2.point) - 2.0f) < 0.02f);
}

static void testHardEdges()
{
    Mesh c = prim::cube();
    uint32_t hard = 0;
    for (uint32_t e = 0; e < c.numEdges(); ++e) hard += c.isHard(e);
    CHECK_EQ(hard, 12u); // Maya cube: all edges hard
    std::vector<Vec3> cn;
    std::vector<uint32_t> grp;
    c.computeCornerNormals(cn, &grp);
    std::sort(grp.begin(), grp.end());
    grp.erase(std::unique(grp.begin(), grp.end()), grp.end());
    CHECK_EQ(grp.size(), size_t(24)); // 8 vertices x 3 normals
    bool flat = true;
    for (uint32_t h = 0; h < c.numHalfEdges(); ++h) flat &= dot(cn[h], c.faceNormal(c.heFace[h])) > 0.999f;
    CHECK(flat);
    // Soft sphere: one normal per vertex.
    Mesh s = prim::sphere(1, 12, 8);
    CHECK(!s.hasHardEdges());
    s.computeCornerNormals(cn, &grp);
    std::sort(grp.begin(), grp.end());
    grp.erase(std::unique(grp.begin(), grp.end()), grp.end());
    CHECK_EQ(grp.size(), size_t(s.numVerts()));
    // Cylinder: cap rings hard, sides soft.
    Mesh cy = prim::cylinder(1, 2, 16, 1, true);
    hard = 0;
    for (uint32_t e = 0; e < cy.numEdges(); ++e) hard += cy.isHard(e);
    CHECK_EQ(hard, 32u);
    // Hardness survives topology ops.
    auto plan = ops::planEdgeLoop(c, 0, 0.5f);
    ops::insertEdgeLoop(c, plan);
    hard = 0;
    for (uint32_t e = 0; e < c.numEdges(); ++e) hard += c.isHard(e);
    CHECK_EQ(hard, 16u); // 4 split edges -> 8 halves, plus the 8 untouched, new loop soft
    Mesh mc = prim::cube();
    ops::extrudeFaces(mc, {0}, true, 0.2f);
    ops::mirror(mc, 0, 1e-4f);
    CHECK(mc.hasHardEdges());
    // Normal angle: soften all, then 30 degrees hardens the cube again.
    Mesh n = prim::cube();
    ops::setEdgesHard(n, {}, false);
    CHECK(!n.hasHardEdges());
    ops::setNormalAngle(n, {}, 30.0f);
    hard = 0;
    for (uint32_t e = 0; e < n.numEdges(); ++e) hard += n.isHard(e);
    CHECK_EQ(hard, 12u);
    // Bevel: rounded bevel strips soft inside, hard boundary only where sharper than 30 deg.
    Mesh b = prim::cube();
    IndexList all;
    for (uint32_t e = 0; e < b.numEdges(); ++e) all.push_back(e);
    ops::bevelEdges(b, all, {0.1f, 4, true});
    hard = 0;
    for (uint32_t e = 0; e < b.numEdges(); ++e) hard += b.isHard(e);
    CHECK_EQ(hard, 0u); // 4 segments of a 90 deg turn = 18 deg steps: all soft
    Mesh b1 = prim::cube();
    ops::bevelEdges(b1, all, {0.1f, 1, false});
    hard = 0;
    for (uint32_t e = 0; e < b1.numEdges(); ++e) hard += b1.isHard(e);
    CHECK(hard > 0); // single chamfer: 45 deg turns -> hard
}

static void testPerformance()
{
    // A dense cage: ~100k quads to 1.6M quads at level 2.
    Mesh m = prim::sphere(1, 400, 250);
    auto t0 = std::chrono::steady_clock::now();
    SubdivSurface s;
    const Mesh& r = s.evaluate(m, 2);
    auto t1 = std::chrono::steady_clock::now();
    m.touchPositions();
    s.evaluate(m, 2);
    auto t2 = std::chrono::steady_clock::now();
    std::vector<Vec3> n;
    r.computeVertexNormals(n);
    auto t3 = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    std::printf("  cage %u faces -> %u faces | refine+eval %.0f ms, re-eval %.0f ms, normals %.0f ms\n",
                m.numFaces(), r.numFaces(), ms(t0, t1), ms(t1, t2), ms(t2, t3));
    CHECK(r.numFaces() == m.numHalfEdges() * 4);
}

int main()
{
    std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"primitives", testPrimitives}, {"subdivision", testSubdivision}, {"extrude", testExtrude},
        {"loops", testLoops}, {"delete/merge", testDeleteMerge}, {"mirror/separate", testMirrorSeparate},
        {"convert/grow", testConvertGrow}, {"obj", testObj}, {"uvs through ops", testUVsThroughOps},
        {"bevel", testBevel}, {"bridge/multicut/weld", testBridgeCut},
        {"uv tools", testUVTools}, {"hard edges", testHardEdges}, {"bvh", testBVH}, {"performance", testPerformance}};
    for (auto& [name, fn] : tests) {
        int before = g_fail;
        fn();
        std::printf("%s %s\n", g_fail == before ? "PASS" : "FAIL", name);
    }
    std::printf(g_fail ? "%d check(s) failed\n" : "All tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
