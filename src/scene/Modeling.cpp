#include "scene/Modeling.h"
#include "scene/Commands.h"
#include "mesh/ObjIO.h"
#include "mesh/Primitives.h"

#include <QFileInfo>

namespace facet::modeling {

namespace {

std::vector<SceneObject*> hilited(Scene& s)
{
    std::vector<SceneObject*> out;
    for (SceneObject* o : s.selectedObjects())
        if (o->st.visible) out.push_back(o);
    return out;
}

std::unique_ptr<SceneObject> makeObject(Scene& s, const QString& name, Mesh mesh)
{
    auto o = std::make_unique<SceneObject>();
    o->id = s.allocateId();
    o->st.name = s.uniqueName(name);
    o->st.mesh = std::make_shared<Mesh>(std::move(mesh));
    return o;
}

QString shortName(const QString& type)
{
    if (type == "polyCube") return "pCube";
    if (type == "polySphere") return "pSphere";
    if (type == "polyCylinder") return "pCylinder";
    if (type == "polyCone") return "pCone";
    if (type == "polyPlane") return "pPlane";
    if (type == "polyTorus") return "pTorus";
    return "pMesh";
}

// Runs a component-level op on each hilited object that has a selection in the current mode.
template <class Fn>
int forEachComponentEdit(Scene& s, const QString& label, Fn fn)
{
    int n = 0;
    s.undo.beginMacro(label);
    for (SceneObject* o : hilited(s)) {
        auto items = o->compList(s.mode());
        if (items.empty()) continue;
        if (s.editMesh(o, label, [&](Mesh& m) { return fn(m, items, *o); })) ++n;
    }
    s.undo.endMacro();
    return n;
}

} // namespace

QString createPrimitive(Scene& s, const QString& type)
{
    PrimitiveParams p = defaultPrimitive(type);
    auto o = makeObject(s, shortName(type) + "1", buildPrimitive(p));
    o->st.history = p;
    QString name = o->st.name;
    std::vector<std::unique_ptr<SceneObject>> v;
    v.push_back(std::move(o));
    if (s.mode() != SelectMode::Object) s.setMode(SelectMode::Object);
    s.undo.push(new AddRemoveCommand(&s, std::move(v), "Create " + name));
    return "Created " + name;
}

QString deleteSelection(Scene& s)
{
    if (s.mode() == SelectMode::Object) {
        auto ids = s.selection();
        if (ids.empty()) return "Nothing selected";
        s.undo.push(new AddRemoveCommand(&s, ids, "Delete"));
        return QString("Deleted %1 object(s)").arg(ids.size());
    }
    const SelectMode mode = s.mode();
    int n = forEachComponentEdit(s, "Delete Components", [mode](Mesh& m, const std::vector<uint32_t>& items, SceneObject&) {
        if (mode == SelectMode::Face) ops::deleteFaces(m, items);
        else if (mode == SelectMode::Edge) ops::dissolveEdges(m, items, true);
        else ops::deleteVertices(m, items);
        return std::optional<std::vector<uint32_t>>(std::vector<uint32_t>{});
    });
    return n ? "Deleted components" : "Select components to delete";
}

QString duplicate(Scene& s)
{
    if (s.mode() != SelectMode::Object) return "Duplicate works on objects (F8 to object mode)";
    std::vector<std::unique_ptr<SceneObject>> copies;
    for (SceneObject* o : s.selectedObjects()) {
        auto c = std::make_unique<SceneObject>();
        c->id = s.allocateId();
        c->st = o->st;
        c->st.mesh = std::make_shared<Mesh>(*o->st.mesh); // deep copy: in-place edits must not alias
        c->st.name = s.uniqueName(o->st.name);
        copies.push_back(std::move(c));
    }
    if (copies.empty()) return "Nothing selected";
    size_t n = copies.size();
    s.undo.push(new AddRemoveCommand(&s, std::move(copies), "Duplicate"));
    return QString("Duplicated %1 object(s)").arg(n);
}

QString extrude(Scene& s, bool keepFacesTogether)
{
    if (s.mode() != SelectMode::Face) return "Extrude: select faces (F11)";
    int n = forEachComponentEdit(s, "Extrude", [&](Mesh& m, const std::vector<uint32_t>& faces, SceneObject&) {
        return std::optional<std::vector<uint32_t>>(ops::extrudeFaces(m, faces, keepFacesTogether, 0.0f));
    });
    if (!n) return "Extrude: select faces";
    s.moveOrient = AxisOrient::Normal;
    s.setTool(Tool::Move);
    return "Extruded — drag the green (normal) axis to pull the faces out";
}

QString mergeVertices(Scene& s, float threshold)
{
    if (s.mode() == SelectMode::Object) return "Merge: select vertices, edges or faces";
    const ops::Comp ct = s.compType();
    uint32_t removed = 0;
    s.undo.beginMacro("Merge");
    for (SceneObject* o : hilited(s)) {
        auto verts = ops::convert(o->mesh(), ct, o->compList(s.mode()), ops::Comp::Vertex);
        if (verts.empty()) continue;
        s.editMesh(o, "Merge", [&](Mesh& m) {
            removed += ops::mergeByDistance(m, verts, threshold);
            return std::optional<std::vector<uint32_t>>(std::vector<uint32_t>{});
        });
    }
    s.undo.endMacro();
    return QString("Merged %1 vertices (threshold %2)").arg(removed).arg(threshold);
}

QString mergeToCenter(Scene& s)
{
    if (s.mode() == SelectMode::Object) return "Merge to Center: select components";
    const ops::Comp ct = s.compType();
    s.undo.beginMacro("Merge to Center");
    for (SceneObject* o : hilited(s)) {
        auto verts = ops::convert(o->mesh(), ct, o->compList(s.mode()), ops::Comp::Vertex);
        if (verts.size() < 2) continue;
        s.editMesh(o, "Merge to Center", [&](Mesh& m) {
            ops::mergeToCenter(m, verts);
            return std::optional<std::vector<uint32_t>>(std::vector<uint32_t>{});
        });
    }
    s.undo.endMacro();
    return "Merged to center";
}

QString collapse(Scene& s)
{
    if (s.mode() == SelectMode::Object) return "Collapse: select edges or faces";
    const ops::Comp ct = s.compType();
    s.undo.beginMacro("Collapse");
    for (SceneObject* o : hilited(s)) {
        auto items = o->compList(s.mode());
        if (items.empty()) continue;
        s.editMesh(o, "Collapse", [&](Mesh& m) {
            if (ct == ops::Comp::Face) {
                // Collapse each face to its centre: union all edges of each face.
                ops::collapseEdges(m, ops::convert(m, ct, items, ops::Comp::Edge));
            } else if (ct == ops::Comp::Edge) {
                ops::collapseEdges(m, items);
            } else {
                ops::mergeToCenter(m, items);
            }
            return std::optional<std::vector<uint32_t>>(std::vector<uint32_t>{});
        });
    }
    s.undo.endMacro();
    return "Collapsed";
}

QString fillHole(Scene& s)
{
    int total = 0;
    s.undo.beginMacro("Fill Hole");
    for (SceneObject* o : hilited(s)) {
        std::vector<uint32_t> edges;
        if (s.mode() != SelectMode::Object) {
            edges = ops::convert(o->mesh(), s.compType(), o->compList(s.mode()), ops::Comp::Edge);
            if (edges.empty()) continue;
        }
        s.editMesh(o, "Fill Hole", [&](Mesh& m) {
            auto faces = ops::fillHoles(m, edges);
            total += int(faces.size());
            if (s.mode() == SelectMode::Face) return std::optional<std::vector<uint32_t>>(faces);
            return std::optional<std::vector<uint32_t>>(std::vector<uint32_t>{});
        });
    }
    s.undo.endMacro();
    return QString("Filled %1 hole(s)").arg(total);
}

QString smooth(Scene& s, int levels)
{
    auto objs = hilited(s);
    if (objs.empty()) return "Smooth: select objects";
    s.undo.beginMacro("Smooth");
    for (SceneObject* o : objs)
        s.editMesh(o, "Smooth", [&](Mesh& m) {
            m = subdivide(m, levels);
            return std::optional<std::vector<uint32_t>>(std::vector<uint32_t>{});
        });
    s.undo.endMacro();
    if (s.mode() != SelectMode::Object) s.clearComponents();
    return QString("Smoothed %1 object(s)").arg(objs.size());
}

QString triangulate(Scene& s)
{
    s.undo.beginMacro("Triangulate");
    for (SceneObject* o : hilited(s)) {
        std::vector<uint32_t> faces;
        if (s.mode() != SelectMode::Object) faces = ops::convert(o->mesh(), s.compType(), o->compList(s.mode()), ops::Comp::Face);
        if (s.mode() != SelectMode::Object && faces.empty()) continue;
        s.editMesh(o, "Triangulate", [&](Mesh& m) {
            ops::triangulate(m, faces);
            return std::optional<std::vector<uint32_t>>(std::vector<uint32_t>{});
        });
    }
    s.undo.endMacro();
    return "Triangulated";
}

QString reverseNormals(Scene& s)
{
    s.undo.beginMacro("Reverse");
    for (SceneObject* o : hilited(s)) {
        std::vector<uint32_t> faces;
        if (s.mode() == SelectMode::Face) faces = o->sel.faces;
        s.editMesh(o, "Reverse", [&](Mesh& m) {
            ops::reverseFaces(m, faces);
            return std::optional<std::vector<uint32_t>>(faces);
        }, true);
    }
    s.undo.endMacro();
    return "Reversed normals";
}

QString mirror(Scene& s, int axis)
{
    auto objs = hilited(s);
    if (objs.empty()) return "Mirror: select objects";
    s.undo.beginMacro("Mirror");
    for (SceneObject* o : objs)
        s.editMesh(o, "Mirror", [&](Mesh& m) {
            float size = length(m.bounds().size());
            ops::mirror(m, axis, std::max(1e-5f, size * 1e-4f));
            return std::optional<std::vector<uint32_t>>(std::vector<uint32_t>{});
        });
    s.undo.endMacro();
    return QString("Mirrored across local %1 = 0").arg(QChar("XYZ"[axis]));
}

QString combine(Scene& s)
{
    auto objs = s.selectedObjects();
    if (objs.size() < 2) return "Combine: select two or more objects";
    Mesh combined;
    for (SceneObject* o : objs) combined.append(o->mesh(), o->worldMatrix());
    combined.buildTopology();
    auto obj = makeObject(s, "polySurface1", std::move(combined));
    std::vector<uint32_t> ids;
    for (SceneObject* o : objs) ids.push_back(o->id);
    s.undo.beginMacro("Combine");
    s.undo.push(new AddRemoveCommand(&s, ids, "Remove"));
    std::vector<std::unique_ptr<SceneObject>> v;
    v.push_back(std::move(obj));
    s.undo.push(new AddRemoveCommand(&s, std::move(v), "Add"));
    s.undo.endMacro();
    return QString("Combined %1 objects").arg(objs.size());
}

QString separate(Scene& s)
{
    SceneObject* o = s.lead();
    if (!o) return "Separate: select an object";
    auto parts = ops::separate(o->mesh());
    if (parts.size() < 2) return "Separate: mesh has a single shell";
    std::vector<std::unique_ptr<SceneObject>> v;
    for (auto& part : parts) {
        auto c = makeObject(s, "polySurface1", std::move(part));
        c->st.translate = o->st.translate;
        c->st.rotate = o->st.rotate;
        c->st.scale = o->st.scale;
        c->st.smoothDisplay = o->st.smoothDisplay;
        c->st.smoothLevel = o->st.smoothLevel;
        v.push_back(std::move(c));
    }
    size_t n = v.size();
    if (s.mode() != SelectMode::Object) s.setMode(SelectMode::Object);
    s.undo.beginMacro("Separate");
    s.undo.push(new AddRemoveCommand(&s, std::vector<uint32_t>{o->id}, "Remove"));
    s.undo.push(new AddRemoveCommand(&s, std::move(v), "Add"));
    s.undo.endMacro();
    return QString("Separated into %1 objects").arg(n);
}

QString setCrease(Scene& s, float value)
{
    if (s.mode() == SelectMode::Object) return "Crease: select edges (F10)";
    int edgesTotal = 0;
    s.undo.beginMacro("Crease");
    for (SceneObject* o : hilited(s)) {
        auto edges = ops::convert(o->mesh(), s.compType(), o->compList(s.mode()), ops::Comp::Edge);
        if (edges.empty()) continue;
        edgesTotal += int(edges.size());
        auto keep = o->compList(s.mode());
        s.editMesh(o, "Crease", [&](Mesh& m) {
            for (uint32_t e : edges) m.edgeCrease[e] = value;
            m.topologyId = nextVersionId(); // creases change the subdivision rules
            m.touchPositions();
            return std::optional<std::vector<uint32_t>>(keep);
        });
    }
    s.undo.endMacro();
    if (!edgesTotal) return "Crease: select edges";
    return value <= 0 ? QString("Removed crease from %1 edges").arg(edgesTotal)
                      : QString("Creased %1 edges (sharpness %2)").arg(edgesTotal).arg(value >= 10 ? QString("full") : QString::number(value));
}

QString freezeTransforms(Scene& s)
{
    auto objs = s.selectedObjects();
    if (objs.empty()) return "Freeze: select objects";
    s.undo.beginMacro("Freeze Transformations");
    for (SceneObject* o : objs) {
        Mat4 w = o->worldMatrix();
        ObjectState before = o->st;
        s.editMesh(o, "Freeze", [&](Mesh& m) {
            for (auto& p : m.positions) p = w.transformPoint(p);
            if (w.at(0, 0) * w.at(1, 1) * w.at(2, 2) < 0) ops::reverseFaces(m); // keep normals outward after negative scale
            m.touchPositions();
            return std::optional<std::vector<uint32_t>>(std::nullopt);
        });
        ObjectState b2 = o->st;
        o->st.translate = {};
        o->st.rotate = {};
        o->st.scale = {1, 1, 1};
        s.pushState(o, b2, "Reset");
    }
    s.undo.endMacro();
    return "Froze transformations";
}

QString centerPivot(Scene& s)
{
    auto objs = s.selectedObjects();
    if (objs.empty()) return "Center Pivot: select objects";
    s.undo.beginMacro("Center Pivot");
    for (SceneObject* o : objs) {
        Vec3 c = o->mesh().bounds().center();
        Vec3 offset = (Mat4::rotateXYZ(o->st.rotate) * Mat4::scale(o->st.scale)).transformDir(c);
        s.editMesh(o, "Center Pivot", [&](Mesh& m) {
            for (auto& p : m.positions) p -= c;
            m.touchPositions();
            return std::optional<std::vector<uint32_t>>(std::nullopt);
        });
        ObjectState b = o->st;
        o->st.translate += offset;
        s.pushState(o, b, "Move pivot");
    }
    s.undo.endMacro();
    return "Centered pivot";
}

QString resetTransforms(Scene& s)
{
    std::vector<std::pair<uint32_t, ObjectState>> before;
    for (SceneObject* o : s.selectedObjects()) {
        before.emplace_back(o->id, o->st);
        o->st.translate = {};
        o->st.rotate = {};
        o->st.scale = {1, 1, 1};
    }
    s.pushStates(before, "Reset Transformations");
    return "Reset transformations";
}

QString deleteHistory(Scene& s)
{
    std::vector<std::pair<uint32_t, ObjectState>> before;
    for (SceneObject* o : s.selectedObjects()) {
        before.emplace_back(o->id, o->st);
        o->st.history.reset();
    }
    s.pushStates(before, "Delete History");
    return "Deleted history";
}

QString setSmoothDisplay(Scene& s, int mode)
{
    std::vector<std::pair<uint32_t, ObjectState>> before;
    for (SceneObject* o : s.selectedObjects()) {
        before.emplace_back(o->id, o->st);
        o->st.smoothDisplay = mode;
    }
    if (before.empty()) return "Smooth preview: select objects";
    s.pushStates(before, "Smooth Preview");
    static const char* names[] = {"off", "cage + smooth", "smooth"};
    return QString("Smooth mesh preview: %1").arg(names[std::clamp(mode, 0, 2)]);
}

QString changeSmoothLevel(Scene& s, int delta)
{
    std::vector<std::pair<uint32_t, ObjectState>> before;
    int level = 0;
    for (SceneObject* o : s.selectedObjects()) {
        before.emplace_back(o->id, o->st);
        o->st.smoothLevel = std::clamp(o->st.smoothLevel + delta, 0, 5);
        if (o->st.smoothDisplay == 0) o->st.smoothDisplay = 2;
        level = o->st.smoothLevel;
    }
    if (before.empty()) return "Smooth level: select objects";
    s.pushStates(before, "Smooth Level");
    return QString("Smooth level %1").arg(level);
}

QString hideSelected(Scene& s)
{
    std::vector<std::pair<uint32_t, ObjectState>> before;
    for (SceneObject* o : s.selectedObjects()) {
        before.emplace_back(o->id, o->st);
        o->st.visible = false;
    }
    s.pushStates(before, "Hide");
    s.notifyObjectsChanged();
    return "Hidden";
}

QString showAll(Scene& s)
{
    std::vector<std::pair<uint32_t, ObjectState>> before;
    for (auto& o : s.objects)
        if (!o->st.visible) {
            before.emplace_back(o->id, o->st);
            o->st.visible = true;
        }
    s.pushStates(before, "Show All");
    s.notifyObjectsChanged();
    return "Showing all";
}

// ------------------------------------------------------------------------------------------
// Selection

QString selectAll(Scene& s)
{
    if (s.mode() == SelectMode::Object) {
        std::vector<uint32_t> ids;
        for (auto& o : s.objects)
            if (o->st.visible) ids.push_back(o->id);
        s.selectObjects(ids, SelectOp::Replace);
        return QString("Selected %1 objects").arg(ids.size());
    }
    for (SceneObject* o : hilited(s)) {
        const Mesh& m = o->mesh();
        uint32_t n = s.mode() == SelectMode::Vertex ? m.numVerts() : s.mode() == SelectMode::Edge ? m.numEdges() : m.numFaces();
        std::vector<uint32_t> all(n);
        for (uint32_t i = 0; i < n; ++i) all[i] = i;
        s.selectComponents(o, std::move(all), SelectOp::Replace, false);
    }
    s.notifySelectionChanged();
    return "Selected all";
}

QString invertSelection(Scene& s)
{
    if (s.mode() == SelectMode::Object) {
        std::vector<uint32_t> ids;
        for (auto& o : s.objects)
            if (o->st.visible && !s.isSelected(o->id)) ids.push_back(o->id);
        s.selectObjects(ids, SelectOp::Replace);
        return "Inverted selection";
    }
    for (SceneObject* o : hilited(s)) {
        const Mesh& m = o->mesh();
        uint32_t n = s.mode() == SelectMode::Vertex ? m.numVerts() : s.mode() == SelectMode::Edge ? m.numEdges() : m.numFaces();
        std::vector<uint32_t> all(n);
        for (uint32_t i = 0; i < n; ++i) all[i] = i;
        s.selectComponents(o, std::move(all), SelectOp::Toggle, false);
    }
    s.notifySelectionChanged();
    return "Inverted selection";
}

QString growSelection(Scene& s)
{
    if (s.mode() == SelectMode::Object) return "Grow: component mode only";
    for (SceneObject* o : hilited(s)) s.setComponents(o, s.mode(), ops::grow(o->mesh(), s.compType(), o->compList(s.mode())));
    return "Grew selection";
}

QString shrinkSelection(Scene& s)
{
    if (s.mode() == SelectMode::Object) return "Shrink: component mode only";
    for (SceneObject* o : hilited(s)) s.setComponents(o, s.mode(), ops::shrink(o->mesh(), s.compType(), o->compList(s.mode())));
    return "Shrank selection";
}

QString selectEdgeLoop(Scene& s)
{
    if (s.mode() != SelectMode::Edge) return "Edge loop: select an edge (F10), or double-click an edge";
    for (SceneObject* o : hilited(s)) {
        std::vector<uint32_t> out;
        for (uint32_t e : o->sel.edges) {
            auto l = ops::edgeLoop(o->mesh(), e);
            out.insert(out.end(), l.begin(), l.end());
        }
        s.setComponents(o, SelectMode::Edge, out);
    }
    return "Selected edge loop";
}

QString selectEdgeRing(Scene& s)
{
    if (s.mode() != SelectMode::Edge) return "Edge ring: select an edge (F10)";
    for (SceneObject* o : hilited(s)) {
        std::vector<uint32_t> out;
        for (uint32_t e : o->sel.edges) {
            auto l = ops::edgeRing(o->mesh(), e);
            out.insert(out.end(), l.begin(), l.end());
        }
        s.setComponents(o, SelectMode::Edge, out);
    }
    return "Selected edge ring";
}

QString convertSelection(Scene& s, SelectMode to)
{
    if (s.mode() == SelectMode::Object || to == SelectMode::Object) return setComponentMode(s, to);
    const ops::Comp from = s.compType();
    const ops::Comp target = to == SelectMode::Vertex ? ops::Comp::Vertex : to == SelectMode::Edge ? ops::Comp::Edge : ops::Comp::Face;
    std::vector<std::pair<SceneObject*, std::vector<uint32_t>>> conv;
    for (SceneObject* o : hilited(s)) conv.emplace_back(o, ops::convert(o->mesh(), from, o->compList(s.mode()), target));
    s.setMode(to);
    for (auto& [o, items] : conv) s.setComponents(o, to, items);
    return "Converted selection";
}

QString setComponentMode(Scene& s, SelectMode m)
{
    s.setMode(m);
    static const char* names[] = {"Object", "Vertex", "Edge", "Face"};
    if (m != SelectMode::Object && s.selectedObjects().empty()) return QString("%1 mode — select an object first (RMB marking menu also works)").arg(names[int(m)]);
    return QString("%1 mode").arg(names[int(m)]);
}

// ------------------------------------------------------------------------------------------

QString importObj(Scene& s, const QString& path)
{
    std::vector<NamedMesh> meshes;
    std::string err;
    if (!readObj(path.toStdString(), meshes, &err)) return "Import failed: " + QString::fromStdString(err);
    std::vector<std::unique_ptr<SceneObject>> v;
    for (auto& nm : meshes) {
        QString name = QString::fromStdString(nm.name);
        if (name.isEmpty() || name == "mesh") name = QFileInfo(path).baseName();
        v.push_back(makeObject(s, name + "1", std::move(nm.mesh)));
    }
    size_t n = v.size();
    if (s.mode() != SelectMode::Object) s.setMode(SelectMode::Object);
    s.undo.push(new AddRemoveCommand(&s, std::move(v), "Import OBJ"));
    return QString("Imported %1 mesh(es) from %2").arg(n).arg(QFileInfo(path).fileName());
}

QString exportObj(Scene& s, const QString& path, bool selectedOnly)
{
    std::vector<NamedMesh> out;
    for (auto& o : s.objects) {
        if (selectedOnly && !s.isSelected(o->id)) continue;
        out.push_back({o->st.name.toStdString(), o->mesh(), o->worldMatrix()});
    }
    if (out.empty()) return "Export: nothing to export";
    std::string err;
    if (!writeObj(path.toStdString(), out, &err)) return "Export failed: " + QString::fromStdString(err);
    return QString("Exported %1 mesh(es) to %2").arg(out.size()).arg(QFileInfo(path).fileName());
}

} // namespace facet::modeling
