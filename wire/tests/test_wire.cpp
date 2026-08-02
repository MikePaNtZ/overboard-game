// Standalone test harness for the OBW1/OBI1 wire layer and the MuJoCo -> Unreal transform.
// Builds and runs with plain clang++, no Unreal dependency. See wire/README.md.
#include "../OverboardWire.h"
#include "../CoordinateTransform.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace OverboardWire;

namespace
{
	int gFailures = 0;

	void Check(bool Cond, const char* Msg)
	{
		if (!Cond)
		{
			std::printf("  FAIL: %s\n", Msg);
			++gFailures;
		}
	}

	bool NearlyEqual(double A, double B, double Eps = 1e-5)
	{
		return std::fabs(A - B) <= Eps;
	}

	// ---- State packet decode: golden bytes built by hand from the field table -------------
	void Test_DecodeValidStatePacketV1()
	{
		std::printf("Test_DecodeValidStatePacketV1\n");

		FBoardState In;
		In.SchemaVersion = kStateSchemaVersionV1;
		In.Flags = EStateFlags::Armed | EStateFlags::Valid;
		In.Seq = 424242;
		In.SimTimeS = 12.5;
		In.Pos[0] = 1.5f; In.Pos[1] = -2.25f; In.Pos[2] = 0.75f;
		In.Quat[0] = 0.9f; In.Quat[1] = 0.1f; In.Quat[2] = 0.2f; In.Quat[3] = 0.3f;
		In.WheelAngleRad = 1.1f;
		In.WheelRateRadS = 2.2f;
		In.PitchRad = 0.05f;
		In.YawRad = -0.4f;
		In.MotorCurrentA = 8.5f;
		// Deliberately NOT setting RiderForeAftM/RiderLateralM -- v1 has no rider data at all;
		// this is the "old host, new game client" side of the compatibility contract.

		uint8_t Buf[kStatePacketWireSizeV1];
		EncodeBoardState(In, Buf);

		FBoardState Out;
		std::string Err;
		bool Ok = DecodeBoardState(Buf, sizeof(Buf), Out, Err);
		Check(Ok, "decode should succeed on a well-formed v1 packet");
		Check(Err.empty(), "no error message on success");
		Check(Out.SchemaVersion == kStateSchemaVersionV1, "schema_version round-trips as 1");
		Check(Out.Flags == In.Flags, "flags round-trip");
		Check(Out.Seq == In.Seq, "seq round-trip");
		Check(NearlyEqual(Out.SimTimeS, In.SimTimeS), "sim_time_s round-trip");
		Check(NearlyEqual(Out.Pos[0], In.Pos[0]) && NearlyEqual(Out.Pos[1], In.Pos[1]) && NearlyEqual(Out.Pos[2], In.Pos[2]),
			"pos round-trip");
		Check(NearlyEqual(Out.Quat[0], In.Quat[0]) && NearlyEqual(Out.Quat[1], In.Quat[1]) &&
			NearlyEqual(Out.Quat[2], In.Quat[2]) && NearlyEqual(Out.Quat[3], In.Quat[3]), "quat round-trip");
		Check(NearlyEqual(Out.WheelAngleRad, In.WheelAngleRad), "wheel_angle_rad round-trip");
		Check(NearlyEqual(Out.WheelRateRadS, In.WheelRateRadS), "wheel_rate_rad_s round-trip");
		Check(NearlyEqual(Out.PitchRad, In.PitchRad), "pitch_rad round-trip");
		Check(NearlyEqual(Out.YawRad, In.YawRad), "yaw_rad round-trip");
		Check(NearlyEqual(Out.MotorCurrentA, In.MotorCurrentA), "motor_current_a round-trip");
		// v1 -> "rider neutral", not garbage and not left uninitialised.
		Check(NearlyEqual(Out.RiderForeAftM, 0.0), "v1 packet: rider_fore_aft_m defaults to neutral (0)");
		Check(NearlyEqual(Out.RiderLateralM, 0.0), "v1 packet: rider_lateral_m defaults to neutral (0)");

		// Also nail down exact byte offsets against the field table, not just round-trip via
		// our own encoder (which could hide a matched pair of bugs).
		uint32_t MagicAtZero;
		std::memcpy(&MagicAtZero, Buf, 4);
		Check(MagicAtZero == kStateMagic, "magic sits at byte offset 0, little-endian");
		Check(sizeof(Buf) == 72, "v1 state packet wire size is 72 bytes");
	}

	// v2 (overboard#162): appends rider_fore_aft_m/rider_lateral_m. This is the "new host, and
	// this game client" side of the compatibility contract -- the actual rider values must
	// round-trip, not just default to neutral.
	void Test_DecodeValidStatePacketV2()
	{
		std::printf("Test_DecodeValidStatePacketV2\n");

		FBoardState In; // SchemaVersion defaults to kStateSchemaVersionLatest (2)
		Check(In.SchemaVersion == kStateSchemaVersionV2, "FBoardState defaults to v2");
		In.Flags = EStateFlags::Armed | EStateFlags::Valid;
		In.Seq = 99;
		In.SimTimeS = 3.0;
		In.RiderForeAftM = 0.03f;   // ~3cm forward -- realistic ballast displacement, not exaggerated
		In.RiderLateralM = -0.04f;  // ~4cm lateral, the COO's stated "full lateral" figure

		uint8_t Buf[kStatePacketWireSizeV2];
		EncodeBoardState(In, Buf);
		Check(sizeof(Buf) == 80, "v2 state packet wire size is 80 bytes");

		FBoardState Out;
		std::string Err;
		bool Ok = DecodeBoardState(Buf, sizeof(Buf), Out, Err);
		Check(Ok, "decode should succeed on a well-formed v2 packet");
		Check(Out.SchemaVersion == kStateSchemaVersionV2, "schema_version round-trips as 2");
		Check(NearlyEqual(Out.RiderForeAftM, In.RiderForeAftM), "rider_fore_aft_m round-trip");
		Check(NearlyEqual(Out.RiderLateralM, In.RiderLateralM), "rider_lateral_m round-trip");
		// Base v1 fields must still work unchanged in a v2 packet.
		Check(Out.Seq == In.Seq, "v2 packet: base seq field still round-trips");
		Check(NearlyEqual(Out.SimTimeS, In.SimTimeS), "v2 packet: base sim_time_s field still round-trips");
	}

	void Test_DecodeRejectsBadMagic()
	{
		std::printf("Test_DecodeRejectsBadMagic\n");
		FBoardState In;
		uint8_t Buf[kStatePacketWireSizeV2];
		EncodeBoardState(In, Buf);
		Buf[0] = 0x00; // corrupt magic

		FBoardState Out;
		std::string Err;
		bool Ok = DecodeBoardState(Buf, sizeof(Buf), Out, Err);
		Check(!Ok, "decode must fail on bad magic");
		Check(!Err.empty(), "error message must be set on bad magic (fail loudly)");
		Check(Err.find("magic") != std::string::npos, "error should mention magic");
	}

	void Test_DecodeRejectsUnknownSchemaVersion()
	{
		std::printf("Test_DecodeRejectsUnknownSchemaVersion\n");
		FBoardState In;
		In.SchemaVersion = 3; // neither 1 nor 2 -- this build must not guess
		uint8_t Buf[kStatePacketWireSizeV1]; // Encode only writes v1-shaped bytes for a non-v2 version
		EncodeBoardState(In, Buf);

		FBoardState Out;
		std::string Err;
		bool Ok = DecodeBoardState(Buf, sizeof(Buf), Out, Err);
		Check(!Ok, "decode must fail on schema_version 3 (only 1 and 2 are understood)");
		Check(Err.find("schema_version") != std::string::npos, "error should mention schema_version");
	}

	void Test_DecodeRejectsShortBuffer()
	{
		std::printf("Test_DecodeRejectsShortBuffer\n");
		uint8_t Buf[10] = {0};
		FBoardState Out;
		std::string Err;
		bool Ok = DecodeBoardState(Buf, sizeof(Buf), Out, Err);
		Check(!Ok, "decode must fail on truncated buffer instead of reading past it");
		Check(!Err.empty(), "error message set on short buffer");
	}

	void Test_DecodeRejectsShortV2Buffer()
	{
		std::printf("Test_DecodeRejectsShortV2Buffer\n");
		// A v1-sized buffer (72 bytes) claiming to be v2 -- long enough to pass the v1 floor
		// but short of the 80 bytes v2 actually needs. Must not read 8 bytes past the end.
		FBoardState In;
		In.SchemaVersion = kStateSchemaVersionV2;
		// Encode into a FULL v2-sized buffer. An earlier version of this test encoded into a
		// v1-sized (72 byte) array, on the assumption that EncodeBoardState "only writes the
		// v1-shaped bytes" for a truncated buffer. It does not -- it writes according to
		// SchemaVersion, so a v2 encode wrote 80 bytes into 72 and smashed the stack. The
		// truncation being tested belongs in the LENGTH passed to the decoder, not in the
		// size of the buffer handed to the encoder.
		uint8_t Buf[kStatePacketWireSizeV2];
		EncodeBoardState(In, Buf);

		FBoardState Out;
		std::string Err;
		// Claim only v1 many bytes arrived: a v2 packet truncated on the wire.
		bool Ok = DecodeBoardState(Buf, kStatePacketWireSizeV1, Out, Err);
		Check(!Ok, "decode must fail on a v2 packet truncated to v1 length, not read past the buffer");
		Check(!Err.empty(), "error message set on short v2 buffer");
	}

	void Test_InputPacketRoundTrip()
	{
		std::printf("Test_InputPacketRoundTrip\n");
		FInputPacket In;
		In.Flags = EInputFlags::Arm;
		In.Seq = 7;
		In.WeightShiftForeAft = 0.4f;
		In.WeightShiftLateral = -0.6f;
		In.Steer = 1.0f;

		uint8_t Buf[kInputPacketWireSize];
		EncodeInputPacket(In, Buf);
		Check(sizeof(Buf) == 28, "input packet wire size is 28 bytes");

		FInputPacket Out;
		std::string Err;
		bool Ok = DecodeInputPacket(Buf, sizeof(Buf), Out, Err);
		Check(Ok, "input packet should decode");
		Check(Out.Flags == In.Flags, "input flags round-trip");
		Check(Out.Seq == In.Seq, "input seq round-trip");
		Check(NearlyEqual(Out.WeightShiftForeAft, In.WeightShiftForeAft), "weight_shift_fore_aft round-trip");
		Check(NearlyEqual(Out.WeightShiftLateral, In.WeightShiftLateral), "weight_shift_lateral round-trip");
		Check(NearlyEqual(Out.Steer, In.Steer), "steer round-trip");
	}

	// ---- Coordinate transform --------------------------------------------------------------
	void Test_TransformIdentity()
	{
		std::printf("Test_TransformIdentity\n");
		float Pos[3] = {0.f, 0.f, 0.f};
		float Quat[4] = {1.f, 0.f, 0.f, 0.f};
		FUeTransform Out = MuJoCoToUnreal(Pos, Quat);
		Check(NearlyEqual(Out.PosCm[0], 0.0) && NearlyEqual(Out.PosCm[1], 0.0) && NearlyEqual(Out.PosCm[2], 0.0),
			"identity position stays at origin");
		Check(NearlyEqual(Out.QuatWXYZ[0], 1.0) && NearlyEqual(Out.QuatWXYZ[1], 0.0) &&
			NearlyEqual(Out.QuatWXYZ[2], 0.0) && NearlyEqual(Out.QuatWXYZ[3], 0.0), "identity quat stays identity");
	}

	void Test_TransformPosition()
	{
		std::printf("Test_TransformPosition\n");
		float Pos[3] = {1.5f, 2.0f, -0.5f};
		float Quat[4] = {1.f, 0.f, 0.f, 0.f};
		FUeTransform Out = MuJoCoToUnreal(Pos, Quat);
		Check(NearlyEqual(Out.PosCm[0], 150.0), "X: metres -> cm, no sign flip");
		Check(NearlyEqual(Out.PosCm[1], -200.0), "Y: metres -> cm, sign flips (handedness)");
		Check(NearlyEqual(Out.PosCm[2], -50.0), "Z: metres -> cm, no sign flip");
	}

	// ---- Rotation derivations ---------------------------------------------------------------
	//
	// Both frames put X = forward, Z = up. MuJoCo is right-handed, so its Y = LEFT (the only
	// basis that makes X-forward, Z-up right-handed: X x Y = Z requires Y = left). UE is
	// left-handed with Y = RIGHT. That is the entire physical content of "mirror Y".
	//
	// General rule for reflecting a rotation through a single mirrored axis (proved by
	// conjugating the rotation matrix R by the reflection M = diag(1,-1,1): M R M represents the
	// same physical rotation re-expressed in the mirrored frame, and equals a rotation by -theta
	// about the axis-with-Y-negated): components along the mirrored axis (y) and the scalar (w)
	// are unchanged; components along the two non-mirrored axes (x, z) negate. That is exactly
	// (w,x,y,z) -> (w,-x,y,-z), and it is a physical-consistency requirement, not an arbitrary
	// sign choice: mirroring the coordinate LABELS must not mirror the EVENT being rendered.
	// A rider leaning left must still render as leaning left; nose-up must still render as
	// nose-up. Each test below picks a physically-named MuJoCo attitude, derives what it must
	// look like once re-expressed in UE's own left-handed convention (independently, via the
	// standard axis-angle rotation matrices for each frame), and asserts MuJoCoToUnreal produces
	// exactly that quaternion. A handedness bug -- e.g. dropping a sign, or negating the wrong
	// pair of components -- passes "it compiles" but fails these because the physical direction
	// comes out mirrored (nose-up renders as nose-down, roll-right renders as roll-left, etc.),
	// which is precisely the failure mode that survives eyeballing a symmetric placeholder box.

	// pitch_rad is spec'd "nose-up positive" (see OverboardWire.h). Using the standard
	// right-handed rotation matrix about +Y, R_y(theta)*X = (cos theta, 0, -sin theta): a
	// *positive* theta about MuJoCo's raw +Y axis tips the forward vector toward -Z (nose DOWN).
	// So "nose up by Phi" is theta = -Phi about +Y, i.e. quat (cos(Phi/2), 0, -sin(Phi/2), 0).
	void Test_TransformPureNoseUpPitch()
	{
		std::printf("Test_TransformPureNoseUpPitch\n");
		const double Phi = 0.3; // radians, nose-up magnitude
		float Pos[3] = {0.f, 0.f, 0.f};
		float MuJoCoNoseUp[4] = {
			static_cast<float>(std::cos(Phi / 2.0)),
			0.f,
			static_cast<float>(-std::sin(Phi / 2.0)),
			0.f,
		};
		FUeTransform Out = MuJoCoToUnreal(Pos, MuJoCoNoseUp);
		// y is the mirrored axis's own component -> unchanged; x, z stay 0 -> unchanged. The
		// result is numerically identical to the input, and by the same R_y(theta)*X argument
		// applied in UE's frame, (w, 0, -sin(Phi/2), 0) is ALSO nose-up there (rotation about
		// +Y by -Phi tips UE's forward vector toward +Z). Pitch does not touch Y itself, so this
		// axis is the one case where "unchanged" is the physically-correct answer, not a
		// red flag.
		Check(NearlyEqual(Out.QuatWXYZ[0], MuJoCoNoseUp[0]), "nose-up pitch: w unchanged");
		Check(NearlyEqual(Out.QuatWXYZ[1], 0.0), "nose-up pitch: x stays 0");
		Check(NearlyEqual(Out.QuatWXYZ[2], MuJoCoNoseUp[2]), "nose-up pitch: y (mirrored axis, unaffected by pitch) unchanged");
		Check(NearlyEqual(Out.QuatWXYZ[3], 0.0), "nose-up pitch: z stays 0");
	}

	// Yaw sign is not spec'd (yaw_rad is a non-physical game channel with no fixed convention),
	// so this picks one and names it: MuJoCo's raw +Z rotation swings the forward vector toward
	// +Y, i.e. toward MuJoCo-LEFT (from R_z(theta)*X = (cos theta, sin theta, 0)). Quat:
	// (cos(Phi/2), 0, 0, sin(Phi/2)).
	void Test_TransformPureYawTowardLeft()
	{
		std::printf("Test_TransformPureYawTowardLeft\n");
		const double Phi = 0.3; // radians
		float Pos[3] = {0.f, 0.f, 0.f};
		float MuJoCoYawLeft[4] = {
			static_cast<float>(std::cos(Phi / 2.0)),
			0.f,
			0.f,
			static_cast<float>(std::sin(Phi / 2.0)),
		};
		FUeTransform Out = MuJoCoToUnreal(Pos, MuJoCoYawLeft);
		// z is one of the two mirrored components -> negates: (w, 0, 0, -sin(Phi/2)). That is
		// rotation about UE's +Z by -Phi, which by the identical R_z(theta)*X argument swings
		// UE's forward vector toward -Y. UE's +Y is RIGHT, so -Y is UE-LEFT. MuJoCo-left in ->
		// UE-left out: the physical direction survives the relabelling, which is the point.
		// (A handedness bug that failed to negate z, or negated the wrong component, would
		// instead render this as a turn to the right -- silently mirrored.)
		Check(NearlyEqual(Out.QuatWXYZ[0], MuJoCoYawLeft[0]), "yaw-left: w unchanged");
		Check(NearlyEqual(Out.QuatWXYZ[1], 0.0), "yaw-left: x stays 0");
		Check(NearlyEqual(Out.QuatWXYZ[2], 0.0), "yaw-left: y stays 0");
		Check(NearlyEqual(Out.QuatWXYZ[3], -MuJoCoYawLeft[3]), "yaw-left: z negates (physical left is preserved -- see comment)");
	}

	// Roll about MuJoCo's raw +X (forward) axis. This is the axis the widened wheel geom will
	// exercise most on Sunday (lean side to side while balancing). From R_x(theta)*Z =
	// (0, -sin theta, cos theta): a positive theta about +X tips the up vector toward -Y, i.e.
	// toward MuJoCo-RIGHT (since +Y is left) -- the right edge dips. Call that "roll right".
	// Quat: (cos(Phi/2), sin(Phi/2), 0, 0).
	void Test_TransformPureRollRight()
	{
		std::printf("Test_TransformPureRollRight\n");
		const double Phi = 0.3; // radians
		float Pos[3] = {0.f, 0.f, 0.f};
		float MuJoCoRollRight[4] = {
			static_cast<float>(std::cos(Phi / 2.0)),
			static_cast<float>(std::sin(Phi / 2.0)),
			0.f,
			0.f,
		};
		FUeTransform Out = MuJoCoToUnreal(Pos, MuJoCoRollRight);
		// x is the other mirrored component -> negates: (w, -sin(Phi/2), 0, 0). That is rotation
		// about UE's +X by -Phi. By the same R_x(theta)*Z argument, a *negative* theta about +X
		// tips UE's up vector toward +Y -- and UE's +Y is RIGHT, so the right edge dips there
		// too. Roll-right in -> roll-right out; physical direction preserved, same as yaw.
		Check(NearlyEqual(Out.QuatWXYZ[0], MuJoCoRollRight[0]), "roll-right: w unchanged");
		Check(NearlyEqual(Out.QuatWXYZ[1], -MuJoCoRollRight[1]), "roll-right: x negates (physical right-dip is preserved -- see comment)");
		Check(NearlyEqual(Out.QuatWXYZ[2], 0.0), "roll-right: y stays 0");
		Check(NearlyEqual(Out.QuatWXYZ[3], 0.0), "roll-right: z stays 0");
	}
}

int main()
{
	Test_DecodeValidStatePacketV1();
	Test_DecodeValidStatePacketV2();
	Test_DecodeRejectsBadMagic();
	Test_DecodeRejectsUnknownSchemaVersion();
	Test_DecodeRejectsShortBuffer();
	Test_DecodeRejectsShortV2Buffer();
	Test_InputPacketRoundTrip();
	Test_TransformIdentity();
	Test_TransformPosition();
	Test_TransformPureNoseUpPitch();
	Test_TransformPureYawTowardLeft();
	Test_TransformPureRollRight();

	if (gFailures == 0)
	{
		std::printf("\nALL TESTS PASSED\n");
		return 0;
	}
	std::printf("\n%d CHECK(S) FAILED\n", gFailures);
	return 1;
}
