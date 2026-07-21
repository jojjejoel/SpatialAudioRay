#include "Audio/AsyncCastManager.h"
#include "Audio/SpatialAudioComponent.h"
#include "Audio/Math.h"

#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
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

// The reverse trace catches an endpoint sitting inside a mesh (traces that start inside
// geometry exit without a blocking hit).
bool FAsyncCastManager::HasClearShortcut(const USpatialAudioComponent& Component, const UWorld* World,
                                         const FVector& Edge, const FVector& Anchor) {
	FHitResult Hit;
	return !Component.TraceLine(World, Hit, Edge, Anchor)
		&& !Component.TraceLine(World, Hit, Anchor, Edge);
}

// The traveled path (crawl steps + bounce detours) overestimates the acoustic source->edge
// distance — sound shortcuts straight between points that can "see" each other. Multi-level
// string pull: starting at the edge point, find the first anchor (source, then waypoints in
// path order) with a clear straight segment to the current point, hop there, and repeat from
// that anchor until the source is reached — every link in the chain is a verified straight
// segment. A level where nothing is visible keeps the traveled route for the remaining prefix.
// OutPath receives the polyline the returned distance was measured along (source ... edge).
// OutVerifiedFrom is the OutPath index where the HasClearShortcut-verified chain begins:
// segments from there to the edge are known-clear straight lines; anything before it is
// traveled route whose straight segments were never traced (and may be blocked by design).
float FAsyncCastManager::ComputeStringPulledLeg1(const USpatialAudioComponent& Component, const UWorld* World,
                                                 const FSpatialRayState& Ray, const FVector& SourcePos,
                                                 TArray<FVector>& OutPath, int32& OutVerifiedFrom) {
	// Waypoints at/past the LoS origin can't be prefix anchors.
	int32 NumUsable = 0;
	while (NumUsable < Ray.BounceWaypoints.Num()
		&& Ray.BounceWaypoints[NumUsable].CumDist < Ray.LoSCumulativeDistance) {
		++NumUsable;
	}

	TArray<FVector> ReversePath;
	ReversePath.Add(Ray.LoSOrigin);

	FVector Current = Ray.LoSOrigin;
	int32 CurrentIdx = NumUsable;
	float CurrentCumDist = Ray.LoSCumulativeDistance;
	float ChainDist = 0.f;
	bool bReachedSource = false;

	// CurrentIdx strictly decreases each hop, so the loop terminates.
	while (true) {
		if (HasClearShortcut(Component, World, Current, SourcePos)) {
			ChainDist += FVector::Dist(Current, SourcePos);
			bReachedSource = true;
			break;
		}
		int32 FoundIdx = INDEX_NONE;
		for (int32 i = 0; i < CurrentIdx; ++i) {
			if (HasClearShortcut(Component, World, Current, Ray.BounceWaypoints[i].Pos)) {
				FoundIdx = i;
				break;
			}
		}
		if (FoundIdx == INDEX_NONE) {
			break;
		}
		ChainDist += FVector::Dist(Current, Ray.BounceWaypoints[FoundIdx].Pos);
		Current = Ray.BounceWaypoints[FoundIdx].Pos;
		CurrentIdx = FoundIdx;
		CurrentCumDist = Ray.BounceWaypoints[FoundIdx].CumDist;
		ReversePath.Add(Current);
	}

	float Result;
	if (bReachedSource) {
		Result = ChainDist;
		OutVerifiedFrom = 0;
	}
	else {
		// Nothing visible from Current: the traveled distance covers the remaining prefix, and
		// the polyline follows the traveled waypoints back to the source. These prefix segments
		// are NOT straight clear lines — crawl legs hug the wall between recorded turn points —
		// so consumers re-tracing the polyline must skip them (blocked at discovery already,
		// a blocked re-trace there says nothing about geometry having changed).
		Result = ChainDist + CurrentCumDist;
		for (int32 i = CurrentIdx - 1; i >= 0; --i) {
			ReversePath.Add(Ray.BounceWaypoints[i].Pos);
		}
		OutVerifiedFrom = CurrentIdx + 1;
	}
	Result = FMath::Min(Result, Ray.LoSCumulativeDistance);

	ReversePath.Add(SourcePos);
	OutPath.Reset(ReversePath.Num());
	for (int32 i = ReversePath.Num() - 1; i >= 0; --i) {
		OutPath.Add(ReversePath[i]);
	}
	return Result;
}

FRandomStream FAsyncCastManager::MakeBiasStream(const FVector& SourcePos, const FVector& ListenerPos, int32 RayIndex) {
	uint32 Seed = HashCombine(GetTypeHash(SourcePos), GetTypeHash(ListenerPos));
	Seed = HashCombine(Seed, static_cast<uint32>(RayIndex));
	return FRandomStream(static_cast<int32>(Seed));
}

void FAsyncCastManager::StartAsyncFullCast(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings) {
	UWorld* World = Component.GetWorld();
	AActor* Owner = Component.GetOwner();
	if (!World || !Owner) {
		return;
	}

	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC || !PC->GetPawn()) {
		return;
	}

	const int32 CycleCount = FMath::Max(1, Settings.FullSweepCycleCount);

	Component.TraceDiag.SweepTraceAccum = 0;
	Component.TraceDiag.SweepFrameAccum = 0;
	Component.bPreSweepCast = Component.IsPreSweepActive();
	Component.AsyncSourcePos = Owner->GetActorLocation();
	Component.AsyncListenerPos = PC->GetPawn()->GetActorLocation();
	// Velocity-led STEERING positions: aim rays where source/listener are heading — or, within
	// the lead time of losing LoS, where they came from (see ComputeSteeringLead). Probes/gates
	// keep verifying against the actual positions.
	Component.AsyncSteeringSourcePos = Component.AsyncSourcePos
		+ Component.ComputeSteeringLead(Component.VelocityScaling.SmoothedSourceVelocity, Settings);
	Component.AsyncSteeringListenerPos = Component.AsyncListenerPos
		+ Component.ComputeSteeringLead(Component.VelocityScaling.SmoothedListenerVelocity, Settings);

	if (FVector::DistSquared(Component.AsyncSourcePos, Component.AsyncListenerPos) > FMath::Square(
		Settings.MaxRayDistance)) {
		Component.TargetOcclusion = 0.f;
		Component.TargetVirtualSourceLocation = Component.AsyncSourcePos;
		return;
	}

	int32 ScaledRayCount;
	Component.GetEffectiveRayCounts(ScaledRayCount, Component.CurrentPriority);
	Component.AsyncMaxBounces = FMath::Max(Settings.MinMaxBounces,
	                                        FMath::RoundToInt(
		                                        Settings.MaxBounces * Component.CurrentPriority));
	Component.AsyncTotalRays = ScaledRayCount;

	Component.PendingValidCachedPoints.Reset();
	if (Settings.bCacheEdgePoints) {
		Component.PendingValidCachedPoints.Append(Component.CachedEdgePoints);
	}

	Component.AsyncActualRays = FMath::Max(0, ScaledRayCount - Component.PendingValidCachedPoints.Num());
	Component.TraceDiag.SweepAsyncRayAccum = 0;
	Component.TraceDiag.LastSweepCachedReplaced = Component.PendingValidCachedPoints.Num();

	Component.CachedEdgeDirs.Reset();
	if (Settings.bCacheEdgePoints && Settings.CachedEdgeExclusionAngleDeg > 0.f && !Settings.IsDirectionSkippingDisabled()) {
		const float MoveThreshSq = FMath::Square(Settings.CachedEdgeUpdateMoveThreshold);
		for (const FCachedEdgePoint& EP : Component.PendingValidCachedPoints) {
			if (EP.bEvicting) {
				continue;
			}
			const bool bSrcMoved = FVector::DistSquared(Component.AsyncSourcePos, EP.CapturedSourcePos) >
				MoveThreshSq;
			const bool bLisMoved = FVector::DistSquared(Component.AsyncListenerPos, EP.CapturedListenerPos) >
				MoveThreshSq;
			if (bSrcMoved || bLisMoved) {
				continue;
			}
			const FVector ToEdge = EP.EdgePoint - Component.AsyncSourcePos;
			const float Dist = ToEdge.Size();
			if (Dist > 1.f) {
				Component.CachedEdgeDirs.Add(ToEdge / Dist);
			}
		}
	}

	Component.ActiveMissDirs.Reset();
	if (Settings.bCacheEdgePoints && Settings.CachedMissExclusionAngleDeg > 0.f && !Settings.IsDirectionSkippingDisabled()) {
		const float MoveThreshSq = FMath::Square(Settings.CachedEdgeUpdateMoveThreshold);
		for (int32 i = Component.CachedMissDirs.Num() - 1; i >= 0; --i) {
			const FCachedMissDir& MD = Component.CachedMissDirs[i];
			if (FVector::DistSquared(Component.AsyncSourcePos, MD.CapturedSourcePos) > MoveThreshSq) {
				Component.CachedMissDirs.RemoveAt(i);
				continue;
			}
			const bool bLisMoved = FVector::DistSquared(Component.AsyncListenerPos, MD.CapturedListenerPos) >
				MoveThreshSq;
			if (!bLisMoved) {
				Component.ActiveMissDirs.Add(MD.Dir);
			}
		}
	}

	Component.CycleAccum.RaysReached = 0;
	Component.CycleAccum.LoSBounces = 0;
	Component.CycleAccum.MinLoSDist = TNumericLimits<float>::Max();
	Component.CycleAccum.WeightedPos = FVector::ZeroVector;
	Component.CycleAccum.TotalWeight = 0.f;
	Component.CycleAccum.WeightedDist = 0.f;
	Component.CycleAccum.bDirectLoSFound = false;

	Component.LoSDiffractionPaths.Reset();
	Component.StoredLoSPaths.Reset();

	const float FullCastDistance = FVector::Dist(Component.AsyncSteeringSourcePos, Component.AsyncSteeringListenerPos);
	const FVector ToListenerDir = FullCastDistance > 0.f
		                               ? (Component.AsyncSteeringListenerPos - Component.AsyncSteeringSourcePos) / FullCastDistance
		                               : FVector::ForwardVector;

	const TArray<FVector> AllDirections = Math::GenerateFibonacciDirections(ScaledRayCount, ToListenerDir);
	TArray<FVector> Directions;
	TArray<int32> DirectionIndices;
	Directions.Reserve(AllDirections.Num() / CycleCount + 1);
	DirectionIndices.Reserve(AllDirections.Num() / CycleCount + 1);
	for (int32 i = Component.CycleAccum.Index; i < AllDirections.Num(); i += CycleCount) {
		Directions.Add(AllDirections[i]);
		DirectionIndices.Add(i);
	}

	const bool bBias = Settings.bBiasRayDirections && FullCastDistance > 0.f;

	Component.AsyncRays.Reset(Directions.Num());

	for (int32 i = 0; i < Directions.Num(); ++i) {
		FVector Dir = Directions[i];

		bool bIsMissDir = false;
		bool bDirFixed = false;
		if (Component.ActiveMissDirs.Num() > 0 && Settings.CachedMissExclusionAngleDeg > 0.f) {
			const float MissMinDot = FMath::Cos(
				FMath::DegreesToRadians(Settings.CachedMissExclusionAngleDeg));
			int32 MatchedMissDirIdx = -1;
			for (int32 mi = 0; mi < Component.ActiveMissDirs.Num(); ++mi) {
				if (FVector::DotProduct(Dir, Component.ActiveMissDirs[mi]) >= MissMinDot) {
					bIsMissDir = true;
					MatchedMissDirIdx = mi;
					break;
				}
			}
			if (bIsMissDir && FMath::FRand() > Settings.MissDirectionCastProbability) {
				if (Component.SuccessfulEdgeDirHints.Num() > 0
					&& Settings.MissRedirectConeAngleDeg > 0.f
					&& FMath::FRand() < Settings.MissRedirectProbability) {
					const FVector& Hint = Component.SuccessfulEdgeDirHints[FMath::RandHelper(
						Component.SuccessfulEdgeDirHints.Num())];
					Dir = FMath::VRandCone(Hint, FMath::DegreesToRadians(Settings.MissRedirectConeAngleDeg));
					bDirFixed = true;
					bIsMissDir = false;
				}
				else {
					continue;
				}
			}
			else if (bIsMissDir) {
				Dir = Component.ActiveMissDirs[MatchedMissDirIdx];
				bDirFixed = true;
			}
		}

		if (!bDirFixed && bBias) {
			const float Radius = Settings.DirectLoSSampleRadius;
			const float FibWeight = Math::ComputeRayDirectionWeight(
				Dir, ToListenerDir, Component.LastDirectLoSFraction, Radius, FullCastDistance);

			FRandomStream BiasStream = FAsyncCastManager::MakeBiasStream(
				Component.AsyncSourcePos, Component.AsyncListenerPos, DirectionIndices[i]);

			if (BiasStream.FRand() > FibWeight) {
				for (int32 Attempt = 0; Attempt < 30; ++Attempt) {
					const FVector Candidate = BiasStream.VRand();
					float w = Math::ComputeRayDirectionWeight(
						Candidate, ToListenerDir, Component.LastDirectLoSFraction, Radius, FullCastDistance);
					if (BiasStream.FRand() < FMath::Min(w, 1.f)) {
						Dir = Candidate;
						break;
					}
				}
			}
		}

		if (Component.CachedEdgeDirs.Num() > 0) {
			const float MinDot = FMath::Cos(
				FMath::DegreesToRadians(Settings.CachedEdgeExclusionAngleDeg));
			bool bExcluded = false;
			for (const FVector& CachedDir : Component.CachedEdgeDirs) {
				if (FVector::DotProduct(Dir, CachedDir) >= MinDot) {
					bExcluded = true;
					break;
				}
			}
			if (bExcluded) {
				continue;
			}
		}

		FSpatialRayState& Ray = Component.AsyncRays.AddDefaulted_GetRef();
		Ray.Origin = Component.AsyncSourcePos;
		Ray.Dir = Dir;
		Ray.LoSOrigin = Component.AsyncSourcePos;
		Ray.bNextHitCrawls = bIsMissDir ? false : (i % 2 == 0);
		Ray.bWasMissDir = bIsMissDir;

		float SegLen = Component.MaxRayDistance * Settings.RayLengthMultiplier;
		if (Settings.MaxStraightFlightDistance > 0.f) {
			SegLen = FMath::Min(SegLen, Settings.MaxStraightFlightDistance);
		}
		Ray.SegSubmitLen = SegLen;
		Ray.SegHandle = Component.SubmitAsyncTrace(World, Ray.Origin, Ray.Origin + Ray.Dir * SegLen);
	}

	Component.TraceDiag.SweepAsyncRayAccum += Component.AsyncRays.Num();
	Component.bAsyncCastActive = true;
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

		const bool bLoSWasFound = Ray.bLoSFound;
		DrainPendingLoSProbes(Component, Ray, World, Component.AsyncListenerPos);
		if (!bLoSWasFound && Ray.bLoSFound
			&& Component.bDrawDebugRays && (Component.bShowBounceRays || Component.bShowSurfaceCrawl)) {
			DrawDebugSphere(World, Ray.LoSOrigin, 10.f, 8, FColor::Green, false, Settings.DebugLineDuration, SDPG_Foreground, 2.f);
		}

		if (Ray.bLoSFound) {
			if (Ray.PendingLoSProbes.IsEmpty()) {
				Ray.bDone = true;
			}
			else {
				Ray.bTerminalLoSPending = true;
				bAllDone = false;
			}
			continue;
		}

		if (Ray.bTerminalLoSPending) {
			if (Ray.PendingLoSProbes.IsEmpty()) {
				Ray.bTerminalLoSPending = false;
				Ray.bDone = true;
			}
			else {
				bAllDone = false;
			}
			continue;
		}

		if (Ray.BatchPhase == FSpatialRayState::ERayBatchPhase::CrawlBatch) {
			ProcessCrawlBatch(Component, Ray, World, bBias, Budget, bAllDone, Settings);
			continue;
		}

		FTraceDatum SegData;
		if (!World->QueryTraceData(Ray.SegHandle, SegData)) {
			bAllDone = false;
			continue;
		}

		const bool bSegMissed = SegData.OutHits.IsEmpty();

		// The last guard is the best-case prune: past that bound no future LoS probe of this ray
		// can pass the CumDist + dist(point, listener) <= Budget gate (triangle inequality — the
		// sum only grows), so a hopeless miss takes the terminal branch below instead of turning.
		if (bSegMissed && Settings.MaxStraightFlightDistance > 0.f
			&& Ray.Bounce < Component.AsyncMaxBounces
			&& Budget - (Ray.CumulativeDistance + Ray.SegSubmitLen) >= 1.f
			&& Ray.CumulativeDistance + Ray.SegSubmitLen
			   + FVector::Dist(Ray.Origin + Ray.Dir * Ray.SegSubmitLen, Component.AsyncListenerPos) <= Budget) {
			const FVector TurnPoint = Ray.Origin + Ray.Dir * Ray.SegSubmitLen;

			if (!Ray.bLoSFound) {
				SubmitSegmentLoSProbes(Component, Ray, World, Ray.Origin, Ray.Dir, Ray.SegSubmitLen, Budget, Settings);
			}

			if (Component.bDrawDebugRays && (Component.bShowBounceRays || Component.bShowSurfaceCrawl)) {
				DrawDebugLine(World, Ray.Origin, TurnPoint, FColor::White,
				              false, Settings.DebugLineDuration, 0, 1.f);
				DrawDebugSphere(World, TurnPoint, 5.f, 6, FColor::White,
				                false, Settings.DebugLineDuration, SDPG_Foreground, 1.f);
			}

			Ray.CumulativeDistance += Ray.SegSubmitLen;
			Ray.Dir = ComputeMidAirTurnDirection(Ray.Dir, TurnPoint, Component.AsyncSteeringListenerPos,
			                                     !Ray.bLoSFound && bBias, Settings.SurfaceRoughness,
			                                     Settings.BounceListenerBias);
			Ray.Origin = TurnPoint;
			++Ray.Bounce;
			Ray.BounceWaypoints.Add({TurnPoint, Ray.CumulativeDistance});

			if (!Ray.bLoSFound) {
				const float DirectToListener = FVector::Dist(Ray.Origin, Component.AsyncListenerPos);
				if (Ray.CumulativeDistance + DirectToListener <= Budget) {
					FSpatialRayState::FAsyncLoSProbe TurnProbe;
					TurnProbe.LoSHandle = Component.SubmitAsyncTrace(World, Ray.Origin, Component.AsyncListenerPos);
					TurnProbe.SamplePos = Ray.Origin;
					TurnProbe.CumDist = Ray.CumulativeDistance;
					TurnProbe.BounceAtSubmit = Ray.Bounce;
					Ray.PendingLoSProbes.Add(MoveTemp(TurnProbe));
					if (Component.bDrawDebugRays && Component.bShowLoSChecks) {
						DrawDebugLine(World, Ray.Origin, Component.AsyncListenerPos, FColor(160, 0, 255),
						              false, Settings.DebugLineDuration, 0, 0.5f);
					}
				}
			}

			const float Remain = FMath::Min(FMath::Min(Component.MaxRayDistance, Settings.MaxStraightFlightDistance),
			                                Budget - Ray.CumulativeDistance);
			Ray.SegSubmitLen = Remain;
			Ray.SegHandle = Component.SubmitAsyncTrace(World, Ray.Origin, Ray.Origin + Ray.Dir * Remain);
			bAllDone = false;
			continue;
		}

		if (bSegMissed || Ray.Bounce >= Component.AsyncMaxBounces) {
			if (bSegMissed) {
				// SegSubmitLen caps the terminal point to the distance actually traced — the budget
				// recompute alone would overshoot when MaxStraightFlightDistance clamped the segment,
				// putting the terminal point (and its LoS probes) in space the trace never verified.
				const float RemainingBudget = FMath::Max(0.f, FMath::Min(
					FMath::Min(Component.MaxRayDistance * Settings.RayLengthMultiplier, Ray.SegSubmitLen),
					Budget - Ray.CumulativeDistance));
				Ray.TerminalPoint = Ray.Origin + Ray.Dir * RemainingBudget;
				Ray.bHasTerminalPoint = true;

				if (Component.bDrawDebugRays && (Component.bShowBounceRays || Component.bShowSurfaceCrawl)) {
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
				if (Component.bDrawDebugRays && (Component.bShowBounceRays || Component.bShowSurfaceCrawl)) {
					DrawDebugLine(World, Ray.Origin, TermHit.Location, FColor::White,
					              false, Settings.DebugLineDuration, 0, 1.f);
					DrawDebugSphere(World, TermHit.Location, 5.f, 6, FColor::White,
					                false, Settings.DebugLineDuration, SDPG_Foreground, 1.f);
				}
				if (!Ray.bLoSFound) {
					const float TermSegLen = FVector::Dist(Ray.Origin, TermHit.Location);
					if (TermSegLen > 1.f) {
						const FVector TermDir = (TermHit.Location - Ray.Origin).GetSafeNormal();
						SubmitSegmentLoSProbes(Component, Ray, World, Ray.Origin, TermDir, TermSegLen, Budget, Settings);
					}
				}
			}
			if (!Ray.PendingLoSProbes.IsEmpty()) {
				Ray.bTerminalLoSPending = true;
				bAllDone = false;
			}
			else {
				Ray.bDone = true;
			}
		}
		else {
			const FHitResult& Hit = SegData.OutHits[0];

			if (FVector::DotProduct(Hit.Normal, Ray.Dir) > 0.f) {
				if (Component.bDrawDebugRays && Component.bShowSurfaceCrawl) {
					DrawDebugSphere(World, Hit.Location, 18.f, 8, FColor::Red,
					                false, Settings.DebugLineDuration * 4.f, SDPG_Foreground, 3.f);
					DrawDebugLine(World, Hit.Location, Hit.Location + Hit.Normal * 40.f,
					              FColor::Red, false, Settings.DebugLineDuration * 4.f, 0, 2.f);
				}
				if (!Ray.PendingLoSProbes.IsEmpty()) {
					Ray.bTerminalLoSPending = true;
					bAllDone = false;
				}
				else {
					Ray.bDone = true;
				}
				continue;
			}

			if (!Ray.bLoSFound) {
				const float MidSegLen = FVector::Dist(Ray.Origin, Hit.Location);
				if (MidSegLen > 1.f) {
					const FVector MidDir = (Hit.Location - Ray.Origin).GetSafeNormal();
					SubmitSegmentLoSProbes(Component, Ray, World, Ray.Origin, MidDir, MidSegLen, Budget, Settings);
				}
			}

			Ray.CumulativeDistance += FVector::Dist(Ray.Origin, Hit.Location);

			if (Component.bDrawDebugRays && (Component.bShowBounceRays || Component.bShowSurfaceCrawl)) {
				DrawDebugLine(World, Ray.Origin, Hit.Location, FColor::White,
				              false, Settings.DebugLineDuration, 0, 1.f);
				DrawDebugSphere(World, Hit.Location, 5.f, 6, FColor::White,
				                false, Settings.DebugLineDuration, SDPG_Foreground, 1.f);
			}

			const bool bDoCrawl = Settings.bEnableSurfaceCrawl && Ray.bNextHitCrawls
				&& (!Settings.bCrawlOnFirstBounceOnly || Ray.Bounce == 0);

			if (bDoCrawl) {
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

				if (!CrawlDir.IsNearlyZero()) {
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
						const FVector ProbeEnd = StepPos + BackDir * BackProbeLen;
						const FVector FwdEnd = StepPos + CrawlDir * Settings.CrawlStepSize;

						FSpatialRayState::FAsyncCrawlStepProbe SP;
						SP.StepPos = StepPos;
						SP.StepCumDist = Ray.CumulativeDistance + Step * Settings.CrawlStepSize;
						SP.BackHandle = Component.SubmitAsyncTrace(World, StepPos, ProbeEnd);
						SP.LoSHandle = Component.SubmitAsyncTrace(World, StepPos, Component.AsyncListenerPos);
						SP.PerpHandle = Component.SubmitAsyncTrace(World, StepPos, FwdEnd);
						Ray.CrawlStepProbes.Add(MoveTemp(SP));
					}

					Ray.BatchPhase = FSpatialRayState::ERayBatchPhase::CrawlBatch;
					bAllDone = false;
					continue;
				}
			}

			Ray.Dir = ComputeBouncedDirection(Ray.Dir, Hit.Normal, !Ray.bLoSFound && bBias,
			                                  Hit.Location, Component.AsyncSteeringListenerPos, Settings.SurfaceRoughness,
			                                  Settings.BounceListenerBias);
			Ray.Origin = Hit.Location + Hit.Normal * Settings.RaySurfaceBias;
			++Ray.Bounce;
			Ray.BounceWaypoints.Add({Ray.Origin, Ray.CumulativeDistance});

			if (Settings.bEnableSurfaceCrawl) {
				Ray.bNextHitCrawls = !Ray.bNextHitCrawls;
			}

			const float DirectToListener = FVector::Dist(Ray.Origin, Component.AsyncListenerPos);
			if (!Ray.bLoSFound && Ray.CumulativeDistance + DirectToListener <= Budget) {
				FSpatialRayState::FAsyncLoSProbe BounceProbe;
				BounceProbe.LoSHandle = Component.SubmitAsyncTrace(World, Ray.Origin, Component.AsyncListenerPos);
				BounceProbe.SamplePos = Ray.Origin;
				BounceProbe.CumDist = Ray.CumulativeDistance;
				BounceProbe.BounceAtSubmit = Ray.Bounce;
				Ray.PendingLoSProbes.Add(MoveTemp(BounceProbe));
				if (Component.bDrawDebugRays && Component.bShowLoSChecks) {
					DrawDebugLine(World, Ray.Origin, Component.AsyncListenerPos, FColor(160, 0, 255),
					              false, Settings.DebugLineDuration, 0, 0.5f);
				}
			}

			float Remain = FMath::Min(Component.MaxRayDistance, Budget - Ray.CumulativeDistance);
			if (Settings.MaxStraightFlightDistance > 0.f) {
				Remain = FMath::Min(Remain, Settings.MaxStraightFlightDistance);
			}
			// Best-case prune: every LoS probe is gated on CumDist + dist(point, listener) <= Budget,
			// and by the triangle inequality that sum only grows with further travel — once it exceeds
			// the budget here, no future probe of this ray can ever pass the gate. Flying on would only
			// burn traces with zero possible payoff.
			if (Remain < 1.f || Ray.CumulativeDistance + DirectToListener > Budget) {
				if (!Ray.PendingLoSProbes.IsEmpty()) {
					Ray.bTerminalLoSPending = true;
					bAllDone = false;
				}
				else {
					Ray.bDone = true;
				}
			}
			else {
				Ray.SegSubmitLen = Remain;
				Ray.SegHandle = Component.SubmitAsyncTrace(World, Ray.Origin, Ray.Origin + Ray.Dir * Remain);
				bAllDone = false;
			}
		}
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
		FSpatialRayState::FAsyncLoSProbe& P = Ray.PendingLoSProbes[ProbeIdx];
		FTraceDatum LD;
		if (!World->QueryTraceData(P.LoSHandle, LD)) {
			continue;
		}

		if (!Ray.bLoSFound && IsTraceClear(LD) && P.CumDist < BestCumDist) {
			// Reverse sanity trace: a probe origin inside geometry exits silently on the forward
			// trace, but the listener->origin trace hits the geometry's outer face and rejects it.
			FHitResult SanityHit;
			if (!Component.TraceLine(World, SanityHit, ListenerPos, P.SamplePos)) {
				BestCumDist = P.CumDist;
				BestLoSPos = P.SamplePos;
				BestBounce = P.BounceAtSubmit;
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

void FAsyncCastManager::ProcessCrawlBatch(USpatialAudioComponent& Component, FSpatialRayState& Ray, UWorld* World,
                                          bool bBias, float Budget, bool& bAllDone,
                                          const USpatialAudioSettings& Settings) {
	FTraceDatum RD;
	if (!World->QueryTraceData(Ray.CrawlRangeHandle, RD)) {
		bAllDone = false;
		return;
	}

	for (FSpatialRayState::FAsyncCrawlStepProbe& SP : Ray.CrawlStepProbes) {
		FTraceDatum D;
		if (!World->QueryTraceData(SP.BackHandle, D) ||
			!World->QueryTraceData(SP.LoSHandle, D) ||
			!World->QueryTraceData(SP.PerpHandle, D)) {
			bAllDone = false;
			return;
		}
	}

	const int32 NumSteps = Ray.CrawlStepProbes.Num();
	int32 EffMaxSteps = Ray.CrawlMaxSteps;
	if (!RD.OutHits.IsEmpty() && RD.OutHits[0].bBlockingHit) {
		const float HitDist = FVector::Dist(Ray.CrawlNudgedStart, RD.OutHits[0].Location);
		EffMaxSteps = FMath::Min(
			FMath::FloorToInt(HitDist / FMath::Max(Ray.CrawlStepSz, 1.f)),
			Ray.CrawlMaxSteps);
	}

	bool bCrawlSucceeded = false;
	bool bPerpWallHit = false;
	FVector EdgePoint = FVector::ZeroVector;
	FVector EdgeDir = FVector::ZeroVector;
	FVector PerpNormal = FVector::ZeroVector;
	float CrawlDistResult = 0.f;
	int32 FoundAtStep = -1;

	const int32 Limit = FMath::Min(EffMaxSteps, NumSteps);
	for (int32 StepIdx = 0; StepIdx < Limit; ++StepIdx) {
		FSpatialRayState::FAsyncCrawlStepProbe& SP = Ray.CrawlStepProbes[StepIdx];

		if (!Ray.bLoSFound
			&& FVector::DotProduct(Component.AsyncListenerPos - SP.StepPos, Ray.CrawlHitNormal) > 0.f) {
			if (Component.bDrawDebugRays && Component.bShowLoSChecks && World) {
				DrawDebugLine(World, SP.StepPos, Component.AsyncListenerPos, FColor(160, 0, 255),
				              false, Settings.DebugLineDuration, 0, 0.5f);
			}
			FTraceDatum LSD;
			World->QueryTraceData(SP.LoSHandle, LSD);
			if (IsTraceClear(LSD)) {
				FHitResult SanityHit;
				const bool bReverseOk = !Component.TraceLine(World, SanityHit, Component.AsyncListenerPos, SP.StepPos);
				if (bReverseOk) {
					Ray.bLoSFound = true;
					Ray.LoSBounces = FMath::Max(1, Ray.Bounce);
					Ray.LoSOrigin = SP.StepPos;
					// Stops at the edge point (SP.StepCumDist) — excludes the edge->listener
					// confirmation leg, same reasoning as DrainPendingLoSProbes above.
					Ray.LoSCumulativeDistance = SP.StepCumDist;
					if (Component.bDrawDebugRays && Component.bShowSurfaceCrawl && World) {
						DrawDebugSphere(World, SP.StepPos, 10.f, 8, FColor::Green,
						                false, Settings.DebugLineDuration, SDPG_Foreground, 2.f);
					}
				}
			}
		}

		FTraceDatum PD;
		World->QueryTraceData(SP.PerpHandle, PD);
		if (!PD.OutHits.IsEmpty() && PD.OutHits[0].bBlockingHit) {
			const FHitResult& PH = PD.OutHits[0];
			EdgePoint = PH.Location + PH.Normal * Ray.CrawlNudgeDist;
			EdgeDir = Math::ReflectDirection(Ray.CrawlDir, PH.Normal);
			CrawlDistResult = static_cast<float>(StepIdx) * Ray.CrawlStepSz + FVector::Dist(SP.StepPos, PH.Location);
			PerpNormal = PH.Normal;
			bPerpWallHit = true;
			bCrawlSucceeded = true;
			FoundAtStep = StepIdx;
			break;
		}

		FTraceDatum BD;
		World->QueryTraceData(SP.BackHandle, BD);
		if (BD.OutHits.IsEmpty() || !BD.OutHits[0].bBlockingHit) {
			const FVector ToListener = (Component.AsyncSteeringListenerPos - SP.StepPos).GetSafeNormal();
			EdgePoint = SP.StepPos;
			EdgeDir = SelectEdgeDirection(Ray.CrawlHitNormal, ToListener);
			CrawlDistResult = static_cast<float>(StepIdx + 1) * Ray.CrawlStepSz;
			bCrawlSucceeded = true;
			FoundAtStep = StepIdx;
			break;
		}
	}

	if (Component.bDrawDebugRays && Component.bShowSurfaceCrawl && World) {
		DrawDebugSphere(World, Ray.CrawlNudgedStart, 6.f, 6, FColor::Cyan,
		                false, Settings.DebugLineDuration, SDPG_Foreground, 1.5f);

		FVector Prev = Ray.CrawlNudgedStart;
		const int32 DrawLimit = (FoundAtStep >= 0) ? FoundAtStep + 1 : Limit;
		for (int32 StepIdx = 0; StepIdx < DrawLimit; ++StepIdx) {
			const FSpatialRayState::FAsyncCrawlStepProbe& SP = Ray.CrawlStepProbes[StepIdx];

			const bool bIsEdgeStep = (StepIdx == FoundAtStep);
			const FColor StepColor = !bIsEdgeStep  ? FColor::White
			                       : bPerpWallHit  ? FColor::Orange
			                       :                 FColor::Yellow;
			const float Radius = bIsEdgeStep ? 12.f : 4.f;

			DrawDebugSphere(World, SP.StepPos, Radius, 6, StepColor,
			                false, Settings.DebugLineDuration, SDPG_Foreground, 1.f);
			// Cyan = crawl movement, matching the replay sweep's crawl color; flight segments stay white.
			DrawDebugLine(World, Prev, SP.StepPos, FColor(0, 220, 255),
			              false, Settings.DebugLineDuration, 0, 1.5f);
			Prev = SP.StepPos;
		}

		if (bCrawlSucceeded) {
			const FColor EdgeColor = bPerpWallHit ? FColor::Orange : FColor::Yellow;
			DrawDebugLine(World, EdgePoint, EdgePoint + EdgeDir * 40.f,
			              EdgeColor, false, Settings.DebugLineDuration, 0, 2.f);
		}
		else {
			DrawDebugSphere(World, Ray.CrawlHitLoc, 10.f, 6, FColor(255, 80, 80),
			                false, Settings.DebugLineDuration, SDPG_Foreground, 2.f);
		}
	}

	Ray.CrawlStepProbes.Empty();
	Ray.BatchPhase = FSpatialRayState::ERayBatchPhase::None;

	if (bCrawlSucceeded) {
		Ray.CumulativeDistance += CrawlDistResult;
		if (bPerpWallHit) {
			++Ray.Bounce;
			Ray.Dir = EdgeDir;
			Ray.Origin = EdgePoint + PerpNormal * Settings.RaySurfaceBias;
			Ray.BounceWaypoints.Add({Ray.Origin, Ray.CumulativeDistance});
		}
		else {
			Ray.Dir = EdgeDir;
			Ray.Origin = EdgePoint;
			++Ray.Bounce;
			Ray.BounceWaypoints.Add({EdgePoint, Ray.CumulativeDistance});
		}
	}
	else {
		Ray.Dir = ComputeBouncedDirection(Ray.CrawlInDir, Ray.CrawlHitNormal, !Ray.bLoSFound && bBias,
		                                  Ray.CrawlHitLoc, Component.AsyncSteeringListenerPos, Settings.SurfaceRoughness,
		                                  Settings.BounceListenerBias);
		Ray.Origin = Ray.CrawlHitLoc + Ray.CrawlHitNormal * Settings.RaySurfaceBias;
		++Ray.Bounce;
	}

	if (Settings.bEnableSurfaceCrawl) {
		Ray.bNextHitCrawls = !Ray.bNextHitCrawls;
	}

	const float DirectToListener = FVector::Dist(Ray.Origin, Component.AsyncListenerPos);
	if (!Ray.bLoSFound && Ray.CumulativeDistance + DirectToListener <= Budget) {
		FSpatialRayState::FAsyncLoSProbe BounceProbe;
		BounceProbe.LoSHandle = Component.SubmitAsyncTrace(World, Ray.Origin, Component.AsyncListenerPos);
		BounceProbe.SamplePos = Ray.Origin;
		BounceProbe.CumDist = Ray.CumulativeDistance;
		BounceProbe.BounceAtSubmit = Ray.Bounce;
		Ray.PendingLoSProbes.Add(MoveTemp(BounceProbe));
		if (Component.bDrawDebugRays && Component.bShowLoSChecks && World) {
			DrawDebugLine(World, Ray.Origin, Component.AsyncListenerPos, FColor(160, 0, 255),
			              false, Settings.DebugLineDuration, 0, 0.5f);
		}
	}

	// Best-case prune — see TickAsyncCast: past this bound no future LoS probe can pass the budget gate.
	if (Ray.bLoSFound || Budget - Ray.CumulativeDistance < 1.f
		|| Ray.CumulativeDistance + DirectToListener > Budget) {
		if (!Ray.PendingLoSProbes.IsEmpty()) {
			Ray.bTerminalLoSPending = true;
			bAllDone = false;
		}
		else {
			Ray.bDone = true;
		}
		return;
	}

	float RemainC = FMath::Min(Component.MaxRayDistance, Budget - Ray.CumulativeDistance);
	if (Settings.MaxStraightFlightDistance > 0.f) {
		RemainC = FMath::Min(RemainC, Settings.MaxStraightFlightDistance);
	}
	Ray.SegSubmitLen = RemainC;
	Ray.SegHandle = Component.SubmitAsyncTrace(World, Ray.Origin, Ray.Origin + Ray.Dir * RemainC);
	bAllDone = false;
}

TArray<FVector> FAsyncCastManager::BuildEdgeDirHints(const TArray<FStoredLoSPath>& StoredPaths, const FVector& SourcePos) {
	TArray<FVector> Hints;
	Hints.Reserve(StoredPaths.Num());
	for (const FStoredLoSPath& SP : StoredPaths) {
		const FVector ToEdge = SP.LoSOrigin - SourcePos;
		const float Len = ToEdge.Size();
		if (Len > 1.f) {
			Hints.Add(ToEdge / Len);
		}
	}
	return Hints;
}

FVector FAsyncCastManager::ComputeBouncedDirection(const FVector& InDir, const FVector& SurfaceNormal,
                                                   bool bApplyBias, const FVector& HitLocation,
                                                   const FVector& ListenerPos, float SurfaceRoughness,
                                                   float BounceListenerBias) {
	const FVector Reflected = Math::ReflectDirection(InDir, SurfaceNormal);
	FVector RandH = FMath::VRand();
	if (FVector::DotProduct(RandH, SurfaceNormal) < 0.f) {
		RandH = -RandH;
	}
	FVector Result = FMath::Lerp(Reflected, RandH, SurfaceRoughness).GetSafeNormal();

	if (bApplyBias) {
		const FVector HitToLis = ListenerPos - HitLocation;
		const float HitLisDist = HitToLis.Size();
		if (HitLisDist > 0.f) {
			const FVector HitToListenerDir = HitToLis / HitLisDist;
			for (int32 Attempt = 0; Attempt < 20; ++Attempt) {
				FVector RandH2 = FMath::VRand();
				if (FVector::DotProduct(RandH2, SurfaceNormal) < 0.f) {
					RandH2 = -RandH2;
				}
				const FVector Cand = FMath::Lerp(Reflected, RandH2, SurfaceRoughness).GetSafeNormal();
				if (FMath::FRand() < (1.f - FMath::Abs(FVector::DotProduct(Cand, HitToListenerDir)))) {
					Result = Cand;
					break;
				}
			}
		}
	}

	if (BounceListenerBias > 0.f) {
		const FVector ToListener = (ListenerPos - HitLocation).GetSafeNormal();
		Result = FMath::Lerp(Result, ToListener, BounceListenerBias).GetSafeNormal();
		if (FVector::DotProduct(Result, SurfaceNormal) < 0.f) {
			Result -= 2.f * FVector::DotProduct(Result, SurfaceNormal) * SurfaceNormal;
			Result.Normalize();
		}
	}

	return Result;
}

FVector FAsyncCastManager::ComputeMidAirTurnDirection(const FVector& InDir, const FVector& TurnPoint,
                                                      const FVector& ListenerPos, bool bApplyBias,
                                                      float SurfaceRoughness, float BounceListenerBias) {
	if (SurfaceRoughness <= 0.f && BounceListenerBias <= 0.f) {
		// With no scatter and no bias the lerp below returns InDir unchanged — the ray would burn a
		// bounce flying straight. Turn 90° instead, at an angle seeded from the turn point so a
		// stationary scene replays the identical direction every sweep (MakeBiasStream pattern).
		const uint32 Seed = HashCombine(GetTypeHash(TurnPoint), GetTypeHash(ListenerPos));
		FRandomStream Stream(static_cast<int32>(Seed));
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

		const FVector SamplePt = SegOrigin + SegDir * T;
		const float SampCumDist = Ray.CumulativeDistance + T;
		if (SampCumDist + FVector::Dist(SamplePt, Component.AsyncListenerPos) > Budget) {
			break;
		}

		FSpatialRayState::FAsyncLoSProbe Probe;
		Probe.LoSHandle = Component.SubmitAsyncTrace(World, SamplePt, Component.AsyncListenerPos);
		Probe.SamplePos = SamplePt;
		Probe.CumDist = SampCumDist;
		Probe.BounceAtSubmit = Ray.Bounce;
		Ray.PendingLoSProbes.Add(MoveTemp(Probe));
		if (Component.bDrawDebugRays && Component.bShowLoSChecks && World) {
			DrawDebugLine(World, SamplePt, Component.AsyncListenerPos, FColor(160, 0, 255),
			              false, Settings.DebugLineDuration, 0, 0.5f);
		}
	}

	{
		const float SafeT = FMath::Max(0.f, SegLen - 2.f);
		const FVector EndSamplePt = SegOrigin + SegDir * SafeT;
		const float EndCumDist = Ray.CumulativeDistance + SafeT;
		if (SafeT > 0.f && EndCumDist + FVector::Dist(EndSamplePt, Component.AsyncListenerPos) <= Budget) {
			FSpatialRayState::FAsyncLoSProbe Probe;
			Probe.LoSHandle = Component.SubmitAsyncTrace(World, EndSamplePt, Component.AsyncListenerPos);
			Probe.SamplePos = EndSamplePt;
			Probe.CumDist = EndCumDist;
			Probe.BounceAtSubmit = Ray.Bounce;
			Ray.PendingLoSProbes.Add(MoveTemp(Probe));
			if (Component.bDrawDebugRays && Component.bShowLoSChecks && World) {
				DrawDebugLine(World, EndSamplePt, Component.AsyncListenerPos, FColor(160, 0, 255),
				              false, Settings.DebugLineDuration, 0, 0.5f);
			}
		}
	}
}

void FAsyncCastManager::SubmitFinalizeBatch(USpatialAudioComponent& Component, const USpatialAudioSettings& Settings) {
	UWorld* World = Component.GetWorld();

	// Pre-warm casts fire while the source is still partially visible, so rays reaching the
	// listener directly are expected — masking the flag keeps the refine probes (the whole
	// point of the pre-sweep) and stops readback from wiping the freshly warmed cache.
	const bool bDirectLoSFound = !Component.bPreSweepCast && Math::HasAnyDirectLoS(Component.AsyncRays);

	const FCachedPointAccum Accum = AccumulateCachedPoints(
		Component.PendingValidCachedPoints,
		Component.AsyncListenerPos, Settings);

	int32 RaysReached = Accum.RaysReached;
	int32 TotalLoSBounces = 0;
	float MinLoSDist = Accum.MinLoSDist;
	FVector WeightedPosSum = Accum.WeightedPos;
	float TotalWeight = Accum.TotalWeight;
	float WeightedDistSum = Accum.WeightedDist;

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
			                                             Probe.ShortestPath, Probe.ShortestPathVerifiedFrom);
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
	Component.Finalize.WeightedPosSum = WeightedPosSum;
	Component.Finalize.TotalWeight = TotalWeight;
	Component.Finalize.WeightedDistSum = WeightedDistSum;
	Component.Finalize.bDirectLoSFound = bDirectLoSFound;
	Component.Finalize.bPending = true;
	Component.bAsyncCastActive = false;
	Component.AsyncRays.Reset();
}
