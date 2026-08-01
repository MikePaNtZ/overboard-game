// StlLoader.h
//
// A minimal binary-STL parser, deliberately engine-free (same reasoning as wire/: prove it
// standalone with plain clang++ before wiring it into Unreal -- see mesh/README.md). UE 5.7 has
// no built-in STL import factory (checked: no importer/factory anywhere under Engine/Plugins or
// Engine/Source mentions STL), so instead of fighting a missing importer or a conversion-tool
// dependency, this project parses the STL triangles directly and hands them to a
// UProceduralMeshComponent at runtime -- no .uasset import step, no editor GUI required.
//
// Binary STL layout (the only variant these files use -- verified: all seven start with the
// magic "STLB", not the ASCII "solid" keyword):
//   80 bytes   header (arbitrary tool-specific text, ignored)
//   4 bytes    uint32 triangle count, little-endian
//   per triangle, 50 bytes:
//     12 bytes   facet normal (3x float32) -- ignored; normals are recomputed from winding so a
//                degenerate or inconsistent exporter-supplied normal can't produce a bad shade
//     36 bytes   3 vertices (3x float32 each), in mm (see mesh/README.md for the mm -> cm scale)
//     2 bytes    attribute byte count -- ignored (some exporters pack a colour here; this
//                project's per-part colour comes from the MJCF material palette instead, not
//                from the mesh file, so there is exactly one source of truth for colour)
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace OverboardMesh
{
	struct FVec3
	{
		float X = 0.f;
		float Y = 0.f;
		float Z = 0.f;
	};

	struct FStlTriangle
	{
		FVec3 Normal; // as read from the file; not trusted for shading, kept for diagnostics
		FVec3 V0;
		FVec3 V1;
		FVec3 V2;
	};

	struct FStlMesh
	{
		std::vector<FStlTriangle> Triangles;

		// Axis-aligned bounding box across all vertices, in the file's native units (mm for
		// these files). Min/Max default to a state that makes an empty mesh obviously empty
		// rather than accidentally reading as a valid box at the origin.
		FVec3 BoundsMin{ 1e9f, 1e9f, 1e9f };
		FVec3 BoundsMax{ -1e9f, -1e9f, -1e9f };
	};

	// Parses a binary STL file from disk. Returns false and fills OutError on any failure
	// (missing file, truncated data, ASCII-STL instead of binary). Never partially trusts a
	// malformed file -- on failure OutMesh is left empty.
	bool LoadBinaryStl(const std::string& Path, FStlMesh& OutMesh, std::string& OutError);
}
