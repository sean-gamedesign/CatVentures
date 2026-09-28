// CatPropSyncComponent.cpp — see the header for the model and why it exists.

#include "CatPropSyncComponent.h"
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
	SendAccum     += DeltaTime;
	KeyframeAccum += DeltaTime;
	if (SendAccum < 1.0f / SendRate) return;
	SendAccum = 0.0f;

	const double Now = GetWorld()->GetTimeSeconds();
	const bool bKeyframe = KeyframeAccum >= KeyframeInterval;
	if (bKeyframe) KeyframeAccum = 0.0f;

	TArray<FCatPropPose> Out;
	for (auto It = Props.CreateIterator(); It; ++It)
	{
		AActor* Prop = It.Key().Get();
		FTracked& T  = It.Value();
		UGeometryCollectionComponent* GCC = T.GCC.Get();
		// Broken props leave the registry: from the break on, every machine owns its debris.
		if (!Prop || !GCC || GCC->IsRootBroken()) { It.RemoveCurrent(); continue; }

		const FTransform Pose = GCC->GetRootCurrentTransform();

		// Velocity from consecutive samples (every send tick, sent or not) — the GC's own
		// component velocity is not the prop's (the component never moves on the server).
		FVector Velocity = FVector::ZeroVector;
		if (T.PrevSampleTime >= 0.0 && Now > T.PrevSampleTime)
		{
			Velocity = (Pose.GetLocation() - T.PrevSampleLoc) / static_cast<float>(Now - T.PrevSampleTime);
		}
		T.PrevSampleLoc  = Pose.GetLocation();
		T.PrevSampleTime = Now;

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
