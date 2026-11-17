// FBX round-trip + ASCII parsing tests.
#include "io/Fbx.h"
#include "mesh/Primitives.h"

#include <cmath>
#include <cstdio>
#include <fstream>

using namespace facet;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("  FAILED line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main(int argc, char** argv)
{
    std::string dir = argc > 1 ? argv[1] : "/tmp";
    // ---- binary round trip ----
    std::vector<FbxMesh> out(2);
    out[0].name = "box";
    out[0].mesh = prim::cube(2, 1, 1, 2, 1, 1);
    out[0].translate = {1, 2, 3};
    out[0].rotate = {10, 20, 30};
    out[0].scale = {1, 2, 1};
    out[1].name = "ball";
    out[1].mesh = prim::sphere(1, 40, 30); // big enough to trigger zlib-compressed arrays
    std::string err;
    std::string path = dir + "/facet_test.fbx";
    CHECK(writeFbx(path, out, &err));
    std::vector<FbxMesh> in;
    CHECK(readFbx(path, in, &err));
    if (!err.empty()) std::printf("  err: %s\n", err.c_str());
    CHECK(in.size() == 2);
    for (const auto& fm : in) {
        const FbxMesh& ref = fm.name == "box" ? out[0] : out[1];
        CHECK(fm.mesh.numVerts() == ref.mesh.numVerts());
        CHECK(fm.mesh.numFaces() == ref.mesh.numFaces());
        CHECK(fm.mesh.hasUVs() && fm.mesh.uvs.size() == ref.mesh.uvs.size());
        CHECK(length(fm.translate - ref.translate) < 1e-4f);
        CHECK(length(fm.rotate - ref.rotate) < 1e-3f);
        CHECK(length(fm.scale - ref.scale) < 1e-4f);
        bool same = true;
        for (uint32_t v = 0; v < ref.mesh.numVerts(); ++v) same &= length(fm.mesh.positions[v] - ref.mesh.positions[v]) < 1e-5f;
        CHECK(same);
    }
    std::printf("%s binary round trip\n", g_fail ? "FAIL" : "PASS");

    // ---- ASCII (hand-written, Maya-style naming, parented model with pre-rotation) ----
    const char* ascii = R"(; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
}
Objects:  {
	Geometry: 100, "Geometry::quad", "Mesh" {
		Vertices: *12 {
			a: 0,0,0,1,0,0,1,0,1,
			0,0,1
		}
		PolygonVertexIndex: *4 {
			a: 0,3,2,-2
		}
		LayerElementUV: 0 {
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "IndexToDirect"
			UV: *8 {
				a: 0,0,1,0,1,1,0,1
			}
			UVIndex: *4 {
				a: 0,3,2,1
			}
		}
	}
	Model: 200, "Model::quadShape", "Mesh" {
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",5,0,0
		}
	}
	Model: 300, "Model::grp", "Null" {
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",0,1,0
			P: "PreRotation", "Vector3D", "Vector", "",0,90,0
		}
	}
}
Connections:  {
	C: "OO",100,200
	C: "OO",200,300
	C: "OO",300,0
}
)";
    std::string apath = dir + "/facet_ascii.fbx";
    std::ofstream(apath) << ascii;
    std::vector<FbxMesh> a;
    CHECK(readFbx(apath, a, &err));
    CHECK(a.size() == 1);
    if (a.size() == 1) {
        CHECK(a[0].name == "quadShape");
        CHECK(a[0].mesh.numFaces() == 1 && a[0].mesh.faceSize(0) == 4);
        CHECK(a[0].mesh.hasUVs() && a[0].mesh.uvs.size() == 4);
        // Parent rotates +90 about Y, so child's +5 X lands at -5 Z; parent lifts by 1.
        CHECK(std::abs(a[0].translate.x) < 1e-4f && std::abs(a[0].translate.z + 5) < 1e-4f && std::abs(a[0].translate.y - 1) < 1e-4f);
        CHECK(std::abs(a[0].rotate.y - 90) < 1e-3f);
    }
    std::printf("%s ascii\n", g_fail ? "FAIL" : "PASS");
    std::printf(g_fail ? "%d IO check(s) failed\n" : "All IO tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
