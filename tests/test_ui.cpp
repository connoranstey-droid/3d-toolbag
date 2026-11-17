// End-to-end interaction tests: real MainWindow + Viewport driven with synthesized input.
#include "scene/Modeling.h"
#include "ui/MainWindow.h"
#include "viewport/Viewport.h"

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
    std::printf("%s space quad toggle\n", (g_fail == g_mark ? "PASS" : "FAIL")); g_mark = g_fail;

    // ---- save / load roundtrip ----
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
