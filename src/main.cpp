#include "scene/Modeling.h"
#include "ui/MainWindow.h"
#include "viewport/Viewport.h"

#include <QApplication>
#include <QPainter>
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
    cli.addOptions({demo, shot, quad});
    cli.addPositionalArgument("scene", "A .facet scene or .obj file to open.");
    cli.process(app);

    MainWindow w;
    w.show();
    const QStringList pos = cli.positionalArguments();
    if (!pos.isEmpty()) {
        if (pos[0].endsWith(".obj", Qt::CaseInsensitive)) w.report(modeling::importObj(*w.scene(), pos[0]));
        else w.scene()->load(pos[0]);
    } else if (cli.isSet(demo) || cli.isSet(shot)) {
        buildDemo(w, cli.value(demo));
    }
    if (cli.isSet(quad)) w.viewport()->setQuadView(true);
    QTimer::singleShot(0, &w, [&w] { w.viewport()->frameAll(true); });

    if (cli.isSet(shot)) {
        QString file = cli.value(shot);
        QTimer::singleShot(900, &w, [&w, file] {
            // QWidget::grab() re-renders GL widgets without their QPainter overlay, so composite
            // the viewport's own framebuffer (which includes the HUD) over the window grab.
            QPixmap shot = w.grab();
            QImage vp = w.viewport()->grabFramebuffer();
            QPainter p(&shot);
            p.drawImage(QRect(w.viewport()->mapTo(&w, QPoint(0, 0)), w.viewport()->size()), vp);
            p.end();
            shot.save(file);
            QApplication::quit();
        });
    }
    return app.exec();
}
