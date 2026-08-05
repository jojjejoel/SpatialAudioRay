#pragma once

#include "CoreMinimal.h"
#include "Engine/HitResult.h"
#include "WorldCollision.h"

inline bool IsTraceClear(const FTraceDatum& Datum) {
	return Datum.OutHits.IsEmpty() || !Datum.OutHits[0].bBlockingHit;
}

struct FStoredLoSPath {
	FVector LoSOrigin;
	float LoSCumulativeDistance = 0.f;
	float PathDist = 0.f;
	TArray<FVector> ShortestPath;
	TArray<bool> ShortestPathSegmentVerified;
};

struct FCachedEdgePoint {
	FVector EdgePoint = FVector::ZeroVector;
	float GeomDist = 0.f;
	float PathDist = 0.f;
	TArray<FVector> ShortestPath;
	TArray<bool> ShortestPathSegmentVerified;
	FVector CapturedSourcePos = FVector::ZeroVector;
	FVector CapturedListenerPos = FVector::ZeroVector;

	FTraceHandle AsyncPhase0Handle;
	bool bPhase0Pending = false;

	FTraceHandle Phase0OffsetHandles[4];
	FVector Phase0OffsetPts[4];
	bool bPhase0OffsetPending = false;

	bool bEvicting = false;

	bool bNewSinceFillArm = false;

	FVector LastLoSListenerPos = FVector::ZeroVector;
	bool bHasLastLoSListenerPos = false;

	bool bRelayed = false;
	FVector RelayPoint = FVector::ZeroVector;
	float RelayDist = 0.f;

	FTraceHandle RelayCheckHandles[4];
	bool bRelayCheckPending = false;

	FTraceHandle RescueHandles[4];
	bool bRescuePending = false;

	void ClearRelay() {
		bRelayed = false;
		RelayPoint = FVector::ZeroVector;
		RelayDist = 0.f;
		bRelayCheckPending = false;
	}

	FVector EffectivePoint() const { return bRelayed ? RelayPoint : EdgePoint; }
	float EffectivePathDist() const { return PathDist + RelayDist; }
};

/** Everything the cache admission policy reads from the component, so that policy is a pure function
 *  over data: no world, no traces, and testable by constructing the inputs directly. */
struct FCacheMergeContext {
	FVector SourcePos = FVector::ZeroVector;
	FVector ListenerPos = FVector::ZeroVector;
	float MaxRayDistance = 1.f;
};

struct FEdgeCluster {
	FVector Centroid = FVector::ZeroVector;
	float PathDist = 0.f;
	float TotalWeight = 0.f;
};

struct FVirtualVoice {
	bool bActive = false;
	FVector TargetPosition = FVector::ZeroVector;
	float PathDist = 0.f;
	float TargetPathAttenuation = 0.f;
	float CurrentPathAttenuation = 0.f;
	float CurrentPathBend = 0.f;
	float TargetWeightShare = 1.f;
	float CurrentWeightShare = 1.f;
	int32 SlotIndex = INDEX_NONE;
	int32 ClusterIndex = INDEX_NONE;
};

struct FVirtualSlot {
	enum class EState : uint8 { Idle, FadingIn, Active, FadingOut };

	EState State = EState::Idle;
	float FadeAlpha = 0.f;
	int32 VoiceIndex = INDEX_NONE;
	FVector WorldOffset = FVector::ZeroVector;
	bool bOffsetInit = false;

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
	float SegSubmitLen = 0.f;

	float CumulativeDistance = 0.f;

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
	TArray<FVector> ShortestPath;
	TArray<bool> ShortestPathSegmentVerified;
};
