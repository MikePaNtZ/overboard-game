// OverboardWire.h
//
// The wire contract between the controls host (`overboard`) and this game client, fixed by the
// COO for GitHub issue #162 and ratified as ADR-0010. Binding on both sides; do not drift from it
// unilaterally -- report a suspected field error instead of quietly changing it.
//
// State packet is versioned: v1 (72 bytes) is the original contract; v2 (80 bytes) appends two
// f32 rider-ballast fields, everything before unchanged. BOTH are accepted on decode -- v1 means
// "no rider data, treat the rider as neutral", not an error, because whichever side (host or
// game) ships its version bump first must not freeze the other. Only a schema_version that is
// neither 1 nor 2 fails loudly. InputPacket is unversioned-in-practice: still schema_version 1,
// unchanged by the v2 bump.
//
// This header is intentionally UE-free so it can be compiled and unit-tested standalone before
// the Unreal project exists (see wire/README.md).
//
// All multi-byte fields are little-endian and packed with no padding, field order exactly as
// specified. Decoding never trusts struct layout/alignment for the wire representation -- every
// field is read/written at an explicit byte offset (see OverboardWire.cpp) so this is portable
// regardless of compiler packing behaviour.
//
// State in  : host -> game. Host SENDS to 127.0.0.1:9601, so the game BINDS/LISTENS on 9601.
// Input out : game -> host. Host LISTENS on 127.0.0.1:9602, so the game SENDS to 9602.
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace OverboardWire
{
	// ---- Constants -----------------------------------------------------------------------

	constexpr uint32_t kStateMagic = 0x4F425731; // "OBW1"
	constexpr uint32_t kInputMagic = 0x4F424931; // "OBI1"

	// InputPacket's schema is unchanged by the v2 state bump -- still 1.
	constexpr uint16_t kInputSchemaVersion = 1;

	// State packet: v1 is the original 72-byte contract; v2 appends rider_fore_aft_m and
	// rider_lateral_m (8 bytes). Both are accepted on decode -- see header comment above.
	constexpr uint16_t kStateSchemaVersionV1 = 1;
	constexpr uint16_t kStateSchemaVersionV2 = 2;
	// v3 (ADR-0012): appends lin_vel[3] and ang_vel[3] -- the physics-authority handoff needs a
	// velocity to seed Unreal's simulation with, and v2 carried none.
	constexpr uint16_t kStateSchemaVersionV3 = 3;
	constexpr uint16_t kStateSchemaVersionLatest = kStateSchemaVersionV3; // what this repo encodes by default (tests, fake_sender)

	constexpr size_t kStatePacketWireSizeV1 = 72; // see field table below
	constexpr size_t kStatePacketWireSizeV2 = 80; // v1 + rider_fore_aft_m + rider_lateral_m
	constexpr size_t kStatePacketWireSizeV3 = 104; // v2 + lin_vel[3] + ang_vel[3]
	constexpr size_t kInputPacketWireSize = 28;

	// Returns the exact wire size for a given state schema_version, or 0 if the version isn't
	// one this repo understands (1 or 2) -- callers should treat 0 as "fail loudly", not "empty".
	constexpr size_t GetStatePacketWireSize(uint16_t SchemaVersion)
	{
		return SchemaVersion == kStateSchemaVersionV1 ? kStatePacketWireSizeV1
			: SchemaVersion == kStateSchemaVersionV2 ? kStatePacketWireSizeV2
			: SchemaVersion == kStateSchemaVersionV3 ? kStatePacketWireSizeV3
			: 0;
	}

	// State packet flags (bit0 armed, bit1 valid, bit2 fallen, bit3 loss-of-authority warning)
	namespace EStateFlags
	{
		constexpr uint16_t Armed = 1u << 0;
		constexpr uint16_t Valid = 1u << 1;
		constexpr uint16_t Fallen = 1u << 2;

		/// ADR-0011 exit criterion (c), condition 3 of the second ratification: the
		/// loss-of-authority warning. Set while the host's filtered authority utilisation is
		/// over `AUTHORITY_UTILISATION_WARN` AND the board is below speed-cap onset -- the
		/// discriminator the ADR specifies, because saturation ABOVE the onset is survivable
		/// (something is already unloading the board) and a warning that fired on it would be
		/// noise. Measured host-side lead: the warning asserts at 3.000 s, saturation at
		/// 4.920 s, `FALLEN` at 5.868 s. It leads `FALLEN` by **2.868 s**; `FALLEN` itself
		/// TRAILS saturation by -0.948 s, i.e. it annunciates a fall that is already decided.
		///
		/// ============================================================================
		/// THE HOST DOES NOT SET THIS BIT YET. THIS IS THE RECEIVING HALF ONLY.
		/// ============================================================================
		///
		/// `sim-host` computes the signal every cycle (`host.rs`, `authority_warning`) and
		/// currently sends it to **stderr and the trace CSV only** -- it is not on the wire.
		/// Issue overboard-game#19 was filed on the understanding that this "arrives over the
		/// existing state stream"; it does not, and that gap is the reason the warning cannot
		/// reach a player today. Requested from Senior Controls as a work request; until it
		/// lands this bit reads 0 on every packet, which is exactly the neutral, backward-
		/// compatible behaviour a not-yet-set flag bit should have.
		///
		/// **No schema bump.** This is a new bit inside the existing `flags` field, not a wider
		/// wire, so it is backward compatible in both directions -- the same call `sim-host`
		/// itself made for `INPUT_FLAG_KICK` (input bit 2), which was added without touching
		/// `schema_version`. A v1 or v2 packet from a host that does not set it decodes
		/// identically to before.
		constexpr uint16_t AuthorityWarning = 1u << 3;

		/// ADR-0012 physics-authority handoff. Set from the cycle the host declares a
		/// terminating event (bumper contact above threshold, or tilt past 35 deg from
		/// vertical) onward, and cleared ONLY by the input `reset` bit.
		///
		/// While it is set the host has STOPPED PROPAGATING the board: `Pos`, `Quat`,
		/// `LinVel` and `AngVel` are frozen at their strike-cycle values and repeat on
		/// every packet, and this client owns the board's motion until reset.
		///
		/// It is a LEVEL, not an edge, and that is deliberate -- a one-shot edge could be
		/// dropped by the UDP wire and leave this client following a board MuJoCo had
		/// already stopped simulating. A client that misses the announcing packet takes
		/// over on the next one.
		///
		/// **This client must never simulate on inference.** Taking over because the board
		/// "looks fallen" is forbidden by ADR-0012: the authority decision belongs to the
		/// host, and two engines integrating the same board is the failure the latch exists
		/// to prevent.
		constexpr uint16_t PhysicsHandoff = 1u << 4;
	}

	// Input packet flags (bit0 arm, bit1 reset)
	namespace EInputFlags
	{
		constexpr uint16_t Arm = 1u << 0;
		constexpr uint16_t Reset = 1u << 1;
	}

	// ---- State packet (host -> game) ------------------------------------------------------
	//
	// field            type      offset  size   since
	// magic            u32       0       4      v1
	// schema_version   u16       4       2      v1
	// flags            u16       6       2      v1
	// seq              u64       8       8      v1
	// sim_time_s       f64       16      8      v1
	// pos              float[3]  24      12     v1   raw MuJoCo frame: metres, Z-up, right-handed
	// quat             float[4]  36      16     v1   w,x,y,z order, raw MuJoCo
	// wheel_angle_rad  f32       52      4      v1
	// wheel_rate_rad_s f32       56      4      v1
	// pitch_rad        f32       60      4      v1   nose-up positive
	// yaw_rad          f32       64      4      v1   NON-PHYSICAL game steering channel
	// motor_current_a  f32       68      4      v1
	//                                    72 total (v1)
	// rider_fore_aft_m f32       72      4      v2   actual ballast fore/aft displacement, metres, signed
	// rider_lateral_m  f32       76      4      v2   actual ballast lateral displacement, metres, signed
	//                                    80 total (v2)
	// lin_vel          float[3]  80      12     v3   world frame, m/s, raw MuJoCo (Z-up, right-handed)
	// ang_vel          float[3]  92      12     v3   world frame, rad/s, raw MuJoCo, right-hand rule
	//                                    104 total (v3)
	struct FBoardState
	{
		uint32_t Magic = kStateMagic;
		uint16_t SchemaVersion = kStateSchemaVersionLatest;
		uint16_t Flags = 0;
		uint64_t Seq = 0;
		double SimTimeS = 0.0;
		float Pos[3] = {0.f, 0.f, 0.f};
		float Quat[4] = {1.f, 0.f, 0.f, 0.f}; // w, x, y, z
		float WheelAngleRad = 0.f;
		float WheelRateRadS = 0.f;
		float PitchRad = 0.f;
		float YawRad = 0.f;
		float MotorCurrentA = 0.f;
		// v2 only. On a v1 packet these stay at 0 -- "rider neutral, no rider data" -- which is a
		// real, meaningful value here (centred stance), not a sentinel for "missing".
		float RiderForeAftM = 0.f;
		float RiderLateralM = 0.f;
		// v3 only (ADR-0012). On a v1/v2 packet these stay at 0 -- "no velocity data". Unlike
		// the rider fields, zero here is NOT a meaningful physical value, it is an absence:
		// a handoff seeded from a v2 packet would start the crash from rest. The host only
		// ever sets PhysicsHandoff on a v3 packet, so the two cannot come apart.
		//
		// RAW MuJoCo world frame, both of them -- metres/second and radians/second, Z-up and
		// right-handed, exactly as Pos and Quat are. Convert through CoordinateTransform and
		// nowhere else.
		float LinVel[3] = {0.f, 0.f, 0.f};
		float AngVel[3] = {0.f, 0.f, 0.f};
	};

	// Decodes a raw OBW1 packet. Returns false and fills OutError on any failure: short buffer,
	// bad magic, or a schema_version that is not 1, 2 or 3. Never partially trusts a mismatched
	// packet -- on failure OutState is left at default values and must not be used. This is the
	// "fail loudly, never misparse a float" gate from the wire spec: callers must log OutError
	// and drop the packet rather than proceed. A v1 packet is NOT a failure -- OutState.Magic/
	// SchemaVersion will read back as 1 and RiderForeAftM/RiderLateralM stay at their neutral
	// default; that is the documented v1 behaviour, not a partial decode.
	bool DecodeBoardState(const uint8_t* Buffer, size_t Len, FBoardState& OutState, std::string& OutError);

	// Encodes into Buffer, which must be at least GetStatePacketWireSize(State.SchemaVersion)
	// bytes -- 72 for v1, 80 for v2, 104 for v3. Exists mainly so tests and the fake sender tool can produce
	// real OBW1 bytes without duplicating the layout. Encoding an unrecognised SchemaVersion is a
	// caller bug (there is no "OutError" here to fail loudly into); it writes v1-shaped bytes
	// with that version number, which will then correctly fail loudly on decode.
	void EncodeBoardState(const FBoardState& State, uint8_t* Buffer);

	// ---- Input packet (game -> host) -------------------------------------------------------
	//
	// field                     type   offset  size
	// magic                     u32    0       4
	// schema_version            u16    4       2
	// flags                     u16    6       2
	// seq                       u64    8       8
	// weight_shift_fore_aft     f32    16      4   [-1,1]
	// weight_shift_lateral      f32    20      4   [-1,1]
	// steer                     f32    24      4   [-1,1] NON-PHYSICAL
	//                                          28 total
	struct FInputPacket
	{
		uint32_t Magic = kInputMagic;
		uint16_t SchemaVersion = kInputSchemaVersion;
		uint16_t Flags = 0;
		uint64_t Seq = 0;
		float WeightShiftForeAft = 0.f;
		float WeightShiftLateral = 0.f;
		float Steer = 0.f;
	};

	// Encodes into Buffer, which must be at least kInputPacketWireSize bytes.
	void EncodeInputPacket(const FInputPacket& Packet, uint8_t* Buffer);

	// Decodes a raw OBI1 packet. Provided for symmetry/testing (the real host is the only
	// consumer of this direction on the wire; this repo doesn't need it at runtime).
	bool DecodeInputPacket(const uint8_t* Buffer, size_t Len, FInputPacket& OutPacket, std::string& OutError);
}
