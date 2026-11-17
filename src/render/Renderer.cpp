#include "render/Renderer.h"

#include <QDebug>

#include <cmath>
#include <unordered_set>

namespace facet {

namespace {

const char* kMeshVS = R"(#version 410 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
layout(location = 2) in vec2 aUV;
layout(location = 3) in float aMask;
uniform mat4 uMVP;
uniform mat4 uModelView;
uniform mat3 uNormalMat;
uniform int uHasMask; // disabled attribute values are undefined after other draws: gate explicitly
out vec3 vN;
out vec3 vViewPos;
out vec2 vUV;
out float vMask;
void main() {
    vUV = aUV;
    vMask = uHasMask == 1 ? aMask : 0.0;
    vec4 vp = uModelView * vec4(aPos, 1.0);
    vViewPos = vp.xyz;
    vN = uNormalMat * aNrm;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

// Three-point studio rig in view space + hemisphere ambient: reads form well on high-poly surfaces.
const char* kMeshFS = R"(#version 410 core
in vec3 vN;
in vec3 vViewPos;
in vec2 vUV;
in float vMask;
uniform vec3 uColor;
uniform float uAlpha;
uniform int uFlat;
uniform int uChecker;
out vec4 frag;
void main() {
    vec3 base = uColor;
    if (uChecker == 1) {
        vec2 c = floor(vUV * 8.0);
        float k = mod(c.x + c.y, 2.0);
        base = mix(vec3(0.22, 0.22, 0.24), vec3(0.86, 0.86, 0.86), k);
        // Tint by quadrant so flipped / rotated shells are easy to spot.
        base *= vec3(0.8 + 0.2 * fract(vUV.x), 0.8 + 0.2 * fract(vUV.y), 0.92);
    }
    vec3 n = (uFlat == 1) ? normalize(cross(dFdx(vViewPos), dFdy(vViewPos))) : normalize(vN);
    vec3 v = normalize(-vViewPos);
    if (dot(n, v) < 0.0) n = -n;
    vec3 key  = normalize(vec3(-0.45, 0.55, 0.70));
    vec3 fill = normalize(vec3(0.70, -0.15, 0.45));
    vec3 rimL = normalize(vec3(0.0, 0.6, -0.8));
    float dk = max(dot(n, key), 0.0);
    float df = max(dot(n, fill), 0.0);
    float hemi = 0.5 + 0.5 * n.y;
    vec3 col = base * (0.10 + 0.22 * hemi + 0.72 * dk + 0.22 * df);
    vec3 h = normalize(key + v);
    col += vec3(0.22) * pow(max(dot(n, h), 0.0), 40.0);
    col += vec3(0.10) * pow(1.0 - max(dot(n, v), 0.0), 3.0) * (0.5 + 0.5 * max(dot(n, rimL), 0.0));
    col *= mix(vec3(1.0), vec3(0.32, 0.36, 0.45), clamp(vMask, 0.0, 1.0)); // sculpt mask
    col = pow(clamp(col, 0.0, 1.0), vec3(1.0 / 1.15));
    frag = vec4(col, uAlpha);
}
)";

const char* kLineVS = R"(#version 410 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in float aW;
uniform mat4 uMVP;
uniform float uPointSize;
out float vW;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    gl_PointSize = uPointSize;
    vW = aW;
}
)";

// uMode 0: flat colour. 1: soft-selection ramp where weighted, else base colour.
// 2: like 1 but unweighted elements are discarded.
const char* kLineFS = R"(#version 410 core
in float vW;
uniform vec4 uColor;
uniform int uMode;
out vec4 frag;
vec3 ramp(float w) {
    if (w >= 0.999) return vec3(1.0, 0.95, 0.15);
    vec3 a = mix(vec3(0.35, 0.0, 0.0), vec3(1.0, 0.25, 0.0), smoothstep(0.0, 0.45, w));
    return mix(a, vec3(1.0, 0.85, 0.0), smoothstep(0.45, 1.0, w));
}
void main() {
    if (uMode == 0) { frag = uColor; return; }
    if (vW <= 0.0) {
        if (uMode == 2) discard;
        frag = uColor;
        return;
    }
    frag = vec4(ramp(vW), 1.0);
}
)";

const char* kPickVS = R"(#version 410 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
void main() { gl_Position = uMVP * vec4(aPos, 1.0); }
)";

const char* kPickFS = R"(#version 410 core
uniform uint uObj;
layout(location = 0) out uvec2 id;
void main() { id = uvec2(uObj, uint(gl_PrimitiveID)); }
)";

const char* kBgVS = R"(#version 410 core
out float vT;
void main() {
    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);
    vT = p.y * 0.5 + 0.5;
    gl_Position = vec4(p, 0.999, 1.0);
}
)";

const char* kBgFS = R"(#version 410 core
in float vT;
uniform vec3 uTop;
uniform vec3 uBottom;
out vec4 frag;
void main() { frag = vec4(mix(uBottom, uTop, clamp(vT, 0.0, 1.0)), 1.0); }
)";

Mat4 normalMatrix(const Mat4& mv)
{
    return mv.inverse().transposed();
}

} // namespace

GLuint Renderer::compile(const char* vs, const char* fs, const char* name)
{
    auto stage = [&](GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[4096];
            glGetShaderInfoLog(s, sizeof log, nullptr, log);
            qWarning("Shader %s compile error: %s", name, log);
        }
        return s;
    };
    GLuint p = glCreateProgram();
    GLuint v = stage(GL_VERTEX_SHADER, vs), f = stage(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        qWarning("Program %s link error: %s", name, log);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

bool Renderer::initialize()
{
    if (!initializeOpenGLFunctions()) return false;
    meshProg_ = compile(kMeshVS, kMeshFS, "mesh");
    lineProg_ = compile(kLineVS, kLineFS, "line");
    pickProg_ = compile(kPickVS, kPickFS, "pick");
    bgProg_ = compile(kBgVS, kBgFS, "background");
    glGenVertexArrays(1, &immVao_);
    glGenBuffers(1, &immVbo_);
    glBindVertexArray(immVao_);
    glBindBuffer(GL_ARRAY_BUFFER, immVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vec3), nullptr);
    glGenVertexArrays(1, &bgVao_);
    glBindVertexArray(0);
    glEnable(GL_PROGRAM_POINT_SIZE);
    return true;
}

void Renderer::deleteGpu(QOpenGLFunctions_4_1_Core* gl, GpuMesh& g)
{
    GLuint vaos[] = {g.vao, g.dWireVao, g.cVao, g.selEdgeVao, g.selTriVao, g.texVao, g.hardVao};
    GLuint bufs[] = {g.pos, g.nrm, g.tri, g.dWire, g.cPos, g.cWeight, g.cEdges, g.selEdges, g.selTris, g.texPos, g.texNrm, g.texUV, g.texIdx, g.hardIdx,
                     g.maskBuf, g.texMask};
    gl->glDeleteVertexArrays(7, vaos);
    gl->glDeleteBuffers(16, bufs);
}

void Renderer::cleanup()
{
    for (auto& [id, g] : meshes_) deleteGpu(this, g);
    meshes_.clear();
    glDeleteProgram(meshProg_);
    glDeleteProgram(lineProg_);
    glDeleteProgram(pickProg_);
    glDeleteProgram(bgProg_);
    glDeleteVertexArrays(1, &immVao_);
    glDeleteVertexArrays(1, &bgVao_);
    glDeleteBuffers(1, &immVbo_);
}

GpuMesh* Renderer::gpu(uint32_t id)
{
    auto it = meshes_.find(id);
    return it == meshes_.end() ? nullptr : &it->second;
}

void Renderer::purge(const Scene& scene)
{
    for (auto it = meshes_.begin(); it != meshes_.end();) {
        if (!scene.find(it->first)) {
            deleteGpu(this, it->second);
            it = meshes_.erase(it);
        } else {
            ++it;
        }
    }
}

void Renderer::syncObject(Scene& scene, SceneObject& obj)
{
    auto [it, created] = meshes_.try_emplace(obj.id);
    GpuMesh& g = it->second;
    if (created) {
        glGenVertexArrays(1, &g.vao);
        glGenVertexArrays(1, &g.dWireVao);
        glGenVertexArrays(1, &g.cVao);
        glGenVertexArrays(1, &g.selEdgeVao);
        glGenVertexArrays(1, &g.selTriVao);
        GLuint* bufs[] = {&g.pos, &g.nrm, &g.tri, &g.dWire, &g.cPos, &g.cWeight, &g.cEdges, &g.selEdges, &g.selTris,
                          &g.texPos, &g.texNrm, &g.texUV, &g.texIdx, &g.hardIdx, &g.maskBuf, &g.texMask};
        for (GLuint* b : bufs) glGenBuffers(1, b);
        glGenVertexArrays(1, &g.hardVao);
        glGenVertexArrays(1, &g.texVao);
        glBindVertexArray(g.texVao);
        GLuint texBufs[3] = {g.texPos, g.texNrm, g.texUV};
        for (int a = 0; a < 3; ++a) {
            glBindBuffer(GL_ARRAY_BUFFER, texBufs[a]);
            glEnableVertexAttribArray(GLuint(a));
            glVertexAttribPointer(GLuint(a), a == 2 ? 2 : 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.texIdx);

        glBindVertexArray(g.vao);
        glBindBuffer(GL_ARRAY_BUFFER, g.pos);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vec3), nullptr);
        glBindBuffer(GL_ARRAY_BUFFER, g.nrm);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vec3), nullptr);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.tri);

        glBindVertexArray(g.dWireVao);
        glBindBuffer(GL_ARRAY_BUFFER, g.pos);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vec3), nullptr);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.dWire);

        glBindVertexArray(g.selTriVao);
        glBindBuffer(GL_ARRAY_BUFFER, g.pos);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vec3), nullptr);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.selTris);
        glBindVertexArray(0);
    }
    const bool smooth = obj.displayFaceOrigin() != nullptr;
    uploadDisplay(obj, g);
    // Sculpt mask (only when the display mesh is the cage).
    const bool wantMask = !smooth && obj.hasMask();
    if (wantMask != g.maskOn || (wantMask && g.maskVersion != obj.maskVersion)) {
        if (wantMask) {
            glBindBuffer(GL_ARRAY_BUFFER, g.maskBuf);
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(obj.mask.size() * sizeof(float)), obj.mask.data(), GL_DYNAMIC_DRAW);
        }
        glBindVertexArray(g.vao);
        if (wantMask) {
            glBindBuffer(GL_ARRAY_BUFFER, g.maskBuf);
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(float), nullptr);
        } else {
            glDisableVertexAttribArray(3);
        }
        glBindVertexArray(0);
        g.maskOn = wantMask;
        g.maskVersion = obj.maskVersion;
        g.texMaskVersion = 0;
    }
    uploadCage(scene, obj, g, smooth);
    uploadSelection(scene, obj, g, smooth);
}

void Renderer::uploadDisplay(SceneObject& obj, GpuMesh& g)
{
    const Mesh& d = obj.displayMesh();
    const bool smooth = &d != &obj.mesh();
    if (g.dTopo != d.topologyId) {
        std::vector<uint32_t> idx;
        idx.reserve(d.triangleCount() * 3);
        g.triFace.clear();
        g.triFace.reserve(d.triangleCount());
        for (uint32_t f = 0; f < d.numFaces(); ++f) {
            auto fv = d.faceVertices(f);
            for (size_t i = 1; i + 1 < fv.size(); ++i) {
                idx.insert(idx.end(), {fv[0], fv[i], fv[i + 1]});
                g.triFace.push_back(f);
            }
        }
        glBindBuffer(GL_ARRAY_BUFFER, g.tri);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(idx.size() * 4), idx.data(), GL_STATIC_DRAW);
        g.triIndexCount = GLsizei(idx.size());
        if (smooth) {
            std::vector<uint32_t> w;
            w.reserve(d.numEdges() * 2);
            for (const auto& e : d.edges) w.insert(w.end(), {e.v0, e.v1});
            glBindBuffer(GL_ARRAY_BUFFER, g.dWire);
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(w.size() * 4), w.data(), GL_STATIC_DRAW);
            g.dWireCount = GLsizei(w.size());
        } else {
            g.dWireCount = 0;
        }
        g.dTopo = d.topologyId;
        g.dPos = 0;
        g.selKey = 0;
        g.hasHard = d.hasHardEdges() || d.normalWeightPower > 0.0f;
        // Hard edges of the cage (for the "show hard edges" overlay).
        const Mesh& c = obj.mesh();
        std::vector<uint32_t> he;
        for (uint32_t e = 0; e < c.numEdges(); ++e)
            if (c.isHard(e)) he.insert(he.end(), {c.edges[e].v0, c.edges[e].v1});
        glBindBuffer(GL_ARRAY_BUFFER, g.hardIdx);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(he.size() * 4), he.data(), GL_STATIC_DRAW);
        g.hardCount = GLsizei(he.size());
    }
    if (g.dPos != d.positionId) {
        std::vector<Vec3>& n = g.normals;
        d.computeVertexNormals(n);
        glBindBuffer(GL_ARRAY_BUFFER, g.pos);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(d.positions.size() * sizeof(Vec3)), d.positions.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, g.nrm);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(n.size() * sizeof(Vec3)), n.data(), GL_DYNAMIC_DRAW);
        g.dPos = d.positionId;
    }
}

bool Renderer::uploadSplit(SceneObject& obj, GpuMesh& g, bool withUV)
{
    const Mesh& d = obj.displayMesh();
    if (withUV && !d.hasUVs()) withUV = false;
    if (g.texTopo != d.topologyId || (withUV && g.texUvId != d.uvId) || g.texWithUV != withUV) {
        // One GPU vertex per (smoothing group, uv) pair.
        std::vector<Vec3> cn;
        std::vector<uint32_t> group;
        d.computeCornerNormals(cn, &group);
        std::unordered_map<uint64_t, uint32_t> split;
        split.reserve(d.numHalfEdges());
        g.texSrc.clear();
        std::vector<Vec2> uvs;
        std::vector<uint32_t> cornerIdx(d.numHalfEdges());
        for (uint32_t h = 0; h < d.numHalfEdges(); ++h) {
            uint64_t key = (uint64_t(group[h]) << 32) | (withUV ? d.faceUVs[h] : 0u);
            auto [it, ins] = split.emplace(key, uint32_t(g.texSrc.size()));
            if (ins) {
                g.texSrc.push_back(h);
                uvs.push_back(withUV ? d.cornerUV(h) : Vec2{});
            }
            cornerIdx[h] = it->second;
        }
        std::vector<uint32_t> idx;
        idx.reserve(d.triangleCount() * 3);
        for (uint32_t f = 0; f < d.numFaces(); ++f)
            for (uint32_t i = 1; i + 1 < d.faceSize(f); ++i) {
                uint32_t b0 = d.faceStart[f];
                idx.insert(idx.end(), {cornerIdx[b0], cornerIdx[b0 + i], cornerIdx[b0 + i + 1]});
            }
        glBindBuffer(GL_ARRAY_BUFFER, g.texUV);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(uvs.size() * sizeof(Vec2)), uvs.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, g.texIdx);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(idx.size() * 4), idx.data(), GL_STATIC_DRAW);
        g.texIndexCount = GLsizei(idx.size());
        g.texTopo = d.topologyId;
        g.texUvId = d.uvId;
        g.texWithUV = withUV;
        g.texPosId = 0;
    }
    if (g.texPosId != d.positionId) {
        std::vector<Vec3> cn;
        d.computeCornerNormals(cn);
        std::vector<Vec3> p(g.texSrc.size()), n(g.texSrc.size());
        for (size_t i = 0; i < g.texSrc.size(); ++i) {
            p[i] = d.positions[d.faceVerts[g.texSrc[i]]];
            n[i] = cn[g.texSrc[i]];
        }
        glBindBuffer(GL_ARRAY_BUFFER, g.texPos);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(p.size() * sizeof(Vec3)), p.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, g.texNrm);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(n.size() * sizeof(Vec3)), n.data(), GL_DYNAMIC_DRAW);
        g.texPosId = d.positionId;
    }
    // Mask on the split vertices.
    const bool wantMask = obj.hasMask() && &d == &obj.mesh();
    if (wantMask && (g.texMaskVersion != obj.maskVersion || g.texMaskTopo != g.texTopo)) {
        std::vector<float> mk(g.texSrc.size());
        for (size_t i = 0; i < g.texSrc.size(); ++i) mk[i] = obj.mask[d.faceVerts[g.texSrc[i]]];
        glBindBuffer(GL_ARRAY_BUFFER, g.texMask);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(mk.size() * sizeof(float)), mk.data(), GL_DYNAMIC_DRAW);
        g.texMaskVersion = obj.maskVersion;
        g.texMaskTopo = g.texTopo;
    }
    if (wantMask != g.texMaskOn) {
        glBindVertexArray(g.texVao);
        if (wantMask) {
            glBindBuffer(GL_ARRAY_BUFFER, g.texMask);
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(float), nullptr);
        } else {
            glDisableVertexAttribArray(3);
        }
        glBindVertexArray(0);
        g.texMaskOn = wantMask;
    }
    return true;
}

void Renderer::uploadCage(Scene& scene, SceneObject& obj, GpuMesh& g, bool smooth)
{
    (void)scene;
    const Mesh& c = obj.mesh();
    const bool shared = !smooth;
    if (shared != g.cageShared || !g.cageVaoReady) {
        GLuint posBuf = shared ? g.pos : g.cPos;
        glBindVertexArray(g.hardVao);
        glBindBuffer(GL_ARRAY_BUFFER, posBuf);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vec3), nullptr);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.hardIdx);
        for (GLuint vao : {g.cVao, g.selEdgeVao}) {
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, posBuf);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vec3), nullptr);
            glBindBuffer(GL_ARRAY_BUFFER, g.cWeight);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, sizeof(float), nullptr);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vao == g.cVao ? g.cEdges : g.selEdges);
        }
        glBindVertexArray(0);
        g.cageShared = shared;
        g.cageVaoReady = true;
        g.cPosId = 0;
    }
    if (g.cTopo != c.topologyId) {
        std::vector<uint32_t> w;
        w.reserve(c.numEdges() * 2);
        for (const auto& e : c.edges) w.insert(w.end(), {e.v0, e.v1});
        glBindBuffer(GL_ARRAY_BUFFER, g.cEdges);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(w.size() * 4), w.data(), GL_STATIC_DRAW);
        g.cEdgeCount = GLsizei(w.size());
        g.cVertCount = GLsizei(c.numVerts());
        g.cTopo = c.topologyId;
        g.cPosId = 0;
        g.selKey = 0;
    }
    if (!shared && g.cPosId != c.positionId) {
        glBindBuffer(GL_ARRAY_BUFFER, g.cPos);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(c.positions.size() * sizeof(Vec3)), c.positions.data(), GL_DYNAMIC_DRAW);
        g.cPosId = c.positionId;
    }
}

void Renderer::uploadSelection(Scene& scene, SceneObject& obj, GpuMesh& g, bool smooth)
{
    const Mesh& c = obj.mesh();
    const bool comp = scene.mode() != SelectMode::Object && scene.isSelected(obj.id);
    uint64_t key = scene.selectionVersion() * 1000003ull ^ c.topologyId * 7919ull ^ uint64_t(scene.mode()) * 31ull ^
                   (comp ? 17ull : 0ull) ^ (smooth ? 0x55ull : 0ull) ^ g.dTopo * 104729ull;
    if (comp && scene.softSelect) {
        key ^= uint64_t(std::hash<float>{}(scene.softRadius)) * 13ull;
        if (!scene.dragging) key ^= c.positionId * 2654435761ull;
        else if (g.selKey != 0 && !g.weights.empty()) key = g.selKey; // freeze weights while dragging
    }
    if (key == g.selKey) return;
    g.selKey = key;

    g.weights.assign(c.numVerts(), 0.0f);
    if (comp) {
        std::vector<float> w = scene.softWeights(&obj);
        if (w.size() == g.weights.size()) g.weights = std::move(w);
    }
    glBindBuffer(GL_ARRAY_BUFFER, g.cWeight);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(g.weights.size() * sizeof(float)), g.weights.data(), GL_DYNAMIC_DRAW);

    std::vector<uint32_t> se;
    if (comp && scene.mode() == SelectMode::Edge)
        for (uint32_t e : obj.sel.edges)
            if (e < c.numEdges()) se.insert(se.end(), {c.edges[e].v0, c.edges[e].v1});
    glBindBuffer(GL_ARRAY_BUFFER, g.selEdges);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(se.size() * 4), se.data(), GL_DYNAMIC_DRAW);
    g.selEdgeCount = GLsizei(se.size());

    std::vector<uint32_t> st;
    if (comp && scene.mode() == SelectMode::Face && !obj.sel.faces.empty()) {
        std::vector<uint8_t> sel(c.numFaces(), 0);
        for (uint32_t f : obj.sel.faces)
            if (f < sel.size()) sel[f] = 1;
        const Mesh& d = obj.displayMesh();
        const std::vector<uint32_t>* origin = obj.displayFaceOrigin();
        for (uint32_t f = 0; f < d.numFaces(); ++f) {
            uint32_t cf = origin ? (*origin)[f] : f;
            if (!sel[cf]) continue;
            auto fv = d.faceVertices(f);
            for (size_t i = 1; i + 1 < fv.size(); ++i) st.insert(st.end(), {fv[0], fv[i], fv[i + 1]});
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, g.selTris);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(st.size() * 4), st.data(), GL_DYNAMIC_DRAW);
    g.selTriCount = GLsizei(st.size());
}

void Renderer::beginView(const ViewParams& vp, bool)
{
    glViewport(vp.x, vp.y, vp.w, vp.h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(vp.x, vp.y, vp.w, vp.h);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void Renderer::drawBackground(const Vec3& top, const Vec3& bottom)
{
    glDisable(GL_DEPTH_TEST);
    glUseProgram(bgProg_);
    glUniform3f(glGetUniformLocation(bgProg_, "uTop"), top.x, top.y, top.z);
    glUniform3f(glGetUniformLocation(bgProg_, "uBottom"), bottom.x, bottom.y, bottom.z);
    glBindVertexArray(bgVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

void Renderer::drawGrid(const ViewParams& vp, int viewKind, float spacing, int lines)
{
    // Plane axes: persp/top -> XZ, front -> XY, side -> ZY.
    int a = 0, b = 2;
    if (viewKind == 2) { a = 0; b = 1; }
    if (viewKind == 3) { a = 2; b = 1; }
    std::vector<Vec3> minor, major, axisA, axisB;
    const float ext = spacing * lines;
    for (int i = -lines; i <= lines; ++i) {
        float t = i * spacing;
        Vec3 p0, p1, q0, q1;
        p0[a] = t; p0[b] = -ext; p1[a] = t; p1[b] = ext;
        q0[b] = t; q0[a] = -ext; q1[b] = t; q1[a] = ext;
        auto& dst = (i == 0) ? axisA : (i % 5 == 0 ? major : minor);
        dst.insert(dst.end(), {p0, p1});
        auto& dst2 = (i == 0) ? axisB : (i % 5 == 0 ? major : minor);
        dst2.insert(dst2.end(), {q0, q1});
    }
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    drawLines(minor, {0.55f, 0.57f, 0.60f, 0.35f}, vp.viewProj, true);
    drawLines(major, {0.62f, 0.64f, 0.66f, 0.6f}, vp.viewProj, true);
    drawLines(axisA, {0.08f, 0.08f, 0.09f, 0.9f}, vp.viewProj, true);
    drawLines(axisB, {0.08f, 0.08f, 0.09f, 0.9f}, vp.viewProj, true);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

void Renderer::drawScene(Scene& scene, const ViewParams& vp, const DrawOptions& opt)
{
    std::vector<SceneObject*> visible;
    for (auto& o : scene.objects)
        if (o->st.visible) {
            syncObject(scene, *o);
            visible.push_back(o.get());
        }
    SceneObject* lead = scene.lead();
    const bool compMode = scene.mode() != SelectMode::Object;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    if (opt.backfaceCulling) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);

    // ---- shaded pass ----
    if (opt.shading == ShadingMode::Shaded) {
        glUseProgram(meshProg_);
        GLint uMVP = glGetUniformLocation(meshProg_, "uMVP"), uMV = glGetUniformLocation(meshProg_, "uModelView"),
              uNM = glGetUniformLocation(meshProg_, "uNormalMat"), uColor = glGetUniformLocation(meshProg_, "uColor"),
              uAlpha = glGetUniformLocation(meshProg_, "uAlpha"), uFlat = glGetUniformLocation(meshProg_, "uFlat"),
              uChecker = glGetUniformLocation(meshProg_, "uChecker"), uHasMask = glGetUniformLocation(meshProg_, "uHasMask");
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.0f, 1.0f);
        if (opt.xray) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }
        glUniform1i(uFlat, opt.flatShading ? 1 : 0);
        glUniform1f(uAlpha, opt.xray ? 0.45f : 1.0f);
        for (SceneObject* o : visible) {
            GpuMesh* g = gpu(o->id);
            Mat4 model = o->worldMatrix(), mv = vp.view * model, mvp = vp.viewProj * model;
            Mat4 nm4 = normalMatrix(mv);
            float nm[9] = {nm4.m[0], nm4.m[1], nm4.m[2], nm4.m[4], nm4.m[5], nm4.m[6], nm4.m[8], nm4.m[9], nm4.m[10]};
            glUniformMatrix4fv(uMVP, 1, GL_FALSE, mvp.data());
            glUniformMatrix4fv(uMV, 1, GL_FALSE, mv.data());
            glUniformMatrix3fv(uNM, 1, GL_FALSE, nm);
            const bool live = o->id == scene.liveId;
            if (live) {
                glUniform3f(uColor, 0.42f, 0.62f, 0.44f);
                glPolygonOffset(4.0f, 24.0f); // keep the live surface behind retopology drawn onto it
            } else {
                glUniform3f(uColor, 0.58f, 0.59f, 0.60f);
                glPolygonOffset(1.0f, 1.0f);
            }
            const bool tex = opt.textured && !live && o->displayMesh().hasUVs();
            if ((tex || g->hasHard) && uploadSplit(*o, *g, tex)) {
                glUniform1i(uHasMask, g->texMaskOn ? 1 : 0);
                glUniform1i(uChecker, tex ? 1 : 0);
                glBindVertexArray(g->texVao);
                glDrawElements(GL_TRIANGLES, g->texIndexCount, GL_UNSIGNED_INT, nullptr);
                glUniform1i(uChecker, 0);
            } else {
                glUniform1i(uHasMask, g->maskOn ? 1 : 0);
                glBindVertexArray(g->vao);
                glDrawElements(GL_TRIANGLES, g->triIndexCount, GL_UNSIGNED_INT, nullptr);
            }
        }
        glPolygonOffset(1.0f, 1.0f);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }
    glDisable(GL_CULL_FACE);

    // ---- wire / component pass ----
    glUseProgram(lineProg_);
    GLint lMVP = glGetUniformLocation(lineProg_, "uMVP"), lColor = glGetUniformLocation(lineProg_, "uColor"),
          lMode = glGetUniformLocation(lineProg_, "uMode"), lPt = glGetUniformLocation(lineProg_, "uPointSize");
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    const bool shaded = opt.shading == ShadingMode::Shaded;
    for (SceneObject* o : visible) {
        GpuMesh* g = gpu(o->id);
        Mat4 mvp = vp.viewProj * o->worldMatrix();
        glUniformMatrix4fv(lMVP, 1, GL_FALSE, mvp.data());
        glUniform1i(lMode, 0);
        const bool sel = scene.isSelected(o->id);
        const bool hilite = compMode && sel;
        const int smooth = o->displayFaceOrigin() ? o->st.smoothDisplay : 0;

        Vec4 wire = shaded ? Vec4{0.05f, 0.05f, 0.06f, 0.55f} : Vec4{0.02f, 0.05f, 0.35f, 1.0f};
        if (sel && !hilite) wire = (o == lead) ? Vec4{0.26f, 1.0f, 0.64f, 1.0f} : Vec4{1.0f, 1.0f, 1.0f, 1.0f};
        if (hilite) wire = Vec4{0.33f, 0.47f, 0.95f, 1.0f};

        const bool sculpting = scene.tool() == Tool::Sculpt && shaded;
        bool drawCage = !shaded || hilite || (sel && !sculpting) || opt.wireOnShaded || smooth == 1;
        bool drawSmoothWire = smooth == 2 && (!shaded || opt.wireOnShaded) && !sel;
        if (smooth == 2 && !sel && !(shaded && opt.wireOnShaded)) drawCage = false;
        if (smooth == 2 && !shaded && !sel) drawCage = false;

        if (drawSmoothWire && g->dWireCount) {
            Vec4 c = wire;
            if (shaded) c.w *= 0.45f;
            glUniform4f(lColor, c.x, c.y, c.z, c.w);
            glBindVertexArray(g->dWireVao);
            glDrawElements(GL_LINES, g->dWireCount, GL_UNSIGNED_INT, nullptr);
        }
        if (drawCage) {
            Vec4 c = wire;
            if (smooth == 1 && !sel) c = shaded ? Vec4{0.08f, 0.09f, 0.12f, 0.8f} : Vec4{0.1f, 0.15f, 0.45f, 1.0f};
            glUniform4f(lColor, c.x, c.y, c.z, c.w);
            glUniform1i(lMode, hilite && scene.softSelect ? 1 : 0);
            glBindVertexArray(g->cVao);
            glDrawElements(GL_LINES, g->cEdgeCount, GL_UNSIGNED_INT, nullptr);
            glUniform1i(lMode, 0);
        }
        if (opt.showHardEdges && g->hardCount) {
            glUniform1i(lMode, 0);
            glUniform4f(lColor, 0.1f, 0.95f, 1.0f, 1.0f);
            glBindVertexArray(g->hardVao);
            glDrawElements(GL_LINES, g->hardCount, GL_UNSIGNED_INT, nullptr);
        }
        if (!hilite) continue;

        if (scene.mode() == SelectMode::Face && g->selTriCount) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(-1.0f, -1.0f);
            glUniform4f(lColor, 1.0f, 0.45f, 0.05f, 0.42f);
            glBindVertexArray(g->selTriVao);
            glDrawElements(GL_TRIANGLES, g->selTriCount, GL_UNSIGNED_INT, nullptr);
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
        if (scene.mode() == SelectMode::Edge && g->selEdgeCount) {
            glUniform4f(lColor, 1.0f, 0.55f, 0.0f, 1.0f);
            glBindVertexArray(g->selEdgeVao);
            glDepthFunc(GL_ALWAYS);
            glDrawElements(GL_LINES, g->selEdgeCount, GL_UNSIGNED_INT, nullptr);
            glDepthFunc(GL_LEQUAL);
        }
        if (scene.mode() == SelectMode::Vertex) {
            glUniform1i(lMode, 1);
            glUniform4f(lColor, 0.62f, 0.12f, 0.62f, 1.0f);
            glUniform1f(lPt, 5.0f * vp.dpr);
            glBindVertexArray(g->cVao);
            glDrawArrays(GL_POINTS, 0, g->cVertCount);
            glUniform1i(lMode, 0);
        } else if (scene.softSelect) {
            // Show soft-selection falloff as coloured points in edge/face modes as well.
            glUniform1i(lMode, 2);
            glUniform1f(lPt, 4.0f * vp.dpr);
            glBindVertexArray(g->cVao);
            glDrawArrays(GL_POINTS, 0, g->cVertCount);
            glUniform1i(lMode, 0);
        }
    }
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

void Renderer::drawPick(Scene& scene, const ViewParams& vp, bool selectedOnly)
{
    glUseProgram(pickProg_);
    GLint uMVP = glGetUniformLocation(pickProg_, "uMVP"), uObj = glGetUniformLocation(pickProg_, "uObj");
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
    for (auto& o : scene.objects) {
        if (!o->st.visible) continue;
        if (selectedOnly && !scene.isSelected(o->id)) continue;
        syncObject(scene, *o);
        GpuMesh* g = gpu(o->id);
        Mat4 mvp = vp.viewProj * o->worldMatrix();
        glUniformMatrix4fv(uMVP, 1, GL_FALSE, mvp.data());
        glUniform1ui(uObj, o->id);
        glBindVertexArray(g->vao);
        glDrawElements(GL_TRIANGLES, g->triIndexCount, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);
}

void Renderer::drawLines(const std::vector<Vec3>& pts, const Vec4& color, const Mat4& mvp, bool depthTest, GLenum prim)
{
    if (pts.empty()) return;
    glUseProgram(lineProg_);
    glUniformMatrix4fv(glGetUniformLocation(lineProg_, "uMVP"), 1, GL_FALSE, mvp.data());
    glUniform4f(glGetUniformLocation(lineProg_, "uColor"), color.x, color.y, color.z, color.w);
    glUniform1i(glGetUniformLocation(lineProg_, "uMode"), 0);
    if (depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glBindVertexArray(immVao_);
    glBindBuffer(GL_ARRAY_BUFFER, immVbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(pts.size() * sizeof(Vec3)), pts.data(), GL_STREAM_DRAW);
    glDrawArrays(prim, 0, GLsizei(pts.size()));
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::drawPoints(const std::vector<Vec3>& pts, const Vec4& color, const Mat4& mvp, float size, bool depthTest)
{
    glUseProgram(lineProg_);
    glUniform1f(glGetUniformLocation(lineProg_, "uPointSize"), size);
    drawLines(pts, color, mvp, depthTest, GL_POINTS);
}

void Renderer::drawAxisTriad(const ViewParams& vp)
{
    // Small orientation triad in the bottom-left corner.
    const int size = int(70 * vp.dpr);
    glViewport(vp.x, vp.y, size, size);
    glScissor(vp.x, vp.y, size, size);
    Mat4 rot = vp.view;
    rot.at(0, 3) = rot.at(1, 3) = 0;
    rot.at(2, 3) = -3.0f;
    Mat4 mvp = Mat4::perspective(30.0f, 1.0f, 0.1f, 10.0f) * rot;
    glClear(GL_DEPTH_BUFFER_BIT);
    drawLines({{0, 0, 0}, {0.8f, 0, 0}}, {0.95f, 0.2f, 0.2f, 1}, mvp, false);
    drawLines({{0, 0, 0}, {0, 0.8f, 0}}, {0.3f, 0.95f, 0.3f, 1}, mvp, false);
    drawLines({{0, 0, 0}, {0, 0, 0.8f}}, {0.3f, 0.5f, 1.0f, 1}, mvp, false);
    glViewport(vp.x, vp.y, vp.w, vp.h);
    glScissor(vp.x, vp.y, vp.w, vp.h);
}

} // namespace facet
