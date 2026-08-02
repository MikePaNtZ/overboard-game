#include "SurfaceCheck.h"
#include "TerrainLimits.h"

#include "../mesh/StlLoader.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <unordered_map>

namespace OverboardTerrain
{
	namespace
	{
		constexpr double kPi = 3.14159265358979323846;

		/// Vertices closer than this are the same vertex. Chosen an order of magnitude below
		/// `kMaxAuthoredStepMm` so welding can never hide a step the check is looking for.
		constexpr double kWeldTolMm = 0.01;

		/// Two boundary edges whose plan positions are within this are "the same place in plan",
		/// so any height difference between them is a seam step rather than a slope. 5 mm is
		/// generous in plan and still 20x tighter than the smallest feature anybody authors on
		/// purpose.
		constexpr double kSeamPlanTolMm = 5.0;

		/// Below this horizontal separation, two probe points describe a step rather than a
		/// grade -- 5 cm apart with 20 mm of rise between them is a kerb, not a 21 degree hill.
		constexpr double kProbeStepPlanTolM = 0.05;

		char Buf[512];

		std::string Fmt(const char* Format, ...)
		{
			va_list Args;
			va_start(Args, Format);
			std::vsnprintf(Buf, sizeof(Buf), Format, Args);
			va_end(Args);
			return std::string(Buf);
		}

		struct FV3
		{
			double X = 0, Y = 0, Z = 0;
		};

		FV3 Sub(const FV3& A, const FV3& B) { return FV3{ A.X - B.X, A.Y - B.Y, A.Z - B.Z }; }
		FV3 Cross(const FV3& A, const FV3& B)
		{
			return FV3{ A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X };
		}
		double Len(const FV3& A) { return std::sqrt(A.X * A.X + A.Y * A.Y + A.Z * A.Z); }

		int64_t Quant(double V, double Tol) { return static_cast<int64_t>(std::llround(V / Tol)); }

		struct FEdgeKey
		{
			int64_t A[3];
			int64_t B[3];

			bool operator<(const FEdgeKey& O) const
			{
				for (int i = 0; i < 3; ++i)
				{
					if (A[i] != O.A[i]) return A[i] < O.A[i];
				}
				for (int i = 0; i < 3; ++i)
				{
					if (B[i] != O.B[i]) return B[i] < O.B[i];
				}
				return false;
			}
		};

		FEdgeKey MakeEdgeKey(const FV3& P, const FV3& Q)
		{
			int64_t PK[3] = { Quant(P.X, kWeldTolMm), Quant(P.Y, kWeldTolMm), Quant(P.Z, kWeldTolMm) };
			int64_t QK[3] = { Quant(Q.X, kWeldTolMm), Quant(Q.Y, kWeldTolMm), Quant(Q.Z, kWeldTolMm) };

			// Canonical order so an edge and its reverse hash together -- that is the whole
			// point: an edge shared by two triangles appears once forwards and once backwards.
			const bool Swap = std::lexicographical_compare(QK, QK + 3, PK, PK + 3);
			FEdgeKey K;
			for (int i = 0; i < 3; ++i)
			{
				K.A[i] = Swap ? QK[i] : PK[i];
				K.B[i] = Swap ? PK[i] : QK[i];
			}
			return K;
		}

		/// Keeps only the WORST violation of each kind for a surface, with a count of how many
		/// there were. A mesh with a bad tiling pass can produce tens of thousands of identical
		/// findings; printing them all buries the one number a reader needs.
		class FWorstOfEachKind
		{
		public:
			void Record(EViolationKind Kind, double Measured, double Limit,
			            const std::string& SurfaceName, const std::string& Detail)
			{
				FEntry& E = Entries[static_cast<int>(Kind)];
				++E.Count;
				if (!E.Seen || Measured > E.V.Measured)
				{
					E.Seen = true;
					E.V.Kind = Kind;
					E.V.SurfaceName = SurfaceName;
					E.V.Detail = Detail;
					E.V.Measured = Measured;
					E.V.Limit = Limit;
				}
			}

			void Flush(std::vector<FViolation>& Out)
			{
				for (auto& KV : Entries)
				{
					FEntry& E = KV.second;
					if (!E.Seen)
					{
						continue;
					}
					if (E.Count > 1)
					{
						E.V.Detail += Fmt("  [%d occurrences; worst shown]", E.Count);
					}
					Out.push_back(E.V);
				}
			}

		private:
			struct FEntry
			{
				bool Seen = false;
				int Count = 0;
				FViolation V;
			};
			std::map<int, FEntry> Entries;
		};
	}

	const char* ViolationKindName(EViolationKind Kind)
	{
		switch (Kind)
		{
			case EViolationKind::Step:         return "STEP";
			case EViolationKind::Slope:        return "SLOPE";
			case EViolationKind::RunOut:       return "RUN-OUT";
			case EViolationKind::Coverage:     return "COVERAGE";
			case EViolationKind::AssetMissing: return "ASSET-MISSING";
		}
		return "?";
	}

	void CheckMeshGeometry(const OverboardMesh::FStlMesh& Mesh,
	                       double UnitsToMm,
	                       const std::string& SurfaceName,
	                       std::vector<FViolation>& OutViolations)
	{
		FWorstOfEachKind Worst;

		std::map<FEdgeKey, int> EdgeUse;

		for (const OverboardMesh::FStlTriangle& T : Mesh.Triangles)
		{
			const FV3 V0{ T.V0.X * UnitsToMm, T.V0.Y * UnitsToMm, T.V0.Z * UnitsToMm };
			const FV3 V1{ T.V1.X * UnitsToMm, T.V1.Y * UnitsToMm, T.V1.Z * UnitsToMm };
			const FV3 V2{ T.V2.X * UnitsToMm, T.V2.Y * UnitsToMm, T.V2.Z * UnitsToMm };

			// Normal recomputed from winding, never taken from the file -- the same reasoning
			// StlLoader documents for shading. An exporter's normal is not evidence.
			const FV3 N = Cross(Sub(V1, V0), Sub(V2, V0));
			const double L = Len(N);
			if (L <= 0.0)
			{
				continue; // degenerate facet: no area, no surface, nothing to judge
			}

			// |n.z| so a facet and its flip read as the same inclination -- an underside is not
			// a different slope from the surface it belongs to.
			const double CosTheta = std::min(1.0, std::fabs(N.Z) / L);
			const double SlopeDeg = std::acos(CosTheta) * 180.0 / kPi;

			const double ZMin = std::min({ V0.Z, V1.Z, V2.Z });
			const double ZMax = std::max({ V0.Z, V1.Z, V2.Z });
			const double ZExtentMm = ZMax - ZMin;

			if (SlopeDeg >= kWallFacetSlopeDeg)
			{
				// A wall. Its vertical extent is a step -- this is the kerb case.
				if (ZExtentMm > kMaxAuthoredStepMm)
				{
					Worst.Record(EViolationKind::Step, ZExtentMm, kMaxAuthoredStepMm, SurfaceName,
						Fmt("wall facet %.1f deg from horizontal rises %.3f mm (limit %.3f mm). "
						    "A %.0f mm lip imparts %.0f deg/s of pitch rate against ~%.0f deg/s of KD authority.",
						    SlopeDeg, ZExtentMm, kMaxAuthoredStepMm,
						    kMeasuredKerbStepMm, kMeasuredKerbImpartedPitchRateDegS,
						    kMeasuredKdAuthorityPitchRateDegS));
				}
			}
			else if (SlopeDeg > kMaxAuthoredSlopeDeg)
			{
				Worst.Record(EViolationKind::Slope, SlopeDeg, kMaxAuthoredSlopeDeg, SurfaceName,
					Fmt("facet grade %.3f deg exceeds the authored ceiling %.3f deg "
					    "(0.80 x the %.1f deg the host self-arrests a released board on).",
					    SlopeDeg, kMaxAuthoredSlopeDeg, kMeasuredSelfArrestSlopeDeg));
			}

			++EdgeUse[MakeEdgeKey(V0, V1)];
			++EdgeUse[MakeEdgeKey(V1, V2)];
			++EdgeUse[MakeEdgeKey(V2, V0)];
		}

		// ---- Seam detection --------------------------------------------------------------
		//
		// An edge used by exactly one triangle is a boundary. Within one surface mesh, two
		// boundary edges sitting on top of each other in plan but at different heights are a
		// seam that does not meet -- the millimetre-scale discontinuity a tiling pass or a
		// collision-hull artifact produces, and the one nobody sees by looking.

		struct FBoundary
		{
			double XMm, YMm, ZMm;
		};
		std::vector<FBoundary> Boundaries;
		Boundaries.reserve(EdgeUse.size() / 4 + 1);

		for (const auto& KV : EdgeUse)
		{
			if (KV.second != 1)
			{
				continue;
			}
			const FEdgeKey& K = KV.first;
			Boundaries.push_back(FBoundary{
				0.5 * (K.A[0] + K.B[0]) * kWeldTolMm,
				0.5 * (K.A[1] + K.B[1]) * kWeldTolMm,
				0.5 * (K.A[2] + K.B[2]) * kWeldTolMm });
		}

		// Spatial hash on the plan grid so this stays linear rather than quadratic on a big
		// terrain mesh.
		std::unordered_map<int64_t, std::vector<int>> Grid;
		auto CellKey = [](int64_t GX, int64_t GY) { return (GX << 32) ^ (GY & 0xffffffffll); };
		for (int i = 0; i < static_cast<int>(Boundaries.size()); ++i)
		{
			Grid[CellKey(Quant(Boundaries[i].XMm, kSeamPlanTolMm),
			             Quant(Boundaries[i].YMm, kSeamPlanTolMm))].push_back(i);
		}

		for (int i = 0; i < static_cast<int>(Boundaries.size()); ++i)
		{
			const FBoundary& A = Boundaries[i];
			const int64_t GX = Quant(A.XMm, kSeamPlanTolMm);
			const int64_t GY = Quant(A.YMm, kSeamPlanTolMm);
			for (int64_t DX = -1; DX <= 1; ++DX)
			{
				for (int64_t DY = -1; DY <= 1; ++DY)
				{
					auto It = Grid.find(CellKey(GX + DX, GY + DY));
					if (It == Grid.end())
					{
						continue;
					}
					for (int J : It->second)
					{
						if (J <= i)
						{
							continue;
						}
						const FBoundary& B = Boundaries[J];
						const double DPlan = std::hypot(A.XMm - B.XMm, A.YMm - B.YMm);
						if (DPlan > kSeamPlanTolMm)
						{
							continue;
						}
						const double DZ = std::fabs(A.ZMm - B.ZMm);
						if (DZ > kMaxAuthoredStepMm)
						{
							Worst.Record(EViolationKind::Step, DZ, kMaxAuthoredStepMm, SurfaceName,
								Fmt("open seam: two boundary edges %.3f mm apart in plan differ by "
								    "%.3f mm in height (limit %.3f mm). Terrain must be analytically "
								    "smooth; the board rides out ~%.1f mm at a calm point and nothing at its worst.",
								    DPlan, DZ, kMaxAuthoredStepMm, kMeasuredBestCaseSurvivedStepMm));
						}
					}
				}
			}
		}

		Worst.Flush(OutViolations);
	}

	namespace
	{
		void CheckPlane(const FSurfaceDecl& S, std::vector<FViolation>& Out)
		{
			const double Slope = std::fabs(S.SlopeDeg);

			if (Slope > kMaxAuthoredSlopeDeg)
			{
				Out.push_back(FViolation{ EViolationKind::Slope, S.Name,
					Fmt("declared grade %.3f deg exceeds the authored ceiling %.3f deg "
					    "(0.80 x the %.1f deg the host self-arrests a released board on; outrun at 7.0 deg).",
					    Slope, kMaxAuthoredSlopeDeg, kMeasuredSelfArrestSlopeDeg),
					Slope, kMaxAuthoredSlopeDeg });
			}

			const double Allowed = MaxRunOutM(Slope);
			if (S.RunOutM > Allowed)
			{
				const double V = FreeRollSpeedMS(Slope, S.RunOutM);
				Out.push_back(FViolation{ EViolationKind::RunOut, S.Name,
					Fmt("%.1f m of run-out at %.3f deg, against %.1f m allowed. There is no speed loop: "
					    "released, the board free-rolls at %.2f x g sin(phi) and reaches %.1f m/s by the "
					    "bottom, past the %.2f m/s speed-cap onset. A grade is only as safe as it is short.",
					    S.RunOutM, Slope, Allowed, kFreeRollAccelFractionOfGSinPhi, V, kSpeedCapOnsetMS),
					S.RunOutM, Allowed });
			}
		}

		void CheckProbes(const FSurfaceDecl& S, std::vector<FViolation>& Out)
		{
			FWorstOfEachKind Worst;

			double SpanM = 0.0;
			double RunDropM = 0.0;   // accumulated drop of the current descending run
			double WorstDropM = 0.0;

			for (size_t i = 1; i < S.Probes.size(); ++i)
			{
				const FProbePoint& A = S.Probes[i - 1];
				const FProbePoint& B = S.Probes[i];

				const double DPlan = std::hypot(B.XM - A.XM, B.YM - A.YM);
				const double DZ = B.ZM - A.ZM;
				SpanM += DPlan;

				if (DPlan <= kProbeStepPlanTolM)
				{
					const double StepMm = std::fabs(DZ) * 1000.0;
					if (StepMm > kMaxAuthoredStepMm)
					{
						Worst.Record(EViolationKind::Step, StepMm, kMaxAuthoredStepMm, S.Name,
							Fmt("probes %zu->%zu are %.3f m apart in plan but %.3f mm apart in height: "
							    "a step, not a grade (limit %.3f mm).",
							    i - 1, i, DPlan, StepMm, kMaxAuthoredStepMm));
					}
				}
				else
				{
					const double SlopeDeg = std::atan2(std::fabs(DZ), DPlan) * 180.0 / kPi;
					if (SlopeDeg > kMaxAuthoredSlopeDeg)
					{
						Worst.Record(EViolationKind::Slope, SlopeDeg, kMaxAuthoredSlopeDeg, S.Name,
							Fmt("probes %zu->%zu describe a %.3f deg grade, over the %.3f deg ceiling.",
							    i - 1, i, SlopeDeg, kMaxAuthoredSlopeDeg));
					}
				}

				// A continuous descent accumulates speed regardless of its profile -- free-roll
				// speed depends on the DROP alone. Any climb resets it, because the board sheds
				// that speed again on the way up.
				if (DZ < 0.0)
				{
					RunDropM += -DZ;
					WorstDropM = std::max(WorstDropM, RunDropM);
				}
				else
				{
					RunDropM = 0.0;
				}
			}

			const double MaxDrop = MaxDescentM();
			if (WorstDropM > MaxDrop)
			{
				const double V = std::sqrt(2.0 * kFreeRollAccelFractionOfGSinPhi * kGravityMS2 * WorstDropM);
				Worst.Record(EViolationKind::RunOut, WorstDropM, MaxDrop, S.Name,
					Fmt("a continuous descent loses %.2f m of height against %.2f m allowed; a released "
					    "board reaches %.1f m/s at the bottom, past the %.2f m/s speed-cap onset.",
					    WorstDropM, MaxDrop, V, kSpeedCapOnsetMS));
			}

			if (SpanM < S.ReachM)
			{
				Worst.Record(EViolationKind::Coverage, S.ReachM - SpanM, 0.0, S.Name,
					Fmt("probes span %.1f m of a declared %.1f m reach -- %.1f m of drivable ground is "
					    "unmeasured. Ground nobody measured is not ground that passed.",
					    SpanM, S.ReachM, S.ReachM - SpanM));
			}

			Worst.Flush(Out);
		}
	}

	bool CheckSurface(const FSurfaceDecl& Surface,
	                  const std::string& RepoRoot,
	                  std::vector<FViolation>& OutViolations)
	{
		switch (Surface.Kind)
		{
			case ESurfaceKind::Plane:
				CheckPlane(Surface, OutViolations);
				return true;

			case ESurfaceKind::Probe:
				CheckProbes(Surface, OutViolations);
				return true;

			case ESurfaceKind::External:
				// No verdict is possible on geometry this repo cannot open, and pretending
				// otherwise is the whole failure mode ADR-0011 was called over. The parser has
				// already refused to let this surface claim `verified`; `--strict` and the
				// runtime tag do the rest.
				return true;

			case ESurfaceKind::Mesh:
			{
				OverboardMesh::FStlMesh M;
				std::string Err;
				const std::string Full = RepoRoot + "/" + Surface.Path;
				if (!OverboardMesh::LoadBinaryStl(Full, M, Err))
				{
					OutViolations.push_back(FViolation{ EViolationKind::AssetMissing, Surface.Name,
						"declared mesh could not be read: " + Err
						+ ". An uncheckable surface is never a pass.", 0.0, 0.0 });
					return false;
				}
				CheckMeshGeometry(M, Surface.UnitsToMm, Surface.Name, OutViolations);
				return true;
			}
		}
		return false;
	}
}
