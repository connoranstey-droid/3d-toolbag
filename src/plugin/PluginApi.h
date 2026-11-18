#pragma once
// Facet plugin API (version 1).
//
// A plugin is a Qt plugin (shared library) placed in a "plugins" folder next to the Facet
// executable, in <build>/plugins, or in a folder listed in FACET_PLUGIN_PATH. It implements
// FacetPlugin and returns commands; Facet adds each command to the Plugins menu (and to the menu
// it names), builds an options dialog from its options, and runs it on copies of the selected
// meshes so the whole operation is a single undo step. Plugins link only against facet_core
// (geometry: Mesh, MeshOps, UVOps, ...) and QtCore.
#include "mesh/Mesh.h"

#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QtPlugin>

#include <functional>
#include <vector>

namespace facet {

struct PluginMesh {
    QString name;
    Mesh* mesh = nullptr;              // a copy owned by the host; edit freely
    Mat4 world;                        // object-to-world transform
    std::vector<uint32_t> faces;       // selected faces (empty = whole mesh)
    std::vector<uint32_t> edges;       // selected edges
};

struct PluginOption {
    QString key;
    QString label;
    QVariant defaultValue;             // double, int or bool
    double minimum = 0.0, maximum = 1e9;
    QString tooltip;
};

struct PluginCommand {
    QString id;                        // unique, e.g. "smartUnwrap"
    QString menu;                      // extra main-menu to appear in ("UV", "Mesh", ...), optional
    QString label;
    QString tooltip;
    std::vector<PluginOption> options;
    bool uvOnly = false;               // true: keeps construction history (geometry untouched)
    QString shortcut;                  // optional default hotkey, e.g. "Ctrl+Alt+U" (user can rebind)
    // Returns a status message for the help line.
    std::function<QString(std::vector<PluginMesh>& meshes, const QVariantMap& options)> run;
};

class FacetPlugin {
public:
    virtual ~FacetPlugin() = default;
    virtual QString name() const = 0;
    virtual QString version() const = 0;
    virtual QString description() const = 0;
    virtual std::vector<PluginCommand> commands() = 0;
};

} // namespace facet

#define FacetPlugin_iid "org.facet.FacetPlugin/1"
Q_DECLARE_INTERFACE(facet::FacetPlugin, FacetPlugin_iid)
