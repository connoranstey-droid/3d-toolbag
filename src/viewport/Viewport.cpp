#include "viewport/Viewport.h"
#include "scene/Commands.h"
#include "util/Parallel.h"

#include <QApplication>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>

#include <atomic>
#include <cmath>
#include <unordered_set>

namespace facet {

namespace {
struct Background { Vec3 top, bottom; const char* name; };
const Background kBackgrounds[] = {
    {{0.42f, 0.47f, 0.53f}, {0.13f, 0.14f, 0.16f}, "Gradient"},
    {{0.36f, 0.36f, 0.36f}, {0.36f, 0.36f, 0.36f}, "Gray"},
    {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, "Black"},
    {{0.63f, 0.63f, 0.63f}, {0.63f, 0.63f, 0.63f}, "Light gray"},
};
constexpr int kSlotOrderQuad[4] = {1, 0, 2, 3}; // top | persp / front | side (Maya four-view)
} // namespace

Viewport::Viewport(Scene* scene, QWidget* parent) : QOpenGLWidget(parent), scene_(scene)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(320, 240);
    hotboxTimer_.setSingleShot(true);
    hotboxTimer_.setInterval(180);
    connect(&hotboxTimer_, &QTimer::timeout, this, [this] {
        if (spaceHeld_) {
            hotboxShown_ = true;
            emit hotboxRequested(QCursor::pos());
        }
    });
    auto refresh = [this] { update(); };
    connect(scene_, &Scene::sceneChanged, this, refresh);
    connect(scene_, &Scene::selectionChanged, this, refresh);
    connect(scene_, &Scene::toolChanged, this, [this] {
        loop_ = {};
        cut_ = {};
        weld_ = {};
        qd_ = {};
        update();
    });
    connect(scene_, &Scene::modeChanged, this, [this] { hoverComp_ = {}; update(); });
    connect(scene_, &Scene::objectsChanged, this, [this] {
        loop_ = {};
        hoverComp_ = {};
        cut_ = {};
        weld_ = {};
        if (qd_.target && !scene_->find(qd_.target)) qd_ = {};
        update();
    });
}

Viewport::~Viewport()
{
    if (glReady_) {
        makeCurrent();
        renderer_.cleanup();
        auto* gl = renderer_.gl();
        if (pickFbo_) gl->glDeleteFramebuffers(1, &pickFbo_);
        if (pickTex_) gl->glDeleteTextures(1, &pickTex_);
        if (pickDepth_) gl->glDeleteRenderbuffers(1, &pickDepth_);
        doneCurrent();
    }
}

// ------------------------------------------------------------------------------------------
// Layout

std::vector<int> Viewport::visibleSlots() const
{
    if (quad_) return {0, 1, 2, 3};
    return {single_};
}

QRect Viewport::slotRect(int slot) const
{
    if (!quad_) return rect();
    int pos = 0;
    for (int i = 0; i < 4; ++i)
        if (kSlotOrderQuad[i] == slot) pos = i;
    int hw = width() / 2, hh = height() / 2;
    int col = pos % 2, row = pos / 2;
    return QRect(col * hw + (col ? 1 : 0), row * hh + (row ? 1 : 0), col ? width() - hw - 1 : hw, row ? height() - hh - 1 : hh);
}

int Viewport::slotAt(QPoint p) const
{
    for (int s : visibleSlots())
        if (slotRect(s).contains(p)) return s;
    return visibleSlots().front();
}

ViewParams Viewport::viewParams(int slot) const
{
    const Camera& c = cams_[size_t(slot)];
    QRect r = slotRect(slot);
    ViewParams vp;
    vp.dpr = float(devicePixelRatioF());
    vp.x = int(std::lround(r.x() * vp.dpr));
    vp.y = int(std::lround((height() - r.y() - r.height()) * vp.dpr));
    vp.w = std::max(1, int(std::lround(r.width() * vp.dpr)));
    vp.h = std::max(1, int(std::lround(r.height() * vp.dpr)));
    vp.view = c.view();
    vp.proj = c.proj(float(r.width()) / std::max(1, r.height()));
    vp.viewProj = vp.proj * vp.view;
    vp.eye = c.eye();
    vp.forward = c.forward();
    vp.ortho = c.ortho();
    return vp;
}

Vec3 Viewport::project(int slot, const Vec3& w) const
{
    QRect r = slotRect(slot);
    const Camera& c = cams_[size_t(slot)];
    Mat4 vpm = c.proj(float(r.width()) / std::max(1, r.height())) * c.view();
    Vec4 clip = vpm * Vec4(w.x, w.y, w.z, 1.0f);
    if (clip.w <= 1e-6f) return {-1e6f, -1e6f, 2.0f};
    Vec3 ndc{clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
    return {r.x() + (ndc.x * 0.5f + 0.5f) * r.width(), r.y() + (1.0f - (ndc.y * 0.5f + 0.5f)) * r.height(), ndc.z};
}

GizmoView Viewport::gizmoView(int slot) const
{
    const Camera& c = cams_[size_t(slot)];
    GizmoView v;
    v.viewProj = viewParams(slot).viewProj;
    v.forward = c.forward();
    v.right = c.right();
    v.up = c.up();
    v.eye = c.eye();
    v.ortho = c.ortho();
    v.toScreen = [this, slot](const Vec3& w) {
        Vec3 p = project(slot, w);
        return Vec2{p.x, p.y};
    };
    return v;
}

Ray Viewport::rayAt(int slot, QPoint p) const
{
    QRect r = slotRect(slot);
    float nx = (float(p.x() - r.x()) + 0.5f) / r.width() * 2.0f - 1.0f;
    float ny = 1.0f - (float(p.y() - r.y()) + 0.5f) / r.height() * 2.0f;
    return cams_[size_t(slot)].ray(nx, ny, float(r.width()) / std::max(1, r.height()));
}

void Viewport::setQuadView(bool on)
{
    quad_ = on;
    if (!on) single_ = active_;
    update();
}

void Viewport::toggleQuadView()
{
    if (quad_) {
        single_ = slotAt(mapFromGlobal(QCursor::pos()));
        active_ = single_;
        quad_ = false;
    } else {
        quad_ = true;
    }
    update();
}

void Viewport::setSingleView(ViewKind k)
{
    quad_ = false;
    single_ = active_ = int(k);
    update();
}

void Viewport::cycleBackground()
{
    bgIndex_ = (bgIndex_ + 1) % int(std::size(kBackgrounds));
    emit message(QString("Background: %1").arg(kBackgrounds[bgIndex_].name));
    update();
}

AABB Viewport::selectionBounds(bool componentsOnly) const
{
    AABB box;
    for (SceneObject* o : scene_->selectedObjects()) {
        Mat4 w = o->worldMatrix();
        if (scene_->mode() != SelectMode::Object) {
            for (uint32_t v : scene_->selectedVerts(o)) box.expand(w.transformPoint(o->mesh().positions[v]));
        } else if (!componentsOnly) {
            box.expand(o->mesh().bounds().transformed(w));
        }
    }
    if (!box.valid() && scene_->mode() != SelectMode::Object && !componentsOnly)
        for (SceneObject* o : scene_->selectedObjects()) box.expand(o->mesh().bounds().transformed(o->worldMatrix()));
    return box;
}

void Viewport::frameSelected(bool allViews)
{
    AABB box = selectionBounds(false);
    if (!box.valid()) { frameAll(allViews); return; }
    for (int s : (allViews ? std::vector<int>{0, 1, 2, 3} : std::vector<int>{active_})) {
        QRect r = slotRect(s);
        cams_[size_t(s)].frame(box, float(r.width()) / std::max(1, r.height()));
    }
    update();
}

void Viewport::frameAll(bool allViews)
{
    AABB box;
    for (auto& o : scene_->objects)
        if (o->st.visible) box.expand(o->mesh().bounds().transformed(o->worldMatrix()));
    if (!box.valid()) { box.expand({-6, -1, -6}); box.expand({6, 1, 6}); }
    for (int s : (allViews ? std::vector<int>{0, 1, 2, 3} : std::vector<int>{active_})) {
        QRect r = slotRect(s);
        cams_[size_t(s)].frame(box, float(r.width()) / std::max(1, r.height()));
    }
    update();
}

// ------------------------------------------------------------------------------------------
// GL lifecycle

void Viewport::initializeGL()
{
    if (!renderer_.initialize()) {
        qWarning("OpenGL 4.1 core profile is required");
        return;
    }
    glReady_ = true;
}

void Viewport::resizeGL(int, int) { update(); }

void Viewport::paintGL()
{
    if (!glReady_) return;
    auto* gl = renderer_.gl();
    renderer_.purge(*scene_);
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glClearColor(0.11f, 0.11f, 0.12f, 1.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const auto& bg = kBackgrounds[bgIndex_];
    for (int s : visibleSlots()) {
        ViewParams vp = viewParams(s);
        renderer_.beginView(vp, s == active_);
        renderer_.drawBackground(bg.top, bg.bottom);
        if (options.showGrid) {
            const Camera& c = cams_[size_t(s)];
            int lines = 12;
            float spacing = 1.0f;
            if (c.ortho()) {
                float extent = c.orthoHeight * float(vp.w) / vp.h;
                while (extent / spacing > 60.0f) spacing *= 5.0f;
                lines = std::max(12, int(extent / spacing) + 4);
                Vec3 shift = c.coi;
                (void)shift;
            }
            renderer_.drawGrid(vp, int(c.kind), spacing, lines);
        }
        renderer_.drawScene(*scene_, vp, options);
        drawOverlays(s, vp);
        renderer_.drawAxisTriad(vp);
    }
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glBindVertexArray(0);
    gl->glUseProgram(0);

    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
    gl->glViewport(0, 0, int(width() * devicePixelRatioF()), int(height() * devicePixelRatioF()));
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    drawHud(p);
}

void Viewport::drawOverlays(int slot, const ViewParams& vp)
{
    auto* gl = renderer_.gl();
    // Insert-edge-loop preview.
    if (scene_->tool() == Tool::InsertEdgeLoop && loop_.obj && !loop_.lines.empty()) {
        gl->glDisable(GL_DEPTH_TEST);
        renderer_.drawLines(loop_.lines, {1.0f, 0.9f, 0.1f, 1.0f}, vp.viewProj, false);
    }
    // Multi-Cut preview.
    if (scene_->tool() == Tool::MultiCut && (cut_.obj || cut_.hoverValid)) {
        std::vector<Vec3> pts = cut_.world;
        if (cut_.hoverValid) pts.push_back(cut_.hoverWorld);
        std::vector<Vec3> lines;
        for (size_t i = 0; i + 1 < pts.size(); ++i) lines.insert(lines.end(), {pts[i], pts[i + 1]});
        renderer_.drawLines(lines, {1.0f, 0.85f, 0.1f, 1.0f}, vp.viewProj, false);
        renderer_.drawPoints(cut_.world, {0.2f, 1.0f, 0.4f, 1.0f}, vp.viewProj, 7.0f * vp.dpr, false);
        if (cut_.hoverValid) renderer_.drawPoints({cut_.hoverWorld}, {1.0f, 0.3f, 0.2f, 1.0f}, vp.viewProj, 8.0f * vp.dpr, false);
    }
    // Target Weld preview.
    if (scene_->tool() == Tool::TargetWeld && weld_.obj && weld_.src != kInvalid && weld_.src < weld_.obj->mesh().numVerts()) {
        Mat4 w = weld_.obj->worldMatrix();
        Vec3 a = w.transformPoint(weld_.obj->mesh().positions[weld_.src]);
        Vec3 b;
        if (weld_.dst != kInvalid) b = w.transformPoint(weld_.obj->mesh().positions[weld_.dst]);
        else {
            Ray r = rayAt(slot, mousePos_);
            b = r.origin + r.dir * dot(a - r.origin, r.dir);
        }
        renderer_.drawLines({a, b}, {1.0f, 0.85f, 0.1f, 1.0f}, vp.viewProj, false);
        renderer_.drawPoints({a}, {1.0f, 0.3f, 0.2f, 1.0f}, vp.viewProj, 8.0f * vp.dpr, false);
        if (weld_.dst != kInvalid) renderer_.drawPoints({b}, {0.2f, 1.0f, 0.4f, 1.0f}, vp.viewProj, 9.0f * vp.dpr, false);
    }
    // Quad Draw: dots, hovered component and quad preview.
    if (scene_->tool() == Tool::QuadDraw) {
        if (SceneObject* t = quadTarget(false)) {
            const Mesh& m = t->mesh();
            Mat4 w = t->worldMatrix(), mvp = vp.viewProj * w;
            std::vector<Vec3> dots, verts;
            for (uint32_t v = 0; v < m.numVerts(); ++v) (m.vertexFaces(v).empty() ? dots : verts).push_back(m.positions[v]);
            std::vector<Vec3> wire;
            for (const auto& e : m.edges) wire.insert(wire.end(), {m.positions[e.v0], m.positions[e.v1]});
            renderer_.drawLines(wire, {0.35f, 0.75f, 1.0f, 1.0f}, mvp, false);
            renderer_.drawPoints(verts, {0.35f, 0.75f, 1.0f, 1.0f}, mvp, 5.0f * vp.dpr, false);
            renderer_.drawPoints(dots, {0.2f, 1.0f, 0.35f, 1.0f}, mvp, 7.0f * vp.dpr, false);
            if (qd_.hoverVert != kInvalid && qd_.hoverVert < m.numVerts())
                renderer_.drawPoints({m.positions[qd_.hoverVert]}, {1.0f, 0.3f, 0.2f, 1.0f}, mvp, 9.0f * vp.dpr, false);
            else if (qd_.hoverEdge != kInvalid && qd_.hoverEdge < m.numEdges())
                renderer_.drawLines({m.positions[m.edges[qd_.hoverEdge].v0], m.positions[m.edges[qd_.hoverEdge].v1]}, {1.0f, 0.3f, 0.2f, 1.0f}, mvp, false);
            if (qd_.preview.size() >= 3) {
                std::vector<Vec3> l, tri;
                for (size_t i = 0; i < qd_.preview.size(); ++i)
                    l.insert(l.end(), {m.positions[qd_.preview[i]], m.positions[qd_.preview[(i + 1) % qd_.preview.size()]]});
                for (size_t i = 1; i + 1 < qd_.preview.size(); ++i)
                    tri.insert(tri.end(), {m.positions[qd_.preview[0]], m.positions[qd_.preview[i]], m.positions[qd_.preview[i + 1]]});
                gl->glEnable(GL_BLEND);
                gl->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                renderer_.drawLines(tri, {1.0f, 0.85f, 0.1f, 0.25f}, mvp, false, GL_TRIANGLES);
                gl->glDisable(GL_BLEND);
                renderer_.drawLines(l, {1.0f, 0.85f, 0.1f, 1.0f}, mvp, false);
            }
        }
    }
    // Pre-selection highlight.
    bool hoverAlive = false;
    for (const auto& o : scene_->objects) hoverAlive |= o.get() == hoverComp_.obj; // compare, never dereference a stale pointer
    if (hoverAlive && hoverComp_.index != kInvalid && drag_ == Drag::None) {
        SceneObject* o = hoverComp_.obj;
        const Mesh& m = o->mesh();
        Mat4 mvp = vp.viewProj * o->worldMatrix();
        Vec4 red{1.0f, 0.15f, 0.15f, 1.0f};
        if (scene_->mode() == SelectMode::Vertex && hoverComp_.index < m.numVerts())
            renderer_.drawPoints({m.positions[hoverComp_.index]}, red, mvp, 7.0f * vp.dpr, false);
        else if (scene_->mode() == SelectMode::Edge && hoverComp_.index < m.numEdges())
            renderer_.drawLines({m.positions[m.edges[hoverComp_.index].v0], m.positions[m.edges[hoverComp_.index].v1]}, red, mvp, false);
        else if (scene_->mode() == SelectMode::Face && hoverComp_.index < m.numFaces()) {
            std::vector<Vec3> l;
            auto fv = m.faceVertices(hoverComp_.index);
            for (size_t i = 0; i < fv.size(); ++i) l.insert(l.end(), {m.positions[fv[i]], m.positions[fv[(i + 1) % fv.size()]]});
            renderer_.drawLines(l, red, mvp, false);
        }
    }
    // Manipulator.
    Tool t = scene_->tool();
    if (t == Tool::Move || t == Tool::Rotate || t == Tool::Scale) {
        GizmoFrame f;
        if (drag_ == Drag::Manip) f = manip_.frame;
        if (drag_ == Drag::Manip || gizmoFrame(slot, f)) {
            if (drag_ == Drag::Manip) {
                // Keep the handle size constant on screen while dragging.
                GizmoFrame cur;
                if (gizmoFrame(slot, cur)) { f.origin = cur.origin; f.size = cur.size; if (t != Tool::Rotate) { for (int i = 0; i < 3; ++i) f.axis[i] = cur.axis[i]; } }
            }
            drawGizmo(renderer_, t, f, gizmoView(slot), slot == dragSlot_ || drag_ != Drag::Manip ? hover_ : Handle::None,
                      drag_ == Drag::Manip ? manip_.handle : Handle::None);
        }
    }
}

void Viewport::drawHud(QPainter& p)
{
    QFont f = p.font();
    f.setPointSizeF(9.0);
    p.setFont(f);
    const QFontMetrics fm(f);

    for (int s : visibleSlots()) {
        QRect r = slotRect(s);
        const Camera& c = cams_[size_t(s)];
        p.setPen(QColor(230, 230, 230));
        QString name = c.name();
        p.drawText(r.x() + (r.width() - fm.horizontalAdvance(name)) / 2, r.bottom() - 8, name);
        if (quad_) {
            p.setPen(s == active_ ? QPen(QColor(110, 170, 255), 2) : QPen(QColor(20, 20, 20), 1));
            p.setBrush(Qt::NoBrush);
            p.drawRect(r.adjusted(0, 0, -1, -1));
        }
    }

    if (showHud) {
        uint64_t v[3] = {}, e[3] = {}, fc[3] = {}, t[3] = {}, dt = 0;
        bool smoothAny = false;
        for (auto& o : scene_->objects) {
            if (!o->st.visible) continue;
            const Mesh& m = o->mesh();
            bool sel = scene_->isSelected(o->id);
            uint64_t tris = m.triangleCount();
            for (int k = 0; k < (sel ? 2 : 1); ++k) { v[k] += m.numVerts(); e[k] += m.numEdges(); fc[k] += m.numFaces(); t[k] += tris; }
            if (GpuMesh* g = renderer_.gpu(o->id)) {
                if (o->displayFaceOrigin()) smoothAny = true;
                dt += uint64_t(g->triIndexCount / 3);
            }
            if (sel && scene_->mode() != SelectMode::Object) {
                v[2] += o->sel.verts.size();
                e[2] += o->sel.edges.size();
                fc[2] += o->sel.faces.size();
            }
        }
        QRect r = slotRect(active_);
        int x = r.x() + 12, y = r.y() + 20;
        auto num = [](uint64_t n) { return QLocale().toString(qulonglong(n)); };
        QStringList rows = {"Verts:", "Edges:", "Faces:", "Tris:"};
        uint64_t* cols[4] = {v, e, fc, t};
        p.setPen(QColor(235, 235, 235));
        for (int i = 0; i < 4; ++i) {
            p.drawText(x, y + i * 15, rows[i]);
            p.drawText(x + 50, y + i * 15, num(cols[i][0]));
            p.drawText(x + 140, y + i * 15, num(cols[i][1]));
            if (scene_->mode() != SelectMode::Object && i < 3) p.drawText(x + 220, y + i * 15, num(cols[i][2]));
        }
        if (smoothAny) {
            p.setPen(QColor(150, 210, 255));
            p.drawText(x, y + 4 * 15, QString("Display tris (smooth): %1").arg(num(dt)));
        }
    }

    if (drag_ == Drag::Marquee) {
        QRect mr = QRect(pressPos_, lastPos_).normalized();
        if (mr.width() > 2 || mr.height() > 2) {
            p.setPen(QPen(QColor(240, 240, 240), 1, Qt::DashLine));
            p.setBrush(QColor(255, 255, 255, 18));
            p.drawRect(mr);
        }
    }
    if (scene_->tool() == Tool::Sculpt && underMouse() && rect().contains(mousePos_)) {
        static const QColor colors[] = {QColor(120, 200, 255), QColor(160, 255, 160), QColor(255, 200, 90), QColor(255, 140, 220),
                                        QColor(220, 220, 120), QColor(255, 120, 120), QColor(255, 160, 80), QColor(170, 170, 255)};
        const auto b = drag_ == Drag::Sculpt ? sc_.brush : scene_->sculpt.brush;
        QColor c = colors[int(b)];
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(c, 1.5));
        p.setBrush(Qt::NoBrush);
        const float rpx = scene_->sculpt.radiusPx;
        p.drawEllipse(QPointF(mousePos_), rpx, rpx);
        p.setPen(QPen(c, 1, Qt::DotLine));
        p.drawEllipse(QPointF(mousePos_), rpx * scene_->sculpt.strength, rpx * scene_->sculpt.strength);
        p.setRenderHint(QPainter::Antialiasing, false);
        QRect r = slotRect(active_);
        p.setPen(c);
        p.drawText(r.x() + 12, r.bottom() - 44,
                   QString("Sculpt %1   radius %2px   strength %3   ·   B+drag radius · M+drag strength · Shift = smooth · Ctrl = invert%4")
                       .arg(sculpt::brushName(b))
                       .arg(int(rpx))
                       .arg(scene_->sculpt.strength, 0, 'f', 2)
                       .arg(scene_->symmetry >= 0 ? QString(" · symmetry %1").arg(QChar("XYZ"[scene_->symmetry])) : QString()));
    }
    if (scene_->tool() == Tool::QuadDraw && drag_ == Drag::QuadRelax) {
        p.setPen(QPen(QColor(120, 220, 255), 1));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(mousePos_, 60, 60);
    }
    if (scene_->tool() == Tool::QuadDraw) {
        QRect r = slotRect(active_);
        p.setPen(QColor(150, 230, 160));
        p.drawText(r.x() + 12, r.bottom() - 44,
                   scene_->liveObject() ? "Quad Draw: click = dot · Shift+click = fill quad · Shift+drag = relax · drag dot = slide · Ctrl+click = delete / insert loop · Tab+drag border = extend"
                                        : "Quad Draw: make a mesh live first (Mesh Tools > Make Live)");
    }
    if (scene_->tool() == Tool::MultiCut) {
        QRect r = slotRect(active_);
        p.setPen(QColor(255, 220, 90));
        p.drawText(r.x() + 12, r.bottom() - 44, "Multi-Cut: click edges/vertices · Shift snaps 10% · Ctrl+click = edge loop · Enter/RMB = cut · Backspace = undo point · Esc = clear");
    }
    if (!transientText_.isEmpty()) {
        QRect r = slotRect(active_);
        p.setPen(QColor(255, 220, 90));
        p.drawText(r.x() + 12, r.bottom() - 26, transientText_);
    }
    if (scene_->softSelect && scene_->mode() != SelectMode::Object) {
        QRect r = slotRect(active_);
        p.setPen(QColor(255, 200, 60));
        p.drawText(r.right() - 170, r.y() + 20, QString("Soft Select  r=%1").arg(scene_->softRadius, 0, 'f', 2));
    }
}

// ------------------------------------------------------------------------------------------
// Picking

bool Viewport::renderPick(int slot, QRect logicalRect, bool selectedOnly, std::vector<uint32_t>& ids, std::vector<float>& depth, QRect& devRect)
{
    if (!glReady_) return false;
    makeCurrent();
    auto* gl = renderer_.gl();
    const float dpr = float(devicePixelRatioF());
    QSize dev(int(std::lround(width() * dpr)), int(std::lround(height() * dpr)));
    if (dev != pickSize_ || !pickFbo_) {
        if (pickFbo_) gl->glDeleteFramebuffers(1, &pickFbo_);
        if (pickTex_) gl->glDeleteTextures(1, &pickTex_);
        if (pickDepth_) gl->glDeleteRenderbuffers(1, &pickDepth_);
        gl->glGenFramebuffers(1, &pickFbo_);
        gl->glGenTextures(1, &pickTex_);
        gl->glBindTexture(GL_TEXTURE_2D, pickTex_);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32UI, dev.width(), dev.height(), 0, GL_RG_INTEGER, GL_UNSIGNED_INT, nullptr);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl->glGenRenderbuffers(1, &pickDepth_);
        gl->glBindRenderbuffer(GL_RENDERBUFFER, pickDepth_);
        gl->glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, dev.width(), dev.height());
        gl->glBindFramebuffer(GL_FRAMEBUFFER, pickFbo_);
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pickTex_, 0);
        gl->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, pickDepth_);
        if (gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) qWarning("Pick framebuffer incomplete");
        pickSize_ = dev;
    }
    ViewParams vp = viewParams(slot);
    QRect vr(vp.x, vp.y, vp.w, vp.h);
    QRect lr = logicalRect.intersected(slotRect(slot));
    if (lr.isEmpty()) { doneCurrent(); return false; }
    int rx = int(std::floor(lr.x() * dpr)), rw = std::max(1, int(std::ceil(lr.width() * dpr)));
    int rh = std::max(1, int(std::ceil(lr.height() * dpr)));
    int ry = dev.height() - int(std::floor(lr.y() * dpr)) - rh;
    devRect = QRect(rx, ry, rw, rh).intersected(vr);
    if (devRect.isEmpty()) { doneCurrent(); return false; }

    gl->glBindFramebuffer(GL_FRAMEBUFFER, pickFbo_);
    gl->glViewport(vp.x, vp.y, vp.w, vp.h);
    gl->glEnable(GL_SCISSOR_TEST);
    gl->glScissor(devRect.x(), devRect.y(), devRect.width(), devRect.height());
    const GLuint zero[4] = {0, 0, 0, 0};
    gl->glClearBufferuiv(GL_COLOR, 0, zero);
    gl->glClearDepth(1.0);
    gl->glClear(GL_DEPTH_BUFFER_BIT);
    renderer_.drawPick(*scene_, vp, selectedOnly);
    ids.resize(size_t(devRect.width()) * devRect.height() * 2);
    depth.resize(size_t(devRect.width()) * devRect.height());
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    gl->glReadBuffer(GL_COLOR_ATTACHMENT0);
    gl->glReadPixels(devRect.x(), devRect.y(), devRect.width(), devRect.height(), GL_RG_INTEGER, GL_UNSIGNED_INT, ids.data());
    gl->glReadPixels(devRect.x(), devRect.y(), devRect.width(), devRect.height(), GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    doneCurrent();
    return true;
}

Viewport::Hit Viewport::decode(uint32_t objId, uint32_t prim) const
{
    Hit h;
    if (!objId) return h;
    SceneObject* o = scene_->find(objId);
    GpuMesh* g = const_cast<Renderer&>(renderer_).gpu(objId);
    if (!o || !g || prim >= g->triFace.size()) return h;
    uint32_t df = g->triFace[prim];
    const std::vector<uint32_t>* origin = o->displayFaceOrigin();
    h.obj = o;
    h.cageFace = origin ? (df < origin->size() ? (*origin)[df] : kInvalid) : df;
    if (h.cageFace >= o->mesh().numFaces()) h.cageFace = kInvalid;
    return h;
}

Viewport::Hit Viewport::pickSurface(int slot, QPoint p, int radius, bool selectedOnly)
{
    std::vector<uint32_t> ids;
    std::vector<float> depth;
    QRect dr;
    if (!renderPick(slot, QRect(p.x() - radius, p.y() - radius, 2 * radius + 1, 2 * radius + 1), selectedOnly, ids, depth, dr)) return {};
    const float dpr = float(devicePixelRatioF());
    float cx = p.x() * dpr, cy = pickSize_.height() - p.y() * dpr;
    float best = 1e9f;
    Hit out;
    for (int y = 0; y < dr.height(); ++y)
        for (int x = 0; x < dr.width(); ++x) {
            size_t k = size_t(y) * dr.width() + x;
            if (!ids[k * 2]) continue;
            float d = std::hypot(dr.x() + x + 0.5f - cx, dr.y() + y + 0.5f - cy);
            if (d < best) {
                Hit h = decode(ids[k * 2], ids[k * 2 + 1]);
                if (h.obj) { best = d; out = h; }
            }
        }
    return out;
}

uint32_t Viewport::pickVertex(int slot, QPoint p, SceneObject*& obj)
{
    obj = nullptr;
    Vec2 mp{float(p.x()), float(p.y())};
    Hit h = pickSurface(slot, p, 4, true);
    float best = 22.0f;
    uint32_t bestV = kInvalid;
    if (h.obj && h.cageFace != kInvalid) {
        Mat4 w = h.obj->worldMatrix();
        for (uint32_t v : h.obj->mesh().faceVertices(h.cageFace)) {
            Vec3 s = project(slot, w.transformPoint(h.obj->mesh().positions[v]));
            float d = (Vec2{s.x, s.y} - mp).length();
            if (d < best) { best = d; bestV = v; obj = h.obj; }
        }
    }
    if (bestV == kInvalid) {
        // Off-surface fallback (silhouettes, floating smooth-preview cage): nearest projected vertex.
        best = 9.0f;
        for (SceneObject* o : scene_->selectedObjects()) {
            if (!o->st.visible) continue;
            Mat4 w = o->worldMatrix();
            const Mesh& m = o->mesh();
            for (uint32_t v = 0; v < m.numVerts(); ++v) {
                Vec3 s = project(slot, w.transformPoint(m.positions[v]));
                float d = (Vec2{s.x, s.y} - mp).length();
                if (d < best) { best = d; bestV = v; obj = o; }
            }
        }
    }
    return bestV;
}

uint32_t Viewport::pickEdge(int slot, QPoint p, bool selectedOnly, SceneObject*& obj, float* tOut)
{
    obj = nullptr;
    Hit h = pickSurface(slot, p, 5, selectedOnly);
    if (!h.obj || h.cageFace == kInvalid) return kInvalid;
    const Mesh& m = h.obj->mesh();
    Mat4 w = h.obj->worldMatrix();
    Vec2 mp{float(p.x()), float(p.y())};
    float best = 1e9f;
    uint32_t bestE = kInvalid;
    for (uint32_t he = m.faceStart[h.cageFace]; he < m.faceStart[h.cageFace + 1]; ++he) {
        uint32_t e = m.heEdge[he];
        Vec3 a = project(slot, w.transformPoint(m.positions[m.edges[e].v0]));
        Vec3 b = project(slot, w.transformPoint(m.positions[m.edges[e].v1]));
        Vec2 A{a.x, a.y}, B{b.x, b.y}, AB = B - A;
        float l2 = dot(AB, AB);
        float t = l2 > 0 ? std::clamp(dot(mp - A, AB) / l2, 0.0f, 1.0f) : 0.0f;
        float d = (mp - (A + AB * t)).length();
        if (d < best) { best = d; bestE = e; }
    }
    if (bestE != kInvalid && tOut) {
        Vec3 a = w.transformPoint(m.positions[m.edges[bestE].v0]), b = w.transformPoint(m.positions[m.edges[bestE].v1]);
        *tOut = std::clamp(closestOnLine(a, b - a, rayAt(slot, p)), 0.02f, 0.98f);
    }
    obj = h.obj;
    return bestE;
}

SelectOp Viewport::opFromModifiers(Qt::KeyboardModifiers m)
{
    bool shift = m & Qt::ShiftModifier, ctrl = m & Qt::ControlModifier;
    if (shift && ctrl) return SelectOp::Add;
    if (shift) return SelectOp::Toggle;
    if (ctrl) return SelectOp::Remove;
    return SelectOp::Replace;
}

void Viewport::clickSelect(int slot, QPoint p, SelectOp op)
{
    const SelectMode mode = scene_->mode();
    if (mode == SelectMode::Object) {
        Hit h = pickSurface(slot, p, 3, false);
        if (h.obj) scene_->selectObjects({h.obj->id}, op);
        else if (op == SelectOp::Replace) scene_->selectObjects({}, SelectOp::Replace);
        return;
    }
    SceneObject* obj = nullptr;
    uint32_t idx = kInvalid;
    if (mode == SelectMode::Vertex) idx = pickVertex(slot, p, obj);
    else if (mode == SelectMode::Edge) idx = pickEdge(slot, p, true, obj);
    else {
        Hit h = pickSurface(slot, p, 2, true);
        obj = h.obj;
        idx = h.cageFace;
    }
    if (op == SelectOp::Replace) scene_->clearComponents(false);
    if (obj && idx != kInvalid) scene_->selectComponents(obj, {idx}, op == SelectOp::Replace ? SelectOp::Add : op);
    else scene_->notifySelectionChanged();
}

void Viewport::marqueeSelect(int slot, QRect r, SelectOp op)
{
    r = r.normalized().intersected(slotRect(slot));
    if (r.isEmpty()) return;
    const SelectMode mode = scene_->mode();
    std::vector<uint32_t> ids;
    std::vector<float> depth;
    QRect dr;
    const bool selectedOnly = mode != SelectMode::Object;
    if (!renderPick(slot, r, selectedOnly, ids, depth, dr)) return;
    const bool through = options.shading == ShadingMode::Wireframe || options.xray;

    if (mode == SelectMode::Object) {
        std::vector<uint32_t> hit;
        std::unordered_set<uint32_t> seen;
        for (size_t k = 0; k < depth.size(); ++k)
            if (ids[k * 2] && seen.insert(ids[k * 2]).second) hit.push_back(ids[k * 2]);
        if (through) {
            // In wireframe also catch objects whose projected bounds intersect the box.
            for (auto& o : scene_->objects) {
                if (!o->st.visible || seen.count(o->id)) continue;
                AABB b = o->mesh().bounds().transformed(o->worldMatrix());
                QRectF pr;
                for (int i = 0; i < 8; ++i) {
                    Vec3 s = project(slot, {(i & 1) ? b.hi.x : b.lo.x, (i & 2) ? b.hi.y : b.lo.y, (i & 4) ? b.hi.z : b.lo.z});
                    pr = pr.isNull() ? QRectF(s.x, s.y, 0.01, 0.01) : pr.united(QRectF(s.x, s.y, 0.01, 0.01));
                }
                if (pr.intersects(QRectF(r))) hit.push_back(o->id);
            }
        }
        scene_->selectObjects(hit, op);
        return;
    }

    if (op == SelectOp::Replace) scene_->clearComponents(false);
    const SelectOp addOp = op == SelectOp::Replace ? SelectOp::Add : op;
    const float dpr = float(devicePixelRatioF());

    if (mode == SelectMode::Face && !through) {
        std::unordered_map<uint32_t, std::vector<uint32_t>> faces;
        for (size_t k = 0; k < depth.size(); ++k) {
            if (!ids[k * 2]) continue;
            Hit h = decode(ids[k * 2], ids[k * 2 + 1]);
            if (h.obj && h.cageFace != kInvalid) faces[h.obj->id].push_back(h.cageFace);
        }
        for (auto& [id, list] : faces) scene_->selectComponents(scene_->find(id), list, addOp, false);
        scene_->notifySelectionChanged();
        return;
    }

    // Vertices, edges (and faces in x-ray / wireframe) are tested by projection + depth visibility.
    const Camera& cam = cams_[size_t(slot)];
    const float zn = cam.nearClip(), zf = cam.farClip();
    auto linearize = [&](float d) {
        if (cam.ortho()) return zn + d * (zf - zn);
        float ndc = d * 2.0f - 1.0f;
        return 2.0f * zn * zf / (zf + zn - ndc * (zf - zn));
    };
    auto visible = [&](float sx, float sy, float ndcZ) {
        if (through) return true;
        int px = int(sx * dpr) - dr.x(), py = (pickSize_.height() - int(sy * dpr)) - dr.y();
        float maxD = -1;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                int x = px + dx, y = py + dy;
                if (x < 0 || y < 0 || x >= dr.width() || y >= dr.height()) continue;
                maxD = std::max(maxD, depth[size_t(y) * dr.width() + x]);
            }
        if (maxD < 0) return true;
        if (maxD >= 1.0f) return true;
        float zp = linearize(ndcZ * 0.5f + 0.5f), zb = linearize(maxD);
        return zp <= zb * 1.004f + 1e-4f;
    };
    const QRectF rf(r);
    for (SceneObject* o : scene_->selectedObjects()) {
        if (!o->st.visible) continue;
        const Mesh& m = o->mesh();
        Mat4 w = o->worldMatrix();
        std::vector<uint32_t> picked;
        if (mode == SelectMode::Vertex) {
            std::vector<uint8_t> flag(m.numVerts(), 0);
            parallelFor(m.numVerts(), [&](size_t v) {
                Vec3 s = project(slot, w.transformPoint(m.positions[v]));
                if (s.z <= 1.0f && rf.contains(s.x, s.y) && visible(s.x, s.y, s.z)) flag[v] = 1;
            });
            for (uint32_t v = 0; v < m.numVerts(); ++v)
                if (flag[v]) picked.push_back(v);
        } else if (mode == SelectMode::Edge) {
            std::vector<uint8_t> flag(m.numEdges(), 0);
            parallelFor(m.numEdges(), [&](size_t e) {
                Vec3 s = project(slot, w.transformPoint(m.edgeMidpoint(uint32_t(e))));
                if (s.z <= 1.0f && rf.contains(s.x, s.y) && visible(s.x, s.y, s.z)) flag[e] = 1;
            });
            for (uint32_t e = 0; e < m.numEdges(); ++e)
                if (flag[e]) picked.push_back(e);
        } else {
            std::vector<uint8_t> flag(m.numFaces(), 0);
            parallelFor(m.numFaces(), [&](size_t f) {
                Vec3 s = project(slot, w.transformPoint(m.faceCenter(uint32_t(f))));
                if (s.z <= 1.0f && rf.contains(s.x, s.y)) flag[f] = 1;
            });
            for (uint32_t f = 0; f < m.numFaces(); ++f)
                if (flag[f]) picked.push_back(f);
        }
        scene_->selectComponents(o, picked, addOp, false);
    }
    scene_->notifySelectionChanged();
}

// ------------------------------------------------------------------------------------------
// Manipulators

bool Viewport::gizmoFrame(int slot, GizmoFrame& f) const
{
    Tool t = scene_->tool();
    SceneObject* lead = scene_->lead();
    if (!lead) return false;
    AxisOrient orient = t == Tool::Move ? scene_->moveOrient : t == Tool::Rotate ? scene_->rotateOrient : scene_->scaleOrient;
    Mat4 rot = Mat4::rotateXYZ(lead->st.rotate);
    if (scene_->mode() == SelectMode::Object) {
        f.origin = lead->st.translate;
        if (t == Tool::Scale) orient = AxisOrient::Object; // object scale is always local
        if (orient != AxisOrient::World)
            for (int i = 0; i < 3; ++i) f.axis[i] = normalize(rot.transformDir(Vec3{i == 0 ? 1.f : 0.f, i == 1 ? 1.f : 0.f, i == 2 ? 1.f : 0.f}));
    } else {
        AABB box;
        Vec3 normal;
        for (SceneObject* o : scene_->selectedObjects()) {
            Mat4 w = o->worldMatrix();
            const Mesh& m = o->mesh();
            std::vector<uint32_t> vs = scene_->selectedVerts(o);
            for (uint32_t v : vs) box.expand(w.transformPoint(m.positions[v]));
            if (orient == AxisOrient::Normal) {
                Mat4 nm = w.inverse().transposed();
                std::vector<uint32_t> fs = scene_->mode() == SelectMode::Face ? o->sel.faces : ops::convert(m, scene_->compType(), o->compList(scene_->mode()), ops::Comp::Face);
                if (scene_->mode() == SelectMode::Vertex) fs = ops::convert(m, ops::Comp::Vertex, o->sel.verts, ops::Comp::Face);
                for (uint32_t fi : fs) normal += nm.transformDir(m.faceNormalRaw(fi));
            }
        }
        if (!box.valid()) return false;
        f.origin = box.center();
        if (orient == AxisOrient::Object) {
            for (int i = 0; i < 3; ++i) f.axis[i] = normalize(rot.transformDir(Vec3{i == 0 ? 1.f : 0.f, i == 1 ? 1.f : 0.f, i == 2 ? 1.f : 0.f}));
        } else if (orient == AxisOrient::Normal && length(normal) > 1e-8f) {
            Vec3 n = normalize(normal);
            Vec3 x = cross({0, 1, 0}, n);
            if (length(x) < 1e-4f) x = {1, 0, 0};
            x = normalize(x - n * dot(x, n));
            if (std::abs(n.y) > 0.999f) x = {1, 0, 0};
            Vec3 z = normalize(cross(x, n));
            f.axis[0] = x; f.axis[1] = n; f.axis[2] = z;
        }
    }
    const Camera& c = cams_[size_t(slot)];
    QRect r = slotRect(slot);
    float depth = std::max(c.nearClip() * 2, dot(f.origin - c.eye(), c.forward()));
    f.size = 95.0f * c.worldPerPixel(float(r.height()), depth);
    return true;
}

void Viewport::beginManip(int slot, Handle h, QPoint p)
{
    Manip m;
    m.tool = scene_->tool();
    m.handle = h;
    if (!gizmoFrame(slot, m.frame)) return;
    m.startPos = p;
    Ray ray = rayAt(slot, p);
    const Camera& c = cams_[size_t(slot)];
    const GizmoFrame& f = m.frame;
    auto axisIndex = [](Handle hh) { return hh == Handle::X ? 0 : hh == Handle::Y ? 1 : hh == Handle::Z ? 2 : -1; };
    int ai = axisIndex(h);
    if (m.tool == Tool::Move || m.tool == Tool::Scale) {
        if (ai >= 0) m.startParam = closestOnLine(f.origin, f.axis[ai], ray);
        Vec3 n = h == Handle::YZ ? f.axis[0] : h == Handle::XZ ? f.axis[1] : h == Handle::XY ? f.axis[2] : c.forward();
        if (ai < 0) rayPlane(ray, f.origin, n, m.startHit);
    } else if (m.tool == Tool::Rotate) {
        Vec3 axis = ai >= 0 ? f.axis[ai] : c.forward();
        Vec3 hit;
        if (rayPlane(ray, f.origin, axis, hit)) m.prevVec = normalize(hit - f.origin);
    }
    if (scene_->mode() == SelectMode::Object) {
        for (SceneObject* o : scene_->selectedObjects()) m.objStart.emplace_back(o->id, o->st);
    } else {
        for (SceneObject* o : scene_->selectedObjects()) {
            std::vector<float> w = scene_->softWeights(o);
            if (w.empty()) continue;
            CompEntry ce{o, o->st.mesh, {}, {}, {}, o->worldMatrix(), o->worldMatrix().inverse(), {}};
            for (uint32_t v = 0; v < w.size(); ++v)
                if (w[v] > 0.0f) {
                    ce.idx.push_back(v);
                    ce.w.push_back(w[v]);
                    ce.start.push_back(o->mesh().positions[v]);
                    ce.mirror.push_back(0);
                }
            if (scene_->symmetry >= 0) {
                // Each affected vertex drags its mirror partner; the negative side follows the positive side.
                const int ax = scene_->symmetry;
                auto mm = scene_->mirrorMap(o, ax);
                std::vector<int> slot(w.size(), -1);
                for (size_t k = 0; k < ce.idx.size(); ++k) slot[ce.idx[k]] = int(k);
                const size_t n0 = ce.idx.size();
                for (size_t k = 0; k < n0; ++k) {
                    uint32_t v = ce.idx[k], mv = mm[v];
                    if (mv == kInvalid) continue;
                    if (mv == v) { ce.mirror[k] = 2; continue; }
                    if (slot[mv] >= 0) {
                        if (ce.start[k][ax] < 0) ce.mirror[k] = 1;
                        continue;
                    }
                    slot[mv] = int(ce.idx.size());
                    ce.idx.push_back(mv);
                    ce.w.push_back(ce.w[k]);
                    ce.start.push_back(o->mesh().positions[mv]);
                    ce.mirror.push_back(1);
                }
            }
            if (!ce.idx.empty()) m.comps.push_back(std::move(ce));
        }
        if (m.comps.empty()) return;
    }
    if (m.objStart.empty() && m.comps.empty()) return;
    int ti = m.tool == Tool::Move ? 0 : m.tool == Tool::Rotate ? 1 : 2;
    lastHandle_[ti] = h;
    manip_ = std::move(m);
    drag_ = Drag::Manip;
    dragSlot_ = slot;
    scene_->dragging = true;
}

void Viewport::updateManip(QPoint p)
{
    Manip& m = manip_;
    const GizmoFrame& f = m.frame;
    const Camera& c = cams_[size_t(dragSlot_)];
    Ray ray = rayAt(dragSlot_, p);
    m.moved = true;
    int ai = m.handle == Handle::X ? 0 : m.handle == Handle::Y ? 1 : m.handle == Handle::Z ? 2 : -1;

    Vec3 delta;
    Vec3 rotAxis{0, 1, 0};
    Vec3 scl{1, 1, 1};
    if (m.tool == Tool::Move) {
        if (ai >= 0) {
            float t = closestOnLine(f.origin, f.axis[ai], ray);
            delta = f.axis[ai] * (t - m.startParam);
        } else {
            Vec3 n = m.handle == Handle::YZ ? f.axis[0] : m.handle == Handle::XZ ? f.axis[1] : m.handle == Handle::XY ? f.axis[2] : c.forward();
            Vec3 hit;
            if (rayPlane(ray, f.origin, n, hit)) delta = hit - m.startHit;
            if (m.handle != Handle::Center) delta = delta - n * dot(delta, n);
        }
        if (xHeld_) {
            // Grid snap: land the pivot on whole grid units along the free directions.
            Vec3 target = f.origin + delta;
            Vec3 snapped{std::round(target.x), std::round(target.y), std::round(target.z)};
            Vec3 d = snapped - f.origin;
            if (ai >= 0) delta = f.axis[ai] * dot(d, f.axis[ai]);
            else delta = d;
        }
        transientText_ = QString("Move  %1, %2, %3").arg(delta.x, 0, 'f', 3).arg(delta.y, 0, 'f', 3).arg(delta.z, 0, 'f', 3);
    } else if (m.tool == Tool::Rotate) {
        rotAxis = ai >= 0 ? f.axis[ai] : c.forward();
        Vec3 hit;
        if (std::abs(dot(normalize(ray.dir), rotAxis)) > 0.08f && rayPlane(ray, f.origin, rotAxis, hit)) {
            Vec3 v = normalize(hit - f.origin);
            float a = degrees(std::atan2(dot(cross(m.prevVec, v), rotAxis), dot(m.prevVec, v)));
            m.angle += a;
            m.prevVec = v;
        } else {
            m.angle = float(p.x() - m.startPos.x()) * 0.5f;
        }
        transientText_ = QString("Rotate  %1°").arg(jHeld_ ? std::round(m.angle / 15.0f) * 15.0f : m.angle, 0, 'f', 2);
    } else if (m.tool == Tool::Scale) {
        if (ai >= 0) {
            float t = closestOnLine(f.origin, f.axis[ai], ray);
            float s = std::abs(m.startParam) > 1e-6f ? t / m.startParam : 1.0f;
            scl[ai] = s;
        } else {
            float s = std::exp(float(p.x() - m.startPos.x()) * 0.01f);
            scl = {s, s, s};
        }
        transientText_ = QString("Scale  %1, %2, %3").arg(scl.x, 0, 'f', 3).arg(scl.y, 0, 'f', 3).arg(scl.z, 0, 'f', 3);
    }
    float angle = jHeld_ ? std::round(m.angle / 15.0f) * 15.0f : m.angle;

    for (auto& [id, st0] : m.objStart) {
        SceneObject* o = scene_->find(id);
        if (!o) continue;
        if (m.tool == Tool::Move) o->st.translate = st0.translate + delta;
        else if (m.tool == Tool::Rotate) o->st.rotate = eulerXYZ(rotateAxis(rotAxis, angle) * Mat4::rotateXYZ(st0.rotate));
        else if (m.tool == Tool::Scale) o->st.scale = mul(st0.scale, scl);
    }
    for (CompEntry& ce : m.comps) {
        Mesh& mesh = *ce.mesh;
        const Vec3 localDelta = ce.inv.transformDir(delta);
        const int ax = scene_->symmetry;
        auto xf = [&](const Vec3& p0, float w) {
            if (m.tool == Tool::Move) return p0 + localDelta * w;
            if (m.tool == Tool::Rotate) {
                Vec3 pw = ce.world.transformPoint(p0);
                Vec3 r = rotateAxis(rotAxis, angle * w).transformDir(pw - f.origin);
                return ce.inv.transformPoint(f.origin + r);
            }
            Vec3 q = ce.world.transformPoint(p0) - f.origin, q2;
            for (int i = 0; i < 3; ++i) q2 += f.axis[i] * (dot(q, f.axis[i]) * (1.0f + (scl[i] - 1.0f) * w));
            return ce.inv.transformPoint(f.origin + q2);
        };
        parallelFor(ce.idx.size(), [&](size_t k) {
            Vec3 out;
            const uint8_t mir = ce.mirror.empty() ? 0 : ce.mirror[k];
            if (mir == 1 && ax >= 0) {
                Vec3 q = ce.start[k];
                q[ax] = -q[ax];
                out = xf(q, ce.w[k]);
                out[ax] = -out[ax];
            } else {
                out = xf(ce.start[k], ce.w[k]);
                if (mir == 2 && ax >= 0) out[ax] = 0.0f;
            }
            mesh.positions[ce.idx[k]] = out;
        }, 4096);
        mesh.touchPositions();
    }
    scene_->notifyChanged();
}

void Viewport::endManip()
{
    scene_->dragging = false;
    transientText_.clear();
    Manip& m = manip_;
    if (!m.moved) { drag_ = Drag::None; update(); return; }
    const QString label = m.tool == Tool::Move ? "Move" : m.tool == Tool::Rotate ? "Rotate" : "Scale";
    if (!m.objStart.empty()) {
        scene_->pushStates(m.objStart, label);
    } else {
        std::vector<VertexMoveCommand::Entry> entries;
        bool anyHistory = false;
        for (CompEntry& ce : m.comps) {
            VertexMoveCommand::Entry e{ce.obj->id, ce.mesh, ce.idx, ce.start, {}};
            e.after.reserve(ce.idx.size());
            for (uint32_t v : ce.idx) e.after.push_back(ce.mesh->positions[v]);
            entries.push_back(std::move(e));
            anyHistory |= ce.obj->st.history.has_value();
        }
        scene_->undo.beginMacro(label + " components");
        if (anyHistory) {
            // Tweaking vertices bakes the primitive: its INPUTS can no longer regenerate it.
            std::vector<std::pair<uint32_t, ObjectState>> before;
            for (CompEntry& ce : m.comps) {
                before.emplace_back(ce.obj->id, ce.obj->st);
                ce.obj->st.history.reset();
            }
            scene_->pushStates(before, "Delete history");
        }
        scene_->undo.push(new VertexMoveCommand(scene_, std::move(entries), label));
        scene_->undo.endMacro();
    }
    drag_ = Drag::None;
    update();
}

// ------------------------------------------------------------------------------------------
// Tools

void Viewport::updateLoopPreview(int slot, QPoint p, bool keepEdge)
{
    SceneObject* obj = loop_.obj;
    float t = 0.5f;
    uint32_t e = kInvalid;
    if (keepEdge && obj && loop_.edge != kInvalid) {
        const Mesh& m = obj->mesh();
        Mat4 w = obj->worldMatrix();
        Vec3 a = w.transformPoint(m.positions[m.edges[loop_.edge].v0]), b = w.transformPoint(m.positions[m.edges[loop_.edge].v1]);
        t = std::clamp(closestOnLine(a, b - a, rayAt(slot, p)), 0.02f, 0.98f);
        e = loop_.edge;
    } else {
        e = pickEdge(slot, p, false, obj, &t);
    }
    loop_ = {};
    if (!obj || e == kInvalid) { update(); return; }
    loop_.obj = obj;
    loop_.edge = e;
    loop_.t = t;
    loop_.plan = ops::planEdgeLoop(obj->mesh(), e, t);
    const Mesh& m = obj->mesh();
    Mat4 w = obj->worldMatrix();
    std::unordered_map<uint32_t, float> splitT;
    for (const auto& s : loop_.plan.splits) splitT[s.edge] = s.t;
    auto pt = [&](uint32_t edge) {
        const MeshEdge& ed = m.edges[edge];
        return w.transformPoint(lerp(m.positions[ed.v0], m.positions[ed.v1], splitT[edge]));
    };
    for (const auto& c : loop_.plan.cuts) loop_.lines.insert(loop_.lines.end(), {pt(c.e0), pt(c.e1)});
    update();
}

void Viewport::updateHover(int slot, QPoint p)
{
    HoverComp h;
    if (scene_->mode() != SelectMode::Object && scene_->tool() != Tool::InsertEdgeLoop && !scene_->selectedObjects().empty()) {
        if (scene_->mode() == SelectMode::Vertex) h.index = pickVertex(slot, p, h.obj);
        else if (scene_->mode() == SelectMode::Edge) h.index = pickEdge(slot, p, true, h.obj);
        else {
            Hit hit = pickSurface(slot, p, 1, true);
            h.obj = hit.obj;
            h.index = hit.cageFace;
        }
    }
    if (h.obj != hoverComp_.obj || h.index != hoverComp_.index) {
        hoverComp_ = h;
        update();
    }
}

// ------------------------------------------------------------------------------------------
// Multi-Cut

bool Viewport::cutPointAt(int slot, QPoint p, SceneObject*& obj, ops::CutPoint& cp, Vec3& world)
{
    Hit h = pickSurface(slot, p, 6, false);
    if (!h.obj || h.cageFace == kInvalid) return false;
    const Mesh& m = h.obj->mesh();
    Mat4 w = h.obj->worldMatrix();
    Vec2 mp{float(p.x()), float(p.y())};
    // Snap to a face vertex within 10px.
    for (uint32_t v : m.faceVertices(h.cageFace)) {
        Vec3 s = project(slot, w.transformPoint(m.positions[v]));
        if ((Vec2{s.x, s.y} - mp).length() < 10.0f) {
            obj = h.obj;
            cp = {v, kInvalid, 0.0f};
            world = w.transformPoint(m.positions[v]);
            return true;
        }
    }
    float t = 0.5f;
    uint32_t e = pickEdge(slot, p, false, obj, &t);
    if (!obj || e == kInvalid) return false;
    if (QApplication::keyboardModifiers() & Qt::ShiftModifier) t = std::clamp(std::round(t * 10.0f) / 10.0f, 0.1f, 0.9f);
    const MeshEdge& ed = obj->mesh().edges[e];
    cp = {ed.v0, ed.v1, t};
    world = w.transformPoint(lerp(obj->mesh().positions[ed.v0], obj->mesh().positions[ed.v1], t));
    return true;
}

void Viewport::commitCut()
{
    if (!cut_.obj || cut_.pts.size() < 2) { cut_ = {}; update(); return; }
    SceneObject* obj = cut_.obj;
    auto pts = cut_.pts;
    cut_ = {};
    if (!scene_->isSelected(obj->id)) scene_->selectObjects({obj->id}, SelectOp::Replace);
    if (scene_->mode() == SelectMode::Object) scene_->setMode(SelectMode::Edge);
    scene_->editMesh(obj, "Multi-Cut", [&](Mesh& m) -> std::optional<std::vector<uint32_t>> {
        auto edges = ops::multiCut(m, pts);
        if (scene_->mode() == SelectMode::Edge) return edges;
        return std::nullopt;
    });
    emit message("Multi-Cut applied");
    update();
}

// ------------------------------------------------------------------------------------------
// Quad Draw (retopology on a live surface)

bool Viewport::liveHit(int slot, QPoint p, Vec3& world)
{
    return scene_->rayToLive(rayAt(slot, p), world);
}

SceneObject* Viewport::quadTarget(bool create)
{
    if (SceneObject* t = qd_.target ? scene_->find(qd_.target) : nullptr) return t;
    SceneObject* lead = scene_->lead();
    if (lead && lead->id != scene_->liveId) {
        qd_.target = lead->id;
        return lead;
    }
    if (!create) return nullptr;
    auto o = std::make_unique<SceneObject>();
    o->id = scene_->allocateId();
    o->st.name = scene_->uniqueName("retopo1");
    o->st.mesh = std::make_shared<Mesh>();
    o->st.mesh->buildTopology();
    qd_.target = o->id;
    std::vector<std::unique_ptr<SceneObject>> v;
    v.push_back(std::move(o));
    scene_->undo.push(new AddRemoveCommand(scene_, std::move(v), "Create retopology mesh"));
    return scene_->find(qd_.target);
}

void Viewport::updateQuadHover(int slot, QPoint p, bool shift)
{
    QuadState old = qd_;
    qd_.hoverVert = qd_.hoverEdge = kInvalid;
    qd_.preview.clear();
    SceneObject* t = quadTarget(false);
    if (!t) { if (old.hoverVert != kInvalid || !old.preview.empty()) update(); return; }
    const Mesh& m = t->mesh();
    Mat4 w = t->worldMatrix();
    Vec2 mp{float(p.x()), float(p.y())};
    std::vector<Vec2> scr(m.numVerts());
    float best = 12.0f;
    for (uint32_t v = 0; v < m.numVerts(); ++v) {
        Vec3 s = project(slot, w.transformPoint(m.positions[v]));
        scr[v] = {s.x, s.y};
        float d = (scr[v] - mp).length();
        if (d < best) { best = d; qd_.hoverVert = v; }
    }
    if (qd_.hoverVert == kInvalid) {
        float bestE = 8.0f;
        for (uint32_t e = 0; e < m.numEdges(); ++e) {
            Vec2 a = scr[m.edges[e].v0], b = scr[m.edges[e].v1], ab = b - a;
            float l2 = dot(ab, ab);
            float tt = l2 > 0 ? std::clamp(dot(mp - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
            float d = (mp - (a + ab * tt)).length();
            if (d < bestE) { bestE = d; qd_.hoverEdge = e; qd_.hoverT = std::clamp(tt, 0.05f, 0.95f); }
        }
    }
    if (shift) {
        // Four nearest free vertices (dots or border vertices) around the cursor.
        std::vector<std::pair<float, uint32_t>> cand;
        for (uint32_t v = 0; v < m.numVerts(); ++v) {
            if (!m.vertexFaces(v).empty() && !m.isBorderVertex(v)) continue;
            float d = (scr[v] - mp).length();
            if (d < 260.0f) cand.push_back({d, v});
        }
        std::sort(cand.begin(), cand.end());
        if (cand.size() >= 4) {
            std::vector<uint32_t> q{cand[0].second, cand[1].second, cand[2].second, cand[3].second};
            Vec2 c;
            for (uint32_t v : q) c += scr[v];
            c = c / 4.0f;
            std::sort(q.begin(), q.end(), [&](uint32_t a, uint32_t b) {
                return std::atan2(scr[a].y - c.y, scr[a].x - c.x) < std::atan2(scr[b].y - c.y, scr[b].x - c.x);
            });
            // Face the camera.
            Vec3 n, ctr;
            for (size_t i = 0; i < 4; ++i) {
                Vec3 a = w.transformPoint(m.positions[q[i]]), b = w.transformPoint(m.positions[q[(i + 1) % 4]]);
                n += cross(a, b);
                ctr += a;
            }
            const Camera& cam = cams_[size_t(slot)];
            Vec3 toCam = cam.ortho() ? -cam.forward() : cam.eye() - ctr / 4.0f;
            if (dot(n, toCam) < 0) std::reverse(q.begin(), q.end());
            qd_.preview = q;
        }
    }
    if (qd_.hoverVert != old.hoverVert || qd_.hoverEdge != old.hoverEdge || qd_.preview != old.preview) update();
}

namespace {
// Runs an op that may drop isolated vertices (dots) and puts the dots back afterwards.
template <class Fn>
void keepDots(Mesh& m, Fn fn)
{
    if (!m.topologyValid()) m.buildTopology();
    std::vector<Vec3> dots;
    for (uint32_t v = 0; v < m.numVerts(); ++v)
        if (m.vertexFaces(v).empty()) dots.push_back(m.positions[v]);
    fn(m);
    bool added = false;
    for (const Vec3& d : dots) {
        bool exists = false;
        for (const Vec3& p : m.positions) exists |= length2(p - d) < 1e-14f;
        if (!exists) { m.addVertex(d); added = true; }
    }
    if (added) m.buildTopology();
}
} // namespace

void Viewport::quadPress(int slot, QPoint p, Qt::KeyboardModifiers mods)
{
    if (!scene_->liveObject()) { emit message("Quad Draw: make a mesh live first (Mesh Tools > Make Live)"); return; }
    qd_.pressPos = p;
    const bool shift = mods & Qt::ShiftModifier, ctrl = mods & Qt::ControlModifier;
    updateQuadHover(slot, p, shift);
    SceneObject* t = quadTarget(false);
    if (tabHeld_ && t && qd_.hoverEdge != kInvalid && t->mesh().isBorderEdge(qd_.hoverEdge)) {
        drag_ = Drag::QuadExtend;
        return;
    }
    if (shift && !ctrl) {
        drag_ = Drag::QuadShift; // click = fill quad, drag = relax
        qd_.touched.clear();
        qd_.before.clear();
        return;
    }
    if (ctrl && t) {
        const Mesh& m = t->mesh();
        if (qd_.hoverVert != kInvalid) {
            uint32_t v = qd_.hoverVert;
            scene_->editMesh(t, "Quad Draw: delete vertex", [&](Mesh& mm) -> std::optional<std::vector<uint32_t>> {
                if (mm.vertexFaces(v).empty()) {
                    // Remove an isolated dot.
                    mm.positions.erase(mm.positions.begin() + v);
                    for (uint32_t& fv : mm.faceVerts) if (fv > v) --fv;
                    mm.edges.clear();
                    mm.buildTopology();
                } else {
                    keepDots(mm, [&](Mesh& x) { ops::deleteVertices(x, {v}); });
                }
                return std::nullopt;
            });
        } else if (qd_.hoverEdge != kInvalid && !shift) {
            auto plan = ops::planEdgeLoop(m, qd_.hoverEdge, qd_.hoverT);
            if (plan.valid()) {
                Mat4 w = t->worldMatrix(), inv = w.inverse();
                scene_->editMesh(t, "Quad Draw: insert loop", [&](Mesh& mm) -> std::optional<std::vector<uint32_t>> {
                    const uint32_t before = mm.numVerts();
                    keepDots(mm, [&](Mesh& x) { ops::insertEdgeLoop(x, plan); });
                    for (uint32_t v = before; v < mm.numVerts(); ++v) {
                        Vec3 out;
                        if (scene_->closestOnLive(w.transformPoint(mm.positions[v]), out)) mm.positions[v] = inv.transformPoint(out);
                    }
                    mm.touchPositions();
                    return std::nullopt;
                });
            }
        } else {
            // Ctrl+Shift+click on a face deletes it.
            Mat4 w = t->worldMatrix();
            Vec2 mp{float(p.x()), float(p.y())};
            for (uint32_t f = 0; f < m.numFaces(); ++f) {
                auto fv = m.faceVertices(f);
                bool inside = false;
                for (size_t i = 0, j = fv.size() - 1; i < fv.size(); j = i++) {
                    Vec3 a = project(slot, w.transformPoint(m.positions[fv[i]])), b = project(slot, w.transformPoint(m.positions[fv[j]]));
                    if ((a.y > mp.y) != (b.y > mp.y) && mp.x < (b.x - a.x) * (mp.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
                }
                if (inside) {
                    scene_->editMesh(t, "Quad Draw: delete face", [&](Mesh& mm) -> std::optional<std::vector<uint32_t>> {
                        keepDots(mm, [&](Mesh& x) {
                            // Keep the face's vertices as dots too.
                            std::vector<Vec3> fvp;
                            for (uint32_t v : x.faceVertices(f)) fvp.push_back(x.positions[v]);
                            ops::deleteFaces(x, {f});
                            for (const Vec3& d : fvp) {
                                bool exists = false;
                                for (const Vec3& q : x.positions) exists |= length2(q - d) < 1e-14f;
                                if (!exists) x.addVertex(d);
                            }
                            x.buildTopology();
                        });
                        return std::nullopt;
                    });
                    break;
                }
            }
        }
        updateQuadHover(slot, p, false);
        return;
    }
    if (t && qd_.hoverVert != kInvalid) {
        drag_ = Drag::QuadVert;
        qd_.dragVert = qd_.hoverVert;
        qd_.touched = {qd_.dragVert};
        qd_.before = {t->mesh().positions[qd_.dragVert]};
        scene_->dragging = true;
        return;
    }
    Vec3 hit;
    if (!liveHit(slot, p, hit)) return;
    SceneObject* target = quadTarget(true);
    Mat4 inv = target->worldMatrix().inverse();
    scene_->editMesh(target, "Quad Draw: add point", [&](Mesh& mm) -> std::optional<std::vector<uint32_t>> {
        mm.addVertex(inv.transformPoint(hit));
        mm.buildTopology();
        return std::nullopt;
    }, true);
    updateQuadHover(slot, p, false);
}

void Viewport::quadRelax(int slot, QPoint p)
{
    SceneObject* t = quadTarget(false);
    if (!t) return;
    Mesh& m = *t->st.mesh;
    Mat4 w = t->worldMatrix(), inv = w.inverse();
    Vec2 mp{float(p.x()), float(p.y())};
    const float radius = 60.0f;
    std::vector<std::pair<uint32_t, Vec3>> updates;
    for (uint32_t v = 0; v < m.numVerts(); ++v) {
        if (m.vertexEdges(v).size() < 2 || m.isBorderVertex(v)) continue;
        Vec3 s = project(slot, w.transformPoint(m.positions[v]));
        float d = (Vec2{s.x, s.y} - mp).length();
        if (d > radius) continue;
        Vec3 avg;
        for (uint32_t e : m.vertexEdges(v)) avg += m.positions[m.edgeOther(e, v)];
        avg = avg / float(m.vertexEdges(v).size());
        float k = 0.5f * (1.0f - d / radius);
        Vec3 np = lerp(m.positions[v], avg, k), onLive;
        if (scene_->closestOnLive(w.transformPoint(np), onLive)) np = inv.transformPoint(onLive);
        updates.push_back({v, np});
    }
    for (auto& [v, np] : updates) {
        if (std::find(qd_.touched.begin(), qd_.touched.end(), v) == qd_.touched.end()) {
            qd_.touched.push_back(v);
            qd_.before.push_back(m.positions[v]);
        }
        m.positions[v] = np;
    }
    if (!updates.empty()) {
        m.touchPositions();
        scene_->notifyChanged();
    }
}

void Viewport::quadRelease(QPoint p)
{
    SceneObject* t = quadTarget(false);
    const Drag d = drag_;
    drag_ = Drag::None;
    scene_->dragging = false;
    if (!t) return;
    Mesh& m = *t->st.mesh;
    if (d == Drag::QuadShift) {
        // Plain Shift+click: fill the previewed quad.
        if (qd_.preview.size() == 4) {
            auto q = qd_.preview;
            scene_->editMesh(t, "Quad Draw: fill quad", [&](Mesh& mm) -> std::optional<std::vector<uint32_t>> {
                mm.addFace(q);
                mm.buildTopology();
                return std::nullopt;
            });
        }
    } else if ((d == Drag::QuadVert || d == Drag::QuadRelax) && !qd_.touched.empty()) {
        VertexMoveCommand::Entry e{t->id, t->st.mesh, qd_.touched, qd_.before, {}};
        for (uint32_t v : qd_.touched) e.after.push_back(m.positions[v]);
        std::vector<VertexMoveCommand::Entry> entries;
        entries.push_back(std::move(e));
        scene_->undo.push(new VertexMoveCommand(scene_, std::move(entries), d == Drag::QuadVert ? "Quad Draw: move" : "Quad Draw: relax"));
    } else if (d == Drag::QuadExtend && qd_.hoverEdge != kInvalid && qd_.hoverEdge < m.numEdges()) {
        Vec3 hit;
        if (liveHit(dragSlot_, p, hit)) {
            Mat4 w = t->worldMatrix(), inv = w.inverse();
            const MeshEdge ed = m.edges[qd_.hoverEdge];
            Vec3 a = w.transformPoint(m.positions[ed.v0]), b = w.transformPoint(m.positions[ed.v1]);
            Vec3 off = hit - (a + b) * 0.5f, a2 = a + off, b2 = b + off, tmp;
            if (scene_->closestOnLive(a2, tmp)) a2 = tmp;
            if (scene_->closestOnLive(b2, tmp)) b2 = tmp;
            const Camera& cam = cams_[size_t(dragSlot_)];
            scene_->editMesh(t, "Quad Draw: extend", [&](Mesh& mm) -> std::optional<std::vector<uint32_t>> {
                uint32_t na = mm.addVertex(inv.transformPoint(a2)), nb = mm.addVertex(inv.transformPoint(b2));
                std::vector<uint32_t> q{ed.v0, ed.v1, nb, na};
                Vec3 n = cross(b - a, a2 - a), toCam = cam.ortho() ? -cam.forward() : cam.eye() - a;
                if (dot(n, toCam) < 0) std::reverse(q.begin(), q.end());
                mm.addFace(q);
                mm.buildTopology();
                return std::nullopt;
            });
        }
    }
    qd_.touched.clear();
    qd_.before.clear();
    updateQuadHover(dragSlot_, p, false);
}

// ------------------------------------------------------------------------------------------
// Sculpting

bool Viewport::surfaceHit(int slot, QPoint p, bool selectedOnly, SceneObject*& obj, Vec3& world)
{
    std::vector<uint32_t> ids;
    std::vector<float> depth;
    QRect dr;
    const int r = 2;
    if (!renderPick(slot, QRect(p.x() - r, p.y() - r, 2 * r + 1, 2 * r + 1), selectedOnly, ids, depth, dr)) return false;
    const float dpr = float(devicePixelRatioF());
    const float cx = p.x() * dpr, cy = pickSize_.height() - p.y() * dpr;
    float best = 1e9f;
    int bx = -1, by = -1;
    for (int y = 0; y < dr.height(); ++y)
        for (int x = 0; x < dr.width(); ++x) {
            size_t k = size_t(y) * dr.width() + x;
            if (!ids[k * 2] || depth[k] >= 1.0f) continue;
            float d = std::hypot(dr.x() + x + 0.5f - cx, dr.y() + y + 0.5f - cy);
            if (d < best) { best = d; bx = x; by = y; }
        }
    if (bx < 0) return false;
    size_t k = size_t(by) * dr.width() + bx;
    obj = scene_->find(ids[k * 2]);
    if (!obj) return false;
    ViewParams vp = viewParams(slot);
    float px = dr.x() + bx + 0.5f, py = dr.y() + by + 0.5f;
    Vec3 ndc{(px - vp.x) / vp.w * 2.0f - 1.0f, (py - vp.y) / vp.h * 2.0f - 1.0f, depth[k] * 2.0f - 1.0f};
    world = vp.viewProj.inverse().project(ndc);
    return true;
}

void Viewport::sculptPress(int slot, QPoint p, Qt::KeyboardModifiers mods)
{
    SceneObject* obj = nullptr;
    Vec3 hit;
    // Prefer the selected mesh; fall back to whatever is under the cursor (and select it).
    if (!surfaceHit(slot, p, !scene_->selectedObjects().empty(), obj, hit) && !surfaceHit(slot, p, false, obj, hit)) return;
    if (!scene_->isSelected(obj->id)) scene_->selectObjects({obj->id}, SelectOp::Replace);
    if (obj->displayFaceOrigin()) emit message("Sculpting the cage: bake with Mesh > Smooth (or press 1) to sculpt the dense surface");
    const Camera& cam = cams_[size_t(slot)];
    float depth = std::max(cam.nearClip(), dot(hit - cam.eye(), cam.forward()));
    float rWorld = scene_->sculpt.radiusPx * cam.worldPerPixel(float(slotRect(slot).height()), depth);
    Vec3 sc = obj->st.scale;
    float scale = std::max((std::abs(sc.x) + std::abs(sc.y) + std::abs(sc.z)) / 3.0f, 1e-6f);
    sc_.obj = obj;
    sc_.inv = obj->worldMatrix().inverse();
    sc_.radiusLocal = rWorld / scale;
    sc_.brush = (mods & Qt::ShiftModifier) ? sculpt::Brush::Smooth : scene_->sculpt.brush;
    sc_.invert = mods & Qt::ControlModifier;
    if (sc_.brush == sculpt::Brush::Mask && !obj->hasMask()) obj->mask.assign(obj->mesh().numVerts(), 0.0f);
    sc_.stroke.begin(*obj->st.mesh, &obj->mask, sc_.brush, sc_.radiusLocal, scene_->symmetry);
    sc_.lastLocal = sc_.inv.transformPoint(hit);
    sc_.grabStartWorld = hit;
    sc_.grabNormal = cam.forward();
    sc_.stroke.dab({sc_.lastLocal, sc_.radiusLocal, scene_->sculpt.strength, sc_.invert});
    if (sc_.brush == sculpt::Brush::Mask) obj->maskVersion++;
    drag_ = Drag::Sculpt;
    scene_->notifyChanged();
}

void Viewport::sculptMove(QPoint p)
{
    if (!sc_.obj || !sc_.stroke.active()) return;
    if (sc_.brush == sculpt::Brush::Grab) {
        Vec3 pt;
        if (rayPlane(rayAt(dragSlot_, p), sc_.grabStartWorld, sc_.grabNormal, pt)) {
            sc_.stroke.grab(sc_.inv.transformDir(pt - sc_.grabStartWorld));
            scene_->notifyChanged();
        }
        return;
    }
    SceneObject* obj = nullptr;
    Vec3 hit;
    if (!surfaceHit(dragSlot_, p, true, obj, hit) || obj != sc_.obj) return;
    Vec3 local = sc_.inv.transformPoint(hit);
    // Even dab spacing along the stroke.
    const float spacing = std::max(sc_.radiusLocal * 0.2f, 1e-6f);
    float dist = length(local - sc_.lastLocal);
    int n = std::min(64, int(dist / spacing));
    for (int i = 1; i <= n; ++i) {
        Vec3 c = lerp(sc_.lastLocal, local, float(i) / float(n));
        sc_.stroke.dab({c, sc_.radiusLocal, scene_->sculpt.strength, sc_.invert});
    }
    if (n > 0) {
        sc_.lastLocal = local;
        if (sc_.brush == sculpt::Brush::Mask) sc_.obj->maskVersion++;
        scene_->notifyChanged();
    }
}

void Viewport::sculptRelease()
{
    drag_ = Drag::None;
    if (!sc_.obj) return;
    std::vector<uint32_t> idx = sc_.stroke.touched();
    std::vector<Vec3> before = sc_.stroke.before();
    sc_.stroke.end();
    SceneObject* obj = sc_.obj;
    sc_.obj = nullptr;
    if (idx.empty()) { update(); return; }
    VertexMoveCommand::Entry e{obj->id, obj->st.mesh, idx, before, {}};
    for (uint32_t v : idx) e.after.push_back(obj->mesh().positions[v]);
    std::vector<VertexMoveCommand::Entry> entries;
    entries.push_back(std::move(e));
    const QString label = QString("Sculpt %1").arg(sculpt::brushName(sc_.brush));
    scene_->undo.beginMacro(label);
    if (obj->st.history) {
        ObjectState b = obj->st;
        obj->st.history.reset();
        scene_->pushState(obj, b, "Delete history");
    }
    scene_->undo.push(new VertexMoveCommand(scene_, std::move(entries), label));
    scene_->undo.endMacro();
}

// ------------------------------------------------------------------------------------------
// Input

void Viewport::mousePressEvent(QMouseEvent* e)
{
    setFocus();
    const QPoint p = e->position().toPoint();
    pressPos_ = lastPos_ = mousePos_ = p;
    pressMods_ = e->modifiers();
    const int slot = slotAt(p);
    if (active_ != slot) { active_ = slot; update(); }
    dragSlot_ = slot;
    const bool alt = e->modifiers() & Qt::AltModifier;

    if (alt) {
        if (e->buttons() == (Qt::LeftButton | Qt::MiddleButton) || e->button() == Qt::RightButton) drag_ = Drag::Dolly;
        else if (e->button() == Qt::LeftButton) drag_ = cams_[size_t(slot)].ortho() ? Drag::Track : Drag::Tumble;
        else if (e->button() == Qt::MiddleButton) drag_ = Drag::Track;
        return;
    }
    if (e->button() == Qt::RightButton && scene_->tool() == Tool::MultiCut && cut_.pts.size() >= 2) {
        commitCut();
        return;
    }
    if (e->button() == Qt::RightButton) {
        int kind = (e->modifiers() & Qt::ShiftModifier) ? 1 : (e->modifiers() & Qt::ControlModifier) ? 2 : 0;
        uint32_t under = 0;
        if (kind == 0) {
            Hit h = pickSurface(slot, p, 3, false);
            under = h.obj ? h.obj->id : 0;
        }
        drag_ = Drag::Menu;
        emit markingMenuRequested(kind, e->globalPosition().toPoint(), under);
        return;
    }
    if (e->button() == Qt::LeftButton && bHeld_) {
        bUsed_ = true;
        drag_ = scene_->tool() == Tool::Sculpt ? Drag::BrushRadius : Drag::SoftRadius;
        return;
    }
    if (e->button() == Qt::LeftButton && mHeld_ && scene_->tool() == Tool::Sculpt) {
        drag_ = Drag::BrushStrength;
        return;
    }
    if (e->button() == Qt::LeftButton && scene_->tool() == Tool::Sculpt) {
        sculptPress(slot, p, e->modifiers());
        return;
    }
    const Tool tool = scene_->tool();
    if (e->button() == Qt::LeftButton && tool == Tool::QuadDraw) {
        quadPress(slot, p, e->modifiers());
        return;
    }
    if (e->button() == Qt::LeftButton && tool == Tool::MultiCut) {
        SceneObject* obj = nullptr;
        ops::CutPoint cp;
        Vec3 world;
        if (cutPointAt(slot, p, obj, cp, world)) {
            if (e->modifiers() & Qt::ControlModifier && cp.b != kInvalid) {
                // Ctrl+click: full edge loop through this edge.
                uint32_t edge = obj->mesh().findEdge(cp.a, cp.b);
                float t = obj->mesh().edges[edge].v0 == cp.a ? cp.t : 1.0f - cp.t;
                auto plan = ops::planEdgeLoop(obj->mesh(), edge, t);
                if (!scene_->isSelected(obj->id)) scene_->selectObjects({obj->id}, SelectOp::Replace);
                if (scene_->mode() == SelectMode::Object) scene_->setMode(SelectMode::Edge);
                scene_->editMesh(obj, "Insert Edge Loop", [&](Mesh& m) -> std::optional<std::vector<uint32_t>> {
                    auto edges = ops::insertEdgeLoop(m, plan);
                    if (scene_->mode() == SelectMode::Edge) return edges;
                    return std::nullopt;
                });
                cut_ = {};
                return;
            }
            if (cut_.obj != obj) cut_ = {};
            cut_.obj = obj;
            if (cut_.pts.empty() || cut_.pts.back().a != cp.a || cut_.pts.back().b != cp.b || cut_.pts.back().t != cp.t) {
                cut_.pts.push_back(cp);
                cut_.world.push_back(world);
            }
            update();
        }
        return;
    }
    if (e->button() == Qt::LeftButton && tool == Tool::TargetWeld) {
        SceneObject* obj = nullptr;
        uint32_t v = kInvalid;
        if (!scene_->selectedObjects().empty()) v = pickVertex(slot, p, obj);
        if (v == kInvalid) {
            Hit h = pickSurface(slot, p, 6, false);
            if (h.obj && h.cageFace != kInvalid) {
                float best = 25.0f;
                Mat4 w = h.obj->worldMatrix();
                for (uint32_t fv : h.obj->mesh().faceVertices(h.cageFace)) {
                    Vec3 s = project(slot, w.transformPoint(h.obj->mesh().positions[fv]));
                    float d = (Vec2{s.x, s.y} - Vec2{float(p.x()), float(p.y())}).length();
                    if (d < best) { best = d; v = fv; obj = h.obj; }
                }
            }
        }
        if (obj && v != kInvalid) {
            weld_ = {obj, v, kInvalid};
            drag_ = Drag::Weld;
        }
        return;
    }
    if (e->button() == Qt::LeftButton && tool == Tool::InsertEdgeLoop) {
        updateLoopPreview(slot, p);
        if (loop_.obj && loop_.plan.valid()) drag_ = Drag::LoopSlide;
        return;
    }
    const bool xform = tool == Tool::Move || tool == Tool::Rotate || tool == Tool::Scale;
    if (xform && e->button() == Qt::LeftButton) {
        GizmoFrame f;
        if (gizmoFrame(slot, f)) {
            Handle h = hitGizmo(tool, f, gizmoView(slot), {float(p.x()), float(p.y())});
            if (h != Handle::None) { beginManip(slot, h, p); if (drag_ == Drag::Manip) return; }
        }
    }
    if (xform && e->button() == Qt::MiddleButton) {
        int ti = tool == Tool::Move ? 0 : tool == Tool::Rotate ? 1 : 2;
        beginManip(slot, lastHandle_[ti], p);
        return;
    }
    if (e->button() == Qt::LeftButton) drag_ = Drag::Marquee;
}

void Viewport::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint p = e->position().toPoint();
    const QPoint d = p - lastPos_;
    mousePos_ = p;
    Camera& cam = cams_[size_t(dragSlot_)];
    switch (drag_) {
    case Drag::Tumble: cam.tumble(float(d.x()), float(d.y())); update(); break;
    case Drag::Track: cam.track(float(d.x()), float(d.y()), float(slotRect(dragSlot_).height())); update(); break;
    case Drag::Dolly: cam.dolly(float(d.x() - d.y()) * 0.01f); update(); break;
    case Drag::Marquee: update(); break;
    case Drag::Manip: updateManip(p); break;
    case Drag::SoftRadius:
        scene_->softRadius = std::clamp(scene_->softRadius * std::exp(float(d.x()) * 0.01f), 1e-3f, 1e4f);
        scene_->notifySelectionChanged();
        break;
    case Drag::Menu: emit markingMenuTrack(e->globalPosition().toPoint()); break;
    case Drag::LoopSlide: updateLoopPreview(dragSlot_, p, true); break;
    case Drag::Weld: {
        weld_.dst = kInvalid;
        if (weld_.obj) {
            const Mesh& m = weld_.obj->mesh();
            Mat4 w = weld_.obj->worldMatrix();
            float best = 15.0f;
            for (uint32_t v = 0; v < m.numVerts(); ++v) {
                if (v == weld_.src) continue;
                Vec3 s = project(dragSlot_, w.transformPoint(m.positions[v]));
                float d = (Vec2{s.x, s.y} - Vec2{float(p.x()), float(p.y())}).length();
                if (d < best) { best = d; weld_.dst = v; }
            }
        }
        update();
        break;
    }
    case Drag::QuadVert: {
        SceneObject* t = quadTarget(false);
        Vec3 hit;
        if (t && liveHit(dragSlot_, p, hit) && qd_.dragVert < t->mesh().numVerts()) {
            t->st.mesh->positions[qd_.dragVert] = t->worldMatrix().inverse().transformPoint(hit);
            t->st.mesh->touchPositions();
            scene_->notifyChanged();
        }
        break;
    }
    case Drag::QuadShift:
        if ((p - qd_.pressPos).manhattanLength() > 5) {
            drag_ = Drag::QuadRelax;
            scene_->dragging = true;
            quadRelax(dragSlot_, p);
        }
        break;
    case Drag::QuadRelax: quadRelax(dragSlot_, p); update(); break;
    case Drag::QuadExtend: update(); break;
    case Drag::Cut: break;
    case Drag::Sculpt: sculptMove(p); update(); break;
    case Drag::BrushRadius:
        scene_->sculpt.radiusPx = std::clamp(scene_->sculpt.radiusPx * std::exp(float(d.x()) * 0.01f), 3.0f, 2000.0f);
        update();
        break;
    case Drag::BrushStrength:
        scene_->sculpt.strength = std::clamp(scene_->sculpt.strength + float(d.x()) * 0.005f, 0.01f, 1.0f);
        update();
        break;
    case Drag::None: {
        int slot = slotAt(p);
        Tool t = scene_->tool();
        if (t == Tool::Move || t == Tool::Rotate || t == Tool::Scale) {
            GizmoFrame f;
            Handle h = gizmoFrame(slot, f) ? hitGizmo(t, f, gizmoView(slot), {float(p.x()), float(p.y())}) : Handle::None;
            if (h != hover_) { hover_ = h; update(); }
            if (h != Handle::None) break;
        }
        if (t == Tool::Sculpt) { update(); break; }
        if (t == Tool::InsertEdgeLoop) updateLoopPreview(slot, p);
        else if (t == Tool::QuadDraw) updateQuadHover(slot, p, e->modifiers() & Qt::ShiftModifier);
        else if (t == Tool::MultiCut) {
            SceneObject* obj = nullptr;
            cut_.hoverValid = cutPointAt(slot, p, obj, cut_.hover, cut_.hoverWorld) && (!cut_.obj || obj == cut_.obj);
            update();
        } else updateHover(slot, p);
        break;
    }
    }
    lastPos_ = p;
}

void Viewport::mouseReleaseEvent(QMouseEvent* e)
{
    const QPoint p = e->position().toPoint();
    switch (drag_) {
    case Drag::Marquee: {
        if ((p - pressPos_).manhattanLength() < 4) clickSelect(dragSlot_, p, opFromModifiers(pressMods_));
        else marqueeSelect(dragSlot_, QRect(pressPos_, p), opFromModifiers(pressMods_));
        drag_ = Drag::None;
        update();
        break;
    }
    case Drag::Manip: endManip(); break;
    case Drag::Sculpt: sculptRelease(); update(); break;
    case Drag::BrushRadius:
    case Drag::BrushStrength: drag_ = Drag::None; update(); break;
    case Drag::Weld: {
        drag_ = Drag::None;
        if (weld_.obj && weld_.src != kInvalid && weld_.dst != kInvalid) {
            uint32_t src = weld_.src, dst = weld_.dst;
            scene_->editMesh(weld_.obj, "Target Weld", [&](Mesh& m) -> std::optional<std::vector<uint32_t>> {
                ops::mergeInto(m, src, dst);
                return std::nullopt;
            });
            emit message("Target Weld");
        }
        weld_ = {};
        update();
        break;
    }
    case Drag::QuadVert:
    case Drag::QuadRelax:
    case Drag::QuadShift:
    case Drag::QuadExtend:
        quadRelease(p);
        break;
    case Drag::Menu:
        drag_ = Drag::None;
        emit markingMenuRelease(e->globalPosition().toPoint());
        break;
    case Drag::LoopSlide: {
        drag_ = Drag::None;
        if (loop_.obj && loop_.plan.valid()) {
            SceneObject* obj = loop_.obj;
            auto plan = loop_.plan;
            if (!scene_->isSelected(obj->id)) scene_->selectObjects({obj->id}, SelectOp::Replace);
            if (scene_->mode() == SelectMode::Object) scene_->setMode(SelectMode::Edge);
            scene_->editMesh(obj, "Insert Edge Loop", [&](Mesh& m) -> std::optional<std::vector<uint32_t>> {
                auto edges = ops::insertEdgeLoop(m, plan);
                if (scene_->mode() == SelectMode::Edge) return edges;
                return std::nullopt;
            });
            loop_ = {};
            updateLoopPreview(dragSlot_, p);
        }
        break;
    }
    default:
        if (e->buttons() == Qt::NoButton) drag_ = Drag::None;
        break;
    }
}

void Viewport::mouseDoubleClickEvent(QMouseEvent* e)
{
    // Double-click an edge selects its loop (Shift adds), as in Maya.
    if (e->button() == Qt::LeftButton && scene_->mode() == SelectMode::Edge && !(e->modifiers() & Qt::AltModifier)) {
        SceneObject* obj = nullptr;
        uint32_t edge = pickEdge(slotAt(e->position().toPoint()), e->position().toPoint(), true, obj);
        if (obj && edge != kInvalid) {
            auto loop = ops::edgeLoop(obj->mesh(), edge);
            scene_->selectComponents(obj, loop, (e->modifiers() & Qt::ShiftModifier) ? SelectOp::Add : SelectOp::Replace);
        }
        drag_ = Drag::None;
        return;
    }
    mousePressEvent(e);
}

void Viewport::wheelEvent(QWheelEvent* e)
{
    int slot = slotAt(e->position().toPoint());
    cams_[size_t(slot)].dolly(float(e->angleDelta().y()) / 120.0f * 0.12f);
    update();
}

bool Viewport::event(QEvent* e)
{
    // Keys the active tool needs that would otherwise be eaten by focus chaining or menu shortcuts.
    if (e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress || e->type() == QEvent::KeyRelease) {
        auto* ke = static_cast<QKeyEvent*>(e);
        const int k = ke->key();
        if (k == Qt::Key_Tab && scene_->tool() == Tool::QuadDraw) {
            if (e->type() == QEvent::KeyPress && !ke->isAutoRepeat()) tabHeld_ = true;
            if (e->type() == QEvent::KeyRelease && !ke->isAutoRepeat()) tabHeld_ = false;
            e->accept();
            return true;
        }
        if (scene_->tool() == Tool::MultiCut && (k == Qt::Key_Return || k == Qt::Key_Enter || k == Qt::Key_Escape || k == Qt::Key_Backspace)) {
            if (e->type() == QEvent::ShortcutOverride) { e->accept(); return true; }
            if (e->type() == QEvent::KeyPress) {
                if (k == Qt::Key_Escape) cut_ = {};
                else if (k == Qt::Key_Backspace) {
                    if (!cut_.pts.empty()) { cut_.pts.pop_back(); cut_.world.pop_back(); }
                    if (cut_.pts.empty()) cut_.obj = nullptr;
                } else commitCut();
                update();
                return true;
            }
        }
        if (e->type() == QEvent::KeyRelease && k == Qt::Key_Shift && scene_->tool() == Tool::QuadDraw) {
            qd_.preview.clear();
            update();
        }
        if (e->type() == QEvent::KeyPress && k == Qt::Key_Shift && scene_->tool() == Tool::QuadDraw) {
            QOpenGLWidget::event(e);
            updateQuadHover(slotAt(mousePos_), mousePos_, true);
            return true;
        }
    }
    return QOpenGLWidget::event(e);
}

void Viewport::keyPressEvent(QKeyEvent* e)
{
    if (e->isAutoRepeat()) { e->accept(); return; }
    switch (e->key()) {
    case Qt::Key_Space:
        spaceHeld_ = true;
        hotboxShown_ = false;
        spaceTimer_.start();
        hotboxTimer_.start();
        return;
    case Qt::Key_B:
        if (e->modifiers() & Qt::AltModifier) { cycleBackground(); return; }
        bHeld_ = true;
        bUsed_ = false;
        return;
    case Qt::Key_X: xHeld_ = true; return;
    case Qt::Key_J: jHeld_ = true; return;
    case Qt::Key_M:
        if (scene_->tool() == Tool::Sculpt) { mHeld_ = true; return; }
        break;
    default: break;
    }
    QOpenGLWidget::keyPressEvent(e);
}

void Viewport::keyReleaseEvent(QKeyEvent* e)
{
    if (e->isAutoRepeat()) { e->accept(); return; }
    switch (e->key()) {
    case Qt::Key_Space: {
        spaceHeld_ = false;
        hotboxTimer_.stop();
        if (hotboxShown_) {
            hotboxShown_ = false;
            emit hotboxDismissed();
        } else if (spaceTimer_.elapsed() < 300) {
            toggleQuadView();
        }
        return;
    }
    case Qt::Key_B:
        bHeld_ = false;
        if (!bUsed_ && scene_->tool() != Tool::Sculpt) {
            scene_->softSelect = !scene_->softSelect;
            emit message(scene_->softSelect ? "Soft Select on (hold B + drag to resize)" : "Soft Select off");
            scene_->notifySelectionChanged();
        }
        return;
    case Qt::Key_X: xHeld_ = false; return;
    case Qt::Key_J: jHeld_ = false; return;
    case Qt::Key_M: mHeld_ = false; return;
    default: break;
    }
    QOpenGLWidget::keyReleaseEvent(e);
}

void Viewport::enterEvent(QEnterEvent* e)
{
    // Maya routes hotkeys to the panel under the cursor.
    QWidget* fw = QApplication::focusWidget();
    if (!qobject_cast<QLineEdit*>(fw)) setFocus();
    QOpenGLWidget::enterEvent(e);
}

void Viewport::leaveEvent(QEvent* e)
{
    hover_ = Handle::None;
    hoverComp_ = {};
    if (scene_->tool() == Tool::InsertEdgeLoop && drag_ == Drag::None) loop_ = {};
    update();
    QOpenGLWidget::leaveEvent(e);
}

void Viewport::focusOutEvent(QFocusEvent* e)
{
    bHeld_ = xHeld_ = jHeld_ = mHeld_ = false;
    if (spaceHeld_) {
        spaceHeld_ = false;
        hotboxTimer_.stop();
    }
    QOpenGLWidget::focusOutEvent(e);
}

} // namespace facet
