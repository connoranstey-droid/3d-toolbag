#include "ui/UVEditor.h"
#include "mesh/UVOps.h"
#include "scene/Commands.h"
#include "scene/Modeling.h"

#include <QActionGroup>
#include <QKeyEvent>
#include <QPainter>
#include <QToolBar>
#include <QVBoxLayout>

#include <cmath>

namespace facet {

namespace M = modeling;

UVCanvas::UVCanvas(Scene* scene, QWidget* parent) : QWidget(parent), scene_(scene)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(240, 200);
    auto refresh = [this] { update(); };
    connect(scene_, &Scene::sceneChanged, this, refresh);
    connect(scene_, &Scene::selectionChanged, this, refresh);
}

QPointF UVCanvas::toScreen(Vec2 uv) const
{
    return {width() * 0.5 + (uv.x - center_.x) * zoom_, height() * 0.5 - (uv.y - center_.y) * zoom_};
}

Vec2 UVCanvas::toUV(QPointF p) const
{
    return {float(center_.x + (p.x() - width() * 0.5) / zoom_), float(center_.y - (p.y() - height() * 0.5) / zoom_)};
}

std::vector<SceneObject*> UVCanvas::objects() const
{
    std::vector<SceneObject*> out;
    for (SceneObject* o : scene_->selectedObjects())
        if (o->st.visible && o->mesh().hasUVs()) out.push_back(o);
    return out;
}

void UVCanvas::frame(bool selectionOnly)
{
    Vec2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
    bool any = false;
    for (SceneObject* o : objects()) {
        const Mesh& m = o->mesh();
        auto consider = [&](uint32_t u) {
            lo = {std::min(lo.x, m.uvs[u].x), std::min(lo.y, m.uvs[u].y)};
            hi = {std::max(hi.x, m.uvs[u].x), std::max(hi.y, m.uvs[u].y)};
            any = true;
        };
        if (selectionOnly && !o->sel.uvs.empty()) for (uint32_t u : o->sel.uvs) { if (u < m.uvs.size()) consider(u); }
        else if (!selectionOnly) for (uint32_t u = 0; u < m.uvs.size(); ++u) consider(u);
    }
    if (!any) { lo = {0, 0}; hi = {1, 1}; }
    center_ = (lo + hi) * 0.5f;
    float ext = std::max({hi.x - lo.x, hi.y - lo.y, 1e-4f});
    zoom_ = std::min(width(), height()) * 0.85f / ext;
    update();
}

void UVCanvas::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(42, 42, 44));
    // 0..1 tile and grid.
    QRectF tile(toScreen({0, 1}), toScreen({1, 0}));
    p.fillRect(tile, QColor(58, 58, 62));
    p.setPen(QPen(QColor(75, 75, 80), 1));
    for (int i = 1; i < 10; ++i) {
        float t = i / 10.0f;
        p.drawLine(toScreen({t, 0}), toScreen({t, 1}));
        p.drawLine(toScreen({0, t}), toScreen({1, t}));
    }
    p.setPen(QPen(QColor(120, 120, 128), 1));
    p.drawRect(tile);
    p.setPen(QColor(150, 150, 150));
    p.drawText(toScreen({0, 0}) + QPointF(-10, 14), "0");
    p.drawText(toScreen({1, 1}) + QPointF(4, -4), "1");

    p.setRenderHint(QPainter::Antialiasing, zoom_ < 4000);
    for (SceneObject* o : objects()) {
        const Mesh& m = o->mesh();
        // Faces selected in the viewport are shaded.
        if (scene_->mode() == SelectMode::Face && !o->sel.faces.empty()) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 120, 20, 70));
            for (uint32_t f : o->sel.faces) {
                if (f >= m.numFaces()) continue;
                QPolygonF poly;
                for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h) poly << toScreen(m.cornerUV(h));
                p.drawPolygon(poly);
            }
        }
        QVector<QLineF> lines, borders, selEdges;
        const bool heavy = m.numHalfEdges() > 600000;
        std::vector<uint8_t> edgeSel;
        if (scene_->mode() == SelectMode::Edge) {
            edgeSel.assign(m.numEdges(), 0);
            for (uint32_t e : o->sel.edges) if (e < m.numEdges()) edgeSel[e] = 1;
        }
        for (uint32_t h = 0; h < m.numHalfEdges(); ++h) {
            uint32_t e = m.heEdge[h];
            bool border = m.isUVBorderEdge(e);
            if (heavy && !border) continue;
            QLineF l(toScreen(m.cornerUV(h)), toScreen(m.cornerUV(m.heNext(h))));
            if (!edgeSel.empty() && edgeSel[e]) selEdges << l;
            else if (border) borders << l;
            else if (m.heTwin[h] == kInvalid || h < m.heTwin[h]) lines << l;
        }
        p.setPen(QPen(QColor(150, 175, 210), 1));
        p.drawLines(lines);
        p.setPen(QPen(QColor(225, 225, 235), 2));
        p.drawLines(borders);
        p.setPen(QPen(QColor(255, 140, 0), 2.5));
        p.drawLines(selEdges);
        // UV points.
        std::vector<uint8_t> sel(m.uvs.size(), 0);
        for (uint32_t u : o->sel.uvs) if (u < sel.size()) sel[u] = 1;
        p.setPen(Qt::NoPen);
        if (mode == Mode::UV && m.uvs.size() < 200000) {
            p.setBrush(QColor(170, 80, 200));
            for (uint32_t u = 0; u < m.uvs.size(); ++u)
                if (!sel[u]) p.drawRect(QRectF(toScreen(m.uvs[u]) - QPointF(1.5, 1.5), QSizeF(3, 3)));
        }
        p.setBrush(QColor(80, 255, 120));
        for (uint32_t u = 0; u < m.uvs.size(); ++u)
            if (sel[u]) p.drawRect(QRectF(toScreen(m.uvs[u]) - QPointF(2.5, 2.5), QSizeF(5, 5)));
    }
    if (drag_ == Drag::Marquee) {
        p.setPen(QPen(QColor(240, 240, 240), 1, Qt::DashLine));
        p.setBrush(QColor(255, 255, 255, 20));
        p.drawRect(QRect(press_, last_).normalized());
    }
    p.setPen(QColor(200, 200, 200));
    static const char* tools[] = {"Move (W)", "Rotate (E)", "Scale (R)"};
    p.drawText(8, height() - 8, QString("%1 · %2 · MMB-drag transforms · Alt+MMB pan · wheel zoom · F frame")
                                    .arg(mode == Mode::UV ? "UV" : "UV Shell", tools[int(xtool)]));
    if (objects().empty()) {
        p.setPen(QColor(220, 200, 120));
        p.drawText(rect(), Qt::AlignCenter, "Select a mesh with UVs (use a projection to create them)");
    }
}

void UVCanvas::select(QPointF pt, SelectOp op, bool marquee, QRectF rect)
{
    for (SceneObject* o : objects()) {
        const Mesh& m = o->mesh();
        std::vector<uint32_t> hit;
        if (marquee) {
            for (uint32_t u = 0; u < m.uvs.size(); ++u)
                if (rect.contains(toScreen(m.uvs[u]))) hit.push_back(u);
        } else if (mode == Mode::UV) {
            float best = 9.0f;
            uint32_t bu = kInvalid;
            for (uint32_t u = 0; u < m.uvs.size(); ++u) {
                QPointF s = toScreen(m.uvs[u]);
                float d = float(std::hypot(s.x() - pt.x(), s.y() - pt.y()));
                if (d < best) { best = d; bu = u; }
            }
            if (bu != kInvalid) hit.push_back(bu);
        } else {
            // Shell mode: the face under the cursor.
            Vec2 q = toUV(pt);
            for (uint32_t f = 0; f < m.numFaces() && hit.empty(); ++f) {
                bool inside = false;
                const uint32_t b = m.faceStart[f], n = m.faceSize(f);
                for (uint32_t i = 0, j = n - 1; i < n; j = i++) {
                    Vec2 a = m.cornerUV(b + i), c = m.cornerUV(b + j);
                    if ((a.y > q.y) != (c.y > q.y) && q.x < (c.x - a.x) * (q.y - a.y) / (c.y - a.y) + a.x) inside = !inside;
                }
                if (inside) hit.push_back(m.faceUVs[b]);
            }
        }
        if (mode == Mode::Shell) hit = uv::expandToShells(m, hit);
        std::sort(hit.begin(), hit.end());
        auto& list = o->sel.uvs;
        std::vector<uint32_t> out;
        switch (op) {
        case SelectOp::Replace: out = hit; break;
        case SelectOp::Add: std::set_union(list.begin(), list.end(), hit.begin(), hit.end(), std::back_inserter(out)); break;
        case SelectOp::Remove: std::set_difference(list.begin(), list.end(), hit.begin(), hit.end(), std::back_inserter(out)); break;
        case SelectOp::Toggle: std::set_symmetric_difference(list.begin(), list.end(), hit.begin(), hit.end(), std::back_inserter(out)); break;
        }
        list = std::move(out);
    }
    scene_->notifySelectionChanged();
}

void UVCanvas::beginXform(QPoint p)
{
    xform_.clear();
    Vec2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
    for (SceneObject* o : objects()) {
        if (o->sel.uvs.empty()) continue;
        XEntry e{o, o->st.mesh, {}, {}};
        for (uint32_t u : o->sel.uvs)
            if (u < o->mesh().uvs.size()) {
                e.idx.push_back(u);
                e.start.push_back(o->mesh().uvs[u]);
                lo = {std::min(lo.x, o->mesh().uvs[u].x), std::min(lo.y, o->mesh().uvs[u].y)};
                hi = {std::max(hi.x, o->mesh().uvs[u].x), std::max(hi.y, o->mesh().uvs[u].y)};
            }
        if (!e.idx.empty()) xform_.push_back(std::move(e));
    }
    if (xform_.empty()) return;
    pivot_ = (lo + hi) * 0.5f;
    press_ = p;
    drag_ = Drag::Xform;
}

void UVCanvas::applyXform(QPoint p)
{
    const float dx = float(p.x() - press_.x()), dy = float(p.y() - press_.y());
    for (XEntry& e : xform_) {
        for (size_t k = 0; k < e.idx.size(); ++k) {
            Vec2 s = e.start[k], out = s;
            if (xtool == XTool::Move) out = s + Vec2{dx / zoom_, -dy / zoom_};
            else if (xtool == XTool::Rotate) {
                float a = radians(-dx * 0.5f), c = std::cos(a), sn = std::sin(a);
                Vec2 d = s - pivot_;
                out = pivot_ + Vec2{c * d.x - sn * d.y, sn * d.x + c * d.y};
            } else {
                float k2 = std::exp(dx * 0.01f);
                out = pivot_ + (s - pivot_) * k2;
            }
            e.mesh->uvs[e.idx[k]] = out;
        }
        e.mesh->touchUVs();
    }
    scene_->notifyChanged();
}

void UVCanvas::endXform()
{
    std::vector<UVMoveCommand::Entry> entries;
    for (XEntry& e : xform_) {
        UVMoveCommand::Entry c{e.mesh, e.idx, e.start, {}};
        for (uint32_t u : e.idx) c.after.push_back(e.mesh->uvs[u]);
        entries.push_back(std::move(c));
    }
    if (!entries.empty()) scene_->undo.push(new UVMoveCommand(scene_, std::move(entries), "Transform UVs"));
    xform_.clear();
}

void UVCanvas::mousePressEvent(QMouseEvent* e)
{
    setFocus();
    press_ = last_ = e->position().toPoint();
    mods_ = e->modifiers();
    const bool alt = e->modifiers() & Qt::AltModifier;
    if (alt && e->button() == Qt::MiddleButton) { drag_ = Drag::Pan; return; }
    if (alt && e->button() == Qt::RightButton) { drag_ = Drag::Zoom; return; }
    if (e->button() == Qt::MiddleButton) { beginXform(press_); return; }
    if (e->button() == Qt::LeftButton) {
        // LMB on a selected uv (or inside a selected shell) drags it; otherwise select.
        for (SceneObject* o : objects())
            for (uint32_t u : o->sel.uvs) {
                if (u >= o->mesh().uvs.size()) continue;
                QPointF s = toScreen(o->mesh().uvs[u]);
                if (std::hypot(s.x() - press_.x(), s.y() - press_.y()) < 7 && !(e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
                    beginXform(press_);
                    return;
                }
            }
        drag_ = Drag::Marquee;
    }
}

void UVCanvas::mouseMoveEvent(QMouseEvent* e)
{
    QPoint p = e->position().toPoint();
    QPoint d = p - last_;
    switch (drag_) {
    case Drag::Pan: userMoved_ = true; center_ = center_ - Vec2{d.x() / zoom_, -d.y() / zoom_}; update(); break;
    case Drag::Zoom: zoom_ = std::clamp(zoom_ * std::exp(float(d.x() - d.y()) * 0.01f), 5.0f, 1e6f); update(); break;
    case Drag::Marquee: last_ = p; update(); break;
    case Drag::Xform: applyXform(p); break;
    default: break;
    }
    last_ = p;
}

void UVCanvas::mouseReleaseEvent(QMouseEvent* e)
{
    QPoint p = e->position().toPoint();
    if (drag_ == Drag::Marquee) {
        bool shift = mods_ & Qt::ShiftModifier, ctrl = mods_ & Qt::ControlModifier;
        SelectOp op = shift && ctrl ? SelectOp::Add : shift ? SelectOp::Toggle : ctrl ? SelectOp::Remove : SelectOp::Replace;
        if ((p - press_).manhattanLength() < 4) select(p, op, false);
        else select({}, op, true, QRectF(QRect(press_, p).normalized()));
    } else if (drag_ == Drag::Xform) {
        endXform();
    }
    drag_ = Drag::None;
    update();
}

void UVCanvas::wheelEvent(QWheelEvent* e)
{
    userMoved_ = true;
    Vec2 before = toUV(e->position());
    zoom_ = std::clamp(zoom_ * std::exp(float(e->angleDelta().y()) / 120.0f * 0.15f), 5.0f, 1e6f);
    Vec2 after = toUV(e->position());
    center_ = center_ + (before - after);
    update();
}

void UVCanvas::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (!userMoved_) frame(false); // keep the 0-1 tile framed until the user pans or zooms
}

bool UVCanvas::event(QEvent* e)
{
    // Keep W/E/R/Q/F/A local to the UV editor instead of the main-window shortcuts.
    if (e->type() == QEvent::ShortcutOverride) {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->modifiers() == Qt::NoModifier &&
            (k->key() == Qt::Key_W || k->key() == Qt::Key_E || k->key() == Qt::Key_R || k->key() == Qt::Key_Q || k->key() == Qt::Key_F || k->key() == Qt::Key_A)) {
            e->accept();
            return true;
        }
    }
    return QWidget::event(e);
}

void UVCanvas::keyPressEvent(QKeyEvent* e)
{
    switch (e->key()) {
    case Qt::Key_W: xtool = XTool::Move; break;
    case Qt::Key_E: xtool = XTool::Rotate; break;
    case Qt::Key_R: xtool = XTool::Scale; break;
    case Qt::Key_F: frame(true); break;
    case Qt::Key_A: frame(false); break;
    default: QWidget::keyPressEvent(e); return;
    }
    update();
}

// ------------------------------------------------------------------------------------------

UVEditor::UVEditor(Scene* scene, std::function<void(const QString&)> report, QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    auto* tb = new QToolBar;
    tb->setToolButtonStyle(Qt::ToolButtonTextOnly);
    canvas_ = new UVCanvas(scene, this);
    canvas_->report = report;
    lay->addWidget(tb);
    lay->addWidget(canvas_, 1);
    Scene& s = *scene;
    auto add = [&](const QString& label, const QString& tip, std::function<QString()> fn) {
        QAction* a = tb->addAction(label);
        a->setToolTip(tip);
        connect(a, &QAction::triggered, this, [fn, report, this] {
            if (report) report(fn());
            canvas_->update();
        });
        return a;
    };
    auto* modes = new QActionGroup(this);
    QAction* uvMode = tb->addAction("UV");
    QAction* shellMode = tb->addAction("Shell");
    for (QAction* a : {uvMode, shellMode}) { a->setCheckable(true); modes->addAction(a); }
    uvMode->setChecked(true);
    connect(uvMode, &QAction::triggered, this, [this] { canvas_->mode = UVCanvas::Mode::UV; canvas_->update(); });
    connect(shellMode, &QAction::triggered, this, [this] { canvas_->mode = UVCanvas::Mode::Shell; canvas_->update(); });
    tb->addSeparator();
    auto* tools = new QActionGroup(this);
    const char* tn[] = {"Move", "Rotate", "Scale"};
    for (int i = 0; i < 3; ++i) {
        QAction* a = tb->addAction(tn[i]);
        a->setCheckable(true);
        a->setChecked(i == 0);
        tools->addAction(a);
        connect(a, &QAction::triggered, this, [this, i] { canvas_->xtool = UVCanvas::XTool(i); canvas_->update(); });
    }
    tb->addSeparator();
    add("Planar", "Planar projection (best plane) of selected faces or the whole mesh", [&s] { return M::projectUVs(s, M::Projection::PlanarBest); });
    add("Cylindrical", "Cylindrical projection", [&s] { return M::projectUVs(s, M::Projection::Cylindrical); });
    add("Spherical", "Spherical projection", [&s] { return M::projectUVs(s, M::Projection::Spherical); });
    add("Automatic", "Automatic mapping (6 directions) + layout", [&s] { return M::projectUVs(s, M::Projection::Automatic); });
    tb->addSeparator();
    add("Cut", "Cut UVs along the selected edges (seams)", [&s] { return M::cutUVs(s); });
    add("Sew", "Move and sew UVs along the selected edges", [&s] { return M::sewUVs(s); });
    tb->addSeparator();
    add("Unfold", "LSCM unfold the selected shells (all if none)", [&s] { return M::unfoldUVs(s); });
    add("Layout", "Pack all shells into 0-1 with uniform texel density", [&s] { return M::layoutUVs(s); });
    add("Normalize", "Fit the selection into 0-1", [&s] { return M::normalizeUVs(s); });
    tb->addSeparator();
    add("Flip U", "", [&s] { return M::flipUVs(s, true); });
    add("Flip V", "", [&s] { return M::flipUVs(s, false); });
    add("Rot 90", "Rotate selected shells 90°", [&s] { return M::rotateUVs(s, 90.0f); });
    tb->addSeparator();
    add("Frame", "Frame all UVs (A); F frames the selection", [this] { canvas_->frame(false); return QString(); });
    checker_ = tb->addAction("Checker");
    checker_->setCheckable(true);
    checker_->setToolTip("Show a UV checker in the viewport (6)");
}

} // namespace facet
