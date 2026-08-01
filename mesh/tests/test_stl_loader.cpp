// Standalone test for the STL loader, run against the REAL mesh files this project ships
// (Meshes/openwheel/*.stl -- copied verbatim from the controls repo's sim/models/meshes/openwheel/,
// see mesh/README.md). Builds and runs with plain clang++, no Unreal dependency.
//
// Checks two things:
//   1. All seven files parse as valid binary STL with a sane triangle count.
//   2. The parsed bounding boxes match the specific facts overboard_onewheel.xml's authors
//      recorded from the raw STL data -- front_enclosure spans x = -431.8..-145.4 mm, and the
//      front/rear enclosure pair mirrors across X=0 with a 290.8mm gap (i.e. a 145.4mm tire
//      radius). This is a real cross-repo consistency check, not an arbitrary tolerance test:
//      if it fails, either this copy of the STL differs from the one MuJoCo simulates, or the
//      XML comment is wrong -- either way, something upstream of this test needs attention, not
//      the test itself.
#include "../StlLoader.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace OverboardMesh;

namespace
{
	int gFailures = 0;

	void Check(bool Cond, const std::string& Msg)
	{
		if (!Cond)
		{
			std::printf("  FAIL: %s\n", Msg.c_str());
			++gFailures;
		}
	}

	bool NearlyEqual(float A, float B, float Eps)
	{
		return std::fabs(A - B) <= Eps;
	}

	// Loads Name.stl from Meshes/openwheel/ relative to the repo root. Tests run from mesh/, so
	// the mesh directory is one level up.
	bool LoadPart(const std::string& Name, FStlMesh& OutMesh)
	{
		std::string Err;
		bool Ok = LoadBinaryStl("../Meshes/openwheel/" + Name + ".stl", OutMesh, Err);
		if (!Ok)
		{
			std::printf("  FAIL: %s: %s\n", Name.c_str(), Err.c_str());
			++gFailures;
		}
		return Ok;
	}
}

int main()
{
	const char* Parts[] = {
		"front_enclosure", "rear_enclosure", "front_bumper", "rear_bumper",
		"front_footpad", "rear_footpad", "electronics_platform",
	};

	std::printf("Test_AllPartsParse\n");
	for (const char* Name : Parts)
	{
		FStlMesh Mesh;
		if (LoadPart(Name, Mesh))
		{
			Check(!Mesh.Triangles.empty(), std::string(Name) + ": has at least one triangle");
			std::printf("  %s: %zu triangles, bounds X[%.1f,%.1f] Y[%.1f,%.1f] Z[%.1f,%.1f] mm\n",
				Name, Mesh.Triangles.size(),
				Mesh.BoundsMin.X, Mesh.BoundsMax.X, Mesh.BoundsMin.Y, Mesh.BoundsMax.Y,
				Mesh.BoundsMin.Z, Mesh.BoundsMax.Z);
		}
	}

	std::printf("Test_FrontEnclosureBoundsMatchXmlComment\n");
	{
		FStlMesh Mesh;
		if (LoadPart("front_enclosure", Mesh))
		{
			// overboard_onewheel.xml: "the front enclosure spans x = -431.8..-145.4 mm"
			Check(NearlyEqual(Mesh.BoundsMin.X, -431.8f, 1.0f), "front_enclosure BoundsMin.X ~= -431.8mm");
			Check(NearlyEqual(Mesh.BoundsMax.X, -145.4f, 1.0f), "front_enclosure BoundsMax.X ~= -145.4mm");
		}
	}

	std::printf("Test_FrontRearEnclosureMirrorAcrossXZero\n");
	{
		FStlMesh Front, Rear;
		if (LoadPart("front_enclosure", Front) && LoadPart("rear_enclosure", Rear))
		{
			// "the front/rear enclosure ... pairs mirror exactly across X=0, and the gap between
			// the enclosures is 290.8 mm, i.e. a 145.4 mm tire radius"
			Check(NearlyEqual(Front.BoundsMin.X, -Rear.BoundsMax.X, 1.0f), "front/rear enclosure mirror: min(front.X) ~= -max(rear.X)");
			Check(NearlyEqual(Front.BoundsMax.X, -Rear.BoundsMin.X, 1.0f), "front/rear enclosure mirror: max(front.X) ~= -min(rear.X)");
			const float Gap = Rear.BoundsMin.X - Front.BoundsMax.X;
			Check(NearlyEqual(Gap, 290.8f, 1.0f), "gap between front and rear enclosure ~= 290.8mm (tire radius 145.4mm)");
		}
	}

	if (gFailures == 0)
	{
		std::printf("\nALL TESTS PASSED\n");
		return 0;
	}
	std::printf("\n%d CHECK(S) FAILED\n", gFailures);
	return 1;
}
