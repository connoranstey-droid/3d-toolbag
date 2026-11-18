#include "SmartUnwrapPlugin.h"
#include "SmartUnwrap.h"

using namespace facet;

std::vector<PluginCommand> SmartUnwrapPlugin::commands()
{
    PluginCommand c;
    c.id = "smartUnwrap";
    c.menu = "UV";
    c.label = "Smart Unwrap";
    c.tooltip = "Automatic mapping with good shells and uniform texel density";
    c.uvOnly = true;
    c.shortcut = "Ctrl+Alt+U";
    c.options = {
        {"angleLimit", "Shell angle limit (deg)", 66.0, 5, 89, "Maximum normal deviation inside one shell"},
        {"maxStretch", "Max area stretch", 1.1, 1.01, 3, "Shells distorted more than this are split"},
        {"hardEdgeSeams", "Seams on hard edges", true, 0, 1, ""},
        {"keepSeams", "Keep existing UV seams", false, 0, 1, ""},
        {"textureSize", "Texture size (px)", 2048, 64, 16384, "Used for padding and the texel density readout"},
        {"paddingPx", "Shell padding (px)", 8.0, 0, 256, ""},
        {"packTogether", "Pack objects together (shared density)", true, 0, 1, ""},
        {"rotateShells", "Rotate shells for packing", true, 0, 1, ""},
    };
    c.run = [](std::vector<PluginMesh>& meshes, const QVariantMap& o) {
        smart::Options opt;
        opt.angleLimit = float(o.value("angleLimit", 66.0).toDouble());
        opt.maxStretch = float(o.value("maxStretch", 1.1).toDouble());
        opt.hardEdgeSeams = o.value("hardEdgeSeams", true).toBool();
        opt.keepSeams = o.value("keepSeams", false).toBool();
        opt.textureSize = o.value("textureSize", 2048).toInt();
        opt.paddingPx = float(o.value("paddingPx", 8.0).toDouble());
        opt.packTogether = o.value("packTogether", true).toBool();
        opt.rotateShells = o.value("rotateShells", true).toBool();
        std::vector<smart::Target> targets;
        for (auto& pm : meshes) targets.push_back({pm.mesh, pm.world, pm.faces});
        if (targets.empty()) return QString("Smart Unwrap: select meshes");
        auto r = smart::unwrap(targets, opt);
        return QString("Smart Unwrap: %1 shells (%2 splits, %3 merges), %4 px/unit at %5px, %6% packed")
            .arg(r.shells)
            .arg(r.splits)
            .arg(r.merges)
            .arg(r.texelDensity, 0, 'f', 1)
            .arg(opt.textureSize)
            .arg(r.efficiency * 100.0f, 0, 'f', 1);
    };
    return {c};
}
