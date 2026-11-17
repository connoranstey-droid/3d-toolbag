#include "scene/Scene.h"
#include "scene/Commands.h"
#include "mesh/Primitives.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace facet {

// ------------------------------------------------------------------------------------------
// Primitives with construction history

double PrimitiveParams::get(const QString& k, double def) const
{
    for (const auto& [name, v] : values)
        if (name == k) return v;
    return def;
}

void PrimitiveParams::set(const QString& k, double v)
{
    for (auto& [name, val] : values)
        if (name == k) { val = v; return; }
    values.emplace_back(k, v);
}

PrimitiveParams defaultPrimitive(const QString& type)
{
    PrimitiveParams p;
    p.type = type;
    if (type == "polyCube")
        p.values = {{"width", 1}, {"height", 1}, {"depth", 1}, {"subdivisionsWidth", 1}, {"subdivisionsHeight", 1}, {"subdivisionsDepth", 1}};
    else if (type == "polyPlane")
        p.values = {{"width", 1}, {"height", 1}, {"subdivisionsWidth", 10}, {"subdivisionsHeight", 10}};
    else if (type == "polySphere")
        p.values = {{"radius", 1}, {"subdivisionsAxis", 20}, {"subdivisionsHeight", 20}};
    else if (type == "polyCylinder")
        p.values = {{"radius", 1}, {"height", 2}, {"subdivisionsAxis", 20}, {"subdivisionsHeight", 1}, {"caps", 1}};
    else if (type == "polyCone")
        p.values = {{"radius", 1}, {"height", 2}, {"subdivisionsAxis", 20}, {"subdivisionsHeight", 1}};
    else if (type == "polyTorus")
        p.values = {{"radius", 1}, {"sectionRadius", 0.5}, {"subdivisionsAxis", 20}, {"subdivisionsHeight", 20}};
    return p;
}

Mesh buildPrimitive(const PrimitiveParams& p)
{
    auto f = [&](const char* k, double d) { return float(p.get(k, d)); };
    auto i = [&](const char* k, double d, int lo = 1) { return std::max(lo, int(std::lround(p.get(k, d)))); };
    if (p.type == "polyCube")
        return prim::cube(f("width", 1), f("height", 1), f("depth", 1), i("subdivisionsWidth", 1), i("subdivisionsHeight", 1), i("subdivisionsDepth", 1));
    if (p.type == "polyPlane") return prim::plane(f("width", 1), f("height", 1), i("subdivisionsWidth", 10), i("subdivisionsHeight", 10));
    if (p.type == "polySphere") return prim::sphere(f("radius", 1), i("subdivisionsAxis", 20, 3), i("subdivisionsHeight", 20, 2));
    if (p.type == "polyCylinder")
        return prim::cylinder(f("radius", 1), f("height", 2), i("subdivisionsAxis", 20, 3), i("subdivisionsHeight", 1), p.get("caps", 1) > 0.5);
    if (p.type == "polyCone") return prim::cone(f("radius", 1), f("height", 2), i("subdivisionsAxis", 20, 3), i("subdivisionsHeight", 1));
    if (p.type == "polyTorus")
        return prim::torus(f("radius", 1), f("sectionRadius", 0.5), i("subdivisionsAxis", 20, 3), i("subdivisionsHeight", 20, 3));
    return prim::cube();
}

// ------------------------------------------------------------------------------------------
// SceneObject

Mat4 SceneObject::worldMatrix() const
{
    return Mat4::translate(st.translate) * Mat4::rotateXYZ(st.rotate) * Mat4::scale(st.scale);
}

const Mesh& SceneObject::displayMesh()
{
    if (st.smoothDisplay <= 0 || st.smoothLevel <= 0) return *st.mesh;
    return subdiv.evaluate(*st.mesh, st.smoothLevel);
}

const std::vector<uint32_t>* SceneObject::displayFaceOrigin() const
{
    if (st.smoothDisplay <= 0 || st.smoothLevel <= 0) return nullptr;
    return &subdiv.faceOrigin();
}

std::vector<uint32_t>& SceneObject::compList(SelectMode m)
{
    switch (m) {
    case SelectMode::Vertex: return sel.verts;
    case SelectMode::Edge: return sel.edges;
    default: return sel.faces;
    }
}

// ------------------------------------------------------------------------------------------
// Scene

Scene::Scene(QObject* parent) : QObject(parent) {}
Scene::~Scene() { undo.clear(); }

SceneObject* Scene::find(uint32_t id) const
{
    for (const auto& o : objects)
        if (o->id == id) return o.get();
    return nullptr;
}

SceneObject* Scene::lead() const
{
    for (auto it = selection_.rbegin(); it != selection_.rend(); ++it)
        if (SceneObject* o = find(*it)) return o;
    return nullptr;
}

std::vector<SceneObject*> Scene::selectedObjects() const
{
    std::vector<SceneObject*> out;
    for (uint32_t id : selection_)
        if (SceneObject* o = find(id)) out.push_back(o);
    return out;
}

QString Scene::uniqueName(const QString& base) const
{
    QString stem = base;
    while (!stem.isEmpty() && stem.back().isDigit()) stem.chop(1);
    if (stem.isEmpty()) stem = "object";
    for (int n = 1;; ++n) {
        QString cand = stem + QString::number(n);
        bool used = false;
        for (const auto& o : objects) used |= o->st.name == cand;
        if (!used) return cand;
    }
}

void Scene::insertObject(std::unique_ptr<SceneObject> obj, int index)
{
    if (obj->id >= nextId_) nextId_ = obj->id + 1;
    if (index < 0 || index > int(objects.size())) objects.push_back(std::move(obj));
    else objects.insert(objects.begin() + index, std::move(obj));
}

std::unique_ptr<SceneObject> Scene::takeObject(uint32_t id, int* indexOut)
{
    for (size_t i = 0; i < objects.size(); ++i)
        if (objects[i]->id == id) {
            auto o = std::move(objects[i]);
            objects.erase(objects.begin() + i);
            if (indexOut) *indexOut = int(i);
            selection_.erase(std::remove(selection_.begin(), selection_.end(), id), selection_.end());
            return o;
        }
    return nullptr;
}

void Scene::clear()
{
    undo.clear();
    liveId = 0;
    liveBvhFor_ = 0;
    objects.clear();
    selection_.clear();
    mode_ = SelectMode::Object;
    filePath.clear();
    notifyObjectsChanged();
    notifySelectionChanged();
    emit modeChanged();
}

void Scene::setMode(SelectMode m)
{
    if (m == mode_) return;
    mode_ = m;
    if ((tool_ == Tool::InsertEdgeLoop || tool_ == Tool::MultiCut || tool_ == Tool::TargetWeld) && m == SelectMode::Object) tool_ = Tool::Select;
    emit modeChanged();
    notifySelectionChanged();
}

bool Scene::isSelected(uint32_t id) const
{
    return std::find(selection_.begin(), selection_.end(), id) != selection_.end();
}

void Scene::selectObjects(const std::vector<uint32_t>& ids, SelectOp op)
{
    switch (op) {
    case SelectOp::Replace: selection_ = ids; break;
    case SelectOp::Add:
        for (uint32_t id : ids) {
            selection_.erase(std::remove(selection_.begin(), selection_.end(), id), selection_.end());
            selection_.push_back(id);
        }
        break;
    case SelectOp::Remove:
        for (uint32_t id : ids) selection_.erase(std::remove(selection_.begin(), selection_.end(), id), selection_.end());
        break;
    case SelectOp::Toggle:
        for (uint32_t id : ids) {
            if (isSelected(id)) selection_.erase(std::remove(selection_.begin(), selection_.end(), id), selection_.end());
            else selection_.push_back(id);
        }
        break;
    }
    notifySelectionChanged();
}

void Scene::setSelection(const std::vector<uint32_t>& ids)
{
    selection_ = ids;
    notifySelectionChanged();
}

void Scene::selectComponents(SceneObject* obj, std::vector<uint32_t> items, SelectOp op, bool notify)
{
    if (!obj || mode_ == SelectMode::Object) return;
    items = withMirrored(obj, items);
    ops::sortUnique(items);
    auto& list = obj->compList(mode_);
    std::vector<uint32_t> out;
    switch (op) {
    case SelectOp::Replace: out = std::move(items); break;
    case SelectOp::Add: std::set_union(list.begin(), list.end(), items.begin(), items.end(), std::back_inserter(out)); break;
    case SelectOp::Remove: std::set_difference(list.begin(), list.end(), items.begin(), items.end(), std::back_inserter(out)); break;
    case SelectOp::Toggle: std::set_symmetric_difference(list.begin(), list.end(), items.begin(), items.end(), std::back_inserter(out)); break;
    }
    list = std::move(out);
    if (notify) notifySelectionChanged();
}

void Scene::clearComponents(bool notify)
{
    for (auto& o : objects) o->sel = {};
    if (notify) notifySelectionChanged();
}

void Scene::setComponents(SceneObject* obj, SelectMode m, std::vector<uint32_t> items)
{
    ops::sortUnique(items);
    obj->compList(m) = std::move(items);
    notifySelectionChanged();
}

bool Scene::hasComponentSelection() const
{
    if (mode_ == SelectMode::Object) return false;
    for (SceneObject* o : selectedObjects())
        if (!o->compList(mode_).empty()) return true;
    return false;
}

ops::Comp Scene::compType() const
{
    switch (mode_) {
    case SelectMode::Vertex: return ops::Comp::Vertex;
    case SelectMode::Edge: return ops::Comp::Edge;
    default: return ops::Comp::Face;
    }
}

std::vector<uint32_t> Scene::selectedVerts(SceneObject* obj) const
{
    if (mode_ == SelectMode::Object || !obj) return {};
    return ops::convert(obj->mesh(), compType(), obj->compList(mode_), ops::Comp::Vertex);
}

std::vector<float> Scene::softWeights(SceneObject* obj) const
{
    std::vector<uint32_t> verts = selectedVerts(obj);
    if (verts.empty()) return {};
    const Mesh& m = obj->mesh();
    std::vector<float> w(m.numVerts(), 0.0f);
    for (uint32_t v : verts) w[v] = 1.0f;
    if (!softSelect || softRadius <= 0.0f) return w;

    // World-space falloff (like Maya): spatial hash of selected vertices with cell = radius.
    const Mat4 xf = obj->worldMatrix();
    std::vector<Vec3> P(m.numVerts());
    for (uint32_t v = 0; v < m.numVerts(); ++v) P[v] = xf.transformPoint(m.positions[v]);
    const float r = softRadius, inv = 1.0f / r;
    auto cellKey = [](int64_t x, int64_t y, int64_t z) { return uint64_t((x * 73856093) ^ (y * 19349663) ^ (z * 83492791)); };
    std::unordered_multimap<uint64_t, uint32_t> grid;
    grid.reserve(verts.size() * 2);
    for (uint32_t v : verts) {
        const Vec3& p = P[v];
        grid.emplace(cellKey(int64_t(std::floor(p.x * inv)), int64_t(std::floor(p.y * inv)), int64_t(std::floor(p.z * inv))), v);
    }
    for (uint32_t v = 0; v < m.numVerts(); ++v) {
        if (w[v] >= 1.0f) continue;
        const Vec3& p = P[v];
        int64_t cx = int64_t(std::floor(p.x * inv)), cy = int64_t(std::floor(p.y * inv)), cz = int64_t(std::floor(p.z * inv));
        float best = r * r;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    auto range = grid.equal_range(cellKey(cx + dx, cy + dy, cz + dz));
                    for (auto it = range.first; it != range.second; ++it) best = std::min(best, length2(P[it->second] - p));
                }
        if (best < r * r) {
            float x = std::sqrt(best) / r;
            float f = 1.0f - x * x;
            w[v] = f * f;
        }
    }
    return w;
}

std::vector<uint32_t> Scene::mirrorMap(SceneObject* obj, int axis) const
{
    const Mesh& m = obj->mesh();
    std::vector<uint32_t> out(m.numVerts(), kInvalid);
    if (axis < 0 || axis > 2) return out;
    float size = std::max(length(m.bounds().size()), 1e-6f);
    const float tol = size * 1e-4f, inv = 1.0f / (tol * 4);
    auto key = [&](const Vec3& p) {
        return uint64_t((int64_t(std::floor(p.x * inv)) * 73856093) ^ (int64_t(std::floor(p.y * inv)) * 19349663) ^
                        (int64_t(std::floor(p.z * inv)) * 83492791));
    };
    std::unordered_multimap<uint64_t, uint32_t> grid;
    for (uint32_t v = 0; v < m.numVerts(); ++v) grid.emplace(key(m.positions[v]), v);
    for (uint32_t v = 0; v < m.numVerts(); ++v) {
        Vec3 q = m.positions[v];
        q[axis] = -q[axis];
        float best = tol * tol;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    Vec3 c = q + Vec3{dx / inv, dy / inv, dz / inv};
                    auto r = grid.equal_range(key(c));
                    for (auto it = r.first; it != r.second; ++it) {
                        float d = length2(m.positions[it->second] - q);
                        if (d <= best) { best = d; out[v] = it->second; }
                    }
                }
    }
    return out;
}

std::vector<uint32_t> Scene::withMirrored(SceneObject* obj, const std::vector<uint32_t>& items) const
{
    if (symmetry < 0 || mode_ == SelectMode::Object || !obj) return items;
    auto mm = mirrorMap(obj, symmetry);
    const Mesh& m = obj->mesh();
    std::vector<uint32_t> out = items;
    for (uint32_t i : items) {
        if (mode_ == SelectMode::Vertex) {
            if (i < mm.size() && mm[i] != kInvalid) out.push_back(mm[i]);
        } else if (mode_ == SelectMode::Edge) {
            if (i >= m.numEdges()) continue;
            uint32_t a = mm[m.edges[i].v0], b = mm[m.edges[i].v1];
            if (a != kInvalid && b != kInvalid) {
                uint32_t e = m.findEdge(a, b);
                if (e != kInvalid) out.push_back(e);
            }
        } else {
            if (i >= m.numFaces()) continue;
            std::vector<uint32_t> want;
            bool ok = true;
            for (uint32_t v : m.faceVertices(i)) { ok &= mm[v] != kInvalid; if (ok) want.push_back(mm[v]); }
            if (!ok) continue;
            std::sort(want.begin(), want.end());
            for (uint32_t f : m.vertexFaces(want[0])) {
                std::vector<uint32_t> have(m.faceVertices(f).begin(), m.faceVertices(f).end());
                std::sort(have.begin(), have.end());
                if (have == want) { out.push_back(f); break; }
            }
        }
    }
    return out;
}

const TriBVH* Scene::liveBVH()
{
    SceneObject* o = liveObject();
    if (!o) return nullptr;
    const Mesh& m = o->mesh();
    if (liveBvhFor_ != o->id || liveBvh_.topologyId != m.topologyId || liveBvh_.positionId != m.positionId) {
        liveBvh_.build(m);
        liveBvhFor_ = o->id;
    }
    return &liveBvh_;
}

bool Scene::rayToLive(const Ray& r, Vec3& hit, Vec3* normal)
{
    const TriBVH* b = liveBVH();
    if (!b) return false;
    SceneObject* o = liveObject();
    Mat4 w = o->worldMatrix(), inv = w.inverse();
    Ray lr{inv.transformPoint(r.origin), inv.transformDir(r.dir)};
    auto h = b->raycast(lr);
    if (!h.hit) return false;
    hit = w.transformPoint(h.point);
    if (normal) *normal = normalize(inv.transposed().transformDir(h.normal));
    return true;
}

bool Scene::closestOnLive(const Vec3& p, Vec3& out, float maxDist)
{
    const TriBVH* b = liveBVH();
    if (!b) return false;
    SceneObject* o = liveObject();
    Mat4 w = o->worldMatrix(), inv = w.inverse();
    auto h = b->closest(inv.transformPoint(p), maxDist);
    if (!h.hit) return false;
    out = w.transformPoint(h.point);
    return true;
}

void Scene::setTool(Tool t)
{
    if (t == tool_) return;
    tool_ = t;
    emit toolChanged();
}

bool Scene::editMesh(SceneObject* obj, const QString& label, const MeshEditFn& fn, bool keepHistory)
{
    if (!obj) return false;
    auto copy = std::make_shared<Mesh>(*obj->st.mesh);
    std::optional<std::vector<uint32_t>> newSel = fn(*copy);
    if (!copy->topologyValid()) copy->buildTopology();
    ObjectStateCommand::Entry e;
    e.id = obj->id;
    e.before = obj->st;
    e.after = obj->st;
    e.after.mesh = copy;
    if (!keepHistory) e.after.history.reset();
    e.selBefore = obj->sel;
    if (newSel && mode_ != SelectMode::Object) {
        ComponentSelection s;
        s.verts.clear();
        switch (mode_) {
        case SelectMode::Vertex: s.verts = *newSel; break;
        case SelectMode::Edge: s.edges = *newSel; break;
        case SelectMode::Face: s.faces = *newSel; break;
        default: break;
        }
        e.selAfter = s;
    }
    std::vector<ObjectStateCommand::Entry> entries;
    entries.push_back(std::move(e));
    undo.push(new ObjectStateCommand(this, std::move(entries), label));
    return true;
}

void Scene::pushState(SceneObject* obj, const ObjectState& before, const QString& label)
{
    pushStates({{obj->id, before}}, label);
}

void Scene::pushStates(const std::vector<std::pair<uint32_t, ObjectState>>& before, const QString& label)
{
    std::vector<ObjectStateCommand::Entry> entries;
    for (const auto& [id, st] : before) {
        SceneObject* o = find(id);
        if (!o) continue;
        entries.push_back({id, st, o->st, o->sel, o->sel});
    }
    if (!entries.empty()) undo.push(new ObjectStateCommand(this, std::move(entries), label));
}

void Scene::notifyChanged() { emit sceneChanged(); }
void Scene::notifySelectionChanged()
{
    ++selVersion_;
    emit selectionChanged();
}
void Scene::notifyObjectsChanged()
{
    emit objectsChanged();
    emit sceneChanged();
}

// ------------------------------------------------------------------------------------------
// File IO (.facet: JSON with base64 binary payloads)

namespace {
template <class T>
QString encode(const std::vector<T>& v)
{
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(v.data()), qsizetype(v.size() * sizeof(T))).toBase64());
}
template <class T>
bool decode(const QJsonValue& val, std::vector<T>& out)
{
    QByteArray b = QByteArray::fromBase64(val.toString().toLatin1());
    if (b.size() % sizeof(T)) return false;
    out.resize(b.size() / sizeof(T));
    std::memcpy(out.data(), b.constData(), b.size());
    return true;
}
QJsonArray vec(const Vec3& v) { return {v.x, v.y, v.z}; }
Vec3 vec(const QJsonValue& j, Vec3 def = {})
{
    QJsonArray a = j.toArray();
    if (a.size() != 3) return def;
    return {float(a[0].toDouble()), float(a[1].toDouble()), float(a[2].toDouble())};
}
} // namespace

bool Scene::save(const QString& path, QString* error) const
{
    QJsonArray objs;
    for (const auto& o : objects) {
        const Mesh& m = *o->st.mesh;
        QJsonObject j;
        j["name"] = o->st.name;
        j["translate"] = vec(o->st.translate);
        j["rotate"] = vec(o->st.rotate);
        j["scale"] = vec(o->st.scale);
        j["visible"] = o->st.visible;
        j["smoothDisplay"] = o->st.smoothDisplay;
        j["smoothLevel"] = o->st.smoothLevel;
        if (o->st.history) {
            QJsonObject h;
            h["type"] = o->st.history->type;
            QJsonArray vals;
            for (const auto& [k, v] : o->st.history->values) vals.append(QJsonArray{k, v});
            h["values"] = vals;
            j["history"] = h;
        }
        j["positions"] = encode(m.positions);
        j["faceStart"] = encode(m.faceStart);
        j["faceVerts"] = encode(m.faceVerts);
        QJsonArray creases;
        for (uint32_t e = 0; e < m.numEdges(); ++e)
            if (m.edgeCrease[e] > 0) creases.append(QJsonArray{double(m.edges[e].v0), double(m.edges[e].v1), m.edgeCrease[e]});
        j["creases"] = creases;
        objs.append(j);
    }
    QJsonObject root;
    root["format"] = "facet";
    root["version"] = 1;
    root["objects"] = objs;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    return true;
}

bool Scene::load(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (doc.isNull() || doc.object()["format"].toString() != "facet") {
        if (error) *error = "Not a Facet scene file";
        return false;
    }
    std::vector<std::unique_ptr<SceneObject>> loaded;
    for (const QJsonValue& v : doc.object()["objects"].toArray()) {
        QJsonObject j = v.toObject();
        auto o = std::make_unique<SceneObject>();
        o->st.name = j["name"].toString();
        o->st.translate = vec(j["translate"]);
        o->st.rotate = vec(j["rotate"]);
        o->st.scale = vec(j["scale"], {1, 1, 1});
        o->st.visible = j["visible"].toBool(true);
        o->st.smoothDisplay = j["smoothDisplay"].toInt(0);
        o->st.smoothLevel = j["smoothLevel"].toInt(2);
        if (j.contains("history")) {
            PrimitiveParams p;
            p.type = j["history"].toObject()["type"].toString();
            for (const QJsonValue& kv : j["history"].toObject()["values"].toArray())
                p.values.emplace_back(kv.toArray()[0].toString(), kv.toArray()[1].toDouble());
            o->st.history = p;
        }
        auto m = std::make_shared<Mesh>();
        if (!decode(j["positions"], m->positions) || !decode(j["faceStart"], m->faceStart) || !decode(j["faceVerts"], m->faceVerts) ||
            m->faceStart.empty() || m->faceStart.back() != m->faceVerts.size()) {
            if (error) *error = "Corrupt mesh data in " + o->st.name;
            return false;
        }
        for (uint32_t fv : m->faceVerts)
            if (fv >= m->positions.size()) {
                if (error) *error = "Corrupt face indices in " + o->st.name;
                return false;
            }
        m->buildTopology();
        for (const QJsonValue& c : j["creases"].toArray()) {
            QJsonArray a = c.toArray();
            m->setCrease(uint32_t(a[0].toDouble()), uint32_t(a[1].toDouble()), float(a[2].toDouble()));
        }
        o->st.mesh = m;
        loaded.push_back(std::move(o));
    }
    clear();
    for (auto& o : loaded) {
        o->id = allocateId();
        insertObject(std::move(o));
    }
    filePath = path;
    notifyObjectsChanged();
    return true;
}

} // namespace facet
