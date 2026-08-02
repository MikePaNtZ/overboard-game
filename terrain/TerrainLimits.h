// TerrainLimits.h
//
// THE AUTHORED-WORLD ENVELOPE — ADR-0011 second ratification, condition 2.
//
// ADR-0011 moved the kerb-strike and static-robustness criteria off the game gate and onto the
// hardware gate. That move is honest ONLY under three conditions, and condition 2 is:
//
//     "The authored world is constrained to what the controller survives, and the constraint is
//      encoded as a checkable asset rule -- not a hope."
//
// This header is that rule's numeric half. `SurfaceCheck` and `terraincheck` are the enforcing
// half. If this file is ever edited to widen a limit so a level passes, the criterion move has
// become the softening manoeuvre the ADR forbids -- so every constant below carries the
// measurement it came from, and the two that are CHOICES rather than measurements say so in
// those words.
//
// ============================================================================================
// WHY A RENDERER HAS AN OPINION ABOUT TERRAIN AT ALL
// ============================================================================================
//
// This repo computes no physics (ADR-0009). MuJoCo rides a single flat, level 20x20 m plane
// (`sim/models/overboard_onewheel.xml`, `<geom name="ground" type="plane">`); everything drawn
// here is scenery the board is composited over. So why check it?
//
//   1. A drawn surface that deviates from the ridden one makes the render FALSE. The board would
//      be drawn riding a kerb it never rode. Under the `Playable Sim` provenance rules a game run
//      may state facts about the machinery and never a result -- footage of the board crossing
//      authored geometry it did not physically cross is exactly such a result.
//
//   2. The drawn world is the SPECIFICATION for the ridden one. ADR-0010 cut terrain from the
//      wire and explicitly did not cancel it ("Deliberately absent... none are cancelled").
//      The day terrain reaches the physics, the world authored here becomes the world ridden
//      there. A limit encoded now binds then. A limit written in prose now will not.
//
// Neither reason requires this repo to compute anything. These are constraints ON assets,
// parameterised by numbers MEASURED IN `overboard`. Nothing here re-derives them.
//
// ============================================================================================
// PROVENANCE — every number below was measured elsewhere
// ============================================================================================
//
// | source                          | what it established                                      |
// |---------------------------------|----------------------------------------------------------|
// | overboard#205 (merged)          | step survivability; static pitch-error band              |
// | overboard#207 (merged, 01cc9cf) | incline tolerance, free-roll accel, self-arrest limit    |
// | ADR-0011, both ratifications    | the envelope, the reserve pattern, what may not be tuned |
//
#pragma once

#include <cmath>

namespace OverboardTerrain
{
	// ---- Measured inputs, quoted verbatim from `overboard` --------------------------------
	//
	// These are NOT thresholds. They are the measurements the thresholds below are derived
	// from, kept separate so a future re-measurement lands in one obvious place and the
	// derivation re-runs rather than being re-argued.

	/// The largest lip the board rode out at full stick, at a CALM point of the hold
	/// (overboard#205). At the hold's worst point it rode out **nothing**.
	///
	/// Read that sentence twice before using this number. It is not "1 mm is safe". It is
	/// "1 mm is the largest thing ever survived, and only at the best possible moment."
	constexpr double kMeasuredBestCaseSurvivedStepMm = 1.0;

	/// A 20 mm lip -- brick edge, root heave, ordinary pavement -- imparts 201 deg/s of pitch
	/// rate against a KD channel affording about 76 deg/s at zero lean (overboard#205). A 2.6x
	/// authority deficit; ADR-0011 concluded the undersized elements are ballast stroke and CoM
	/// height, so no software change moves this.
	constexpr double kMeasuredKerbStepMm = 20.0;
	constexpr double kMeasuredKerbImpartedPitchRateDegS = 201.0;
	constexpr double kMeasuredKdAuthorityPitchRateDegS = 76.0;

	/// Free-roll acceleration of a RELEASED board on a grade, as a fraction of g*sin(phi)
	/// (overboard#207). The remainder is drivetrain and hinge damping. There is no speed loop:
	/// `MAX_GROUND_SPEED_M_S` shapes the STICK, and a stick cap cannot brake against gravity,
	/// so a released board on a grade accelerates with nothing in the host intervening --
	/// 43 m/s on a 15 deg grade inside one 18.5 s schedule.
	constexpr double kFreeRollAccelFractionOfGSinPhi = 0.70;

	/// Steepest grade the host arrests a released board on, using the largest sustained braking
	/// it commands on its own (0.6 lean, the corridor brake). Outrun at 7.0 deg (overboard#207).
	constexpr double kMeasuredSelfArrestSlopeDeg = 6.5;

	/// Speed at which `SPEED_CAP_MARGIN_M_S` begins withdrawing accelerating fore/aft authority
	/// (`SPEED_CAP_ONSET_M_S`, overboard `host.rs`). Used below as the speed a free-rolling
	/// board may not exceed by the bottom of an authored descent.
	constexpr double kSpeedCapOnsetMS = 8.34;

	/// Standard gravity, matching MuJoCo's default and `overboard`'s own constant.
	constexpr double kGravityMS2 = 9.80665;

	/// The command-envelope reserve fraction ADR-0011 derived and pinned
	/// (`CMD_ENVELOPE_RESERVE`, overboard#205/#207). Reused below as the margin on the slope
	/// ceiling rather than inventing a second, unrelated safety factor.
	constexpr double kCommandEnvelopeReserve = 0.80;

	// ---- The limits ------------------------------------------------------------------------

	/// MAXIMUM AUTHORED STEP DISCONTINUITY, in millimetres.
	///
	/// **This is a geometric noise floor, NOT a survivability allowance.** The distinction is
	/// the whole point. The controller rides out ~1 mm at its calmest and nothing at its worst,
	/// so the only defensible authored rule is that steps are ZERO -- terrain must be
	/// analytically smooth. A tolerance is still needed, because "zero" is not a thing floating
	/// point can represent across a kilometre-scale level, so it is set from REPRESENTATION,
	/// not from survival:
	///
	///   float32 ulp at 1 km from the world origin = 0.119 mm.
	///
	/// 0.25 mm is 2x that floor -- large enough that mesh precision at the far edge of a large
	/// level cannot produce a false positive, and 4x BELOW the best-case survived step so it can
	/// never be read as permission. Anything this check reports is a feature somebody authored,
	/// not a rounding artifact.
	///
	/// Deriving this from `kMeasuredBestCaseSurvivedStepMm` instead would be deriving the
	/// acceptance number from what happened to pass -- the precise move ADR-0011's criterion (f)
	/// exists to forbid.
	constexpr double kMaxAuthoredStepMm = 0.25;

	/// MAXIMUM AUTHORED SLOPE, in degrees.
	///
	/// **Note the premise that changed.** ADR-0011 and issue #208 both predicted "under 0.5 deg",
	/// reasoning that a slope is an effective static pitch disturbance and therefore eats the
	/// measured +-0.25 deg static-error band. overboard#207 measured it and the middle step
	/// fails: a static pitch ESTIMATE ERROR is a signal the controller cannot see, so it
	/// regulates a lie forever; a SLOPE is one it can see, because the IMU still measures true
	/// gravity, so attitude stays correct on a hill and the grade arrives as an ordinary
	/// along-track force the balance loop already leans against. Different disturbances,
	/// different tolerances. The ADR-0011 matrix inverts NOWHERE in +-12 deg (hold) / +-8 deg
	/// (reversal) -- at least 24x the prediction.
	///
	/// What binds instead is RUNAWAY, and it is a different rule -- see `MaxRunOutM` below.
	/// The ceiling here is where a released board stops being recoverable by anything the host
	/// commands: 6.5 deg self-arrest, outrun at 7.0.
	///
	/// **The 0.80 is a CHOICE, and this is the one number in this file that is.** It is the
	/// reserve fraction ADR-0011 already derived and pinned for the command envelope, reused
	/// rather than replaced by a fresh invented margin -- a stated convention, not a second
	/// measurement. Tuning it up to admit a level would be tuning to the cliff, which ADR-0011
	/// forbids by name.
	constexpr double kMaxAuthoredSlopeDeg = kCommandEnvelopeReserve * kMeasuredSelfArrestSlopeDeg; // 5.2

	/// Above this facet inclination a surface is treated as a WALL rather than a ramp, and its
	/// vertical extent is judged as a step instead of as a slope. A kerb is exactly this: a
	/// near-vertical face with 20 mm of Z extent. 60 deg is well above any rideable grade and
	/// well below vertical, so the classification is never marginal.
	constexpr double kWallFacetSlopeDeg = 60.0;

	// ---- Derived rules ---------------------------------------------------------------------

	/// MAXIMUM RUN-OUT LENGTH for an authored descent at `SlopeDeg`, in metres.
	///
	/// The binding constraint on an incline is not inversion, it is that **there is no speed
	/// loop.** Released on a grade, the board accelerates at `0.70 * g * sin(phi)` with nothing
	/// in the host intervening (overboard#207). So an authored grade is only as safe as it is
	/// SHORT, and the asset rule needs an angle AND a length: 0.25 deg is fine for a metre and
	/// not for a kilometre.
	///
	/// From rest, `v = sqrt(2 a L)`. Requiring the board not to exceed speed-cap onset by the
	/// bottom of the descent gives:
	///
	///     L_max(phi) = kSpeedCapOnsetMS^2 / (2 * 0.70 * g * sin(phi))
	///
	/// **Cross-check, and the reason to trust this formula rather than the arithmetic in it:**
	/// overboard#207 independently reports "~1159 m to speed-cap onset from rest" at 0.25 deg.
	/// This returns 1161.09 m -- 0.18% apart. The model is validated against the controls role's
	/// own number rather than invented here. `test_terrain` asserts that agreement, so if either
	/// side's constants move, the disagreement is a red build rather than a silent divergence.
	///
	/// Returns infinity for a level surface, which is correct: a 0 deg grade has no run-out
	/// limit.
	inline double MaxRunOutM(double SlopeDeg)
	{
		const double S = std::sin(std::fabs(SlopeDeg) * 3.14159265358979323846 / 180.0);
		if (S <= 0.0)
		{
			return HUGE_VAL;
		}
		return (kSpeedCapOnsetMS * kSpeedCapOnsetMS)
			/ (2.0 * kFreeRollAccelFractionOfGSinPhi * kGravityMS2 * S);
	}

	/// MAXIMUM TOTAL DESCENT, in metres of vertical drop.
	///
	/// The same rule as `MaxRunOutM`, stated in the variable it actually depends on. Free-roll
	/// speed comes from the drop alone -- `v = sqrt(2 * 0.70 * g * dh)` -- so the profile in
	/// between does not matter: a long shallow ramp and a short steep one that lose the same
	/// height release the board at the same speed. This is the form the probe check uses,
	/// because a measured path gives heights directly and slope only by differencing.
	///
	///     dh_max = kSpeedCapOnsetMS^2 / (2 * 0.70 * g)  =  5.07 m
	///
	/// `MaxRunOutM(phi)` is exactly `dh_max / sin(phi)`; `test_terrain` asserts the two agree,
	/// so the two expressions of one rule cannot drift apart.
	inline double MaxDescentM()
	{
		return (kSpeedCapOnsetMS * kSpeedCapOnsetMS) / (2.0 * kFreeRollAccelFractionOfGSinPhi * kGravityMS2);
	}

	/// Speed a released board reaches from rest after `RunOutM` of descent at `SlopeDeg`, m/s.
	/// The inverse of the above; reported in violation messages so the failure states a physical
	/// consequence ("reaches 14.2 m/s") rather than only a broken inequality.
	inline double FreeRollSpeedMS(double SlopeDeg, double RunOutM)
	{
		const double S = std::sin(std::fabs(SlopeDeg) * 3.14159265358979323846 / 180.0);
		const double A = kFreeRollAccelFractionOfGSinPhi * kGravityMS2 * S;
		if (A <= 0.0 || RunOutM <= 0.0)
		{
			return 0.0;
		}
		return std::sqrt(2.0 * A * RunOutM);
	}
}
