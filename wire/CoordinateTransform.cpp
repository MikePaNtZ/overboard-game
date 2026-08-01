#include "CoordinateTransform.h"

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
}
