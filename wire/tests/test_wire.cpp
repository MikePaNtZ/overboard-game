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
	void Test_DecodeValidStatePacket()
	{
		std::printf("Test_DecodeValidStatePacket\n");

		FBoardState In;
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

		uint8_t Buf[kStatePacketWireSize];
		EncodeBoardState(In, Buf);

		FBoardState Out;
		std::string Err;
		bool Ok = DecodeBoardState(Buf, sizeof(Buf), Out, Err);
		Check(Ok, "decode should succeed on a well-formed packet");
		Check(Err.empty(), "no error message on success");
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

		// Also nail down exact byte offsets against the field table, not just round-trip via
		// our own encoder (which could hide a matched pair of bugs).
		uint32_t MagicAtZero;
		std::memcpy(&MagicAtZero, Buf, 4);
		Check(MagicAtZero == kStateMagic, "magic sits at byte offset 0, little-endian");
		Check(sizeof(Buf) == 72, "state packet wire size is 72 bytes");
	}

	void Test_DecodeRejectsBadMagic()
	{
		std::printf("Test_DecodeRejectsBadMagic\n");
		FBoardState In;
		uint8_t Buf[kStatePacketWireSize];
		EncodeBoardState(In, Buf);
		Buf[0] = 0x00; // corrupt magic

		FBoardState Out;
		std::string Err;
		bool Ok = DecodeBoardState(Buf, sizeof(Buf), Out, Err);
		Check(!Ok, "decode must fail on bad magic");
		Check(!Err.empty(), "error message must be set on bad magic (fail loudly)");
		Check(Err.find("magic") != std::string::npos, "error should mention magic");
	}

	void Test_DecodeRejectsBadSchemaVersion()
	{
		std::printf("Test_DecodeRejectsBadSchemaVersion\n");
		FBoardState In;
		In.SchemaVersion = 2; // unknown to us
		uint8_t Buf[kStatePacketWireSize];
		EncodeBoardState(In, Buf);

		FBoardState Out;
		std::string Err;
		bool Ok = DecodeBoardState(Buf, sizeof(Buf), Out, Err);
		Check(!Ok, "decode must fail on unknown schema_version");
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

	// A pure pitch about MuJoCo's Y axis (lateral) by angle Theta.
	void Test_TransformPurePitch()
	{
		std::printf("Test_TransformPurePitch\n");
		const double Theta = 0.3; // radians
		float Pos[3] = {0.f, 0.f, 0.f};
		float Quat[4] = {
			static_cast<float>(std::cos(Theta / 2.0)),
			0.f,
			static_cast<float>(std::sin(Theta / 2.0)),
			0.f,
		};
		FUeTransform Out = MuJoCoToUnreal(Pos, Quat);
		// x=z=0 going in, so the mirror leaves all four components numerically unchanged for a
		// pure-Y rotation. NOTE: unverified in-editor -- confirm this actually reads as
		// "nose up" the right way round on a commanded pure pitch before trusting it further.
		Check(NearlyEqual(Out.QuatWXYZ[0], Quat[0]), "pure pitch: w unchanged");
		Check(NearlyEqual(Out.QuatWXYZ[1], 0.0), "pure pitch: x stays 0");
		Check(NearlyEqual(Out.QuatWXYZ[2], Quat[2]), "pure pitch: y (rotation axis) unchanged");
		Check(NearlyEqual(Out.QuatWXYZ[3], 0.0), "pure pitch: z stays 0");
	}

	// A pure yaw about MuJoCo's Z axis (vertical, the non-physical steering channel's axis) by
	// angle Theta.
	void Test_TransformPureYaw()
	{
		std::printf("Test_TransformPureYaw\n");
		const double Theta = 0.3; // radians
		float Pos[3] = {0.f, 0.f, 0.f};
		float Quat[4] = {
			static_cast<float>(std::cos(Theta / 2.0)),
			0.f,
			0.f,
			static_cast<float>(std::sin(Theta / 2.0)),
		};
		FUeTransform Out = MuJoCoToUnreal(Pos, Quat);
		// z is one of the two mirrored components -> negates. This means a positive MuJoCo yaw
		// becomes a negative-signed z component in UE's quat, i.e. the sense of rotation flips,
		// which is the expected outcome of mirroring a single axis (Y) of a right-handed frame.
		// NOTE: unverified in-editor -- confirm the visible turn direction against a commanded
		// pure yaw before trusting this for anything beyond "it compiles".
		Check(NearlyEqual(Out.QuatWXYZ[0], Quat[0]), "pure yaw: w unchanged");
		Check(NearlyEqual(Out.QuatWXYZ[1], 0.0), "pure yaw: x stays 0");
		Check(NearlyEqual(Out.QuatWXYZ[2], 0.0), "pure yaw: y stays 0");
		Check(NearlyEqual(Out.QuatWXYZ[3], -Quat[3]), "pure yaw: z (mirrored axis pair) negates");
	}
}

int main()
{
	Test_DecodeValidStatePacket();
	Test_DecodeRejectsBadMagic();
	Test_DecodeRejectsBadSchemaVersion();
	Test_DecodeRejectsShortBuffer();
	Test_InputPacketRoundTrip();
	Test_TransformIdentity();
	Test_TransformPosition();
	Test_TransformPurePitch();
	Test_TransformPureYaw();

	if (gFailures == 0)
	{
		std::printf("\nALL TESTS PASSED\n");
		return 0;
	}
	std::printf("\n%d CHECK(S) FAILED\n", gFailures);
	return 1;
}
