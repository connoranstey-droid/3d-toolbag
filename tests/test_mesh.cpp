// Minimal self-contained test runner for the geometry core (no Qt required).
#include "mesh/Mesh.h"
#include "mesh/MeshOps.h"
#include "mesh/ObjIO.h"
#include "mesh/Primitives.h"
#include "mesh/Subdivision.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

using namespace facet;

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
        {"convert/grow", testConvertGrow}, {"obj", testObj}, {"performance", testPerformance}};
    for (auto& [name, fn] : tests) {
        int before = g_fail;
        fn();
        std::printf("%s %s\n", g_fail == before ? "PASS" : "FAIL", name);
    }
    std::printf(g_fail ? "%d check(s) failed\n" : "All tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
