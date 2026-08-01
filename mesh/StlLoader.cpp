#include "StlLoader.h"

#include <cstdio>
#include <cstring>

namespace OverboardMesh
{
	namespace
	{
		uint32_t GetU32LE(const uint8_t* Buf)
		{
			return static_cast<uint32_t>(Buf[0]) | (static_cast<uint32_t>(Buf[1]) << 8) |
				(static_cast<uint32_t>(Buf[2]) << 16) | (static_cast<uint32_t>(Buf[3]) << 24);
		}

		float GetF32LE(const uint8_t* Buf)
		{
			uint32_t Bits = GetU32LE(Buf);
			float V;
			std::memcpy(&V, &Bits, sizeof(V));
			return V;
		}

		FVec3 GetVec3LE(const uint8_t* Buf)
		{
			return FVec3{ GetF32LE(Buf), GetF32LE(Buf + 4), GetF32LE(Buf + 8) };
		}

		void GrowBounds(FVec3& Min, FVec3& Max, const FVec3& P)
		{
			if (P.X < Min.X) Min.X = P.X;
			if (P.Y < Min.Y) Min.Y = P.Y;
			if (P.Z < Min.Z) Min.Z = P.Z;
			if (P.X > Max.X) Max.X = P.X;
			if (P.Y > Max.Y) Max.Y = P.Y;
			if (P.Z > Max.Z) Max.Z = P.Z;
		}
	}

	bool LoadBinaryStl(const std::string& Path, FStlMesh& OutMesh, std::string& OutError)
	{
		OutMesh = FStlMesh{};

		std::FILE* File = std::fopen(Path.c_str(), "rb");
		if (!File)
		{
			OutError = "cannot open '" + Path + "'";
			return false;
		}

		std::fseek(File, 0, SEEK_END);
		const long FileSizeLong = std::ftell(File);
		std::fseek(File, 0, SEEK_SET);
		if (FileSizeLong < 84)
		{
			std::fclose(File);
			OutError = "'" + Path + "' is too short to be a binary STL (need at least 84 bytes, header + count)";
			return false;
		}
		const size_t FileSize = static_cast<size_t>(FileSizeLong);

		std::vector<uint8_t> Buf(FileSize);
		const size_t ReadBytes = std::fread(Buf.data(), 1, FileSize, File);
		std::fclose(File);
		if (ReadBytes != FileSize)
		{
			OutError = "short read on '" + Path + "'";
			return false;
		}

		// Bytes [0,80) are the header, ignored. Triangle count at offset 80.
		const uint32_t TriCount = GetU32LE(Buf.data() + 80);
		const size_t ExpectedSize = 84 + static_cast<size_t>(TriCount) * 50;
		if (ExpectedSize != FileSize)
		{
			// This is also what rejects an ASCII STL (its first 84 bytes, read as binary,
			// produce a triangle count that essentially never matches the actual file size) --
			// deliberately not a separate ASCII/binary heuristic, just one size check that
			// covers both "truncated binary" and "not binary at all".
			OutError = "'" + Path + "' does not parse as a valid binary STL: header claims " +
				std::to_string(TriCount) + " triangles (expected file size " + std::to_string(ExpectedSize) +
				" bytes), actual file size " + std::to_string(FileSize) + " bytes";
			return false;
		}

		OutMesh.Triangles.reserve(TriCount);
		size_t Offset = 84;
		for (uint32_t i = 0; i < TriCount; ++i)
		{
			FStlTriangle Tri;
			Tri.Normal = GetVec3LE(Buf.data() + Offset);
			Tri.V0 = GetVec3LE(Buf.data() + Offset + 12);
			Tri.V1 = GetVec3LE(Buf.data() + Offset + 24);
			Tri.V2 = GetVec3LE(Buf.data() + Offset + 36);
			// bytes [Offset+48, Offset+50) are the attribute byte count -- ignored, see header comment.
			Offset += 50;

			GrowBounds(OutMesh.BoundsMin, OutMesh.BoundsMax, Tri.V0);
			GrowBounds(OutMesh.BoundsMin, OutMesh.BoundsMax, Tri.V1);
			GrowBounds(OutMesh.BoundsMin, OutMesh.BoundsMax, Tri.V2);

			OutMesh.Triangles.push_back(Tri);
		}

		return true;
	}
}
