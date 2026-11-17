#include "ui/MainWindow.h"
#include "scene/Modeling.h"
#include "ui/ChannelBox.h"
#include "ui/MarkingMenu.h"
#include "ui/Outliner.h"
#include "viewport/Viewport.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace facet {

namespace M = modeling;

MainWindow::MainWindow()
{
    scene_ = new Scene(this);
    central_ = new QWidget(this);
    auto* lay = new QVBoxLayout(central_);
    lay->setContentsMargins(0, 0, 0, 0);
    viewport_ = new Viewport(scene_, central_);
    lay->addWidget(viewport_);
    setCentralWidget(central_);
    marking_ = new MarkingMenu(central_);
    hotbox_ = new Hotbox(central_);
    central_->installEventFilter(this);

    connect(viewport_, &Viewport::message, this, &MainWindow::report);
    connect(viewport_, &Viewport::markingMenuRequested, this, &MainWindow::showMarkingMenu);
    connect(viewport_, &Viewport::markingMenuTrack, marking_, &MarkingMenu::track);
    connect(viewport_, &Viewport::markingMenuRelease, marking_, &MarkingMenu::release);
    connect(viewport_, &Viewport::hotboxRequested, this, [this](QPoint g) {
        std::vector<std::vector<Hotbox::Entry>> rows(2);
        for (size_t i = 0; i < menus_.size(); ++i) rows[i < 7 ? 0 : 1].push_back({menus_[i].first, menus_[i].second});
        hotbox_->showAt(g, rows);
    });
    connect(viewport_, &Viewport::hotboxDismissed, hotbox_, &Hotbox::dismiss);

    buildMenus();
    buildStatusLine();
    buildShelf();
    buildDocks();

    connect(&scene_->undo, &QUndoStack::cleanChanged, this, &MainWindow::updateTitle);
    connect(scene_, &Scene::modeChanged, this, [this] {
        modeActs_[int(scene_->mode())]->setChecked(true);
        updateHelpLine();
    });
    connect(scene_, &Scene::toolChanged, this, [this] {
        toolActs_[int(scene_->tool())]->setChecked(true);
        updateHelpLine();
    });
    connect(scene_, &Scene::selectionChanged, this, [this] { softAct_->setChecked(scene_->softSelect); });
    updateTitle();
    updateHelpLine();
    resize(1600, 960);
}

void MainWindow::report(const QString& msg)
{
    if (!msg.isEmpty()) statusBar()->showMessage(msg, 6000);
}

QAction* MainWindow::addAct(QMenu* menu, const QString& text, const QList<QKeySequence>& keys, std::function<QString()> fn, bool repeatable)
{
    auto* a = new QAction(text, this);
    a->setShortcuts(keys);
    a->setShortcutContext(Qt::WindowShortcut);
    connect(a, &QAction::triggered, this, [this, fn, repeatable, text] {
        QString msg = fn ? fn() : QString();
        if (repeatable) {
            lastRepeatable_ = fn;
            lastRepeatableName_ = QString(text).remove('&');
        }
        report(msg);
    });
    if (menu) menu->addAction(a);
    addAction(a); // shortcuts work even when the menu bar is hidden
    return a;
}

void MainWindow::buildMenus()
{
    auto add = [this](const QString& title) {
        QMenu* m = menuBar()->addMenu(title);
        m->setTearOffEnabled(true);
        menus_.emplace_back(QString(title).remove('&'), m);
        return m;
    };
    Scene& s = *scene_;

    // ---- File ----
    QMenu* file = add("&File");
    addAct(file, "&New Scene", {QKeySequence("Ctrl+N")}, [this] { newScene(); return QString("New scene"); });
    addAct(file, "&Open Scene...", {QKeySequence("Ctrl+O")}, [this] { openScene(); return QString(); });
    addAct(file, "&Save Scene", {QKeySequence("Ctrl+S")}, [this] { saveScene(false); return QString(); });
    addAct(file, "Save Scene &As...", {QKeySequence("Ctrl+Shift+S")}, [this] { saveScene(true); return QString(); });
    file->addSeparator();
    addAct(file, "&Import OBJ...", {}, [this] {
        QString p = QFileDialog::getOpenFileName(this, "Import OBJ", {}, "Wavefront OBJ (*.obj)");
        return p.isEmpty() ? QString() : M::importObj(*scene_, p);
    });
    addAct(file, "Export &Selection to OBJ...", {}, [this] {
        QString p = QFileDialog::getSaveFileName(this, "Export Selection", "selection.obj", "Wavefront OBJ (*.obj)");
        return p.isEmpty() ? QString() : M::exportObj(*scene_, p, true);
    });
    addAct(file, "Export &All to OBJ...", {}, [this] {
        QString p = QFileDialog::getSaveFileName(this, "Export All", "scene.obj", "Wavefront OBJ (*.obj)");
        return p.isEmpty() ? QString() : M::exportObj(*scene_, p, false);
    });
    file->addSeparator();
    addAct(file, "E&xit", {QKeySequence("Ctrl+Q")}, [this] { close(); return QString(); });

    // ---- Edit ----
    QMenu* edit = add("&Edit");
    QAction* undo = s.undo.createUndoAction(this, "Undo");
    undo->setShortcuts({QKeySequence("Ctrl+Z"), QKeySequence("Z")});
    QAction* redo = s.undo.createRedoAction(this, "Redo");
    redo->setShortcuts({QKeySequence("Ctrl+Shift+Z"), QKeySequence("Shift+Z"), QKeySequence("Ctrl+Y")});
    edit->addAction(undo);
    edit->addAction(redo);
    addAction(undo);
    addAction(redo);
    addAct(edit, "Repeat Last", {QKeySequence("G")}, [this] {
        if (!lastRepeatable_) return QString("Nothing to repeat");
        return lastRepeatableName_ + ": " + lastRepeatable_();
    });
    edit->addSeparator();
    addAct(edit, "&Delete", {QKeySequence::Delete, QKeySequence(Qt::Key_Backspace)}, [&s] { return M::deleteSelection(s); });
    addAct(edit, "D&uplicate", {QKeySequence("Ctrl+D")}, [&s] { return M::duplicate(s); }, true);
    addAct(edit, "Delete &History", {QKeySequence("Alt+Shift+D")}, [&s] { return M::deleteHistory(s); });
    edit->addSeparator();
    addAct(edit, "Select &All", {QKeySequence("Ctrl+Shift+A")}, [&s] { return M::selectAll(s); });
    addAct(edit, "&Invert Selection", {QKeySequence("Ctrl+Shift+I")}, [&s] { return M::invertSelection(s); });
    addAct(edit, "Deselect All", {QKeySequence("Ctrl+Shift+D")}, [&s] {
        if (s.mode() == SelectMode::Object) s.selectObjects({}, SelectOp::Replace);
        else s.clearComponents();
        return QString("Cleared selection");
    });

    // ---- Create ----
    QMenu* create = add("&Create");
    QMenu* polys = create->addMenu("&Polygon Primitives");
    const std::pair<const char*, const char*> prims[] = {{"&Sphere", "polySphere"}, {"&Cube", "polyCube"}, {"C&ylinder", "polyCylinder"},
                                                         {"C&one", "polyCone"}, {"&Plane", "polyPlane"}, {"&Torus", "polyTorus"}};
    for (auto [label, type] : prims) {
        QString t = type;
        addAct(polys, label, {}, [&s, t] { return M::createPrimitive(s, t); }, true);
    }

    // ---- Select ----
    QMenu* sel = add("&Select");
    const char* modeNames[] = {"&Object Mode", "&Vertex", "&Edge", "&Face"};
    modeGroup_ = new QActionGroup(this);
    for (int i = 0; i < 4; ++i) {
        QKeySequence key = i == 0 ? QKeySequence("F8") : QKeySequence(Qt::Key(Qt::Key_F8 + i));
        SelectMode m = SelectMode(i);
        modeActs_[i] = addAct(sel, modeNames[i], {key}, [this, m] {
            Scene& sc = *scene_;
            // F8 toggles between object mode and the last component mode, like Maya.
            static SelectMode lastComp = SelectMode::Face;
            if (m == SelectMode::Object) {
                if (sc.mode() == SelectMode::Object) return M::setComponentMode(sc, lastComp);
                lastComp = sc.mode();
                return M::setComponentMode(sc, SelectMode::Object);
            }
            lastComp = m;
            return M::setComponentMode(sc, m);
        });
        modeActs_[i]->setCheckable(true);
        modeGroup_->addAction(modeActs_[i]);
    }
    modeActs_[0]->setChecked(true);
    sel->addSeparator();
    addAct(sel, "&Grow Selection", {QKeySequence(">"), QKeySequence("Shift+.")}, [&s] { return M::growSelection(s); });
    addAct(sel, "&Shrink Selection", {QKeySequence("<"), QKeySequence("Shift+,")}, [&s] { return M::shrinkSelection(s); });
    addAct(sel, "Select Edge &Loop", {}, [&s] { return M::selectEdgeLoop(s); });
    addAct(sel, "Select Edge &Ring", {}, [&s] { return M::selectEdgeRing(s); });
    sel->addSeparator();
    addAct(sel, "Convert to Vertices", {QKeySequence("Ctrl+F9")}, [&s] { return M::convertSelection(s, SelectMode::Vertex); });
    addAct(sel, "Convert to Edges", {QKeySequence("Ctrl+F10")}, [&s] { return M::convertSelection(s, SelectMode::Edge); });
    addAct(sel, "Convert to Faces", {QKeySequence("Ctrl+F11")}, [&s] { return M::convertSelection(s, SelectMode::Face); });
    sel->addSeparator();
    softAct_ = addAct(sel, "Soft Selection (B)", {}, [&s] {
        s.softSelect = !s.softSelect;
        s.notifySelectionChanged();
        return QString(s.softSelect ? "Soft Select on" : "Soft Select off");
    });
    softAct_->setCheckable(true);
    addAct(sel, "Soft Selection Radius...", {}, [this, &s] {
        bool ok = false;
        double r = QInputDialog::getDouble(this, "Soft Selection", "Falloff radius:", s.softRadius, 0.001, 10000, 3, &ok);
        if (ok) { s.softRadius = float(r); s.notifySelectionChanged(); }
        return QString();
    });

    // ---- Modify ----
    QMenu* mod = add("&Modify");
    toolGroup_ = new QActionGroup(this);
    const char* toolNames[] = {"&Select Tool", "&Move Tool", "&Rotate Tool", "S&cale Tool", "Insert &Edge Loop Tool"};
    const char* toolKeys[] = {"Q", "W", "E", "R", ""};
    for (int i = 0; i < 5; ++i) {
        Tool t = Tool(i);
        QList<QKeySequence> keys;
        if (toolKeys[i][0]) keys << QKeySequence(toolKeys[i]);
        toolActs_[i] = addAct(i < 4 ? mod : nullptr, toolNames[i], keys, [this, t] {
            if (t == Tool::InsertEdgeLoop && scene_->mode() == SelectMode::Object && scene_->lead()) scene_->setMode(SelectMode::Edge);
            scene_->setTool(t);
            return QString();
        });
        toolActs_[i]->setCheckable(true);
        toolGroup_->addAction(toolActs_[i]);
    }
    toolActs_[0]->setChecked(true);
    QMenu* orient = mod->addMenu("Move Axis Orientation");
    auto* og = new QActionGroup(this);
    const std::pair<const char*, AxisOrient> orients[] = {{"World", AxisOrient::World}, {"Object", AxisOrient::Object}, {"Normal / Component", AxisOrient::Normal}};
    for (auto [label, o] : orients) {
        AxisOrient ao = o;
        QAction* a = addAct(orient, label, {}, [&s, ao] { s.moveOrient = ao; s.notifySelectionChanged(); return QString(); });
        a->setCheckable(true);
        og->addAction(a);
        if (ao == AxisOrient::World) a->setChecked(true);
        connect(scene_, &Scene::toolChanged, a, [a, ao, &s] { a->setChecked(s.moveOrient == ao); });
    }
    QMenu* rorient = mod->addMenu("Rotate Axis Orientation");
    auto* rg = new QActionGroup(this);
    for (auto [label, o] : orients) {
        AxisOrient ao = o;
        QAction* a = addAct(rorient, label, {}, [&s, ao] { s.rotateOrient = ao; s.notifySelectionChanged(); return QString(); });
        a->setCheckable(true);
        rg->addAction(a);
        if (ao == AxisOrient::World) a->setChecked(true);
    }
    mod->addSeparator();
    addAct(mod, "&Freeze Transformations", {}, [&s] { return M::freezeTransforms(s); }, true);
    addAct(mod, "&Reset Transformations", {}, [&s] { return M::resetTransforms(s); }, true);
    addAct(mod, "&Center Pivot", {}, [&s] { return M::centerPivot(s); }, true);

    // ---- Display ----
    QMenu* disp = add("&Display");
    auto* shadeGroup = new QActionGroup(this);
    QAction* wire = addAct(disp, "&Wireframe", {QKeySequence("4")}, [this] { viewport_->options.shading = ShadingMode::Wireframe; viewport_->update(); return QString(); });
    QAction* shaded = addAct(disp, "&Smooth Shade All", {QKeySequence("5"), QKeySequence("6")}, [this] { viewport_->options.shading = ShadingMode::Shaded; viewport_->update(); return QString(); });
    wire->setCheckable(true);
    shaded->setCheckable(true);
    shaded->setChecked(true);
    shadeGroup->addAction(wire);
    shadeGroup->addAction(shaded);
    auto toggle = [this, disp](const QString& text, const QList<QKeySequence>& keys, bool DrawOptions::*field, bool initial) {
        QAction* a = addAct(disp, text, keys, nullptr);
        a->setCheckable(true);
        a->setChecked(initial);
        connect(a, &QAction::toggled, this, [this, field](bool on) { viewport_->options.*field = on; viewport_->update(); });
        return a;
    };
    toggle("Wireframe on Shaded", {}, &DrawOptions::wireOnShaded, false);
    toggle("Flat Shading", {}, &DrawOptions::flatShading, false);
    toggle("X-Ray", {QKeySequence("Alt+X")}, &DrawOptions::xray, false);
    toggle("Backface Culling", {}, &DrawOptions::backfaceCulling, false);
    toggle("Grid", {}, &DrawOptions::showGrid, true);
    QAction* hud = addAct(disp, "Poly Count HUD", {}, nullptr);
    hud->setCheckable(true);
    hud->setChecked(true);
    connect(hud, &QAction::toggled, this, [this](bool on) { viewport_->showHud = on; viewport_->update(); });
    addAct(disp, "Cycle Background", {QKeySequence("Alt+B")}, [this] { viewport_->cycleBackground(); return QString(); });
    disp->addSeparator();
    addAct(disp, "&Frame Selected", {QKeySequence("F")}, [this] { viewport_->frameSelected(false); return QString(); });
    addAct(disp, "Frame &All", {QKeySequence("A")}, [this] { viewport_->frameAll(false); return QString(); });
    addAct(disp, "Frame Selected in All Views", {QKeySequence("Shift+F")}, [this] { viewport_->frameSelected(true); return QString(); });
    addAct(disp, "Frame All in All Views", {QKeySequence("Shift+A")}, [this] { viewport_->frameAll(true); return QString(); });
    disp->addSeparator();
    addAct(disp, "Toggle Four View (tap Space)", {}, [this] { viewport_->toggleQuadView(); return QString(); });
    const std::pair<const char*, ViewKind> views[] = {{"Perspective View", ViewKind::Persp}, {"Top View", ViewKind::Top}, {"Front View", ViewKind::Front}, {"Side View", ViewKind::Side}};
    for (auto [label, k] : views) {
        ViewKind vk = k;
        addAct(disp, label, {}, [this, vk] { viewport_->setSingleView(vk); return QString(); });
    }
    disp->addSeparator();
    addAct(disp, "&Hide Selection", {QKeySequence("Ctrl+H")}, [&s] { return M::hideSelected(s); });
    addAct(disp, "&Show All", {QKeySequence("Shift+H")}, [&s] { return M::showAll(s); });

    // ---- Windows ----
    QMenu* windows = add("&Windows");
    windows->setObjectName("windowsMenu");
    addAct(windows, "Hotkey Reference", {QKeySequence("F1")}, [this] { showHotkeys(); return QString(); });

    // ---- Mesh ----
    QMenu* mesh = add("Mes&h");
    addAct(mesh, "&Combine", {}, [&s] { return M::combine(s); }, true);
    addAct(mesh, "&Separate", {}, [&s] { return M::separate(s); }, true);
    mesh->addSeparator();
    addAct(mesh, "Smoot&h", {}, [&s] { return M::smooth(s, 1); }, true);
    addAct(mesh, "Smooth (2 levels)", {}, [&s] { return M::smooth(s, 2); }, true);
    addAct(mesh, "&Triangulate", {}, [&s] { return M::triangulate(s); }, true);
    addAct(mesh, "&Fill Hole", {}, [&s] { return M::fillHole(s); }, true);
    QMenu* mir = mesh->addMenu("&Mirror");
    for (int ax = 0; ax < 3; ++ax) addAct(mir, QString("Mirror %1").arg(QChar("XYZ"[ax])), {}, [&s, ax] { return M::mirror(s, ax); }, true);

    // ---- Edit Mesh ----
    QMenu* em = add("Edit &Mesh");
    addAct(em, "&Extrude", {QKeySequence("Ctrl+E")}, [this] { return M::extrude(*scene_, keepFacesTogether_); }, true);
    QAction* keep = addAct(em, "Keep Faces Together", {}, nullptr);
    keep->setCheckable(true);
    keep->setChecked(true);
    connect(keep, &QAction::toggled, this, [this](bool on) { keepFacesTogether_ = on; });
    em->addSeparator();
    addAct(em, "&Merge", {}, [this] { return M::mergeVertices(*scene_, mergeThreshold_); }, true);
    addAct(em, "Merge Threshold...", {}, [this] {
        bool ok = false;
        double t = QInputDialog::getDouble(this, "Merge", "Distance threshold:", mergeThreshold_, 0.0, 1000, 5, &ok);
        if (ok) mergeThreshold_ = float(t);
        return QString();
    });
    addAct(em, "Merge to &Center", {}, [&s] { return M::mergeToCenter(s); }, true);
    addAct(em, "C&ollapse", {}, [&s] { return M::collapse(s); }, true);
    em->addSeparator();
    addAct(em, "Delete Edge/Vertex", {QKeySequence("Ctrl+Backspace"), QKeySequence("Ctrl+Delete")}, [&s] { return M::deleteSelection(s); });
    em->addSeparator();
    QMenu* crease = em->addMenu("&Crease");
    const std::pair<const char*, float> creases[] = {{"Crease 1 (soft)", 1.0f}, {"Crease 2", 2.0f}, {"Crease 3", 3.0f}, {"Full Crease (hard)", 10.0f}, {"Remove Crease", 0.0f}};
    for (auto [label, v] : creases) {
        float val = v;
        addAct(crease, label, {}, [&s, val] { return M::setCrease(s, val); }, true);
    }

    // ---- Mesh Tools ----
    QMenu* mt = add("Mesh &Tools");
    mt->addAction(toolActs_[4]);

    // ---- Mesh Display ----
    QMenu* md = add("Mesh &Display");
    addAct(md, "&Reverse Normals", {}, [&s] { return M::reverseNormals(s); }, true);
    md->addSeparator();
    addAct(md, "Smooth Preview Off", {QKeySequence("1")}, [&s] { return M::setSmoothDisplay(s, 0); });
    addAct(md, "Smooth Preview: Cage + Smooth", {QKeySequence("2")}, [&s] { return M::setSmoothDisplay(s, 1); });
    addAct(md, "Smooth Preview: Smooth", {QKeySequence("3")}, [&s] { return M::setSmoothDisplay(s, 2); });
    addAct(md, "Increase Smooth Level", {QKeySequence(Qt::Key_PageUp)}, [&s] { return M::changeSmoothLevel(s, 1); });
    addAct(md, "Decrease Smooth Level", {QKeySequence(Qt::Key_PageDown)}, [&s] { return M::changeSmoothLevel(s, -1); });

    // ---- Help ----
    QMenu* help = add("Hel&p");
    addAct(help, "Hotkeys...", {}, [this] { showHotkeys(); return QString(); });
    addAct(help, "About Facet", {}, [this] {
        QMessageBox::about(this, "About Facet",
                           "<b>Facet</b> — a personal subdivision &amp; high-poly modeler with Maya-style interaction.<br>"
                           "Half-edge meshes, Catmull–Clark with semi-sharp creases, GPU picking.");
        return QString();
    });
}

void MainWindow::buildStatusLine()
{
    QToolBar* tb = addToolBar("Status Line");
    tb->setObjectName("statusLine");
    tb->setMovable(false);
    tb->addWidget(new QLabel("  Select by: "));
    for (int i = 0; i < 4; ++i) tb->addAction(modeActs_[i]);
    tb->addSeparator();
    tb->addAction(softAct_);
    tb->addSeparator();
    auto* undoBtn = tb->addAction("Undo", [this] { scene_->undo.undo(); });
    auto* redoBtn = tb->addAction("Redo", [this] { scene_->undo.redo(); });
    connect(&scene_->undo, &QUndoStack::canUndoChanged, undoBtn, &QAction::setEnabled);
    connect(&scene_->undo, &QUndoStack::canRedoChanged, redoBtn, &QAction::setEnabled);
    undoBtn->setEnabled(false);
    redoBtn->setEnabled(false);
    addToolBarBreak();
}

void MainWindow::buildShelf()
{
    QToolBar* holder = addToolBar("Shelf");
    holder->setObjectName("shelf");
    holder->setMovable(false);
    auto* tabs = new QTabWidget;
    tabs->setDocumentMode(true);
    tabs->setMaximumHeight(64);
    Scene& s = *scene_;
    auto shelf = [&](const QString& name, std::vector<std::tuple<QString, QString, std::function<QString()>>> items) {
        auto* bar = new QToolBar;
        bar->setToolButtonStyle(Qt::ToolButtonTextOnly);
        for (auto& [label, tip, fn] : items) {
            if (label == "|") { bar->addSeparator(); continue; }
            QAction* a = bar->addAction(label);
            a->setToolTip(tip);
            auto f = fn;
            QString l = label;
            connect(a, &QAction::triggered, this, [this, f, l] {
                report(f());
                lastRepeatable_ = f;
                lastRepeatableName_ = l;
            });
        }
        tabs->addTab(bar, name);
    };
    shelf("Poly Modeling", {
        {"Sphere", "Polygon sphere", [&s] { return M::createPrimitive(s, "polySphere"); }},
        {"Cube", "Polygon cube", [&s] { return M::createPrimitive(s, "polyCube"); }},
        {"Cylinder", "Polygon cylinder", [&s] { return M::createPrimitive(s, "polyCylinder"); }},
        {"Cone", "Polygon cone", [&s] { return M::createPrimitive(s, "polyCone"); }},
        {"Plane", "Polygon plane", [&s] { return M::createPrimitive(s, "polyPlane"); }},
        {"Torus", "Polygon torus", [&s] { return M::createPrimitive(s, "polyTorus"); }},
        {"|", "", nullptr},
        {"Combine", "Combine selected meshes", [&s] { return M::combine(s); }},
        {"Separate", "Separate shells", [&s] { return M::separate(s); }},
        {"Mirror X", "Mirror across local X", [&s] { return M::mirror(s, 0); }},
        {"Smooth", "Bake one Catmull-Clark level", [&s] { return M::smooth(s, 1); }},
        {"|", "", nullptr},
        {"Extrude", "Extrude faces (Ctrl+E)", [this] { return M::extrude(*scene_, keepFacesTogether_); }},
        {"Insert Loop", "Insert Edge Loop tool", [this] { toolActs_[4]->trigger(); return QString("Insert Edge Loop: click (and drag) on an edge"); }},
        {"Merge", "Merge vertices by distance", [this] { return M::mergeVertices(*scene_, mergeThreshold_); }},
        {"Merge Center", "Merge to center", [&s] { return M::mergeToCenter(s); }},
        {"Fill Hole", "Fill holes", [&s] { return M::fillHole(s); }},
        {"|", "", nullptr},
        {"Crease", "Full crease on selected edges", [&s] { return M::setCrease(s, 10.0f); }},
        {"Uncrease", "Remove crease", [&s] { return M::setCrease(s, 0.0f); }},
        {"Triangulate", "Triangulate", [&s] { return M::triangulate(s); }},
    });
    shelf("Subdiv", {
        {"Preview Off (1)", "", [&s] { return M::setSmoothDisplay(s, 0); }},
        {"Cage+Smooth (2)", "", [&s] { return M::setSmoothDisplay(s, 1); }},
        {"Smooth (3)", "", [&s] { return M::setSmoothDisplay(s, 2); }},
        {"Level +", "Page Up", [&s] { return M::changeSmoothLevel(s, 1); }},
        {"Level -", "Page Down", [&s] { return M::changeSmoothLevel(s, -1); }},
        {"|", "", nullptr},
        {"Crease 1", "", [&s] { return M::setCrease(s, 1.0f); }},
        {"Crease 2", "", [&s] { return M::setCrease(s, 2.0f); }},
        {"Crease Full", "", [&s] { return M::setCrease(s, 10.0f); }},
        {"|", "", nullptr},
        {"Bake Smooth", "Mesh > Smooth", [&s] { return M::smooth(s, 1); }},
    });
    holder->addWidget(tabs);
}

void MainWindow::buildDocks()
{
    // Tool box (left, vertical) like Maya's.
    QToolBar* box = new QToolBar("Tool Box");
    box->setObjectName("toolBox");
    box->setOrientation(Qt::Vertical);
    box->setToolButtonStyle(Qt::ToolButtonTextOnly);
    const char* short_[] = {"Select", "Move", "Rotate", "Scale", "Loop"};
    for (int i = 0; i < 5; ++i) {
        QToolButton* b = new QToolButton;
        b->setDefaultAction(toolActs_[i]);
        b->setText(short_[i]);
        b->setMinimumWidth(56);
        box->addWidget(b);
    }
    addToolBar(Qt::LeftToolBarArea, box);

    auto* outDock = new QDockWidget("Outliner", this);
    outDock->setObjectName("outliner");
    outDock->setWidget(new Outliner(scene_));
    addDockWidget(Qt::LeftDockWidgetArea, outDock);
    auto* cbDock = new QDockWidget("Channel Box", this);
    cbDock->setObjectName("channelBox");
    cbDock->setWidget(new ChannelBox(scene_));
    cbDock->setMinimumWidth(250);
    addDockWidget(Qt::RightDockWidgetArea, cbDock);
    for (QMenu* m : findChildren<QMenu*>("windowsMenu")) {
        m->addSeparator();
        m->addAction(outDock->toggleViewAction());
        m->addAction(cbDock->toggleViewAction());
    }
}

void MainWindow::showMarkingMenu(int kind, QPoint g, uint32_t under)
{
    using D = MarkingMenu::Dir;
    Scene& s = *scene_;
    std::vector<MarkingMenu::Item> items;
    auto run = [this](std::function<QString()> f) { return [this, f] { report(f()); }; };
    auto modeItem = [&](const QString& label, D d, SelectMode m) {
        items.push_back({label, d, run([this, m, under] {
                             if (under && !scene_->isSelected(under)) scene_->selectObjects({under}, SelectOp::Replace);
                             return M::setComponentMode(*scene_, m);
                         }),
                         true, s.mode() == m});
    };
    QString title;
    if (kind == 0) {
        title = under ? (s.find(under) ? s.find(under)->st.name : QString()) : QString("Scene");
        if (under) {
            modeItem("Vertex", D::N, SelectMode::Vertex);
            modeItem("Edge", D::W, SelectMode::Edge);
            modeItem("Face", D::S, SelectMode::Face);
            modeItem("Object Mode", D::E, SelectMode::Object);
            items.push_back({"Select", D::NE, run([this, under] { scene_->setMode(SelectMode::Object); scene_->selectObjects({under}, SelectOp::Replace); return QString(); })});
            items.push_back({"Smooth Preview", D::SE, run([this, under] {
                                 SceneObject* o = scene_->find(under);
                                 if (o && !scene_->isSelected(under)) scene_->selectObjects({under}, SelectOp::Replace);
                                 return M::setSmoothDisplay(*scene_, o && o->st.smoothDisplay ? 0 : 2);
                             })});
        } else {
            modeItem("Object Mode", D::E, SelectMode::Object);
            items.push_back({"Select All", D::N, run([&s] { return M::selectAll(s); })});
            items.push_back({"Frame All", D::S, run([this] { viewport_->frameAll(); return QString(); })});
            items.push_back({"Four View", D::W, run([this] { viewport_->toggleQuadView(); return QString(); })});
        }
        items.push_back({"Grow Selection", D::List, run([&s] { return M::growSelection(s); })});
        items.push_back({"Shrink Selection", D::List, run([&s] { return M::shrinkSelection(s); })});
        items.push_back({"Edge Loop", D::List, run([&s] { return M::selectEdgeLoop(s); }), s.mode() == SelectMode::Edge});
        items.push_back({"Edge Ring", D::List, run([&s] { return M::selectEdgeRing(s); }), s.mode() == SelectMode::Edge});
    } else if (kind == 1) {
        switch (s.mode()) {
        case SelectMode::Object:
            title = "Object tools";
            items = {{"Smooth", D::N, run([&s] { return M::smooth(s, 1); })},
                     {"Combine", D::W, run([&s] { return M::combine(s); })},
                     {"Separate", D::E, run([&s] { return M::separate(s); })},
                     {"Mirror X", D::S, run([&s] { return M::mirror(s, 0); })},
                     {"Fill Hole", D::NE, run([&s] { return M::fillHole(s); })},
                     {"Insert Edge Loop", D::NW, run([this] { toolActs_[4]->trigger(); return QString(); })},
                     {"Freeze Transformations", D::List, run([&s] { return M::freezeTransforms(s); })},
                     {"Center Pivot", D::List, run([&s] { return M::centerPivot(s); })},
                     {"Delete History", D::List, run([&s] { return M::deleteHistory(s); })},
                     {"Triangulate", D::List, run([&s] { return M::triangulate(s); })}};
            break;
        case SelectMode::Face:
            title = "Face tools";
            items = {{"Extrude Face", D::N, run([this] { return M::extrude(*scene_, keepFacesTogether_); })},
                     {"Delete Face", D::W, run([&s] { return M::deleteSelection(s); })},
                     {"Insert Edge Loop", D::S, run([this] { toolActs_[4]->trigger(); return QString(); })},
                     {"Collapse", D::E, run([&s] { return M::collapse(s); })},
                     {"Merge", D::NE, run([this] { return M::mergeVertices(*scene_, mergeThreshold_); })},
                     {"Reverse Normals", D::List, run([&s] { return M::reverseNormals(s); })},
                     {"Triangulate", D::List, run([&s] { return M::triangulate(s); })},
                     {"Grow Selection", D::List, run([&s] { return M::growSelection(s); })}};
            break;
        case SelectMode::Edge:
            title = "Edge tools";
            items = {{"Insert Edge Loop", D::N, run([this] { toolActs_[4]->trigger(); return QString(); })},
                     {"Delete Edge", D::W, run([&s] { return M::deleteSelection(s); })},
                     {"Full Crease", D::E, run([&s] { return M::setCrease(s, 10.0f); })},
                     {"Collapse", D::S, run([&s] { return M::collapse(s); })},
                     {"Crease 1", D::NE, run([&s] { return M::setCrease(s, 1.0f); })},
                     {"Uncrease", D::SE, run([&s] { return M::setCrease(s, 0.0f); })},
                     {"Select Loop", D::NW, run([&s] { return M::selectEdgeLoop(s); })},
                     {"Select Ring", D::SW, run([&s] { return M::selectEdgeRing(s); })},
                     {"Fill Hole", D::List, run([&s] { return M::fillHole(s); })},
                     {"Merge", D::List, run([this] { return M::mergeVertices(*scene_, mergeThreshold_); })}};
            break;
        case SelectMode::Vertex:
            title = "Vertex tools";
            items = {{"Merge", D::N, run([this] { return M::mergeVertices(*scene_, mergeThreshold_); })},
                     {"Delete Vertex", D::W, run([&s] { return M::deleteSelection(s); })},
                     {"Merge to Center", D::E, run([&s] { return M::mergeToCenter(s); })},
                     {"Insert Edge Loop", D::S, run([this] { toolActs_[4]->trigger(); return QString(); })},
                     {"Soft Select", D::NE, run([&s] { s.softSelect = !s.softSelect; s.notifySelectionChanged(); return QString(s.softSelect ? "Soft Select on" : "Soft Select off"); })},
                     {"Grow Selection", D::List, run([&s] { return M::growSelection(s); })}};
            break;
        }
    } else {
        title = "Convert selection";
        items = {{"To Vertices", D::N, run([&s] { return M::convertSelection(s, SelectMode::Vertex); })},
                 {"To Edges", D::W, run([&s] { return M::convertSelection(s, SelectMode::Edge); })},
                 {"To Faces", D::E, run([&s] { return M::convertSelection(s, SelectMode::Face); })},
                 {"Object Mode", D::S, run([&s] { return M::setComponentMode(s, SelectMode::Object); })}};
    }
    marking_->popup(g, std::move(items), title);
}

void MainWindow::updateTitle()
{
    QString name = scene_->filePath.isEmpty() ? "untitled" : QFileInfo(scene_->filePath).fileName();
    setWindowTitle(QString("Facet — %1%2").arg(name, scene_->undo.isClean() ? "" : " *"));
}

void MainWindow::updateHelpLine()
{
    static const char* tools[] = {"Select Tool: click or drag to select. Shift toggles, Ctrl deselects, Ctrl+Shift adds.",
                                  "Move Tool: drag a handle; MMB-drag anywhere uses the last handle. Hold X to snap to grid.",
                                  "Rotate Tool: drag a ring; hold J to snap to 15°.",
                                  "Scale Tool: drag an axis or the centre for uniform scale.",
                                  "Insert Edge Loop: hover an edge to preview, click (or drag to slide) to insert."};
    statusBar()->showMessage(tools[int(scene_->tool())]);
}

bool MainWindow::maybeSave()
{
    if (scene_->undo.isClean()) return true;
    auto r = QMessageBox::question(this, "Facet", "Save changes to the current scene?", QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return saveScene(false);
    return true;
}

void MainWindow::newScene()
{
    if (!maybeSave()) return;
    scene_->clear();
    updateTitle();
}

void MainWindow::openScene()
{
    if (!maybeSave()) return;
    QString p = QFileDialog::getOpenFileName(this, "Open Scene", {}, "Facet scene (*.facet)");
    if (p.isEmpty()) return;
    QString err;
    if (!scene_->load(p, &err)) QMessageBox::warning(this, "Open failed", err);
    viewport_->frameAll();
    updateTitle();
}

bool MainWindow::saveScene(bool saveAs)
{
    QString p = scene_->filePath;
    if (saveAs || p.isEmpty()) {
        p = QFileDialog::getSaveFileName(this, "Save Scene", p.isEmpty() ? "untitled.facet" : p, "Facet scene (*.facet)");
        if (p.isEmpty()) return false;
        if (!p.endsWith(".facet")) p += ".facet";
    }
    QString err;
    if (!scene_->save(p, &err)) {
        QMessageBox::warning(this, "Save failed", err);
        return false;
    }
    scene_->filePath = p;
    scene_->undo.setClean();
    updateTitle();
    report("Saved " + p);
    return true;
}

void MainWindow::showHotkeys()
{
    auto* d = new QDialog(this);
    d->setWindowTitle("Facet Hotkeys");
    d->resize(560, 640);
    auto* tb = new QTextBrowser(d);
    tb->setHtml(R"(<h3>Camera</h3>
<table cellpadding=3>
<tr><td><b>Alt + LMB</b></td><td>Tumble (persp)</td></tr>
<tr><td><b>Alt + MMB</b></td><td>Track</td></tr>
<tr><td><b>Alt + RMB</b> / wheel</td><td>Dolly</td></tr>
<tr><td><b>F</b> / <b>A</b></td><td>Frame selected / all (Shift = all views)</td></tr>
<tr><td><b>Space</b> tap / hold</td><td>Four-view toggle / Hotbox</td></tr>
</table>
<h3>Selection</h3>
<table cellpadding=3>
<tr><td><b>F8</b></td><td>Object / component toggle</td></tr>
<tr><td><b>F9 F10 F11</b></td><td>Vertex, Edge, Face</td></tr>
<tr><td><b>Ctrl+F9/F10/F11</b></td><td>Convert selection</td></tr>
<tr><td><b>Shift / Ctrl / Ctrl+Shift</b></td><td>Toggle / deselect / add</td></tr>
<tr><td><b>Double-click edge</b></td><td>Edge loop</td></tr>
<tr><td><b>&gt; / &lt;</b></td><td>Grow / shrink</td></tr>
<tr><td><b>B</b> tap / hold + drag</td><td>Soft selection / falloff radius</td></tr>
</table>
<h3>Tools</h3>
<table cellpadding=3>
<tr><td><b>Q W E R</b></td><td>Select, Move, Rotate, Scale</td></tr>
<tr><td><b>MMB drag</b></td><td>Drag the last manipulator handle from anywhere</td></tr>
<tr><td><b>X</b> / <b>J</b> held</td><td>Grid snap (move) / 15° snap (rotate)</td></tr>
<tr><td><b>RMB</b></td><td>Component marking menu</td></tr>
<tr><td><b>Shift + RMB</b></td><td>Modeling tools marking menu</td></tr>
<tr><td><b>Ctrl + RMB</b></td><td>Convert-selection marking menu</td></tr>
<tr><td><b>Ctrl+E</b></td><td>Extrude</td></tr>
<tr><td><b>G</b></td><td>Repeat last command</td></tr>
<tr><td><b>Ctrl+D</b></td><td>Duplicate</td></tr>
<tr><td><b>Delete</b></td><td>Delete objects / faces / dissolve edges &amp; vertices</td></tr>
<tr><td><b>Z / Shift+Z</b> (Ctrl+Z / Ctrl+Shift+Z)</td><td>Undo / redo</td></tr>
</table>
<h3>Display</h3>
<table cellpadding=3>
<tr><td><b>1 / 2 / 3</b></td><td>Smooth preview off / cage+smooth / smooth</td></tr>
<tr><td><b>Page Up / Down</b></td><td>Smooth level</td></tr>
<tr><td><b>4 / 5</b></td><td>Wireframe / shaded</td></tr>
<tr><td><b>Alt+X</b></td><td>X-Ray</td></tr>
<tr><td><b>Alt+B</b></td><td>Cycle background</td></tr>
<tr><td><b>Ctrl+H / Shift+H</b></td><td>Hide selected / show all</td></tr>
</table>)");
    auto* l = new QVBoxLayout(d);
    l->addWidget(tb);
    d->show();
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (maybeSave()) e->accept();
    else e->ignore();
}

bool MainWindow::eventFilter(QObject* o, QEvent* e)
{
    if (o == central_ && e->type() == QEvent::Resize) {
        if (marking_->isVisible()) marking_->setGeometry(central_->rect());
        if (hotbox_->isVisible()) hotbox_->setGeometry(central_->rect());
    }
    return QMainWindow::eventFilter(o, e);
}

} // namespace facet
