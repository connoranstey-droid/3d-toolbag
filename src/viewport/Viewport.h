#pragma once
#include "render/Renderer.h"
#include "scene/Scene.h"
#include "viewport/Camera.h"
#include "viewport/Gizmo.h"

#include <QElapsedTimer>
#include <QOpenGLWidget>
#include <QTimer>

#include <array>

namespace facet {

class Viewport : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit Viewport(Scene* scene, QWidget* parent = nullptr);
    ~Viewport() override;

    DrawOptions options;
    bool showHud = true;

    bool quadView() const { return quad_; }
    void setQuadView(bool on);
    void toggleQuadView();
    void setSingleView(ViewKind k);
    void frameSelected(bool allViews = false);
    void frameAll(bool allViews = false);
    void cycleBackground();
    // Keyboard camera control (active view). Ortho views pan instead of orbiting.
    void orbitView(float dAzimuthDeg, float dElevationDeg);
    void panView(float fx, float fy); // fractions of the view height
    void zoomView(float steps);       // + = closer
    void resetView();
    // Maya's +/- manipulator size.
    void scaleManipulator(float factor) { gizmoScale_ = std::clamp(gizmoScale_ * factor, 0.25f, 4.0f); update(); }
    float manipulatorScale() const { return gizmoScale_; }
    Camera& activeCamera() { return cams_[size_t(active_)]; }
    // Widget coordinates of a world point in the active view (used by tools and tests).
    QPoint worldToWidget(const Vec3& w) const
    {
        Vec3 p = project(active_, w);
        return QPoint(int(std::lround(p.x)), int(std::lround(p.y)));
    }

signals:
    // kind: 0 = RMB (component/object), 1 = Shift+RMB (tools), 2 = Ctrl+RMB (convert selection)
    void markingMenuRequested(int kind, QPoint globalPos, uint32_t objectUnderCursor);
    void markingMenuTrack(QPoint globalPos);
    void markingMenuRelease(QPoint globalPos);
    void hotboxRequested(QPoint globalPos);
    void hotboxDismissed();
    void message(const QString& text);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    bool event(QEvent* e) override;

private:
    struct Hit {
        SceneObject* obj = nullptr;
        uint32_t cageFace = kInvalid;
    };
    struct CompEntry {
        SceneObject* obj;
        std::shared_ptr<Mesh> mesh;
        std::vector<uint32_t> idx;
        std::vector<float> w;
        std::vector<Vec3> start;
        Mat4 world, inv;
        std::vector<uint8_t> mirror; // 0 = normal, 1 = mirrored partner, 2 = on the symmetry plane
    };
    struct Manip {
        Tool tool = Tool::Select;
        Handle handle = Handle::None;
        GizmoFrame frame;
        Vec3 startHit, prevVec;
        float startParam = 0, angle = 0;
        QPoint startPos;
        bool moved = false;
        std::vector<std::pair<uint32_t, ObjectState>> objStart;
        std::vector<CompEntry> comps;
    };
    struct LoopPreview {
        SceneObject* obj = nullptr;
        uint32_t edge = kInvalid;
        float t = 0.5f;
        ops::EdgeLoopPlan plan;
        std::vector<Vec3> lines; // world space
    };
    struct HoverComp {
        SceneObject* obj = nullptr;
        uint32_t index = kInvalid;
    };
    struct CutState {
        SceneObject* obj = nullptr;
        std::vector<ops::CutPoint> pts;
        std::vector<Vec3> world;
        bool hoverValid = false;
        ops::CutPoint hover;
        Vec3 hoverWorld;
    };
    struct WeldState {
        SceneObject* obj = nullptr;
        uint32_t src = kInvalid, dst = kInvalid;
    };
    struct QuadState {
        uint32_t target = 0;
        std::vector<uint32_t> preview;  // quad to create (target vertex ids)
        uint32_t hoverVert = kInvalid, hoverEdge = kInvalid;
        float hoverT = 0.5f;
        uint32_t dragVert = kInvalid;
        std::vector<uint32_t> touched;   // relax: vertices changed this stroke
        std::vector<Vec3> before;        // their start positions
        bool extend = false;
        QPoint pressPos;
        bool shiftPress = false;
    };
    struct SculptState {
        SceneObject* obj = nullptr;
        sculpt::Stroke stroke;
        Vec3 lastLocal;
        float radiusLocal = 0.1f;
        Mat4 inv;
        Vec3 grabStartWorld, grabNormal;
        bool invert = false;
        sculpt::Brush brush = sculpt::Brush::Draw;
    };
    enum class Drag { None, Tumble, Track, Dolly, Marquee, Manip, SoftRadius, Menu, LoopSlide, Cut, Weld, QuadVert, QuadRelax, QuadShift, QuadExtend,
                      Sculpt, BrushRadius, BrushStrength };

    // layout
    std::vector<int> visibleSlots() const;
    QRect slotRect(int slot) const;
    int slotAt(QPoint p) const;
    ViewParams viewParams(int slot) const;
    GizmoView gizmoView(int slot) const;
    Vec3 project(int slot, const Vec3& world) const; // logical px (x, y) + ndc depth
    Ray rayAt(int slot, QPoint p) const;

    // picking
    bool renderPick(int slot, QRect logicalRect, bool selectedOnly, std::vector<uint32_t>& ids, std::vector<float>& depth, QRect& devRect);
    Hit decode(uint32_t objId, uint32_t prim) const;
    Hit pickSurface(int slot, QPoint p, int radius, bool selectedOnly);
    uint32_t pickVertex(int slot, QPoint p, SceneObject*& obj);
    uint32_t pickEdge(int slot, QPoint p, bool selectedOnly, SceneObject*& obj, float* tOut = nullptr);
    void clickSelect(int slot, QPoint p, SelectOp op);
    void marqueeSelect(int slot, QRect r, SelectOp op);
    static SelectOp opFromModifiers(Qt::KeyboardModifiers m);

    // manipulators
    bool gizmoFrame(int slot, GizmoFrame& f) const;
    void beginManip(int slot, Handle h, QPoint p);
    void updateManip(QPoint p);
    void endManip();

    // tools
    void updateLoopPreview(int slot, QPoint p, bool keepEdge = false);
    void updateHover(int slot, QPoint p);
    void drawOverlays(int slot, const ViewParams& vp);
    void drawHud(class QPainter& p);
    // Multi-Cut / Target Weld / Quad Draw
    bool cutPointAt(int slot, QPoint p, SceneObject*& obj, ops::CutPoint& cp, Vec3& world);
    void commitCut();
    SceneObject* quadTarget(bool create);
    void updateQuadHover(int slot, QPoint p, bool shift);
    void quadPress(int slot, QPoint p, Qt::KeyboardModifiers mods);
    void quadRelease(QPoint p);
    void quadRelax(int slot, QPoint p);
    bool liveHit(int slot, QPoint p, Vec3& world);
    // Sculpting
    bool surfaceHit(int slot, QPoint p, bool selectedOnly, SceneObject*& obj, Vec3& world);
    void sculptPress(int slot, QPoint p, Qt::KeyboardModifiers mods);
    void sculptMove(QPoint p);
    void sculptRelease();
    AABB selectionBounds(bool componentsOnly) const;

    Scene* scene_;
    Renderer renderer_;
    bool glReady_ = false;
    GLuint pickFbo_ = 0, pickTex_ = 0, pickDepth_ = 0;
    QSize pickSize_;

    std::array<Camera, 4> cams_ = {Camera::make(ViewKind::Persp), Camera::make(ViewKind::Top), Camera::make(ViewKind::Front),
                                   Camera::make(ViewKind::Side)};
    bool quad_ = false;
    int single_ = 0;
    int active_ = 0;

    Drag drag_ = Drag::None;
    int dragSlot_ = 0;
    QPoint pressPos_, lastPos_, mousePos_{-10000, -10000};
    Qt::KeyboardModifiers pressMods_;
    Handle hover_ = Handle::None;
    Handle lastHandle_[3] = {Handle::Center, Handle::View, Handle::Center};
    Manip manip_;
    LoopPreview loop_;
    HoverComp hoverComp_;
    CutState cut_;
    WeldState weld_;
    QuadState qd_;
    bool tabHeld_ = false;
    bool mHeld_ = false;
    float gizmoScale_ = 1.0f;
    SculptState sc_;

    bool bHeld_ = false, bUsed_ = false, xHeld_ = false, jHeld_ = false, spaceHeld_ = false, hotboxShown_ = false;
    QElapsedTimer spaceTimer_;
    QTimer hotboxTimer_;
    int bgIndex_ = 0;
    QString transientText_;
};

} // namespace facet
