#pragma once
#include "scene/Scene.h"

#include <QTreeWidget>

namespace facet {

class Outliner : public QTreeWidget {
    Q_OBJECT
public:
    explicit Outliner(Scene* scene, QWidget* parent = nullptr);

private:
    void rebuild();
    void syncSelectionFromScene();
    Scene* scene_;
    bool syncing_ = false;
};

} // namespace facet
