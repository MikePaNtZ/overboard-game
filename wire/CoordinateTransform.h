// CoordinateTransform.h
//
// The ONE place the MuJoCo -> Unreal frame conversion happens. Do not scatter this logic --
// every consumer of a wire FBoardState pose must go through MuJoCoToUnreal below.
#pragma once

namespace OverboardWire
{
	// A pose already converted into Unreal's convention: centimetres, Z-up, left-handed,
	// quaternion in (w, x, y, z) order (UE's FQuat component order).
	struct FUeTransform
	{
		float PosCm[3] = {0.f, 0.f, 0.f};
		float QuatWXYZ[4] = {1.f, 0.f, 0.f, 0.f};
	};

	// Converts a raw MuJoCo pose (metres, Z-up, RIGHT-handed; quaternion w,x,y,z) into Unreal's
	// convention (centimetres, Z-up, LEFT-handed).
	//
	// Handedness note: MuJoCo and Unreal agree on "Z-up", so the only axis that flips sign is Y
	// (mirroring Y turns a right-handed basis into a left-handed one while keeping X and Z as
	// they are). That is why position only negates Y, and why the quaternion only negates the
	// components on the two axes that are NOT the mirrored one (x and z) while leaving w and the
	// mirrored axis's own component (y) unchanged -- that is the standard rule for reflecting a
	// rotation quaternion through a single mirrored axis.
	//
	// This is the given v1 baseline, not yet visually confirmed in the editor (no GUI available
	// in this pass -- see wire/README.md). Sanity-check in-editor with a commanded pure pitch and
	// a commanded pure yaw before trusting this for anything beyond "it compiles": a handedness
	// bug looks exactly like correct motion mirrored, which is easy to miss on a symmetric box.
	FUeTransform MuJoCoToUnreal(const float PosM[3], const float QuatWXYZ[4]);
}
