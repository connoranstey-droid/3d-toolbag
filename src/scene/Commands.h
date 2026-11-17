#pragma once
#include "scene/Scene.h"

#include <QUndoCommand>

namespace facet {

// Restores a set of object states (transform, mesh, history, ...) plus component selections.
class ObjectStateCommand : public QUndoCommand {
public:
    struct Entry {
        uint32_t id;
        ObjectState before, after;
        ComponentSelection selBefore, selAfter;
    };
    ObjectStateCommand(Scene* scene, std::vector<Entry> entries, const QString& text);
    void undo() override;
    void redo() override;

private:
    void apply(bool after);
    Scene* scene_;
    std::vector<Entry> entries_;
    bool first_ = true;
};

// Adds (or, with remove=true, deletes) whole objects.
class AddRemoveCommand : public QUndoCommand {
public:
    AddRemoveCommand(Scene* scene, std::vector<std::unique_ptr<SceneObject>> toAdd, const QString& text);
    AddRemoveCommand(Scene* scene, std::vector<uint32_t> toRemove, const QString& text);
    void undo() override;
    void redo() override;

private:
    void add();
    void remove();
    Scene* scene_;
    bool isRemove_;
    std::vector<uint32_t> ids_;
    std::vector<int> indices_;
    std::vector<std::unique_ptr<SceneObject>> held_;
    std::vector<uint32_t> prevSelection_;
};

// In-place vertex position edits (component transforms, soft selection) without copying topology.
class VertexMoveCommand : public QUndoCommand {
public:
    struct Entry {
        uint32_t id;
        std::shared_ptr<Mesh> mesh;
        std::vector<uint32_t> indices;
        std::vector<Vec3> before, after;
    };
    VertexMoveCommand(Scene* scene, std::vector<Entry> entries, const QString& text);
    void undo() override;
    void redo() override;

private:
    void apply(bool after);
    Scene* scene_;
    std::vector<Entry> entries_;
    bool first_ = true;
};

} // namespace facet
