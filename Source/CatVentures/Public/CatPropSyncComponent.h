// CatPropSyncComponent.h — server-authoritative INTACT prop motion (2026-09-24).
//
// Why this exists: every machine used to simulate every prop in its own Chaos solver, and a
// Geometry Collection's motion is not replicated (turning the engine's GC replication on was
// tried and reverted 2026-09-21 — see Docs/match-destruction.md). Any contact that resolved
// differently split the copies for good: a carried prop snagged behind a wall on one machine
// and not the other ended 467 cm apart, and a split copy is an invisible wall / a ghost push
// for the other machine's cats (2P PIE, 2026-09-24).
//
// The model: INTACT props simulate ONLY on the server. On clients they are kinematic followers
// driven by the poses this component multicasts. Breaking stays local — ForceShatterGC calls
// ReleaseFollower first, which hands the client copy back to its own physics before the break
// (the fracture and debris were always per-machine; this changes nothing after the break).
//
// Owned by ACatGameState: it is replicated and always relevant, so every client has it.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "CatPropSyncComponent.generated.h"

class UGeometryCollectionComponent;
class ACatBase;

/** One prop's root pose, as the server's solver has it. */
USTRUCT()
struct FCatPropPose
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<AActor> Prop = nullptr;

	UPROPERTY()
	FVector_NetQuantize10 Location = FVector::ZeroVector;

	UPROPERTY()
	FQuat Rotation = FQuat::Identity;

	/** Measured from the server's pose samples; clients extrapolate the target by it. */
	UPROPERTY()
	FVector_NetQuantize10 Velocity = FVector::ZeroVector;
};

UCLASS(ClassGroup = (CatVentures))
class CATVENTURES_API UCatPropSyncComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCatPropSyncComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	/** Called by ACatBase::ForceShatterGC on EVERY machine before it breaks a prop. On a client
	 *  whose copy is a kinematic follower, hands it back to local physics (dynamic-state field
	 *  now and again next tick — released pieces otherwise keep the kinematic state, the shrine
	 *  lesson). No-op on the server and for anything that isn't a tracked follower. */
	static void ReleaseFollower(UGeometryCollectionComponent* GCC);

	/** Client only: the LOCAL cat's carry of a prop is simulated here, not followed (2026-09-27).
	 *  Following the server made the carrier's own prop hitch — the server tows it with its copy
	 *  of the client cat, which moves only when a move packet lands (measured 0/300/0/300 cm/s at
	 *  120 fps), and every pose then corrected the client's extrapolation by a few cm. On start the
	 *  prop goes dynamic and the grab constraint tows it exactly as the host's does; on end it goes
	 *  kinematic again and eases to the latest server pose (the poses keep arriving throughout).
	 *  Self-heals if the carrier stops grabbing by any path. No-op on the server / for untracked props. */
	static void SetLocalCarry(UGeometryCollectionComponent* GCC, const ACatBase* Carrier, bool bCarrying);

	/** Client, on the release PRESS: if this prop is the local carry, freeze it where it is
	 *  (kinematic), hold it there until the server's copy has caught up, and return that pose so
	 *  the release RPC can carry it. False = not a local carry (nothing to send). 2026-09-27. */
	static bool BeginPredictedRelease(UGeometryCollectionComponent* GCC, FTransform& OutPose);

	/** Server, after the release: move the server's copy to where the carrier let go, if within a
	 *  sanity range — the copies drift during a carry (up to ~1 m measured), and this makes the
	 *  carrier's view the one that stands, so the release doesn't pop for them. Runs over three
	 *  frames (kinematic → place → verify + dynamic) from ServerTick — see StepAdopt. */
	static void AdoptClientReleasePose(UGeometryCollectionComponent* GCC, const FTransform& Pose);

	/** Master switch. Off = every machine simulates its own props again (the pre-09-24 model). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Prop Sync")
	bool bEnablePropSync = true;

	/** Server: pose batches per second for props that are moving. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Prop Sync", meta = (ClampMin = "2.0", ClampMax = "60.0"))
	float SendRate = 30.0f;

	/** Server: seconds a prop that stopped keeps re-sending its final pose (the batches are
	 *  unreliable — this is what makes a dropped last packet harmless). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Prop Sync", meta = (ClampMin = "0.0"))
	float SettleResendTime = 1.0f;

	/** Server: every N seconds, re-send the pose of every prop that has ever moved — covers
	 *  packet loss and a client that started following late. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Prop Sync", meta = (ClampMin = "0.5"))
	float KeyframeInterval = 2.0f;

	/** Client: exponential follow rate toward the latest server pose (1/s). Higher = snappier,
	 *  lower = smoother but further behind. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Prop Sync", meta = (ClampMin = "1.0"))
	float FollowRate = 30.0f;

	/** Client: how far ahead (s) the target may be extrapolated along the server's velocity —
	 *  covers the send interval + the follow ease so a carried prop keeps up with the cat.
	 *  0 = no extrapolation (pure follow). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Prop Sync", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float MaxExtrapolation = 0.15f;

	/** Client: a gap bigger than this is snapped instead of eased (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Prop Sync", meta = (ClampMin = "10.0"))
	float SnapDistance = 300.0f;

protected:
	virtual void BeginPlay() override;

	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_PropPoses(const TArray<FCatPropPose>& Poses);

private:
	struct FTracked
	{
		TWeakObjectPtr<UGeometryCollectionComponent> GCC;
		FTransform SpawnPose;
		FTransform LastSent;
		double LastMovedTime = -1000.0;
		bool bFollower = false;   // client: kinematic, driven by this component
		bool bReleased = false;   // client: handed back to local physics for its break
		bool bHasTarget = false;
		bool bLocalCarry = false; // client: the local cat is carrying it — simulated here, not followed
		TWeakObjectPtr<const ACatBase> Carrier;
		double ReleaseHoldUntil = 0.0;    // client: ignore stale server poses until then (predicted release)
		FVector ReleaseHoldLoc = FVector::ZeroVector;
		int32 HoldSkipped = 0;
		FTransform Target;
		FVector TargetVelocity = FVector::ZeroVector;
		double TargetTime = 0.0;          // client: world time the target arrived
		FVector PrevSampleLoc = FVector::ZeroVector;   // server: for the velocity estimate
		double PrevSampleTime = -1.0;
		FVector SmoothedVelocity = FVector::ZeroVector; // server: per-tick samples, exponentially smoothed
		// Server: a release-pose adopt in flight, one step per frame (see AdoptClientReleasePose).
		uint8  AdoptStep = 0;             // 0 none · 1 kinematic requested, place next · 2 placed, verify next
		uint64 AdoptFrame = 0;            // GFrameCounter of the last step — the next runs on a LATER frame
		int32  AdoptPlaces = 0;
		FTransform AdoptTarget;
		float  AdoptFromCm = 0.0f;
	};

	TMap<TWeakObjectPtr<AActor>, FTracked> Props;
	bool  bScanned      = false;
	float ScanDelay     = 0.5f;   // let every prop's BeginPlay + physics creation finish first
	float SendAccum     = 0.0f;
	float KeyframeAccum = 0.0f;
	float StatsAccum    = 0.0f;
	int32 StatsSent     = 0;
	int32 StatsReceived = 0;
	int32 StatsSnaps    = 0;

	void Scan();
	void ServerTick(float DeltaTime);
	void ClientTick(float DeltaTime);
	void SendBatched(const TArray<FCatPropPose>& Poses);
	void ReleaseTracked(FTracked& T, const AActor* Prop);
	void EndLocalCarry(FTracked& T, const AActor* Prop);
	void StepAdopt(FTracked& T, const AActor* Prop);

	static void SetDynamicState(UGeometryCollectionComponent* GCC, bool bKinematic);
	static void PlaceRootAt(UGeometryCollectionComponent* GCC, const FTransform& RootPose);
	static UCatPropSyncComponent* Get(const UWorld* World);
	bool IsClient() const;
};
