#include "ui/PluginManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QLibrary>
#include <QPluginLoader>

namespace facet {

PluginManager::PluginManager(Scene* scene, QObject* parent) : QObject(parent), scene_(scene) {}

PluginManager::~PluginManager()
{
    plugins_.clear();
    // Loaders are intentionally not unloaded: commands may still be referenced by actions.
    for (auto& l : loaders_) l.release();
}

QStringList PluginManager::searchPaths() const
{
    QStringList dirs;
    const QString app = QCoreApplication::applicationDirPath();
    dirs << app + "/plugins" << app + "/../plugins" << app + "/../PlugIns" << app + "/../../../plugins";
#ifdef FACET_BUILD_PLUGIN_DIR
    dirs << QStringLiteral(FACET_BUILD_PLUGIN_DIR);
#endif
    const QString env = qEnvironmentVariable("FACET_PLUGIN_PATH");
    if (!env.isEmpty()) dirs << env.split(QDir::listSeparator(), Qt::SkipEmptyParts);
    QStringList out;
    for (const QString& d : dirs) {
        QString c = QDir(d).canonicalPath();
        if (!c.isEmpty() && !out.contains(c)) out << c;
    }
    return out;
}

bool PluginManager::load(const QString& file, QString* error)
{
    for (const auto& p : plugins_)
        if (QFileInfo(p.file).fileName() == QFileInfo(file).fileName()) return true; // already loaded
    auto loader = std::make_unique<QPluginLoader>(file);
    QObject* inst = loader->instance();
    if (!inst) {
        if (error) *error = QFileInfo(file).fileName() + ": " + loader->errorString();
        return false;
    }
    auto* plugin = qobject_cast<FacetPlugin*>(inst);
    if (!plugin) {
        if (error) *error = QFileInfo(file).fileName() + ": not a Facet plugin (API " FacetPlugin_iid ")";
        loader->unload();
        return false;
    }
    plugins_.push_back({file, plugin, plugin->commands()});
    loaders_.push_back(std::move(loader));
    return true;
}

void PluginManager::loadAll()
{
    for (const QString& dir : searchPaths()) {
        QDir d(dir);
        for (const QString& f : d.entryList(QDir::Files)) {
            QString path = d.absoluteFilePath(f);
            if (!QLibrary::isLibrary(path)) continue;
            QString err;
            if (!load(path, &err)) errors_ << err;
        }
    }
}

QVariantMap PluginManager::options(const PluginCommand& c) const
{
    QVariantMap o = options_.value(c.id);
    for (const auto& opt : c.options)
        if (!o.contains(opt.key)) o[opt.key] = opt.defaultValue;
    return o;
}

QString PluginManager::run(const PluginCommand& c)
{
    std::vector<SceneObject*> objs;
    for (SceneObject* o : scene_->selectedObjects())
        if (o->st.visible) objs.push_back(o);
    if (objs.empty()) return c.label + ": select one or more meshes";
    // Work on copies so the whole command is one undo step.
    std::vector<Mesh> copies(objs.size());
    std::vector<PluginMesh> input;
    for (size_t i = 0; i < objs.size(); ++i) {
        SceneObject* o = objs[i];
        copies[i] = o->mesh();
        PluginMesh pm;
        pm.name = o->st.name;
        pm.mesh = &copies[i];
        pm.world = o->worldMatrix();
        if (scene_->mode() != SelectMode::Object) {
            pm.faces = ops::convert(o->mesh(), scene_->compType(), o->compList(scene_->mode()), ops::Comp::Face);
            pm.edges = ops::convert(o->mesh(), scene_->compType(), o->compList(scene_->mode()), ops::Comp::Edge);
        }
        input.push_back(std::move(pm));
    }
    QString msg = c.run ? c.run(input, options(c)) : QString();
    scene_->undo.beginMacro(c.label);
    for (size_t i = 0; i < objs.size(); ++i) {
        SceneObject* o = objs[i];
        auto keep = scene_->mode() == SelectMode::Object ? std::vector<uint32_t>{} : o->compList(scene_->mode());
        Mesh result = std::move(copies[i]);
        if (!result.topologyValid()) result.buildTopology();
        result.touchUVs();
        scene_->editMesh(o, c.label, [&](Mesh& m) {
            m = std::move(result);
            m.touchPositions();
            if (scene_->mode() == SelectMode::Object) return std::optional<std::vector<uint32_t>>(std::nullopt);
            return std::optional<std::vector<uint32_t>>(keep);
        }, c.uvOnly);
        o->sel.uvs.clear();
    }
    scene_->undo.endMacro();
    return msg;
}

} // namespace facet
