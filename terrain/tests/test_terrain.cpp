// Standalone tests for the authored-world envelope (ADR-0011 condition 2). Plain clang++, no
// Unreal, same pattern as ../../wire/tests and ../../mesh/tests.
//
// The issue #208 acceptance criterion is explicit that reading the validator is not evidence it
// works: "demonstrated by authoring a violating asset on purpose and watching it fail." So every
// rule here is exercised by SYNTHESISING a violating asset -- a real binary STL written to disk
// with a real 20 mm kerb in it, a real declaration file with a real over-long run-out -- and
// asserting the check rejects it. Each violating case is paired with a compliant near-miss, so a
// check that simply rejects everything cannot pass this suite.
#include "../SurfaceCheck.h"
#include "../TerrainDecl.h"
#include "../TerrainLimits.h"
#include "../../mesh/StlLoader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace OverboardTerrain;

namespace
{
	int gFailures = 0;
	int gChecks = 0;

	void Check(bool Cond, const std::string& Msg)
	{
		++gChecks;
		if (!Cond)
		{
			std::printf("  FAIL: %s\n", Msg.c_str());
			++gFailures;
		}
	}

	bool NearlyEqual(double A, double B, double RelTol)
	{
		const double Scale = std::max(1.0, std::max(std::fabs(A), std::fabs(B)));
		return std::fabs(A - B) <= RelTol * Scale;
	}

	bool HasKind(const std::vector<FViolation>& Vs, EViolationKind K)
	{
		for (const FViolation& V : Vs)
		{
			if (V.Kind == K)
			{
				return true;
			}
		}
		return false;
	}

	// ---- Fixture writers ------------------------------------------------------------------

	struct FTri
	{
		float V[9];
	};

	void PutU32(std::ofstream& F, uint32_t V)
	{
		unsigned char B[4] = { static_cast<unsigned char>(V & 0xff),
		                       static_cast<unsigned char>((V >> 8) & 0xff),
		                       static_cast<unsigned char>((V >> 16) & 0xff),
		                       static_cast<unsigned char>((V >> 24) & 0xff) };
		F.write(reinterpret_cast<const char*>(B), 4);
	}

	void PutF32(std::ofstream& F, float V)
	{
		uint32_t Bits;
		std::memcpy(&Bits, &V, 4);
		PutU32(F, Bits);
	}

	/// Writes a binary STL. The loader rejects ASCII STL and any file whose triangle count does
	/// not match its size, so this has to be a real, well-formed file -- which is the point: the
	/// checks below run against a file on disk parsed by the shipping loader, not against an
	/// in-memory shortcut.
	void WriteStl(const std::string& Path, const std::vector<FTri>& Tris)
	{
		std::ofstream F(Path, std::ios::binary);
		char Header[80] = "STLB terrain test fixture";
		F.write(Header, 80);
		PutU32(F, static_cast<uint32_t>(Tris.size()));
		for (const FTri& T : Tris)
		{
			for (int i = 0; i < 3; ++i) { PutF32(F, 0.f); } // normal: ignored, recomputed
			for (int i = 0; i < 9; ++i) { PutF32(F, T.V[i]); }
			const unsigned char Attr[2] = { 0, 0 };
			F.write(reinterpret_cast<const char*>(Attr), 2);
		}
	}

	/// Two triangles forming an axis-aligned quad in the XY plane at height Z, all in mm.
	void AddQuad(std::vector<FTri>& Out, float X0, float Y0, float X1, float Y1, float Z)
	{
		Out.push_back(FTri{ { X0, Y0, Z, X1, Y0, Z, X1, Y1, Z } });
		Out.push_back(FTri{ { X0, Y0, Z, X1, Y1, Z, X0, Y1, Z } });
	}

	/// A vertical wall spanning X0..X1 at Y, rising from Z0 to Z1 -- a kerb.
	void AddWall(std::vector<FTri>& Out, float X0, float X1, float Y, float Z0, float Z1)
	{
		Out.push_back(FTri{ { X0, Y, Z0, X1, Y, Z0, X1, Y, Z1 } });
		Out.push_back(FTri{ { X0, Y, Z0, X1, Y, Z1, X0, Y, Z1 } });
	}

	/// A ramp quad rising by RiseMm over its X extent.
	void AddRamp(std::vector<FTri>& Out, float X0, float Y0, float X1, float Y1, float Z0, float RiseMm)
	{
		const float Z1 = Z0 + RiseMm;
		Out.push_back(FTri{ { X0, Y0, Z0, X1, Y0, Z1, X1, Y1, Z1 } });
		Out.push_back(FTri{ { X0, Y0, Z0, X1, Y1, Z1, X0, Y1, Z0 } });
	}

	std::vector<FViolation> CheckStl(const std::string& Path, const std::vector<FTri>& Tris)
	{
		WriteStl(Path, Tris);
		OverboardMesh::FStlMesh M;
		std::string Err;
		std::vector<FViolation> Vs;
		if (!OverboardMesh::LoadBinaryStl(Path, M, Err))
		{
			Check(false, "fixture " + Path + " did not load: " + Err);
			return Vs;
		}
		CheckMeshGeometry(M, 1.0 /* mm */, "fixture", Vs);
		return Vs;
	}

	void WriteText(const std::string& Path, const std::string& Body)
	{
		std::ofstream F(Path);
		F << Body;
	}
}

int main()
{
	std::printf("terrain: authored-world envelope (ADR-0011 condition 2)\n\n");

	// =========================================================================================
	std::printf("1. The limits are derived from the measurements, and still agree with them\n");
	// =========================================================================================

	// The single most important assertion in this file. overboard#207 independently reports
	// "~1159 m to speed-cap onset from rest" on a 0.25 deg grade. This validator computes that
	// distance from first principles out of three separate constants. If either side moves, this
	// goes red rather than the two quietly diverging.
	Check(NearlyEqual(MaxRunOutM(0.25), 1159.0, 0.01),
		"MaxRunOutM(0.25 deg) must reproduce overboard#207's ~1159 m; got "
		+ std::to_string(MaxRunOutM(0.25)));

	// The two expressions of one rule -- run-out length and total descent -- must be the same
	// rule. L_max(phi) == dh_max / sin(phi).
	for (double Phi : { 0.25, 1.0, 3.0, 5.0 })
	{
		const double ViaLength = MaxRunOutM(Phi);
		const double ViaDrop = MaxDescentM() / std::sin(Phi * 3.14159265358979323846 / 180.0);
		Check(NearlyEqual(ViaLength, ViaDrop, 1e-9),
			"MaxRunOutM and MaxDescentM must express the same rule at " + std::to_string(Phi) + " deg");
	}

	Check(MaxRunOutM(0.0) == HUGE_VAL, "a level surface has no run-out limit");

	// The step limit is a geometric noise floor, not a survivability allowance. If someone ever
	// edits it up toward the measured best-case survived step, that is the softening move
	// ADR-0011 forbids -- so assert the ordering, not the value.
	Check(kMaxAuthoredStepMm < kMeasuredBestCaseSurvivedStepMm,
		"the authored step limit must stay BELOW the best-case step the board actually survived");
	Check(kMaxAuthoredSlopeDeg < kMeasuredSelfArrestSlopeDeg,
		"the authored slope ceiling must stay BELOW the grade the host self-arrests on");

	// =========================================================================================
	std::printf("\n2. Mesh geometry -- a violating asset, authored on purpose\n");
	// =========================================================================================

	{
		std::vector<FTri> Flat;
		AddQuad(Flat, 0, 0, 5000, 5000, 0);
		const auto Vs = CheckStl("/tmp/ob_terrain_flat.stl", Flat);
		Check(Vs.empty(), "a flat 5 m x 5 m plate must pass clean");
	}

	{
		// The kerb ADR-0011 measures: a 20 mm lip. 201 deg/s of imparted pitch rate against
		// ~76 deg/s of KD authority.
		std::vector<FTri> Kerb;
		AddQuad(Kerb, 0, 0, 5000, 1000, 0);
		AddWall(Kerb, 0, 5000, 1000, 0, 20);
		AddQuad(Kerb, 0, 1000, 5000, 2000, 20);
		const auto Vs = CheckStl("/tmp/ob_terrain_kerb20mm.stl", Kerb);
		Check(HasKind(Vs, EViolationKind::Step), "a 20 mm kerb must be rejected as a STEP");
	}

	{
		// The case nobody sees by looking: two tiles that do not quite meet. 0.5 mm.
		std::vector<FTri> Seam;
		AddQuad(Seam, 0, 0, 1000, 1000, 0);
		AddQuad(Seam, 0, 1000, 1000, 2000, 0.5f);
		const auto Vs = CheckStl("/tmp/ob_terrain_seam.stl", Seam);
		Check(HasKind(Vs, EViolationKind::Step),
			"a 0.5 mm seam between two tiles must be rejected as a STEP");
	}

	{
		// ...and the same seam inside the tolerance must NOT fire, or the check is just
		// rejecting everything.
		std::vector<FTri> Seam;
		AddQuad(Seam, 0, 0, 1000, 1000, 0);
		AddQuad(Seam, 0, 1000, 1000, 2000, 0.1f);
		const auto Vs = CheckStl("/tmp/ob_terrain_seam_ok.stl", Seam);
		Check(!HasKind(Vs, EViolationKind::Step),
			"a 0.1 mm seam is inside the geometric noise floor and must pass");
	}

	{
		// 6 deg ramp: over the 5.2 deg ceiling.
		std::vector<FTri> Ramp;
		const float Run = 10000.f;
		AddRamp(Ramp, 0, 0, Run, 2000, 0, static_cast<float>(Run * std::tan(6.0 * 3.14159265358979323846 / 180.0)));
		const auto Vs = CheckStl("/tmp/ob_terrain_ramp6deg.stl", Ramp);
		Check(HasKind(Vs, EViolationKind::Slope), "a 6 deg ramp must be rejected as a SLOPE");
	}

	{
		// 5 deg ramp: inside the ceiling, must pass.
		std::vector<FTri> Ramp;
		const float Run = 10000.f;
		AddRamp(Ramp, 0, 0, Run, 2000, 0, static_cast<float>(Run * std::tan(5.0 * 3.14159265358979323846 / 180.0)));
		const auto Vs = CheckStl("/tmp/ob_terrain_ramp5deg.stl", Ramp);
		Check(!HasKind(Vs, EViolationKind::Slope), "a 5 deg ramp is inside the ceiling and must pass");
	}

	// =========================================================================================
	std::printf("\n3. Plane declarations -- the angle is legal, the length is not\n");
	// =========================================================================================

	{
		FSurfaceDecl S;
		S.Kind = ESurfaceKind::Plane;
		S.Name = "gentle_but_endless";
		S.SlopeDeg = 0.25;
		S.RunOutM = 2000.0; // #207's own example: fine for a metre, not for a kilometre
		std::vector<FViolation> Vs;
		CheckSurface(S, ".", Vs);
		Check(HasKind(Vs, EViolationKind::RunOut),
			"0.25 deg held for 2 km must be rejected on RUN-OUT even though the angle is trivial");
		Check(!HasKind(Vs, EViolationKind::Slope), "...and NOT on slope -- 0.25 deg is nowhere near the ceiling");
	}

	{
		FSurfaceDecl S;
		S.Kind = ESurfaceKind::Plane;
		S.Name = "gentle_and_short";
		S.SlopeDeg = 0.25;
		S.RunOutM = 1000.0;
		std::vector<FViolation> Vs;
		CheckSurface(S, ".", Vs);
		Check(Vs.empty(), "0.25 deg held for 1 km is inside the run-out allowance and must pass");
	}

	{
		FSurfaceDecl S;
		S.Kind = ESurfaceKind::Plane;
		S.Name = "too_steep";
		S.SlopeDeg = 7.0; // the grade #207 measured the host being OUTRUN on
		S.RunOutM = 5.0;
		std::vector<FViolation> Vs;
		CheckSurface(S, ".", Vs);
		Check(HasKind(Vs, EViolationKind::Slope), "7 deg -- where the host is outrun -- must be rejected");
	}

	// =========================================================================================
	std::printf("\n4. Probe series -- measured ground, and ground nobody measured\n");
	// =========================================================================================

	{
		FSurfaceDecl S;
		S.Kind = ESurfaceKind::Probe;
		S.Name = "kerb_in_the_path";
		S.ReachM = 3.0;
		S.Probes = { { 0, 0, 0 }, { 1, 0, 0 }, { 1.01, 0, 0.02 }, { 3, 0, 0.02 } };
		std::vector<FViolation> Vs;
		CheckSurface(S, ".", Vs);
		Check(HasKind(Vs, EViolationKind::Step),
			"a 20 mm rise over 1 cm of ground is a step, and must be rejected as one");
	}

	{
		FSurfaceDecl S;
		S.Kind = ESurfaceKind::Probe;
		S.Name = "short_probes";
		S.ReachM = 100.0;
		S.Probes = { { 0, 0, 0 }, { 10, 0, 0 } };
		std::vector<FViolation> Vs;
		CheckSurface(S, ".", Vs);
		Check(HasKind(Vs, EViolationKind::Coverage),
			"probes covering 10 m of a 100 m reach must fail on COVERAGE -- unmeasured ground is not ground that passed");
	}

	{
		FSurfaceDecl S;
		S.Kind = ESurfaceKind::Probe;
		S.Name = "long_gentle_descent";
		S.ReachM = 200.0;
		// 200 m of 2 deg descent: the angle is legal, the accumulated drop (7.0 m) is not.
		S.Probes.push_back(FProbePoint{ 0, 0, 0 });
		for (int i = 1; i <= 20; ++i)
		{
			S.Probes.push_back(FProbePoint{ i * 10.0, 0, -i * 10.0 * std::tan(2.0 * 3.14159265358979323846 / 180.0) });
		}
		std::vector<FViolation> Vs;
		CheckSurface(S, ".", Vs);
		Check(HasKind(Vs, EViolationKind::RunOut),
			"200 m of legal 2 deg descent accumulates 7.0 m of drop and must fail on RUN-OUT");
		Check(!HasKind(Vs, EViolationKind::Slope), "...and not on slope; 2 deg is legal");
	}

	{
		FSurfaceDecl S;
		S.Kind = ESurfaceKind::Probe;
		S.Name = "flat_road";
		S.ReachM = 50.0;
		S.Probes = { { 0, 0, 0 }, { 25, 0, 0 }, { 50, 0, 0 } };
		std::vector<FViolation> Vs;
		CheckSurface(S, ".", Vs);
		Check(Vs.empty(), "50 m of measured flat road across a 50 m reach must pass clean");
	}

	// =========================================================================================
	std::printf("\n5. The declaration parser is strict, because a half-parsed declaration\n");
	std::printf("   reads as coverage it does not have\n");
	// =========================================================================================

	{
		const std::string P = "/tmp/ob_terrain_good.terrain";
		WriteText(P,
			"level Content/Maps/X.umap\n"
			"level_hash 0123456789abcdef\n"
			"surface road\n"
			"  kind plane\n"
			"  status verified\n"
			"  slope_deg 0.0\n"
			"  run_out_m 100.0\n"
			"  source unit test\n");
		FLevelDecl D;
		std::string E;
		Check(ParseLevelDecl(P, D, E), "a well-formed declaration must parse: " + E);
		Check(D.LevelHash == 0x0123456789abcdefull, "level_hash must round-trip as hex");
		Check(D.Surfaces.size() == 1, "one surface expected");
	}

	{
		// Known-answer test for the staleness hash. Written after a one-digit typo in the FNV
		// offset basis made every level read as stale -- loud rather than silent, but a hash
		// function with no KAT is a hash function nobody has checked.
		const std::string P = "/tmp/ob_terrain_kat.bin";
		WriteText(P, "hello");
		uint64_t H = 0;
		std::string E;
		Check(HashFile(P, H, E), "HashFile must read a file it can open: " + E);
		Check(H == 0xa430d84680aabd0bull,
			"FNV-1a 64 of \"hello\" is a430d84680aabd0b; got " + std::to_string(H));
		Check(!HashFile("/tmp/ob_terrain_does_not_exist.bin", H, E),
			"HashFile must fail loudly on a file it cannot open");
	}

	{
		const std::string P = "/tmp/ob_terrain_typo.terrain";
		WriteText(P,
			"level Content/Maps/X.umap\n"
			"level_hash 1\n"
			"surface road\n"
			"  kind plane\n"
			"  slop_deg 9.0\n" // typo: would silently read as "no slope declared"
			"  source unit test\n");
		FLevelDecl D;
		std::string E;
		Check(!ParseLevelDecl(P, D, E), "a typo'd key must be rejected, not ignored");
	}

	{
		const std::string P = "/tmp/ob_terrain_nosource.terrain";
		WriteText(P,
			"level Content/Maps/X.umap\n"
			"level_hash 1\n"
			"surface road\n"
			"  kind plane\n"
			"  slope_deg 0.0\n");
		FLevelDecl D;
		std::string E;
		Check(!ParseLevelDecl(P, D, E), "a surface with no `source` must be rejected");
	}

	{
		const std::string P = "/tmp/ob_terrain_nohash.terrain";
		WriteText(P,
			"level Content/Maps/X.umap\n"
			"surface road\n"
			"  kind plane\n"
			"  slope_deg 0.0\n"
			"  source unit test\n");
		FLevelDecl D;
		std::string E;
		Check(!ParseLevelDecl(P, D, E),
			"a declaration with no level_hash must be rejected -- it could never go stale loudly");
	}

	{
		const std::string P = "/tmp/ob_terrain_oneprobe.terrain";
		WriteText(P,
			"level Content/Maps/X.umap\n"
			"level_hash 1\n"
			"surface road\n"
			"  kind probe\n"
			"  reach_m 10\n"
			"  probe 0 0 0\n"
			"  source unit test\n");
		FLevelDecl D;
		std::string E;
		Check(!ParseLevelDecl(P, D, E), "one probe point measures a height, not a surface");
	}

	{
		const std::string P = "/tmp/ob_terrain_units.terrain";
		WriteText(P,
			"level Content/Maps/X.umap\n"
			"level_hash 1\n"
			"surface road\n"
			"  kind plane\n"
			"  slope_deg 0.5deg\n"
			"  source unit test\n");
		FLevelDecl D;
		std::string E;
		Check(!ParseLevelDecl(P, D, E),
			"a number with a unit suffix must be rejected, not silently truncated to 0.5");
	}

	{
		std::string E;
		FSurfaceDecl S;
		S.Kind = ESurfaceKind::Mesh;
		S.Name = "missing";
		S.Path = "Meshes/does_not_exist.stl";
		std::vector<FViolation> Vs;
		Check(!CheckSurface(S, ".", Vs), "a mesh that cannot be read is not checkable");
		Check(HasKind(Vs, EViolationKind::AssetMissing),
			"...and an uncheckable surface must be recorded as a violation, never as a pass");
	}

	// =========================================================================================
	std::printf("\n6. The declarations this repo actually ships parse and stay inside the envelope\n");
	// =========================================================================================

	for (const char* Name : { "OB_Main", "OB_City" })
	{
		const std::string P = std::string("levels/") + Name + ".terrain";
		FLevelDecl D;
		std::string E;
		if (!ParseLevelDecl(P, D, E))
		{
			Check(false, std::string("shipped declaration ") + Name + " must parse: " + E);
			continue;
		}
		std::vector<FViolation> Vs;
		for (const FSurfaceDecl& S : D.Surfaces)
		{
			CheckSurface(S, "..", Vs);
		}
		Check(Vs.empty(), std::string("shipped declaration ") + Name
			+ " must be inside the envelope ("
			+ (Vs.empty() ? "" : Vs.front().Detail) + ")");
	}

	std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
	return gFailures == 0 ? 0 : 1;
}
