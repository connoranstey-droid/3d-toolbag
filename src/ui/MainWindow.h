#pragma once
#include "scene/Scene.h"

#include <QMainWindow>

#include <functional>

class QActionGroup;
class QMenu;

namespace facet {

class Viewport;
class MarkingMenu;
class Hotbox;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    Scene* scene() { return scene_; }
    Viewport* viewport() { return viewport_; }
    void report(const QString& msg);

protected:
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    QAction* addAct(QMenu* menu, const QString& text, const QList<QKeySequence>& keys, std::function<QString()> fn,
                    bool repeatable = false);
    void buildMenus();
    void buildStatusLine();
    void buildShelf();
    void buildDocks();
    void showMarkingMenu(int kind, QPoint globalPos, uint32_t under);
    void updateTitle();
    void updateHelpLine();
    bool maybeSave();
    void newScene();
    void openScene();
    bool saveScene(bool saveAs);
    void showHotkeys();

    Scene* scene_;
    Viewport* viewport_;
    QWidget* central_;
    MarkingMenu* marking_;
    Hotbox* hotbox_;
    std::vector<std::pair<QString, QMenu*>> menus_;
    std::function<QString()> lastRepeatable_;
    QString lastRepeatableName_;
    bool keepFacesTogether_ = true;
    float mergeThreshold_ = 0.001f;
    QActionGroup* modeGroup_ = nullptr;
    QActionGroup* toolGroup_ = nullptr;
    QAction* modeActs_[4] = {};
    QAction* toolActs_[5] = {};
    QAction* softAct_ = nullptr;
    QString helpText_;
};

} // namespace facet
