#pragma once
#include "mesh/Mesh.h"
#include "mesh/MeshOps.h"
#include "mesh/Subdivision.h"
#include "mesh/BVH.h"
#include "mesh/Sculpt.h"

#include <QObject>
#include <QString>
#include <QUndoStack>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace facet {

enum class SelectMode { Object, Vertex, Edge, Face };
enum class SelectOp { Replace, Toggle, Add, Remove };
enum class Tool { Select, Move, Rotate, Scale, InsertEdgeLoop, MultiCut, TargetWeld, QuadDraw, Sculpt };
enum class AxisOrient { World, Object, Normal };

// Editable construction history for primitives (Channel Box "INPUTS").
struct PrimitiveParams {
    QString type; // polyCube, polySphere, ...
    std::vector<std::pair<QString, double>> values;
    double get(const QString& k, double def = 0) const;
    void set(const QString& k, double v);
};
PrimitiveParams defaultPrimitive(const QString& type);
Mesh buildPrimitive(const PrimitiveParams& p);

// Everything about an object that undo/redo can restore.
struct ObjectState {
    QString name;
    Vec3 translate, rotate, scale{1, 1, 1};
    bool visible = true;
    std::shared_ptr<Mesh> mesh;
    int smoothDisplay = 0; // 0 = off (key 1), 1 = cage + smooth (key 2), 2 = smooth only (key 3)
    int smoothLevel = 2;
    std::optional<PrimitiveParams> history;
};

struct ComponentSelection {
    std::vector<uint32_t> verts, edges, faces;
    std::vector<uint32_t> uvs; // UV editor selection (uv indices)
    bool empty() const { return verts.empty() && edges.empty() && faces.empty(); }
};

class SceneObject {
public:
    uint32_t id = 0;
    ObjectState st;
    ComponentSelection sel;
    SubdivSurface subdiv;
    // Sculpt mask (per cage vertex, 1 = protected). Not part of undo; reset when topology changes.
    std::vector<float> mask;
    uint64_t maskVersion = 1;
    bool hasMask() const { return mask.size() == st.mesh->numVerts(); }

    Mat4 worldMatrix() const;
    Mesh& mesh() { return *st.mesh; }
    const Mesh& mesh() const { return *st.mesh; }
    // Mesh to draw: the cached subdivision surface while smooth preview is on, else the cage.
    const Mesh& displayMesh();
    // For each display face, the cage face it came from (nullptr when display == cage).
    const std::vector<uint32_t>* displayFaceOrigin() const;
    std::vector<uint32_t>& compList(SelectMode m);
};

class Scene : public QObject {
    Q_OBJECT
public:
    explicit Scene(QObject* parent = nullptr);
    ~Scene() override;

    std::vector<std::unique_ptr<SceneObject>> objects;
    QUndoStack undo;

    // ---- object management ----
    SceneObject* find(uint32_t id) const;
    SceneObject* lead() const; // last selected object
    std::vector<SceneObject*> selectedObjects() const;
    QString uniqueName(const QString& base) const;
    uint32_t allocateId() { return nextId_++; }
    // Low level (used by commands): insert/remove without undo.
    void insertObject(std::unique_ptr<SceneObject> obj, int index = -1);
    std::unique_ptr<SceneObject> takeObject(uint32_t id, int* indexOut = nullptr);
    void clear();

    // ---- selection ----
    SelectMode mode() const { return mode_; }
    void setMode(SelectMode m);
    const std::vector<uint32_t>& selection() const { return selection_; }
    bool isSelected(uint32_t id) const;
    void selectObjects(const std::vector<uint32_t>& ids, SelectOp op);
    void setSelection(const std::vector<uint32_t>& ids);
    void selectComponents(SceneObject* obj, std::vector<uint32_t> items, SelectOp op, bool notify = true);
    void clearComponents(bool notify = true);
    void setComponents(SceneObject* obj, SelectMode m, std::vector<uint32_t> items);
    bool hasComponentSelection() const;
    ops::Comp compType() const;
    // Vertices affected by the current component selection (cage indices).
    std::vector<uint32_t> selectedVerts(SceneObject* obj) const;

    // Soft selection
    bool softSelect = false;
    bool dragging = false; // true while a manipulator drag is live (renderer keeps cached weights)
    float softRadius = 1.0f;
    // Per-vertex weights (1 = selected) for an object; empty when nothing is affected.
    std::vector<float> softWeights(SceneObject* obj) const;

    // Symmetry (object space): -1 off, 0 = X, 1 = Y, 2 = Z. Component edits and selection are mirrored.
    int symmetry = -1;
    // For each vertex its mirror partner across the symmetry plane (kInvalid if none).
    std::vector<uint32_t> mirrorMap(SceneObject* obj, int axis) const;
    // Adds the mirror counterparts of `items` (in the current mode).
    std::vector<uint32_t> withMirrored(SceneObject* obj, const std::vector<uint32_t>& items) const;

    // Retopology: the "live" surface everything snaps to.
    uint32_t liveId = 0;
    SceneObject* liveObject() const { return liveId ? find(liveId) : nullptr; }
    // BVH of the live object's cage (local space), rebuilt lazily.
    const TriBVH* liveBVH();
    // Ray (world) onto the live surface. Returns false if it misses.
    bool rayToLive(const Ray& worldRay, Vec3& hitWorld, Vec3* normalWorld = nullptr);
    bool closestOnLive(const Vec3& world, Vec3& outWorld, float maxDist = 1e30f);

    struct SculptSettings {
        sculpt::Brush brush = sculpt::Brush::Draw;
        float radiusPx = 60.0f;
        float strength = 0.5f;
    } sculpt;

    // ---- tool state shared by UI and viewport ----
    Tool tool() const { return tool_; }
    void setTool(Tool t);
    AxisOrient moveOrient = AxisOrient::World;
    AxisOrient rotateOrient = AxisOrient::World;
    AxisOrient scaleOrient = AxisOrient::Object;

    // ---- editing helpers (all undoable) ----
    // Copies the object's mesh, runs fn, and pushes an undo step. fn returns the new component
    // selection (for the current mode) or std::nullopt to clear it. Returns false if fn failed.
    using MeshEditFn = std::function<std::optional<std::vector<uint32_t>>(Mesh&)>;
    bool editMesh(SceneObject* obj, const QString& label, const MeshEditFn& fn, bool keepHistory = false);
    void pushState(SceneObject* obj, const ObjectState& before, const QString& label);
    void pushStates(const std::vector<std::pair<uint32_t, ObjectState>>& before, const QString& label);

    // ---- change notification ----
    uint64_t selectionVersion() const { return selVersion_; }
    void notifyChanged();          // geometry / attributes
    void notifySelectionChanged();
    void notifyObjectsChanged();   // add / remove / rename / reorder

    // ---- file IO ----
    bool save(const QString& path, QString* error = nullptr) const;
    bool load(const QString& path, QString* error = nullptr);
    QString filePath;

signals:
    void sceneChanged();
    void selectionChanged();
    void objectsChanged();
    void toolChanged();
    void modeChanged();

private:
    SelectMode mode_ = SelectMode::Object;
    Tool tool_ = Tool::Select;
    std::vector<uint32_t> selection_;
    uint32_t nextId_ = 1;
    uint64_t selVersion_ = 1;
    TriBVH liveBvh_;
    uint32_t liveBvhFor_ = 0;
};

} // namespace facet
