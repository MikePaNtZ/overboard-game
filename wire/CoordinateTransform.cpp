#include "CoordinateTransform.h"

namespace
{
	constexpr float kRadToDeg = 57.29577951308232f;
}

namespace OverboardWire
{
	FUeTransform MuJoCoToUnreal(const float PosM[3], const float QuatWXYZ[4])
	{
		FUeTransform Out;

		// Position: metres -> centimetres, mirror Y for the right-handed -> left-handed flip.
		Out.PosCm[0] = PosM[0] * 100.f;
		Out.PosCm[1] = -PosM[1] * 100.f;
		Out.PosCm[2] = PosM[2] * 100.f;

		// Quaternion: (w, x, y, z)_mujoco -> (w, -x, y, -z)_unreal. See header comment.
		Out.QuatWXYZ[0] = QuatWXYZ[0];
		Out.QuatWXYZ[1] = -QuatWXYZ[1];
		Out.QuatWXYZ[2] = QuatWXYZ[2];
		Out.QuatWXYZ[3] = -QuatWXYZ[3];

		return Out;
	}

	FUeVelocity MuJoCoVelocityToUnreal(const float LinVelMS[3], const float AngVelRadS[3])
	{
		FUeVelocity Out;

		// Polar vector: mirror Y, m/s -> cm/s. Identical to the position rule above.
		Out.LinCmS[0] = LinVelMS[0] * 100.f;
		Out.LinCmS[1] = -LinVelMS[1] * 100.f;
		Out.LinCmS[2] = LinVelMS[2] * 100.f;

		// Axial vector: the mirrored axis keeps its sign and the other two flip -- the
		// complement of the polar rule, and the same pattern as the quaternion's (x, y, z)
		// above. rad/s -> deg/s because UE's physics API is in degrees.
		Out.AngDegS[0] = -AngVelRadS[0] * kRadToDeg;
		Out.AngDegS[1] = AngVelRadS[1] * kRadToDeg;
		Out.AngDegS[2] = -AngVelRadS[2] * kRadToDeg;

		return Out;
	}
}
