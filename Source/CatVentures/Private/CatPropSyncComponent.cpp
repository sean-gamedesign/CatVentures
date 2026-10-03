// CatPropSyncComponent.cpp — see the header for the model and why it exists.

#include "CatPropSyncComponent.h"
#include "CatBase.h"
#include "CatCenterpiece.h"
#include "CatVenturesLog.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Field/FieldSystemObjects.h"
#include "GameFramework/GameStateBase.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "TimerManager.h"

namespace
{
	// "Moved" thresholds for the server's send filter. Below these a resting prop's solver
	// jitter would keep it on the wire forever.
	constexpr float MoveEpsilonCm  = 0.5f;
	constexpr float MoveEpsilonRad = 0.0087f;   // 0.5 deg

	// Server velocity estimate: per-tick samples, exponentially smoothed (2026-09-27). A carried
	// prop is towed by the server's copy of its cat, which on a listen server moves only when a
	// client move packet lands — the raw per-frame speed alternates ~70/220 cm/s around a steady
	// 150, and a two-sample estimate taken at a send tick carried that noise into every client's
	// extrapolation.
	constexpr float VelocitySmoothTime = 0.08f;

	// Predicted release (2026-09-27). The server adopts the carrier's release pose within
	// MaxAdoptCm; meanwhile the client holds the prop there and drops server poses more than
	// HoldAcceptCm from it (in flight from before the adopt) for up to HoldTime.
	constexpr float MaxAdoptCm   = 150.0f;
	constexpr float HoldAcceptCm = 25.0f;
	constexpr float HoldTime     = 0.5f;

	// The adopt counts as landed when the root, read a frame after the placement, is this close
	// to the target; otherwise it is re-placed, up to AdoptMaxPlaces times before going dynamic anyway.
	constexpr float AdoptLandedCm  = 5.0f;
	constexpr int32 AdoptMaxPlaces = 3;

	bool PoseMoved(const FTransform& A, const FTransform& B)
	{
		return FVector::DistSquared(A.GetLocation(), B.GetLocation()) > MoveEpsilonCm * MoveEpsilonCm
			|| A.GetRotation().AngularDistance(B.GetRotation()) > MoveEpsilonRad;
	}
}

UCatPropSyncComponent::UCatPropSyncComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	SetIsReplicatedByDefault(true);
}

void UCatPropSyncComponent::BeginPlay()
{
	Super::BeginPlay();
}

bool UCatPropSyncComponent::IsClient() const
{
	return GetNetMode() == NM_Client;
}

UCatPropSyncComponent* UCatPropSyncComponent::Get(const UWorld* World)
{
	const AGameStateBase* GS = World ? World->GetGameState() : nullptr;
	return GS ? GS->FindComponentByClass<UCatPropSyncComponent>() : nullptr;
}

// ── Field + transform helpers ───────────────────────────────────────

void UCatPropSyncComponent::SetDynamicState(UGeometryCollectionComponent* GCC, bool bKinematic)
{
	if (!GCC) return;
	// A uniform dynamic-state field over the whole collection. SetSimulatePhysics(false) does
	// NOT make a GC kinematic (tested 2026-09-24: the proxy kept simulating); this does.
	UUniformInteger* Node = NewObject<UUniformInteger>(GetTransientPackage());
	Node->Magnitude = static_cast<int32>(bKinematic ? EObjectStateTypeEnum::Chaos_Object_Kinematic
	                                                : EObjectStateTypeEnum::Chaos_Object_Dynamic);
	GCC->ApplyPhysicsField(true, EGeometryCollectionPhysicsTypeEnum::Chaos_DynamicState, nullptr, Node);
	if (!bKinematic)
	{
		GCC->WakeAllRigidBodies();
	}
}

void UCatPropSyncComponent::PlaceRootAt(UGeometryCollectionComponent* GCC, const FTransform& RootPose)
{
	// The GC component is NOT where the prop is (it stays where the prop was placed — see
	// ACatBase::GetPropWorldLocation), so solve for the component transform that puts the
	// ROOT at RootPose, keeping the root's current offset from the component. A kinematic
	// root follows a component transform set (FGeometryCollectionPhysicsProxy::
	// SetWorldTransform_Internal moves kinematic roots only — verified in PIE: exact to 1e-5 cm).
	const FTransform RootNow = GCC->GetRootCurrentTransform();
	const FTransform CompNow = GCC->GetComponentTransform();
	const FTransform RootInComp = RootNow.GetRelativeTransform(CompNow);
	FTransform NewComp = RootInComp.Inverse() * RootPose;
	NewComp.SetScale3D(CompNow.GetScale3D());
	GCC->SetWorldTransform(NewComp, false, nullptr, ETeleportType::TeleportPhysics);
}

// ── Registry ────────────────────────────────────────────────────────

void UCatPropSyncComponent::Scan()
{
	bScanned = true;
	UWorld* World = GetWorld();
	if (!World) return;

	const bool bClient = IsClient();
	int32 Count = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		// The shrine is kinematic and server-driven already; unreplicated actors have no
		// stable net identity to address in the batch.
		if (!A || A->IsA<ACatCenterpiece>() || !A->GetIsReplicated()) continue;
		UGeometryCollectionComponent* GCC = A->FindComponentByClass<UGeometryCollectionComponent>();
		if (!GCC || !GCC->IsSimulatingPhysics() || GCC->IsRootBroken()) continue;

		FTracked T;
		T.GCC       = GCC;
		T.SpawnPose = GCC->GetRootCurrentTransform();
		T.LastSent  = T.SpawnPose;
		if (bClient)
		{
			SetDynamicState(GCC, /*bKinematic=*/true);
			T.bFollower = true;
		}
		Props.Add(A, T);
		++Count;
	}

	UE_LOG(LogCatVentures, Log, TEXT("[PropSync] %s tracking %d intact prop(s)%s"),
		bClient ? TEXT("CLI") : TEXT("SRV"), Count,
		bClient ? TEXT(" — kinematic followers of the server's copy") : TEXT(" — authoritative, sending poses"));
}

void UCatPropSyncComponent::ReleaseFollower(UGeometryCollectionComponent* GCC)
{
	if (!GCC) return;
	UCatPropSyncComponent* Sync = Get(GCC->GetWorld());
	if (!Sync || !Sync->IsClient()) return;
	if (FTracked* T = Sync->Props.Find(GCC->GetOwner()))
	{
		Sync->ReleaseTracked(*T, GCC->GetOwner());
	}
}

void UCatPropSyncComponent::ReleaseTracked(FTracked& T, const AActor* Prop)
{
	if (!T.bFollower || T.bReleased) return;
	T.bReleased  = true;
	T.bHasTarget = false;
	UGeometryCollectionComponent* GCC = T.GCC.Get();
	if (!GCC) return;

	SetDynamicState(GCC, /*bKinematic=*/false);
	// Again next tick: the pieces a break releases can keep the state their parent had when
	// it was set (the kinematic shrine sections hung in the air until this ran after the break).
	TWeakObjectPtr<UGeometryCollectionComponent> WeakGCC = GCC;
	GetWorld()->GetTimerManager().SetTimerForNextTick([WeakGCC]()
	{
		if (UGeometryCollectionComponent* G = WeakGCC.Get()) SetDynamicState(G, /*bKinematic=*/false);
	});

	UE_LOG(LogCatVentures, Log, TEXT("[PropSync] CLI released '%s' to local physics for its break"), *GetNameSafe(Prop));
}

void UCatPropSyncComponent::SetLocalCarry(UGeometryCollectionComponent* GCC, const ACatBase* Carrier, bool bCarrying)
{
	if (!GCC) return;
	UCatPropSyncComponent* Sync = Get(GCC->GetWorld());
	if (!Sync || !Sync->IsClient()) return;
	FTracked* T = Sync->Props.Find(GCC->GetOwner());
	if (!T || !T->bFollower || T->bReleased) return;

	if (bCarrying)
	{
		if (T->bLocalCarry) return;
		T->bLocalCarry = true;
		T->Carrier     = Carrier;
		SetDynamicState(GCC, /*bKinematic=*/false);
		UE_LOG(LogCatVentures, Log, TEXT("[PropSync] CLI local carry START '%s' — simulated here until release"),
			*GetNameSafe(GCC->GetOwner()));
	}
	else if (T->bLocalCarry)
	{
		Sync->EndLocalCarry(*T, GCC->GetOwner());
	}
}

bool UCatPropSyncComponent::BeginPredictedRelease(UGeometryCollectionComponent* GCC, FTransform& OutPose)
{
	if (!GCC) return false;
	UCatPropSyncComponent* Sync = Get(GCC->GetWorld());
	if (!Sync || !Sync->IsClient()) return false;
	FTracked* T = Sync->Props.Find(GCC->GetOwner());
	if (!T || !T->bLocalCarry || T->bReleased) return false;

	OutPose = GCC->GetRootCurrentTransform();
	Sync->EndLocalCarry(*T, GCC->GetOwner());   // kinematic now; logs what the pop would have been

	// Hold here rather than easing to the pre-release server pose EndLocalCarry just targeted.
	const double Now = Sync->GetWorld()->GetTimeSeconds();
	T->Target           = FTransform(OutPose.GetRotation(), OutPose.GetLocation(), FVector::OneVector);
	T->TargetVelocity   = FVector::ZeroVector;
	T->TargetTime       = Now;
	T->bHasTarget       = true;
	T->ReleaseHoldUntil = Now + HoldTime;
	T->ReleaseHoldLoc   = OutPose.GetLocation();
	T->HoldSkipped      = 0;
	return true;
}

void UCatPropSyncComponent::AdoptClientReleasePose(UGeometryCollectionComponent* GCC, const FTransform& Pose)
{
	if (!GCC || GCC->IsRootBroken()) return;
	UCatPropSyncComponent* Sync = Get(GCC->GetWorld());
	if (!Sync || Sync->IsClient() || !Sync->bEnablePropSync) return;

	const FTransform Root = GCC->GetRootCurrentTransform();
	const float Dist = FVector::Dist(Root.GetLocation(), Pose.GetLocation());
	if (Dist > MaxAdoptCm)
	{
		UE_LOG(LogCatVentures, Log, TEXT("[PropSync] SRV release pose REJECTED for '%s' — %.0f cm from the server's copy (> %.0f)"),
			*GetNameSafe(GCC->GetOwner()), Dist, MaxAdoptCm);
		return;
	}

	FTracked* T = Sync->Props.Find(GCC->GetOwner());
	if (!T)
	{
		UE_LOG(LogCatVentures, Warning, TEXT("[PropSync] SRV release pose for '%s' not adopted — prop is not tracked"),
			*GetNameSafe(GCC->GetOwner()));
		return;
	}

	// The documented way to move a simulating GC — kinematic, place the ROOT, dynamic again — but
	// ONE STEP PER FRAME (2026-10-03). Done in one frame on the server's DYNAMIC copy, the placement
	// could land before the kinematic field did (the proxy moves kinematic roots only) and the copy
	// stayed put: 2 of 6 releases on 09-27 logged "adopted" while the server stayed 82 / 104 cm away.
	// The 09-24 recipe was only ever verified on client copies that were ALREADY kinematic.
	// This frame: the field. ServerTick → StepAdopt does the rest on later frames.
	SetDynamicState(GCC, /*bKinematic=*/true);
	T->AdoptStep   = 1;
	T->AdoptFrame  = GFrameCounter;
	T->AdoptPlaces = 0;
	T->AdoptTarget = FTransform(Pose.GetRotation(), Pose.GetLocation(), Root.GetScale3D());
	T->AdoptFromCm = Dist;
	UE_LOG(LogCatVentures, Log, TEXT("[PropSync] SRV adopting the carrier's release pose for '%s' (%.0f cm away) — kinematic now, placing next frame"),
		*GetNameSafe(GCC->GetOwner()), Dist);
}

void UCatPropSyncComponent::StepAdopt(FTracked& T, const AActor* Prop)
{
	// Each step needs a physics step + results sync since the previous one. The release RPC can
	// arrive before this component ticks in the same frame, so gate on the frame counter.
	if (GFrameCounter <= T.AdoptFrame) return;
	UGeometryCollectionComponent* GCC = T.GCC.Get();
	if (!GCC) { T.AdoptStep = 0; return; }
	T.AdoptFrame = GFrameCounter;

	if (T.AdoptStep == 1)
	{
		PlaceRootAt(GCC, T.AdoptTarget);
		++T.AdoptPlaces;
		T.AdoptStep = 2;
		return;
	}

	// Step 2: read the root a frame AFTER the placement. GetRootCurrentTransform() is the
	// physics-synced component-space root × the CURRENT component transform, so read in the same
	// frame as PlaceRootAt it always reports the target; only after a sync does it show whether
	// the physics body actually moved.
	const float Miss = FVector::Dist(GCC->GetRootCurrentTransform().GetLocation(), T.AdoptTarget.GetLocation());
	if (Miss > AdoptLandedCm && T.AdoptPlaces < AdoptMaxPlaces)
	{
		UE_LOG(LogCatVentures, Log, TEXT("[PropSync] SRV adopt on '%s' — root still %.0f cm off after place %d, placing again"),
			*GetNameSafe(Prop), Miss, T.AdoptPlaces);
		PlaceRootAt(GCC, T.AdoptTarget);
		++T.AdoptPlaces;
		return;
	}

	SetDynamicState(GCC, /*bKinematic=*/false);
	T.AdoptStep        = 0;
	T.SmoothedVelocity = FVector::ZeroVector;
	T.LastMovedTime    = GetWorld()->GetTimeSeconds();   // settle-resend covers a landing under the send epsilon
	if (Miss <= AdoptLandedCm)
	{
		UE_LOG(LogCatVentures, Log, TEXT("[PropSync] SRV adopted the carrier's release pose for '%s' (moved %.0f cm) — landed %.1f cm from target after %d place(s), dynamic again"),
			*GetNameSafe(Prop), T.AdoptFromCm, Miss, T.AdoptPlaces);
	}
	else
	{
		UE_LOG(LogCatVentures, Warning, TEXT("[PropSync] SRV adopt on '%s' DID NOT LAND — root %.0f cm from target after %d place(s); dynamic again where it is"),
			*GetNameSafe(Prop), Miss, T.AdoptPlaces);
	}
}

void UCatPropSyncComponent::EndLocalCarry(FTracked& T, const AActor* Prop)
{
	T.bLocalCarry = false;
	T.Carrier.Reset();
	UGeometryCollectionComponent* GCC = T.GCC.Get();
	if (!GCC || T.bReleased) return;   // broke while carried — its debris is local now

	SetDynamicState(GCC, /*bKinematic=*/true);
	// Poses kept arriving through the carry, so the target is current: ease onto it.
	T.bHasTarget = T.TargetTime > 0.0;
	const float Gap = T.bHasTarget
		? FVector::Dist(GCC->GetRootCurrentTransform().GetLocation(), T.Target.GetLocation()) : -1.0f;
	UE_LOG(LogCatVentures, Log, TEXT("[PropSync] CLI local carry END '%s' — back to following, gap to server %.0f cm"),
		*GetNameSafe(Prop), Gap);
}

// ── Tick ────────────────────────────────────────────────────────────

void UCatPropSyncComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Standalone has nobody to agree with.
	if (!bEnablePropSync || GetNetMode() == NM_Standalone) return;

	if (!bScanned)
	{
		ScanDelay -= DeltaTime;
		if (ScanDelay > 0.0f) return;
		Scan();
	}

	if (GetOwner()->HasAuthority()) ServerTick(DeltaTime);
	else                            ClientTick(DeltaTime);

	StatsAccum += DeltaTime;
	if (StatsAccum >= 5.0f)
	{
		if (StatsSent > 0 || StatsReceived > 0 || StatsSnaps > 0)
		{
			UE_LOG(LogCatVentures, Log, TEXT("[PropSync] %s last 5 s: %d pose(s) sent, %d received, %d snap(s)"),
				IsClient() ? TEXT("CLI") : TEXT("SRV"), StatsSent, StatsReceived, StatsSnaps);
		}
		StatsAccum = 0.0f; StatsSent = StatsReceived = StatsSnaps = 0;
	}
}

void UCatPropSyncComponent::ServerTick(float DeltaTime)
{
	const double Now = GetWorld()->GetTimeSeconds();

	// Velocity every tick, smoothed (see VelocitySmoothTime) — the GC's own component velocity is
	// not the prop's (the component never moves on the server).
	for (auto It = Props.CreateIterator(); It; ++It)
	{
		FTracked& T = It.Value();
		UGeometryCollectionComponent* GCC = T.GCC.Get();
		// Broken props leave the registry: from the break on, every machine owns its debris.
		if (!It.Key().Get() || !GCC || GCC->IsRootBroken()) { It.RemoveCurrent(); continue; }

		if (T.AdoptStep != 0)
		{
			// Mid-adopt the root jumps by the adopt distance: keep that out of the velocity
			// estimate, and resume sampling from wherever the adopt leaves it.
			StepAdopt(T, It.Key().Get());
			T.PrevSampleLoc  = GCC->GetRootCurrentTransform().GetLocation();
			T.PrevSampleTime = Now;
			continue;
		}

		const FVector Loc = GCC->GetRootCurrentTransform().GetLocation();
		if (T.PrevSampleTime >= 0.0 && Now > T.PrevSampleTime)
		{
			const float Dt = static_cast<float>(Now - T.PrevSampleTime);
			const FVector Raw = (Loc - T.PrevSampleLoc) / Dt;
			T.SmoothedVelocity = FMath::Lerp(T.SmoothedVelocity, Raw, 1.0f - FMath::Exp(-Dt / VelocitySmoothTime));
		}
		T.PrevSampleLoc  = Loc;
		T.PrevSampleTime = Now;
	}

	SendAccum     += DeltaTime;
	KeyframeAccum += DeltaTime;
	const float SendInterval = 1.0f / SendRate;
	if (SendAccum < SendInterval) return;
	// Carry the remainder: a reset to 0 turned the 30 Hz cadence into an irregular one (at
	// 120 fps, 4 frames fell a hair short of the interval, so it sent every 5th = 24 Hz).
	SendAccum = FMath::Fmod(SendAccum, SendInterval);

	const bool bKeyframe = KeyframeAccum >= KeyframeInterval;
	if (bKeyframe) KeyframeAccum = 0.0f;

	TArray<FCatPropPose> Out;
	for (auto It = Props.CreateIterator(); It; ++It)
	{
		AActor* Prop = It.Key().Get();
		FTracked& T  = It.Value();
		UGeometryCollectionComponent* GCC = T.GCC.Get();
		// Mid-adopt the pose isn't authoritative yet (the old spot, or a placement not yet proven);
		// the landed pose goes out once StepAdopt finishes (it moved, or the settle-resend window).
		if (!Prop || !GCC || T.AdoptStep != 0) continue;

		const FTransform Pose = GCC->GetRootCurrentTransform();
		const FVector Velocity = T.SmoothedVelocity;

		bool bSend = false;
		if (PoseMoved(Pose, T.LastSent))
		{
			T.LastMovedTime = Now;
			bSend = true;
		}
		else if (Now - T.LastMovedTime < SettleResendTime || (bKeyframe && PoseMoved(Pose, T.SpawnPose)))
		{
			bSend = true;   // settling (loss cover) or the periodic catch-up
		}
		if (!bSend) continue;

		T.LastSent = Pose;
		FCatPropPose& P = Out.AddDefaulted_GetRef();
		P.Prop     = Prop;
		P.Location = Pose.GetLocation();
		P.Rotation = Pose.GetRotation();
		P.Velocity = Velocity;
	}
	if (Out.Num() > 0) SendBatched(Out);
}

void UCatPropSyncComponent::SendBatched(const TArray<FCatPropPose>& Poses)
{
	// ~30 bytes a pose; 24 a batch keeps every bunch well under the unreliable size limit.
	constexpr int32 BatchSize = 24;
	for (int32 i = 0; i < Poses.Num(); i += BatchSize)
	{
		TArray<FCatPropPose> Batch(Poses.GetData() + i, FMath::Min(BatchSize, Poses.Num() - i));
		Multicast_PropPoses(Batch);
	}
	StatsSent += Poses.Num();
}

void UCatPropSyncComponent::Multicast_PropPoses_Implementation(const TArray<FCatPropPose>& Poses)
{
	if (GetOwner()->HasAuthority() || !bEnablePropSync) return;   // the server IS the source
	for (const FCatPropPose& P : Poses)
	{
		FTracked* T = P.Prop ? Props.Find(P.Prop.Get()) : nullptr;
		if (!T || !T->bFollower || T->bReleased) continue;
		if (T->ReleaseHoldUntil > 0.0)
		{
			// Predicted release: poses sent before the server adopted our release pose would
			// flick the prop back toward the old server copy — drop them until one agrees.
			const float Dist = FVector::Dist(FVector(P.Location), T->ReleaseHoldLoc);
			const bool bAgrees = Dist <= HoldAcceptCm;
			if (!bAgrees && GetWorld()->GetTimeSeconds() < T->ReleaseHoldUntil)
			{
				++T->HoldSkipped;
				continue;
			}
			UE_LOG(LogCatVentures, Log, TEXT("[PropSync] CLI release hold on '%s' ended — %s (%d stale pose(s) dropped, %.0f cm)"),
				*GetNameSafe(P.Prop.Get()), bAgrees ? TEXT("server agrees") : TEXT("timed out, easing"), T->HoldSkipped, Dist);
			T->ReleaseHoldUntil = 0.0;
			T->HoldSkipped      = 0;
		}
		T->Target         = FTransform(P.Rotation, P.Location, FVector::OneVector);
		T->TargetVelocity = P.Velocity;
		T->TargetTime     = GetWorld()->GetTimeSeconds();
		T->bHasTarget     = true;
		++StatsReceived;
	}
}

void UCatPropSyncComponent::ClientTick(float DeltaTime)
{
	const float Alpha = 1.0f - FMath::Exp(-FollowRate * DeltaTime);
	for (auto It = Props.CreateIterator(); It; ++It)
	{
		FTracked& T = It.Value();
		if (T.bLocalCarry)
		{
			// Simulated here while the local cat carries it. Self-heal: however the carry ended
			// (release, drift drop, stagger drop, the pawn going away), stop simulating it here.
			const ACatBase* Carrier = T.Carrier.Get();
			if (!Carrier || !Carrier->IsGrabbing()) EndLocalCarry(T, It.Key().Get());
			continue;
		}
		if (!T.bFollower || T.bReleased || !T.bHasTarget) continue;
		UGeometryCollectionComponent* GCC = T.GCC.Get();
		if (!GCC) { It.RemoveCurrent(); continue; }
		if (GCC->IsRootBroken())
		{
			// Broke by a path that skipped ForceShatterGC — hand it back all the same.
			ReleaseTracked(T, It.Key().Get());
			continue;
		}

		// The server's pose is already a send interval + an ease behind the cat that is moving
		// it; run the target ahead along the server's velocity by the sample's age (capped) so
		// a carried prop keeps up. A resting prop sends ~zero velocity, so it lands exactly.
		const float Age = FMath::Min(static_cast<float>(GetWorld()->GetTimeSeconds() - T.TargetTime), MaxExtrapolation);
		FTransform Goal = T.Target;
		Goal.AddToTranslation(T.TargetVelocity * Age);

		const FTransform Now = GCC->GetRootCurrentTransform();
		const float Gap = FVector::Dist(Now.GetLocation(), Goal.GetLocation());
		FTransform Next;
		if (Gap > SnapDistance)
		{
			Next = Goal;
			++StatsSnaps;
			UE_LOG(LogCatVentures, Log, TEXT("[PropSync] CLI snapped '%s' %.0f cm to the server's pose"),
				*GetNameSafe(It.Key().Get()), Gap);
		}
		else
		{
			Next.SetLocation(FMath::Lerp(Now.GetLocation(), Goal.GetLocation(), Alpha));
			Next.SetRotation(FQuat::Slerp(Now.GetRotation(), Goal.GetRotation(), Alpha));
		}

		// Arrived at a prop that has stopped — idle until the server moves it again. (A moving
		// target keeps following: its extrapolated goal keeps advancing between samples.)
		if (!PoseMoved(Next, Goal) && T.TargetVelocity.SizeSquared() < 1.0f)
		{
			Next = Goal;
			T.bHasTarget = false;
		}
		Next.SetScale3D(Now.GetScale3D());
		PlaceRootAt(GCC, Next);
	}
}
