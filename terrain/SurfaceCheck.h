// SurfaceCheck.h
//
// The enforcing half of ADR-0011 condition 2: given a declared surface, does it stay inside the
// envelope in `TerrainLimits.h`?
//
// Three checks, one per surface kind, all reporting the same violation type so the CLI can print
// them uniformly and so a test can assert on them individually.
//
//   Plane -- slope, and run-out against `MaxRunOutM(slope)`.
//   Mesh  -- the two ways a mesh actually breaks the smoothness rule in practice:
//              * a WALL facet (near-vertical) whose vertical extent is a step -- this is a kerb,
//                a brick edge, a root heave. The 20 mm case ADR-0011 measures at 201 deg/s.
//              * a SEAM: two boundary edges sitting at the same place in plan but at different
//                heights, which is what a tiling mistake or a collision-hull artifact produces.
//                These are the millimetre-scale discontinuities issue #208 warns about, and they
//                are invisible to the eye at the scale that matters.
//            Plus ordinary ramp slope on every non-wall facet.
//   Probe -- consecutive measured points along the drivable path. The rise between two points is
//            a slope over their separation; a rise with (almost) no separation is a step.
//            Coverage against `reach_m` is checked too: probes that stop short measure nothing
//            about the ground past where they stop, and silently reporting a pass for that is
//            the failure mode this whole exercise exists to prevent.
#pragma once

#include "TerrainDecl.h"

#include <string>
#include <vector>

namespace OverboardMesh { struct FStlMesh; }

namespace OverboardTerrain
{
	enum class EViolationKind
	{
		Step,          // a discontinuity above kMaxAuthoredStepMm
		Slope,         // a grade above kMaxAuthoredSlopeDeg
		RunOut,        // a legal grade held for longer than MaxRunOutM allows
		Coverage,      // probes do not span the declared reach
		AssetMissing,  // a declared mesh could not be read
	};

	struct FViolation
	{
		EViolationKind Kind = EViolationKind::Step;
		std::string SurfaceName;
		std::string Detail;   // human-readable, states the measured value AND the limit
		double Measured = 0.0;
		double Limit = 0.0;
	};

	const char* ViolationKindName(EViolationKind Kind);

	/// Checks one surface. `RepoRoot` is prefixed to a mesh surface's `path`. Appends to
	/// `OutViolations` rather than replacing, so a caller can accumulate across a level.
	/// Returns false if the surface could not be checked at all (missing asset), which is itself
	/// recorded as an `AssetMissing` violation -- an uncheckable surface is never a pass.
	bool CheckSurface(const FSurfaceDecl& Surface,
	                  const std::string& RepoRoot,
	                  std::vector<FViolation>& OutViolations);

	/// The mesh check, exposed separately so tests can drive it with synthesised geometry
	/// without going through the filesystem. `UnitsToMm` scales the mesh's native units.
	void CheckMeshGeometry(const OverboardMesh::FStlMesh& Mesh,
	                       double UnitsToMm,
	                       const std::string& SurfaceName,
	                       std::vector<FViolation>& OutViolations);
}
