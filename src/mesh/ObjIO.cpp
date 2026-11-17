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
    // Each object references global vertex indices; we remap them into per-object vertices.
    struct Pending { std::string name; std::vector<uint32_t> faceStart{0}; std::vector<uint32_t> faceVerts; };
    std::vector<Pending> objs(1);
    objs[0].name = "mesh";

    const char* p = data.data();
    const char* end = p + data.size();
    std::vector<uint32_t> poly;
    while (p < end) {
        const char* lineEnd = p;
        while (lineEnd < end && *lineEnd != '\n' && *lineEnd != '\r') ++lineEnd;
        const char* q = skipWs(p, lineEnd);
        if (q + 1 < lineEnd && q[0] == 'v' && (q[1] == ' ' || q[1] == '\t')) {
            q += 1;
            float x = parseFloat(q, lineEnd), y = parseFloat(q, lineEnd), z = parseFloat(q, lineEnd);
            globalPos.push_back({x, y, z});
        } else if (q + 1 < lineEnd && q[0] == 'f' && (q[1] == ' ' || q[1] == '\t')) {
            q += 1;
            poly.clear();
            while (true) {
                q = skipWs(q, lineEnd);
                if (q >= lineEnd) break;
                long idx = 0;
                auto r = std::from_chars(q, lineEnd, idx);
                if (r.ec != std::errc()) break;
                q = r.ptr;
                while (q < lineEnd && *q != ' ' && *q != '\t') ++q; // skip /vt/vn
                long resolved = idx > 0 ? idx - 1 : long(globalPos.size()) + idx;
                if (resolved >= 0) poly.push_back(uint32_t(resolved));
            }
            if (poly.size() >= 3) {
                auto& o = objs.back();
                o.faceVerts.insert(o.faceVerts.end(), poly.begin(), poly.end());
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
        nm.mesh.faceStart = std::move(o.faceStart);
        nm.mesh.faceVerts = std::move(o.faceVerts);
        nm.mesh.buildTopology();
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
    uint64_t base = 1;
    for (const auto& nm : meshes) {
        std::fprintf(f, "o %s\n", nm.name.c_str());
        for (const Vec3& p : nm.mesh.positions) {
            Vec3 w = nm.xform.transformPoint(p);
            std::fprintf(f, "v %.7g %.7g %.7g\n", w.x, w.y, w.z);
        }
        for (uint32_t fi = 0; fi < nm.mesh.numFaces(); ++fi) {
            std::fputc('f', f);
            for (uint32_t v : nm.mesh.faceVertices(fi)) std::fprintf(f, " %llu", (unsigned long long)(v + base));
            std::fputc('\n', f);
        }
        base += nm.mesh.numVerts();
    }
    bool ok = std::ferror(f) == 0;
    std::fclose(f);
    if (!ok && error) *error = "Write error";
    return ok;
}

} // namespace facet
