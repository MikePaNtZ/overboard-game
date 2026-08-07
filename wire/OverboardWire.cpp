#include "OverboardWire.h"

#include <cstring>

namespace OverboardWire
{
	namespace
	{
		// Explicit little-endian byte-at-a-time read/write. This machine (Apple Silicon,
		// x86_64 hosts) is little-endian already, but writing it out explicitly rather than
		// relying on memcpy + host endianness means the wire layer stays correct if this code
		// ever runs somewhere that isn't, and it is self-documenting about what "little-endian"
		// means at each field.
		void PutU16(uint8_t* Buf, size_t Offset, uint16_t V)
		{
			Buf[Offset + 0] = static_cast<uint8_t>(V & 0xFF);
			Buf[Offset + 1] = static_cast<uint8_t>((V >> 8) & 0xFF);
		}

		void PutU32(uint8_t* Buf, size_t Offset, uint32_t V)
		{
			for (int i = 0; i < 4; ++i)
			{
				Buf[Offset + i] = static_cast<uint8_t>((V >> (8 * i)) & 0xFF);
			}
		}

		void PutU64(uint8_t* Buf, size_t Offset, uint64_t V)
		{
			for (int i = 0; i < 8; ++i)
			{
				Buf[Offset + i] = static_cast<uint8_t>((V >> (8 * i)) & 0xFF);
			}
		}

		void PutF32(uint8_t* Buf, size_t Offset, float V)
		{
			uint32_t Bits;
			std::memcpy(&Bits, &V, sizeof(Bits));
			PutU32(Buf, Offset, Bits);
		}

		void PutF64(uint8_t* Buf, size_t Offset, double V)
		{
			uint64_t Bits;
			std::memcpy(&Bits, &V, sizeof(Bits));
			PutU64(Buf, Offset, Bits);
		}

		uint16_t GetU16(const uint8_t* Buf, size_t Offset)
		{
			return static_cast<uint16_t>(Buf[Offset + 0]) | (static_cast<uint16_t>(Buf[Offset + 1]) << 8);
		}

		uint32_t GetU32(const uint8_t* Buf, size_t Offset)
		{
			uint32_t V = 0;
			for (int i = 0; i < 4; ++i)
			{
				V |= static_cast<uint32_t>(Buf[Offset + i]) << (8 * i);
			}
			return V;
		}

		uint64_t GetU64(const uint8_t* Buf, size_t Offset)
		{
			uint64_t V = 0;
			for (int i = 0; i < 8; ++i)
			{
				V |= static_cast<uint64_t>(Buf[Offset + i]) << (8 * i);
			}
			return V;
		}

		float GetF32(const uint8_t* Buf, size_t Offset)
		{
			uint32_t Bits = GetU32(Buf, Offset);
			float V;
			std::memcpy(&V, &Bits, sizeof(V));
			return V;
		}

		double GetF64(const uint8_t* Buf, size_t Offset)
		{
			uint64_t Bits = GetU64(Buf, Offset);
			double V;
			std::memcpy(&V, &Bits, sizeof(V));
			return V;
		}
	}

	bool DecodeBoardState(const uint8_t* Buffer, size_t Len, FBoardState& OutState, std::string& OutError)
	{
		OutState = FBoardState{};

		// Magic and schema_version both live in the fixed v1 header region (offsets 0 and 4),
		// unchanged by the v2 bump, so it's safe to read them before we know which version-sized
		// buffer we actually need.
		if (Buffer == nullptr || Len < kStatePacketWireSizeV1)
		{
			OutError = "OBW1 packet too short: got " + std::to_string(Len) + " bytes, need at least " +
				std::to_string(kStatePacketWireSizeV1);
			return false;
		}

		const uint32_t Magic = GetU32(Buffer, 0);
		if (Magic != kStateMagic)
		{
			char Hex[11];
			std::snprintf(Hex, sizeof(Hex), "0x%08X", Magic);
			OutError = std::string("OBW1 magic mismatch: got ") + Hex + ", expected 0x4F425731 (\"OBW1\"). Dropping packet.";
			return false;
		}

		const uint16_t SchemaVersion = GetU16(Buffer, 4);
		const size_t ExpectedSize = GetStatePacketWireSize(SchemaVersion);
		if (ExpectedSize == 0)
		{
			// Not 1, 2 or 3 -- this is the actual "fail loudly" gate. A v1/v2 packet is NOT
			// this branch; see the header comment on why they are accepted, not errors.
			OutError = "OBW1 schema_version " + std::to_string(SchemaVersion) + " is not 1, 2 or 3 -- this build only understands v1/v2/v3. Dropping packet.";
			return false;
		}
		if (Len < ExpectedSize)
		{
			OutError = "OBW1 v" + std::to_string(SchemaVersion) + " packet too short: got " + std::to_string(Len) +
				" bytes, need at least " + std::to_string(ExpectedSize);
			return false;
		}

		OutState.Magic = Magic;
		OutState.SchemaVersion = SchemaVersion;
		OutState.Flags = GetU16(Buffer, 6);
		OutState.Seq = GetU64(Buffer, 8);
		OutState.SimTimeS = GetF64(Buffer, 16);
		OutState.Pos[0] = GetF32(Buffer, 24);
		OutState.Pos[1] = GetF32(Buffer, 28);
		OutState.Pos[2] = GetF32(Buffer, 32);
		OutState.Quat[0] = GetF32(Buffer, 36); // w
		OutState.Quat[1] = GetF32(Buffer, 40); // x
		OutState.Quat[2] = GetF32(Buffer, 44); // y
		OutState.Quat[3] = GetF32(Buffer, 48); // z
		OutState.WheelAngleRad = GetF32(Buffer, 52);
		OutState.WheelRateRadS = GetF32(Buffer, 56);
		OutState.PitchRad = GetF32(Buffer, 60);
		OutState.YawRad = GetF32(Buffer, 64);
		OutState.MotorCurrentA = GetF32(Buffer, 68);

		if (SchemaVersion >= kStateSchemaVersionV2)
		{
			OutState.RiderForeAftM = GetF32(Buffer, 72);
			OutState.RiderLateralM = GetF32(Buffer, 76);
		}
		// else: v1 -- RiderForeAftM/RiderLateralM already default-initialised to 0 (neutral).

		if (SchemaVersion >= kStateSchemaVersionV3)
		{
			OutState.LinVel[0] = GetF32(Buffer, 80);
			OutState.LinVel[1] = GetF32(Buffer, 84);
			OutState.LinVel[2] = GetF32(Buffer, 88);
			OutState.AngVel[0] = GetF32(Buffer, 92);
			OutState.AngVel[1] = GetF32(Buffer, 96);
			OutState.AngVel[2] = GetF32(Buffer, 100);
		}
		// else: v1/v2 -- LinVel/AngVel stay zero. See the header: that is an ABSENCE, not a
		// value, and the host never sets PhysicsHandoff on a packet that lacks them.

		return true;
	}

	void EncodeBoardState(const FBoardState& State, uint8_t* Buffer)
	{
		PutU32(Buffer, 0, State.Magic);
		PutU16(Buffer, 4, State.SchemaVersion);
		PutU16(Buffer, 6, State.Flags);
		PutU64(Buffer, 8, State.Seq);
		PutF64(Buffer, 16, State.SimTimeS);
		PutF32(Buffer, 24, State.Pos[0]);
		PutF32(Buffer, 28, State.Pos[1]);
		PutF32(Buffer, 32, State.Pos[2]);
		PutF32(Buffer, 36, State.Quat[0]);
		PutF32(Buffer, 40, State.Quat[1]);
		PutF32(Buffer, 44, State.Quat[2]);
		PutF32(Buffer, 48, State.Quat[3]);
		PutF32(Buffer, 52, State.WheelAngleRad);
		PutF32(Buffer, 56, State.WheelRateRadS);
		PutF32(Buffer, 60, State.PitchRad);
		PutF32(Buffer, 64, State.YawRad);
		PutF32(Buffer, 68, State.MotorCurrentA);
		if (State.SchemaVersion >= kStateSchemaVersionV2)
		{
			PutF32(Buffer, 72, State.RiderForeAftM);
			PutF32(Buffer, 76, State.RiderLateralM);
		}
		if (State.SchemaVersion >= kStateSchemaVersionV3)
		{
			PutF32(Buffer, 80, State.LinVel[0]);
			PutF32(Buffer, 84, State.LinVel[1]);
			PutF32(Buffer, 88, State.LinVel[2]);
			PutF32(Buffer, 92, State.AngVel[0]);
			PutF32(Buffer, 96, State.AngVel[1]);
			PutF32(Buffer, 100, State.AngVel[2]);
		}
	}

	void EncodeInputPacket(const FInputPacket& Packet, uint8_t* Buffer)
	{
		PutU32(Buffer, 0, Packet.Magic);
		PutU16(Buffer, 4, Packet.SchemaVersion);
		PutU16(Buffer, 6, Packet.Flags);
		PutU64(Buffer, 8, Packet.Seq);
		PutF32(Buffer, 16, Packet.WeightShiftForeAft);
		PutF32(Buffer, 20, Packet.WeightShiftLateral);
		PutF32(Buffer, 24, Packet.Steer);
	}

	bool DecodeInputPacket(const uint8_t* Buffer, size_t Len, FInputPacket& OutPacket, std::string& OutError)
	{
		OutPacket = FInputPacket{};

		if (Buffer == nullptr || Len < kInputPacketWireSize)
		{
			OutError = "OBI1 packet too short: got " + std::to_string(Len) + " bytes, need at least " +
				std::to_string(kInputPacketWireSize);
			return false;
		}

		const uint32_t Magic = GetU32(Buffer, 0);
		if (Magic != kInputMagic)
		{
			char Hex[11];
			std::snprintf(Hex, sizeof(Hex), "0x%08X", Magic);
			OutError = std::string("OBI1 magic mismatch: got ") + Hex + ", expected 0x4F424931 (\"OBI1\"). Dropping packet.";
			return false;
		}

		const uint16_t SchemaVersion = GetU16(Buffer, 4);
		if (SchemaVersion != kInputSchemaVersion)
		{
			OutError = "OBI1 schema_version mismatch: got " + std::to_string(SchemaVersion) + ", expected " +
				std::to_string(kInputSchemaVersion) + ". Dropping packet.";
			return false;
		}

		OutPacket.Magic = Magic;
		OutPacket.SchemaVersion = SchemaVersion;
		OutPacket.Flags = GetU16(Buffer, 6);
		OutPacket.Seq = GetU64(Buffer, 8);
		OutPacket.WeightShiftForeAft = GetF32(Buffer, 16);
		OutPacket.WeightShiftLateral = GetF32(Buffer, 20);
		OutPacket.Steer = GetF32(Buffer, 24);
		return true;
	}
}
