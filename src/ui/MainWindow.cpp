#include "ui/MainWindow.h"
#include "scene/Modeling.h"
#include "ui/ChannelBox.h"
#include "ui/MarkingMenu.h"
#include "ui/Outliner.h"
#include "ui/UVEditor.h"
#include "ui/PluginManager.h"
#include "ui/Hotkeys.h"
#include "viewport/Viewport.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QSpinBox>
#include <QCloseEvent>
#include <QDialog>
#include <QPushButton>
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
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace facet {

namespace M = modeling;

MainWindow::MainWindow()
{
    scene_ = new Scene(this);
    hotkeys_ = new HotkeyManager(this);
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
    plugins_ = new PluginManager(scene_, this);
    plugins_->loadAll();
    buildPluginMenus();
    hotkeys_->load(); // user overrides from the Hotkey Editor
    connect(hotbox_, &Hotbox::centerClicked, this, [this](QPoint g) {
        using D = MarkingMenu::Dir;
        auto view = [this](ViewKind k) { return [this, k] { viewport_->setSingleView(k); }; };
        marking_->popup(g,
                        {{"Top", D::N, view(ViewKind::Top)},
                         {"Front", D::S, view(ViewKind::Front)},
                         {"Side", D::E, view(ViewKind::Side)},
                         {"Persp", D::W, view(ViewKind::Persp)},
                         {"Four View", D::NE, [this] { viewport_->setQuadView(true); }},
                         {"Frame All", D::SE, [this] { viewport_->frameAll(); }},
                         {"Frame Selected", D::SW, [this] { viewport_->frameSelected(); }}},
                        "Views");
        marking_->release(g); // stays open as a click menu
    });
    connect(scene_, &Scene::toolChanged, this, [this] {
        Tool t = scene_->tool();
        if (t != Tool::Select && t != Tool::Move && t != Tool::Rotate && t != Tool::Scale) lastSpecialTool_ = t;
    });

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
    if (hotkeys_) hotkeys_->registerAction(a, menu ? menu->title() : QString("Tools"));
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
    addAct(file, "&Import (FBX / OBJ)...", {QKeySequence("Ctrl+I")}, [this] {
        QString p = QFileDialog::getOpenFileName(this, "Import", {}, "Meshes (*.fbx *.obj);;FBX (*.fbx);;Wavefront OBJ (*.obj)");
        return p.isEmpty() ? QString() : M::importFile(*scene_, p);
    });
    addAct(file, "Export Selection to &FBX...", {}, [this] {
        QString p = QFileDialog::getSaveFileName(this, "Export Selection", "selection.fbx", "FBX (*.fbx)");
        return p.isEmpty() ? QString() : M::exportFbx(*scene_, p, true);
    });
    addAct(file, "Export All to F&BX...", {}, [this] {
        QString p = QFileDialog::getSaveFileName(this, "Export All", "scene.fbx", "FBX (*.fbx)");
        return p.isEmpty() ? QString() : M::exportFbx(*scene_, p, false);
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
    hotkeys_->registerAction(undo, "Edit");
    hotkeys_->registerAction(redo, "Edit");
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
    const char* toolNames[] = {"&Select Tool", "&Move Tool", "&Rotate Tool", "S&cale Tool", "Insert &Edge Loop Tool", "&Multi-Cut Tool",
                               "&Target Weld Tool", "&Quad Draw Tool", "Scu&lpt Tool"};
    // Q W E R like Maya; the newer tools get Ctrl+Shift/Ctrl+Alt bindings (rebind in the Hotkey Editor).
    const char* toolKeys[] = {"Q", "W", "E", "R", "Ctrl+Shift+L", "Ctrl+Shift+X", "Ctrl+Shift+W", "Ctrl+Shift+Q", "Ctrl+Alt+S"};
    for (int i = 0; i < 9; ++i) {
        Tool t = Tool(i);
        QList<QKeySequence> keys;
        if (toolKeys[i][0]) keys << QKeySequence(toolKeys[i]);
        toolActs_[i] = addAct(i < 4 ? mod : nullptr, toolNames[i], keys, [this, t] {
            if ((t == Tool::InsertEdgeLoop || t == Tool::MultiCut) && scene_->mode() == SelectMode::Object && scene_->lead()) scene_->setMode(SelectMode::Edge);
            if (t == Tool::TargetWeld && scene_->mode() == SelectMode::Object && scene_->lead()) scene_->setMode(SelectMode::Vertex);
            if (t == Tool::Sculpt && scene_->mode() != SelectMode::Object) scene_->setMode(SelectMode::Object);
            if (t == Tool::QuadDraw) {
                if (scene_->mode() != SelectMode::Object) scene_->setMode(SelectMode::Object);
                if (!scene_->liveObject()) report("Quad Draw: make the high-poly mesh live first (Mesh Tools > Make Live)");
            }
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
    addAct(mod, "Last Tool Used", {QKeySequence("Y")}, [this] {
        toolActs_[int(lastSpecialTool_)]->trigger();
        return QString("Last tool: %1").arg(HotkeyManager::cleanName(toolActs_[int(lastSpecialTool_)]->text()));
    });
    addAct(mod, "Increase Manipulator Size", {QKeySequence("+"), QKeySequence("=")}, [this] { viewport_->scaleManipulator(1.25f); return QString(); });
    addAct(mod, "Decrease Manipulator Size", {QKeySequence("-")}, [this] { viewport_->scaleManipulator(0.8f); return QString(); });
    mod->addSeparator();
    addAct(mod, "&Freeze Transformations", {}, [&s] { return M::freezeTransforms(s); }, true);
    addAct(mod, "&Reset Transformations", {}, [&s] { return M::resetTransforms(s); }, true);
    addAct(mod, "&Center Pivot", {}, [&s] { return M::centerPivot(s); }, true);

    // ---- Display ----
    QMenu* disp = add("&Display");
    auto* shadeGroup = new QActionGroup(this);
    QAction* wire = addAct(disp, "&Wireframe", {QKeySequence("4")}, [this] { viewport_->options.shading = ShadingMode::Wireframe; viewport_->update(); return QString(); });
    QAction* shaded = addAct(disp, "&Smooth Shade All", {QKeySequence("5")}, [this] {
        viewport_->options.shading = ShadingMode::Shaded;
        if (checkerAct_) checkerAct_->setChecked(false);
        viewport_->update();
        return QString();
    });
    checkerAct_ = addAct(disp, "UV Checker (Textured)", {QKeySequence("6")}, nullptr);
    checkerAct_->setCheckable(true);
    connect(checkerAct_, &QAction::toggled, this, [this](bool on) {
        viewport_->options.textured = on;
        if (on) viewport_->options.shading = ShadingMode::Shaded;
        viewport_->update();
    });
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
    addAct(disp, "&Isolate Select", {QKeySequence("Ctrl+1")}, [&s] { return M::toggleIsolate(s); });
    addAct(disp, "&Hide Selection", {QKeySequence("Ctrl+H")}, [&s] { return M::hideSelected(s); });
    addAct(disp, "&Show All", {QKeySequence("Shift+H")}, [&s] { return M::showAll(s); });

    // ---- Windows ----
    QMenu* windows = add("&Windows");
    windows->setObjectName("windowsMenu");
    QMenu* prefs = windows->addMenu("Settings/Preferences");
    addAct(prefs, "Hotkey Editor...", {QKeySequence("F1")}, [this] { showHotkeyEditor(); return QString(); });
    addAct(windows, "Hotkey Reference (held keys)", {}, [this] { showHotkeys(); return QString(); });

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
    addAct(em, "&Bevel", {QKeySequence("Ctrl+B")}, [this] { return M::bevel(*scene_, bevelOffset_, bevelSegments_, bevelRound_); }, true);
    addAct(em, "Bevel Options...", {}, [this] { bevelOptions(); return QString(); });
    addAct(em, "B&ridge", {QKeySequence("Ctrl+Alt+B")}, [this] { return M::bridge(*scene_, bridgeDivisions_); }, true);
    addAct(em, "Bridge Divisions...", {}, [this] {
        bool ok = false;
        int d = QInputDialog::getInt(this, "Bridge", "Divisions:", bridgeDivisions_, 0, 100, 1, &ok);
        if (ok) bridgeDivisions_ = d;
        return QString();
    });
    em->addSeparator();
    addAct(em, "&Merge", {QKeySequence("Ctrl+Alt+M")}, [this] { return M::mergeVertices(*scene_, mergeThreshold_); }, true);
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
    for (int i = 4; i < 8; ++i) mt->addAction(toolActs_[i]);
    mt->addSeparator();
    addAct(mt, "Make &Live (toggle)", {QKeySequence("Ctrl+Alt+L")}, [&s] { return M::toggleLive(s); });
    addAct(mt, "&Conform to Live Surface", {QKeySequence("Ctrl+Alt+C")}, [&s] { return M::conform(s); }, true);
    mt->addSeparator();
    QMenu* sym = mt->addMenu("&Symmetry");
    auto* symGroup = new QActionGroup(this);
    const char* symNames[] = {"Off", "Object X", "Object Y", "Object Z"};
    for (int i = 0; i < 4; ++i) {
        QAction* a = addAct(sym, symNames[i], {}, [this, i] {
            scene_->symmetry = i - 1;
            if (auto* c = findChild<QComboBox*>("symmetryCombo")) c->setCurrentIndex(i);
            return QString("Symmetry: %1").arg(i == 0 ? "off" : QString("object %1").arg(QChar("XYZ"[i - 1])));
        });
        a->setCheckable(true);
        a->setChecked(i == 0);
        symGroup->addAction(a);
    }

    // ---- Mesh Display ----
    QMenu* md = add("Mesh &Display");
    addAct(md, "&Reverse Normals", {}, [&s] { return M::reverseNormals(s); }, true);
    md->addSeparator();
    addAct(md, "&Soften Edge", {QKeySequence("Ctrl+Alt+F")}, [&s] { return M::softenEdges(s); }, true);
    addAct(md, "&Harden Edge", {QKeySequence("Ctrl+Alt+H")}, [&s] { return M::hardenEdges(s); }, true);
    addAct(md, "Set Normal &Angle...", {}, [this] {
        bool ok = false;
        double a = QInputDialog::getDouble(this, "Set Normal Angle", "Edges sharper than this angle become hard:", normalAngle_, 0, 180, 1, &ok);
        if (!ok) return QString();
        normalAngle_ = float(a);
        return M::setNormalAngle(*scene_, normalAngle_);
    }, true);
    addAct(md, "&Weighted Normals...", {}, [this] {
        bool ok = false;
        double p = QInputDialog::getDouble(this, "Weighted Normals", "Strength (0 = off; 2-4 typical for hard-surface):", weightPower_, 0, 10, 1, &ok);
        if (!ok) return QString();
        weightPower_ = float(p);
        return M::setWeightedNormals(*scene_, weightPower_);
    }, true);
    addAct(md, "Clear Weighted Normals", {}, [&s] { return M::setWeightedNormals(s, 0.0f); });
    QAction* showHard = addAct(md, "Show Hard Edges", {}, nullptr);
    showHard->setCheckable(true);
    connect(showHard, &QAction::toggled, this, [this](bool on) { viewport_->options.showHardEdges = on; viewport_->update(); });
    md->addSeparator();
    addAct(md, "Smooth Preview Off", {QKeySequence("1")}, [&s] { return M::setSmoothDisplay(s, 0); });
    addAct(md, "Smooth Preview: Cage + Smooth", {QKeySequence("2")}, [&s] { return M::setSmoothDisplay(s, 1); });
    addAct(md, "Smooth Preview: Smooth", {QKeySequence("3")}, [&s] { return M::setSmoothDisplay(s, 2); });
    addAct(md, "Increase Smooth Level", {QKeySequence(Qt::Key_PageUp)}, [&s] { return M::changeSmoothLevel(s, 1); });
    addAct(md, "Decrease Smooth Level", {QKeySequence(Qt::Key_PageDown)}, [&s] { return M::changeSmoothLevel(s, -1); });

    // ---- Sculpt ----
    QMenu* sm = add("S&culpt");
    sm->addAction(toolActs_[8]);
    sm->addSeparator();
    brushGroup_ = new QActionGroup(this);
    const sculpt::Brush brushes[] = {sculpt::Brush::Draw, sculpt::Brush::Smooth, sculpt::Brush::Grab, sculpt::Brush::Inflate,
                                     sculpt::Brush::Flatten, sculpt::Brush::Pinch, sculpt::Brush::Crease, sculpt::Brush::Mask};
    for (sculpt::Brush b : brushes) {
        QAction* a = addAct(sm, QString("%1 Brush").arg(sculpt::brushName(b)), {}, [this, b] {
            scene_->sculpt.brush = b;
            if (scene_->tool() != Tool::Sculpt) toolActs_[8]->trigger();
            viewport_->update();
            return QString("Sculpt brush: %1").arg(sculpt::brushName(b));
        });
        a->setCheckable(true);
        a->setChecked(b == sculpt::Brush::Draw);
        brushGroup_->addAction(a);
    }
    sm->addSeparator();
    addAct(sm, "Brush Settings...", {}, [this] { sculptSettings(); return QString(); });
    addAct(sm, "Invert Mask", {}, [&s] { return M::invertMask(s); });
    addAct(sm, "Clear Mask", {}, [&s] { return M::clearMask(s); });
    sm->addSeparator();
    addAct(sm, "Subdivide for Sculpting (bake 1 level)", {}, [&s] { return M::smooth(s, 1); }, true);

    // ---- UV ----
    QMenu* uvm = add("&UV");
    addAct(uvm, "&Planar (best plane)", {}, [&s] { return M::projectUVs(s, M::Projection::PlanarBest); }, true);
    addAct(uvm, "Planar X", {}, [&s] { return M::projectUVs(s, M::Projection::PlanarX); }, true);
    addAct(uvm, "Planar Y", {}, [&s] { return M::projectUVs(s, M::Projection::PlanarY); }, true);
    addAct(uvm, "Planar Z", {}, [&s] { return M::projectUVs(s, M::Projection::PlanarZ); }, true);
    addAct(uvm, "&Cylindrical", {}, [&s] { return M::projectUVs(s, M::Projection::Cylindrical); }, true);
    addAct(uvm, "&Spherical", {}, [&s] { return M::projectUVs(s, M::Projection::Spherical); }, true);
    addAct(uvm, "&Automatic", {}, [&s] { return M::projectUVs(s, M::Projection::Automatic); }, true);
    uvm->addSeparator();
    addAct(uvm, "Cu&t UV Edges", {}, [&s] { return M::cutUVs(s); }, true);
    addAct(uvm, "Move and S&ew", {}, [&s] { return M::sewUVs(s); }, true);
    addAct(uvm, "&Unfold", {}, [&s] { return M::unfoldUVs(s); }, true);
    addAct(uvm, "&Layout", {}, [&s] { return M::layoutUVs(s); }, true);
    addAct(uvm, "&Normalize", {}, [&s] { return M::normalizeUVs(s); }, true);
    addAct(uvm, "Flip U", {}, [&s] { return M::flipUVs(s, true); }, true);
    addAct(uvm, "Flip V", {}, [&s] { return M::flipUVs(s, false); }, true);
    addAct(uvm, "Rotate 90°", {}, [&s] { return M::rotateUVs(s, 90.0f); }, true);
    uvm->addSeparator();
    addAct(uvm, "UV &Editor", {QKeySequence("Ctrl+Shift+U")}, [this] { openUVEditor(); return QString(); });

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
    tb->addWidget(new QLabel(" Symmetry: "));
    auto* symCombo = new QComboBox;
    symCombo->setObjectName("symmetryCombo");
    symCombo->addItems({"Off", "Object X", "Object Y", "Object Z"});
    symCombo->setFocusPolicy(Qt::NoFocus);
    connect(symCombo, &QComboBox::currentIndexChanged, this, [this](int i) { scene_->symmetry = i - 1; });
    tb->addWidget(symCombo);
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
    shelf("Tools", {
        {"Bevel", "Bevel selected edges (Ctrl+B)", [this] { return M::bevel(*scene_, bevelOffset_, bevelSegments_, bevelRound_); }},
        {"Bevel 3 seg", "Rounded 3-segment bevel", [this] { return M::bevel(*scene_, bevelOffset_, 3, true); }},
        {"Bridge", "Bridge two border loops / face regions", [this] { return M::bridge(*scene_, bridgeDivisions_); }},
        {"Multi-Cut", "Multi-Cut tool", [this] { toolActs_[5]->trigger(); return QString(); }},
        {"Target Weld", "Drag a vertex onto another", [this] { toolActs_[6]->trigger(); return QString(); }},
        {"Insert Loop", "Insert Edge Loop tool", [this] { toolActs_[4]->trigger(); return QString(); }},
        {"Harden", "Harden selected edges (whole object in object mode)", [&s] { return M::hardenEdges(s); }},
        {"Soften", "Soften selected edges", [&s] { return M::softenEdges(s); }},
        {"Angle 30", "Hard above 30°, soft below", [&s] { return M::setNormalAngle(s, 30.0f); }},
        {"|", "", nullptr},
        {"Sym X", "Toggle object-X symmetry", [this] {
            auto* c = findChild<QComboBox*>("symmetryCombo");
            if (c) c->setCurrentIndex(scene_->symmetry == 0 ? 0 : 1);
            return QString(scene_->symmetry == 0 ? "Symmetry: object X" : "Symmetry: off");
        }},
    });
    shelf("Retopo", {
        {"Make Live", "Make the selected (high-poly) mesh live", [&s] { return M::toggleLive(s); }},
        {"Quad Draw", "Quad Draw tool", [this] { toolActs_[7]->trigger(); return QString(); }},
        {"Conform", "Snap the selection to the live surface", [&s] { return M::conform(s); }},
        {"Relax+Conform", "Smooth then conform", [&s] { return M::conform(s); }},
    });
    shelf("UV", {
        {"Planar", "", [&s] { return M::projectUVs(s, M::Projection::PlanarBest); }},
        {"Cylindrical", "", [&s] { return M::projectUVs(s, M::Projection::Cylindrical); }},
        {"Spherical", "", [&s] { return M::projectUVs(s, M::Projection::Spherical); }},
        {"Automatic", "", [&s] { return M::projectUVs(s, M::Projection::Automatic); }},
        {"|", "", nullptr},
        {"Cut", "Cut UV edges", [&s] { return M::cutUVs(s); }},
        {"Sew", "Move and sew", [&s] { return M::sewUVs(s); }},
        {"Unfold", "LSCM unfold", [&s] { return M::unfoldUVs(s); }},
        {"Layout", "Pack shells", [&s] { return M::layoutUVs(s); }},
        {"|", "", nullptr},
        {"UV Editor", "Open the UV Editor", [this] { openUVEditor(); return QString(); }},
        {"Checker", "Toggle the viewport UV checker (6)", [this] { checkerAct_->toggle(); return QString(); }},
    });
    {
        std::vector<std::tuple<QString, QString, std::function<QString()>>> items;
        const sculpt::Brush brushes[] = {sculpt::Brush::Draw, sculpt::Brush::Smooth, sculpt::Brush::Grab, sculpt::Brush::Inflate,
                                         sculpt::Brush::Flatten, sculpt::Brush::Pinch, sculpt::Brush::Crease, sculpt::Brush::Mask};
        for (sculpt::Brush b : brushes)
            items.emplace_back(sculpt::brushName(b), "Sculpt brush", [this, b] {
                scene_->sculpt.brush = b;
                int i = int(b);
                if (brushGroup_ && i < brushGroup_->actions().size()) brushGroup_->actions()[i]->setChecked(true);
                if (scene_->tool() != Tool::Sculpt) toolActs_[8]->trigger();
                viewport_->update();
                return QString("Sculpt brush: %1").arg(sculpt::brushName(b));
            });
        items.emplace_back("|", "", nullptr);
        items.emplace_back("Invert Mask", "", [&s] { return M::invertMask(s); });
        items.emplace_back("Clear Mask", "", [&s] { return M::clearMask(s); });
        items.emplace_back("Subdivide", "Bake one subdivision level for more detail", [&s] { return M::smooth(s, 1); });
        items.emplace_back("Settings", "Brush radius / strength", [this] { sculptSettings(); return QString(); });
        shelf("Sculpt", items);
    }
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
    const char* short_[] = {"Select", "Move", "Rotate", "Scale", "Loop", "Cut", "Weld", "Quad", "Sculpt"};
    for (int i = 0; i < 9; ++i) {
        QToolButton* b = new QToolButton;
        b->setDefaultAction(toolActs_[i]);
        b->setText(short_[i]);
        b->setMinimumWidth(56);
        box->addWidget(b);
    }
    addToolBar(Qt::LeftToolBarArea, box);

    auto* outDock = new QDockWidget("Outliner", this);
    outDock->setObjectName("outliner");
    outliner_ = new Outliner(scene_);
    outliner_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(outliner_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        // Right-clicking an item selects it first (unless it is already part of the selection).
        if (QTreeWidgetItem* it = outliner_->itemAt(pos)) {
            uint32_t id = it->data(0, Qt::UserRole).toUInt();
            if (!scene_->isSelected(id)) scene_->selectObjects({id}, SelectOp::Replace);
        }
        QMenu* m = buildOutlinerMenu();
        m->setAttribute(Qt::WA_DeleteOnClose);
        m->popup(outliner_->viewport()->mapToGlobal(pos));
    });
    outDock->setWidget(outliner_);
    addDockWidget(Qt::LeftDockWidgetArea, outDock);
    auto* cbDock = new QDockWidget("Channel Box", this);
    cbDock->setObjectName("channelBox");
    cbDock->setWidget(new ChannelBox(scene_));
    cbDock->setMinimumWidth(250);
    addDockWidget(Qt::RightDockWidgetArea, cbDock);
    uvDock_ = new QDockWidget("UV Editor", this);
    uvDock_->setObjectName("uvEditor");
    uvEditor_ = new UVEditor(scene_, [this](const QString& m) { report(m); });
    uvDock_->setWidget(uvEditor_);
    addDockWidget(Qt::RightDockWidgetArea, uvDock_);
    uvDock_->setFloating(true); // its own window, like Maya's UV Editor (dock it by dragging)
    uvDock_->resize(820, 680);
    uvDock_->hide();
    connect(uvEditor_->checkerAction(), &QAction::toggled, checkerAct_, &QAction::setChecked);
    connect(checkerAct_, &QAction::toggled, uvEditor_->checkerAction(), &QAction::setChecked);
    for (QMenu* m : findChildren<QMenu*>("windowsMenu")) {
        m->addSeparator();
        m->addAction(outDock->toggleViewAction());
        m->addAction(cbDock->toggleViewAction());
        m->addAction(uvDock_->toggleViewAction());
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

    // ---- tool-specific menus (like Maya's per-tool marking menus) ----
    if (s.tool() == Tool::Sculpt && kind != 2) {
        auto brush = [&](sculpt::Brush b, D d) {
            items.push_back({sculpt::brushName(b), d, run([this, b] {
                                 scene_->sculpt.brush = b;
                                 if (brushGroup_ && int(b) < brushGroup_->actions().size()) brushGroup_->actions()[int(b)]->setChecked(true);
                                 viewport_->update();
                                 return QString("Sculpt brush: %1").arg(sculpt::brushName(b));
                             }),
                             true, s.sculpt.brush == b});
        };
        if (kind == 0) {
            title = "Sculpt brushes";
            brush(sculpt::Brush::Draw, D::N);
            brush(sculpt::Brush::Smooth, D::S);
            brush(sculpt::Brush::Grab, D::W);
            brush(sculpt::Brush::Inflate, D::E);
            brush(sculpt::Brush::Flatten, D::NE);
            brush(sculpt::Brush::Pinch, D::NW);
            brush(sculpt::Brush::Crease, D::SE);
            brush(sculpt::Brush::Mask, D::SW);
            items.push_back({"Invert Mask", D::List, run([&s] { return M::invertMask(s); })});
            items.push_back({"Clear Mask", D::List, run([&s] { return M::clearMask(s); })});
            items.push_back({"Brush Settings...", D::List, run([this] { sculptSettings(); return QString(); })});
        } else {
            title = "Sculpt options";
            items = {{"Symmetry X", D::N, run([this] {
                          auto* c = findChild<QComboBox*>("symmetryCombo");
                          if (c) c->setCurrentIndex(scene_->symmetry == 0 ? 0 : 1);
                          return QString(scene_->symmetry == 0 ? "Symmetry: object X" : "Symmetry: off");
                      }), true, s.symmetry == 0},
                     {"Subdivide", D::E, run([&s] { return M::smooth(s, 1); })},
                     {"Exit Sculpt", D::S, run([this] { toolActs_[0]->trigger(); return QString(); })},
                     {"Invert Mask", D::W, run([&s] { return M::invertMask(s); })},
                     {"Clear Mask", D::List, run([&s] { return M::clearMask(s); })},
                     {"Weighted Normals", D::List, run([this] { return M::setWeightedNormals(*scene_, weightPower_); })}};
        }
        marking_->popup(g, std::move(items), title);
        return;
    }
    if (s.tool() == Tool::QuadDraw && kind == 0) {
        title = "Retopology";
        items = {{s.liveId ? "Clear Live" : "Make Live", D::N, run([&s] { return M::toggleLive(s); })},
                 {"Conform", D::W, run([&s] { return M::conform(s); })},
                 {"Exit Quad Draw", D::S, run([this] { toolActs_[0]->trigger(); return QString(); })},
                 {"Smooth Preview", D::E, run([&s] { return M::setSmoothDisplay(s, s.lead() && s.lead()->st.smoothDisplay ? 0 : 1); })},
                 {"Insert Edge Loop", D::NE, run([this] { toolActs_[4]->trigger(); return QString(); })},
                 {"Target Weld", D::NW, run([this] { toolActs_[6]->trigger(); return QString(); })},
                 {"Automatic UVs", D::List, run([&s] { return M::projectUVs(s, M::Projection::Automatic); })}};
        marking_->popup(g, std::move(items), title);
        return;
    }
    if (kind == 1 && s.selection().empty()) {
        // Shift+RMB on nothing: create polygon primitives.
        title = "Create";
        auto prim = [&](const QString& label, const QString& type, D d) {
            items.push_back({label, d, run([&s, type] { return M::createPrimitive(s, type); })});
        };
        prim("Sphere", "polySphere", D::N);
        prim("Cube", "polyCube", D::NE);
        prim("Cylinder", "polyCylinder", D::E);
        prim("Cone", "polyCone", D::SE);
        prim("Plane", "polyPlane", D::S);
        prim("Torus", "polyTorus", D::SW);
        items.push_back({"Import...", D::List, run([this] {
                             QString p = QFileDialog::getOpenFileName(this, "Import", {}, "Meshes (*.fbx *.obj)");
                             return p.isEmpty() ? QString() : M::importFile(*scene_, p);
                         })});
        marking_->popup(g, std::move(items), title);
        return;
    }
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
                     {"Make Live", D::SE, run([&s] { return M::toggleLive(s); })},
                     {"Quad Draw", D::SW, run([this] { toolActs_[7]->trigger(); return QString(); })},
                     {"Automatic UVs", D::List, run([&s] { return M::projectUVs(s, M::Projection::Automatic); })},
                     {"Soften Edges", D::List, run([&s] { return M::softenEdges(s); })},
                     {"Normal Angle 30°", D::List, run([&s] { return M::setNormalAngle(s, 30.0f); })},
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
                     {"Bevel", D::NW, run([this] { return M::bevel(*scene_, bevelOffset_, bevelSegments_, bevelRound_); })},
                     {"Bridge", D::SW, run([this] { return M::bridge(*scene_, bridgeDivisions_); })},
                     {"Planar UVs", D::SE, run([&s] { return M::projectUVs(s, M::Projection::PlanarBest); })},
                     {"Reverse Normals", D::List, run([&s] { return M::reverseNormals(s); })},
                     {"Triangulate", D::List, run([&s] { return M::triangulate(s); })},
                     {"Grow Selection", D::List, run([&s] { return M::growSelection(s); })},
                     {"Multi-Cut", D::List, run([this] { toolActs_[5]->trigger(); return QString(); })}};
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
                     {"Bevel", D::List, run([this] { return M::bevel(*scene_, bevelOffset_, bevelSegments_, bevelRound_); })},
                     {"Bridge", D::List, run([this] { return M::bridge(*scene_, bridgeDivisions_); })},
                     {"Harden Edge", D::List, run([&s] { return M::hardenEdges(s); })},
                     {"Soften Edge", D::List, run([&s] { return M::softenEdges(s); })},
                     {"Multi-Cut", D::List, run([this] { toolActs_[5]->trigger(); return QString(); })},
                     {"Cut UVs", D::List, run([&s] { return M::cutUVs(s); })},
                     {"Sew UVs", D::List, run([&s] { return M::sewUVs(s); })},
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
                     {"Target Weld", D::NW, run([this] { toolActs_[6]->trigger(); return QString(); })},
                     {"Bevel", D::SE, run([this] { return M::bevel(*scene_, bevelOffset_, bevelSegments_, bevelRound_); })},
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
                                  "Insert Edge Loop: hover an edge to preview, click (or drag to slide) to insert.",
                                  "Multi-Cut: click points on edges/vertices, Enter or RMB to cut. Ctrl+click inserts an edge loop.",
                                  "Target Weld: drag from one vertex onto another to merge them.",
                                  "Quad Draw: click to place dots on the live surface, Shift+click fills a quad, Shift+drag relaxes.",
                                  "Sculpt: drag on the mesh. Shift = smooth, Ctrl = invert, B+drag = radius, M+drag = strength."};
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
<tr><td><b>Shift + RMB</b></td><td>Modeling tools marking menu (on empty space: create primitives)</td></tr>
<tr><td><b>RMB in Sculpt / Quad Draw</b></td><td>Brush menu / retopology menu (Shift+RMB: sculpt options)</td></tr>
<tr><td><b>Space + click centre</b></td><td>Hotbox view menu (Top / Front / Side / Persp / Four View)</td></tr>
<tr><td><b>Y</b></td><td>Last tool used</td></tr>
<tr><td><b>+ / -</b></td><td>Manipulator size</td></tr>
<tr><td><b>Ctrl+1</b></td><td>Isolate Select</td></tr>
<tr><td><b>F1</b></td><td>Hotkey Editor (rebind anything)</td></tr>
<tr><td><b>Ctrl + RMB</b></td><td>Convert-selection marking menu</td></tr>
<tr><td><b>Ctrl+E</b></td><td>Extrude</td></tr>
<tr><td><b>Ctrl+B</b></td><td>Bevel (Edit Mesh &gt; Bevel Options for offset / segments)</td></tr>
<tr><td><b>Multi-Cut</b></td><td>click edges/verts, Shift = 10% snap, Ctrl+click = loop, Enter/RMB = cut</td></tr>
<tr><td><b>Quad Draw</b></td><td>click = dot, Shift+click = quad, Shift+drag = relax, Ctrl+click = delete / insert loop, Tab+drag = extend</td></tr>
<tr><td><b>6</b></td><td>UV checker display; <b>Ctrl+Shift+U</b> UV Editor (W/E/R, F, A inside it)</td></tr>
<tr><td><b>Ctrl+I</b></td><td>Import FBX / OBJ</td></tr>
<tr><td><b>Sculpt tool</b></td><td>drag = brush, Shift = smooth, Ctrl = invert, B+drag = radius, M+drag = strength</td></tr>
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

void MainWindow::buildPluginMenus()
{
    QMenu* pm = menuBar()->addMenu("&Plugins");
    menus_.emplace_back("Plugins", pm);
    addAct(pm, "Plugin Manager...", {}, [this] { showPluginManager(); return QString(); });
    pm->addSeparator();
    for (const auto& lp : plugins_->plugins()) {
        QMenu* sub = pm->addMenu(lp.plugin->name());
        for (const PluginCommand& c : lp.commands) {
            PluginCommand cmd = c;
            auto run = [this, cmd] { return plugins_->run(cmd); };
            QList<QKeySequence> keys;
            if (!cmd.shortcut.isEmpty()) keys << QKeySequence(cmd.shortcut);
            QAction* a = addAct(sub, cmd.label, keys, run, true);
            a->setToolTip(cmd.tooltip);
            if (!cmd.options.empty())
                addAct(sub, cmd.label + " Options...", {}, [this, cmd] {
                    if (pluginOptionsDialog(cmd)) return plugins_->run(cmd);
                    return QString();
                });
            // Also list it in the menu it asks for (e.g. UV).
            for (auto& [title, menu] : menus_)
                if (!cmd.menu.isEmpty() && title == cmd.menu) {
                    menu->addSeparator();
                    QAction* b = addAct(menu, cmd.label + "  (plugin)", {}, run, true);
                    b->setToolTip(cmd.tooltip);
                    if (!cmd.options.empty())
                        addAct(menu, cmd.label + " Options...", {}, [this, cmd] {
                            if (pluginOptionsDialog(cmd)) return plugins_->run(cmd);
                            return QString();
                        });
                }
        }
    }
    if (plugins_->plugins().empty()) pm->addAction("(no plugins found)")->setEnabled(false);
}

bool MainWindow::pluginOptionsDialog(const PluginCommand& c)
{
    QDialog d(this);
    d.setWindowTitle(c.label + " Options");
    auto* f = new QFormLayout(&d);
    QVariantMap cur = plugins_->options(c);
    std::vector<std::function<void(QVariantMap&)>> readers;
    for (const PluginOption& o : c.options) {
        QVariant v = cur.value(o.key, o.defaultValue);
        QWidget* w = nullptr;
        if (o.defaultValue.typeId() == QMetaType::Bool) {
            auto* cb = new QCheckBox;
            cb->setChecked(v.toBool());
            readers.push_back([cb, k = o.key](QVariantMap& m) { m[k] = cb->isChecked(); });
            w = cb;
        } else if (o.defaultValue.typeId() == QMetaType::Int) {
            auto* sb = new QSpinBox;
            sb->setRange(int(o.minimum), int(std::min(o.maximum, 2e9)));
            sb->setValue(v.toInt());
            readers.push_back([sb, k = o.key](QVariantMap& m) { m[k] = sb->value(); });
            w = sb;
        } else {
            auto* sb = new QDoubleSpinBox;
            sb->setDecimals(3);
            sb->setRange(o.minimum, o.maximum);
            sb->setValue(v.toDouble());
            readers.push_back([sb, k = o.key](QVariantMap& m) { m[k] = sb->value(); });
            w = sb;
        }
        w->setToolTip(o.tooltip);
        f->addRow(o.label + ":", w);
    }
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText("Apply");
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return false;
    QVariantMap out;
    for (auto& r : readers) r(out);
    plugins_->setOptions(c, out);
    return true;
}

void MainWindow::showPluginManager()
{
    QString html = "<h3>Loaded plugins</h3>";
    if (plugins_->plugins().empty()) html += "<p>None.</p>";
    for (const auto& p : plugins_->plugins()) {
        html += QString("<p><b>%1</b> %2<br><i>%3</i><br>%4<br>Commands: ")
                    .arg(p.plugin->name().toHtmlEscaped(), p.plugin->version().toHtmlEscaped(), p.file.toHtmlEscaped(),
                         p.plugin->description().toHtmlEscaped());
        QStringList cmds;
        for (const auto& c : p.commands) cmds << c.label.toHtmlEscaped();
        html += cmds.join(", ") + "</p>";
    }
    html += "<h3>Search paths</h3><ul>";
    for (const QString& s : plugins_->searchPaths()) html += "<li>" + s.toHtmlEscaped() + "</li>";
    html += "</ul><p>Add folders with the <code>FACET_PLUGIN_PATH</code> environment variable.</p>";
    if (!plugins_->errors().isEmpty()) {
        html += "<h3>Errors</h3><ul>";
        for (const QString& e : plugins_->errors()) html += "<li>" + e.toHtmlEscaped() + "</li>";
        html += "</ul>";
    }
    auto* d = new QDialog(this);
    d->setWindowTitle("Plugin Manager");
    d->resize(620, 420);
    auto* tb = new QTextBrowser(d);
    tb->setHtml(html);
    auto* l = new QVBoxLayout(d);
    l->addWidget(tb);
    d->show();
}

void MainWindow::sculptSettings()
{
    QDialog d(this);
    d.setWindowTitle("Sculpt Brush Settings");
    auto* f = new QFormLayout(&d);
    auto* rad = new QDoubleSpinBox;
    rad->setRange(3, 2000);
    rad->setValue(scene_->sculpt.radiusPx);
    rad->setSuffix(" px");
    auto* str = new QDoubleSpinBox;
    str->setRange(0.01, 1.0);
    str->setSingleStep(0.05);
    str->setValue(scene_->sculpt.strength);
    f->addRow("Radius (screen):", rad);
    f->addRow("Strength:", str);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() == QDialog::Accepted) {
        scene_->sculpt.radiusPx = float(rad->value());
        scene_->sculpt.strength = float(str->value());
        viewport_->update();
    }
}

QMenu* MainWindow::buildOutlinerMenu()
{
    Scene& s = *scene_;
    auto* m = new QMenu(this);
    auto act = [this, m](const QString& text, std::function<QString()> fn) {
        QAction* a = m->addAction(text);
        connect(a, &QAction::triggered, this, [this, fn] { report(fn()); });
        return a;
    };
    const bool sel = !s.selection().empty();
    act("Rename", [this] {
        if (QTreeWidgetItem* it = outliner_->currentItem()) outliner_->editItem(it);
        return QString();
    })->setEnabled(sel);
    act("Frame Selection", [this] { viewport_->frameSelected(); return QString(); })->setEnabled(sel);
    act(s.isolateActive ? "Exit Isolate Select" : "Isolate Select", [&s] { return M::toggleIsolate(s); });
    m->addSeparator();
    act("Duplicate", [&s] { return M::duplicate(s); })->setEnabled(sel);
    act("Delete", [&s] { s.setMode(SelectMode::Object); return M::deleteSelection(s); })->setEnabled(sel);
    m->addSeparator();
    act("Hide", [&s] { return M::hideSelected(s); })->setEnabled(sel);
    act("Show All", [&s] { return M::showAll(s); });
    m->addSeparator();
    act(s.liveId && s.lead() && s.lead()->id == s.liveId ? "Clear Live" : "Make Live", [&s] { return M::toggleLive(s); })->setEnabled(sel || s.liveId);
    act("Center Pivot", [&s] { return M::centerPivot(s); })->setEnabled(sel);
    act("Freeze Transformations", [&s] { return M::freezeTransforms(s); })->setEnabled(sel);
    act("Reset Transformations", [&s] { return M::resetTransforms(s); })->setEnabled(sel);
    act("Delete History", [&s] { return M::deleteHistory(s); })->setEnabled(sel);
    m->addSeparator();
    act("Select All", [&s] { s.setMode(SelectMode::Object); return M::selectAll(s); });
    return m;
}

void MainWindow::showHotkeyEditor()
{
    auto* d = new HotkeyEditor(hotkeys_, this);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->show();
}

void MainWindow::bevelOptions()
{
    QDialog d(this);
    d.setWindowTitle("Bevel Options");
    auto* f = new QFormLayout(&d);
    auto* off = new QDoubleSpinBox;
    off->setDecimals(4);
    off->setRange(0.0001, 1000);
    off->setValue(bevelOffset_);
    auto* seg = new QSpinBox;
    seg->setRange(1, 32);
    seg->setValue(bevelSegments_);
    auto* round = new QCheckBox("Round profile");
    round->setChecked(bevelRound_);
    f->addRow("Offset (fraction-free, world units):", off);
    f->addRow("Segments:", seg);
    f->addRow(round);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel);
    f->addRow(bb);
    auto store = [&] { bevelOffset_ = float(off->value()); bevelSegments_ = seg->value(); bevelRound_ = round->isChecked(); };
    connect(bb, &QDialogButtonBox::accepted, &d, [&] { store(); d.accept(); });
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    connect(bb->button(QDialogButtonBox::Apply), &QPushButton::clicked, &d, [&] {
        store();
        report(M::bevel(*scene_, bevelOffset_, bevelSegments_, bevelRound_));
        d.accept();
    });
    d.exec();
}

void MainWindow::setChecker(bool on) { checkerAct_->setChecked(on); }

void MainWindow::openUVEditor()
{
    uvDock_->show();
    uvDock_->raise();
    QTimer::singleShot(0, uvEditor_->canvas(), [c = uvEditor_->canvas()] { c->frame(false); });
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
