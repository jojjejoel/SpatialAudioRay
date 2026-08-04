#include "Audio/AsyncCastManager.h"
#include "Audio/SpatialAudioComponent.h"
#include "Audio/Math.h"

#include "Algo/Reverse.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "WorldCollision.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"

DECLARE_CYCLE_STAT(TEXT("SpatialAudio Async Tick"), STAT_SpatialAudio_AsyncTick, STATGROUP_Game);

namespace {
	FVector SelectEdgeDirection(const FVector& SurfaceNormal, const FVector& ToListener) {
		return FVector::DotProduct(SurfaceNormal, ToListener) >= 0.f ? SurfaceNormal : -SurfaceNormal;
	}
}

void FAsyncCastManager::FinishOrDefer(FSpatialRayState& Ray, bool& bAllDone) {
	if (Ray.PendingLoSProbes.IsEmpty()) {
		Ray.bDone = true;
	}
	else {
		Ray.bTerminalLoSPending = true;
		bAllDone = false;
	}
}

bool FAsyncCastManager::TryAddListenerLoSProbe(const USpatialAudioComponent& Component, FSpatialRayState& Ray,
                                               UWorld* World, const FVector& SamplePos, float CumDist, float Budget,
                                               const USpatialAudioSettings& Settings) {
	if (!Math::IsWithinPathBudget(CumDist, SamplePos, Component.AsyncListenerPos, Budget)) {
		return false;
	}

	FSpatialRayState::FAsyncLoSProbe Probe;
	Probe.LoSHandle = Component.SubmitAsyncTrace(World, SamplePos, Component.AsyncListenerPos);
	Probe.SamplePos = SamplePos;
	Probe.CumDist = CumDist;
	Probe.BounceAtSubmit = Ray.Bounce;
	Ray.PendingLoSProbes.Add(MoveTemp(Probe));

	if (Component.bDrawDebugRays && Component.bShowLoSChecks) {
		DrawDebugLine(World, SamplePos, Component.AsyncListenerPos, FColor(160, 0, 255),
		              false, Settings.DebugLineDuration, 0, 0.5f);
	}
	return true;
}

void FAsyncCastManager::SubmitFlightSegment(const USpatialAudioComponent& Component, FSpatialRayState& Ray,
                                            UWorld* World, float SegmentLength) {
	Ray.SegSubmitLen = SegmentLength;
	Ray.SegHandle = Component.SubmitAsyncTrace(World, Ray.Origin, Ray.Origin + Ray.Dir * SegmentLength);
}

bool FAsyncCastManager::ShouldDrawFlightPaths(const USpatialAudioComponent& Component) {
	return Component.bDrawDebugRays && (Component.bShowBounceRays || Component.bShowSurfaceCrawl);
}

void FAsyncCastManager::DrawFlightSegment(const USpatialAudioComponent& Component, const UWorld* World,
                                          const FVector& From, const FVector& To,
                                          const USpatialAudioSettings& Settings) {
	if (!ShouldDrawFlightPaths(Component)) {
		return;
	}
	DrawDebugLine(World, From, To, FColor::White, false, Settings.DebugLineDuration, 0, 1.f);
	DrawDebugSphere(World, To, 5.f, 6, FColor::White, false, Settings.DebugLineDuration, SDPG_Foreground, 1.f);
}


void FAsyncCastManager::CaptureSweepPositions(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
                                              const AActor& Owner, const APawn& Pawn) {
	Component.TraceDiag.SweepFrameAccum = 0;
	Component.bPreSweepCast = Component.IsPreSweepActive();
	Component.AsyncSourcePos = Owner.GetActorLocation();
	Component.AsyncListenerPos = Pawn.GetActorLocation();
	Component.AsyncSteeringSourcePos = Component.AsyncSourcePos
		+ Component.ComputeSteeringLead(Component.VelocityScaling.SmoothedSourceVelocity, Settings);
	Component.AsyncSteeringListenerPos = Component.AsyncListenerPos
		+ Component.ComputeSteeringLead(Component.VelocityScaling.SmoothedListenerVelocity, Settings);
}

int32 FAsyncCastManager::CountHeldEdges(const TArray<FCachedEdgePoint>& Points) {
	int32 Count = 0;
	for (const FCachedEdgePoint& Edge : Points) {
		if (!Edge.bRelayed && !Edge.bEvicting) {
			++Count;
		}
	}
	return Count;
}

int32 FAsyncCastManager::ApplyCacheFullnessRayScale(const USpatialAudioComponent& Component, const int32 RayCount,
                                                    const USpatialAudioSettings& Settings) {
	if (Settings.FullCacheRayScale >= 1.f) {
		return RayCount;
	}
	const float Fullness = FMath::Clamp(
		static_cast<float>(CountHeldEdges(Component.CachedEdgePoints))
		/ FMath::Max(1, Settings.CachedEdgeMaxCount), 0.f, 1.f);

	const float Scale = FMath::Lerp(1.f, Settings.FullCacheRayScale, Fullness);
	return FMath::Max(1, FMath::RoundToInt(RayCount * Scale));
}

void FAsyncCastManager::ResolveSweepRayBudget(USpatialAudioComponent& Component,
                                              const USpatialAudioSettings& Settings) {
	int32 ScaledRayCount;
	Component.GetEffectiveRayCounts(ScaledRayCount, Component.CurrentPriority);
	Component.AsyncMaxBounces = FMath::Max(Settings.MinMaxBounces,
	                                       FMath::RoundToInt(Settings.MaxBounces * Component.CurrentPriority));
	Component.AsyncTotalRays = ApplyCacheFullnessRayScale(Component, ScaledRayCount, Settings);

	Component.PendingValidCachedPoints.Reset();
	Component.PendingValidCachedPoints.Append(Component.CachedEdgePoints);

	Component.TraceDiag.SweepAsyncRayAccum = 0;
}

void FAsyncCastManager::ResetCycleAccumulator(USpatialAudioComponent& Component) {
	Component.CycleAccum.RaysReached = 0;
	Component.CycleAccum.MinLoSDist = TNumericLimits<float>::Max();
	Component.CycleAccum.WeightedPos = FVector::ZeroVector;
	Component.CycleAccum.TotalWeight = 0.f;
	Component.CycleAccum.WeightedDist = 0.f;
	Component.CycleAccum.bDirectLoSFound = false;
}

void FAsyncCastManager::SelectCycleDirections(const TArray<FVector>& AllDirections, int32 StartIndex, int32 CycleCount,
                                              TArray<FVector>& OutDirections, TArray<int32>& OutIndices) {
	const int32 Stride = FMath::Max(CycleCount, 1);
	const int32 Expected = AllDirections.Num() / Stride + 1;
	OutDirections.Reset(Expected);
	OutIndices.Reset(Expected);

	for (int32 i = StartIndex; i < AllDirections.Num(); i += Stride) {
		OutDirections.Add(AllDirections[i]);
		OutIndices.Add(i);
	}
}

FRandomStream FAsyncCastManager::MakeBiasStream(const FVector& SourcePos, const FVector& ListenerPos, int32 RayIndex) {
	uint32 Seed = HashCombine(GetTypeHash(SourcePos), GetTypeHash(ListenerPos));
	Seed = HashCombine(Seed, static_cast<uint32>(RayIndex));
	return FRandomStream(static_cast<int32>(Seed));
}

FVector FAsyncCastManager::ApplyLateralBandBias(const USpatialAudioComponent& Component, const FVector& Dir,
                                                const FVector& ToListenerDir, float FullCastDistance,
                                                int32 DirectionIndex, const USpatialAudioSettings& Settings) {
	const float Radius = Settings.DirectLoSSampleRadius;
	const float FibWeight = Math::ComputeRayDirectionWeight(
		Dir, ToListenerDir, Component.LastDirectLoSFraction, Radius, FullCastDistance);

	FRandomStream BiasStream = MakeBiasStream(Component.AsyncSourcePos, Component.AsyncListenerPos, DirectionIndex);
	if (BiasStream.FRand() <= FibWeight) {
		return Dir;
	}

	for (int32 Attempt = 0; Attempt < 30; ++Attempt) {
		const FVector Candidate = BiasStream.VRand();
		const float CandidateWeight = Math::ComputeRayDirectionWeight(
			Candidate, ToListenerDir, Component.LastDirectLoSFraction, Radius, FullCastDistance);
		if (BiasStream.FRand() < FMath::Min(CandidateWeight, 1.f)) {
			return Candidate;
		}
	}
	return Dir;
}

void FAsyncCastManager::SubmitSweepRays(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
                                        UWorld* World, const FVector& ToListenerDir, float FullCastDistance,
                                        int32 CycleCount) {
	const TArray<FVector> AllDirections = Math::GenerateFibonacciDirections(Component.AsyncTotalRays, ToListenerDir);
	TArray<FVector> Directions;
	TArray<int32> DirectionIndices;
	SelectCycleDirections(AllDirections, Component.CycleAccum.Index, CycleCount, Directions, DirectionIndices);

	const bool bBias = FullCastDistance > 0.f;

	Component.AsyncRays.Reset(Directions.Num());

	for (int32 i = 0; i < Directions.Num(); ++i) {
		FVector Dir = Directions[i];
		if (bBias) {
			Dir = ApplyLateralBandBias(Component, Dir, ToListenerDir, FullCastDistance, DirectionIndices[i], Settings);
		}

		FSpatialRayState& Ray = Component.AsyncRays.AddDefaulted_GetRef();
		Ray.Origin = Component.AsyncSourcePos;
		Ray.Dir = Dir;
		Ray.LoSOrigin = Component.AsyncSourcePos;
		Ray.bNextHitCrawls = i % 2 == 0;

		const float SegLen = Math::ComputeNextSegmentLength(
			Component.MaxRayDistance * Settings.RayLengthMultiplier,
			TNumericLimits<float>::Max(), Settings.MaxStraightFlightDistance);
		SubmitFlightSegment(Component, Ray, World, SegLen);
	}
}

void FAsyncCastManager::StartAsyncFullCast(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings) {
	UWorld* World = Component.GetWorld();
	const AActor* Owner = Component.GetOwner();
	if (!World || !Owner) {
		return;
	}

	const APlayerController* PC = World->GetFirstPlayerController();
	if (!PC || !PC->GetPawn()) {
		return;
	}

	const int32 CycleCount = FMath::Max(1, Settings.FullSweepCycleCount);

	CaptureSweepPositions(Component, Settings, *Owner, *PC->GetPawn());

	if (FVector::DistSquared(Component.AsyncSourcePos, Component.AsyncListenerPos) > FMath::Square(
		Component.MaxRayDistance)) {
		Component.TargetOcclusion = 0.f;
		Component.TargetVirtualSourceLocation = Component.AsyncSourcePos;
		return;
	}

	ResolveSweepRayBudget(Component, Settings);
	ResetCycleAccumulator(Component);

	Component.LoSDiffractionPaths.Reset();
	Component.StoredLoSPaths.Reset();

	const float FullCastDistance = FVector::Dist(Component.AsyncSteeringSourcePos, Component.AsyncSteeringListenerPos);
	const FVector ToListenerDir = FullCastDistance > 0.f
		                              ? (Component.AsyncSteeringListenerPos - Component.AsyncSteeringSourcePos) /
		                              FullCastDistance
		                              : FVector::ForwardVector;

	SubmitSweepRays(Component, Settings, World, ToListenerDir, FullCastDistance, CycleCount);

	Component.TraceDiag.SweepAsyncRayAccum += Component.AsyncRays.Num();
	Component.bAsyncCastActive = true;
}


bool FAsyncCastManager::TryProcessMidAirTurn(const USpatialAudioComponent& Component, FSpatialRayState& Ray,
                                             UWorld* World,
                                             float Budget, bool bSegMissed, bool& bAllDone,
                                             const USpatialAudioSettings& Settings) {
	const FVector TurnPoint = Ray.Origin + Ray.Dir * Ray.SegSubmitLen;

	if (!bSegMissed || Settings.MaxStraightFlightDistance <= 0.f
		|| Ray.Bounce >= Component.AsyncMaxBounces
		|| Budget - (Ray.CumulativeDistance + Ray.SegSubmitLen) < 1.f
		|| !Math::IsWithinPathBudget(Ray.CumulativeDistance + Ray.SegSubmitLen, TurnPoint,
		                             Component.AsyncListenerPos, Budget)) {
		return false;
	}

	if (!Ray.bLoSFound) {
		SubmitSegmentLoSProbes(Component, Ray, World, Ray.Origin, Ray.Dir, Ray.SegSubmitLen, Budget, Settings);
	}

	DrawFlightSegment(Component, World, Ray.Origin, TurnPoint, Settings);

	Ray.CumulativeDistance += Ray.SegSubmitLen;
	Ray.Dir = ComputeMidAirTurnDirection(Ray.Dir, TurnPoint, Component.AsyncSteeringListenerPos,
	                                     !Ray.bLoSFound, Settings.SurfaceRoughness,
	                                     Settings.BounceListenerBias);
	Ray.Origin = TurnPoint;
	++Ray.Bounce;
	Ray.BounceWaypoints.Add({TurnPoint, Ray.CumulativeDistance});

	if (!Ray.bLoSFound) {
		TryAddListenerLoSProbe(Component, Ray, World, Ray.Origin, Ray.CumulativeDistance, Budget, Settings);
	}

	const float Remain = Math::ComputeNextSegmentLength(Component.MaxRayDistance,
	                                                    Budget - Ray.CumulativeDistance,
	                                                    Settings.MaxStraightFlightDistance);
	SubmitFlightSegment(Component, Ray, World, Remain);
	bAllDone = false;
	return true;
}

void FAsyncCastManager::ProcessRayTermination(const USpatialAudioComponent& Component, FSpatialRayState& Ray,
                                              UWorld* World,
                                              const FTraceDatum& SegData, bool bSegMissed, float Budget,
                                              bool& bAllDone, const USpatialAudioSettings& Settings) {
	if (bSegMissed) {
		const float RemainingBudget = FMath::Max(0.f, FMath::Min(
			                                         FMath::Min(Component.MaxRayDistance * Settings.RayLengthMultiplier,
			                                                    Ray.SegSubmitLen),
			                                         Budget - Ray.CumulativeDistance));
		const FVector TerminalPoint = Ray.Origin + Ray.Dir * RemainingBudget;

		if (ShouldDrawFlightPaths(Component)) {
			DrawDebugLine(World, Ray.Origin, TerminalPoint,
			              FColor::Red, false, Settings.DebugLineDuration, 0, 0.5f);
		}

		if (!Ray.bLoSFound) {
			const float SegLen = FVector::Dist(Ray.Origin, TerminalPoint);
			SubmitSegmentLoSProbes(Component, Ray, World, Ray.Origin, Ray.Dir, SegLen, Budget, Settings);
		}
	}
	else {
		const FHitResult& TermHit = SegData.OutHits[0];
		DrawFlightSegment(Component, World, Ray.Origin, TermHit.Location, Settings);
		if (!Ray.bLoSFound) {
			const float TermSegLen = FVector::Dist(Ray.Origin, TermHit.Location);
			if (TermSegLen > 1.f) {
				const FVector TermDir = (TermHit.Location - Ray.Origin).GetSafeNormal();
				SubmitSegmentLoSProbes(Component, Ray, World, Ray.Origin, TermDir, TermSegLen, Budget, Settings);
			}
		}
	}

	FinishOrDefer(Ray, bAllDone);
}

bool FAsyncCastManager::TrySetupSurfaceCrawl(USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
                                             const FHitResult& Hit, const USpatialAudioSettings& Settings) {
	const float DotDN = FVector::DotProduct(Ray.Dir, Hit.Normal);
	const FVector Slide = (Ray.Dir - DotDN * Hit.Normal).GetSafeNormal();
	const FVector ToLisRaw = Component.AsyncSteeringListenerPos - Hit.Location;
	const float DotLN = FVector::DotProduct(ToLisRaw, Hit.Normal);
	const FVector ToLisPlane = ToLisRaw - DotLN * Hit.Normal;
	const FVector LisBias = ToLisPlane.IsNearlyZero()
		                        ? Slide
		                        : ToLisPlane.GetSafeNormal();
	const FVector CrawlDir = FMath::Lerp(Slide, LisBias,
	                                     Settings.CrawlListenerBias).GetSafeNormal();

	if (CrawlDir.IsNearlyZero()) {
		return false;
	}

	const float NudgeDist = Settings.RaySurfaceBias;
	const float BackProbeLen = NudgeDist + 5.f;
	const FVector NudgedStart = Hit.Location + Hit.Normal * NudgeDist;
	const FVector BackDir = -Hit.Normal;

	Ray.CrawlNudgedStart = NudgedStart;
	Ray.CrawlDir = CrawlDir;
	Ray.CrawlHitNormal = Hit.Normal;
	Ray.CrawlHitLoc = Hit.Location;
	Ray.CrawlInDir = Ray.Dir;
	Ray.CrawlStepSz = Settings.CrawlStepSize;
	Ray.CrawlNudgeDist = NudgeDist;
	Ray.CrawlInCumDist = Ray.CumulativeDistance;
	int32 CrawlStepCap = Settings.MaxCrawlSteps;
	if (Settings.MaxStraightFlightDistance > 0.f) {
		CrawlStepCap = FMath::Clamp(FMath::FloorToInt(
			                            Settings.MaxStraightFlightDistance / FMath::Max(Settings.CrawlStepSize, 1.f)),
		                            1, Settings.MaxCrawlSteps);
	}
	Ray.CrawlMaxSteps = CrawlStepCap;

	const float MaxCrawlRange = CrawlStepCap * Settings.CrawlStepSize;
	Ray.CrawlRangeHandle = Component.SubmitAsyncTrace(World, NudgedStart,
	                                                  NudgedStart + CrawlDir * MaxCrawlRange);

	Ray.CrawlStepProbes.Reset();
	Ray.CrawlStepProbes.Reserve(CrawlStepCap);
	for (int32 Step = 1; Step <= CrawlStepCap; ++Step) {
		const FVector StepPos = NudgedStart + Step * Settings.CrawlStepSize * CrawlDir;
		const FVector BackEnd = StepPos + BackDir * BackProbeLen;
		const FVector ForwardEnd = StepPos + CrawlDir * Settings.CrawlStepSize;

		FSpatialRayState::FAsyncCrawlStepProbe StepProbe;
		StepProbe.StepPos = StepPos;
		StepProbe.StepCumDist = Ray.CumulativeDistance + Step * Settings.CrawlStepSize;
		StepProbe.BackHandle = Component.SubmitAsyncTrace(World, StepPos, BackEnd);
		StepProbe.LoSHandle = Component.SubmitAsyncTrace(World, StepPos, Component.AsyncListenerPos);
		StepProbe.PerpHandle = Component.SubmitAsyncTrace(World, StepPos, ForwardEnd);
		Ray.CrawlStepProbes.Add(MoveTemp(StepProbe));
	}

	Ray.BatchPhase = FSpatialRayState::ERayBatchPhase::CrawlBatch;
	return true;
}

void FAsyncCastManager::ProcessRayBounceContinuation(USpatialAudioComponent& Component, FSpatialRayState& Ray,
                                                     UWorld* World, const FHitResult& Hit, float Budget,
                                                     bool& bAllDone, const USpatialAudioSettings& Settings) {
	if (FVector::DotProduct(Hit.Normal, Ray.Dir) > 0.f) {
		if (Component.bDrawDebugRays && Component.bShowSurfaceCrawl) {
			DrawDebugSphere(World, Hit.Location, 18.f, 8, FColor::Red,
			                false, Settings.DebugLineDuration * 4.f, SDPG_Foreground, 3.f);
			DrawDebugLine(World, Hit.Location, Hit.Location + Hit.Normal * 40.f,
			              FColor::Red, false, Settings.DebugLineDuration * 4.f, 0, 2.f);
		}
		FinishOrDefer(Ray, bAllDone);
		return;
	}

	if (!Ray.bLoSFound) {
		const float MidSegLen = FVector::Dist(Ray.Origin, Hit.Location);
		if (MidSegLen > 1.f) {
			const FVector MidDir = (Hit.Location - Ray.Origin).GetSafeNormal();
			SubmitSegmentLoSProbes(Component, Ray, World, Ray.Origin, MidDir, MidSegLen, Budget, Settings);
		}
	}

	Ray.CumulativeDistance += FVector::Dist(Ray.Origin, Hit.Location);

	DrawFlightSegment(Component, World, Ray.Origin, Hit.Location, Settings);

	if (Ray.bNextHitCrawls && TrySetupSurfaceCrawl(Component, Ray, World, Hit, Settings)) {
		bAllDone = false;
		return;
	}

	Ray.Dir = Math::ComputeBouncedDirection(Ray.Dir, Hit.Normal, !Ray.bLoSFound,
	                                        Hit.Location, Component.AsyncSteeringListenerPos, Settings.SurfaceRoughness,
	                                        Settings.BounceListenerBias);
	Ray.Origin = Hit.Location + Hit.Normal * Settings.RaySurfaceBias;
	++Ray.Bounce;
	Ray.BounceWaypoints.Add({Ray.Origin, Ray.CumulativeDistance});

	Ray.bNextHitCrawls = !Ray.bNextHitCrawls;

	if (!Ray.bLoSFound) {
		TryAddListenerLoSProbe(Component, Ray, World, Ray.Origin, Ray.CumulativeDistance, Budget, Settings);
	}

	const float Remain = Math::ComputeNextSegmentLength(Component.MaxRayDistance,
	                                                    Budget - Ray.CumulativeDistance,
	                                                    Settings.MaxStraightFlightDistance);
	if (Remain < 1.f
		|| !Math::IsWithinPathBudget(Ray.CumulativeDistance, Ray.Origin, Component.AsyncListenerPos, Budget)) {
		FinishOrDefer(Ray, bAllDone);
	}
	else {
		SubmitFlightSegment(Component, Ray, World, Remain);
		bAllDone = false;
	}
}

void FAsyncCastManager::TickSingleRay(USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
                                      float Budget, bool& bAllDone,
                                      const USpatialAudioSettings& Settings) {
	const bool bLoSWasFound = Ray.bLoSFound;
	DrainPendingLoSProbes(Component, Ray, World, Component.AsyncListenerPos);
	if (!bLoSWasFound && Ray.bLoSFound && ShouldDrawFlightPaths(Component)) {
		DrawDebugSphere(World, Ray.LoSOrigin, 10.f, 8, FColor::Green, false, Settings.DebugLineDuration,
		                SDPG_Foreground, 2.f);
	}

	if (Ray.bLoSFound) {
		FinishOrDefer(Ray, bAllDone);
		return;
	}

	if (Ray.bTerminalLoSPending) {
		if (Ray.PendingLoSProbes.IsEmpty()) {
			Ray.bTerminalLoSPending = false;
			Ray.bDone = true;
		}
		else {
			bAllDone = false;
		}
		return;
	}

	if (Ray.BatchPhase == FSpatialRayState::ERayBatchPhase::CrawlBatch) {
		ProcessCrawlBatch(Component, Ray, World, Budget, bAllDone, Settings);
		return;
	}

	FTraceDatum SegData;
	if (!World->QueryTraceData(Ray.SegHandle, SegData)) {
		bAllDone = false;
		return;
	}

	const bool bSegMissed = SegData.OutHits.IsEmpty();

	if (TryProcessMidAirTurn(Component, Ray, World, Budget, bSegMissed, bAllDone, Settings)) {
		return;
	}

	if (bSegMissed || Ray.Bounce >= Component.AsyncMaxBounces) {
		ProcessRayTermination(Component, Ray, World, SegData, bSegMissed, Budget, bAllDone, Settings);
	}
	else {
		ProcessRayBounceContinuation(Component, Ray, World, SegData.OutHits[0], Budget, bAllDone, Settings);
	}
}

void FAsyncCastManager::TickAsyncCast(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings) {
	SCOPE_CYCLE_COUNTER(STAT_SpatialAudio_AsyncTick);

	UWorld* World = Component.GetWorld();
	if (!World) {
		return;
	}

	const float Budget = Component.MaxRayDistance * Settings.TotalPathBudgetMultiplier;

	bool bAllDone = true;

	for (FSpatialRayState& Ray : Component.AsyncRays) {
		if (Ray.bDone) {
			continue;
		}
		TickSingleRay(Component, Ray, World, Budget, bAllDone, Settings);
	}

	if (bAllDone) {
		SubmitFinalizeBatch(Component, Settings);
	}
}

void FAsyncCastManager::DrainPendingLoSProbes(const USpatialAudioComponent& Component, FSpatialRayState& Ray,
                                              UWorld* World, const FVector& ListenerPos) {
	if (Ray.PendingLoSProbes.IsEmpty()) {
		return;
	}

	FVector BestLoSPos = FVector::ZeroVector;
	float BestCumDist = FLT_MAX;
	int32 BestBounce = 0;
	bool bFoundNewLoS = false;

	for (int32 ProbeIdx = Ray.PendingLoSProbes.Num() - 1; ProbeIdx >= 0; --ProbeIdx) {
		FSpatialRayState::FAsyncLoSProbe& Probe = Ray.PendingLoSProbes[ProbeIdx];
		FTraceDatum LoSData;
		if (!World->QueryTraceData(Probe.LoSHandle, LoSData)) {
			continue;
		}

		if (!Ray.bLoSFound && IsTraceClear(LoSData) && Probe.CumDist < BestCumDist) {
			FHitResult SanityHit;
			if (!Component.TraceLine(World, SanityHit, ListenerPos, Probe.SamplePos)) {
				BestCumDist = Probe.CumDist;
				BestLoSPos = Probe.SamplePos;
				BestBounce = Probe.BounceAtSubmit;
				bFoundNewLoS = true;
			}
		}
		Ray.PendingLoSProbes.RemoveAt(ProbeIdx);
	}

	if (bFoundNewLoS && !Ray.bLoSFound) {
		Ray.bLoSFound = true;
		Ray.LoSBounces = FMath::Max(1, BestBounce);
		Ray.LoSOrigin = BestLoSPos;
		Ray.LoSCumulativeDistance = BestCumDist;
	}
}


bool FAsyncCastManager::AreCrawlTracesReady(UWorld* World, FSpatialRayState& Ray, FTraceDatum& OutRangeData) {
	if (!World->QueryTraceData(Ray.CrawlRangeHandle, OutRangeData)) {
		return false;
	}
	for (FSpatialRayState::FAsyncCrawlStepProbe& StepProbe : Ray.CrawlStepProbes) {
		FTraceDatum Ready;
		if (!World->QueryTraceData(StepProbe.BackHandle, Ready) ||
			!World->QueryTraceData(StepProbe.LoSHandle, Ready) ||
			!World->QueryTraceData(StepProbe.PerpHandle, Ready)) {
			return false;
		}
	}
	return true;
}

void FAsyncCastManager::TryConfirmLoSAtCrawlStep(const USpatialAudioComponent& Component, FSpatialRayState& Ray,
                                                 UWorld* World,
                                                 const FSpatialRayState::FAsyncCrawlStepProbe& StepProbe,
                                                 const USpatialAudioSettings& Settings) {
	if (Ray.bLoSFound
		|| FVector::DotProduct(Component.AsyncListenerPos - StepProbe.StepPos, Ray.CrawlHitNormal) <= 0.f) {
		return;
	}

	if (Component.bDrawDebugRays && Component.bShowLoSChecks) {
		DrawDebugLine(World, StepProbe.StepPos, Component.AsyncListenerPos, FColor(160, 0, 255),
		              false, Settings.DebugLineDuration, 0, 0.5f);
	}

	FTraceDatum LoSData;
	World->QueryTraceData(StepProbe.LoSHandle, LoSData);
	if (!IsTraceClear(LoSData)) {
		return;
	}
	if (FHitResult SanityHit; Component.TraceLine(World, SanityHit, Component.AsyncListenerPos, StepProbe.StepPos)) {
		return;
	}

	Ray.bLoSFound = true;
	Ray.LoSBounces = FMath::Max(1, Ray.Bounce);
	Ray.LoSOrigin = StepProbe.StepPos;
	Ray.LoSCumulativeDistance = StepProbe.StepCumDist;

	if (Component.bDrawDebugRays && Component.bShowSurfaceCrawl) {
		DrawDebugSphere(World, StepProbe.StepPos, 10.f, 8, FColor::Green,
		                false, Settings.DebugLineDuration, SDPG_Foreground, 2.f);
	}
}

bool FAsyncCastManager::TryTakePerpWallExit(const FSpatialRayState& Ray, UWorld* World,
                                            const FSpatialRayState::FAsyncCrawlStepProbe& StepProbe,
                                            const int32 StepIdx, FCrawlStepResult& OutResult) {
	FTraceDatum PerpData;
	World->QueryTraceData(StepProbe.PerpHandle, PerpData);
	if (IsTraceClear(PerpData)) {
		return false;
	}

	const FHitResult& PerpHit = PerpData.OutHits[0];
	OutResult.EdgePoint = PerpHit.Location + PerpHit.Normal * Ray.CrawlNudgeDist;
	OutResult.EdgeDir = Math::ReflectDirection(Ray.CrawlDir, PerpHit.Normal);
	OutResult.CrawlDist = static_cast<float>(StepIdx) * Ray.CrawlStepSz
		+ FVector::Dist(StepProbe.StepPos, PerpHit.Location);
	OutResult.PerpNormal = PerpHit.Normal;
	OutResult.bPerpWallHit = true;
	OutResult.bSucceeded = true;
	OutResult.FoundAtStep = StepIdx;
	return true;
}

bool FAsyncCastManager::TryTakeFreeEdgeExit(const USpatialAudioComponent& Component, const FSpatialRayState& Ray,
                                            UWorld* World,
                                            const FSpatialRayState::FAsyncCrawlStepProbe& StepProbe,
                                            const int32 StepIdx, FCrawlStepResult& OutResult) {
	FTraceDatum BackData;
	World->QueryTraceData(StepProbe.BackHandle, BackData);
	if (!IsTraceClear(BackData)) {
		return false;
	}

	const FVector ToListener = (Component.AsyncSteeringListenerPos - StepProbe.StepPos).GetSafeNormal();
	OutResult.EdgePoint = StepProbe.StepPos;
	OutResult.EdgeDir = SelectEdgeDirection(Ray.CrawlHitNormal, ToListener);
	OutResult.CrawlDist = static_cast<float>(StepIdx + 1) * Ray.CrawlStepSz;
	OutResult.bSucceeded = true;
	OutResult.FoundAtStep = StepIdx;
	return true;
}

FAsyncCastManager::FCrawlStepResult FAsyncCastManager::EvaluateCrawlSteps(
	const USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World, int32 Limit,
	const USpatialAudioSettings& Settings) {
	FCrawlStepResult Result;

	for (int32 StepIdx = 0; StepIdx < Limit; ++StepIdx) {
		const FSpatialRayState::FAsyncCrawlStepProbe& StepProbe = Ray.CrawlStepProbes[StepIdx];

		TryConfirmLoSAtCrawlStep(Component, Ray, World, StepProbe, Settings);
		if (TryTakePerpWallExit(Ray, World, StepProbe, StepIdx, Result)
			|| TryTakeFreeEdgeExit(Component, Ray, World, StepProbe, StepIdx, Result)) {
			break;
		}
	}

	return Result;
}

void FAsyncCastManager::DrawCrawlDebugVisualization(const FSpatialRayState& Ray,
                                                    const UWorld* World, const FCrawlStepResult& Result, int32 Limit,
                                                    const USpatialAudioSettings& Settings) {
	DrawDebugSphere(World, Ray.CrawlNudgedStart, 6.f, 6, FColor::Cyan,
	                false, Settings.DebugLineDuration, SDPG_Foreground, 1.5f);

	FVector PrevStepPos = Ray.CrawlNudgedStart;
	const int32 DrawLimit = (Result.FoundAtStep >= 0) ? Result.FoundAtStep + 1 : Limit;
	for (int32 StepIdx = 0; StepIdx < DrawLimit; ++StepIdx) {
		const FSpatialRayState::FAsyncCrawlStepProbe& StepProbe = Ray.CrawlStepProbes[StepIdx];

		const bool bIsEdgeStep = (StepIdx == Result.FoundAtStep);
		const FColor StepColor = !bIsEdgeStep
			                         ? FColor::White
			                         : Result.bPerpWallHit
			                         ? FColor::Orange
			                         : FColor::Yellow;
		const float Radius = bIsEdgeStep ? 12.f : 4.f;

		DrawDebugSphere(World, StepProbe.StepPos, Radius, 6, StepColor,
		                false, Settings.DebugLineDuration, SDPG_Foreground, 1.f);
		DrawDebugLine(World, PrevStepPos, StepProbe.StepPos, FColor(0, 220, 255),
		              false, Settings.DebugLineDuration, 0, 1.5f);
		PrevStepPos = StepProbe.StepPos;
	}

	if (Result.bSucceeded) {
		const FColor EdgeColor = Result.bPerpWallHit ? FColor::Orange : FColor::Yellow;
		DrawDebugLine(World, Result.EdgePoint, Result.EdgePoint + Result.EdgeDir * 40.f,
		              EdgeColor, false, Settings.DebugLineDuration, 0, 2.f);
	}
	else {
		DrawDebugSphere(World, Ray.CrawlHitLoc, 10.f, 6, FColor(255, 80, 80),
		                false, Settings.DebugLineDuration, SDPG_Foreground, 2.f);
	}
}

void FAsyncCastManager::ApplyCrawlResult(const USpatialAudioComponent& Component, FSpatialRayState& Ray,
                                         const FCrawlStepResult& Result,
                                         const USpatialAudioSettings& Settings) {
	Ray.CrawlStepProbes.Empty();
	Ray.BatchPhase = FSpatialRayState::ERayBatchPhase::None;

	if (Result.bSucceeded) {
		Ray.CumulativeDistance += Result.CrawlDist;
		Ray.Dir = Result.EdgeDir;
		Ray.Origin = Result.bPerpWallHit
			             ? Result.EdgePoint + Result.PerpNormal * Settings.RaySurfaceBias
			             : Result.EdgePoint;
		++Ray.Bounce;
		Ray.BounceWaypoints.Add({Ray.Origin, Ray.CumulativeDistance});
	}
	else {
		Ray.Dir = Math::ComputeBouncedDirection(Ray.CrawlInDir, Ray.CrawlHitNormal, !Ray.bLoSFound,
		                                        Ray.CrawlHitLoc, Component.AsyncSteeringListenerPos,
		                                        Settings.SurfaceRoughness,
		                                        Settings.BounceListenerBias);
		Ray.Origin = Ray.CrawlHitLoc + Ray.CrawlHitNormal * Settings.RaySurfaceBias;
		++Ray.Bounce;
	}
}

void FAsyncCastManager::ProcessCrawlBatch(const USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
                                          float Budget, bool& bAllDone,
                                          const USpatialAudioSettings& Settings) {
	FTraceDatum RangeData;
	if (!AreCrawlTracesReady(World, Ray, RangeData)) {
		bAllDone = false;
		return;
	}

	const int32 NumSteps = Ray.CrawlStepProbes.Num();
	int32 EffMaxSteps = Ray.CrawlMaxSteps;
	if (!IsTraceClear(RangeData)) {
		const float HitDist = FVector::Dist(Ray.CrawlNudgedStart, RangeData.OutHits[0].Location);
		EffMaxSteps = FMath::Min(
			FMath::FloorToInt(HitDist / FMath::Max(Ray.CrawlStepSz, 1.f)),
			Ray.CrawlMaxSteps);
	}
	const int32 Limit = FMath::Min(EffMaxSteps, NumSteps);

	const FCrawlStepResult Result = EvaluateCrawlSteps(Component, Ray, World, Limit, Settings);

	if (Component.bDrawDebugRays && Component.bShowSurfaceCrawl) {
		DrawCrawlDebugVisualization(Ray, World, Result, Limit, Settings);
	}

	ApplyCrawlResult(Component, Ray, Result, Settings);

	Ray.bNextHitCrawls = !Ray.bNextHitCrawls;

	if (!Ray.bLoSFound) {
		TryAddListenerLoSProbe(Component, Ray, World, Ray.Origin, Ray.CumulativeDistance, Budget, Settings);
	}

	if (Ray.bLoSFound || Budget - Ray.CumulativeDistance < 1.f
		|| !Math::IsWithinPathBudget(Ray.CumulativeDistance, Ray.Origin, Component.AsyncListenerPos, Budget)) {
		FinishOrDefer(Ray, bAllDone);
		return;
	}

	const float Remain = Math::ComputeNextSegmentLength(Component.MaxRayDistance,
	                                                    Budget - Ray.CumulativeDistance,
	                                                    Settings.MaxStraightFlightDistance);
	SubmitFlightSegment(Component, Ray, World, Remain);
	bAllDone = false;
}

FVector FAsyncCastManager::ComputeMidAirTurnDirection(const FVector& InDir, const FVector& TurnPoint,
                                                      const FVector& ListenerPos, bool bApplyBias,
                                                      float SurfaceRoughness, float BounceListenerBias) {
	if (SurfaceRoughness <= 0.f && BounceListenerBias <= 0.f) {
		const uint32 Seed = HashCombine(GetTypeHash(TurnPoint), GetTypeHash(ListenerPos));
		const FRandomStream Stream(static_cast<int32>(Seed));
		FVector AxisU, AxisV;
		InDir.FindBestAxisVectors(AxisU, AxisV);
		const float Angle = Stream.FRand() * 2.f * PI;
		return (FMath::Cos(Angle) * AxisU + FMath::Sin(Angle) * AxisV).GetSafeNormal();
	}

	FVector Result = FMath::Lerp(InDir, FMath::VRand(), SurfaceRoughness).GetSafeNormal();

	if (bApplyBias) {
		const FVector ToLis = ListenerPos - TurnPoint;
		const float LisDist = ToLis.Size();
		if (LisDist > 0.f) {
			const FVector ToListenerDir = ToLis / LisDist;
			for (int32 Attempt = 0; Attempt < 20; ++Attempt) {
				const FVector Cand = FMath::Lerp(InDir, FMath::VRand(), SurfaceRoughness).GetSafeNormal();
				if (FMath::FRand() < (1.f - FMath::Abs(FVector::DotProduct(Cand, ToListenerDir)))) {
					Result = Cand;
					break;
				}
			}
		}
	}

	if (BounceListenerBias > 0.f) {
		const FVector ToListener = (ListenerPos - TurnPoint).GetSafeNormal();
		Result = FMath::Lerp(Result, ToListener, BounceListenerBias).GetSafeNormal();
	}

	return Result;
}

void FAsyncCastManager::SubmitSegmentLoSProbes(const USpatialAudioComponent& Component, FSpatialRayState& Ray,
                                               UWorld* World, const FVector& SegOrigin, const FVector& SegDir,
                                               float SegLen, float Budget,
                                               const USpatialAudioSettings& Settings) {
	const float StepSize = Settings.DiffractionEdgeSampleStep;
	const int32 MaxSamples = Settings.MaxSamplesPerSegment > 0 ? Settings.MaxSamplesPerSegment : 16;
	int32 SampleCount = 0;

	for (float T = StepSize; T < SegLen; T += StepSize) {
		if (SampleCount >= MaxSamples) {
			break;
		}
		++SampleCount;

		if (!TryAddListenerLoSProbe(Component, Ray, World, SegOrigin + SegDir * T,
		                            Ray.CumulativeDistance + T, Budget, Settings)) {
			break;
		}
	}

	const float SafeT = FMath::Max(0.f, SegLen - 2.f);
	if (SafeT > 0.f) {
		TryAddListenerLoSProbe(Component, Ray, World, SegOrigin + SegDir * SafeT,
		                       Ray.CumulativeDistance + SafeT, Budget, Settings);
	}
}


bool FAsyncCastManager::HasClearShortcut(const USpatialAudioComponent& Component, const UWorld* World,
                                         const FVector& Edge, const FVector& Anchor) {
	FHitResult Hit;
	return !Component.TraceLine(World, Hit, Edge, Anchor)
		&& !Component.TraceLine(World, Hit, Anchor, Edge);
}

int32 FAsyncCastManager::CountPrefixAnchorWaypoints(const TArray<FSpatialRayState::FBounceWaypoint>& Waypoints,
                                                    float LoSCumulativeDistance) {
	int32 Count = 0;
	while (Count < Waypoints.Num() && Waypoints[Count].CumDist < LoSCumulativeDistance) {
		++Count;
	}
	return Count;
}

int32 FAsyncCastManager::FindFirstVisibleAnchor(const USpatialAudioComponent& Component, const UWorld* World,
                                                const FVector& FromPoint,
                                                const TArray<FSpatialRayState::FBounceWaypoint>& Waypoints,
                                                int32 SearchLimit) {
	for (int32 AnchorIdx = 0; AnchorIdx < SearchLimit; ++AnchorIdx) {
		if (HasClearShortcut(Component, World, FromPoint, Waypoints[AnchorIdx].Pos)) {
			return AnchorIdx;
		}
	}
	return INDEX_NONE;
}

float FAsyncCastManager::ComputeStringPulledLeg1(const USpatialAudioComponent& Component, const UWorld* World,
                                                 const FSpatialRayState& Ray, const FVector& SourcePos,
                                                 TArray<FVector>& OutPath, TArray<bool>& OutSegmentVerified) {
	TArray<FVector> ReversePath;
	TArray<bool> ReverseSegmentVerified;
	ReversePath.Add(Ray.LoSOrigin);

	FVector PullPoint = Ray.LoSOrigin;
	float PullPointCumDist = Ray.LoSCumulativeDistance;
	int32 RemainingAnchors = CountPrefixAnchorWaypoints(Ray.BounceWaypoints, Ray.LoSCumulativeDistance);
	float PulledDist = 0.f;

	while (true) {
		if (HasClearShortcut(Component, World, PullPoint, SourcePos)) {
			PulledDist += FVector::Dist(PullPoint, SourcePos);
			ReverseSegmentVerified.Add(true);
			ReversePath.Add(SourcePos);
			break;
		}

		const int32 AnchorIdx = FindFirstVisibleAnchor(Component, World, PullPoint, Ray.BounceWaypoints,
		                                               RemainingAnchors);
		if (AnchorIdx != INDEX_NONE) {
			PulledDist += FVector::Dist(PullPoint, Ray.BounceWaypoints[AnchorIdx].Pos);
			ReverseSegmentVerified.Add(true);
			PullPoint = Ray.BounceWaypoints[AnchorIdx].Pos;
			PullPointCumDist = Ray.BounceWaypoints[AnchorIdx].CumDist;
			RemainingAnchors = AnchorIdx;
			ReversePath.Add(PullPoint);
			continue;
		}

		if (RemainingAnchors == 0) {
			PulledDist += PullPointCumDist;
			ReverseSegmentVerified.Add(false);
			ReversePath.Add(SourcePos);
			break;
		}

		const FSpatialRayState::FBounceWaypoint& Preceding = Ray.BounceWaypoints[RemainingAnchors - 1];
		PulledDist += PullPointCumDist - Preceding.CumDist;
		ReverseSegmentVerified.Add(false);
		PullPoint = Preceding.Pos;
		PullPointCumDist = Preceding.CumDist;
		--RemainingAnchors;
		ReversePath.Add(PullPoint);
	}

	PulledDist = FMath::Min(PulledDist, Ray.LoSCumulativeDistance);

	OutPath = MoveTemp(ReversePath);
	Algo::Reverse(OutPath);
	OutSegmentVerified = MoveTemp(ReverseSegmentVerified);
	Algo::Reverse(OutSegmentVerified);
	return PulledDist;
}

void FAsyncCastManager::SubmitFinalizeBatch(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings) {
	const UWorld* World = Component.GetWorld();

	const bool bDirectLoSFound = !Component.bPreSweepCast && Math::HasAnyDirectLoS(Component.AsyncRays);

	const FCachedPointAccum Accum = AccumulateCachedPoints(
		Component.PendingValidCachedPoints, Component.MaxRayDistance, Settings);

	int32 RaysReached = Accum.RaysReached;
	float MinLoSDist = Accum.MinLoSDist;

	Component.Finalize.RefineProbes.Reset();

	for (int32 RayIdx = 0; RayIdx < Component.AsyncRays.Num(); ++RayIdx) {
		const FSpatialRayState& Ray = Component.AsyncRays[RayIdx];

		if (Ray.bLoSFound) {
			++RaysReached;
			MinLoSDist = FMath::Min(MinLoSDist, Ray.LoSCumulativeDistance);
		}

		if (Ray.bLoSFound && Ray.LoSBounces > 0 && !bDirectLoSFound) {
			FFinalizeRefineProbe Probe;
			Probe.LoSOrigin = Ray.LoSOrigin;
			Probe.BasePathDist = ComputeStringPulledLeg1(Component, World, Ray, Component.AsyncSourcePos,
			                                             Probe.ShortestPath, Probe.ShortestPathSegmentVerified);
			Probe.LoSBounces = Ray.LoSBounces;
			Probe.BounceWeightFactor = Settings.bWeightCandidatesByBounceCount
				                           ? FMath::Pow(Settings.BounceCountFalloff, static_cast<float>(Ray.LoSBounces))
				                           : 1.f;
			Component.Finalize.RefineProbes.Add(MoveTemp(Probe));
		}
	}

	Component.Finalize.RaysReached = RaysReached;
	Component.Finalize.MinLoSDist = MinLoSDist;
	Component.Finalize.WeightedPosSum = Accum.WeightedPos;
	Component.Finalize.TotalWeight = Accum.TotalWeight;
	Component.Finalize.WeightedDistSum = Accum.WeightedDist;
	Component.Finalize.bDirectLoSFound = bDirectLoSFound;
	Component.Finalize.bPending = true;
	Component.bAsyncCastActive = false;
	Component.AsyncRays.Reset();
}
