// terraincheck -- the gate for ADR-0011 condition 2.
//
// Walks every level this repo ships, requires each to declare its drivable surfaces, and checks
// every declared surface against the envelope in TerrainLimits.h. Exits non-zero on any
// violation, on any level with no declaration, and on any declaration that has gone stale
// against the level it describes.
//
// Usage:  terraincheck [--repo-root <path>] [--strict] [--quiet]
//
//   --emit-header <path>
//              write the per-level verified/unverified table as a plain C++ header and exit.
//              The runtime reads it to decide whether to tag the screen (AOverboardHUD), which
//              is the third of the three things closing the `kind external` hole in this gate.
//              Generated rather than hand-maintained on purpose: a hand-kept copy of "which
//              levels are verified" is a second source of truth, and this whole directory exists
//              because a constraint kept in prose stops being true without anybody noticing. CI
//              regenerates and diffs it, so the two cannot part company.
//
//   --strict   also fail on surfaces declared `status unverified`. This is what a public-build
//              job runs. It is NOT the default, because `OB_City` sits over a Fab pack that is
//              gitignored and can never be committed, and a permanently-red required check
//              poisons the one interrupt this org has that works -- ADR-0011 makes exactly that
//              argument about `publish-sim-artifact`. Unverified levels are instead reported
//              loudly here and tagged on screen at runtime, so no footage can be shot on one
//              without the tag being visible.
#include "../SurfaceCheck.h"
#include "../TerrainDecl.h"
#include "../TerrainLimits.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

using namespace OverboardTerrain;

namespace
{
	bool ListDir(const std::string& Dir, const std::string& Suffix, std::vector<std::string>& Out)
	{
		DIR* D = opendir(Dir.c_str());
		if (D == nullptr)
		{
			return false;
		}
		while (dirent* E = readdir(D))
		{
			const std::string Name = E->d_name;
			if (Name.size() > Suffix.size()
				&& Name.compare(Name.size() - Suffix.size(), Suffix.size(), Suffix) == 0)
			{
				Out.push_back(Name);
			}
		}
		closedir(D);
		std::sort(Out.begin(), Out.end());
		return true;
	}

	std::string StemOf(const std::string& FileName)
	{
		const size_t Dot = FileName.find_last_of('.');
		return Dot == std::string::npos ? FileName : FileName.substr(0, Dot);
	}
}

int main(int Argc, char** Argv)
{
	std::string RepoRoot = "..";
	std::string EmitHeaderPath;
	bool Strict = false;
	bool Quiet = false;

	for (int i = 1; i < Argc; ++i)
	{
		if (std::strcmp(Argv[i], "--repo-root") == 0 && i + 1 < Argc) { RepoRoot = Argv[++i]; }
		else if (std::strcmp(Argv[i], "--emit-header") == 0 && i + 1 < Argc) { EmitHeaderPath = Argv[++i]; }
		else if (std::strcmp(Argv[i], "--strict") == 0) { Strict = true; }
		else if (std::strcmp(Argv[i], "--quiet") == 0) { Quiet = true; }
		else
		{
			std::fprintf(stderr, "terraincheck: unknown argument '%s'\n", Argv[i]);
			return 2;
		}
	}

	const std::string MapsDir = RepoRoot + "/Content/Maps";
	const std::string DeclDir = RepoRoot + "/terrain/levels";

	std::vector<std::string> Maps;
	if (!ListDir(MapsDir, ".umap", Maps))
	{
		std::fprintf(stderr, "terraincheck: cannot list '%s'\n", MapsDir.c_str());
		return 2;
	}

	if (!EmitHeaderPath.empty())
	{
		std::vector<std::pair<std::string, bool>> Table;
		for (const std::string& MapFile : Maps)
		{
			const std::string Stem = StemOf(MapFile);
			FLevelDecl Decl;
			std::string Err;
			// A level with no declaration, or one that will not parse, is reported UNVERIFIED
			// here as well as failing the check below. The two answers must not disagree: a
			// level nobody declared is not a level that passed.
			bool Verified = ParseLevelDecl(DeclDir + "/" + Stem + ".terrain", Decl, Err);
			for (const FSurfaceDecl& S : Decl.Surfaces)
			{
				if (S.Status != EVerifyStatus::Verified)
				{
					Verified = false;
				}
			}
			Table.emplace_back(Stem, Verified);
		}

		FILE* F = std::fopen(EmitHeaderPath.c_str(), "w");
		if (F == nullptr)
		{
			std::fprintf(stderr, "terraincheck: cannot write '%s'\n", EmitHeaderPath.c_str());
			return 2;
		}
		std::fprintf(F,
			"// GENERATED FILE -- do not edit by hand.\n"
			"//\n"
			"// Written by `terraincheck --emit-header` from terrain/levels/*.terrain, which are the\n"
			"// source of truth. CI regenerates this and fails on any diff, so it cannot drift from the\n"
			"// declarations the way a hand-kept second copy would.\n"
			"//\n"
			"// A level is verified only if EVERY declared surface is `status verified`. A level with no\n"
			"// declaration, or one that will not parse, is unverified -- a level nobody declared is not\n"
			"// a level that passed.\n"
			"//\n"
			"// Consumed by AOverboardHUD to tag the screen on an unverified level (ADR-0011 condition 2,\n"
			"// terrain/README.md \"the hole in the gate\"). Deliberately plain C++ with no UE types, so\n"
			"// the generator stays engine-free.\n"
			"#pragma once\n"
			"\n"
			"namespace OverboardTerrainVerification\n"
			"{\n"
			"\tstruct FLevelVerification\n"
			"\t{\n"
			"\t\tconst char* LevelName;\n"
			"\t\tbool bVerified;\n"
			"\t};\n"
			"\n"
			"\tinline constexpr FLevelVerification kLevels[] = {\n");
		for (const auto& KV : Table)
		{
			std::fprintf(F, "\t\t{ \"%s\", %s },\n", KV.first.c_str(), KV.second ? "true" : "false");
		}
		std::fprintf(F,
			"\t};\n"
			"\n"
			"\t/// Unknown level names return false. An unrecognised level has certainly not been\n"
			"\t/// measured against the envelope, so \"unverified\" is the only safe default.\n"
			"\tinline bool IsLevelVerified(const char* Name)\n"
			"\t{\n"
			"\t\tfor (const FLevelVerification& L : kLevels)\n"
			"\t\t{\n"
			"\t\t\tconst char* A = L.LevelName;\n"
			"\t\t\tconst char* B = Name;\n"
			"\t\t\twhile (*A && *A == *B) { ++A; ++B; }\n"
			"\t\t\tif (*A == 0 && *B == 0) { return L.bVerified; }\n"
			"\t\t}\n"
			"\t\treturn false;\n"
			"\t}\n"
			"}\n");
		std::fclose(F);
		std::printf("terraincheck: wrote %s (%zu level(s))\n", EmitHeaderPath.c_str(), Table.size());
		return 0;
	}

	if (!Quiet)
	{
		std::printf("terraincheck -- ADR-0011 condition 2, the authored-world envelope\n");
		std::printf("  max authored step      %.3f mm   (geometric noise floor, NOT a survivability allowance)\n",
			kMaxAuthoredStepMm);
		std::printf("  max authored slope     %.3f deg  (0.80 x %.1f deg self-arrest, overboard#207)\n",
			kMaxAuthoredSlopeDeg, kMeasuredSelfArrestSlopeDeg);
		std::printf("  max continuous descent %.2f m     (to speed-cap onset %.2f m/s at %.2f x g sin phi)\n",
			MaxDescentM(), kSpeedCapOnsetMS, kFreeRollAccelFractionOfGSinPhi);
		std::printf("  %zu level(s) in %s\n\n", Maps.size(), MapsDir.c_str());
	}

	int Failures = 0;
	int UnverifiedSurfaces = 0;

	for (const std::string& MapFile : Maps)
	{
		const std::string Stem = StemOf(MapFile);
		const std::string DeclPath = DeclDir + "/" + Stem + ".terrain";

		struct stat St;
		if (stat(DeclPath.c_str(), &St) != 0)
		{
			std::printf("FAIL  %s\n", MapFile.c_str());
			std::printf("      no terrain declaration at terrain/levels/%s.terrain\n", Stem.c_str());
			std::printf("      A level with no declared drivable surface has not been checked against\n"
			            "      anything. ADR-0011 condition 2 requires the authored world to be\n"
			            "      constrained by a check, and this is where a new level meets it.\n\n");
			++Failures;
			continue;
		}

		FLevelDecl Decl;
		std::string Err;
		if (!ParseLevelDecl(DeclPath, Decl, Err))
		{
			std::printf("FAIL  %s\n      %s\n\n", MapFile.c_str(), Err.c_str());
			++Failures;
			continue;
		}

		// Staleness. This is what stops the declaration becoming a comment that drifts.
		const std::string ActualLevelPath = RepoRoot + "/" + Decl.LevelPath;
		uint64_t ActualHash = 0;
		if (!HashFile(ActualLevelPath, ActualHash, Err))
		{
			std::printf("FAIL  %s\n      declaration names a level it cannot read: %s\n\n",
				MapFile.c_str(), Err.c_str());
			++Failures;
			continue;
		}
		if (ActualHash != Decl.LevelHash)
		{
			std::printf("FAIL  %s\n", MapFile.c_str());
			std::printf("      STALE: %s has changed since its terrain was declared.\n", Decl.LevelPath.c_str());
			std::printf("      declared level_hash %016llx, actual %016llx\n",
				static_cast<unsigned long long>(Decl.LevelHash),
				static_cast<unsigned long long>(ActualHash));
			std::printf("      Re-run the terrain probe over the level and update the declaration.\n"
			            "      A declaration that outlives the level it describes is worse than none.\n\n");
			++Failures;
			continue;
		}

		std::vector<FViolation> Violations;
		int LevelUnverified = 0;
		for (const FSurfaceDecl& S : Decl.Surfaces)
		{
			CheckSurface(S, RepoRoot, Violations);
			if (S.Status == EVerifyStatus::Unverified)
			{
				++LevelUnverified;
				++UnverifiedSurfaces;
			}
		}

		const bool HardFail = !Violations.empty() || (Strict && LevelUnverified > 0);
		std::printf("%s  %s  (%zu surface(s))\n",
			HardFail ? "FAIL" : (LevelUnverified > 0 ? "WARN" : "PASS"),
			MapFile.c_str(), Decl.Surfaces.size());

		for (const std::string& N : Decl.Notes)
		{
			std::printf("      NOTE          %s\n", N.c_str());
		}

		for (const FViolation& V : Violations)
		{
			std::printf("      %-13s %s: %s\n", ViolationKindName(V.Kind), V.SurfaceName.c_str(),
				V.Detail.c_str());
		}

		for (const FSurfaceDecl& S : Decl.Surfaces)
		{
			if (S.Status == EVerifyStatus::Unverified)
			{
				std::printf("      UNVERIFIED    %s: %s\n", S.Name.c_str(), S.Source.c_str());
			}
		}

		if (HardFail)
		{
			++Failures;
		}
		std::printf("\n");
	}

	if (Failures > 0)
	{
		std::printf("terraincheck: %d level(s) FAILED.\n", Failures);
		std::printf("Do not widen a limit in TerrainLimits.h to make this pass. ADR-0011's criterion\n"
		            "move onto the hardware gate is honest only while this check constrains the world;\n"
		            "loosening it to admit a level is the softening manoeuvre the ADR forbids by name.\n");
		return 1;
	}

	if (UnverifiedSurfaces > 0)
	{
		std::printf("terraincheck: all declared surfaces are inside the envelope, but %d surface(s) are\n"
		            "UNVERIFIED -- declared rather than measured. These fail under --strict, which is what\n"
		            "a public build must run. ADR-0011 holds the launch until the exit criteria are met.\n",
			UnverifiedSurfaces);
	}
	else
	{
		std::printf("terraincheck: all levels inside the authored-world envelope.\n");
	}
	return 0;
}
