#include "ui/Outliner.h"

#include <QHeaderView>

namespace facet {

Outliner::Outliner(Scene* scene, QWidget* parent) : QTreeWidget(parent), scene_(scene)
{
    setHeaderHidden(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setRootIsDecorated(false);
    setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    connect(scene_, &Scene::objectsChanged, this, &Outliner::rebuild);
    connect(scene_, &Scene::sceneChanged, this, [this] {
        // Names / visibility can change through undo.
        bool stale = topLevelItemCount() != int(scene_->objects.size());
        for (int i = 0; !stale && i < topLevelItemCount(); ++i) {
            SceneObject* o = scene_->objects[size_t(i)].get();
            stale = topLevelItem(i)->text(0) != o->st.name || topLevelItem(i)->data(0, Qt::UserRole).toUInt() != o->id ||
                    topLevelItem(i)->data(0, Qt::UserRole + 1).toBool() != o->st.visible;
        }
        if (stale) rebuild();
    });
    connect(scene_, &Scene::selectionChanged, this, &Outliner::syncSelectionFromScene);
    connect(this, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (syncing_) return;
        std::vector<uint32_t> ids;
        for (QTreeWidgetItem* it : selectedItems()) ids.push_back(it->data(0, Qt::UserRole).toUInt());
        // Keep the previous order so the lead object stays last.
        std::vector<uint32_t> ordered;
        for (uint32_t id : scene_->selection())
            if (std::find(ids.begin(), ids.end(), id) != ids.end()) ordered.push_back(id);
        for (uint32_t id : ids)
            if (std::find(ordered.begin(), ordered.end(), id) == ordered.end()) ordered.push_back(id);
        syncing_ = true;
        scene_->setSelection(ordered);
        syncing_ = false;
    });
    connect(this, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* it, int) {
        if (syncing_) return;
        SceneObject* o = scene_->find(it->data(0, Qt::UserRole).toUInt());
        QString name = it->text(0).trimmed();
        if (!o || name.isEmpty() || name == o->st.name) return;
        for (const auto& other : scene_->objects)
            if (other.get() != o && other->st.name == name) name = scene_->uniqueName(name);
        ObjectState before = o->st;
        o->st.name = name;
        scene_->pushState(o, before, "Rename");
        scene_->notifyObjectsChanged();
    });
    rebuild();
}

void Outliner::rebuild()
{
    syncing_ = true;
    clear();
    for (const auto& o : scene_->objects) {
        auto* it = new QTreeWidgetItem(this, QStringList{o->st.name});
        it->setData(0, Qt::UserRole, o->id);
        it->setData(0, Qt::UserRole + 1, o->st.visible);
        it->setFlags(it->flags() | Qt::ItemIsEditable);
        if (!o->st.visible) {
            it->setForeground(0, QColor(120, 120, 120));
            QFont f = it->font(0);
            f.setItalic(true);
            it->setFont(0, f);
        }
    }
    syncing_ = false;
    syncSelectionFromScene();
}

void Outliner::syncSelectionFromScene()
{
    if (syncing_) return;
    syncing_ = true;
    for (int i = 0; i < topLevelItemCount(); ++i) {
        QTreeWidgetItem* it = topLevelItem(i);
        it->setSelected(scene_->isSelected(it->data(0, Qt::UserRole).toUInt()));
    }
    syncing_ = false;
}

} // namespace facet
