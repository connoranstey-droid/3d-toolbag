#include "mesh/ObjIO.h"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace facet {

namespace {
const char* skipWs(const char* p, const char* e)
{
    while (p < e && (*p == ' ' || *p == '\t')) ++p;
    return p;
}
float parseFloat(const char*& p, const char* e)
{
    p = skipWs(p, e);
    float v = 0;
    auto r = std::from_chars(p, e, v);
    p = r.ptr;
    return v;
}
} // namespace

bool readObj(const std::string& path, std::vector<NamedMesh>& out, std::string* error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "Cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string data = ss.str();

    std::vector<Vec3> globalPos;
    std::vector<Vec2> globalUV;
    // Each object references global vertex indices; we remap them into per-object vertices.
    struct Pending { std::string name; std::vector<uint32_t> faceStart{0}; std::vector<uint32_t> faceVerts; std::vector<uint32_t> faceUV; bool allUV = true; };
    std::vector<Pending> objs(1);
    objs[0].name = "mesh";

    const char* p = data.data();
    const char* end = p + data.size();
    std::vector<uint32_t> poly, polyUV;
    while (p < end) {
        const char* lineEnd = p;
        while (lineEnd < end && *lineEnd != '\n' && *lineEnd != '\r') ++lineEnd;
        const char* q = skipWs(p, lineEnd);
        if (q + 1 < lineEnd && q[0] == 'v' && (q[1] == ' ' || q[1] == '\t')) {
            q += 1;
            float x = parseFloat(q, lineEnd), y = parseFloat(q, lineEnd), z = parseFloat(q, lineEnd);
            globalPos.push_back({x, y, z});
        } else if (q + 2 < lineEnd && q[0] == 'v' && q[1] == 't' && (q[2] == ' ' || q[2] == '\t')) {
            q += 2;
            float u = parseFloat(q, lineEnd), v = parseFloat(q, lineEnd);
            globalUV.push_back({u, v});
        } else if (q + 1 < lineEnd && q[0] == 'f' && (q[1] == ' ' || q[1] == '\t')) {
            q += 1;
            poly.clear();
            polyUV.clear();
            bool faceHasUV = true;
            while (true) {
                q = skipWs(q, lineEnd);
                if (q >= lineEnd) break;
                long idx = 0;
                auto r = std::from_chars(q, lineEnd, idx);
                if (r.ec != std::errc()) break;
                q = r.ptr;
                long vt = 0;
                bool gotVT = false;
                if (q < lineEnd && *q == '/') {
                    ++q;
                    auto r2 = std::from_chars(q, lineEnd, vt);
                    if (r2.ec == std::errc()) { gotVT = true; q = r2.ptr; }
                }
                while (q < lineEnd && *q != ' ' && *q != '\t') ++q; // skip /vn
                long resolved = idx > 0 ? idx - 1 : long(globalPos.size()) + idx;
                long resolvedUV = vt > 0 ? vt - 1 : long(globalUV.size()) + vt;
                if (resolved >= 0) {
                    poly.push_back(uint32_t(resolved));
                    bool okUV = gotVT && resolvedUV >= 0 && resolvedUV < long(globalUV.size());
                    faceHasUV &= okUV;
                    polyUV.push_back(okUV ? uint32_t(resolvedUV) : 0u);
                }
            }
            if (poly.size() >= 3) {
                auto& o = objs.back();
                o.faceVerts.insert(o.faceVerts.end(), poly.begin(), poly.end());
                o.faceUV.insert(o.faceUV.end(), polyUV.begin(), polyUV.end());
                o.allUV &= faceHasUV;
                o.faceStart.push_back(uint32_t(o.faceVerts.size()));
            }
        } else if (q + 1 < lineEnd && (q[0] == 'o' || q[0] == 'g') && (q[1] == ' ' || q[1] == '\t')) {
            std::string name(skipWs(q + 1, lineEnd), lineEnd);
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
            if (objs.back().faceVerts.empty()) objs.back().name = name.empty() ? "mesh" : name;
            else { objs.emplace_back(); objs.back().name = name.empty() ? "mesh" : name; }
        }
        p = lineEnd;
        while (p < end && (*p == '\n' || *p == '\r')) ++p;
    }

    std::vector<uint32_t> local(globalPos.size(), kInvalid);
    for (auto& o : objs) {
        if (o.faceVerts.empty()) continue;
        NamedMesh nm;
        nm.name = o.name;
        std::vector<uint32_t> touched;
        bool bad = false;
        for (uint32_t& v : o.faceVerts) {
            if (v >= globalPos.size()) { bad = true; break; }
            if (local[v] == kInvalid) {
                local[v] = nm.mesh.addVertex(globalPos[v]);
                touched.push_back(v);
            }
            v = local[v];
        }
        for (uint32_t t : touched) local[t] = kInvalid;
        if (bad) {
            if (error) *error = "Face references a missing vertex";
            return false;
        }
        if (o.allUV && !globalUV.empty()) {
            // Localise the referenced uvs.
            std::vector<uint32_t> uvLocal(globalUV.size(), kInvalid);
            for (uint32_t& u : o.faceUV) {
                if (uvLocal[u] == kInvalid) { uvLocal[u] = uint32_t(nm.mesh.uvs.size()); nm.mesh.uvs.push_back(globalUV[u]); }
                u = uvLocal[u];
            }
            nm.mesh.faceUVs = std::move(o.faceUV);
        }
        nm.mesh.faceStart = std::move(o.faceStart);
        nm.mesh.faceVerts = std::move(o.faceVerts);
        nm.mesh.buildTopology();
        nm.mesh.touchUVs();
        out.push_back(std::move(nm));
    }
    return true;
}

bool writeObj(const std::string& path, const std::vector<NamedMesh>& meshes, std::string* error)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    std::fprintf(f, "# Exported by Facet\n");
    uint64_t base = 1, uvBase = 1;
    for (const auto& nm : meshes) {
        const Mesh& m = nm.mesh;
        std::fprintf(f, "o %s\n", nm.name.c_str());
        for (const Vec3& p : m.positions) {
            Vec3 w = nm.xform.transformPoint(p);
            std::fprintf(f, "v %.7g %.7g %.7g\n", w.x, w.y, w.z);
        }
        const bool uv = m.hasUVs();
        if (uv)
            for (const Vec2& t : m.uvs) std::fprintf(f, "vt %.7g %.7g\n", t.x, t.y);
        for (uint32_t fi = 0; fi < m.numFaces(); ++fi) {
            std::fputc('f', f);
            for (uint32_t h = m.faceStart[fi]; h < m.faceStart[fi + 1]; ++h) {
                if (uv) std::fprintf(f, " %llu/%llu", (unsigned long long)(m.faceVerts[h] + base), (unsigned long long)(m.faceUVs[h] + uvBase));
                else std::fprintf(f, " %llu", (unsigned long long)(m.faceVerts[h] + base));
            }
            std::fputc('\n', f);
        }
        base += m.numVerts();
        if (uv) uvBase += m.uvs.size();
    }
    bool ok = std::ferror(f) == 0;
    std::fclose(f);
    if (!ok && error) *error = "Write error";
    return ok;
}

} // namespace facet
