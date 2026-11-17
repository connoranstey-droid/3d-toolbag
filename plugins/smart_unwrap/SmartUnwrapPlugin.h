#pragma once
#include "plugin/PluginApi.h"

#include <QObject>

class SmartUnwrapPlugin : public QObject, public facet::FacetPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID FacetPlugin_iid FILE "smart_unwrap.json")
    Q_INTERFACES(facet::FacetPlugin)
public:
    QString name() const override { return "Smart Unwrap"; }
    QString version() const override { return "1.0"; }
    QString description() const override
    {
        return "Automatic UVs with low-distortion shells and uniform texel density: curvature-aware shell growing, "
               "LSCM unfolding, distortion-driven splitting, world-space density and skyline packing with texel padding.";
    }
    std::vector<facet::PluginCommand> commands() override;
};
