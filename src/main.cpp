#include "scene/Modeling.h"
#include "ui/MainWindow.h"
#include "ui/PluginManager.h"
#include "ui/MarkingMenu.h"
#include "ui/Hotkeys.h"
#include "mesh/MeshOps.h"
#include "mesh/Sculpt.h"
#include "viewport/Viewport.h"

#include <QApplication>
#include <QPainter>
#include <QMouseEvent>
#include <QCommandLineParser>
#include <QStyleFactory>
#include <QSurfaceFormat>
#include <QTimer>

using namespace facet;

static void applyDarkTheme(QApplication& app)
{
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window, QColor(68, 68, 68));
    p.setColor(QPalette::WindowText, QColor(220, 220, 220));
    p.setColor(QPalette::Base, QColor(43, 43, 43));
    p.setColor(QPalette::AlternateBase, QColor(56, 56, 56));
    p.setColor(QPalette::ToolTipBase, QColor(60, 60, 60));
    p.setColor(QPalette::ToolTipText, QColor(230, 230, 230));
    p.setColor(QPalette::Text, QColor(220, 220, 220));
    p.setColor(QPalette::Button, QColor(82, 82, 82));
    p.setColor(QPalette::ButtonText, QColor(225, 225, 225));
    p.setColor(QPalette::Highlight, QColor(82, 133, 166));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(120, 120, 120));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(120, 120, 120));
    app.setPalette(p);
}

// Small scripted scene used by --demo and --screenshot (handy for smoke tests).
static void buildDemo(MainWindow& w, const QString& variant)
{
    Scene& s = *w.scene();
    if (variant == "bevel") {
        modeling::createPrimitive(s, "polyCube");
        SceneObject* c = s.lead();
        c->st.translate = {-1.6f, 0.5f, 0};
        s.setMode(SelectMode::Edge);
        for (uint32_t e = 0; e < c->mesh().numEdges(); ++e) c->sel.edges.push_back(e);
        modeling::bevel(s, 0.12f, 3, true);
        s.setMode(SelectMode::Object);
        modeling::createPrimitive(s, "polyCylinder");
        SceneObject* cy = s.lead();
        cy->st.translate = {1.4f, 1.0f, 0};
        s.setMode(SelectMode::Edge);
        const Mesh& m = cy->mesh();
        for (uint32_t e = 0; e < m.numEdges(); ++e)
            if (m.edges[e].faceCount == 2 && std::abs(std::abs(m.positions[m.edges[e].v0].y) - 1) < 1e-4f &&
                std::abs(m.positions[m.edges[e].v0].y - m.positions[m.edges[e].v1].y) < 1e-4f) // rim edges only
                cy->sel.edges.push_back(e);
        modeling::bevel(s, 0.15f, 4, true);
        s.setMode(SelectMode::Object);
        s.selectObjects({c->id}, SelectOp::Replace);
        w.viewport()->options.wireOnShaded = true;
        s.undo.setClean();
        return;
    }
    if (variant == "uv") {
        modeling::createPrimitive(s, "polyCylinder");
        SceneObject* cy = s.lead();
        cy->st.translate = {1.2f, 1.0f, 0};
        modeling::createPrimitive(s, "polyCube");
        SceneObject* c = s.lead();
        c->st.translate = {-1.4f, 0.6f, 0};
        modeling::projectUVs(s, modeling::Projection::Automatic);
        // Unfold the cylinder's seamed default UVs (side + two caps) with LSCM, then pack.
        s.selectObjects({cy->id}, SelectOp::Replace);
        modeling::unfoldUVs(s);
        modeling::layoutUVs(s);
        w.setChecker(true);
        w.openUVEditor();
        s.undo.setClean();
        return;
    }
    if (variant == "sculpt") {
        modeling::createPrimitive(s, "polySphere");
        SceneObject* sp = s.lead();
        sp->st.history->set("subdivisionsAxis", 160);
        sp->st.history->set("subdivisionsHeight", 120);
        sp->st.mesh = std::make_shared<Mesh>(buildPrimitive(*sp->st.history));
        sp->st.history.reset();
        sp->st.translate = {1.2f, 1.1f, 0};
        Mesh& m = *sp->st.mesh;
        sculpt::Stroke st;
        auto stroke = [&](sculpt::Brush b, std::vector<Vec3> path, float r, float strength, bool inv = false, int repeat = 1) {
            st.begin(m, &sp->mask, b, r, 0);
            for (int k = 0; k < repeat; ++k)
                for (size_t i = 0; i + 1 < path.size(); ++i)
                    for (int j = 0; j < 12; ++j) st.dab({normalize(lerp(path[i], path[i + 1], j / 12.0f)), r, strength, inv});
            st.end();
        };
        // Brow ridge, cheek volumes, a creased mouth line and eye sockets (mirrored in X).
        stroke(sculpt::Brush::Draw, {{0.15f, 0.45f, 0.9f}, {0.55f, 0.4f, 0.75f}}, 0.22f, 1.0f, false, 3);
        stroke(sculpt::Brush::Inflate, {{0.45f, -0.1f, 0.85f}, {0.5f, -0.2f, 0.8f}}, 0.3f, 0.8f, false, 3);
        stroke(sculpt::Brush::Crease, {{0.0f, -0.45f, 0.9f}, {0.35f, -0.4f, 0.85f}}, 0.12f, 1.0f, false, 4);
        stroke(sculpt::Brush::Draw, {{0.3f, 0.2f, 0.95f}, {0.32f, 0.22f, 0.94f}}, 0.16f, 1.0f, true, 6);
        stroke(sculpt::Brush::Draw, {{0.0f, 0.1f, 1.0f}, {0.0f, -0.2f, 1.0f}}, 0.15f, 1.0f, false, 4); // nose
        stroke(sculpt::Brush::Smooth, {{0.0f, 0.0f, 1.0f}, {0.4f, 0.0f, 0.9f}}, 0.35f, 0.3f);
        sp->mask.assign(m.numVerts(), 0.0f);
        stroke(sculpt::Brush::Mask, {{0.0f, 1.0f, 0.2f}, {0.0f, 0.8f, -0.6f}}, 0.45f, 1.0f, false, 2);
        sp->maskVersion++;
        m.touchPositions();
        // Hard-surface piece with weighted normals.
        modeling::createPrimitive(s, "polyCube");
        SceneObject* box = s.lead();
        box->st.translate = {-1.6f, 0.6f, 0};
        box->st.scale = {1.5f, 1.2f, 1.2f};
        s.setMode(SelectMode::Edge);
        for (uint32_t e = 0; e < box->mesh().numEdges(); ++e) box->sel.edges.push_back(e);
        modeling::bevel(s, 0.08f, 2, true);
        s.setMode(SelectMode::Object);
        modeling::softenEdges(s);
        modeling::setWeightedNormals(s, 3.0f);
        s.selectObjects({sp->id}, SelectOp::Replace);
        s.symmetry = 0;
        s.setTool(Tool::Sculpt);
        s.undo.setClean();
        return;
    }
    if (variant == "smartuv") {
        modeling::createPrimitive(s, "polyCube");
        SceneObject* box = s.lead();
        box->st.translate = {-1.7f, 0.6f, 0};
        box->st.scale = {1.6f, 1.0f, 1.2f};
        s.setMode(SelectMode::Edge);
        for (uint32_t e = 0; e < box->mesh().numEdges(); ++e) box->sel.edges.push_back(e);
        modeling::bevel(s, 0.12f, 3, true);
        s.setMode(SelectMode::Object);
        modeling::createPrimitive(s, "polySphere");
        SceneObject* sp = s.lead();
        sp->st.history->set("subdivisionsAxis", 48);
        sp->st.history->set("subdivisionsHeight", 32);
        sp->st.mesh = std::make_shared<Mesh>(buildPrimitive(*sp->st.history));
        for (auto& p : sp->st.mesh->positions) p = p * (1.0f + 0.12f * std::sin(p.x * 3.1f) * std::cos(p.y * 2.7f));
        sp->st.mesh->touchPositions();
        sp->st.history.reset();
        sp->st.translate = {1.4f, 1.0f, 0};
        s.selectObjects({box->id, sp->id}, SelectOp::Replace);
        for (const auto& lp : w.pluginManager()->plugins())
            for (const auto& c : lp.commands)
                if (c.id == "smartUnwrap") w.report(w.pluginManager()->run(c));
        w.setChecker(true);
        w.openUVEditor();
        s.undo.setClean();
        return;
    }
    if (variant == "retopo") {
        modeling::createPrimitive(s, "polySphere");
        SceneObject* sp = s.lead();
        sp->st.history->set("subdivisionsAxis", 80);
        sp->st.history->set("subdivisionsHeight", 60);
        sp->st.history->set("radius", 2);
        sp->st.mesh = std::make_shared<Mesh>(buildPrimitive(*sp->st.history));
        // Lumpy "sculpt" so the retopo visibly follows a surface.
        for (auto& p : sp->st.mesh->positions) p = p * (1.0f + 0.08f * std::sin(p.x * 3.0f) * std::cos(p.y * 4.0f));
        sp->st.mesh->touchPositions();
        sp->st.history.reset();
        modeling::toggleLive(s);
        modeling::createPrimitive(s, "polyPlane");
        SceneObject* pl = s.lead();
        pl->st.history->set("width", 2.4);
        pl->st.history->set("height", 2.4);
        pl->st.history->set("subdivisionsWidth", 6);
        pl->st.history->set("subdivisionsHeight", 6);
        pl->st.mesh = std::make_shared<Mesh>(buildPrimitive(*pl->st.history));
        pl->st.name = "retopo1";
        pl->st.translate = {0, 2.3f, 0};
        modeling::conform(s);
        s.setTool(Tool::QuadDraw);
        s.undo.setClean();
        return;
    }
    modeling::createPrimitive(s, "polyCube");
    SceneObject* cube = s.lead();
    cube->st.translate = {-2.2f, 0.5f, 0};
    cube->st.smoothDisplay = 1;
    // Hold the cube's top edges sharp so smooth preview shows a crease.
    s.setMode(SelectMode::Edge);
    for (uint32_t e = 0; e < cube->mesh().numEdges(); ++e) {
        const auto& ed = cube->mesh().edges[e];
        if (cube->mesh().positions[ed.v0].y > 0.4f && cube->mesh().positions[ed.v1].y > 0.4f) cube->sel.edges.push_back(e);
    }
    modeling::setCrease(s, 10.0f);
    s.setMode(SelectMode::Object);

    modeling::createPrimitive(s, "polySphere");
    s.lead()->st.translate = {0.4f, 1.0f, 0};
    modeling::createPrimitive(s, "polyTorus");
    SceneObject* torus = s.lead();
    torus->st.translate = {3.2f, 0.5f, 0};
    torus->st.rotate = {60, 0, 0};
    torus->st.smoothDisplay = 2;
    torus->st.smoothLevel = 2;

    if (variant == "faces") {
        s.selectObjects({cube->id}, SelectOp::Replace);
        s.setMode(SelectMode::Face);
        s.setComponents(cube, SelectMode::Face, {4});
        modeling::extrude(s, true);
    } else if (variant == "soft") {
        modeling::createPrimitive(s, "polyPlane");
        SceneObject* pl = s.lead();
        pl->st.translate = {0, 0, 3.5f};
        pl->st.scale = {6, 1, 3};
        s.setMode(SelectMode::Vertex);
        s.setComponents(pl, SelectMode::Vertex, {60});
        s.softSelect = true;
        s.softRadius = 0.35f;
        s.setTool(Tool::Move);
    } else {
        s.selectObjects({torus->id}, SelectOp::Replace);
        s.setTool(Tool::Rotate);
    }
    s.undo.setClean();
}

int main(int argc, char** argv)
{
    QSurfaceFormat fmt;
    fmt.setVersion(4, 1);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    fmt.setSamples(4);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    app.setApplicationName("Facet");
    app.setOrganizationName("Facet");
    applyDarkTheme(app);

    QCommandLineParser cli;
    cli.addHelpOption();
    QCommandLineOption demo("demo", "Open with a demo scene (variant: default, faces, soft).", "variant");
    QCommandLineOption shot("screenshot", "Render the demo scene to an image and exit.", "file");
    QCommandLineOption quad("quad", "Start in four-view layout.");
    QCommandLineOption show("show", "With --screenshot: also open 'hotkeys' (Hotkey Editor) or 'menu' (RMB marking menu).", "what");
    cli.addOptions({demo, shot, quad, show});
    cli.addPositionalArgument("scene", "A .facet scene or .obj file to open.");
    cli.process(app);

    MainWindow w;
    w.show();
    const QStringList pos = cli.positionalArguments();
    if (!pos.isEmpty()) {
        if (pos[0].endsWith(".obj", Qt::CaseInsensitive) || pos[0].endsWith(".fbx", Qt::CaseInsensitive))
            w.report(modeling::importFile(*w.scene(), pos[0]));
        else w.scene()->load(pos[0]);
    } else if (cli.isSet(demo) || cli.isSet(shot)) {
        buildDemo(w, cli.value(demo));
    }
    if (cli.isSet(quad)) w.viewport()->setQuadView(true);
    QTimer::singleShot(0, &w, [&w] { w.viewport()->frameAll(true); });

    if (cli.isSet(shot)) {
        QString file = cli.value(shot);
        const QString what = cli.value(show);
        if (what == "menu")
            QTimer::singleShot(600, &w, [&w] {
                // Simulate an RMB press in the middle of the viewport (opens the tool's marking menu).
                QPoint c(w.viewport()->width() / 2, w.viewport()->height() / 2);
                QMouseEvent e(QEvent::MouseButtonPress, QPointF(c), QPointF(w.viewport()->mapToGlobal(c)), Qt::RightButton, Qt::RightButton, {});
                QApplication::sendEvent(w.viewport(), &e);
                QMouseEvent m(QEvent::MouseMove, QPointF(c + QPoint(0, -60)), QPointF(w.viewport()->mapToGlobal(c + QPoint(0, -60))), Qt::NoButton,
                              Qt::RightButton, {});
                QApplication::sendEvent(w.viewport(), &m);
            });
        if (what == "hotkeys") QTimer::singleShot(600, &w, [&w] { w.showHotkeyEditor(); });
        QTimer::singleShot(900, &w, [&w, file, what] {
            // QWidget::grab() re-renders GL widgets without their QPainter overlay, so composite
            // the viewport's own framebuffer (which includes the HUD) over the window grab.
            QPixmap shot = w.grab();
            QImage vp = w.viewport()->grabFramebuffer();
            QPainter p(&shot);
            p.drawImage(QRect(w.viewport()->mapTo(&w, QPoint(0, 0)), w.viewport()->size()), vp);
            // Overlays (marking menus, hotbox) sit above the GL viewport: paint them last.
            for (QWidget* ov : w.findChildren<MarkingMenu*>())
                if (ov->isVisible()) ov->render(&p, ov->mapTo(&w, QPoint(0, 0)), QRegion(), QWidget::DrawChildren);
            p.end();
            if (what == "hotkeys")
                if (auto* ed = w.findChild<HotkeyEditor*>()) {
                    QString f2 = file;
                    f2.insert(f2.lastIndexOf('.'), "-hotkeys");
                    ed->grab().save(f2);
                }
            shot.save(file);
            if (QWidget* uvw = w.uvEditorWindow(); uvw && uvw->isVisible()) {
                QString f2 = file;
                f2.insert(f2.lastIndexOf('.'), "-uveditor");
                uvw->grab().save(f2);
            }
            QApplication::quit();
        });
    }
    return app.exec();
}
