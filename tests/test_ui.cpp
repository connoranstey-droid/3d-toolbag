// End-to-end interaction tests: real MainWindow + Viewport driven with synthesized input.
#include "scene/Modeling.h"
#include "ui/MainWindow.h"
#include "viewport/Viewport.h"
#include "ui/UVEditor.h"
#include "ui/PluginManager.h"
#include "ui/Hotkeys.h"
#include "ui/ChannelBox.h"
#include "ui/MarkingMenu.h"
#include <QMenu>
#include <QSettings>
#include "mesh/UVOps.h"

#include <QApplication>
#include <QMouseEvent>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTest>

#include <cstdio>

using namespace facet;
namespace M = facet::modeling;

static int g_fail = 0;
static int g_mark = 0;
#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) { std::printf("  FAILED line %d: %s\n", __LINE__, #c); ++g_fail; } \
    } while (0)

static void send(QWidget* w, QEvent::Type t, QPoint p, Qt::MouseButton b, Qt::MouseButtons bs, Qt::KeyboardModifiers m = {})
{
    QMouseEvent e(t, QPointF(p), QPointF(w->mapToGlobal(p)), b, bs, m);
    QApplication::sendEvent(w, &e);
}
static void press(QWidget* w, QPoint p, Qt::MouseButton b = Qt::LeftButton, Qt::KeyboardModifiers m = {}) { send(w, QEvent::MouseButtonPress, p, b, b, m); }
static void moveTo(QWidget* w, QPoint p, Qt::MouseButtons bs = Qt::NoButton, Qt::KeyboardModifiers m = {}) { send(w, QEvent::MouseMove, p, Qt::NoButton, bs, m); }
static void release(QWidget* w, QPoint p, Qt::MouseButton b = Qt::LeftButton, Qt::KeyboardModifiers m = {}) { send(w, QEvent::MouseButtonRelease, p, b, Qt::NoButton, m); }
static void click(QWidget* w, QPoint p, Qt::KeyboardModifiers m = {}) { press(w, p, Qt::LeftButton, m); release(w, p, Qt::LeftButton, m); }
static void drag(QWidget* w, QPoint a, QPoint b, Qt::MouseButton btn = Qt::LeftButton, Qt::KeyboardModifiers m = {})
{
    press(w, a, btn, m);
    for (int i = 1; i <= 8; ++i) moveTo(w, a + (b - a) * i / 8, btn, m);
    release(w, b, btn, m);
}
static void key(QWidget* w, Qt::Key k, Qt::KeyboardModifiers m = {}) { QTest::keyClick(w, k, m); QApplication::processEvents(); }

static float maxY(const Mesh& m)
{
    float y = -1e9f;
    for (auto& p : m.positions) y = std::max(y, p.y);
    return y;
}

int main(int argc, char** argv)
{
    QSurfaceFormat fmt;
    fmt.setVersion(4, 1);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(fmt);
    QApplication app(argc, argv);
    // Keep the user's real hotkey settings untouched.
    QCoreApplication::setOrganizationName("FacetUITest");
    QCoreApplication::setApplicationName("FacetUITest");
    QSettings().clear();

    MainWindow w;
    w.resize(1400, 900);
    w.show();
    if (!QTest::qWaitForWindowExposed(&w)) { std::printf("window not exposed\n"); return 2; }
    // Window-context shortcuts need an active window (there is no window manager under Xvfb).
    w.activateWindow();
    QApplication::setActiveWindow(&w);
    QTest::qWaitForWindowActive(&w);
    Viewport* vp = w.viewport();
    Scene& s = *w.scene();
    vp->setFocus();
    QApplication::processEvents();

    // ---- object selection ----
    M::createPrimitive(s, "polyCube");
    SceneObject* cube = s.lead();
    vp->frameAll();
    vp->repaint();
    QApplication::processEvents();
    CHECK(s.selection().size() == 1);
    click(vp, QPoint(15, vp->height() - 15));
    CHECK(s.selection().empty());
    click(vp, vp->worldToWidget({0, 0, 0}));
    CHECK(s.selection().size() == 1 && s.isSelected(cube->id));
    std::printf("%s click select object\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    M::createPrimitive(s, "polySphere");
    SceneObject* sphere = s.lead();
    sphere->st.translate = {3, 0, 0};
    vp->frameAll();
    vp->repaint();
    drag(vp, QPoint(5, 5), QPoint(vp->width() - 5, vp->height() - 5));
    CHECK(s.selection().size() == 2);
    click(vp, vp->worldToWidget({3, 0, 0}), Qt::ShiftModifier); // shift toggles off
    CHECK(s.selection().size() == 1 && s.isSelected(cube->id));
    std::printf("%s marquee + shift toggle\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- object move via gizmo ----
    key(vp, Qt::Key_W);
    CHECK(s.tool() == Tool::Move);
    vp->frameSelected();
    vp->repaint();
    QPoint o = vp->worldToWidget(cube->st.translate);
    QPoint xTip = vp->worldToWidget(cube->st.translate + Vec3{0.3f, 0, 0});
    QPointF dir = QPointF(xTip - o);
    dir /= std::hypot(dir.x(), dir.y());
    QPoint handle = o + (dir * 55).toPoint();
    drag(vp, handle, handle + (dir * 80).toPoint());
    CHECK(cube->st.translate.x > 0.2f && std::abs(cube->st.translate.y) < 1e-4f && std::abs(cube->st.translate.z) < 1e-4f);
    float movedX = cube->st.translate.x;
    key(vp, Qt::Key_Z, Qt::ControlModifier);
    CHECK(std::abs(cube->st.translate.x) < 1e-5f);
    key(vp, Qt::Key_Z, Qt::ShiftModifier); // Maya redo
    CHECK(std::abs(cube->st.translate.x - movedX) < 1e-5f);
    key(vp, Qt::Key_Z, Qt::ControlModifier);
    std::printf("%s move gizmo + undo/redo (dx=%.3f)\n", (g_fail == g_mark ? "PASS" : "FAIL"), movedX); g_mark = g_fail;

    // ---- face mode, extrude, drag along normal ----
    key(vp, Qt::Key_F11);
    CHECK(s.mode() == SelectMode::Face);
    vp->frameSelected();
    vp->repaint();
    click(vp, vp->worldToWidget({0, 0.5f, 0}));
    CHECK(cube->sel.faces.size() == 1 && cube->mesh().faceNormal(cube->sel.faces[0]).y > 0.9f);
    key(vp, Qt::Key_E, Qt::ControlModifier);
    CHECK(cube->mesh().numFaces() == 10);
    CHECK(s.tool() == Tool::Move && s.moveOrient == AxisOrient::Normal);
    vp->repaint();
    o = vp->worldToWidget({0, 0.5f, 0});
    QPoint yTip = vp->worldToWidget({0, 0.8f, 0});
    dir = QPointF(yTip - o);
    dir /= std::hypot(dir.x(), dir.y());
    handle = o + (dir * 55).toPoint();
    drag(vp, handle, handle + (dir * 60).toPoint());
    CHECK(maxY(cube->mesh()) > 0.6f);
    std::printf("%s extrude + pull along normal (top y=%.3f)\n", (g_fail == g_mark ? "PASS" : "FAIL"), maxY(cube->mesh())); g_mark = g_fail;
    key(vp, Qt::Key_Z, Qt::ControlModifier);
    CHECK(std::abs(maxY(cube->mesh()) - 0.5f) < 1e-5f);
    key(vp, Qt::Key_Z, Qt::ControlModifier);
    CHECK(cube->mesh().numFaces() == 6);
    std::printf("%s undo extrude\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- vertex marquee: only visible vertices ----
    key(vp, Qt::Key_F9);
    CHECK(s.mode() == SelectMode::Vertex);
    vp->frameSelected();
    vp->repaint();
    drag(vp, QPoint(5, 5), QPoint(vp->width() - 5, vp->height() - 5));
    std::printf("  visible verts selected: %zu\n", cube->sel.verts.size());
    CHECK(cube->sel.verts.size() == 7);
    key(vp, Qt::Key_X, Qt::AltModifier); // x-ray selects through
    drag(vp, QPoint(5, 5), QPoint(vp->width() - 5, vp->height() - 5));
    CHECK(cube->sel.verts.size() == 8);
    key(vp, Qt::Key_X, Qt::AltModifier);
    std::printf("%s vertex marquee respects occlusion\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- soft selection toggle (tap B) ----
    QTest::keyPress(vp, Qt::Key_B);
    QTest::keyRelease(vp, Qt::Key_B);
    CHECK(s.softSelect);
    QTest::keyPress(vp, Qt::Key_B);
    QTest::keyRelease(vp, Qt::Key_B);
    CHECK(!s.softSelect);
    std::printf("%s soft select hotkey\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- edge mode: insert edge loop tool ----
    key(vp, Qt::Key_F10);
    CHECK(s.mode() == SelectMode::Edge);
    s.setTool(Tool::InsertEdgeLoop);
    vp->repaint();
    // Front-right vertical edge of the cube: x=0.5, z=0.5.
    QPoint edgePt = vp->worldToWidget({0.5f, 0.1f, 0.5f});
    QPoint inward = vp->worldToWidget({0.45f, 0.1f, 0.45f});
    QPoint pickAt = edgePt + (inward - edgePt) / 3;
    moveTo(vp, pickAt);
    click(vp, pickAt);
    CHECK(cube->mesh().numFaces() == 10);
    CHECK(cube->sel.edges.size() == 4);
    std::printf("%s insert edge loop (faces=%u)\n", (g_fail == g_mark ? "PASS" : "FAIL"), cube->mesh().numFaces()); g_mark = g_fail;

    // ---- double-click edge selects the loop ----
    s.setTool(Tool::Select);
    vp->repaint();
    {
        QMouseEvent dc(QEvent::MouseButtonDblClick, QPointF(pickAt), QPointF(vp->mapToGlobal(pickAt)), Qt::LeftButton, Qt::LeftButton, {});
        QApplication::sendEvent(vp, &dc);
        release(vp, pickAt);
    }
    std::printf("  loop size: %zu\n", cube->sel.edges.size());
    CHECK(cube->sel.edges.size() >= 2);
    std::printf("%s double-click loop select\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- RMB marking menu gesture: drag north -> Vertex ----
    key(vp, Qt::Key_F8); // back to object mode
    CHECK(s.mode() == SelectMode::Object);
    vp->repaint();
    QPoint c = vp->worldToWidget({0, 0, 0});
    press(vp, c, Qt::RightButton);
    for (int i = 1; i <= 6; ++i) moveTo(vp, c - QPoint(0, 20 * i), Qt::RightButton);
    release(vp, c - QPoint(0, 120), Qt::RightButton);
    QApplication::processEvents();
    CHECK(s.mode() == SelectMode::Vertex);
    std::printf("%s marking menu gesture\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- smooth preview hotkeys ----
    key(vp, Qt::Key_F8);
    s.selectObjects({cube->id}, SelectOp::Replace);
    key(vp, Qt::Key_3);
    CHECK(cube->st.smoothDisplay == 2);
    key(vp, Qt::Key_PageUp);
    CHECK(cube->st.smoothLevel == 3);
    vp->repaint();
    key(vp, Qt::Key_1);
    CHECK(cube->st.smoothDisplay == 0);
    std::printf("%s smooth preview keys\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- quad view: tap Space ----
    QTest::keyPress(vp, Qt::Key_Space);
    QTest::keyRelease(vp, Qt::Key_Space);
    CHECK(vp->quadView());
    vp->repaint();
    QTest::keyPress(vp, Qt::Key_Space);
    QTest::keyRelease(vp, Qt::Key_Space);
    CHECK(!vp->quadView());
    vp->setSingleView(ViewKind::Persp); // the toggle maximizes whichever panel the (headless) cursor is over
    std::printf("%s space quad toggle\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- symmetry: moving a vertex drags its mirror partner ----
    s.clear();
    M::createPrimitive(s, "polyCube");
    SceneObject* sc = s.lead();
    vp->frameSelected();
    vp->repaint();
    s.symmetry = 0;
    key(vp, Qt::Key_F9);
    click(vp, vp->worldToWidget({0.5f, 0.5f, 0.5f}));
    CHECK(sc->sel.verts.size() == 2); // symmetric selection picks the partner too
    key(vp, Qt::Key_W);
    s.moveOrient = AxisOrient::World;
    vp->repaint();
    {
        uint32_t pv = kInvalid, nv = kInvalid;
        for (uint32_t v = 0; v < sc->mesh().numVerts(); ++v) {
            Vec3 q = sc->mesh().positions[v];
            if (std::abs(q.y - 0.5f) < 1e-4f && std::abs(q.z - 0.5f) < 1e-4f) (q.x > 0 ? pv : nv) = v;
        }
        // Keep only the +X vertex selected; symmetry still drags the partner.
        s.symmetry = -1;
        s.setComponents(sc, SelectMode::Vertex, {pv});
        s.symmetry = 0;
        vp->repaint();
        QPoint o2 = vp->worldToWidget({0.5f, 0.5f, 0.5f});
        QPoint yT = vp->worldToWidget({0.5f, 0.8f, 0.5f});
        QPointF dy = QPointF(yT - o2);
        dy /= std::hypot(dy.x(), dy.y());
        QPoint h2 = o2 + (dy * 55).toPoint();
        drag(vp, h2, h2 + (dy * 50).toPoint());
        Vec3 a = sc->mesh().positions[pv], b = sc->mesh().positions[nv];
        std::printf("  sym: +x vertex y=%.3f, mirror y=%.3f x=%.3f\n", a.y, b.y, b.x);
        CHECK(a.y > 0.55f && std::abs(a.y - b.y) < 1e-4f && std::abs(b.x + 0.5f) < 1e-4f);
    }
    s.symmetry = -1;
    std::printf("%s symmetry\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- bevel hotkey ----
    key(vp, Qt::Key_Z, Qt::ControlModifier);
    key(vp, Qt::Key_F10);
    vp->repaint();
    s.clearComponents();
    click(vp, vp->worldToWidget({0.0f, 0.5f, 0.5f}));
    CHECK(sc->sel.edges.size() == 1);
    key(vp, Qt::Key_B, Qt::ControlModifier);
    CHECK(sc->mesh().numFaces() == 7);
    std::printf("%s bevel (Ctrl+B)\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- Multi-Cut across a single-quad plane ----
    s.clear();
    M::createPrimitive(s, "polyPlane");
    SceneObject* pl = s.lead();
    pl->st.history->set("subdivisionsWidth", 1);
    pl->st.history->set("subdivisionsHeight", 1);
    pl->st.mesh = std::make_shared<Mesh>(buildPrimitive(*pl->st.history));
    vp->frameSelected();
    s.setTool(Tool::MultiCut);
    vp->repaint();
    QPoint m1 = vp->worldToWidget({0.0f, 0.0f, 0.49f}), m2 = vp->worldToWidget({0.0f, 0.0f, -0.49f});
    moveTo(vp, m1);
    click(vp, m1);
    moveTo(vp, m2);
    click(vp, m2);
    key(vp, Qt::Key_Return);
    CHECK(pl->mesh().numFaces() == 2 && pl->mesh().numVerts() == 6);
    std::printf("%s multi-cut (faces=%u)\n", (g_fail == g_mark ? "PASS" : "FAIL"), pl->mesh().numFaces()); g_mark = g_fail;

    // ---- Target Weld: drag a corner onto the cut vertex ----
    s.setMode(SelectMode::Vertex);
    s.setTool(Tool::TargetWeld);
    vp->repaint();
    {
        QPoint from = vp->worldToWidget({-0.5f, 0.0f, 0.5f}), to = vp->worldToWidget({0.0f, 0.0f, 0.5f});
        drag(vp, from, to);
    }
    CHECK(pl->mesh().numVerts() == 5);
    std::printf("%s target weld (verts=%u)\n", (g_fail == g_mark ? "PASS" : "FAIL"), pl->mesh().numVerts()); g_mark = g_fail;

    // ---- Quad Draw on a live sphere ----
    s.clear();
    s.setTool(Tool::Select);
    s.setMode(SelectMode::Object);
    M::createPrimitive(s, "polySphere");
    SceneObject* live = s.lead();
    vp->frameSelected();
    M::toggleLive(s);
    CHECK(s.liveId == live->id);
    s.setTool(Tool::QuadDraw);
    vp->repaint();
    {
        const Camera& cam = vp->activeCamera();
        Vec3 n = normalize(cam.eye() - Vec3{0, 0, 0});
        Vec3 t1 = normalize(cross({0, 1, 0}, n)), t2 = cross(n, t1);
        Vec3 corners[4] = {normalize(n + t1 * 0.35f + t2 * 0.35f), normalize(n - t1 * 0.35f + t2 * 0.35f),
                           normalize(n - t1 * 0.35f - t2 * 0.35f), normalize(n + t1 * 0.35f - t2 * 0.35f)};
        for (const Vec3& c : corners) { QPoint q = vp->worldToWidget(c); moveTo(vp, q); click(vp, q); }
        SceneObject* topo = s.lead();
        CHECK(topo && topo->id != live->id && topo->mesh().numVerts() == 4);
        QPoint mid = vp->worldToWidget(n);
        moveTo(vp, mid, Qt::NoButton, Qt::ShiftModifier);
        click(vp, mid, Qt::ShiftModifier);
        if (topo) {
            CHECK(topo->mesh().numFaces() == 1);
            float maxErr = 0;
            for (const Vec3& q : topo->mesh().positions) maxErr = std::max(maxErr, std::abs(length(q) - 1.0f));
            std::printf("  retopo verts on surface, max error %.4f\n", maxErr);
            CHECK(maxErr < 0.02f);
            // Face should point outward (towards the camera).
            if (topo->mesh().numFaces()) CHECK(dot(topo->mesh().faceNormal(0), n) > 0.5f);
            // Shift+drag relaxes without breaking anything; undo removes the face.
            key(vp, Qt::Key_Z, Qt::ControlModifier);
            CHECK(topo->mesh().numFaces() == 0);
        }
    }
    std::printf("%s quad draw\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- UV editor: automatic projection, marquee select, MMB move, undo ----
    s.clear();
    s.setTool(Tool::Select);
    M::createPrimitive(s, "polyCube");
    SceneObject* uc = s.lead();
    M::projectUVs(s, M::Projection::Automatic);
    CHECK(uc->mesh().hasUVs());
    key(vp, Qt::Key_U, Qt::ControlModifier | Qt::ShiftModifier);
    UVCanvas* canvas = w.findChild<UVCanvas*>();
    CHECK(canvas && canvas->isVisible());
    if (canvas) {
        QApplication::processEvents();
        canvas->frame(false);
        drag(canvas, QPoint(2, 2), QPoint(canvas->width() - 2, canvas->height() - 2));
        CHECK(uc->sel.uvs.size() == uc->mesh().uvs.size());
        Vec2 before = uc->mesh().uvs[0];
        drag(canvas, QPoint(50, 50), QPoint(90, 50), Qt::MiddleButton);
        Vec2 after = uc->mesh().uvs[0];
        CHECK(after.x > before.x + 1e-3f && std::abs(after.y - before.y) < 1e-5f);
        key(vp, Qt::Key_Z, Qt::ControlModifier);
        CHECK(std::abs(uc->mesh().uvs[0].x - before.x) < 1e-6f);
    }
    std::printf("%s uv editor\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- checker display renders ----
    key(vp, Qt::Key_6);
    CHECK(vp->options.textured);
    vp->repaint();
    key(vp, Qt::Key_5);
    CHECK(!vp->options.textured);

    // ---- FBX export / import through the app ----
    {
        QTemporaryDir td;
        QString fp = td.filePath("scene.fbx");
        M::createPrimitive(s, "polyTorus");
        s.lead()->st.translate = {2, 0, 0};
        size_t count = s.objects.size();
        QString msg = M::exportFbx(s, fp, false);
        s.clear();
        msg = M::importFbx(s, fp);
        CHECK(s.objects.size() == count);
        bool torusOk = false;
        for (auto& o : s.objects) torusOk |= o->st.name == "pTorus1" && std::abs(o->st.translate.x - 2) < 1e-5f && o->mesh().hasUVs();
        CHECK(torusOk);
    }
    std::printf("%s fbx via app\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- hard / soft edges ----
    s.clear();
    s.setTool(Tool::Select);
    s.setMode(SelectMode::Object);
    M::createPrimitive(s, "polyCube");
    SceneObject* hc = s.lead();
    CHECK(hc->mesh().hasHardEdges());
    vp->repaint(); // split-normal render path
    M::softenEdges(s);
    CHECK(!hc->mesh().hasHardEdges());
    vp->repaint();
    M::setNormalAngle(s, 30.0f);
    CHECK(hc->mesh().hasHardEdges());
    key(vp, Qt::Key_Z, Qt::ControlModifier);
    CHECK(!hc->mesh().hasHardEdges());
    std::printf("%s hard/soft edges\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- Smart Unwrap plugin loaded by the app ----
    {
        PluginManager* pm = w.pluginManager();
        CHECK(pm && !pm->plugins().empty());
        const PluginCommand* smartCmd = nullptr;
        if (pm)
            for (const auto& lp : pm->plugins())
                for (const auto& c : lp.commands)
                    if (c.id == "smartUnwrap") smartCmd = &c;
        CHECK(smartCmd != nullptr);
        if (smartCmd) {
            M::createPrimitive(s, "polyTorus");
            SceneObject* t = s.lead();
            Mesh before = t->mesh();
            QString msg = pm->run(*smartCmd);
            std::printf("  %s\n", qPrintable(msg));
            auto q = uv::measure(t->mesh(), t->worldMatrix());
            CHECK(q.flippedFaces == 0 && q.overlap < 0.005f && q.densitySpread < 0.03f);
            CHECK(t->st.history.has_value()); // uv-only plugin keeps construction history
            key(vp, Qt::Key_Z, Qt::ControlModifier);
            CHECK(t->mesh().uvs.size() == before.uvs.size());
            // The command is in the Plugins menu and the UV menu.
            int found = 0;
            for (QAction* a : w.findChildren<QAction*>())
                if (a->text().startsWith("Smart Unwrap")) ++found;
            CHECK(found >= 2);
        }
    }
    std::printf("%s smart unwrap plugin\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- sculpting through the viewport ----
    {
        s.clear();
        s.setTool(Tool::Select);
        s.setMode(SelectMode::Object);
        M::createPrimitive(s, "polySphere");
        SceneObject* sp = s.lead();
        sp->st.history->set("subdivisionsAxis", 64);
        sp->st.history->set("subdivisionsHeight", 48);
        sp->st.mesh = std::make_shared<Mesh>(buildPrimitive(*sp->st.history));
        vp->frameSelected();
        s.setTool(Tool::Sculpt);
        s.sculpt.brush = sculpt::Brush::Draw;
        s.sculpt.strength = 1.0f;
        vp->repaint();
        const Camera& cam = vp->activeCamera();
        Vec3 n = normalize(cam.eye());
        Vec3 t1 = normalize(cross({0, 1, 0}, n));
        auto maxR = [&] { float r = 0; for (auto& q : sp->mesh().positions) r = std::max(r, length(q)); return r; };
        QPoint a = vp->worldToWidget(normalize(n - t1 * 0.3f)), b = vp->worldToWidget(normalize(n + t1 * 0.3f));
        drag(vp, a, b);
        float r1 = maxR();
        std::printf("  after draw stroke max radius %.3f\n", r1);
        CHECK(r1 > 1.01f);
        CHECK(!sp->st.history.has_value());
        key(vp, Qt::Key_Z, Qt::ControlModifier);
        CHECK(maxR() < 1.0001f);
        // Mask the region first: then the same stroke does nothing there.
        s.sculpt.brush = sculpt::Brush::Mask;
        drag(vp, a, b);
        CHECK(sp->hasMask());
        float masked = 0;
        for (float m : sp->mask) masked = std::max(masked, m);
        CHECK(masked > 0.5f);
        vp->repaint(); // mask shading path
        s.sculpt.brush = sculpt::Brush::Draw;
        drag(vp, a, b);
        std::printf("  masked stroke max radius %.3f\n", maxR());
        CHECK(maxR() < 1.0f + (r1 - 1.0f) * 0.15f); // >85% of the effect blocked (mask fades at the brush rim)
        M::clearMask(s);
        // B + drag changes the radius.
        float r0 = s.sculpt.radiusPx;
        const bool softBefore = s.softSelect;
        QTest::keyPress(vp, Qt::Key_B);
        drag(vp, QPoint(100, 100), QPoint(160, 100));
        QTest::keyRelease(vp, Qt::Key_B);
        CHECK(s.sculpt.radiusPx > r0 * 1.3f);
        CHECK(s.softSelect == softBefore); // B in sculpt mode never toggles soft select
        s.setTool(Tool::Select);
        // Weighted normals render path.
        M::setWeightedNormals(s, 3.0f);
        CHECK(sp->mesh().normalWeightPower == 3.0f);
        vp->repaint();
        key(vp, Qt::Key_Z, Qt::ControlModifier);
        CHECK(sp->mesh().normalWeightPower == 0.0f);
    }
    std::printf("%s sculpt + weighted normals\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- hotkeys: defaults, editor rebinding, conflicts, persistence ----
    {
        s.clear();
        s.setTool(Tool::Select);
        s.setMode(SelectMode::Object);
        HotkeyManager* hk = w.hotkeyManager();
        CHECK(hk && hk->entries().size() > 100);
        auto shortcutOf = [&](const QString& id) {
            auto* e = hk->find(id);
            return e && e->action ? e->action->shortcut().toString(QKeySequence::PortableText) : QString("<missing>");
        };
        CHECK(shortcutOf("Edit/Duplicate") == "Ctrl+D");
        CHECK(shortcutOf("Tools/Multi-Cut Tool") == "Ctrl+Shift+X");
        CHECK(shortcutOf("Smart Unwrap/Smart Unwrap") == "Ctrl+Alt+U");
        // Rebind Duplicate to Ctrl+J and use it.
        M::createPrimitive(s, "polyCube");
        hk->assign("Edit/Duplicate", QKeySequence("Ctrl+J"));
        key(vp, Qt::Key_J, Qt::ControlModifier);
        CHECK(s.objects.size() == 2);
        key(vp, Qt::Key_D, Qt::ControlModifier); // old binding no longer duplicates
        CHECK(s.objects.size() == 2);
        CHECK(QSettings().value("hotkeys/Edit/Duplicate").toStringList() == QStringList{"Ctrl+J"});
        // Conflicts: giving Move the Q key takes it from the Select Tool.
        auto taken = hk->assign("Modify/Move Tool", QKeySequence("Q"));
        CHECK(taken.contains("Modify/Select Tool"));
        CHECK(shortcutOf("Modify/Select Tool").isEmpty());
        key(vp, Qt::Key_Q);
        CHECK(s.tool() == Tool::Move);
        hk->resetAll();
        CHECK(shortcutOf("Modify/Select Tool") == "Q" && shortcutOf("Edit/Duplicate") == "Ctrl+D");
        CHECK(!QSettings().contains("hotkeys/Edit/Duplicate"));
        // The editor dialog opens and lists commands.
        key(vp, Qt::Key_F1);
        auto* ed = w.findChild<HotkeyEditor*>();
        CHECK(ed && ed->isVisible());
        if (ed) ed->close();
        // New default hotkeys.
        key(vp, Qt::Key_X, Qt::ControlModifier | Qt::ShiftModifier);
        CHECK(s.tool() == Tool::MultiCut);
        key(vp, Qt::Key_W);
        key(vp, Qt::Key_Y); // Maya: last tool used
        CHECK(s.tool() == Tool::MultiCut);
        key(vp, Qt::Key_Q);
        float g0 = vp->manipulatorScale();
        key(vp, Qt::Key_Equal);
        CHECK(vp->manipulatorScale() > g0);
        key(vp, Qt::Key_Minus);
        CHECK(std::abs(vp->manipulatorScale() - g0) < 1e-4f);
    }
    std::printf("%s hotkeys + editor\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- isolate select (Ctrl+1) ----
    {
        s.clear();
        M::createPrimitive(s, "polyCube");
        SceneObject* keep = s.lead();
        M::createPrimitive(s, "polySphere");
        SceneObject* other = s.lead();
        other->st.translate = {3, 0, 0};
        vp->frameAll();
        s.selectObjects({keep->id}, SelectOp::Replace);
        key(vp, Qt::Key_1, Qt::ControlModifier);
        CHECK(s.isolateActive && s.isShown(keep) && !s.isShown(other));
        vp->repaint();
        click(vp, vp->worldToWidget({3, 0, 0})); // hidden object is not pickable
        CHECK(s.selection().empty());
        key(vp, Qt::Key_1, Qt::ControlModifier);
        CHECK(!s.isolateActive && s.isShown(other));
    }
    std::printf("%s isolate select\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- marking menus: create (Shift+RMB on empty), sculpt brushes (RMB in Sculpt) ----
    {
        s.clear();
        s.setTool(Tool::Select);
        vp->frameAll();
        vp->repaint();
        QPoint c0(vp->width() / 2, vp->height() / 2);
        press(vp, c0, Qt::RightButton, Qt::ShiftModifier);
        for (int i = 1; i <= 6; ++i) moveTo(vp, c0 - QPoint(0, 20 * i), Qt::RightButton, Qt::ShiftModifier);
        release(vp, c0 - QPoint(0, 120), Qt::RightButton, Qt::ShiftModifier);
        QApplication::processEvents();
        CHECK(s.objects.size() == 1 && s.objects[0]->st.history && s.objects[0]->st.history->type == "polySphere");
        // Sculpt: RMB flick south -> Smooth brush.
        vp->frameAll();
        s.setTool(Tool::Sculpt);
        s.sculpt.brush = sculpt::Brush::Draw;
        vp->repaint();
        QPoint c = vp->worldToWidget({0, 0, 0});
        press(vp, c, Qt::RightButton);
        for (int i = 1; i <= 6; ++i) moveTo(vp, c + QPoint(0, 15 * i), Qt::RightButton);
        release(vp, c + QPoint(0, 90), Qt::RightButton);
        QApplication::processEvents();
        CHECK(s.sculpt.brush == sculpt::Brush::Smooth);
        s.setTool(Tool::Select);
    }
    std::printf("%s create + sculpt marking menus\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- hotbox centre zone: view menu ----
    {
        QTest::keyPress(vp, Qt::Key_Space);
        QTest::qWait(350);
        Hotbox* hb = w.findChild<Hotbox*>();
        CHECK(hb && hb->isVisible());
        if (hb && hb->isVisible()) {
            QPoint centre = hb->mapFromGlobal(QCursor::pos());
            QPoint local = hb->rect().contains(centre) ? centre : hb->rect().center();
            // The hotbox is centred where the cursor was when Space went down.
            press(hb, local);
            MarkingMenu* mm = nullptr;
            for (auto* m : w.findChildren<MarkingMenu*>()) if (m->isVisible()) mm = m;
            CHECK(mm != nullptr);
            if (mm) mm->hide();
        }
        QTest::keyRelease(vp, Qt::Key_Space);
        vp->setSingleView(ViewKind::Persp);
    }
    std::printf("%s hotbox centre views\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- UV editor marking menu: RMB flick south -> UV Shell mode ----
    {
        s.clear();
        M::createPrimitive(s, "polyCube");
        key(vp, Qt::Key_U, Qt::ControlModifier | Qt::ShiftModifier);
        UVCanvas* cv = w.findChild<UVCanvas*>();
        CHECK(cv && cv->isVisible());
        if (cv) {
            QApplication::processEvents();
            cv->mode = UVCanvas::Mode::UV;
            QPoint c(cv->width() / 2, cv->height() / 2);
            press(cv, c, Qt::RightButton);
            for (int i = 1; i <= 6; ++i) moveTo(cv, c + QPoint(0, 15 * i), Qt::RightButton);
            release(cv, c + QPoint(0, 90), Qt::RightButton);
            QApplication::processEvents();
            CHECK(cv->mode == UVCanvas::Mode::Shell);
            // Shift+RMB flick west -> cylindrical projection (undoable uv edit).
            size_t undoBefore = size_t(s.undo.count());
            press(cv, c, Qt::RightButton, Qt::ShiftModifier);
            for (int i = 1; i <= 6; ++i) moveTo(cv, c - QPoint(20 * i, 0), Qt::RightButton, Qt::ShiftModifier);
            release(cv, c - QPoint(120, 0), Qt::RightButton, Qt::ShiftModifier);
            QApplication::processEvents();
            CHECK(size_t(s.undo.count()) == undoBefore + 1);
        }
    }
    std::printf("%s uv editor marking menus\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- right-click context menus: Outliner and Channel Box ----
    {
        s.clear();
        M::createPrimitive(s, "polyCube");
        SceneObject* cb = s.lead();
        QMenu* om = w.buildOutlinerMenu();
        QAction* dup = nullptr;
        for (QAction* a : om->actions()) if (a->text() == "Duplicate") dup = a;
        CHECK(dup && dup->isEnabled());
        if (dup) dup->trigger();
        CHECK(s.objects.size() == 2);
        delete om;
        s.selectObjects({cb->id}, SelectOp::Replace);
        cb->st.translate.x = 4.0f;
        s.notifyChanged();
        ChannelBox* box = w.findChild<ChannelBox*>();
        CHECK(box != nullptr);
        if (box) {
            int tx = -1;
            for (int i = 0; i < box->channelCount(); ++i) if (box->channelLabel(i) == "Translate X") tx = i;
            CHECK(tx >= 0);
            QMenu* cm = box->channelMenu(tx);
            CHECK(!cm->actions().isEmpty() && cm->actions()[0]->text().startsWith("Reset Translate X"));
            cm->actions()[0]->trigger();
            CHECK(std::abs(cb->st.translate.x) < 1e-6f);
            delete cm;
        }
    }
    std::printf("%s context menus\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- save / load roundtrip ----
    s.clear();
    M::createPrimitive(s, "polyCube");
    M::createPrimitive(s, "polySphere");
    cube = s.objects[0].get();
    QTemporaryDir tmp;
    QString path = tmp.filePath("t.facet");
    uint32_t faces = cube->mesh().numFaces();
    CHECK(s.save(path));
    CHECK(s.load(path));
    CHECK(s.objects.size() == 2 && s.objects[0]->mesh().numFaces() == faces);
    std::printf("%s save/load\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    std::printf(g_fail ? "%d UI check(s) failed\n" : "All UI tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
