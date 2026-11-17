// Loads the Smart Unwrap plugin like the app does and benchmarks it against uv::automatic.
#include "mesh/MeshOps.h"
#include "mesh/Primitives.h"
#include "mesh/Subdivision.h"
#include "mesh/UVOps.h"
#include "plugin/PluginApi.h"

#include <QCoreApplication>
#include <QDir>
#include <QLibrary>
#include <QPluginLoader>
#include <QElapsedTimer>

#include <cmath>
#include <cstdio>

using namespace facet;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("  FAILED line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static Mesh lumpy()
{
    Mesh m = prim::sphere(1.5f, 64, 48);
    for (auto& p : m.positions) p = p * (1.0f + 0.12f * std::sin(p.x * 3.1f) * std::cos(p.y * 2.7f) + 0.06f * std::sin(p.z * 5.0f));
    m.touchPositions();
    return m;
}

static Mesh roundedBox()
{
    Mesh c = prim::cube(2, 1, 1.4f);
    std::vector<uint32_t> all;
    for (uint32_t e = 0; e < c.numEdges(); ++e) all.push_back(e);
    ops::bevelEdges(c, all, {0.12f, 3, true});
    return c;
}

static void row(const char* name, const char* method, const uv::Quality& q)
{
    std::printf("  %-14s %-10s shells %4u  flips %3u  overlap %5.2f%%  areaDist %5.3f  angleDist %5.3f  densitySpread %5.3f  coverage %5.1f%%\n",
                name, method, q.shells, q.flippedFaces, q.overlap * 100, q.areaDistortion, q.angleDistortion, q.densitySpread, q.efficiency * 100);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    FacetPlugin* plugin = nullptr;
    QDir dir(QStringLiteral(FACET_BUILD_PLUGIN_DIR));
    for (const QString& f : dir.entryList(QDir::Files)) {
        if (!QLibrary::isLibrary(dir.absoluteFilePath(f))) continue;
        auto* loader = new QPluginLoader(dir.absoluteFilePath(f));
        if (auto* p = qobject_cast<FacetPlugin*>(loader->instance())) plugin = p;
        else std::printf("  load error: %s\n", qPrintable(loader->errorString()));
    }
    CHECK(plugin != nullptr);
    if (!plugin) return 1;
    auto cmds = plugin->commands();
    CHECK(cmds.size() == 1 && cmds[0].id == "smartUnwrap");
    std::printf("Loaded plugin: %s %s\n", qPrintable(plugin->name()), qPrintable(plugin->version()));
    const PluginCommand& cmd = cmds[0];
    QVariantMap opts;
    for (const auto& o : cmd.options) opts[o.key] = o.defaultValue;

    struct Case { const char* name; Mesh mesh; Mat4 world; };
    std::vector<Case> cases;
    cases.push_back({"sphere", prim::sphere(1, 32, 16), Mat4()});
    cases.push_back({"torus", prim::torus(1, 0.4f, 32, 16), Mat4()});
    cases.push_back({"rounded box", roundedBox(), Mat4()});
    cases.push_back({"lumpy sculpt", lumpy(), Mat4()});
    cases.push_back({"smooth cube", subdivide(prim::cube(), 3), Mat4()});
    cases.push_back({"scaled cyl", prim::cylinder(1, 2, 24, 4, true), Mat4::scale({1, 3, 1})});

    for (Case& c : cases) {
        Mesh autoM = c.mesh;
        uv::automatic(autoM, {});
        uv::Quality qa = uv::measure(autoM, c.world);
        Mesh smartM = c.mesh;
        std::vector<PluginMesh> in{{c.name, &smartM, c.world, {}, {}}};
        QString msg = cmd.run(in, opts);
        uv::Quality qs = uv::measure(smartM, c.world);
        row(c.name, "automatic", qa);
        row(c.name, "smart", qs);
        std::printf("  %s\n", qPrintable(msg));
        CHECK(smartM.hasUVs());
        CHECK(qs.flippedFaces == 0);
        CHECK(qs.overlap < 0.005f);
        CHECK(qs.densitySpread < 0.03f);           // uniform texel density
        CHECK(qs.areaDistortion <= 1.2f);
        CHECK(qs.areaDistortion <= qa.areaDistortion + 1e-3f);
        CHECK(qs.angleDistortion <= qa.angleDistortion + 1e-3f);
        CHECK(qs.efficiency >= qa.efficiency - 0.03f); // packing at least as tight as box-mapping (within 3%)
        bool inside = true;
        for (const Vec2& u : smartM.uvs) inside &= u.x >= -1e-4f && u.x <= 1.0001f && u.y >= -1e-4f && u.y <= 1.0001f;
        CHECK(inside);
    }

    // Dense mesh timing.
    {
        Mesh dense = prim::sphere(2, 256, 128);
        for (auto& p : dense.positions) p = p * (1.0f + 0.1f * std::sin(p.x * 4.0f) * std::cos(p.z * 3.0f));
        dense.touchPositions();
        std::vector<PluginMesh> in{{"dense", &dense, Mat4(), {}, {}}};
        QElapsedTimer t;
        t.start();
        QString msg = cmd.run(in, opts);
        qint64 ms = t.elapsed();
        uv::Quality q = uv::measure(dense);
        std::printf("  dense (%u faces): %lld ms  %s\n", dense.numFaces(), (long long)ms, qPrintable(msg));
        row("dense", "smart", q);
        CHECK(q.flippedFaces == 0 && q.overlap < 0.005f);
    }

    // Two objects of very different size packed together keep the same texel density.
    {
        Mesh a = prim::cube(), b = prim::cube(4, 4, 4);
        std::vector<PluginMesh> in{{"small", &a, Mat4(), {}, {}}, {"big", &b, Mat4::translate({5, 0, 0}), {}, {}}};
        cmd.run(in, opts);
        auto area = [](const Mesh& m) {
            double s = 0;
            for (uint32_t f = 0; f < m.numFaces(); ++f)
                for (uint32_t i = 1; i + 1 < m.faceSize(f); ++i) {
                    uint32_t b0 = m.faceStart[f];
                    s += 0.5 * std::abs(cross2(m.cornerUV(b0 + i) - m.cornerUV(b0), m.cornerUV(b0 + i + 1) - m.cornerUV(b0)));
                }
            return s;
        };
        double ratio = area(b) / area(a); // world area ratio is 16
        std::printf("  packed together: uv area ratio big/small = %.2f (world 16.00)\n", ratio);
        CHECK(std::abs(ratio - 16.0) < 0.5);
    }
    std::printf(g_fail ? "%d plugin check(s) failed\n" : "All plugin tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
