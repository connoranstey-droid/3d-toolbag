#include "io/Fbx.h"

#include <QByteArray>
#include <QFile>

#include <cstring>
#include <map>
#include <unordered_map>

namespace facet {

namespace {

// ------------------------------------------------------------------------------------------
// Generic node tree (shared by the binary and ASCII readers)

struct Prop {
    char type = 0; // 'I' integer-ish, 'D' real, 'S' string/raw, 'i' int array, 'd' real array
    int64_t i = 0;
    double d = 0;
    std::string s;
    std::vector<int64_t> ia;
    std::vector<double> da;
    double num() const { return type == 'D' ? d : double(i); }
    int64_t integer() const { return type == 'D' ? int64_t(d) : i; }
};

struct Node {
    std::string name;
    std::vector<Prop> props;
    std::vector<Node> children;
    const Node* child(const char* n) const
    {
        for (const auto& c : children)
            if (c.name == n) return &c;
        return nullptr;
    }
};

// ---- binary ----

struct BinReader {
    const uint8_t* p;
    size_t size;
    uint32_t version;
    std::string err;

    template <class T>
    bool rd(size_t& at, T& v)
    {
        if (at + sizeof(T) > size) { err = "Unexpected end of file"; return false; }
        std::memcpy(&v, p + at, sizeof(T));
        at += sizeof(T);
        return true;
    }

    bool readArray(size_t& at, char t, Prop& out)
    {
        uint32_t len, enc, clen;
        if (!rd(at, len) || !rd(at, enc) || !rd(at, clen)) return false;
        size_t elem = (t == 'd' || t == 'l') ? 8 : (t == 'b' ? 1 : 4);
        if (at + clen > size) { err = "Array out of range"; return false; }
        QByteArray raw;
        if (enc == 1) {
            QByteArray z(4, 0);
            uint32_t expect = uint32_t(len * elem);
            z[0] = char((expect >> 24) & 0xff); z[1] = char((expect >> 16) & 0xff);
            z[2] = char((expect >> 8) & 0xff); z[3] = char(expect & 0xff);
            z.append(reinterpret_cast<const char*>(p + at), qsizetype(clen));
            raw = qUncompress(z);
            if (size_t(raw.size()) != len * elem) { err = "Failed to decompress array"; return false; }
        } else {
            raw = QByteArray(reinterpret_cast<const char*>(p + at), qsizetype(len * elem));
        }
        at += clen;
        const char* b = raw.constData();
        if (t == 'd' || t == 'f') {
            out.type = 'd';
            out.da.resize(len);
            for (uint32_t k = 0; k < len; ++k) {
                if (t == 'd') { double v; std::memcpy(&v, b + k * 8, 8); out.da[k] = v; }
                else { float v; std::memcpy(&v, b + k * 4, 4); out.da[k] = v; }
            }
        } else {
            out.type = 'i';
            out.ia.resize(len);
            for (uint32_t k = 0; k < len; ++k) {
                if (t == 'l') { int64_t v; std::memcpy(&v, b + k * 8, 8); out.ia[k] = v; }
                else if (t == 'i') { int32_t v; std::memcpy(&v, b + k * 4, 4); out.ia[k] = v; }
                else out.ia[k] = uint8_t(b[k]);
            }
        }
        return true;
    }

    // Returns false on error; sets `isNull` when the record is the list terminator.
    bool readNode(size_t& at, Node& node, bool& isNull)
    {
        uint64_t end, nprops, plen;
        if (version >= 7500) {
            if (!rd(at, end) || !rd(at, nprops) || !rd(at, plen)) return false;
        } else {
            uint32_t e32, n32, l32;
            if (!rd(at, e32) || !rd(at, n32) || !rd(at, l32)) return false;
            end = e32; nprops = n32; plen = l32;
        }
        uint8_t nameLen;
        if (!rd(at, nameLen)) return false;
        isNull = end == 0;
        if (isNull) return true;
        if (end > size || at + nameLen > size) { err = "Corrupt node"; return false; }
        node.name.assign(reinterpret_cast<const char*>(p + at), nameLen);
        at += nameLen;
        for (uint64_t k = 0; k < nprops; ++k) {
            uint8_t t;
            if (!rd(at, t)) return false;
            Prop pr;
            switch (t) {
            case 'Y': { int16_t v; if (!rd(at, v)) return false; pr.type = 'I'; pr.i = v; break; }
            case 'C': { uint8_t v; if (!rd(at, v)) return false; pr.type = 'I'; pr.i = v; break; }
            case 'I': { int32_t v; if (!rd(at, v)) return false; pr.type = 'I'; pr.i = v; break; }
            case 'L': { int64_t v; if (!rd(at, v)) return false; pr.type = 'I'; pr.i = v; break; }
            case 'F': { float v; if (!rd(at, v)) return false; pr.type = 'D'; pr.d = v; break; }
            case 'D': { double v; if (!rd(at, v)) return false; pr.type = 'D'; pr.d = v; break; }
            case 'S': case 'R': {
                uint32_t len;
                if (!rd(at, len)) return false;
                if (at + len > size) { err = "String out of range"; return false; }
                pr.type = 'S';
                pr.s.assign(reinterpret_cast<const char*>(p + at), len);
                at += len;
                break;
            }
            case 'f': case 'd': case 'l': case 'i': case 'b':
                if (!readArray(at, char(t), pr)) return false;
                break;
            default: err = std::string("Unknown property type ") + char(t); return false;
            }
            node.props.push_back(std::move(pr));
        }
        while (at < end) {
            Node c;
            bool nul = false;
            if (!readNode(at, c, nul)) return false;
            if (nul) break;
            node.children.push_back(std::move(c));
        }
        at = end;
        return true;
    }
};

// ---- ASCII ----

struct AsciiReader {
    const char* p;
    const char* e;
    void ws()
    {
        while (p < e) {
            if (*p == ';') { while (p < e && *p != '\n') ++p; }
            else if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
            else break;
        }
    }
    void wsLine() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\r')) ++p; }
    bool value(Prop& pr)
    {
        wsLine();
        if (p >= e) return false;
        if (*p == '"') {
            const char* s = ++p;
            while (p < e && *p != '"') ++p;
            pr.type = 'S';
            pr.s.assign(s, p);
            if (p < e) ++p;
            return true;
        }
        if (*p == '*') { // array size marker
            ++p;
            const char* s = p;
            while (p < e && std::isdigit(uint8_t(*p))) ++p;
            pr.type = '*';
            pr.i = std::strtoll(std::string(s, p).c_str(), nullptr, 10);
            return true;
        }
        const char* s = p;
        while (p < e && *p != ',' && *p != '\n' && *p != '\r' && *p != '{' && *p != '}' && *p != ' ' && *p != '\t') ++p;
        std::string tok(s, p);
        if (tok.empty()) return false;
        bool isNum = std::isdigit(uint8_t(tok[0])) || ((tok[0] == '-' || tok[0] == '+' || tok[0] == '.') && tok.size() > 1);
        if (isNum) {
            if (tok.find_first_of(".eE") != std::string::npos) { pr.type = 'D'; pr.d = std::strtod(tok.c_str(), nullptr); }
            else { pr.type = 'I'; pr.i = std::strtoll(tok.c_str(), nullptr, 10); }
        } else {
            pr.type = 'S';
            pr.s = tok;
        }
        return true;
    }
    void list(std::vector<Node>& out)
    {
        while (true) {
            ws();
            if (p >= e) return;
            if (*p == '}') { ++p; return; }
            const char* s = p;
            while (p < e && *p != ':' && *p != '\n') ++p;
            if (p >= e || *p != ':') { ++p; continue; }
            Node n;
            n.name.assign(s, p);
            while (!n.name.empty() && (n.name.back() == ' ' || n.name.back() == '\t')) n.name.pop_back();
            ++p;
            while (true) {
                Prop pr;
                wsLine();
                if (p < e && (*p == '{' || *p == '\n' || *p == '}')) break;
                if (!value(pr)) break;
                n.props.push_back(std::move(pr));
                wsLine();
                if (p < e && *p == ',') { ++p; ws(); continue; }
                break;
            }
            wsLine();
            if (p < e && *p == '{') {
                ++p;
                list(n.children);
            }
            // Collapse "Name: *N { a: v,v,v }" into an array property.
            if (!n.props.empty() && n.props[0].type == '*') {
                Prop arr;
                bool real = false;
                if (const Node* a = n.child("a")) {
                    for (const auto& v : a->props) real |= v.type == 'D';
                    arr.type = real ? 'd' : 'i';
                    for (const auto& v : a->props) {
                        if (real) arr.da.push_back(v.num());
                        else arr.ia.push_back(v.i);
                    }
                }
                n.props = {arr};
                n.children.clear();
            }
            out.push_back(std::move(n));
        }
    }
};

std::string cleanName(const std::string& s)
{
    // Binary: "Name\x00\x01Class"; ASCII: "Class::Name".
    size_t z = s.find('\0');
    if (z != std::string::npos) return s.substr(0, z);
    size_t c = s.find("::");
    if (c != std::string::npos) return s.substr(c + 2);
    return s;
}

std::vector<double> realArray(const Node* n)
{
    if (!n || n->props.empty()) return {};
    const Prop& p = n->props[0];
    if (p.type == 'd') return p.da;
    std::vector<double> out;
    for (int64_t v : p.ia) out.push_back(double(v));
    return out;
}

std::vector<int64_t> intArray(const Node* n)
{
    if (!n || n->props.empty()) return {};
    const Prop& p = n->props[0];
    if (p.type == 'i') return p.ia;
    std::vector<int64_t> out;
    for (double v : p.da) out.push_back(int64_t(v));
    return out;
}

std::string strChild(const Node* n, const char* name)
{
    const Node* c = n ? n->child(name) : nullptr;
    return c && !c->props.empty() ? c->props[0].s : std::string();
}

Vec3 decomposeEuler(const Mat4& m, Vec3& t, Vec3& s)
{
    t = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    Vec3 c0{m.at(0, 0), m.at(1, 0), m.at(2, 0)}, c1{m.at(0, 1), m.at(1, 1), m.at(2, 1)}, c2{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
    s = {length(c0), length(c1), length(c2)};
    if (dot(cross(c0, c1), c2) < 0) s.x = -s.x;
    Mat4 r;
    for (int i = 0; i < 3; ++i) {
        r.at(i, 0) = (i == 0 ? c0.x : i == 1 ? c0.y : c0.z) / (s.x != 0 ? s.x : 1);
        r.at(i, 1) = (i == 0 ? c1.x : i == 1 ? c1.y : c1.z) / (s.y != 0 ? s.y : 1);
        r.at(i, 2) = (i == 0 ? c2.x : i == 1 ? c2.y : c2.z) / (s.z != 0 ? s.z : 1);
    }
    return eulerXYZ(r);
}

Mesh buildMesh(const Node& g)
{
    Mesh m;
    auto verts = realArray(g.child("Vertices"));
    auto pvi = intArray(g.child("PolygonVertexIndex"));
    for (size_t i = 0; i + 2 < verts.size(); i += 3) m.addVertex({float(verts[i]), float(verts[i + 1]), float(verts[i + 2])});
    std::vector<uint32_t> poly;
    std::vector<int64_t> pvIndexOfCorner; // polygon-vertex index for each kept corner
    std::vector<int64_t> polyPV;
    for (size_t k = 0; k < pvi.size(); ++k) {
        int64_t v = pvi[k];
        bool last = v < 0;
        if (last) v = ~v;
        if (v >= 0 && v < int64_t(m.numVerts())) { poly.push_back(uint32_t(v)); polyPV.push_back(int64_t(k)); }
        if (last) {
            if (poly.size() >= 3) {
                m.faceVerts.insert(m.faceVerts.end(), poly.begin(), poly.end());
                m.faceStart.push_back(uint32_t(m.faceVerts.size()));
                pvIndexOfCorner.insert(pvIndexOfCorner.end(), polyPV.begin(), polyPV.end());
            }
            poly.clear();
            polyPV.clear();
        }
    }
    // First UV layer.
    if (const Node* luv = g.child("LayerElementUV")) {
        auto uvv = realArray(luv->child("UV"));
        auto uvi = intArray(luv->child("UVIndex"));
        std::string map = strChild(luv, "MappingInformationType"), ref = strChild(luv, "ReferenceInformationType");
        const bool byVertex = map == "ByVertice" || map == "ByVertex" || map == "ByControlPoint";
        const bool indexed = ref == "IndexToDirect" || ref == "Index";
        for (size_t i = 0; i + 1 < uvv.size(); i += 2) m.uvs.push_back({float(uvv[i]), float(uvv[i + 1])});
        bool ok = !m.uvs.empty();
        for (size_t c = 0; ok && c < m.faceVerts.size(); ++c) {
            int64_t key = byVertex ? int64_t(m.faceVerts[c]) : pvIndexOfCorner[c];
            int64_t u = indexed ? (key < int64_t(uvi.size()) ? uvi[size_t(key)] : -1) : key;
            if (u < 0 || u >= int64_t(m.uvs.size())) { ok = false; break; }
            m.faceUVs.push_back(uint32_t(u));
        }
        if (!ok) { m.uvs.clear(); m.faceUVs.clear(); }
        else m.compactUVs();
    }
    m.buildTopology();
    m.touchUVs();
    return m;
}

} // namespace

bool readFbx(const std::string& path, std::vector<FbxMesh>& out, std::string* error)
{
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "Cannot open " + path;
        return false;
    }
    const QByteArray data = f.readAll();
    std::vector<Node> top;
    static const char kMagic[] = "Kaydara FBX Binary  ";
    if (data.size() > 27 && std::memcmp(data.constData(), kMagic, 20) == 0) {
        BinReader r{reinterpret_cast<const uint8_t*>(data.constData()), size_t(data.size()), 0, {}};
        std::memcpy(&r.version, data.constData() + 23, 4);
        size_t at = 27;
        while (at < r.size) {
            Node n;
            bool nul = false;
            if (!r.readNode(at, n, nul)) {
                if (error) *error = "FBX parse error: " + r.err;
                return false;
            }
            if (nul) break;
            top.push_back(std::move(n));
        }
    } else {
        AsciiReader r{data.constData(), data.constData() + data.size()};
        r.list(top);
        if (top.empty()) {
            if (error) *error = "Not an FBX file";
            return false;
        }
    }

    const Node* objects = nullptr;
    const Node* conns = nullptr;
    for (const auto& n : top) {
        if (n.name == "Objects") objects = &n;
        if (n.name == "Connections") conns = &n;
    }
    if (!objects) {
        if (error) *error = "FBX has no Objects section";
        return false;
    }
    std::unordered_map<int64_t, const Node*> geoms, models;
    for (const auto& n : objects->children) {
        if (n.props.empty()) continue;
        int64_t id = n.props[0].integer();
        if (n.name == "Geometry" && n.child("Vertices")) geoms[id] = &n;
        if (n.name == "Model") models[id] = &n;
    }
    std::unordered_map<int64_t, int64_t> geomToModel, modelParent;
    if (conns)
        for (const auto& c : conns->children) {
            if (c.name != "C" || c.props.size() < 3 || c.props[0].s != "OO") continue;
            int64_t child = c.props[1].integer(), parent = c.props[2].integer();
            if (geoms.count(child) && models.count(parent)) geomToModel[child] = parent;
            if (models.count(child) && models.count(parent)) modelParent[child] = parent;
        }

    auto localMatrix = [&](const Node* model) {
        Vec3 t, r, s{1, 1, 1}, pre;
        if (const Node* p70 = model->child("Properties70"))
            for (const auto& pr : p70->children) {
                if (pr.props.size() < 7 || pr.props[0].type != 'S') continue;
                Vec3 v{float(pr.props[4].num()), float(pr.props[5].num()), float(pr.props[6].num())};
                const std::string& k = pr.props[0].s;
                if (k == "Lcl Translation") t = v;
                else if (k == "Lcl Rotation") r = v;
                else if (k == "Lcl Scaling") s = v;
                else if (k == "PreRotation") pre = v;
            }
        return Mat4::translate(t) * Mat4::rotateXYZ(pre) * Mat4::rotateXYZ(r) * Mat4::scale(s);
    };
    auto worldMatrix = [&](int64_t id) {
        Mat4 m = localMatrix(models[id]);
        int guard = 0;
        for (auto it = modelParent.find(id); it != modelParent.end() && ++guard < 64; it = modelParent.find(it->second))
            m = localMatrix(models[it->second]) * m;
        return m;
    };

    for (auto& [gid, g] : geoms) {
        FbxMesh fm;
        fm.mesh = buildMesh(*g);
        if (fm.mesh.numFaces() == 0) continue;
        auto it = geomToModel.find(gid);
        if (it != geomToModel.end()) {
            const Node* model = models[it->second];
            fm.name = model->props.size() > 1 ? cleanName(model->props[1].s) : "mesh";
            fm.rotate = decomposeEuler(worldMatrix(it->second), fm.translate, fm.scale);
        } else {
            fm.name = g->props.size() > 1 ? cleanName(g->props[1].s) : "mesh";
        }
        out.push_back(std::move(fm));
    }
    // Stable order (file order of geometry).
    std::map<int64_t, size_t> order;
    if (out.empty()) {
        if (error) *error = "FBX contains no polygon meshes";
        return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------
// Binary writer (FBX 7.4)

namespace {

struct WProp {
    char type;
    QByteArray bytes; // already-encoded payload (without type byte)
};

struct WNode {
    std::string name;
    std::vector<WProp> props;
    std::vector<WNode> children;

    template <class T>
    static QByteArray raw(const T& v) { return QByteArray(reinterpret_cast<const char*>(&v), sizeof(T)); }
    WNode& I(int32_t v) { props.push_back({'I', raw(v)}); return *this; }
    WNode& L(int64_t v) { props.push_back({'L', raw(v)}); return *this; }
    WNode& D(double v) { props.push_back({'D', raw(v)}); return *this; }
    WNode& C(bool v) { uint8_t b = v; props.push_back({'C', raw(b)}); return *this; }
    WNode& S(const std::string& s)
    {
        uint32_t n = uint32_t(s.size());
        props.push_back({'S', raw(n) + QByteArray(s.data(), qsizetype(s.size()))});
        return *this;
    }
    WNode& R(const QByteArray& b)
    {
        uint32_t n = uint32_t(b.size());
        props.push_back({'R', raw(n) + b});
        return *this;
    }
    template <class T>
    WNode& array(char t, const std::vector<T>& v)
    {
        QByteArray data(reinterpret_cast<const char*>(v.data()), qsizetype(v.size() * sizeof(T)));
        uint32_t len = uint32_t(v.size()), enc = 0;
        if (data.size() > 128) {
            QByteArray z = qCompress(data, 6).mid(4); // strip Qt's length prefix -> plain zlib stream
            if (z.size() < data.size()) { data = z; enc = 1; }
        }
        uint32_t clen = uint32_t(data.size());
        props.push_back({t, raw(len) + raw(enc) + raw(clen) + data});
        return *this;
    }
    WNode& child(const std::string& n) { children.push_back({n, {}, {}}); return children.back(); }
};

void writeNode(QByteArray& buf, const WNode& n)
{
    const qsizetype start = buf.size();
    uint32_t zero = 0, np = uint32_t(n.props.size());
    buf.append(WNode::raw(zero));
    buf.append(WNode::raw(np));
    buf.append(WNode::raw(zero));
    buf.append(char(n.name.size()));
    buf.append(n.name.data(), qsizetype(n.name.size()));
    const qsizetype ps = buf.size();
    for (const auto& p : n.props) {
        buf.append(p.type);
        buf.append(p.bytes);
    }
    uint32_t plen = uint32_t(buf.size() - ps);
    std::memcpy(buf.data() + start + 8, &plen, 4);
    if (!n.children.empty() || n.props.empty()) {
        for (const auto& c : n.children) writeNode(buf, c);
        buf.append(QByteArray(13, 0));
    }
    uint32_t end = uint32_t(buf.size());
    std::memcpy(buf.data() + start, &end, 4);
}

// P "name", "type", "label", "flags", values...
WNode& P(WNode& p70, const std::string& name, const std::string& type, const std::string& label, const std::string& flags)
{
    return p70.child("P").S(name).S(type).S(label).S(flags);
}

} // namespace

bool writeFbx(const std::string& path, const std::vector<FbxMesh>& meshes, std::string* error)
{
    const uint32_t version = 7400;
    std::vector<WNode> top;

    // Header extension with the fixed timestamp the FBX SDK pairs with the file id/footer below.
    {
        WNode h{"FBXHeaderExtension", {}, {}};
        h.child("FBXHeaderVersion").I(1003);
        h.child("FBXVersion").I(int32_t(version));
        h.child("EncryptionType").I(0);
        WNode& ts = h.child("CreationTimeStamp");
        ts.child("Version").I(1000);
        ts.child("Year").I(1970); ts.child("Month").I(1); ts.child("Day").I(1);
        ts.child("Hour").I(10); ts.child("Minute").I(0); ts.child("Second").I(0); ts.child("Millisecond").I(0);
        h.child("Creator").S("Facet");
        top.push_back(std::move(h));
    }
    static const uint8_t kFileId[16] = {0x28, 0xb3, 0x2a, 0xeb, 0xb6, 0x24, 0xcc, 0xc2, 0xbf, 0xc8, 0xb0, 0x2a, 0xa9, 0x2b, 0xfc, 0xf1};
    top.push_back({"FileId", {}, {}});
    top.back().R(QByteArray(reinterpret_cast<const char*>(kFileId), 16));
    top.push_back({"CreationTime", {}, {}});
    top.back().S("1970-01-01 10:00:00:000");
    top.push_back({"Creator", {}, {}});
    top.back().S("Facet");
    {
        WNode g{"GlobalSettings", {}, {}};
        g.child("Version").I(1000);
        WNode& p = g.child("Properties70");
        P(p, "UpAxis", "int", "Integer", "").I(1);
        P(p, "UpAxisSign", "int", "Integer", "").I(1);
        P(p, "FrontAxis", "int", "Integer", "").I(2);
        P(p, "FrontAxisSign", "int", "Integer", "").I(1);
        P(p, "CoordAxis", "int", "Integer", "").I(0);
        P(p, "CoordAxisSign", "int", "Integer", "").I(1);
        P(p, "OriginalUpAxis", "int", "Integer", "").I(1);
        P(p, "OriginalUpAxisSign", "int", "Integer", "").I(1);
        P(p, "UnitScaleFactor", "double", "Number", "").D(1.0);
        P(p, "OriginalUnitScaleFactor", "double", "Number", "").D(1.0);
        top.push_back(std::move(g));
    }
    {
        WNode d{"Documents", {}, {}};
        d.child("Count").I(1);
        WNode& doc = d.child("Document");
        doc.L(1000000).S("Scene").S("Scene");
        doc.child("Properties70");
        doc.child("RootNode").L(0);
        top.push_back(std::move(d));
    }
    top.push_back({"References", {}, {}});
    {
        WNode def{"Definitions", {}, {}};
        def.child("Version").I(100);
        def.child("Count").I(int32_t(1 + meshes.size() * 2 + 1));
        def.child("ObjectType").S("GlobalSettings").child("Count").I(1);
        def.child("ObjectType").S("Model").child("Count").I(int32_t(meshes.size()));
        def.child("ObjectType").S("Geometry").child("Count").I(int32_t(meshes.size()));
        def.child("ObjectType").S("Material").child("Count").I(1);
        top.push_back(std::move(def));
    }

    WNode objects{"Objects", {}, {}};
    WNode conns{"Connections", {}, {}};
    const int64_t matId = 2000000;
    {
        WNode& mat = objects.child("Material");
        mat.L(matId).S(std::string("lambert1\0\1Material", 18)).S("");
        mat.child("Version").I(102);
        mat.child("ShadingModel").S("lambert");
        mat.child("MultiLayer").I(0);
        WNode& p = mat.child("Properties70");
        P(p, "DiffuseColor", "Color", "", "A").D(0.5).D(0.5).D(0.5);
    }
    int64_t nextId = 3000000;
    for (const FbxMesh& fm : meshes) {
        const Mesh& m = fm.mesh;
        const int64_t gid = nextId++, mid = nextId++;
        WNode& g = objects.child("Geometry");
        g.L(gid).S(fm.name + std::string("\0\1Geometry", 10)).S("Mesh");
        g.child("Properties70");
        g.child("GeometryVersion").I(124);
        std::vector<double> verts;
        for (const Vec3& p : m.positions) { verts.push_back(p.x); verts.push_back(p.y); verts.push_back(p.z); }
        g.child("Vertices").array('d', verts);
        std::vector<int32_t> pvi;
        for (uint32_t f = 0; f < m.numFaces(); ++f)
            for (uint32_t h = m.faceStart[f]; h < m.faceStart[f + 1]; ++h)
                pvi.push_back(h + 1 == m.faceStart[f + 1] ? ~int32_t(m.faceVerts[h]) : int32_t(m.faceVerts[h]));
        g.child("PolygonVertexIndex").array('i', pvi);
        // Smooth per-vertex normals expanded per polygon-vertex.
        std::vector<Vec3> vn;
        m.computeVertexNormals(vn);
        std::vector<double> normals;
        for (uint32_t v : m.faceVerts) { normals.push_back(vn[v].x); normals.push_back(vn[v].y); normals.push_back(vn[v].z); }
        {
            WNode& ln = g.child("LayerElementNormal");
            ln.I(0);
            ln.child("Version").I(101);
            ln.child("Name").S("");
            ln.child("MappingInformationType").S("ByPolygonVertex");
            ln.child("ReferenceInformationType").S("Direct");
            ln.child("Normals").array('d', normals);
        }
        const bool uv = m.hasUVs();
        if (uv) {
            WNode& lu = g.child("LayerElementUV");
            lu.I(0);
            lu.child("Version").I(101);
            lu.child("Name").S("map1");
            lu.child("MappingInformationType").S("ByPolygonVertex");
            lu.child("ReferenceInformationType").S("IndexToDirect");
            std::vector<double> uvd;
            for (const Vec2& t : m.uvs) { uvd.push_back(t.x); uvd.push_back(t.y); }
            lu.child("UV").array('d', uvd);
            std::vector<int32_t> uvi(m.faceUVs.begin(), m.faceUVs.end());
            lu.child("UVIndex").array('i', uvi);
        }
        {
            WNode& lm = g.child("LayerElementMaterial");
            lm.I(0);
            lm.child("Version").I(101);
            lm.child("Name").S("");
            lm.child("MappingInformationType").S("AllSame");
            lm.child("ReferenceInformationType").S("IndexToDirect");
            lm.child("Materials").array('i', std::vector<int32_t>{0});
        }
        {
            WNode& layer = g.child("Layer");
            layer.I(0);
            layer.child("Version").I(100);
            WNode& a = layer.child("LayerElement");
            a.child("Type").S("LayerElementNormal");
            a.child("TypedIndex").I(0);
            if (uv) {
                WNode& b = layer.child("LayerElement");
                b.child("Type").S("LayerElementUV");
                b.child("TypedIndex").I(0);
            }
            WNode& c = layer.child("LayerElement");
            c.child("Type").S("LayerElementMaterial");
            c.child("TypedIndex").I(0);
        }

        WNode& model = objects.child("Model");
        model.L(mid).S(fm.name + std::string("\0\1Model", 7)).S("Mesh");
        model.child("Version").I(232);
        WNode& p = model.child("Properties70");
        P(p, "Lcl Translation", "Lcl Translation", "", "A").D(fm.translate.x).D(fm.translate.y).D(fm.translate.z);
        P(p, "Lcl Rotation", "Lcl Rotation", "", "A").D(fm.rotate.x).D(fm.rotate.y).D(fm.rotate.z);
        P(p, "Lcl Scaling", "Lcl Scaling", "", "A").D(fm.scale.x).D(fm.scale.y).D(fm.scale.z);
        P(p, "DefaultAttributeIndex", "int", "Integer", "").I(0);
        P(p, "InheritType", "enum", "", "").I(1);
        model.child("MultiLayer").I(0);
        model.child("MultiTake").I(0);
        model.child("Shading").C(true);
        model.child("Culling").S("CullingOff");

        conns.child("C").S("OO").L(mid).L(0);
        conns.child("C").S("OO").L(gid).L(mid);
        conns.child("C").S("OO").L(matId).L(mid);
    }
    top.push_back(std::move(objects));
    top.push_back(std::move(conns));
    {
        WNode takes{"Takes", {}, {}};
        takes.child("Current").S("");
        top.push_back(std::move(takes));
    }

    QByteArray buf;
    buf.append("Kaydara FBX Binary  ", 20);
    buf.append('\0');
    buf.append('\x1a');
    buf.append('\0');
    buf.append(WNode::raw(version));
    for (const auto& n : top) writeNode(buf, n);
    buf.append(QByteArray(13, 0)); // end of top-level list
    static const uint8_t kFootId[16] = {0xfa, 0xbc, 0xab, 0x09, 0xd0, 0xc8, 0xd4, 0x66, 0xb1, 0x76, 0xfb, 0x83, 0x1c, 0xf7, 0x26, 0x7e};
    buf.append(reinterpret_cast<const char*>(kFootId), 16);
    buf.append(QByteArray(4, 0));
    qsizetype pad = ((buf.size() + 15) & ~qsizetype(15)) - buf.size();
    if (pad == 0) pad = 16;
    buf.append(QByteArray(pad, 0));
    buf.append(WNode::raw(version));
    buf.append(QByteArray(120, 0));
    static const uint8_t kFootMagic[16] = {0xf8, 0x5a, 0x8c, 0x6a, 0xde, 0xf5, 0xd9, 0x7e, 0xec, 0xe9, 0x0c, 0xe3, 0x75, 0x8f, 0x29, 0x0b};
    buf.append(reinterpret_cast<const char*>(kFootMagic), 16);

    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::WriteOnly) || f.write(buf) != buf.size()) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    return true;
}

} // namespace facet
