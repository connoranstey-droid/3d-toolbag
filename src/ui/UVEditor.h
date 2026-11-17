#pragma once
#include "scene/Scene.h"

#include <QWidget>

#include <functional>

class QAction;

namespace facet {

class UVCanvas : public QWidget {
    Q_OBJECT
public:
    enum class Mode { UV, Shell };
    enum class XTool { Move, Rotate, Scale };
    explicit UVCanvas(Scene* scene, QWidget* parent = nullptr);
    Mode mode = Mode::UV;
    XTool xtool = XTool::Move;
    void frame(bool selectionOnly);
    std::function<void(const QString&)> report;
    QPointF toScreen(Vec2 uv) const;
    Vec2 toUV(QPointF p) const;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    bool event(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    enum class Drag { None, Pan, Zoom, Marquee, Xform };
    struct XEntry {
        SceneObject* obj;
        std::shared_ptr<Mesh> mesh;
        std::vector<uint32_t> idx;
        std::vector<Vec2> start;
    };
    std::vector<SceneObject*> objects() const;
    void select(QPointF p, SelectOp op, bool marquee, QRectF rect = {});
    void beginXform(QPoint p);
    void applyXform(QPoint p);
    void endXform();

    Scene* scene_;
    Vec2 center_{0.5f, 0.5f};
    float zoom_ = 400.0f; // pixels per uv unit
    Drag drag_ = Drag::None;
    QPoint press_, last_;
    Qt::KeyboardModifiers mods_;
    std::vector<XEntry> xform_;
    Vec2 pivot_;
    bool userMoved_ = false;
};

// Dockable editor: toolbar + canvas.
class UVEditor : public QWidget {
    Q_OBJECT
public:
    UVEditor(Scene* scene, std::function<void(const QString&)> report, QWidget* parent = nullptr);
    UVCanvas* canvas() { return canvas_; }
    QAction* checkerAction() { return checker_; }

private:
    UVCanvas* canvas_;
    QAction* checker_ = nullptr;
};

} // namespace facet
