#pragma once

#include "CoreMinimal.h"
#include "WorldCollision.h"

struct FStoredLoSPath {
	FVector LoSOrigin;
	int32 LoSBounces = 0;
	float LoSCumulativeDistance = 0.f;
	float PathDist = 0.f;
	TArray<FVector> ShortestPath;
	/** ShortestPathSegmentVerified[i]: is the segment ShortestPath[i]->ShortestPath[i+1] a
	 *  HasClearShortcut-verified straight line? Verified and unverified segments can interleave. */
	TArray<bool> ShortestPathSegmentVerified;
};

struct FCachedEdgePoint {
	FVector EdgePoint = FVector::ZeroVector;
	float GeomDist = 0.f;
	float PathDist = 0.f;
	/** String-pulled source→edge polyline captured at discovery ([source, ...anchors..., edge]) —
	 *  the path PathDist was measured along. Drawn under bShowShortestPaths and re-traced by the
	 *  round-robin ShortestPathRecheckInterval validation; never enters gain/position math. */
	TArray<FVector> ShortestPath;
	/** ShortestPathSegmentVerified[i]: was ShortestPath[i]->ShortestPath[i+1] a straight
	 *  HasClearShortcut trace at discovery? false means unverified traveled route (a raw
	 *  crawl/bounce hop the string pull couldn't shortcut past) and must not be re-traced —
	 *  it was blocked at discovery already, so failing on it says nothing about geometry
	 *  change. Verified and unverified segments can interleave (e.g. two separate corners
	 *  each contributing one unverified hop, with a verified stretch on either side). */
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
	/** Source-side evictions (source moved, shortest-path recheck found the source→edge leg
	 *  blocked) can't be revalidated from the listener side — listener→edge LoS is typically
	 *  still clear in both cases, so Phase 0's clear-restore must not resurrect them (and
	 *  Phase 0 stops submitting for them). Only a sweep rewrite (WriteEntry) rehabilitates. */
	bool bSourceSideEviction = false;
	float EvictionAlpha = 1.f;

	/** Set when this entry lands at a genuinely new position (fresh cache slot or worst-rank
	 *  displacement); cleared each time a movement trigger arms the cache-fill burst. A sweep
	 *  merely re-confirming a pre-existing edge (merge-radius match) does NOT set it — the
	 *  burst's exit condition counts only edges discovered since the latest trigger, so stale
	 *  carry-overs can't satisfy the post-movement re-survey. */
	bool bNewSinceFillArm = false;

	/** Most recent listener position whose Phase 0 confirmed LoS to this edge — the anchor
	 *  for the relay rescue below. Not updated while relayed (it must keep pointing at a spot
	 *  that saw the EDGE, not the relay). */
	FVector LastLoSListenerPos = FVector::ZeroVector;
	bool bHasLastLoSListenerPos = false;

	/** Relay hop: when listener→edge LoS is lost but the captured LastLoSListenerPos still
	 *  sees both the edge and the current listener, the edge survives routed via that point.
	 *  RelayPoint/RelayDist are frozen at rescue time — subsequent listener movement changes
	 *  neither, so the extended path stays gain-safe (listener-independence rule). The
	 *  edge→relay leg is re-verified each Phase 0 interval (dynamic geometry can sever it);
	 *  a severed leg drops the relay and evicts. Single level: a relayed edge that loses
	 *  relay LoS evicts normally. Rescue depends only on this edge's own geometry (fan all
	 *  blocked, both legs verify) — never on sibling edges' state, so N edges going dark at
	 *  once produce N relays regardless of processing order. A relay is transitional — the
	 *  first valid maintenance readback converts it in place to a real edge at the LoS
	 *  transition corner on the edge→relay leg, each relayed edge independently (deliberately
	 *  no yield-to-direct-edge eviction and no direct-edge rescue gate: both killed sibling
	 *  relays/rescues the moment the first conversion landed). */
	bool bRelayed = false;
	FVector RelayPoint = FVector::ZeroVector;
	float RelayDist = 0.f;

	/** In-flight relay-maintenance traces (listener↔edge return check + edge↔relay leg,
	 *  forward and reverse each), submitted on the Phase 0 interval while relayed and read
	 *  back the following tick(s). Cleared with the relay itself so a sweep rewrite can't
	 *  leave the flag pointing at dead handles. */
	FTraceHandle RelayCheckHandles[4];
	bool bRelayCheckPending = false;

	/** In-flight relay-rescue traces (edge↔relay candidate and candidate↔listener, forward and
	 *  reverse each), submitted the tick Phase 0 gives up on this edge and read back the next.
	 *  The edge is left alone while they fly rather than fading pre-emptively: a doomed edge
	 *  loses one frame off a fade it was about to start anyway, and a rescued one never dims. */
	FTraceHandle RescueHandles[4];
	bool bRescuePending = false;

	void ClearRelay() {
		bRelayed = false;
		RelayPoint = FVector::ZeroVector;
		RelayDist = 0.f;
		bRelayCheckPending = false;
	}

	/** Where this edge presents itself to positioning/clustering, and the path distance that
	 *  goes with it. Gain formulas consume these — both are captured geometry, never live
	 *  listener state. */
	FVector EffectivePoint() const { return bRelayed ? RelayPoint : EdgePoint; }
	float EffectivePathDist() const { return PathDist + RelayDist; }

	/** Emitter position: EffectivePoint() walked back PullbackDist cm along the arrival path
	 *  (relay leg first, then the string-pulled polyline toward the source). Stays on traced
	 *  free-space segments only — the walk stops at the first unverified segment it meets
	 *  (working backward from the edge) because unverified segments hug geometry and a point
	 *  partway along one can sit inside a wall; it does not resume past that gap even if a
	 *  later stretch is verified again. An absolute cm
	 *  pullback keeps "how far behind the opening the sound sits" independent of source→edge
	 *  distance, and lies on the acoustic path — unlike a source→edge lerp, which cuts straight
	 *  through the very geometry the path bends around. */
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

	/** Refreshed every frame while a voice drives the slot, so when the slot is released to
	 *  FadingOut it keeps its last audible gain without needing the (gone) voice. The live
	 *  crossfade gate is deliberately NOT baked in — a fading-out slot must still gate off
	 *  instantly when direct LoS is regained. (PathBend needs no freeze: the MetaSound simply
	 *  keeps the last value it was sent.) */
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
	 *  actually flown — the terminal point / mid-air turn point must not be recomputed from
	 *  budget formulas, which don't know about the MaxStraightFlightDistance clamp. */
	float SegSubmitLen = 0.f;

	float CumulativeDistance = 0.f;

	/** Direction-change points along the traveled path, in path order. Pos holds the nudged
	 *  free-space origin (not the raw surface hit) so finalize shortening probes traced from
	 *  these points don't start inside geometry. */
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
