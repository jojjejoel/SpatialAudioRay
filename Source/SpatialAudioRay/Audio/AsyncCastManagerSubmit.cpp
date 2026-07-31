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
	bool IsTraceClear(const FTraceDatum& Datum) {
		return Datum.OutHits.IsEmpty() || !Datum.OutHits[0].bBlockingHit;
	}

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

void FAsyncCastManager::DrawFlightSegment(const USpatialAudioComponent& Component, UWorld* World,
                                          const FVector& From, const FVector& To,
                                          const USpatialAudioSettings& Settings) {
	if (!ShouldDrawFlightPaths(Component)) {
		return;
	}
	DrawDebugLine(World, From, To, FColor::White, false, Settings.DebugLineDuration, 0, 1.f);
	DrawDebugSphere(World, To, 5.f, 6, FColor::White, false, Settings.DebugLineDuration, SDPG_Foreground, 1.f);
}

// ── StartAsyncFullCast phases ────────────────────────────────────────────────

void FAsyncCastManager::CaptureSweepPositions(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings,
                                              const AActor& Owner, const APawn& Pawn) {
	Component.TraceDiag.SweepTraceAccum = 0;
	Component.TraceDiag.SweepFrameAccum = 0;
	Component.bPreSweepCast = Component.IsPreSweepActive();
	Component.AsyncSourcePos = Owner.GetActorLocation();
	Component.AsyncListenerPos = Pawn.GetActorLocation();
	// Velocity-led STEERING positions: aim rays where source/listener are heading — or, within
	// the lead time of losing LoS, where they came from (see ComputeSteeringLead). Probes/gates
	// keep verifying against the actual positions.
	Component.AsyncSteeringSourcePos = Component.AsyncSourcePos
		+ Component.ComputeSteeringLead(Component.VelocityScaling.SmoothedSourceVelocity, Settings);
	Component.AsyncSteeringListenerPos = Component.AsyncListenerPos
		+ Component.ComputeSteeringLead(Component.VelocityScaling.SmoothedListenerVelocity, Settings);
}

void FAsyncCastManager::ResolveSweepRayBudget(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings) {
	int32 ScaledRayCount;
	Component.GetEffectiveRayCounts(ScaledRayCount, Component.CurrentPriority);
	Component.AsyncMaxBounces = FMath::Max(Settings.MinMaxBounces,
	                                        FMath::RoundToInt(Settings.MaxBounces * Component.CurrentPriority));
	Component.AsyncTotalRays = ScaledRayCount;

	Component.PendingValidCachedPoints.Reset();
	if (Settings.bCacheEdgePoints) {
		Component.PendingValidCachedPoints.Append(Component.CachedEdgePoints);
	}

	// Only direct, full-strength entries substitute for rays. A relayed edge means the real
	// listener leg is gone — the relay is an audible stopgap, not a found path — and an
	// evicting entry is on its way out; both keep presenting through the snapshot, but the
	// sweep must keep searching at full budget while they're what's playing, so a genuine
	// replacement path can be found and displace them.
	int32 SubstituteCount = 0;
	for (const FCachedEdgePoint& Edge : Component.PendingValidCachedPoints) {
		if (!Edge.bRelayed && !Edge.bEvicting) {
			++SubstituteCount;
		}
	}

	Component.AsyncActualRays = FMath::Max(0, ScaledRayCount - SubstituteCount);
	Component.TraceDiag.SweepAsyncRayAccum = 0;
	Component.TraceDiag.LastSweepCachedReplaced = SubstituteCount;
}

void FAsyncCastManager::BuildCachedEdgeExclusionDirs(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings) {
	Component.CachedEdgeDirs.Reset();
	if (!Settings.bCacheEdgePoints || Settings.CachedEdgeExclusionAngleDeg <= 0.f || Settings.IsDirectionSkippingDisabled()) {
		return;
	}

	const float MoveThresholdSq = FMath::Square(Settings.CachedEdgeUpdateMoveThreshold);
	for (const FCachedEdgePoint& Edge : Component.PendingValidCachedPoints) {
		// A relayed edge must not exclude its direction either: the region around it is exactly
		// where a real replacement path most likely exists, and finding one is what lets the
		// relay yield.
		if (Edge.bEvicting || Edge.bRelayed) {
			continue;
		}
		// The edge was confirmed from where the source and listener stood at the time. Once
		// either has moved, it no longer stands in for a ray this sweep would have cast.
		const bool bSourceMoved =
			FVector::DistSquared(Component.AsyncSourcePos, Edge.CapturedSourcePos) > MoveThresholdSq;
		const bool bListenerMoved =
			FVector::DistSquared(Component.AsyncListenerPos, Edge.CapturedListenerPos) > MoveThresholdSq;
		if (bSourceMoved || bListenerMoved) {
			continue;
		}
		const FVector ToEdge = Edge.EdgePoint - Component.AsyncSourcePos;
		const float DistToEdge = ToEdge.Size();
		if (DistToEdge > 1.f) {
			Component.CachedEdgeDirs.Add(ToEdge / DistToEdge);
		}
	}
}

void FAsyncCastManager::UpdateActiveMissDirs(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings) {
	Component.ActiveMissDirs.Reset();
	if (!Settings.bCacheEdgePoints || Settings.CachedMissExclusionAngleDeg <= 0.f || Settings.IsDirectionSkippingDisabled()) {
		return;
	}

	// Backwards so an entry can be dropped mid-loop. A moved SOURCE retires the record outright —
	// the geometry it learned about no longer applies — whereas a moved listener only benches it
	// for this sweep, since the same direction may still miss from where the source still stands.
	const float MoveThresholdSq = FMath::Square(Settings.CachedEdgeUpdateMoveThreshold);
	for (int32 i = Component.CachedMissDirs.Num() - 1; i >= 0; --i) {
		const FCachedMissDir& MissDir = Component.CachedMissDirs[i];
		if (FVector::DistSquared(Component.AsyncSourcePos, MissDir.CapturedSourcePos) > MoveThresholdSq) {
			Component.CachedMissDirs.RemoveAt(i);
			continue;
		}
		const bool bListenerMoved =
			FVector::DistSquared(Component.AsyncListenerPos, MissDir.CapturedListenerPos) > MoveThresholdSq;
		if (!bListenerMoved) {
			Component.ActiveMissDirs.Add(MissDir.Dir);
		}
	}
}

void FAsyncCastManager::ResetCycleAccumulator(USpatialAudioComponent& Component) {
	Component.CycleAccum.RaysReached = 0;
	Component.CycleAccum.LoSBounces = 0;
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

FAsyncCastManager::FMissDirResolution FAsyncCastManager::ResolveMissDirection(
	const USpatialAudioComponent& Component, const FVector& Dir, float MissMinDot,
	const USpatialAudioSettings& Settings) {
	FMissDirResolution Out;
	Out.Dir = Dir;

	if (Component.ActiveMissDirs.IsEmpty() || Settings.CachedMissExclusionAngleDeg <= 0.f) {
		return Out;
	}

	const int32 MatchedIdx = Math::FindDirectionWithinCone(Dir, Component.ActiveMissDirs, MissMinDot);
	if (MatchedIdx == INDEX_NONE) {
		return Out;
	}
	Out.bIsMissDir = true;

	if (FMath::FRand() <= Settings.MissDirectionCastProbability) {
		// Re-probing the miss: cast the recorded direction itself rather than the neighbour that
		// merely fell inside its cone, so the retry actually retests what failed.
		Out.Dir = Component.ActiveMissDirs[MatchedIdx];
		Out.bDirFixed = true;
		return Out;
	}

	// Not re-probing. Rather than waste the ray, aim it where an edge was found before — but
	// only sometimes, or every skipped miss would pile onto the same few known-good directions.
	if (Component.SuccessfulEdgeDirHints.Num() > 0 && Settings.MissRedirectConeAngleDeg > 0.f
		&& FMath::FRand() < Settings.MissRedirectProbability) {
		const FVector& Hint = Component.SuccessfulEdgeDirHints[
			FMath::RandHelper(Component.SuccessfulEdgeDirHints.Num())];
		Out.Dir = FMath::VRandCone(Hint, FMath::DegreesToRadians(Settings.MissRedirectConeAngleDeg));
		Out.bDirFixed = true;
		Out.bIsMissDir = false;
		return Out;
	}

	Out.bSkip = true;
	return Out;
}

FRandomStream FAsyncCastManager::MakeBiasStream(const FVector& SourcePos, const FVector& ListenerPos, int32 RayIndex) {
	uint32 Seed = HashCombine(GetTypeHash(SourcePos), GetTypeHash(ListenerPos));
	Seed = HashCombine(Seed, static_cast<uint32>(RayIndex));
	return FRandomStream(static_cast<int32>(Seed));
}

// Rejection sampling against ComputeRayDirectionWeight: a direction the weight disfavours is
// redrawn from the seeded stream until one the weight accepts turns up, which concentrates the
// sweep in the lateral band around the listener without ever hard-excluding a direction.
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

	const bool bBias = Settings.bBiasRayDirections && FullCastDistance > 0.f;
	const float MissMinDot = FMath::Cos(FMath::DegreesToRadians(Settings.CachedMissExclusionAngleDeg));
	const float ExclusionMinDot = FMath::Cos(FMath::DegreesToRadians(Settings.CachedEdgeExclusionAngleDeg));

	Component.AsyncRays.Reset(Directions.Num());

	for (int32 i = 0; i < Directions.Num(); ++i) {
		const FMissDirResolution Miss = ResolveMissDirection(Component, Directions[i], MissMinDot, Settings);
		if (Miss.bSkip) {
			continue;
		}

		FVector Dir = Miss.Dir;
		if (!Miss.bDirFixed && bBias) {
			Dir = ApplyLateralBandBias(Component, Dir, ToListenerDir, FullCastDistance, DirectionIndices[i], Settings);
		}

		// A direction a valid cached edge already covers is spent budget — that edge is injected
		// as a confirmed result this sweep and its ray was subtracted from the count.
		if (Math::FindDirectionWithinCone(Dir, Component.CachedEdgeDirs, ExclusionMinDot) != INDEX_NONE) {
			continue;
		}

		FSpatialRayState& Ray = Component.AsyncRays.AddDefaulted_GetRef();
		Ray.Origin = Component.AsyncSourcePos;
		Ray.Dir = Dir;
		Ray.LoSOrigin = Component.AsyncSourcePos;
		Ray.bNextHitCrawls = Miss.bIsMissDir ? false : (i % 2 == 0);
		Ray.bWasMissDir = Miss.bIsMissDir;

		// Nothing has been travelled yet, so the launch segment answers only to the ray's own
		// reach and the straight-flight cap — the path budget starts being spent from here.
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
		Settings.MaxRayDistance)) {
		Component.TargetOcclusion = 0.f;
		Component.TargetVirtualSourceLocation = Component.AsyncSourcePos;
		return;
	}

	ResolveSweepRayBudget(Component, Settings);
	BuildCachedEdgeExclusionDirs(Component, Settings);
	UpdateActiveMissDirs(Component, Settings);
	ResetCycleAccumulator(Component);

	Component.LoSDiffractionPaths.Reset();
	Component.StoredLoSPaths.Reset();

	const float FullCastDistance = FVector::Dist(Component.AsyncSteeringSourcePos, Component.AsyncSteeringListenerPos);
	const FVector ToListenerDir = FullCastDistance > 0.f
		                               ? (Component.AsyncSteeringListenerPos - Component.AsyncSteeringSourcePos) / FullCastDistance
		                               : FVector::ForwardVector;

	SubmitSweepRays(Component, Settings, World, ToListenerDir, FullCastDistance, CycleCount);

	Component.TraceDiag.SweepAsyncRayAccum += Component.AsyncRays.Num();
	Component.bAsyncCastActive = true;
}

// ── TickAsyncCast per-ray phases ─────────────────────────────────────────────

bool FAsyncCastManager::TryProcessMidAirTurn(const USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
                                             bool bBias, float Budget, bool bSegMissed, bool& bAllDone,
                                             const USpatialAudioSettings& Settings) {
	const FVector TurnPoint = Ray.Origin + Ray.Dir * Ray.SegSubmitLen;

	// The last guard prunes a miss that can no longer reach the listener within budget: it takes
	// the terminal branch below instead of spending a bounce on a turn that can never pay off.
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
	                                     !Ray.bLoSFound && bBias, Settings.SurfaceRoughness,
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

void FAsyncCastManager::ProcessRayTermination(const USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
                                              const FTraceDatum& SegData, bool bSegMissed, float Budget,
                                              bool& bAllDone, const USpatialAudioSettings& Settings) {
	if (bSegMissed) {
		// SegSubmitLen caps the terminal point to the distance actually traced — the budget
		// recompute alone would overshoot when MaxStraightFlightDistance clamped the segment,
		// putting the terminal point (and its LoS probes) in space the trace never verified.
		const float RemainingBudget = FMath::Max(0.f, FMath::Min(
			FMath::Min(Component.MaxRayDistance * Settings.RayLengthMultiplier, Ray.SegSubmitLen),
			Budget - Ray.CumulativeDistance));
		Ray.TerminalPoint = Ray.Origin + Ray.Dir * RemainingBudget;
		Ray.bHasTerminalPoint = true;

		if (ShouldDrawFlightPaths(Component)) {
			DrawDebugLine(World, Ray.Origin, Ray.TerminalPoint,
			              FColor::Red, false, Settings.DebugLineDuration, 0, 0.5f);
		}

		if (!Ray.bLoSFound) {
			const float SegLen = FVector::Dist(Ray.Origin, Ray.TerminalPoint);
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
	// Every step's three questions are submitted up front, in one batch, so the whole crawl costs
	// one frame of latency instead of one per step (EvaluateCrawlSteps reads them back in order
	// and stops at the first step that answers).
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
                                                      UWorld* World, const FHitResult& Hit, bool bBias, float Budget,
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

	const bool bDoCrawl = Settings.bEnableSurfaceCrawl && Ray.bNextHitCrawls
		&& (!Settings.bCrawlOnFirstBounceOnly || Ray.Bounce == 0);

	if (bDoCrawl && TrySetupSurfaceCrawl(Component, Ray, World, Hit, Settings)) {
		bAllDone = false;
		return;
	}

	Ray.Dir = Math::ComputeBouncedDirection(Ray.Dir, Hit.Normal, !Ray.bLoSFound && bBias,
	                                  Hit.Location, Component.AsyncSteeringListenerPos, Settings.SurfaceRoughness,
	                                  Settings.BounceListenerBias);
	Ray.Origin = Hit.Location + Hit.Normal * Settings.RaySurfaceBias;
	++Ray.Bounce;
	Ray.BounceWaypoints.Add({Ray.Origin, Ray.CumulativeDistance});

	if (Settings.bEnableSurfaceCrawl) {
		Ray.bNextHitCrawls = !Ray.bNextHitCrawls;
	}

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
                                      bool bBias, float Budget, bool& bAllDone,
                                      const USpatialAudioSettings& Settings) {
	const bool bLoSWasFound = Ray.bLoSFound;
	DrainPendingLoSProbes(Component, Ray, World, Component.AsyncListenerPos);
	if (!bLoSWasFound && Ray.bLoSFound && ShouldDrawFlightPaths(Component)) {
		DrawDebugSphere(World, Ray.LoSOrigin, 10.f, 8, FColor::Green, false, Settings.DebugLineDuration, SDPG_Foreground, 2.f);
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
		ProcessCrawlBatch(Component, Ray, World, bBias, Budget, bAllDone, Settings);
		return;
	}

	FTraceDatum SegData;
	if (!World->QueryTraceData(Ray.SegHandle, SegData)) {
		bAllDone = false;
		return;
	}

	const bool bSegMissed = SegData.OutHits.IsEmpty();

	if (TryProcessMidAirTurn(Component, Ray, World, bBias, Budget, bSegMissed, bAllDone, Settings)) {
		return;
	}

	if (bSegMissed || Ray.Bounce >= Component.AsyncMaxBounces) {
		ProcessRayTermination(Component, Ray, World, SegData, bSegMissed, Budget, bAllDone, Settings);
	}
	else {
		ProcessRayBounceContinuation(Component, Ray, World, SegData.OutHits[0], bBias, Budget, bAllDone, Settings);
	}
}

void FAsyncCastManager::TickAsyncCast(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings) {
	SCOPE_CYCLE_COUNTER(STAT_SpatialAudio_AsyncTick);

	UWorld* World = Component.GetWorld();
	if (!World) {
		return;
	}

	const bool bBias = Settings.bBiasRayDirections;
	const float Budget = Component.MaxRayDistance * Settings.TotalPathBudgetMultiplier;

	bool bAllDone = true;

	for (FSpatialRayState& Ray : Component.AsyncRays) {
		if (Ray.bDone) {
			continue;
		}
		TickSingleRay(Component, Ray, World, bBias, Budget, bAllDone, Settings);
	}

	if (bAllDone) {
		SubmitFinalizeBatch(Component, Settings);
	}
}

void FAsyncCastManager::DrainPendingLoSProbes(const USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World, const FVector& ListenerPos) {
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
			// Reverse sanity trace: a probe origin inside geometry exits silently on the forward
			// trace, but the listener->origin trace hits the geometry's outer face and rejects it.
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
		// Cumulative distance stops at the edge point (BestCumDist) — does not include the final
		// LoS-confirmation leg from the edge to the listener. That leg is Leg2, not part of the
		// traveled source->edge path this value is meant to represent.
		Ray.LoSCumulativeDistance = BestCumDist;
	}
}

// ── ProcessCrawlBatch phases ─────────────────────────────────────────────────

bool FAsyncCastManager::AreCrawlTracesReady(UWorld* World, FSpatialRayState& Ray, FTraceDatum& OutRangeData) {
	if (!World->QueryTraceData(Ray.CrawlRangeHandle, OutRangeData)) {
		return false;
	}
	for (FSpatialRayState::FAsyncCrawlStepProbe& StepProbe : Ray.CrawlStepProbes) {
		// The datum is discarded — this only asks whether all three traces have landed yet.
		FTraceDatum Ready;
		if (!World->QueryTraceData(StepProbe.BackHandle, Ready) ||
			!World->QueryTraceData(StepProbe.LoSHandle, Ready) ||
			!World->QueryTraceData(StepProbe.PerpHandle, Ready)) {
			return false;
		}
	}
	return true;
}

FAsyncCastManager::FCrawlStepResult FAsyncCastManager::EvaluateCrawlSteps(
	const USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World, int32 Limit,
	const USpatialAudioSettings& Settings) {
	FCrawlStepResult Result;

	// Each step along the surface asks three questions in order: has the listener come into view,
	// has a wall closed off the crawl (an inside corner), and has the surface fallen away behind
	// us (an outside corner — the diffraction edge the crawl is hunting for).
	for (int32 StepIdx = 0; StepIdx < Limit; ++StepIdx) {
		FSpatialRayState::FAsyncCrawlStepProbe& StepProbe = Ray.CrawlStepProbes[StepIdx];

		if (!Ray.bLoSFound
			&& FVector::DotProduct(Component.AsyncListenerPos - StepProbe.StepPos, Ray.CrawlHitNormal) > 0.f) {
			if (Component.bDrawDebugRays && Component.bShowLoSChecks) {
				DrawDebugLine(World, StepProbe.StepPos, Component.AsyncListenerPos, FColor(160, 0, 255),
				              false, Settings.DebugLineDuration, 0, 0.5f);
			}
			FTraceDatum LoSData;
			World->QueryTraceData(StepProbe.LoSHandle, LoSData);
			if (IsTraceClear(LoSData)) {
				if (FHitResult SanityHit; !Component.TraceLine(World, SanityHit, Component.AsyncListenerPos, StepProbe.StepPos)) {
					Ray.bLoSFound = true;
					Ray.LoSBounces = FMath::Max(1, Ray.Bounce);
					Ray.LoSOrigin = StepProbe.StepPos;
					// Stops at the edge point (StepCumDist) — excludes the edge->listener
					// confirmation leg, same reasoning as DrainPendingLoSProbes above.
					Ray.LoSCumulativeDistance = StepProbe.StepCumDist;
					if (Component.bDrawDebugRays && Component.bShowSurfaceCrawl) {
						DrawDebugSphere(World, StepProbe.StepPos, 10.f, 8, FColor::Green,
						                false, Settings.DebugLineDuration, SDPG_Foreground, 2.f);
					}
				}
			}
		}

		// Forward one step along the surface: a hit is a wall across the crawl's path, so the
		// crawl ends there and the ray reflects off it instead of rounding anything.
		FTraceDatum PerpData;
		World->QueryTraceData(StepProbe.PerpHandle, PerpData);
		if (!IsTraceClear(PerpData)) {
			const FHitResult& PerpHit = PerpData.OutHits[0];
			Result.EdgePoint = PerpHit.Location + PerpHit.Normal * Ray.CrawlNudgeDist;
			Result.EdgeDir = Math::ReflectDirection(Ray.CrawlDir, PerpHit.Normal);
			Result.CrawlDist = static_cast<float>(StepIdx) * Ray.CrawlStepSz
				+ FVector::Dist(StepProbe.StepPos, PerpHit.Location);
			Result.PerpNormal = PerpHit.Normal;
			Result.bPerpWallHit = true;
			Result.bSucceeded = true;
			Result.FoundAtStep = StepIdx;
			break;
		}

		// Back into the surface being crawled: a CLEAR trace means there is no longer anything
		// behind this step, so the crawl has just walked off the edge. That step point is the
		// diffraction corner, and the ray leaves it aimed around the obstruction.
		FTraceDatum BackData;
		World->QueryTraceData(StepProbe.BackHandle, BackData);
		if (IsTraceClear(BackData)) {
			const FVector ToListener = (Component.AsyncSteeringListenerPos - StepProbe.StepPos).GetSafeNormal();
			Result.EdgePoint = StepProbe.StepPos;
			Result.EdgeDir = SelectEdgeDirection(Ray.CrawlHitNormal, ToListener);
			Result.CrawlDist = static_cast<float>(StepIdx + 1) * Ray.CrawlStepSz;
			Result.bSucceeded = true;
			Result.FoundAtStep = StepIdx;
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
		const FColor StepColor = !bIsEdgeStep       ? FColor::White
		                       : Result.bPerpWallHit ? FColor::Orange
		                       :                       FColor::Yellow;
		const float Radius = bIsEdgeStep ? 12.f : 4.f;

		DrawDebugSphere(World, StepProbe.StepPos, Radius, 6, StepColor,
		                false, Settings.DebugLineDuration, SDPG_Foreground, 1.f);
		// Cyan marks crawl movement; flight segments stay white.
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
                                         const FCrawlStepResult& Result, bool bBias,
                                         const USpatialAudioSettings& Settings) {
	Ray.CrawlStepProbes.Empty();
	Ray.BatchPhase = FSpatialRayState::ERayBatchPhase::None;

	if (Result.bSucceeded) {
		Ray.CumulativeDistance += Result.CrawlDist;
		Ray.Dir = Result.EdgeDir;
		// A perp-wall exit reflects off a wall the crawl ran into, so it takes the surface bias
		// off that wall; a free-edge exit just carries on from the step point, which the crawl
		// already held clear of the surface it was following.
		Ray.Origin = Result.bPerpWallHit
			             ? Result.EdgePoint + Result.PerpNormal * Settings.RaySurfaceBias
			             : Result.EdgePoint;
		++Ray.Bounce;
		Ray.BounceWaypoints.Add({Ray.Origin, Ray.CumulativeDistance});
	}
	else {
		Ray.Dir = Math::ComputeBouncedDirection(Ray.CrawlInDir, Ray.CrawlHitNormal, !Ray.bLoSFound && bBias,
		                                  Ray.CrawlHitLoc, Component.AsyncSteeringListenerPos, Settings.SurfaceRoughness,
		                                  Settings.BounceListenerBias);
		Ray.Origin = Ray.CrawlHitLoc + Ray.CrawlHitNormal * Settings.RaySurfaceBias;
		++Ray.Bounce;
	}
}

void FAsyncCastManager::ProcessCrawlBatch(const USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
                                          bool bBias, float Budget, bool& bAllDone,
                                          const USpatialAudioSettings& Settings) {
	FTraceDatum RangeData;
	if (!AreCrawlTracesReady(World, Ray, RangeData)) {
		bAllDone = false;
		return;
	}

	// One trace down the whole crawl path bounds it up front: anything past where that trace hit
	// is inside geometry, so those steps' probes are read back but never evaluated.
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

	ApplyCrawlResult(Component, Ray, Result, bBias, Settings);

	if (Settings.bEnableSurfaceCrawl) {
		Ray.bNextHitCrawls = !Ray.bNextHitCrawls;
	}

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

TArray<FVector> FAsyncCastManager::BuildEdgeDirHints(const TArray<FStoredLoSPath>& StoredPaths, const FVector& SourcePos) {
	TArray<FVector> Hints;
	Hints.Reserve(StoredPaths.Num());
	for (const FStoredLoSPath& StoredPath : StoredPaths) {
		const FVector ToEdge = StoredPath.LoSOrigin - SourcePos;
		const float DistToEdge = ToEdge.Size();
		if (DistToEdge > 1.f) {
			Hints.Add(ToEdge / DistToEdge);
		}
	}
	return Hints;
}

FVector FAsyncCastManager::ComputeMidAirTurnDirection(const FVector& InDir, const FVector& TurnPoint,
                                                      const FVector& ListenerPos, bool bApplyBias,
                                                      float SurfaceRoughness, float BounceListenerBias) {
	if (SurfaceRoughness <= 0.f && BounceListenerBias <= 0.f) {
		// With no scatter and no bias the lerp below returns InDir unchanged — the ray would burn a
		// bounce flying straight. Turn 90° instead, at an angle seeded from the turn point so a
		// stationary scene replays the identical direction every sweep (MakeBiasStream pattern).
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

		// The budget sum only grows along the segment, so the first rejection ends the walk.
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

// ── SubmitFinalizeBatch phases ───────────────────────────────────────────────

// The reverse trace catches an endpoint sitting inside a mesh (traces that start inside
// geometry exit without a blocking hit).
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
	// Earliest waypoint first: the further back down the path a shortcut reaches, the more of the
	// traveled detour it replaces.
	for (int32 AnchorIdx = 0; AnchorIdx < SearchLimit; ++AnchorIdx) {
		if (HasClearShortcut(Component, World, FromPoint, Waypoints[AnchorIdx].Pos)) {
			return AnchorIdx;
		}
	}
	return INDEX_NONE;
}

// Leg1 is the source->edge leg of a diffraction path; Leg2 (edge->listener) is the engine
// attenuation's job. The route a ray actually flew overstates Leg1 — crawl steps hug walls and
// bounces detour — whereas sound shortcuts straight between any two points that can see each
// other. So the traveled route is pulled taut before its length is used.
//
// Starting at the edge point, find the earliest anchor — the source, else a bounce waypoint —
// with a clear straight segment back to the current point, hop there, and repeat until the
// source is reached. Every link in that chain is a verified straight line. When nothing is
// visible, exactly ONE raw traveled hop is consumed (that link stays unverified: it is the real
// crawl or bounce leg, not a straight line) and pulling continues from there — so a single
// blocked corner cannot also swallow a shortcut genuinely available beyond it, such as a second
// opening past a second corner.
//
// OutPath is the polyline the returned distance was measured along, source first.
// OutSegmentVerified marks, per segment, whether it is one of those verified straight lines.
// False means the straight shortcut was traced and came back blocked — which is often blocked
// by design rather than by a geometry change, so callers must not read it as a fault.
float FAsyncCastManager::ComputeStringPulledLeg1(const USpatialAudioComponent& Component, const UWorld* World,
                                                 const FSpatialRayState& Ray, const FVector& SourcePos,
                                                 TArray<FVector>& OutPath, TArray<bool>& OutSegmentVerified) {
	TArray<FVector> ReversePath;
	TArray<bool> ReverseSegmentVerified;
	ReversePath.Add(Ray.LoSOrigin);

	// The pull walks source-ward from the edge point. RemainingAnchors bounds the waypoints still
	// on the source side of PullPoint — the only ones a shortcut may legally reach back to.
	FVector PullPoint = Ray.LoSOrigin;
	float PullPointCumDist = Ray.LoSCumulativeDistance;
	int32 RemainingAnchors = CountPrefixAnchorWaypoints(Ray.BounceWaypoints, Ray.LoSCumulativeDistance);
	float PulledDist = 0.f;

	// RemainingAnchors strictly decreases (or the loop exits) every iteration, so this terminates.
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
			// No waypoints left to try and the source still isn't visible: close the final gap
			// with the traveled distance, same fallback a total failure always used.
			PulledDist += PullPointCumDist;
			ReverseSegmentVerified.Add(false);
			ReversePath.Add(SourcePos);
			break;
		}

		// Nothing visible from PullPoint — not even the immediately preceding traveled waypoint.
		// Consume that one raw hop as an unverified link and keep pulling from there.
		const FSpatialRayState::FBounceWaypoint& Preceding = Ray.BounceWaypoints[RemainingAnchors - 1];
		PulledDist += PullPointCumDist - Preceding.CumDist;
		ReverseSegmentVerified.Add(false);
		PullPoint = Preceding.Pos;
		PullPointCumDist = Preceding.CumDist;
		--RemainingAnchors;
		ReversePath.Add(PullPoint);
	}

	PulledDist = FMath::Min(PulledDist, Ray.LoSCumulativeDistance);

	// Built walking backwards from the edge; callers want it source-first.
	OutPath = MoveTemp(ReversePath);
	Algo::Reverse(OutPath);
	OutSegmentVerified = MoveTemp(ReverseSegmentVerified);
	Algo::Reverse(OutSegmentVerified);
	return PulledDist;
}

void FAsyncCastManager::SubmitFinalizeBatch(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings) {
	const UWorld* World = Component.GetWorld();

	// Pre-warm casts fire while the source is still partially visible, so rays reaching the
	// listener directly are expected — masking the flag keeps the refine probes (the whole
	// point of the pre-sweep) and stops readback from wiping the freshly warmed cache.
	const bool bDirectLoSFound = !Component.bPreSweepCast && Math::HasAnyDirectLoS(Component.AsyncRays);

	const FCachedPointAccum Accum = AccumulateCachedPoints(
		Component.PendingValidCachedPoints, Settings);

	// Only these three take a further contribution from the rays below; the rest of the cached
	// -point accumulation passes straight through to the batch.
	int32 RaysReached = Accum.RaysReached;
	int32 TotalLoSBounces = 0;
	float MinLoSDist = Accum.MinLoSDist;

	Component.Finalize.RefineProbes.Reset();

	for (int32 RayIdx = 0; RayIdx < Component.AsyncRays.Num(); ++RayIdx) {
		const FSpatialRayState& Ray = Component.AsyncRays[RayIdx];

		if (Ray.bLoSFound) {
			++RaysReached;
			TotalLoSBounces += Ray.LoSBounces;
			MinLoSDist = FMath::Min(MinLoSDist, Ray.LoSCumulativeDistance);
		}

		UpdateMissDirState(
			Ray,
			Component.AsyncSourcePos,
			Component.AsyncListenerPos,
			Component.CachedEdgeDirs,
			Component.CachedMissDirs,
			Component.SweepScheduling.bGeometryChangeDetected,
			Settings);

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
	Component.Finalize.TotalLoSBounces = TotalLoSBounces;
	Component.Finalize.MinLoSDist = MinLoSDist;
	Component.Finalize.WeightedPosSum = Accum.WeightedPos;
	Component.Finalize.TotalWeight = Accum.TotalWeight;
	Component.Finalize.WeightedDistSum = Accum.WeightedDist;
	Component.Finalize.bDirectLoSFound = bDirectLoSFound;
	Component.Finalize.bPending = true;
	Component.bAsyncCastActive = false;
	Component.AsyncRays.Reset();
}
