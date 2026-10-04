#include "RiderAnimInstance.h"

#include "Animation/AnimNodeBase.h"
#include "BonePose.h"
#include "TwoBoneIK.h"

DEFINE_LOG_CATEGORY_STATIC(LogOverboardRiderAnim, Log, All);

FAnimInstanceProxy* URiderAnimInstance::CreateAnimInstanceProxy()
{
	return new FRiderAnimInstanceProxy(this);
}

void FRiderAnimInstanceProxy::PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds)
{
	FAnimSingleNodeInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);
	if (const URiderAnimInstance* Rider = Cast<URiderAnimInstance>(InAnimInstance))
	{
		Inputs = Rider->Inputs;
	}
}

namespace
{
	// Component-space working copy of a compact pose. Every edit is a rigid delta applied to a
	// bone AND everything below it, so children follow exactly as if the joint had moved.
	struct FCSWork
	{
		TArray<FTransform> CS;
		TArray<FTransform> RefCS;
		TArray<int32> Parent;
		TArray<uint8> InSub;

		void Build(const FCompactPose& Pose)
		{
			const int32 N = Pose.GetNumBones();
			CS.SetNum(N);
			RefCS.SetNum(N);
			Parent.SetNum(N);
			for (FCompactPoseBoneIndex I : Pose.ForEachBoneIndex())
			{
				const int32 i = I.GetInt();
				const int32 p = Pose.GetParentBoneIndex(I).GetInt();
				Parent[i] = p;
				CS[i] = p >= 0 ? Pose[I] * CS[p] : Pose[I];
				RefCS[i] = p >= 0 ? Pose.GetRefPose(I) * RefCS[p] : Pose.GetRefPose(I);
			}
		}

		void ApplyToSubtree(int32 Bone, const FTransform& Delta)
		{
			if (Bone < 0)
			{
				return;
			}
			const int32 N = CS.Num();
			InSub.SetNumZeroed(N);
			FMemory::Memzero(InSub.GetData(), N);
			InSub[Bone] = 1;
			CS[Bone] = CS[Bone] * Delta;
			for (int32 j = Bone + 1; j < N; ++j)
			{
				if (Parent[j] >= 0 && InSub[Parent[j]])
				{
					InSub[j] = 1;
					CS[j] = CS[j] * Delta;
				}
			}
		}

		void RotateAbout(int32 Bone, const FQuat& R, const FVector& Pivot)
		{
			const FTransform Delta = FTransform(-Pivot) * FTransform(R) * FTransform(Pivot);
			ApplyToSubtree(Bone, Delta);
		}

		void WriteBack(FCompactPose& Pose) const
		{
			for (FCompactPoseBoneIndex I : Pose.ForEachBoneIndex())
			{
				const int32 i = I.GetInt();
				FTransform Local = Parent[i] >= 0 ? CS[i].GetRelativeTransform(CS[Parent[i]]) : CS[i];
				Local.NormalizeRotation();
				Pose[I] = Local;
			}
		}

		FVector Loc(int32 b) const { return CS[b].GetLocation(); }
	};

	FQuat RotTowards(const FVector& From, const FVector& To, float AngleDeg)
	{
		const FVector Axis = FVector::CrossProduct(From, To).GetSafeNormal();
		if (Axis.IsNearlyZero() || FMath::IsNearlyZero(AngleDeg))
		{
			return FQuat::Identity;
		}
		return FQuat(Axis, FMath::DegreesToRadians(AngleDeg));
	}

	FVector Horizontal(const FVector& V)
	{
		return FVector(V.X, V.Y, 0.f).GetSafeNormal();
	}
}

bool FRiderAnimInstanceProxy::Evaluate(FPoseContext& Output)
{
	FAnimSingleNodeInstanceProxy::Evaluate(Output);
	if (!Inputs.bEnabled)
	{
		return true;
	}

	FCompactPose& Pose = Output.Pose;
	const FBoneContainer& BC = Pose.GetBoneContainer();
	auto Find = [&BC](const TCHAR* Name) -> int32
	{
		const int32 MeshIndex = BC.GetPoseBoneIndexForBoneName(FName(Name));
		if (MeshIndex == INDEX_NONE)
		{
			return -1;
		}
		return BC.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshIndex)).GetInt();
	};

	const int32 Pelvis = Find(TEXT("pelvis"));
	const int32 Spine03 = Find(TEXT("spine_03"));
	const int32 Spine04 = Find(TEXT("spine_04"));
	const int32 Spine05 = Find(TEXT("spine_05"));
	const int32 Neck = Find(TEXT("neck_01"));
	const int32 Head = Find(TEXT("head"));
	const int32 Thigh[2] = {Find(TEXT("thigh_l")), Find(TEXT("thigh_r"))};
	const int32 Calf[2] = {Find(TEXT("calf_l")), Find(TEXT("calf_r"))};
	const int32 Foot[2] = {Find(TEXT("foot_l")), Find(TEXT("foot_r"))};
	const int32 UpperArm[2] = {Find(TEXT("upperarm_l")), Find(TEXT("upperarm_r"))};
	const int32 Hand[2] = {Find(TEXT("hand_l")), Find(TEXT("hand_r"))};

	const bool bHaveCore = Pelvis >= 0 && Spine03 >= 0 && Spine04 >= 0 && Spine05 >= 0 && Neck >= 0 && Head >= 0
		&& Thigh[0] >= 0 && Thigh[1] >= 0 && Calf[0] >= 0 && Calf[1] >= 0 && Foot[0] >= 0 && Foot[1] >= 0
		&& UpperArm[0] >= 0 && UpperArm[1] >= 0 && Hand[0] >= 0 && Hand[1] >= 0;
	if (!bHaveCore)
	{
		if (!bLoggedMissingBones)
		{
			bLoggedMissingBones = true;
			UE_LOG(LogOverboardRiderAnim, Warning, TEXT("RiderAnimInstance: a core bone is missing (UE5/MetaHuman names expected); procedural layer off."));
		}
		return true;
	}

	FCSWork W;
	W.Build(Pose);
	const FVector Up(0, 0, 1);
	const FVector Travel = Horizontal(Inputs.TravelDirCS);
	const float T = Inputs.TimeS;
	const float TurnAbs = FMath::Abs(Inputs.TurnSigned);

	// 1. Where the blendspace put the feet. They stay there: the deck does not move under them.
	const FTransform FootTarget[2] = {W.CS[Foot[0]], W.CS[Foot[1]]};
	const FVector FeetMid = 0.5f * (FootTarget[0].GetLocation() + FootTarget[1].GetLocation());

	// 2. Crouch: drop the hips. Leg IK (last step) turns this into knee and hip flex.
	W.ApplyToSubtree(Pelvis, FTransform(FVector(0, 0, -Inputs.CrouchCm)));

	// 3. Whole-body lean over the planted feet.
	const float LeanDeg = Inputs.LeanVecCS.Size();
	if (LeanDeg > 0.01f)
	{
		const FVector LeanDir = Horizontal(Inputs.LeanVecCS);
		W.RotateAbout(Pelvis, RotTowards(Up, LeanDir, LeanDeg), FeetMid);
		// The upper body carries a little more of it than the hips.
		W.RotateAbout(Spine03, RotTowards(Up, LeanDir, 0.35f * LeanDeg), W.Loc(Spine03));
	}

	// 4. Chest facing, from the reference pose: the mesh faces +Y in its own component space.
	const FVector ChestAxisLocal = W.RefCS[Spine05].InverseTransformVectorNoScale(FVector(0, 1, 0));
	const FVector ChestFacing = Horizontal(W.CS[Spine05].TransformVectorNoScale(ChestAxisLocal));

	// 5. Breathing: a slow pitch of the chest, about 15 breaths a minute.
	const FVector ChestSide = FVector::CrossProduct(Up, ChestFacing).GetSafeNormal();
	const float BreathDeg = 1.1f * FMath::Sin(2.f * PI * 0.25f * T);
	W.RotateAbout(Spine04, FQuat(ChestSide, FMath::DegreesToRadians(BreathDeg)), W.Loc(Spine04));

	// 6. Upper body leads the carve: a twist about the vertical, towards the turn.
	W.RotateAbout(Spine04, FQuat(Up, FMath::DegreesToRadians(9.f * Inputs.TurnSigned)), W.Loc(Spine04));

	// 7. Arms: out from the body for balance, more in a hard carve, with a slow flutter.
	for (int32 s = 0; s < 2; ++s)
	{
		const FVector Shoulder = W.Loc(UpperArm[s]);
		const FVector ArmDir = (W.Loc(Hand[s]) - Shoulder).GetSafeNormal();
		const FVector Out = Horizontal(Shoulder - W.Loc(Spine05));
		const float Flutter = 2.5f * FMath::Sin(2.f * PI * (0.55f + 0.13f * s) * T + 1.7f * s);
		const float Abduct = 10.f + 16.f * TurnAbs + Flutter;
		W.RotateAbout(UpperArm[s], RotTowards(ArmDir, Out, Abduct), Shoulder);
		// Swing slightly towards the direction of travel with the turn.
		W.RotateAbout(UpperArm[s], RotTowards(ArmDir, Travel, 6.f * Inputs.TurnSigned * (s == 0 ? 1.f : -1.f)), Shoulder);
	}

	// 8. Head looks down the road: yaw towards the travel direction, a little pitch down.
	{
		const FVector HeadAxisLocal = W.RefCS[Head].InverseTransformVectorNoScale(FVector(0, 1, 0));
		const FVector HeadFacing = Horizontal(W.CS[Head].TransformVectorNoScale(HeadAxisLocal));
		const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(FVector::CrossProduct(HeadFacing, Travel).Z, FVector::DotProduct(HeadFacing, Travel)));
		const float YawApplied = FMath::Clamp(Yaw, -80.f, 80.f) * Inputs.HeadLookWeight;
		W.RotateAbout(Neck, FQuat(Up, FMath::DegreesToRadians(0.4f * YawApplied)), W.Loc(Neck));
		W.RotateAbout(Head, FQuat(Up, FMath::DegreesToRadians(0.6f * YawApplied)), W.Loc(Head));
		const FVector NewFacing = Horizontal(W.CS[Head].TransformVectorNoScale(HeadAxisLocal));
		W.RotateAbout(Head, RotTowards(NewFacing, -Up, 8.f), W.Loc(Head));
	}

	// 9. Two-bone leg IK back to the planted feet.
	for (int32 s = 0; s < 2; ++s)
	{
		const FVector Root = W.Loc(Thigh[s]);
		const FVector Joint = W.Loc(Calf[s]);
		const FVector End = W.Loc(Foot[s]);
		FVector KneeOut = (Joint - 0.5f * (Root + End)).GetSafeNormal();
		if (KneeOut.IsNearlyZero())
		{
			KneeOut = Travel;
		}
		const FVector Pole = Joint + 60.f * KneeOut;
		FVector OutJoint, OutEnd;
		AnimationCore::SolveTwoBoneIK(Root, Joint, End, Pole, FootTarget[s].GetLocation(), OutJoint, OutEnd, false, 1.0, 1.1);
		W.RotateAbout(Thigh[s], FQuat::FindBetweenNormals((Joint - Root).GetSafeNormal(), (OutJoint - Root).GetSafeNormal()), Root);
		const FVector Joint2 = W.Loc(Calf[s]);
		const FVector End2 = W.Loc(Foot[s]);
		W.RotateAbout(Calf[s], FQuat::FindBetweenNormals((End2 - Joint2).GetSafeNormal(), (OutEnd - Joint2).GetSafeNormal()), Joint2);
		const FTransform FootDelta = W.CS[Foot[s]].Inverse() * FootTarget[s];
		W.ApplyToSubtree(Foot[s], FootDelta);
	}

	W.WriteBack(Pose);
	return true;
}
