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
	const FVector& ListenerPos, float MaxBudget,
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
				const FVector Reflected = ReflectDirection(EdgeDir, CrawlOut.PerpHit.Normal);
				FVector RandomHemi = FMath::VRand();
				if (FVector::DotProduct(RandomHemi, CrawlOut.PerpHit.Normal) < 0.f) {
					RandomHemi = -RandomHemi;
				}
				InOutDir = FMath::Lerp(Reflected, RandomHemi, Settings.SurfaceRoughness).GetSafeNormal();
				if (Settings.BounceListenerBias > 0.f) {
					const FVector ToListener = (ListenerPos - EdgePoint).GetSafeNormal();
					InOutDir = FMath::Lerp(InOutDir, ToListener, Settings.BounceListenerBias).GetSafeNormal();
					if (FVector::DotProduct(InOutDir, CrawlOut.PerpHit.Normal) < 0.f) {
						InOutDir -= 2.f * FVector::DotProduct(InOutDir, CrawlOut.PerpHit.Normal) * CrawlOut.PerpHit.Normal;
						InOutDir.Normalize();
					}
				}
				InOutOrigin += CrawlOut.PerpHit.Normal * Settings.RaySurfaceBias;
			}
			else {
				InOutDir = EdgeDir;
				++InOutBounce;
			}
		}
	}

	if (!Out.bCrawlSucceeded) {
		const FVector Reflected = ReflectDirection(InOutDir, Hit.Normal);
		FVector RandomHemi = FMath::VRand();
		if (FVector::DotProduct(RandomHemi, Hit.Normal) < 0.f) {
			RandomHemi = -RandomHemi;
		}
		InOutDir = FMath::Lerp(Reflected, RandomHemi, Settings.SurfaceRoughness).GetSafeNormal();

		if (!bLoSAlreadyFound && bBias) {
			const FVector HitToLis = ListenerPos - Hit.Location;
			const float HitLisDist = HitToLis.Size();
			if (HitLisDist > 0.f) {
				const FVector S2L = HitToLis / HitLisDist;
				for (int32 Attempt = 0; Attempt < 20; ++Attempt) {
					FVector RandH = FMath::VRand();
					if (FVector::DotProduct(RandH, Hit.Normal) < 0.f) {
						RandH = -RandH;
					}
					const FVector Cand = FMath::Lerp(Reflected, RandH, Settings.SurfaceRoughness).GetSafeNormal();
					if (FMath::FRand() < (1.f - FMath::Abs(FVector::DotProduct(Cand, S2L)))) {
						InOutDir = Cand;
						break;
					}
				}
			}
		}
		if (Settings.BounceListenerBias > 0.f) {
			const FVector ToListener = (ListenerPos - Hit.Location).GetSafeNormal();
			InOutDir = FMath::Lerp(InOutDir, ToListener, Settings.BounceListenerBias).GetSafeNormal();
			if (FVector::DotProduct(InOutDir, Hit.Normal) < 0.f) {
				InOutDir -= 2.f * FVector::DotProduct(InOutDir, Hit.Normal) * Hit.Normal;
				InOutDir.Normalize();
			}
		}
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
	int32 EffMaxSteps = CrawlStepCap;
	float MaxCrawlRange = CrawlStepCap * S.CrawlStepSize;
	{
		FHitResult FwdHit;
		if (TraceLine(World, FwdHit, NudgedStart, NudgedStart + CrawlDir * MaxCrawlRange)) {
			const float HitDist = FVector::Dist(NudgedStart, FwdHit.Location);
			MaxCrawlRange = HitDist;
			EffMaxSteps = FMath::Min(FMath::FloorToInt(HitDist / S.CrawlStepSize), CrawlStepCap);
		}
	}

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
		for (int32 Step = EffMaxSteps + 1; Step <= S.MaxCrawlSteps; ++Step) {
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
				Out->LoSPoint = StepPos;
				Out->LoSCumDist = InCumDist + Step * S.CrawlStepSize;
				Out->bLoSFound = true;
				break;
			}
		}
	}

	if (Out) {
		Out->Steps.Reset();
		Out->ProbeEnds.Reset();
	}
	return false;
}

