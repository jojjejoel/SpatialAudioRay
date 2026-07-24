#include "Audio/SpatialAudioComponent.h"
#include "Engine/World.h"
#include "Audio/Math.h"
#include "Audio/SpatialAudioSettings.h"

using namespace Math;

namespace {
	bool ShouldCrawlThisHit(const USpatialAudioSettings& S, bool bNextHitCrawls, int32 Bounce) {
		return S.bEnableSurfaceCrawl && bNextHitCrawls && (!S.bCrawlOnFirstBounceOnly || Bounce == 0);
	}

	FVector ComputeCrawlDirection(const FVector& IncomingDir, const FVector& SurfaceNormal,
	                              const FVector& HitPoint, const FVector& ListenerPos,
	                              float ListenerBias) {
		const float DotDN = FVector::DotProduct(IncomingDir, SurfaceNormal);
		const FVector Slide = (IncomingDir - DotDN * SurfaceNormal).GetSafeNormal();

		const FVector ToListenerRaw = ListenerPos - HitPoint;
		const float DotLN = FVector::DotProduct(ToListenerRaw, SurfaceNormal);
		const FVector ToListenerPlaneRaw = ToListenerRaw - DotLN * SurfaceNormal;
		const FVector ListenerBiasDir = ToListenerPlaneRaw.IsNearlyZero()
			                                ? Slide
			                                : ToListenerPlaneRaw.GetSafeNormal();

		return FMath::Lerp(Slide, ListenerBiasDir, ListenerBias).GetSafeNormal();
	}

}

bool USpatialAudioComponent::StepSampleSegmentForLoS(
	const FVector& SegStart, const FVector& SegDir, float SegLen,
	float CumDistBase, const FVector& ListenerPos, float MaxBudget,
	const USpatialAudioSettings& S, const UWorld* World,
	FVector& OutLoSPoint, float& OutLoSCumDist) const {
	const float Step = S.DiffractionEdgeSampleStep;
	const int32 MaxSamples = S.MaxSamplesPerSegment;
	int32 SampleCount = 0;

	for (float T = Step; T < SegLen; T += Step) {
		if (MaxSamples > 0 && SampleCount >= MaxSamples) {
			break;
		}
		++SampleCount;

		const FVector SamplePt = SegStart + SegDir * T;
		const float SampCumDist = CumDistBase + T;
		if (SampCumDist + FVector::Dist(SamplePt, ListenerPos) > MaxBudget) {
			break;
		}

		FHitResult LoSHit;
		if (!TraceLine(World, LoSHit, SamplePt, ListenerPos)
			&& !TraceLine(World, LoSHit, ListenerPos, SamplePt)) {
			OutLoSPoint = SamplePt;
			OutLoSCumDist = SampCumDist;
			return true;
		}
	}
	
	{
		const float SafeT = FMath::Max(0.f, SegLen - 2.f);
		const FVector SamplePt = SegStart + SegDir * SafeT;
		const float SampCumDist = CumDistBase + SafeT;
		if (SampCumDist + FVector::Dist(SamplePt, ListenerPos) <= MaxBudget) {
			FHitResult LoSHit;
			if (!TraceLine(World, LoSHit, SamplePt, ListenerPos)
				&& !TraceLine(World, LoSHit, ListenerPos, SamplePt)) {
				OutLoSPoint = SamplePt;
				OutLoSCumDist = SampCumDist;
				return true;
			}
		}
	}
	return false;
}

void USpatialAudioComponent::ProcessRayHit(
	const FHitResult& Hit,
	FVector& InOutOrigin, FVector& InOutDir,
	float& InOutCumDist, int32& InOutBounce, bool& InOutNextHitCrawls,
	bool bLoSAlreadyFound, bool bBias,
	const FVector& ListenerPos,
	const USpatialAudioSettings& Settings, UWorld* World,
	FRayHitOutput& Out) const {
	Out = FRayHitOutput{};

	if (ShouldCrawlThisHit(Settings, InOutNextHitCrawls, InOutBounce)
		&& FVector::DotProduct(Hit.Normal, InOutDir) <= 0.f) {
		FVector EdgePoint, EdgeDir;
		FCrawlOutput CrawlOut;

		if (CrawlSurfaceToEdge(Hit.Location, InOutDir, Hit.Normal, ListenerPos, World,
		                       EdgePoint, EdgeDir, Out.CrawlDist, Settings,
		                       InOutCumDist, &CrawlOut)) {
			Out.bCrawlSucceeded = true;
			Out.CrawlSteps = MoveTemp(CrawlOut.Steps);
			Out.CrawlProbeEnds = MoveTemp(CrawlOut.ProbeEnds);

			if (!bLoSAlreadyFound && CrawlOut.bLoSFound) {
				Out.bLoSFound = true;
				Out.LoSPoint = CrawlOut.LoSPoint;
				Out.LoSCumDist = CrawlOut.LoSCumDist;
			}

			InOutCumDist += Out.CrawlDist;
			InOutOrigin = EdgePoint;

			if (CrawlOut.bPerpWallHit) {
				Out.bPerpWall = true;
				Out.PerpWallEdgePoint = EdgePoint;
				++InOutBounce;
				InOutDir = ComputeBouncedDirection(EdgeDir, CrawlOut.PerpHit.Normal, /*bApplyBias=*/false,
				                                   EdgePoint, ListenerPos, Settings.SurfaceRoughness,
				                                   Settings.BounceListenerBias);
				InOutOrigin += CrawlOut.PerpHit.Normal * Settings.RaySurfaceBias;
			}
			else {
				InOutDir = EdgeDir;
				++InOutBounce;
			}
		}
	}

	if (!Out.bCrawlSucceeded) {
		InOutDir = ComputeBouncedDirection(InOutDir, Hit.Normal, !bLoSAlreadyFound && bBias,
		                                   Hit.Location, ListenerPos, Settings.SurfaceRoughness,
		                                   Settings.BounceListenerBias);
		InOutOrigin = Hit.Location + Hit.Normal * Settings.RaySurfaceBias;
		++InOutBounce;
	}

	if (Settings.bEnableSurfaceCrawl) {
		InOutNextHitCrawls = !InOutNextHitCrawls;
	}
}

bool USpatialAudioComponent::CrawlSurfaceToEdge(
	const FVector& HitPoint,
	const FVector& IncomingDir,
	const FVector& SurfaceNormal,
	const FVector& ListenerPos,
	UWorld* World,
	FVector& OutEdgePoint,
	FVector& OutCrawlDir,
	float& OutCrawlDist,
	const USpatialAudioSettings& S,
	float InCumDist,
	FCrawlOutput* Out) const {
	const FVector CrawlDir = ComputeCrawlDirection(IncomingDir, SurfaceNormal, HitPoint, ListenerPos,
	                                               S.CrawlListenerBias);
	if (CrawlDir.IsNearlyZero()) {
		return false;
	}

	const float NudgeDist = S.RaySurfaceBias;
	const float BackProbeLen = NudgeDist + 5.f;
	const FVector NudgedStart = HitPoint + SurfaceNormal * NudgeDist;
	const FVector BackDir = -SurfaceNormal;

	if (Out) {
		Out->Steps.Reset();
		Out->ProbeEnds.Reset();
	}

	int32 CrawlStepCap = S.MaxCrawlSteps;
	if (S.MaxStraightFlightDistance > 0.f) {
		CrawlStepCap = FMath::Clamp(FMath::FloorToInt(
			S.MaxStraightFlightDistance / FMath::Max(S.CrawlStepSize, 1.f)),
			1, S.MaxCrawlSteps);
	}
	int32 EffMaxSteps;
	float MaxCrawlRange;
	ComputeCrawlStepBudget(NudgedStart, CrawlDir, World, S, CrawlStepCap, EffMaxSteps, MaxCrawlRange);

	bool bLoSFoundDuringCrawl = false;

	for (int32 Step = 1; Step <= EffMaxSteps; ++Step) {
		const FVector StepPos = NudgedStart + Step * S.CrawlStepSize * CrawlDir;
		const FVector ProbeEnd = StepPos + BackDir * BackProbeLen;

		if (Out) {
			Out->Steps.Add(StepPos);
			Out->ProbeEnds.Add(ProbeEnd);
		}

		if (Out && !bLoSFoundDuringCrawl
			&& FVector::DotProduct(ListenerPos - StepPos, SurfaceNormal) > 0.f) {
			FHitResult LoSHit;
			if (!TraceLine(World, LoSHit, StepPos, ListenerPos)
				&& !TraceLine(World, LoSHit, ListenerPos, StepPos)) {
				Out->LoSPoint = StepPos;
				Out->LoSCumDist = InCumDist + Step * S.CrawlStepSize;
				Out->bLoSFound = true;
				bLoSFoundDuringCrawl = true;
			}
		}

		if (Out) {
			const FVector FwdEnd = StepPos + CrawlDir * S.CrawlStepSize;
			FHitResult FwdHit;
			if (TraceLine(World, FwdHit, StepPos, FwdEnd)) {
				Out->PerpHit = FwdHit;
				Out->bPerpWallHit = true;
				OutEdgePoint = FwdHit.Location + FwdHit.Normal * NudgeDist;
				OutCrawlDir = ReflectDirection(CrawlDir, FwdHit.Normal);
				OutCrawlDist = (Step - 1) * S.CrawlStepSize + FVector::Dist(StepPos, FwdHit.Location);
				return true;
			}
		}

		FHitResult BackHit;
		if (!TraceLine(World, BackHit, StepPos, ProbeEnd)) {
			OutEdgePoint = StepPos;
			{
				const FVector ToListener = (ListenerPos - StepPos).GetSafeNormal();
				OutCrawlDir = FVector::DotProduct(SurfaceNormal, ToListener) >= 0.f
					? SurfaceNormal : -SurfaceNormal;
			}
			OutCrawlDist = Step * S.CrawlStepSize;
			return true;
		}
	}

	if (Out && !bLoSFoundDuringCrawl) {
		TryFindLoSBeyondCrawlSteps(NudgedStart, CrawlDir, SurfaceNormal, ListenerPos, World, S,
		                          EffMaxSteps + 1, MaxCrawlRange, InCumDist, *Out);
	}

	if (Out) {
		Out->Steps.Reset();
		Out->ProbeEnds.Reset();
	}
	return false;
}

void USpatialAudioComponent::ComputeCrawlStepBudget(const FVector& NudgedStart, const FVector& CrawlDir, UWorld* World,
                                                     const USpatialAudioSettings& S, const int32 CrawlStepCap,
                                                     int32& OutEffMaxSteps, float& OutMaxCrawlRange) const {
	OutEffMaxSteps = CrawlStepCap;
	OutMaxCrawlRange = CrawlStepCap * S.CrawlStepSize;
	FHitResult FwdHit;
	if (TraceLine(World, FwdHit, NudgedStart, NudgedStart + CrawlDir * OutMaxCrawlRange)) {
		const float HitDist = FVector::Dist(NudgedStart, FwdHit.Location);
		OutMaxCrawlRange = HitDist;
		OutEffMaxSteps = FMath::Min(FMath::FloorToInt(HitDist / S.CrawlStepSize), CrawlStepCap);
	}
}

bool USpatialAudioComponent::TryFindLoSBeyondCrawlSteps(const FVector& NudgedStart, const FVector& CrawlDir,
                                                         const FVector& SurfaceNormal, const FVector& ListenerPos,
                                                         UWorld* World, const USpatialAudioSettings& S,
                                                         const int32 FromStep, const float MaxCrawlRange,
                                                         const float InCumDist, FCrawlOutput& Out) const {
	for (int32 Step = FromStep; Step <= S.MaxCrawlSteps; ++Step) {
		const float StepDist = Step * S.CrawlStepSize;
		if (StepDist > MaxCrawlRange) {
			break;
		}
		const FVector StepPos = NudgedStart + StepDist * CrawlDir;
		if (FVector::DotProduct(ListenerPos - StepPos, SurfaceNormal) <= 0.f) {
			continue;
		}

		FHitResult LoSHit;
		if (!TraceLine(World, LoSHit, StepPos, ListenerPos)
			&& !TraceLine(World, LoSHit, ListenerPos, StepPos)) {
			Out.LoSPoint = StepPos;
			Out.LoSCumDist = InCumDist + Step * S.CrawlStepSize;
			Out.bLoSFound = true;
			return true;
		}
	}
	return false;
}

