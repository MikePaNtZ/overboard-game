// TerrainDecl.h
//
// The per-level ridden-surface declaration: `terrain/levels/<Level>.terrain`.
//
// ADR-0011 condition 2 asks for the authored-world constraint to be "encoded as a checkable
// asset rule". A rule needs something to check, and the shipped world is not directly checkable
// data: `Content/Maps/*.umap` is an opaque Unreal binary, and `OB_City`'s scenery is a 1.5 GB
// Fab pack that is gitignored and will never be committed (see docs/citypark-level.md). A CI job
// cannot open either.
//
// So the level declares its drivable surfaces as small, reviewable, diffable text, and this
// module parses it. Three surface kinds cover everything this project authors:
//
//   plane  -- an analytic primitive placed by code or by the level. Slope and extent are known
//             exactly, so the declaration states them and no geometry needs opening.
//   mesh   -- a committed mesh file. Checked FOR REAL against the file, via mesh/StlLoader.
//   probe  -- a surface whose geometry is not in this repo (an external pack). Declared as a
//             series of MEASURED points along the drivable path. `TerrainProbeCommandlet`
//             produces these by line-tracing the real level in the real editor; a human may
//             also enter them, provided `source` says how they were obtained.
//
// **The staleness gate is the part that makes this more than a convention.** Each declaration
// records a hash of the level asset it describes. Change the .umap without re-declaring, and
// the check goes red. Without it, a declaration is a comment that drifts -- which is precisely
// the failure mode ADR-0011 records this org repeating ("documentation is a polling surface").
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace OverboardTerrain
{
	enum class ESurfaceKind
	{
		Plane,
		Mesh,
		Probe,
		/// Geometry that is not in this repo and has not been measured yet -- today, only the
		/// City Park pack, which is gitignored, 1.5 GB, and licensed to an Epic account rather
		/// than to this repo (docs/citypark-level.md).
		///
		/// This kind produces NO envelope verdict, which makes it the one hole in the gate. It
		/// is closed three ways rather than left open: the parser refuses to let an `external`
		/// surface call itself `verified`, `--strict` fails on it, and the runtime tags the
		/// level on screen so no footage can be shot on one without the tag being visible. It
		/// exists so that "we have not measured this" is a thing the check SAYS, rather than a
		/// thing it cannot represent -- an undeclarable level would otherwise just be a level
		/// with no declaration, which is indistinguishable from an oversight.
		External,
	};

	/// How much confidence the declaration itself claims. `Unverified` is a legal, non-fatal
	/// state on purpose: `OB_City` sits over a pack this repo cannot commit, and a permanently
	/// red required check poisons the one interrupt this org has that works (ADR-0011 makes
	/// exactly this argument about `publish-sim-artifact`). Unverified levels are reported
	/// loudly, fail under `--strict`, and are tagged on screen at runtime so no footage can be
	/// shot on one without the tag being visible.
	enum class EVerifyStatus
	{
		Verified,
		Unverified,
	};

	struct FProbePoint
	{
		double XM = 0.0;
		double YM = 0.0;
		double ZM = 0.0;
	};

	struct FSurfaceDecl
	{
		std::string Name;
		ESurfaceKind Kind = ESurfaceKind::Plane;
		EVerifyStatus Status = EVerifyStatus::Unverified;
		std::string Source; // free text: where the numbers came from. Required, never blank.

		// kind = plane
		double SlopeDeg = 0.0;
		double RunOutM = 0.0;

		// kind = mesh
		std::string Path;      // repo-relative
		double UnitsToMm = 1.0; // mm = 1.0, cm = 10.0, m = 1000.0

		// kind = probe
		std::vector<FProbePoint> Probes;
		double ReachM = 0.0; // how far from the world origin the board can be driven
	};

	struct FLevelDecl
	{
		std::string DeclPath;   // the .terrain file this came from
		std::string LevelPath;  // repo-relative .umap
		uint64_t LevelHash = 0; // FNV-1a 64 of the .umap bytes at declaration time
		std::vector<std::string> Notes; // printed on every check run -- see the `note` key
		std::vector<FSurfaceDecl> Surfaces;
	};

	/// Parses a declaration file. STRICT: an unknown key, a missing required key for the
	/// declared kind, a blank `source`, or a malformed number is a hard failure. A declaration
	/// that half-parses is worse than none, because it reads as coverage it does not have.
	bool ParseLevelDecl(const std::string& Path, FLevelDecl& OutDecl, std::string& OutError);

	/// FNV-1a 64 over a file's bytes. This is a STALENESS detector, not a security property --
	/// it answers "did this asset change since somebody last looked at it", and a 64-bit
	/// non-cryptographic hash answers that completely. Returns false if the file cannot be read.
	bool HashFile(const std::string& Path, uint64_t& OutHash, std::string& OutError);
}
