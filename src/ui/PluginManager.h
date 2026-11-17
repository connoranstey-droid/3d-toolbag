#pragma once
#include "plugin/PluginApi.h"
#include "scene/Scene.h"

#include <QObject>
#include <QStringList>

#include <memory>

class QPluginLoader;

namespace facet {

class PluginManager : public QObject {
    Q_OBJECT
public:
    struct Loaded {
        QString file;
        FacetPlugin* plugin = nullptr;
        std::vector<PluginCommand> commands;
    };
    explicit PluginManager(Scene* scene, QObject* parent = nullptr);
    ~PluginManager() override;

    QStringList searchPaths() const;
    void loadAll();
    // Loads one plugin file. Returns false and fills error on failure.
    bool load(const QString& file, QString* error = nullptr);
    const std::vector<Loaded>& plugins() const { return plugins_; }
    const QStringList& errors() const { return errors_; }

    QVariantMap options(const PluginCommand& c) const;
    void setOptions(const PluginCommand& c, const QVariantMap& o) { options_[c.id] = o; }
    // Runs a command on the selected (visible) objects as one undo step.
    QString run(const PluginCommand& c);

private:
    Scene* scene_;
    std::vector<Loaded> plugins_;
    std::vector<std::unique_ptr<QPluginLoader>> loaders_;
    QStringList errors_;
    QMap<QString, QVariantMap> options_;
};

} // namespace facet
