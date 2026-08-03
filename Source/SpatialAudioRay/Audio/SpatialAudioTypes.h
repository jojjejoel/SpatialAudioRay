#pragma once

#include "CoreMinimal.h"
// WorldCollision.h only forward-declares FHitResult, so OutHits cannot be dereferenced without it.
#include "Engine/HitResult.h"
#include "WorldCollision.h"

// No hits, or a first hit that is non-blocking. Shared by the sweep and the edge cache, which
// must agree on what "clear" means.
inline bool IsTraceClear(const FTraceDatum& Datum) {
	return Datum.OutHits.IsEmpty() || !Datum.OutHits[0].bBlockingHit;
}

struct FStoredLoSPath {
	FVector LoSOrigin;
	int32 LoSBounces = 0;
	float LoSCumulativeDistance = 0.f;
	float PathDist = 0.f;
	TArray<FVector> ShortestPath;
	/** Is ShortestPath[i] to ShortestPath[i+1] a verified straight line? They can interleave. */
	TArray<bool> ShortestPathSegmentVerified;
};

struct FCachedEdgePoint {
	FVector EdgePoint = FVector::ZeroVector;
	float GeomDist = 0.f;
	float PathDist = 0.f;
	/** String-pulled source-to-edge polyline captured at discovery, the path PathDist was measured
	 *  along. Drawn and re-traced by the recheck; never enters gain or position math. */
	TArray<FVector> ShortestPath;
	/** As FStoredLoSPath: was this segment a straight HasClearShortcut trace at discovery? false
	 *  means a raw crawl or bounce hop that was blocked by design, so re-tracing it says nothing
	 *  about geometry change. Verified and unverified can interleave. */
	TArray<bool> ShortestPathSegmentVerified;
	int32 LoSBounces = 0;
	FVector CapturedSourcePos = FVector::ZeroVector;
	FVector CapturedListenerPos = FVector::ZeroVector;

	FTraceHandle AsyncPhase0Handle;
	bool bPhase0Pending = false;

	FTraceHandle Phase0OffsetHandles[4];
	FVector Phase0OffsetPts[4];
	bool bPhase0OffsetPending = false;

	bool bEvicting = false;
	/** Source-side evictions cannot be revalidated from the listener side, since listener-to-edge
	 *  LoS is typically still clear. Only a sweep rewrite rehabilitates them. */
	bool bSourceSideEviction = false;
	float EvictionAlpha = 1.f;

	/** Set when the entry lands at a genuinely new position; cleared each time a movement trigger
	 *  arms the cache-fill burst. A merge-radius re-confirmation does NOT set it, so stale
	 *  carry-overs cannot satisfy the post-movement re-survey. */
	bool bNewSinceFillArm = false;

	/** Most recent listener position whose Phase 0 confirmed LoS to this edge, and the anchor for
	 *  the rescue below. Not updated while relayed: it must keep pointing at a spot that saw the EDGE. */
	FVector LastLoSListenerPos = FVector::ZeroVector;
	bool bHasLastLoSListenerPos = false;

	/** When listener-to-edge LoS is lost but LastLoSListenerPos still sees both the edge and the
	 *  listener, the edge survives routed via that point. RelayPoint/RelayDist freeze at rescue, so
	 *  later listener movement cannot change the path (listener-independence rule). Single level:
	 *  losing relay LoS evicts normally. Rescue reads only this edge's own geometry, so N edges
	 *  going dark at once produce N relays whatever the order. Transitional, converted on readback. */
	bool bRelayed = false;
	FVector RelayPoint = FVector::ZeroVector;
	float RelayDist = 0.f;

	/** Listener-edge return check plus the edge-relay leg, forward and reverse each. Cleared with
	 *  the relay so a sweep rewrite cannot leave the flag pointing at dead handles. */
	FTraceHandle RelayCheckHandles[4];
	bool bRelayCheckPending = false;

	/** Submitted the tick Phase 0 gives up on this edge, read back the next. The edge is left alone
	 *  while they fly: a doomed edge loses one frame off a fade it was starting anyway. */
	FTraceHandle RescueHandles[4];
	bool bRescuePending = false;

	void ClearRelay() {
		bRelayed = false;
		RelayPoint = FVector::ZeroVector;
		RelayDist = 0.f;
		bRelayCheckPending = false;
	}

	/** Where this edge presents itself to positioning and clustering, with the matching path
	 *  distance. Both are captured geometry, never live listener state. */
	FVector EffectivePoint() const { return bRelayed ? RelayPoint : EdgePoint; }
	float EffectivePathDist() const { return PathDist + RelayDist; }

	/** EffectivePoint() walked back PullbackDist cm along the arrival path. Stops at the first
	 *  unverified segment working backward, since those hug geometry and a point partway along one
	 *  can sit inside a wall, and does not resume past the gap. An absolute cm pullback keeps the
	 *  depth independent of source-to-edge distance and stays on the traced acoustic path. */
	FVector EmitterPoint(float PullbackDist) const {
		FVector Current = EffectivePoint();
		if (PullbackDist <= 0.f) {
			return Current;
		}
		float Remaining = PullbackDist;
		auto WalkToward = [&Current, &Remaining](const FVector& Target) -> bool {
			const float SegLen = FVector::Dist(Current, Target);
			if (SegLen >= Remaining) {
				Current += (Target - Current) * (Remaining / FMath::Max(SegLen, KINDA_SMALL_NUMBER));
				return true;
			}
			Current = Target;
			Remaining -= SegLen;
			return false;
		};
		if (bRelayed && WalkToward(EdgePoint)) {
			return Current;
		}
		for (int32 i = ShortestPath.Num() - 2; i >= 0; --i) {
			if (!ShortestPathSegmentVerified.IsValidIndex(i) || !ShortestPathSegmentVerified[i]) {
				break;
			}
			if (WalkToward(ShortestPath[i])) {
				break;
			}
		}
		return Current;
	}
};

struct FEdgeCluster {
	FVector Centroid = FVector::ZeroVector;
	float PathDist = 0.f;
	float TotalWeight = 0.f;
};

struct FVirtualVoice {
	bool bActive = false;
	FVector TargetPosition = FVector::ZeroVector;
	FVector SmoothedPosition = FVector::ZeroVector;
	float PathDist = 0.f;
	float TargetPathAttenuation = 0.f;
	float CurrentPathAttenuation = 0.f;
	float TargetWeightShare = 1.f;
	float CurrentWeightShare = 1.f;
	int32 SlotIndex = INDEX_NONE;
};

struct FVirtualSlot {
	enum class EState : uint8 { Idle, FadingIn, Active, FadingOut };

	EState State = EState::Idle;
	float FadeAlpha = 0.f;
	int32 VoiceIndex = INDEX_NONE;
	FVector WorldOffset = FVector::ZeroVector;
	bool bOffsetInit = false;

	/** Refreshed while a voice drives the slot, so a released slot keeps its last audible gain
	 *  without the (gone) voice. The live crossfade gate is deliberately NOT baked in: a fading
	 *  slot must still gate off instantly when direct LoS returns. */
	float FrozenGainScale = 0.f;
};

struct FSpatialRayState {
	FVector Origin;
	FVector Dir;
	int32 Bounce = 0;
	bool bLoSFound = false;
	int32 LoSBounces = 0;
	FVector LoSOrigin = FVector::ZeroVector;
	float LoSCumulativeDistance = 0.f;
	bool bDone = false;
	bool bNextHitCrawls = false;

	FTraceHandle SegHandle;

	/** Length the current SegHandle trace was submitted with. On a miss this is the distance
	 *  actually flown, which budget formulas cannot recompute since they miss the flight clamp. */
	float SegSubmitLen = 0.f;

	float CumulativeDistance = 0.f;

	/** Direction-change points in path order. Pos is the nudged free-space origin, not the raw
	 *  surface hit, so probes traced from it do not start inside geometry. */
	struct FBounceWaypoint {
		FVector Pos = FVector::ZeroVector;
		float CumDist = 0.f;
	};
	TArray<FBounceWaypoint> BounceWaypoints;

	enum class ERayBatchPhase : uint8 { None, CrawlBatch };

	ERayBatchPhase BatchPhase = ERayBatchPhase::None;

	bool bTerminalLoSPending = false;

	struct FAsyncLoSProbe {
		FTraceHandle LoSHandle;
		FVector SamplePos;
		float CumDist;
		int32 BounceAtSubmit;
	};

	TArray<FAsyncLoSProbe> PendingLoSProbes;

	struct FAsyncCrawlStepProbe {
		FTraceHandle BackHandle;
		FTraceHandle LoSHandle;
		FTraceHandle PerpHandle;
		FVector StepPos;
		float StepCumDist;
	};

	FTraceHandle CrawlRangeHandle;
	TArray<FAsyncCrawlStepProbe> CrawlStepProbes;

	FVector CrawlNudgedStart = FVector::ZeroVector;
	FVector CrawlDir = FVector::ZeroVector;
	FVector CrawlHitNormal = FVector::ZeroVector;
	FVector CrawlHitLoc = FVector::ZeroVector;
	FVector CrawlInDir = FVector::ZeroVector;
	float CrawlStepSz = 0.f;
	float CrawlNudgeDist = 0.f;
	float CrawlInCumDist = 0.f;
	int32 CrawlMaxSteps = 0;
};

struct FFinalizeRefineProbe {
	FVector LoSOrigin = FVector::ZeroVector;
	float BasePathDist = 0.f;
	int32 LoSBounces = 0;
	float BounceWeightFactor = 1.f;
	TArray<FVector> ShortestPath;
	TArray<bool> ShortestPathSegmentVerified;
};
