// GENERATED FILE -- do not edit by hand.
//
// Written by `terraincheck --emit-header` from terrain/levels/*.terrain, which are the
// source of truth. CI regenerates this and fails on any diff, so it cannot drift from the
// declarations the way a hand-kept second copy would.
//
// A level is verified only if EVERY declared surface is `status verified`. A level with no
// declaration, or one that will not parse, is unverified -- a level nobody declared is not
// a level that passed.
//
// Consumed by AOverboardHUD to tag the screen on an unverified level (ADR-0011 condition 2,
// terrain/README.md "the hole in the gate"). Deliberately plain C++ with no UE types, so
// the generator stays engine-free.
#pragma once

namespace OverboardTerrainVerification
{
	struct FLevelVerification
	{
		const char* LevelName;
		bool bVerified;
	};

	inline constexpr FLevelVerification kLevels[] = {
		{ "OB_City", false },
		{ "OB_Main", true },
	};

	/// Unknown level names return false. An unrecognised level has certainly not been
	/// measured against the envelope, so "unverified" is the only safe default.
	inline bool IsLevelVerified(const char* Name)
	{
		for (const FLevelVerification& L : kLevels)
		{
			const char* A = L.LevelName;
			const char* B = Name;
			while (*A && *A == *B) { ++A; ++B; }
			if (*A == 0 && *B == 0) { return L.bVerified; }
		}
		return false;
	}
}
