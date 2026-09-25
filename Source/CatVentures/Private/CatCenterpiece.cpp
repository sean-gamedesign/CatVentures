// CatCenterpiece.cpp — see the header for the shape.

#include "CatCenterpiece.h"
#include "CatVenturesLog.h"
#include "CatBase.h"
#include "CatCameraShakes.h"
#include "CatGameMode.h"
#include "CatGameState.h"
#include "CatPlayerController.h"
#include "CatPlayerState.h"
#include "Camera/CameraActor.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollectionProxyData.h"
#include "PhysicsProxy/GeometryCollectionPhysicsProxy.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	const TCHAR* HitKindName(ECatFinaleHitKind Kind)
	{
		switch (Kind)
		{
		case ECatFinaleHitKind::Swat:   return TEXT("swat");
		case ECatFinaleHitKind::Charge: return TEXT("charge");
		case ECatFinaleHitKind::Impact: return TEXT("impact");
		default:                        return TEXT("?");
		}
	}

	/** World-space centre of mass of piece i from the proxy's game-thread particle, falling
	 *  back to the transform pivot. Same helper as CatShatter::PieceWorldCOM in CatBase.cpp —
	 *  GC_Cylinder's pieces all PIVOT at the origin, so the transform is useless for aiming. */
	FVector PieceWorldCOM(const UGeometryCollectionComponent* GCC, int32 i,
	                      const TArray<FTransform3f>& Xf, const FTransform& C2W)
	{
		if (const FGeometryCollectionPhysicsProxy* Proxy = GCC->GetPhysicsProxy())
		{
			if (const FGeometryCollectionPhysicsProxy::FParticle* P = Proxy->GetParticleByIndex_External(i))
			{
				return FVector(P->GetX());
			}
		}
		return Xf.IsValidIndex(i) ? C2W.TransformPosition(FVector(Xf[i].GetLocation())) : C2W.GetLocation();
	}

	const TCHAR* EventName(ECatFinaleEvent Event)
	{
		switch (Event)
		{
		case ECatFinaleEvent::LockedHit:     return TEXT("LockedHit");
		case ECatFinaleEvent::WrongSection:  return TEXT("WrongSection");
		case ECatFinaleEvent::LowHit:        return TEXT("LowHit");
		case ECatFinaleEvent::Hit:           return TEXT("Hit");
		case ECatFinaleEvent::Unlocked:      return TEXT("Unlocked");
		case ECatFinaleEvent::WindowExpired: return TEXT("WindowExpired");
		case ECatFinaleEvent::StageComplete: return TEXT("StageComplete");
		case ECatFinaleEvent::Destroyed:     return TEXT("Destroyed");
		default:                             return TEXT("?");
		}
	}
}

ACatCenterpiece::ACatCenterpiece()
{
	PrimaryActorTick.bCanEverTick = true;

	// Replicated so the multicasts reach every machine and the state struct replicates;
	// always relevant because the readout/HUD must be right from across the block.
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicatingMovement(false);
	SetNetUpdateFrequency(20.0f);

	// Belt-and-braces for any future path that only honours the Heavy tag: the charge path
	// intercepts the actor before that gate, but nothing else should ever pop the shrine.
	Tags.Add(TEXT("HeavyProp"));

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	static ConstructorHelpers::FObjectFinder<UGeometryCollection> DefaultAsset(TEXT("/Game/Blueprints/GC_Cylinder.GC_Cylinder"));
	if (DefaultAsset.Succeeded()) SectionAsset = DefaultAsset.Object;

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> DefaultMaterial(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (DefaultMaterial.Succeeded()) SectionMaterial = DefaultMaterial.Object;

	Sections.SetNum(MaxSections);
	for (int32 i = 0; i < MaxSections; ++i)
	{
		UGeometryCollectionComponent* Section = CreateDefaultSubobject<UGeometryCollectionComponent>(
			*FString::Printf(TEXT("Section%d"), i));
		Section->SetupAttachment(Root);

		// KINEMATIC: immovable until ForceShatterGC's ApplyKinematicField wakes the pieces
		// into dynamic on the shatter — the same call every prop's break already makes.
		Section->ObjectType = EObjectStateTypeEnum::Chaos_Object_Kinematic;
		Section->EnableClustering = true;
		Section->bEnableDamageFromCollision = false;      // only ReceiveHit decides
		if (SectionAsset)
		{
			// Raw property on the CDO — SetRestCollection recreates physics state and is
			// not constructor-safe; OnConstruction calls the real setter on placement.
			Section->RestCollection = SectionAsset.Get();
			Section->DamageThreshold = SectionAsset->DamageThreshold;
		}

		// Same object type the props use, so the swat sweep, the grab sweep and the
		// bulldozer's overlap query all find it. Blocks everything: it is a wall until it
		// isn't (Layer 1 in ForceShatterGC flips the debris off the cat's channels).
		Section->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Section->SetCollisionObjectType(ECC_Destructible);
		Section->SetCollisionResponseToAllChannels(ECR_Block);
		Section->SetGenerateOverlapEvents(false);
		Section->SetNotifyRigidBodyCollision(true);        // OnComponentHit for prop impacts
		Section->SetCanEverAffectNavigation(false);

		Sections[i] = Section;
	}

	Headline = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Headline"));
	Headline->SetupAttachment(Root);
	Headline->SetHorizontalAlignment(EHTA_Center);
	Headline->SetVerticalAlignment(EVRTA_TextBottom);
	Headline->SetWorldSize(70.0f);
	Headline->SetTextRenderColor(FColor(235, 60, 60));
	Headline->SetCastShadow(false);
	Headline->SetText(FText::FromString(TEXT("LOCKED")));

	Detail = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Detail"));
	Detail->SetupAttachment(Root);
	Detail->SetHorizontalAlignment(EHTA_Center);
	Detail->SetVerticalAlignment(EVRTA_TextTop);
	Detail->SetWorldSize(38.0f);
	Detail->SetTextRenderColor(FColor::White);
	Detail->SetCastShadow(false);

	// The beacon: a unit cylinder scaled into a column, hidden until the unlock.
	Beacon = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Beacon"));
	Beacon->SetupAttachment(Root);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderMesh.Succeeded()) Beacon->SetStaticMesh(CylinderMesh.Object);
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BeaconMat(TEXT("/Game/Materials/M_Beacon.M_Beacon"));
	if (BeaconMat.Succeeded()) BeaconMaterial = BeaconMat.Object;
	Beacon->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Beacon->SetCastShadow(false);
	Beacon->SetVisibility(false);
	Beacon->SetRelativeScale3D(FVector(0.01f));
}

// ── Layout ──────────────────────────────────────────────────────────

float ACatCenterpiece::SectionHeight(int32 Index) const
{
	return SectionScales.IsValidIndex(Index) ? SectionAssetHeight * SectionScales[Index] : 0.0f;
}

float ACatCenterpiece::StackTopZ(int32 IntactSections) const
{
	float Z = 0.0f;
	for (int32 i = 0; i < FMath::Min(IntactSections, NumActiveSections()); ++i) Z += SectionHeight(i);
	return Z;
}

float ACatCenterpiece::DistanceToStackAxis(const FVector& Point) const
{
	const FVector Base = GetActorLocation();
	const FVector Top  = Base + FVector(0.0f, 0.0f, StackTopZ(FMath::Max(TopIntactSection() + 1, 1)));
	return FMath::PointDistToSegment(Point, Base, Top);
}

void ACatCenterpiece::LayoutSections()
{
	const int32 Num = NumActiveSections();
	float BaseZ = 0.0f;
	for (int32 i = 0; i < MaxSections; ++i)
	{
		UGeometryCollectionComponent* Section = Sections[i];
		if (!Section) continue;

		const bool bLive = (i < Num);
		Section->SetVisibility(bLive, true);
		Section->SetCollisionEnabled(bLive ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
		if (!bLive) continue;

		const float Scale = SectionScales[i];
		Section->SetRelativeScale3D(FVector(Scale));
		Section->SetRelativeLocation(FVector(0.0f, 0.0f, BaseZ - SectionAssetZMin * Scale));
		BaseZ += SectionHeight(i);
	}

	// Editor-time placement only; Tick re-places the readout for each viewer's camera.
	Headline->SetRelativeLocation(FVector(-(BaseRadius() + ReadoutStandoff), 0.0f, ReadoutHeight + 8.0f));
	Detail->SetRelativeLocation(FVector(-(BaseRadius() + ReadoutStandoff), 0.0f, ReadoutHeight - 8.0f));
}

float ACatCenterpiece::BaseRadius() const
{
	// The asset is a unit cylinder of radius 50 (bounds -50..50); the base is section 0.
	return 50.0f * (SectionScales.Num() > 0 ? SectionScales[0] : 1.0f);
}

void ACatCenterpiece::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// Editor placement / property edits: give every live section the asset through the real
	// setter (it recreates physics state, which is fine here and not in the constructor).
	for (int32 i = 0; i < MaxSections; ++i)
	{
		if (Sections[i] && SectionAsset && Sections[i]->GetRestCollection() != SectionAsset)
		{
			Sections[i]->SetRestCollection(SectionAsset);
			Sections[i]->bEnableDamageFromCollision = false;
			Sections[i]->ObjectType = EObjectStateTypeEnum::Chaos_Object_Kinematic;
		}
	}
	LayoutSections();
}

// ── Lifecycle ───────────────────────────────────────────────────────

void ACatCenterpiece::BeginPlay()
{
	Super::BeginPlay();

	State.NumStages     = NumActiveSections();
	State.UnlockPercent = UnlockChaosPercent;
	State.StageHP       = StageHP;
	SectionTints.Init(LockedTint, MaxSections);

	// Tintable body: one MID per live section. If the material has no such parameter the
	// tint is silently a no-op and the readout still carries the state.
	SectionMIDs.SetNum(MaxSections);
	for (int32 i = 0; i < NumActiveSections(); ++i)
	{
		if (!Sections[i] || !SectionMaterial) continue;
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(SectionMaterial, this);
		MID->SetVectorParameterValue(TintParameterName, LockedTint);
		Sections[i]->SetMaterial(0, MID);
		SectionMIDs[i] = MID;
	}

	if (Beacon && BeaconMaterial)
	{
		BeaconMID = UMaterialInstanceDynamic::Create(BeaconMaterial, this);
		BeaconMID->SetVectorParameterValue(TEXT("Color"), BeaconColor);
		BeaconMID->SetScalarParameterValue(TEXT("Intensity"), 0.0f);
		Beacon->SetMaterial(0, BeaconMID);
		Beacon->SetVisibility(false);
	}

	if (HasAuthority())
	{
		OnTakePointDamage.AddDynamic(this, &ACatCenterpiece::OnSwatDamage);
		for (int32 i = 0; i < NumActiveSections(); ++i)
		{
			if (Sections[i]) Sections[i]->OnComponentHit.AddDynamic(this, &ACatCenterpiece::OnSectionHit);
		}

		if (ACatGameMode* GM = GetWorld()->GetAuthGameMode<ACatGameMode>())
		{
			GM->RegisterCenterpiece(this);
		}
	}

	UE_LOG(LogCatVentures, Log, TEXT("[Finale] '%s' ready — %d stages, unlock at %.0f%% of the meter, window %.1fs, %s"),
		*DisplayName, State.NumStages, UnlockChaosPercent * 100.0f, StageWindow,
		HasAuthority() ? TEXT("authority") : TEXT("client"));

	RefreshReadout();
}

void ACatCenterpiece::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(WindowTimer);
		World->GetTimerManager().ClearTimer(HitStopTimer);
		World->GetTimerManager().ClearTimer(RevealTimer);
		World->GetTimerManager().ClearTimer(RevealCleanupTimer);
		CleanupRevealCameras();
		if (HasAuthority())
		{
			if (ACatGameMode* GM = World->GetAuthGameMode<ACatGameMode>()) GM->UnregisterCenterpiece(this);
		}
	}
	Super::EndPlay(EndPlayReason);
}

void ACatCenterpiece::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ACatCenterpiece, State);
}

void ACatCenterpiece::OnRep_State()
{
	RefreshReadout();
}

// ── Hit intake ──────────────────────────────────────────────────────

int32 ACatCenterpiece::SectionIndexOf(const UPrimitiveComponent* Comp) const
{
	for (int32 i = 0; i < NumActiveSections(); ++i)
	{
		if (Sections[i] == Comp) return i;
	}
	return INDEX_NONE;
}

int32 ACatCenterpiece::SectionIndexAtHeight(float WorldZ) const
{
	// The band containing this height, clamped to the stack. The charge path reports the
	// cat's own position: its paws are a little above its centre, so bias up slightly.
	const float Z = WorldZ - GetActorLocation().Z + 15.0f;
	float Base = 0.0f;
	for (int32 i = 0; i < NumActiveSections(); ++i)
	{
		const float Top = Base + SectionHeight(i);
		if (Z < Top) return i;
		Base = Top;
	}
	return NumActiveSections() - 1;
}

void ACatCenterpiece::OnSwatDamage(AActor* /*DamagedActor*/, float /*Damage*/, AController* InstigatedBy, FVector HitLocation,
                                   UPrimitiveComponent* FHitComponent, FName /*BoneName*/, FVector /*ShotFromDirection*/,
                                   const UDamageType* /*DamageType*/, AActor* DamageCauser)
{
	APlayerState* Attacker = InstigatedBy ? InstigatedBy->PlayerState.Get() : nullptr;
	if (!Attacker)
	{
		if (const APawn* CauserPawn = Cast<APawn>(DamageCauser)) Attacker = CauserPawn->GetPlayerState();
	}
	int32 Section = SectionIndexOf(FHitComponent);
	if (Section == INDEX_NONE) Section = SectionIndexAtHeight(HitLocation.Z);
	ReceiveHit(Attacker, ECatFinaleHitKind::Swat, HitLocation, Section);
}

void ACatCenterpiece::OnSectionHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
                                   FVector /*NormalImpulse*/, const FHitResult& Hit)
{
	if (!HasAuthority() || !OtherActor || !OtherComp) return;
	if (OtherActor->IsA<ACatBase>()) return;                 // the capsule: that is the charge path
	if (!OtherComp->IsSimulatingPhysics()) return;

	const float Mass  = OtherComp->GetMass();
	const float Speed = OtherComp->GetComponentVelocity().Size();
	const float MassBySpeed = Mass * Speed;
	if (MassBySpeed < ImpactMassSpeedThreshold) return;

	ACatGameMode* GM = GetWorld()->GetAuthGameMode<ACatGameMode>();
	APlayerState* Attacker = GM ? GM->ResolveAttacker(OtherActor) : nullptr;

	UE_LOG(LogCatVentures, Log, TEXT("[Finale] impact on '%s' by '%s' — mass %.0f x speed %.0f = %.0f (threshold %.0f) — %s"),
		*DisplayName, *GetNameSafe(OtherActor), Mass, Speed, MassBySpeed, ImpactMassSpeedThreshold,
		Attacker ? *Attacker->GetPlayerName() : TEXT("UNATTRIBUTED — dropped"));

	if (Attacker) ReceiveHit(Attacker, ECatFinaleHitKind::Impact, Hit.ImpactPoint, SectionIndexOf(HitComponent));
}

void ACatCenterpiece::ReceiveHit(APlayerState* Attacker, ECatFinaleHitKind Kind, FVector HitLocation, int32 SectionIndex)
{
	if (!HasAuthority() || State.bDestroyed) return;

	UWorld* World = GetWorld();
	const double Now = World->GetTimeSeconds();
	const FString CatName = Attacker ? Attacker->GetPlayerName() : TEXT("?");

	if (Attacker)
	{
		if (const double* Last = LastHitTimeByCat.Find(Attacker))
		{
			if (Now - *Last < PerCatHitCooldown) return;     // same press, several frames
		}
		LastHitTimeByCat.Add(Attacker, Now);
	}

	if (!State.bUnlocked)
	{
		float Percent = 0.0f;
		if (const ACatGameState* GS = World->GetGameState<ACatGameState>()) Percent = GS->GetChaosPercent();
		UE_LOG(LogCatVentures, Log, TEXT("[Finale] %s by %s while LOCKED (meter %.0f%% / %.0f%%)"),
			HitKindName(Kind), *CatName, Percent * 100.0f, UnlockChaosPercent * 100.0f);
		Multicast_FinaleEvent(ECatFinaleEvent::LockedHit, HitLocation, CatName);
		return;
	}

	// Which tier: the glowing top one is the real dealer, anything lower still counts.
	if (SectionIndex == INDEX_NONE) SectionIndex = SectionIndexAtHeight(HitLocation.Z);
	const int32 Top  = TopIntactSection();

	// A fallen tier is rubble, not shrine (PR-06, 2026-09-24). Its pieces are still that
	// section's component, so the swat sweep (ECC_Destructible) finds them and a swat on the
	// pile used to land as a low hit on the live tier. A charge is different: the owner only
	// sends one after overlapping an INTACT section, and its tier comes from the cat's height,
	// which can read one band high when the cat stands on the top tier's ledge — clamp it.
	if (SectionIndex > Top)
	{
		if (Kind == ECatFinaleHitKind::Charge)
		{
			SectionIndex = Top;
		}
		else
		{
			UE_LOG(LogCatVentures, Log, TEXT("[Finale] %s by %s on fallen section %d REJECTED — rubble, top intact is %d"),
				HitKindName(Kind), *CatName, SectionIndex, Top);
			return;
		}
	}
	const bool  bTop = (SectionIndex == Top);
	if (bRequireTopSectionHit && !bTop)
	{
		UE_LOG(LogCatVentures, Log, TEXT("[Finale] %s by %s on section %d — top intact is %d, HIGHER"),
			HitKindName(Kind), *CatName, SectionIndex, Top);
		Multicast_FinaleEvent(ECatFinaleEvent::WrongSection, HitLocation, CatName);
		return;
	}

	if (!Attacker)
	{
		UE_LOG(LogCatVentures, Log, TEXT("[Finale] unattributed %s dropped — damage needs a CAT to credit"), HitKindName(Kind));
		return;
	}

	// Co-op window: every distinct cat whose last hit is inside it multiplies the damage.
	if (State.WindowEndsAt <= 0.0f) OpenWindow();
	if (!HitSetThisWindow.Contains(Attacker))
	{
		HitSetThisWindow.Add(Attacker);
		State.CatsHitThisWindow.Add(CatName);
	}
	State.WindowEndsAt = Now + StageWindow;
	World->GetTimerManager().SetTimer(WindowTimer, this, &ACatCenterpiece::OnWindowExpired, StageWindow, false);

	const int32 Coop = FMath::Max(HitSetThisWindow.Num(), 1);
	const float Mult = 1.0f + CoopBonusPerCat * (Coop - 1);

	// Presence: who is actually here, out of everyone in the match.
	RecountCats();
	const float Damage = (bTop ? TopHitDamage : LowHitDamage) * Mult * State.PresenceScale;

	State.CoopCats       = Coop;
	State.CoopMultiplier = Mult;
	State.LastHitDamage  = Damage;
	State.bLastHitTop    = bTop;
	State.StageDamage    = FMath::Min(State.StageDamage + Damage, StageHP);
	if (ACatPlayerState* CatPS = Cast<ACatPlayerState>(Attacker)) CatPS->FinaleHits++;

	UE_LOG(LogCatVentures, Log, TEXT("[Finale] %s by %s on section %d (%s) — %.1f dmg x%.1f (%d cat%s in window, %d/%d here, presence x%.2f) -> tier %d/%d at %.0f/%.0f"),
		HitKindName(Kind), *CatName, SectionIndex, bTop ? TEXT("TOP") : TEXT("low"), Damage, Mult, Coop, Coop == 1 ? TEXT("") : TEXT("s"),
		State.CatsNear, State.RequiredCats, State.PresenceScale, State.StagesDone + 1, State.NumStages, State.StageDamage, StageHP);

	Multicast_FinaleEvent(bTop ? ECatFinaleEvent::Hit : ECatFinaleEvent::LowHit, HitLocation, CatName);

	if (ACatGameMode* GM = World->GetAuthGameMode<ACatGameMode>()) GM->NotifyFinaleStageProgress();   // the meter fills with damage

	if (State.StageDamage >= StageHP)
	{
		CompleteStage(HitLocation);
	}
	else
	{
		PushState();
	}
}

// ── Stage machine (server) ──────────────────────────────────────────

bool ACatCenterpiece::RecountCats()
{
	int32 Cats = 0, Near = 0;
	const FVector Centre = GetActorLocation();
	if (const ACatGameState* GS = GetWorld()->GetGameState<ACatGameState>())
	{
		for (const TObjectPtr<APlayerState>& PS : GS->PlayerArray)
		{
			if (!PS || PS->IsInactive() || PS->IsOnlyASpectator()) continue;
			++Cats;
			const APawn* Pawn = PS->GetPawn();
			if (!Pawn) continue;
			FVector D = Pawn->GetActorLocation() - Centre; D.Z = 0.0f;
			if (D.Size() <= ConvergenceRadius) ++Near;
		}
	}
	Cats = FMath::Max(Cats, 1);
	Near = FMath::Clamp(Near, 0, Cats);
	float Scale = 1.0f;
	if (bRequirePresence)
	{
		const float Frac = static_cast<float>(Near) / Cats;
		Scale = FMath::Max(Frac * Frac, PresenceDamageFloor);
	}
	const bool bChanged = (State.RequiredCats != Cats) || (State.CatsNear != Near) || !FMath::IsNearlyEqual(State.PresenceScale, Scale);
	State.RequiredCats  = Cats;
	State.CatsNear      = Near;
	State.PresenceScale = Scale;
	return bChanged;
}

void ACatCenterpiece::OpenWindow()
{
	RecountCats();
	HitSetThisWindow.Reset();
	State.CatsHitThisWindow.Reset();
	State.WindowEndsAt = GetWorld()->GetTimeSeconds() + StageWindow;

	UE_LOG(LogCatVentures, Verbose, TEXT("[Finale] co-op window OPEN — %d cat(s) in the match, %.1fs"),
		State.RequiredCats, StageWindow);
}

void ACatCenterpiece::CloseWindow()
{
	GetWorld()->GetTimerManager().ClearTimer(WindowTimer);
	HitSetThisWindow.Reset();
	State.CatsHitThisWindow.Reset();
	State.WindowEndsAt   = 0.0f;
	State.CoopCats       = 1;
	State.CoopMultiplier = 1.0f;
}

void ACatCenterpiece::OnWindowExpired()
{
	// The co-op multiplier lapses quietly — no failure beat any more (round 6): damage is
	// banked, the cats just lost their bonus until they hit together again.
	if (State.bDestroyed || State.WindowEndsAt <= 0.0f) return;
	UE_LOG(LogCatVentures, Verbose, TEXT("[Finale] co-op window lapsed (%d cat(s) were in it)"), HitSetThisWindow.Num());
	CloseWindow();
	PushState();
}

void ACatCenterpiece::CompleteStage(FVector HitLocation)
{
	CloseWindow();
	State.StageDamage   = 0.0f;
	State.LastHitDamage = 0.0f;

	const int32 SectionIndex = NumActiveSections() - 1 - State.StagesDone;   // top first
	State.StagesDone++;
	const bool bFinal = State.StagesDone >= State.NumStages;

	UE_LOG(LogCatVentures, Log, TEXT("[Finale] === STAGE %d/%d COMPLETE — section %d comes down%s ==="),
		State.StagesDone, State.NumStages, SectionIndex, bFinal ? TEXT(" — THE SHRINE FALLS") : TEXT(""));

	Multicast_ShatterSection(SectionIndex, HitLocation);

	// The meter's last stretch: each stage fills a share of what the house left.
	if (ACatGameMode* GM = GetWorld()->GetAuthGameMode<ACatGameMode>())
	{
		GM->NotifyFinaleStageProgress();
	}

	if (bFinal)
	{
		State.bDestroyed = true;
		PushState();
		Multicast_FinaleEvent(ECatFinaleEvent::Destroyed, HitLocation, FString());
		if (ACatGameMode* GM = GetWorld()->GetAuthGameMode<ACatGameMode>())
		{
			GM->BeginMatchEndFromFinale(this, GetCinematicFocus());
		}
	}
	else
	{
		Multicast_FinaleEvent(ECatFinaleEvent::StageComplete, HitLocation, FString());
		// The hit-stop starts from BurstSection, one tick later, AFTER the impulses are queued:
		// started here it dilated the physics step the burst landed in to ~2 ms, the pieces were
		// not released yet, and the impulses hit kinematic bodies and vanished (2-player round,
		// 2026-09-21: host tiers 2 and 1 crumbled in place at ~150 cm/s while the client, whose
		// dilation arrives a frame after the RPC, burst at full strength).
		PushState();
	}
}

void ACatCenterpiece::SetUnlocked(bool bNewUnlocked)
{
	if (!HasAuthority() || State.bUnlocked == bNewUnlocked || State.bDestroyed) return;
	State.bUnlocked = bNewUnlocked;
	UE_LOG(LogCatVentures, Log, TEXT("[Finale] === '%s' %s ==="), *DisplayName, bNewUnlocked ? TEXT("EXPOSED") : TEXT("re-locked"));
	if (bNewUnlocked)
	{
		Multicast_FinaleEvent(ECatFinaleEvent::Unlocked, GetActorLocation(), FString());
		DoHitStop(UnlockSlowmoDilation, UnlockSlowmoDuration);   // the world holds its breath
	}
	PushState();
}

void ACatCenterpiece::PushState()
{
	ForceNetUpdate();
	RefreshReadout();   // the listen host gets no OnRep
}

void ACatCenterpiece::DoHitStop(float Dilation, float RealSeconds)
{
	if (!HasAuthority() || RealSeconds <= 0.0f) return;
	const ACatGameState* GS = GetWorld()->GetGameState<ACatGameState>();
	if (GS && GS->MatchPhase != ECatMatchPhase::Playing) return;

	Dilation = FMath::Clamp(Dilation, 0.01f, 1.0f);
	UGameplayStatics::SetGlobalTimeDilation(this, Dilation);
	// Dilated timer: X real seconds = X * D game seconds (the match-end scheduling rule).
	GetWorld()->GetTimerManager().SetTimer(HitStopTimer, this, &ACatCenterpiece::RestoreDilation,
		RealSeconds * Dilation, false);
}

void ACatCenterpiece::RestoreDilation()
{
	const ACatGameState* GS = GetWorld()->GetGameState<ACatGameState>();
	if (GS && GS->MatchPhase != ECatMatchPhase::Playing) return;   // the match end owns it now
	UGameplayStatics::SetGlobalTimeDilation(this, 1.0f);
}

// ── Multicasts (every machine) ─────────────────────────────────────

void ACatCenterpiece::Multicast_ShatterSection_Implementation(int32 SectionIndex, FVector HitLocation)
{
	if (!Sections.IsValidIndex(SectionIndex) || !Sections[SectionIndex]) return;

	// Burst from the SECTION's own centre, not the hit point (2026-09-20 round 1/2 logs: a
	// swat lands at paw height, and the scatter burst's 400 cm range never reached the upper
	// sections — 0/21 leaves on the top two, so they slumped instead of blowing apart).
	// Radial from the centre also reads as an explosion rather than a sideways shove.
	const FVector Origin = Sections[SectionIndex]->GetComponentLocation();
	ACatBase::ForceShatterGC(Sections[SectionIndex], Origin);
	RegisterAsDebrisLocally();

	// The shrine's own mass-aware burst, next tick for the same reason ForceShatterGC defers
	// its scatter: a same-frame impulse lands on the still-intact root cluster.
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(
			FTimerDelegate::CreateWeakLambda(this, [this, SectionIndex, Origin]()
			{
				BurstSection(SectionIndex, Origin);
			}));
	}

	// Clients learn StagesDone through replication; the readout's stack-top needs it now
	// for the burst frame, so mirror the count locally on non-authority machines.
	if (!HasAuthority()) State.StagesDone = FMath::Max(State.StagesDone, NumActiveSections() - SectionIndex);
	RefreshReadout();
}

void ACatCenterpiece::Multicast_FinaleEvent_Implementation(ECatFinaleEvent Event, FVector Location, const FString& CatName)
{
	UE_LOG(LogCatVentures, Verbose, TEXT("[Finale] event %s at (%.0f,%.0f,%.0f) cat=%s"),
		EventName(Event), Location.X, Location.Y, Location.Z, *CatName);
	ApplyLocalFeedback(Event, Location, CatName);
}

void ACatCenterpiece::ApplyLocalFeedback(ECatFinaleEvent Event, FVector Location, const FString& /*CatName*/)
{
	const float Now = GetWorld()->GetTimeSeconds();
	PunchDecay = 8.0f;
	switch (Event)
	{
	case ECatFinaleEvent::LockedHit:
		ShakeLocalPlayers(UCatShrineHitShake::StaticClass(), 0.6f, Location);
		ReadoutPunch = 0.35f;
		FlashAmount  = 0.25f;
		break;

	case ECatFinaleEvent::WrongSection:
		ShakeLocalPlayers(UCatShrineHitShake::StaticClass(), 0.5f, Location);
		ReadoutPunch   = 0.6f;
		OverrideHead   = TEXT("HIGHER!");
		OverrideDetail = FString::Printf(TEXT("HIT THE TOP SECTION   STAGE %d / %d"), State.StagesDone + 1, State.NumStages);
		OverrideColor  = FColor(120, 200, 255);
		OverrideUntil  = Now + 1.0f;
		break;

	case ECatFinaleEvent::LowHit:
		ShakeLocalPlayers(UCatShrineHitShake::StaticClass(), 0.6f, Location);
		ReadoutPunch   = 0.4f;
		FlashAmount    = 0.25f;
		OverrideHead   = TEXT("THE GLOWING TIER HITS HARDER");
		OverrideDetail.Reset();
		OverrideColor  = FColor(120, 200, 255);
		OverrideUntil  = Now + 0.9f;
		break;

	case ECatFinaleEvent::Hit:
		ShakeLocalPlayers(UCatShrineHitShake::StaticClass(), 1.0f, Location);
		ReadoutPunch = 0.6f;
		FlashAmount  = 0.7f;
		if (State.CatsNear < State.RequiredCats)
			OverrideHead = FString::Printf(TEXT("TOP HIT   x%.1f  (%d / %d HERE)"), State.PresenceScale, State.CatsNear, State.RequiredCats);
		else if (State.CoopCats > 1)
			OverrideHead = FString::Printf(TEXT("TOP HIT   x%.1f"), State.CoopMultiplier);
		else
			OverrideHead = TEXT("TOP HIT");
		OverrideDetail.Reset();
		OverrideColor  = FColor(255, 240, 120);
		OverrideUntil  = Now + 0.5f;
		break;

	case ECatFinaleEvent::Unlocked:
		// The beat: everyone rumbles wherever they are, the yard erupts, the headline lands
		// oversized and settles, the body flashes white and holds before going gold. The
		// server adds the slow-mo pulse on top (SetUnlocked).
		ShakeLocalPlayers(UCatShrineStageShake::StaticClass(), UnlockShakeScale, Location, /*bIgnoreFalloff=*/ true);
		EruptNearbyProps();
		if (bUnlockRevealCut) StartUnlockReveal();
		BeaconPulse    = 1.0f;
		ReadoutPunch   = UnlockReadoutPunch;
		PunchDecay     = UnlockReadoutSettleRate;
		FlashAmount    = 1.0f;
		FlashHoldUntil = Now + UnlockFlashHold;
		break;

	case ECatFinaleEvent::WindowExpired:
		ReadoutPunch   = 0.3f;
		OverrideHead   = TEXT("AGAIN - ALL TOGETHER");
		OverrideDetail = FString::Printf(TEXT("STAGE %d / %d   NEEDS %d CATS ON TOP"), State.StagesDone + 1, State.NumStages, State.RequiredCats);
		OverrideColor  = FColor(255, 120, 40);
		OverrideUntil  = Now + 1.2f;
		break;

	case ECatFinaleEvent::StageComplete:
		ShakeLocalPlayers(UCatShrineStageShake::StaticClass(), 1.0f, Location);
		ReadoutPunch = 1.0f;
		FlashAmount  = 1.0f;
		break;

	case ECatFinaleEvent::Destroyed:
		ShakeLocalPlayers(UCatShrineStageShake::StaticClass(), 1.6f, Location);
		ReadoutPunch = 1.2f;
		FlashAmount  = 1.0f;
		break;

	default:
		break;
	}
	RefreshReadout();
}

void ACatCenterpiece::ShakeLocalPlayers(TSubclassOf<UCameraShakeBase> ShakeClass, float Scale, FVector Epicenter, bool bIgnoreFalloff)
{
	if (!ShakeClass) return;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC || !PC->IsLocalController() || !PC->PlayerCameraManager) continue;

		float Falloff = 1.0f;
		if (!bIgnoreFalloff)
		{
			const float Dist = FVector::Dist(PC->PlayerCameraManager->GetCameraLocation(), Epicenter);
			Falloff = 1.0f - FMath::Clamp((Dist - 300.0f) / ShakeRadius, 0.0f, 1.0f);
		}
		if (Falloff <= 0.0f) continue;
		PC->PlayerCameraManager->StartCameraShake(ShakeClass, Scale * Falloff);
	}
}

void ACatCenterpiece::EruptNearbyProps()
{
	// Per machine, like the bulldozer: props and chunks live in each machine's own solver, so
	// each copy of the shrine shoves that machine's copies. Intact props get ONE mass-capped
	// impulse at their centre (the bump-push model — a vase hops, a fridge rocks); fractured
	// debris gets the per-chunk proxy impulse the bulldozer uses. Cats are not in the query.
	UWorld* World = GetWorld();
	if (!World || UnlockEruptionRadius <= 0.0f) return;

	const FVector Centre = GetActorLocation();
	FCollisionObjectQueryParams ObjParams;
	ObjParams.AddObjectTypesToQuery(ECC_Destructible);
	ObjParams.AddObjectTypesToQuery(ECC_PhysicsBody);
	FCollisionQueryParams QParams(FName(TEXT("ShrineEruption")), /*bTraceComplex=*/ false, this);

	TArray<FOverlapResult> Overlaps;
	World->OverlapMultiByObjectType(Overlaps, Centre, FQuat::Identity, ObjParams,
		FCollisionShape::MakeSphere(UnlockEruptionRadius), QParams);

	constexpr float UpBias = 0.35f;
	constexpr float RefMass = 250.0f;   // BumpPushRefMass's value — same cap, same feel
	int32 Props = 0, Chunks = 0;
	TSet<UPrimitiveComponent*> Done;

	for (const FOverlapResult& O : Overlaps)
	{
		UPrimitiveComponent* Comp = O.GetComponent();
		if (!Comp || Comp->GetOwner() == this || Done.Contains(Comp) || !Comp->IsSimulatingPhysics()) continue;
		Done.Add(Comp);

		if (UGeometryCollectionComponent* GCC = Cast<UGeometryCollectionComponent>(Comp))
		{
			const bool bIntact = (GCC->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Overlap);
			GCC->WakeAllRigidBodies();
			if (bIntact)
			{
				const FVector Loc = GCC->GetComponentLocation();
				FVector Dir = Loc - Centre; Dir.Z = 0.0f;
				if (!Dir.Normalize()) Dir = FVector::ForwardVector;
				Dir += FVector::UpVector * UpBias;
				const float Mass = FMath::Max(GCC->GetMass(), 1.0f);
				GCC->AddImpulseAtLocation(Dir * FMath::Min(Mass, RefMass) * UnlockEruptionSpeed, Loc);
				++Props;
			}
			else
			{
				const TArray<FTransform3f>& ChunkXf = GCC->GetComponentSpaceTransforms3f();
				const FTransform CompToWorld = GCC->GetComponentTransform();
				for (const FTransform3f& X : ChunkXf)
				{
					const FVector ChunkW = CompToWorld.TransformPosition(FVector(X.GetLocation()));
					if (FVector::DistSquared(ChunkW, Centre) > FMath::Square(UnlockEruptionRadius)) continue;
					FVector Dir = ChunkW - Centre; Dir.Z = 0.0f;
					if (!Dir.Normalize()) Dir = FVector::ForwardVector;
					Dir += FVector::UpVector * UpBias;
					GCC->AddImpulseAtLocation(Dir * UnlockEruptionDebrisImpulse, ChunkW);
					++Chunks;
				}
			}
		}
		else
		{
			FVector Dir = Comp->GetComponentLocation() - Centre; Dir.Z = 0.0f;
			if (!Dir.Normalize()) Dir = FVector::ForwardVector;
			Dir += FVector::UpVector * UpBias;
			Comp->AddImpulse(Dir * UnlockEruptionSpeed, NAME_None, /*bVelChange=*/ true);
			++Props;
		}
	}

	UE_LOG(LogCatVentures, Log, TEXT("[Finale] eruption — %d prop(s), %d chunk(s) shoved within %.0f cm"),
		Props, Chunks, UnlockEruptionRadius);
}

void ACatCenterpiece::BurstSection(int32 SectionIndex, FVector Origin, int32 Attempt)
{
	if (!Sections.IsValidIndex(SectionIndex)) return;
	UGeometryCollectionComponent* GCC = Sections[SectionIndex];
	if (!GCC || !IsValid(GCC)) return;

	const UGeometryCollection* Rest = GCC->GetRestCollection();
	const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> Coll = Rest ? Rest->GetGeometryCollection() : nullptr;
	const int32 NumSim = Coll.IsValid() ? Coll->SimulationType.Num() : 0;

	const TArray<FTransform3f>& ChunkXf = GCC->GetComponentSpaceTransforms3f();
	const FTransform CompToWorld = GCC->GetComponentTransform();

	int32 Leaves = 0, Broken = 0;
	FGeometryDynamicCollection* Dyn = GCC->GetDynamicCollection();
	for (int32 i = 0; i < ChunkXf.Num(); ++i)
	{
		if (i < NumSim && !Coll->IsRigid(i)) continue;
		++Leaves;
		if (Dyn)
		{
			FGeometryCollectionDynamicStateFacade Facade(*Dyn);
			if (Facade.HasBrokenOff(i)) ++Broken;
		}
	}
	if (Leaves == 0) return;

	// READINESS (2-player rounds, 2026-09-21): whether the physics thread has processed the
	// break by "next tick" depends on where in the frame the shatter was requested — the host,
	// shattering from inside a swat notify, lost the burst on one tier per run (impulses on
	// still-kinematic pieces vanish) while the client, whose RPC lands at frame start, never
	// did. So: only burst once the pieces report as broken off; otherwise re-arm next tick.
	constexpr int32 MaxAttempts = 12;
	if (Dyn && Broken < (Leaves + 1) / 2 && Attempt < MaxAttempts)
	{
		if (UWorld* World = GetWorld())
		{
			const int32 Next = Attempt + 1;
			World->GetTimerManager().SetTimerForNextTick(
				FTimerDelegate::CreateWeakLambda(this, [this, SectionIndex, Origin, Next]()
				{
					BurstSection(SectionIndex, Origin, Next);
				}));
		}
		UE_LOG(LogCatVentures, Verbose, TEXT("[Finale] section %d not broken yet (%d/%d) — burst re-armed, attempt %d"),
			SectionIndex, Broken, Leaves, Attempt + 1);
		return;
	}

	// Round-4 read ("bits fell but it didn't crumble") with every impulse logged as landing:
	// the released pieces were still KINEMATIC. ObjectType=Kinematic is the state of EVERY
	// particle in the collection, and ForceShatterGC's dynamic-state field ran before the
	// break, on the root cluster — the children it released kept their own kinematic state
	// and hung where they were. So: set the released pieces dynamic NOW, wake them, then
	// impulse; and once more shortly after for anything the propagation released late.
	const float FieldRadius = SectionHeight(SectionIndex) + BaseRadius() + 150.0f;
	GCC->ApplyKinematicField(FieldRadius, Origin);
	GCC->WakeAllRigidBodies();
	if (UWorld* World = GetWorld())
	{
		FTimerHandle Straggler;
		World->GetTimerManager().SetTimer(Straggler, FTimerDelegate::CreateWeakLambda(GCC, [GCC, FieldRadius, Origin]()
		{
			GCC->ApplyKinematicField(FieldRadius, Origin);
			GCC->WakeAllRigidBodies();
		}), 0.35f, false);
	}

	// GetMass() on a Geometry Collection reports the ASSET's unscaled mass whatever the
	// component scale (verified 2026-09-20: identical for scale 1.0, 1.6 and 2.6), so the
	// section's real mass is that times scale cubed; a chunk is its share of that.
	const float Scale       = SectionScales.IsValidIndex(SectionIndex) ? SectionScales[SectionIndex] : 1.0f;
	const float SectionMass = FMath::Max(GCC->GetMass(), 1.0f) * Scale * Scale * Scale;
	const float ChunkMass   = SectionMass / Leaves;
	const float Impulse     = ChunkMass * ShrineBurstSpeed;

	int32 Hit = 0, Leaf = 0;
	for (int32 i = 0; i < ChunkXf.Num(); ++i)
	{
		if (i < NumSim && !Coll->IsRigid(i)) continue;
		const FVector ChunkW = PieceWorldCOM(GCC, i, ChunkXf, CompToWorld);

		FVector Dir = ChunkW - Origin;
		Dir.Z = 0.0f;
		if (!Dir.Normalize())
		{
			// A chunk sitting on the axis: fan it out by index so the core doesn't all go one way.
			const float A = Leaf * 2.399963f;   // golden angle
			Dir = FVector(FMath::Cos(A), FMath::Sin(A), 0.0f);
		}
		Dir += FVector::UpVector * ShrineBurstUpBias;
		GCC->AddImpulseAtLocation(Dir * Impulse, ChunkW);
		++Hit; ++Leaf;
	}

	UE_LOG(LogCatVentures, Log, TEXT("[Finale] section %d burst — %d/%d leaves (%d broken off, attempt %d), chunk ~%.0f kg, %.0f cm/s target, impulse %.0f"),
		SectionIndex, Hit, Leaves, Broken, Attempt, ChunkMass, ShrineBurstSpeed, Impulse);

	// Now the hit-stop (server only; skipped on the final tier, where the match end owns
	// dilation). The impulses above are already queued for a full-length physics step.
	if (!State.bDestroyed) DoHitStop(HitStopDilation, HitStopDuration);

	// Diagnostic (2-player parity, 2026-09-21): how fast are the pieces ACTUALLY moving half a
	// second after the burst on THIS machine? Sean read the client's burst as weaker than the
	// host's while both logged identical impulses — measure before guessing.
	if (UWorld* World = GetWorld())
	{
		FTimerHandle Probe;
		const int32 Idx = SectionIndex;
		World->GetTimerManager().SetTimer(Probe, FTimerDelegate::CreateWeakLambda(GCC, [GCC, Idx]()
		{
			const FGeometryCollectionPhysicsProxy* Proxy = GCC->GetPhysicsProxy();
			const int32 N = GCC->GetComponentSpaceTransforms3f().Num();
			float Sum = 0.0f, Max = 0.0f;
			int32 Moving = 0, Counted = 0;
			for (int32 i = 0; Proxy && i < N; ++i)
			{
				const FGeometryCollectionPhysicsProxy::FParticle* P = Proxy->GetParticleByIndex_External(i);
				if (!P) continue;
				const float S = FVector(P->GetV()).Size();
				Sum += S; Max = FMath::Max(Max, S); ++Counted;
				if (S > 100.0f) ++Moving;
			}
			const UWorld* W = GCC->GetWorld();
			const TCHAR* Tag = !W ? TEXT("?") : (W->GetNetMode() == NM_Client) ? TEXT("CLI") : (W->GetNetMode() == NM_ListenServer) ? TEXT("SRV") : TEXT("SA");
			UE_LOG(LogCatVentures, Log, TEXT("[Finale] %s section %d +0.5s: %d/%d pieces moving >100 cm/s, mean %.0f, max %.0f cm/s"),
				Tag, Idx, Moving, Counted, Counted ? Sum / Counted : 0.0f, Max);
		}), 0.5f, false);
	}
}

void ACatCenterpiece::UpdateBeacon(float DeltaTime)
{
	if (!Beacon) return;

	const bool bLit = State.bUnlocked && !State.bDestroyed;
	const float RiseRate = 1.0f / FMath::Max(BeaconRiseTime, 0.05f);
	BeaconRise = FMath::Clamp(BeaconRise + (bLit ? RiseRate : -RiseRate * 2.0f) * DeltaTime, 0.0f, 1.0f);
	BeaconPulse = FMath::FInterpTo(BeaconPulse, 0.0f, DeltaTime, 1.5f);

	if (BeaconRise <= 0.0f)
	{
		if (Beacon->IsVisible()) Beacon->SetVisibility(false);
		return;
	}
	if (!Beacon->IsVisible()) Beacon->SetVisibility(true);

	// Unit cylinder (100 tall, centred): scale Z to the risen height and lift by half of it.
	const float Height = BeaconHeight * BeaconRise;
	const float Radius = BeaconRadius * (1.0f + BeaconPulse * 1.5f);   // fat at the flash, slim at idle
	Beacon->SetRelativeScale3D(FVector(Radius / 50.0f, Radius / 50.0f, Height / 100.0f));
	Beacon->SetRelativeLocation(FVector(0.0f, 0.0f, Height * 0.5f));

	if (BeaconMID)
	{
		const float Breathe = 1.0f + 0.2f * FMath::Sin(GetWorld()->GetTimeSeconds() * 2.5f);
		const float Intensity = FMath::Lerp(BeaconIdleIntensity * Breathe, BeaconPeakIntensity, BeaconPulse) * BeaconRise;
		BeaconMID->SetScalarParameterValue(TEXT("Intensity"), Intensity);
	}
}

void ACatCenterpiece::StartUnlockReveal()
{
	CleanupRevealCameras();

	const FVector Focus  = GetActorLocation() + FVector(0.0f, 0.0f, StackTopZ(NumActiveSections()) * 0.5f);
	const FVector CamLoc = GetActorLocation() + UnlockRevealOffset;
	const FRotator CamRot = UKismetMathLibrary::FindLookAtRotation(CamLoc, Focus);

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		ACatPlayerController* PC = Cast<ACatPlayerController>(It->Get());
		if (!PC || !PC->IsLocalController() || !PC->GetPawn()) continue;

		// Spawns at the pawn's lens pose and blends the view target to the camera; moving the
		// camera to the shot right away makes that blend travel from the cat to the shrine.
		ACameraActor* Cam = PC->SpawnCinematicTrackerCamera(UnlockRevealBlendIn);
		if (!Cam) continue;
		Cam->SetActorLocationAndRotation(CamLoc, CamRot);
		RevealCameras.Add(Cam);
	}

	if (RevealCameras.Num() > 0)
	{
		GetWorld()->GetTimerManager().SetTimer(RevealTimer, this, &ACatCenterpiece::EndUnlockReveal,
			UnlockRevealBlendIn + UnlockRevealHold, false);
		UE_LOG(LogCatVentures, Log, TEXT("[Finale] reveal cut — %d local camera(s), hold %.1fs"), RevealCameras.Num(), UnlockRevealHold);
	}
}

void ACatCenterpiece::EndUnlockReveal()
{
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC || !PC->IsLocalController()) continue;
		if (APawn* Pawn = PC->GetPawn())
		{
			PC->SetViewTargetWithBlend(Pawn, UnlockRevealBlendOut);
		}
	}
	GetWorld()->GetTimerManager().SetTimer(RevealCleanupTimer, this, &ACatCenterpiece::CleanupRevealCameras,
		UnlockRevealBlendOut + 0.1f, false);
}

void ACatCenterpiece::CleanupRevealCameras()
{
	for (const TWeakObjectPtr<ACameraActor>& Cam : RevealCameras)
	{
		if (Cam.IsValid()) Cam->Destroy();
	}
	RevealCameras.Reset();
}

void ACatCenterpiece::RegisterAsDebrisLocally()
{
	if (bRegisteredDebrisLocally) return;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (ACatPlayerController* PC = Cast<ACatPlayerController>(It->Get()))
		{
			if (PC->IsLocalController()) { PC->RegisterDebrisActor(this); bRegisteredDebrisLocally = true; }
		}
	}
}

// ── Readout / cosmetics (every machine, every tick) ────────────────

float ACatCenterpiece::WindowRemaining() const
{
	if (State.WindowEndsAt <= 0.0f) return 0.0f;
	const ACatGameState* GS = GetWorld()->GetGameState<ACatGameState>();
	const float Now = GS ? GS->GetServerWorldTimeSeconds() : GetWorld()->GetTimeSeconds();
	return FMath::Max(State.WindowEndsAt - Now, 0.0f);
}

FVector ACatCenterpiece::GetCinematicFocus() const
{
	const int32 Intact = FMath::Max(NumActiveSections() - State.StagesDone, 0);
	const float Z = (Intact > 0) ? StackTopZ(Intact) - SectionHeight(Intact - 1) * 0.5f : SectionHeight(0) * 0.5f;
	return GetActorLocation() + FVector(0.0f, 0.0f, Z);
}

FText ACatCenterpiece::GetHudText(float ChaosPercent) const
{
	FString Line;
	if (State.bDestroyed)
	{
		Line = FString::Printf(TEXT("%s  [SMASHED]"), *DisplayName);
	}
	else if (!State.bUnlocked)
	{
		Line = FString::Printf(TEXT("%s  [LOCKED]   wreck the house: %.0f%% / %.0f%%"),
			*DisplayName, ChaosPercent * 100.0f, State.UnlockPercent * 100.0f);
	}
	else
	{
		const int32 Pct = FMath::RoundToInt(State.StageDamage / FMath::Max(State.StageHP, 1.0f) * 100.0f);
		Line = FString::Printf(TEXT("%s  [EXPOSED]   tier %d/%d at %d%% — hit the GLOWING tier — %d/%d cats here%s"),
			*DisplayName, State.StagesDone + 1, State.NumStages, Pct,
			State.CatsNear, State.RequiredCats,
			(State.CatsNear < State.RequiredCats) ? TEXT(" — GET EVERYONE HERE") : TEXT(""));
	}
	return FText::FromString(Line);
}

void ACatCenterpiece::RefreshReadout()
{
	if (!Headline || !Detail) return;

	const ACatGameState* GS = GetWorld() ? GetWorld()->GetGameState<ACatGameState>() : nullptr;
	const float Percent = GS ? GS->GetChaosPercent() : 0.0f;

	FString Head, Det;
	FColor HeadColor;

	if (State.bDestroyed)
	{
		Head = TEXT("SMASHED");
		Det  = TEXT("");
		HeadColor = FColor(80, 230, 90);
	}
	else if (!State.bUnlocked)
	{
		Head = TEXT("LOCKED");
		Det  = FString::Printf(TEXT("WRECK THE HOUSE   %.0f%% / %.0f%%"), Percent * 100.0f, State.UnlockPercent * 100.0f);
		HeadColor = FColor(235, 60, 60);
	}
	else
	{
		// Tier health bar + the co-op multiplier while a window is live.
		const float Frac = State.StageDamage / FMath::Max(State.StageHP, 1.0f);
		const int32 Filled = FMath::Clamp(FMath::RoundToInt(Frac * 10.0f), 0, 10);
		FString Bar = TEXT("[");
		for (int32 i = 0; i < 10; ++i) Bar += (i < Filled) ? TEXT("#") : TEXT("-");
		Bar += TEXT("]");

		const bool bEveryoneHere = State.CatsNear >= State.RequiredCats;
		Head = bEveryoneHere ? TEXT("SMASH THE GLOWING TIER") : TEXT("GET EVERYONE HERE");
		HeadColor = bEveryoneHere ? FColor(255, 200, 40) : FColor(255, 110, 60);
		if (!bEveryoneHere)
		{
			Det = FString::Printf(TEXT("%s %d%%   TIER %d / %d   %d / %d CATS HERE"), *Bar, FMath::RoundToInt(Frac * 100.0f),
				State.StagesDone + 1, State.NumStages, State.CatsNear, State.RequiredCats);
		}
		else if (State.WindowEndsAt > 0.0f && WindowRemaining() > 0.0f && State.CoopCats > 1)
		{
			Det = FString::Printf(TEXT("%s %d%%   TIER %d / %d   %d CATS x%.1f"), *Bar, FMath::RoundToInt(Frac * 100.0f),
				State.StagesDone + 1, State.NumStages, State.CoopCats, State.CoopMultiplier);
		}
		else if (State.RequiredCats > 1)
		{
			Det = FString::Printf(TEXT("%s %d%%   TIER %d / %d   HIT TOGETHER FOR x%.1f"), *Bar, FMath::RoundToInt(Frac * 100.0f),
				State.StagesDone + 1, State.NumStages, 1.0f + CoopBonusPerCat * (State.RequiredCats - 1));
		}
		else
		{
			Det = FString::Printf(TEXT("%s %d%%   TIER %d / %d"), *Bar, FMath::RoundToInt(Frac * 100.0f),
				State.StagesDone + 1, State.NumStages);
		}

		// A timed override ("TOP HIT x1.5", "THE GLOWING TIER HITS HARDER") replaces the
		// headline, and the detail only when it brought one — the health bar stays otherwise.
		if (GetWorld()->GetTimeSeconds() < OverrideUntil)
		{
			Head = OverrideHead;
			HeadColor = OverrideColor;
			if (!OverrideDetail.IsEmpty()) Det = OverrideDetail;
		}
	}

	if (Head != LastHeadline) { Headline->SetText(FText::FromString(Head)); Headline->SetTextRenderColor(HeadColor); LastHeadline = Head; }
	if (Det  != LastDetail)   { Detail->SetText(FText::FromString(Det)); LastDetail = Det; }
}

void ACatCenterpiece::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Server: keep the presence count live between hits so "N / M CATS HERE" tracks the cats
	// walking up, not just the last swat.
	if (HasAuthority() && State.bUnlocked && !State.bDestroyed)
	{
		PresenceRecountTimer += DeltaTime;
		if (PresenceRecountTimer >= 0.25f)
		{
			PresenceRecountTimer = 0.0f;
			if (RecountCats()) PushState();
		}
	}

	if (GetNetMode() == NM_DedicatedServer) return;

	// Readout sits at cat height on the VIEWER's side of the base, just off its face, and
	// faces the local camera — so it is in front of the section, never inside it, for
	// whoever is looking. Falls back to the editor-time placement with no camera.
	const FVector ActorLoc = GetActorLocation();
	FVector CamLoc = FVector::ZeroVector;
	bool bHaveCam = false;
	if (const APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		if (PC->PlayerCameraManager) { CamLoc = PC->PlayerCameraManager->GetCameraLocation(); bHaveCam = true; }
	}

	ReadoutPunch = FMath::FInterpTo(ReadoutPunch, 0.0f, DeltaTime, PunchDecay);
	const float Scale = 1.0f + ReadoutPunch * 0.5f;

	FVector ToCam = FVector::BackwardVector;
	if (bHaveCam)
	{
		ToCam = CamLoc - ActorLoc; ToCam.Z = 0.0f;
		if (!ToCam.Normalize()) ToCam = FVector::BackwardVector;
	}
	const FVector ReadoutBase = ActorLoc + ToCam * (BaseRadius() + ReadoutStandoff);

	for (UTextRenderComponent* Text : { Headline.Get(), Detail.Get() })
	{
		if (!Text) continue;
		const float Offset = (Text == Headline) ? 8.0f : -8.0f;
		Text->SetWorldLocation(ReadoutBase + FVector(0.0f, 0.0f, ReadoutHeight + Offset));
		if (bHaveCam)
		{
			FRotator Face = UKismetMathLibrary::FindLookAtRotation(Text->GetComponentLocation(), CamLoc);
			Face.Pitch = 0.0f; Face.Roll = 0.0f;
			Text->SetWorldRotation(Face);
		}
		Text->SetXScale(Scale);
		Text->SetYScale(Scale);
	}

	// Window countdown and a timed override both need per-tick text (and the tick after an
	// override lapses, so the readout falls back).
	if (State.WindowEndsAt > 0.0f || OverrideUntil > GetWorld()->GetTimeSeconds() - 0.2f) RefreshReadout();

	UpdateBeacon(DeltaTime);

	// Body tint per tier: the current TOP tier glows and pulses (that is where the damage
	// is), the tiers below sit dim, everything is dark while locked. A white flash rides on
	// top of all of them and (on the unlock) holds before decaying.
	const float NowS = GetWorld()->GetTimeSeconds();
	if (NowS >= FlashHoldUntil)
	{
		FlashAmount = FMath::FInterpTo(FlashAmount, 0.0f, DeltaTime, 5.0f);
	}
	const int32 Top   = TopIntactSection();
	const float Pulse = 0.85f + 0.35f * FMath::Sin(NowS * 5.0f);
	if (SectionTints.Num() < MaxSections) SectionTints.Init(LockedTint, MaxSections);
	for (int32 i = 0; i < NumActiveSections(); ++i)
	{
		FLinearColor Target;
		if      (State.bDestroyed) Target = DestroyedTint;
		else if (!State.bUnlocked) Target = LockedTint;
		else if (i == Top)         Target = ArmedTint * Pulse;
		else if (i < Top)          Target = LowerTierTint;
		else                       Target = LockedTint;   // already fallen — chunks keep the last colour
		SectionTints[i] = FMath::CInterpTo(SectionTints[i], Target, DeltaTime, 5.0f);
		if (UMaterialInstanceDynamic* MID = SectionMIDs.IsValidIndex(i) ? SectionMIDs[i].Get() : nullptr)
		{
			MID->SetVectorParameterValue(TintParameterName, FMath::Lerp(SectionTints[i], FLinearColor::White, FlashAmount));
		}
	}
}
