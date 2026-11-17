#include "scene/Commands.h"

#include <algorithm>

namespace facet {

ObjectStateCommand::ObjectStateCommand(Scene* scene, std::vector<Entry> entries, const QString& text)
    : QUndoCommand(text), scene_(scene), entries_(std::move(entries))
{
}

void ObjectStateCommand::apply(bool after)
{
    bool renamed = false;
    for (auto& e : entries_) {
        SceneObject* o = scene_->find(e.id);
        if (!o) continue;
        const ObjectState& s = after ? e.after : e.before;
        renamed |= o->st.name != s.name;
        o->st = s;
        o->sel = after ? e.selAfter : e.selBefore;
    }
    if (renamed) scene_->notifyObjectsChanged();
    scene_->notifyChanged();
    scene_->notifySelectionChanged();
}

void ObjectStateCommand::undo() { apply(false); }
void ObjectStateCommand::redo() { apply(true); }

AddRemoveCommand::AddRemoveCommand(Scene* scene, std::vector<std::unique_ptr<SceneObject>> toAdd, const QString& text)
    : QUndoCommand(text), scene_(scene), isRemove_(false), held_(std::move(toAdd))
{
    for (auto& o : held_) ids_.push_back(o->id);
}

AddRemoveCommand::AddRemoveCommand(Scene* scene, std::vector<uint32_t> toRemove, const QString& text)
    : QUndoCommand(text), scene_(scene), isRemove_(true), ids_(std::move(toRemove))
{
}

void AddRemoveCommand::add()
{
    prevSelection_ = scene_->selection();
    for (size_t i = 0; i < held_.size(); ++i) {
        int idx = i < indices_.size() ? indices_[i] : -1;
        scene_->insertObject(std::move(held_[i]), idx);
    }
    held_.clear();
    if (!isRemove_) scene_->setSelection(ids_);
    scene_->notifyObjectsChanged();
}

void AddRemoveCommand::remove()
{
    if (isRemove_) prevSelection_ = scene_->selection();
    indices_.clear();
    held_.clear();
    // Remove in reverse index order so restoring by index is stable.
    std::vector<std::pair<int, std::unique_ptr<SceneObject>>> taken;
    for (uint32_t id : ids_) {
        int idx = -1;
        auto o = scene_->takeObject(id, &idx);
        if (o) taken.emplace_back(idx, std::move(o));
    }
    // Store in ascending index order for re-insertion.
    std::sort(taken.begin(), taken.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (auto& [idx, o] : taken) { indices_.push_back(idx); held_.push_back(std::move(o)); }
    ids_.clear();
    for (auto& o : held_) ids_.push_back(o->id);
    scene_->notifyObjectsChanged();
    scene_->notifySelectionChanged();
}

void AddRemoveCommand::redo()
{
    if (isRemove_) remove();
    else add();
}

void AddRemoveCommand::undo()
{
    if (isRemove_) {
        add();
        scene_->setSelection(prevSelection_);
    } else {
        remove();
        scene_->setSelection(prevSelection_);
    }
}

VertexMoveCommand::VertexMoveCommand(Scene* scene, std::vector<Entry> entries, const QString& text)
    : QUndoCommand(text), scene_(scene), entries_(std::move(entries))
{
}

void VertexMoveCommand::apply(bool after)
{
    for (auto& e : entries_) {
        const auto& src = after ? e.after : e.before;
        for (size_t i = 0; i < e.indices.size(); ++i)
            if (e.indices[i] < e.mesh->positions.size()) e.mesh->positions[e.indices[i]] = src[i];
        e.mesh->touchPositions();
    }
    scene_->notifyChanged();
}

UVMoveCommand::UVMoveCommand(Scene* scene, std::vector<Entry> entries, const QString& text)
    : QUndoCommand(text), scene_(scene), entries_(std::move(entries))
{
}

void UVMoveCommand::apply(bool after)
{
    for (auto& e : entries_) {
        const auto& src = after ? e.after : e.before;
        for (size_t i = 0; i < e.indices.size(); ++i)
            if (e.indices[i] < e.mesh->uvs.size()) e.mesh->uvs[e.indices[i]] = src[i];
        e.mesh->touchUVs();
    }
    scene_->notifyChanged();
}

void UVMoveCommand::undo() { apply(false); }
void UVMoveCommand::redo() { apply(true); }

void VertexMoveCommand::undo() { apply(false); }
void VertexMoveCommand::redo() { apply(true); }

} // namespace facet
