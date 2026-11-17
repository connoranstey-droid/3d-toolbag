#pragma once
#include "math/Math.h"
#include "scene/Scene.h"

#include <QOpenGLFunctions_4_1_Core>

#include <unordered_map>
#include <vector>

namespace facet {

enum class ShadingMode { Wireframe, Shaded };

struct DrawOptions {
    ShadingMode shading = ShadingMode::Shaded;
    bool wireOnShaded = false;
    bool flatShading = false;
    bool xray = false;
    bool showGrid = true;
    bool showHardEdges = false;
    bool backfaceCulling = false;
    bool textured = false; // UV checker (Maya's 6 key)
};

struct ViewParams {
    Mat4 view, proj, viewProj;
    Vec3 eye, forward;
    bool ortho = false;
    int x = 0, y = 0, w = 1, h = 1; // device pixels, GL origin (bottom-left)
    float dpr = 1.0f;
};

// GPU copies of an object's display mesh, cage wire and selection overlays.
struct GpuMesh {
    // display (possibly subdivided) surface
    GLuint vao = 0, pos = 0, nrm = 0, tri = 0;
    GLsizei triIndexCount = 0;
    uint64_t dTopo = 0, dPos = 0;
    std::vector<uint32_t> triFace; // display triangle -> display face
    // display-surface wire (only used when smoothed)
    GLuint dWireVao = 0, dWire = 0;
    GLsizei dWireCount = 0;
    // cage
    GLuint cVao = 0, cPos = 0, cWeight = 0, cEdges = 0;
    GLsizei cEdgeCount = 0, cVertCount = 0;
    uint64_t cTopo = 0, cPosId = 0;
    bool cageShared = false;
    bool cageVaoReady = false;
    // selection overlays
    GLuint selEdgeVao = 0, selEdges = 0, selTriVao = 0, selTris = 0;
    GLsizei selEdgeCount = 0, selTriCount = 0;
    uint64_t selKey = 0;
    std::vector<float> weights;
    std::vector<Vec3> normals; // CPU copy of display normals (reused by the textured path)
    // split-vertex display (hard edges and/or UV checker)
    GLuint texVao = 0, texPos = 0, texNrm = 0, texUV = 0, texIdx = 0;
    GLsizei texIndexCount = 0;
    std::vector<uint32_t> texSrc; // split vertex -> a display corner (half-edge) it came from
    uint64_t texTopo = 0, texUvId = 0, texPosId = 0;
    bool texWithUV = false;
    // sculpt mask
    GLuint maskBuf = 0, texMask = 0;
    bool maskOn = false, texMaskOn = false;
    uint64_t maskVersion = 0, texMaskVersion = 0, texMaskTopo = 0;
    bool hasHard = false;
    // hard edges overlay
    GLuint hardVao = 0, hardIdx = 0;
    GLsizei hardCount = 0;
};

class Renderer : protected QOpenGLFunctions_4_1_Core {
public:
    bool initialize();
    void cleanup();

    void beginView(const ViewParams& vp, bool active);
    void drawBackground(const Vec3& top, const Vec3& bottom);
    void drawGrid(const ViewParams& vp, int viewKind, float spacing, int lines);
    void drawScene(Scene& scene, const ViewParams& vp, const DrawOptions& opt);
    void drawPick(Scene& scene, const ViewParams& vp, bool selectedOnly);
    void drawAxisTriad(const ViewParams& vp);

    // Immediate-mode helpers (gizmos, previews).
    void drawLines(const std::vector<Vec3>& pts, const Vec4& color, const Mat4& mvp, bool depthTest, GLenum prim = GL_LINES);
    void drawPoints(const std::vector<Vec3>& pts, const Vec4& color, const Mat4& mvp, float size, bool depthTest);

    GpuMesh* gpu(uint32_t id);
    void syncObject(Scene& scene, SceneObject& obj);
    void purge(const Scene& scene);

    QOpenGLFunctions_4_1_Core* gl() { return this; }

private:
    GLuint compile(const char* vs, const char* fs, const char* name);
    void uploadDisplay(SceneObject& obj, GpuMesh& g);
    void uploadCage(Scene& scene, SceneObject& obj, GpuMesh& g, bool smooth);
    void uploadSelection(Scene& scene, SceneObject& obj, GpuMesh& g, bool smooth);
    bool uploadSplit(SceneObject& obj, GpuMesh& g, bool withUV);
    static void deleteGpu(QOpenGLFunctions_4_1_Core* gl, GpuMesh& g);

    GLuint meshProg_ = 0, lineProg_ = 0, pickProg_ = 0, bgProg_ = 0;
    GLuint immVao_ = 0, immVbo_ = 0, bgVao_ = 0;
    std::unordered_map<uint32_t, GpuMesh> meshes_;
};

} // namespace facet
