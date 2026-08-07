// EngageWireMapping.h
//
// overboard-game#28: explicit rider start/stop and stationary yaw-aiming. The feature needs
// three things on the wire -- a start/engage request (game -> host), an engaged/disengaged state
// (host -> game), and a yaw-aim command honoured only while disengaged (game -> host) -- and NONE
// of the three has a dedicated field yet. The controls-side contract is still in flight (#28's
// "Blocked on" section: "I will post the exact contract on this issue as soon as the controls
// side lands"), so this file is the SINGLE place that pins which existing wire bit/field stands in
// for each until that contract lands. When it does, only the three lines below should need to
// change -- nowhere else in this module reaches into OverboardWire's flags/fields for this feature.
//
// ============================================================================================
// WHY THESE THREE CHOICES, NOT NEW BITS/FIELDS
// ============================================================================================
//
// Engage request and engaged state reuse OverboardWire::EInputFlags::Arm and
// OverboardWire::EStateFlags::Armed respectively -- both ALREADY EXIST, both already look like
// they were put there for exactly this (Space/Gamepad-A were already bound to Arm since #162,
// before this feature existed), and reusing an existing bit costs nothing (no schema bump).
// Whether "arm" already means "engage the motor" or is being overloaded by this reuse is exactly
// the question this file's job is to make cheap to revisit -- see each constant's own comment for
// the current, honest answer.
//
// Yaw-aim has no existing bit to reuse -- it is a continuous [-1,1] command, and
// OverboardWire::FInputPacket has no spare float. Adding one is a schema version bump, and #28 is
// explicit that "the CEO approves any version bump, not the implementer." So this multiplexes the
// command onto the existing `steer` field instead of guessing at a new one -- see
// kYawAimSharesSteerField's comment for exactly how and why that is safe today.
#pragma once

#include "OverboardWire.h"

namespace OverboardEngageWire
{
	// Game -> host: press start / engage. `OverboardWire::EInputFlags::Arm` -- bound to Space and
	// Gamepad Face-Button-Bottom (A) in `AOverboardPlayerController::SetupInputComponent` since
	// #162, well before this feature was filed. `sim-host`'s own `host.rs` tracks and logs this bit
	// today but does not gate anything on it ("armed unconditionally at startup... this host does
	// not gate balancing on the input socket's arm bit"), which reads as "the intended channel,
	// just not wired up yet" rather than an overload -- exactly what #28's engage-sequence work is
	// expected to change.
	constexpr uint16_t kEngageRequestInputFlag = OverboardWire::EInputFlags::Arm;

	// Host -> game: engaged/disengaged. `OverboardWire::EStateFlags::Armed` -- same reasoning as
	// above, in the other direction. CAVEAT, load-bearing for anyone reading a PIE session against
	// a real host today: `sim-host`'s `host.rs` currently sets this bit UNCONDITIONALLY every tick
	// (`wire::STATE_FLAG_ARMED | wire::STATE_FLAG_VALID`, not gated on any engage sequence), so it
	// reads as permanently "engaged" until the controls-side engage-sequence work lands. That is
	// the correct, honest thing for this client to render in the meantime -- rendering "disengaged"
	// against a host that has not implemented disengagement would be exactly the local prediction
	// ADR-0009/#28 forbid.
	constexpr uint16_t kEngagedStateFlag = OverboardWire::EStateFlags::Armed;

	// Game -> host: yaw-aim while disengaged. `OverboardWire::FInputPacket::Steer` -- the existing
	// NON-PHYSICAL steering channel, sent UNCONDITIONALLY (same shaped stick/key value regardless
	// of engage state -- AOverboardPlayerController::SendInputPacket does not gate this), and
	// multiplexed by `sim-host` instead, on the RECEIVE side:
	//   - engaged:    Steer carries the ordinary riding-turn command, exactly as before this
	//                 feature (`sim-host`'s existing curvature yaw law). UNCHANGED, not "noise" --
	//                 this is a live, separately-tuned feature and must not regress.
	//   - disengaged: Steer carries the yaw-aim command instead.
	// Gating the SEND here would either be a no-op (same value, same field) or would zero riding-
	// turn while engaged -- see SendInputPacket's own comment. This is safe to leave unconditional
	// specifically because `sim-host`'s existing yaw law
	// (`yaw_rate_rad_s = steer * curvature(speed) * |speed| * roll_authority`) is already
	// identically ZERO at v=0 -- "you cannot carve a stationary onewheel" (host.rs's own comment)
	// -- so nothing today reads `steer` while parked, and this reuse cannot collide with any live
	// behaviour. `inject_kinematic_yaw` (#163) is already the mechanism the engage-sequence work is
	// expected to extend with a stationary-pivot rate law, gated on engaged/disengaged instead of
	// speed, reading the exact same field.
	constexpr bool kYawAimSharesSteerField = true;
}
