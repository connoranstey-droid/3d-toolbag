#include "ui/ChannelBox.h"

#include <QDoubleValidator>
#include <QFormLayout>
#include <QMouseEvent>
#include <QScrollArea>
#include <QVBoxLayout>

#include <cmath>

namespace facet {

void ScrubLabel::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton) {
        startX_ = int(e->position().x());
        dragging_ = true;
        if (onBegin) onBegin();
    }
}

void ScrubLabel::mouseMoveEvent(QMouseEvent* e)
{
    if (dragging_ && onDrag) onDrag(e->position().x() - startX_);
}

void ScrubLabel::mouseReleaseEvent(QMouseEvent*)
{
    if (dragging_ && onEnd) onEnd();
    dragging_ = false;
}

ChannelBox::ChannelBox(Scene* scene, QWidget* parent) : QWidget(parent), scene_(scene)
{
    root_ = new QVBoxLayout(this);
    root_->setContentsMargins(4, 4, 4, 4);
    nameEdit_ = new QLineEdit;
    nameEdit_->setPlaceholderText("(nothing selected)");
    QFont f = nameEdit_->font();
    f.setBold(true);
    nameEdit_->setFont(f);
    root_->addWidget(nameEdit_);
    connect(nameEdit_, &QLineEdit::editingFinished, this, [this] {
        SceneObject* o = scene_->lead();
        QString n = nameEdit_->text().trimmed();
        if (!o || n.isEmpty() || n == o->st.name) return;
        for (const auto& other : scene_->objects)
            if (other.get() != o && other->st.name == n) n = scene_->uniqueName(n);
        ObjectState before = o->st;
        o->st.name = n;
        scene_->pushState(o, before, "Rename");
        scene_->notifyObjectsChanged();
    });
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    body_ = new QWidget;
    form_ = new QFormLayout(body_);
    form_->setContentsMargins(0, 0, 0, 0);
    form_->setVerticalSpacing(2);
    form_->setLabelAlignment(Qt::AlignRight);
    scroll->setWidget(body_);
    root_->addWidget(scroll, 1);

    connect(scene_, &Scene::selectionChanged, this, &ChannelBox::rebuild);
    connect(scene_, &Scene::objectsChanged, this, &ChannelBox::rebuild);
    connect(scene_, &Scene::sceneChanged, this, [this] {
        if (structureKey() != key_) rebuild();
        else refresh();
    });
    rebuild();
}

QString ChannelBox::structureKey() const
{
    SceneObject* o = scene_->lead();
    if (!o) return {};
    QString k = QString::number(o->id) + ":" + QString::number(scene_->selection().size());
    if (o->st.history) {
        k += o->st.history->type;
        for (const auto& [n, v] : o->st.history->values) k += n;
    }
    return k;
}

void ChannelBox::addSection(const QString& title)
{
    auto* l = new QLabel(title);
    l->setStyleSheet("color:#9ab; font-weight:bold; padding-top:6px;");
    form_->addRow(l);
}

void ChannelBox::addChannel(Channel c)
{
    auto* label = new ScrubLabel(c.label);
    label->setCursor(Qt::SizeHorCursor);
    label->setToolTip("Drag to scrub (virtual slider)");
    auto* edit = new QLineEdit;
    edit->setValidator(new QDoubleValidator(edit));
    edit->setMaximumWidth(110);
    c.edit = edit;
    channels_.push_back(std::move(c));
    size_t idx = channels_.size() - 1;
    connect(edit, &QLineEdit::editingFinished, this, [this, idx] {
        bool ok = false;
        double v = channels_[idx].edit->text().toDouble(&ok);
        if (ok) apply(channels_[idx], v, true);
    });
    label->onBegin = [this] {
        scrubStart_.clear();
        for (SceneObject* o : scene_->selectedObjects()) scrubStart_.emplace_back(o->id, o->st);
    };
    label->onDrag = [this, idx](double px) {
        Channel& ch = channels_[idx];
        for (auto& [id, st0] : scrubStart_) {
            SceneObject* o = scene_->find(id);
            if (!o) continue;
            double v = ch.get(st0) + (ch.integer ? std::round(px / 8.0) : px * ch.step);
            ObjectState st = st0;
            ch.set(st, v);
            if (ch.regenerate && st.history) st.mesh = std::make_shared<Mesh>(buildPrimitive(*st.history));
            o->st = st;
        }
        if (channels_[idx].regenerate) scene_->clearComponents(false);
        scene_->notifyChanged();
    };
    label->onEnd = [this] {
        // Restore start states briefly so the undo command captures the right "before".
        std::vector<std::pair<uint32_t, ObjectState>> before = scrubStart_;
        scene_->pushStates(before, "Scrub channel");
        scrubStart_.clear();
    };
    form_->addRow(label, edit);
}

void ChannelBox::rebuild()
{
    key_ = structureKey();
    channels_.clear();
    while (form_->rowCount() > 0) form_->removeRow(0);
    SceneObject* lead = scene_->lead();
    nameEdit_->setEnabled(lead != nullptr);
    nameEdit_->setText(lead ? lead->st.name : QString());
    if (!lead) return;

    auto vecChan = [this](const QString& base, Vec3 ObjectState::*field, double step) {
        for (int i = 0; i < 3; ++i)
            addChannel({base + " " + QChar("XYZ"[i]), [field, i](const ObjectState& s) { return double((s.*field)[i]); },
                        [field, i](ObjectState& s, double v) { (s.*field)[i] = float(v); }, step});
    };
    vecChan("Translate", &ObjectState::translate, 0.01);
    vecChan("Rotate", &ObjectState::rotate, 0.5);
    vecChan("Scale", &ObjectState::scale, 0.01);
    addChannel({"Visibility", [](const ObjectState& s) { return s.visible ? 1.0 : 0.0; },
                [](ObjectState& s, double v) { s.visible = v > 0.5; }, 1, true});

    addSection("SHAPES  " + lead->st.name + "Shape");
    addChannel({"Smooth Level", [](const ObjectState& s) { return double(s.smoothLevel); },
                [](ObjectState& s, double v) { s.smoothLevel = std::clamp(int(std::lround(v)), 0, 5); }, 1, true});
    addChannel({"Smooth Display", [](const ObjectState& s) { return double(s.smoothDisplay); },
                [](ObjectState& s, double v) { s.smoothDisplay = std::clamp(int(std::lround(v)), 0, 2); }, 1, true});

    if (lead->st.history) {
        addSection("INPUTS  " + lead->st.history->type + "1");
        QString type = lead->st.history->type;
        for (const auto& [name, value] : lead->st.history->values) {
            QString n = name;
            bool integer = n.startsWith("subdivisions") || n == "caps";
            Channel c;
            c.label = n;
            c.get = [n](const ObjectState& s) { return s.history ? s.history->get(n) : 0.0; };
            c.set = [n, type, integer](ObjectState& s, double v) {
                if (!s.history || s.history->type != type) return;
                if (integer) v = std::max(n == "caps" ? 0.0 : 1.0, std::round(v));
                else v = std::max(1e-4, v);
                s.history->set(n, v);
            };
            c.step = integer ? 1 : 0.01;
            c.integer = integer;
            c.regenerate = true;
            addChannel(std::move(c));
        }
    }
    refresh();
}

void ChannelBox::refresh()
{
    SceneObject* lead = scene_->lead();
    if (!lead) return;
    if (!nameEdit_->hasFocus()) nameEdit_->setText(lead->st.name);
    for (Channel& c : channels_) {
        if (c.edit->hasFocus()) continue;
        double v = c.get(lead->st);
        c.edit->setText(c.integer ? QString::number(int(std::lround(v))) : QString::number(v, 'f', 3));
    }
}

void ChannelBox::apply(Channel& c, double value, bool undoable)
{
    std::vector<std::pair<uint32_t, ObjectState>> before;
    for (SceneObject* o : scene_->selectedObjects()) {
        if (std::abs(c.get(o->st) - value) < 1e-9) continue;
        before.emplace_back(o->id, o->st);
        c.set(o->st, value);
        if (c.regenerate && o->st.history) {
            o->st.mesh = std::make_shared<Mesh>(buildPrimitive(*o->st.history));
            o->sel = {};
        }
    }
    if (before.empty()) return;
    if (undoable) scene_->pushStates(before, "Set " + c.label);
    else scene_->notifyChanged();
}

} // namespace facet
