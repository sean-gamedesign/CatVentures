#include "CatTraversalComponent.h"

#include "CatBase.h"
#include "CatVenturesLog.h"
#include "PawPrintSubsystem.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"

UCatTraversalComponent::UCatTraversalComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

// ══════════════════════════════════════════════════════════════════════════
// ── Detection library (shared by every traversal verb) ────────────────────
// ══════════════════════════════════════════════════════════════════════════

const TCHAR* UCatTraversalComponent::RejectToString(ETraversalReject R)
{
	switch (R)
	{
	case ETraversalReject::None:        return TEXT("armed");
	case ETraversalReject::Disabled:    return TEXT("disabled");
	case ETraversalReject::Cooldown:    return TEXT("cooldown");
	case ETraversalReject::NotAirborne: return TEXT("not airborne");
	case ETraversalReject::Grabbing:    return TEXT("grabbing");
	case ETraversalReject::NoInput:     return TEXT("no movement input");
	case ETraversalReject::NoFacing:    return TEXT("no facing");
	case ETraversalReject::NoWall:      return TEXT("no wall in reach");
	case ETraversalReject::WallTooFlat: return TEXT("surface too flat (not a wall)");
	case ETraversalReject::NoLip:       return TEXT("no lip past the wall face");
	case ETraversalReject::LipNotFloor: return TEXT("lip surface too steep to stand on");
	case ETraversalReject::LipTooLow:   return TEXT("ledge below the band");
	case ETraversalReject::LipTooHigh:  return TEXT("ledge above the band");
	case ETraversalReject::NoHeadroom:  return TEXT("no headroom at the landing spot");
	default:                            return TEXT("?");
	}
}

const TCHAR* UCatTraversalComponent::WallAttachEndToString(EWallAttachEnd R)
{
	switch (R)
	{
	case EWallAttachEnd::Kick:     return TEXT("kick");
	case EWallAttachEnd::Mantled:  return TEXT("mantled");
	case EWallAttachEnd::Timeout:  return TEXT("timeout");
	case EWallAttachEnd::WallLost: return TEXT("wall lost");
	case EWallAttachEnd::Grounded: return TEXT("grounded");
	case EWallAttachEnd::Grabbed:  return TEXT("grabbed");
	case EWallAttachEnd::Remote:   return TEXT("remote");
	case EWallAttachEnd::Aborted:  return TEXT("aborted");
	default:                       return TEXT("?");
	}
}

// Sustained rejects are the common case (you spend whole jumps not near a ledge), so
// logging every frame would drown the category. Log on CHANGE; sample the channel every
// frame so a PIE session leaves a reject histogram rather than an impression.
void UCatTraversalComponent::NoteReject(ETraversalReject R)
{
	ACatBase* Cat = GetCat();
	if (R != LastReject)
	{
		if (R != ETraversalReject::None)
		{
			UE_LOG(LogCatVentures, Verbose, TEXT("[%s] Traversal detect — %s"),
				Cat ? *Cat->GetName() : TEXT("?"), RejectToString(R));
		}
		LastReject = R;
	}
	if (Cat && Cat->PawPrint)
	{
		static const FName ChReject(TEXT("MantleReject"));
		Cat->PawPrint->SampleChannel(ChReject, static_cast<float>(R));
	}
}

// Radial chest probe. NumDirections 1 = forward only (the mantle: a ledge is always
// ahead of the facing); 4 = forward/right/back/left (the bounce: the wall can be on
// either side — that is what makes chimney climbing work). Returns the NEAREST
// wall-like hit; bOutAnyHit distinguishes "nothing there" from "hit something flat",
// which is the difference between two very different reject reasons.
bool UCatTraversalComponent::ProbeWalls(float Reach, int32 NumDirections,
	FVector& OutPoint, FVector& OutNormal, bool& bOutAnyHit, const FVector& BasisDir,
	FHitResult* OutHit) const
{
	bOutAnyHit = false;
	const ACatBase* Cat = GetCat();
	if (!Cat || !GetWorld())
	{
		return false;
	}

	FVector Fwd = BasisDir.IsNearlyZero() ? Cat->GetActorForwardVector() : BasisDir;
	Fwd.Z = 0.0f;
	if (!Fwd.Normalize())
	{
		return false;
	}

	const FVector Center = Cat->GetActorLocation();
	FCollisionQueryParams Params(FName(TEXT("CatTraversalWall")), false, Cat);

	bool bFound = false;
	float BestDistSq = TNumericLimits<float>::Max();
	for (int32 i = 0; i < FMath::Max(NumDirections, 1); ++i)
	{
		const FVector Dir = Fwd.RotateAngleAxis(90.0f * i, FVector::UpVector);
		FHitResult Hit;
		if (!GetWorld()->LineTraceSingleByChannel(Hit, Center, Center + Dir * Reach,
			ECC_Visibility, Params))
		{
			continue;
		}
		bOutAnyHit = true;
		if (Hit.ImpactNormal.Z >= 0.5f)   // floor/ramp, not a wall face
		{
			continue;
		}
		const float DistSq = FVector::DistSquared(Center, Hit.ImpactPoint);
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			OutPoint   = Hit.ImpactPoint;
			OutNormal  = Hit.ImpactNormal;
			bFound     = true;
			if (OutHit)
			{
				*OutHit = Hit;   // the scramble needs the actor/component for its surface gate
			}
		}
	}
	return bFound;
}

ACatBase* UCatTraversalComponent::GetCat() const
{
	return Cast<ACatBase>(GetOwner());
}

void UCatTraversalComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	CooldownTimer           = FMath::Max(CooldownTimer - DeltaTime, 0.0f);
	WallBounceCooldownTimer = FMath::Max(WallBounceCooldownTimer - DeltaTime, 0.0f);

	// Rebound window closes on its own timer, or early once the cat is back on the
	// ground (lateral air friction is meaningless there, and leaving it overridden
	// would strand the CMC on a wall-bounce value — the restore-precedence trap).
	if (WallBounceReboundTimer > 0.0f)
	{
		WallBounceReboundTimer -= DeltaTime;
		const ACatBase* Cat = GetCat();
		const UCharacterMovementComponent* CMC = Cat ? Cat->GetCharacterMovement() : nullptr;
		if (WallBounceReboundTimer <= 0.0f || (CMC && CMC->IsMovingOnGround()))
		{
			EndReboundWindow();
		}
	}

	WallAttachCooldownTimer = FMath::Max(WallAttachCooldownTimer - DeltaTime, 0.0f);

	// Landing clears a spent wall: the timeout only has to survive the fall it caused.
	if (!SpentWallNormal.IsZero())
	{
		const ACatBase* GroundCheckCat = GetCat();
		const UCharacterMovementComponent* GroundCheckCMC =
			GroundCheckCat ? GroundCheckCat->GetCharacterMovement() : nullptr;
		if (GroundCheckCMC && GroundCheckCMC->IsMovingOnGround())
		{
			SpentWallNormal = FVector::ZeroVector;
		}
	}

	if (TraversalState == ECatTraversalState::Mantle)
	{
		DriveMantle(DeltaTime);
		return;
	}
	// Runs on the owner AND the server copy (the RPC mirrors the entry, both then
	// constrain velocity from the same deterministic profile — the mantle pattern).
	if (TraversalState == ECatTraversalState::WallAttach)
	{
		DriveWallAttach(DeltaTime);
		// Keep hunting for a ledge WHILE clinging. The slide walks the cat down a wall,
		// which walks any lip above it up into the mantle band — so a cling under a
		// ledge should become a mantle, not slide past it to a timeout (2026-07-25: a
		// cling blinded the detector for 1.4 s while doing exactly that, because this
		// branch used to return immediately). Detection is owner-only as ever; the
		// mantle mirrors itself to the server through its own RPC. This is also the
		// mechanism Vertical Scramble's top-out handoff will ride: a scramble is this
		// same state entered with a rise budget, and at the top the lip enters band.
		if (TraversalState == ECatTraversalState::WallAttach)
		{
			if (ACatBase* AttachedCat = GetCat())
			{
				if (AttachedCat->IsLocallyControlled())
				{
					TryDetectMantle();
				}
			}
		}
		return;
	}

	// Detection runs only for the locally controlled cat (the server and proxies get
	// the takeover via the pawn RPC + replication, never their own detection).
	ACatBase* Cat = GetCat();
	if (Cat && Cat->IsLocallyControlled())
	{
		// Precedence, highest first. A reachable ledge beats climbing the wall under it;
		// a climbable wall run at speed beats merely hanging on it. Each starts a
		// traversal state, which gates the ones below (they check TraversalState).
		TryDetectMantle();
		TryDetectScramble();
		TryWallCling();
	}

	// The balance assist is NOT a verb: no state, no takeover, no replication. It runs
	// on the owner AND the server because both can derive the identical correction from
	// world geometry and the pawn's own position — so unlike every verb above it needs
	// no RPC to stay in agreement, and a dropped packet cannot desync it.
	if (Cat && (Cat->IsLocallyControlled() || Cat->HasAuthority()))
	{
		UpdateBalanceAssist(DeltaTime);
	}
}

void UCatTraversalComponent::TryDetectMantle()
{
	ACatBase* Cat = GetCat();
	if (!Cat)
	{
		return;
	}
	if (!bEnableMantle)                { NoteReject(ETraversalReject::Disabled);    return; }
	if (CooldownTimer > 0.0f)          { NoteReject(ETraversalReject::Cooldown);    return; }

	// Airborne, hands free. All grounded verbs are excluded by the airborne gate — no
	// CMC precedence overlap with stop/start/pivot.
	UCharacterMovementComponent* CMC = Cat->GetCharacterMovement();
	if (!CMC || !CMC->IsFalling())     { NoteReject(ETraversalReject::NotAirborne); return; }
	if (Cat->IsGrabbing())             { NoteReject(ETraversalReject::Grabbing);    return; }

	UCapsuleComponent* Capsule = Cat->GetCapsuleComponent();
	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	const FVector Center = Cat->GetActorLocation();
	const float CapsuleBottomZ = Center.Z - HalfHeight;

	FCollisionQueryParams Params(FName(TEXT("CatMantle")), false, Cat);

	// Heading, not facing: where the cat is GOING is the question a mantle answers.
	// Held input wins; failing that, actual travel counts (so a stick release mid-arc
	// does not disarm a ledge you are already sailing at). Actor forward is
	// deliberately NOT the fallback — air control and camera-relative input pull the
	// two apart constantly, and that divergence is what let the cling, which probes
	// all four ways, grab ledges the forward-only mantle never examined.
	FVector Heading = FVector::ZeroVector;
	if (Cat->PivotInputStaleTime < 0.15f && !Cat->PivotLiveInputDir.IsNearlyZero())
	{
		Heading = Cat->PivotLiveInputDir;
	}
	else
	{
		const FVector HorizVel(CMC->Velocity.X, CMC->Velocity.Y, 0.0f);
		if (HorizVel.SizeSquared() >= FMath::Square(MantleMinApproachSpeed))
		{
			Heading = HorizVel;
		}
	}
	Heading.Z = 0.0f;
	if (!Heading.Normalize())          { NoteReject(ETraversalReject::NoInput);     return; }

	// 1. Chest probe: ONE ray down the heading. Aiming the probe by intent is what
	//    keeps this tight — a radial probe plus a directionless "any input" gate
	//    fired on walls beside and behind the cat (2026-07-25, ~15x the mantle rate).
	//    It also dodges the nearest-hit selection trap: with four rays the probe could
	//    hand back a side wall while the cat was heading at the front one.
	FVector WallPoint, WallNormal;
	bool bAnyHit = false;
	if (!ProbeWalls(MantleReachDistance, /*NumDirections=*/1, WallPoint, WallNormal,
			bAnyHit, Heading))
	{
		NoteReject(bAnyHit ? ETraversalReject::WallTooFlat : ETraversalReject::NoWall);
		return;
	}

	// Measure along the WALL, not the body — the caught face need not be the one the
	// cat faces, since the probe followed the heading.
	FVector IntoWall = -WallNormal;
	IntoWall.Z = 0.0f;
	if (!IntoWall.Normalize())         { NoteReject(ETraversalReject::WallTooFlat);  return; }

	// 2. Lip probe: floor within the mantleable band past the wall face.
	const FVector LipStart(
		WallPoint.X + IntoWall.X * MantleForwardClearance,
		WallPoint.Y + IntoWall.Y * MantleForwardClearance,
		CapsuleBottomZ + MantleMaxLedgeHeight + 10.0f);
	FHitResult LipHit;
	if (!GetWorld()->LineTraceSingleByChannel(LipHit, LipStart,
			LipStart - FVector(0, 0, MantleMaxLedgeHeight - MantleMinLedgeHeight + 20.0f),
			ECC_Visibility, Params))
	{
		NoteReject(ETraversalReject::NoLip);
		return;
	}
	if (LipHit.ImpactNormal.Z <= 0.7f)
	{
		NoteReject(ETraversalReject::LipNotFloor);
		return;
	}

	const float LedgeHeight = LipHit.ImpactPoint.Z - CapsuleBottomZ;
	if (LedgeHeight < MantleMinLedgeHeight) { NoteReject(ETraversalReject::LipTooLow);  return; }
	if (LedgeHeight > MantleMaxLedgeHeight) { NoteReject(ETraversalReject::LipTooHigh); return; }

	// 3. Headroom: the landing spot must fit the capsule.
	const FVector Target = LipHit.ImpactPoint + IntoWall * 12.0f
		+ FVector(0, 0, HalfHeight + 2.0f);
	FHitResult RoomHit;
	if (GetWorld()->LineTraceSingleByChannel(RoomHit, Target,
			Target + FVector(0, 0, HalfHeight), ECC_Visibility, Params))
	{
		NoteReject(ETraversalReject::NoHeadroom);
		return;
	}

	NoteReject(ETraversalReject::None);

	// A ledge outranks the wall below it — including a wall already being held. Release
	// first: StartMantle refuses to stack on a live takeover, and leaving the attach set
	// would strand bGoWallAttach and skip the server's release mirror.
	if (IsWallAttached())
	{
		EndWallAttach(EWallAttachEnd::Mantled);
	}

	StartMantle(Center, Target);
	if (!Cat->HasAuthority())
	{
		Cat->Server_StartMantle(Center, Target);
	}
}

// ══════════════════════════════════════════════════════════════════════════
// ── Wall Bounce (verb 2) ──────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════
//
// A jump press while airborne next to a wall kicks off it instead of doing
// nothing. Unlike the mantle this is NOT a takeover — no movement mode change,
// no CMC apply/restore, no traversal state: it is a single LaunchCharacter
// impulse that hands straight back to the falling physics. That is deliberate,
// and it is why the bounce can safely interrupt anything.
//
// The radial probe (4 rays) is what separates this from the mantle's forward
// ray: pressed into a corner or a chimney, the wall you want to kick is rarely
// the one you are facing. Repeated bounces off alternating normals give chimney
// climbing for free.
//
// MP = the established prediction pattern: the owner launches immediately and
// mirrors the WALL NORMAL to the server, which launches its own copy from the
// same normal. The normal (not the resulting velocity) is the mirrored value so
// both machines derive the launch from the same tuning knobs.

// ══════════════════════════════════════════════════════════════════════════
// ── Wall Attach — the shared cling / scramble state ───────────────────────
// ══════════════════════════════════════════════════════════════════════════
//
// Three vertical phases from one set of entry parameters:
//   RISE  (scramble only, RiseSpeed > 0) — Vz eases RiseSpeed → 0 over RiseTime
//   CATCH (RiseTime..+WallClingCatchTime) — Vz pinned to 0: the "stick"
//   SLIDE (after that)                    — Vz held at −WallClingSlideSpeed
// A cling enters with RiseSpeed 0 and starts at CATCH; a scramble enters with a
// budget and decays through zero into the SAME slide. Same exits either way.
//
// Deliberately a per-tick velocity CONSTRAINT rather than a movement-mode takeover:
// the cat stays in MOVE_Falling, so Landed(), the jump SM, the landing cushion and
// foot IK all continue to behave, and the state can be entered or dropped on any
// frame without a CMC apply/restore pair to get wrong.

bool UCatTraversalComponent::IsScrambleSurface(const FHitResult& Hit) const
{
	if (const AActor* HitActor = Hit.GetActor())
	{
		if (HitActor->ActorHasTag(ScrambleSurfaceTag))
		{
			return true;
		}
	}
	if (const UPrimitiveComponent* HitComp = Hit.GetComponent())
	{
		if (HitComp->ComponentHasTag(ScrambleSurfaceTag))
		{
			return true;
		}
	}

	// Volume override: inside a tagged volume every wall climbs, so a shaft can be
	// blocked out without touching the meshes in it.
	if (const ACatBase* Cat = GetCat())
	{
		TArray<AActor*> Overlapping;
		Cat->GetOverlappingActors(Overlapping);
		for (const AActor* A : Overlapping)
		{
			if (A && A->ActorHasTag(ScrambleVolumeTag))
			{
				return true;
			}
		}
	}
	return false;
}

void UCatTraversalComponent::TryDetectScramble()
{
	ACatBase* Cat = GetCat();
	if (!Cat || !bEnableWallScramble || TraversalState != ECatTraversalState::None)
	{
		return;
	}
	if (WallAttachCooldownTimer > 0.0f || Cat->IsGrabbing())
	{
		return;
	}
	UCharacterMovementComponent* CMC = Cat->GetCharacterMovement();
	if (!CMC)
	{
		return;
	}

	// The run-up IS the entry: gate on closing speed, not on the sprint flag, so the
	// climb height scales continuously with how hard the cat arrived.
	const FVector HorizVel(CMC->Velocity.X, CMC->Velocity.Y, 0.0f);
	const float EntrySpeed = HorizVel.Size();
	if (EntrySpeed < ScrambleMinEntrySpeed)
	{
		return;
	}

	FVector WallPoint, WallNormal;
	bool bAnyHit = false;
	FHitResult WallHit;
	if (!ProbeWalls(ScrambleReach, /*NumDirections=*/1, WallPoint, WallNormal, bAnyHit,
			HorizVel, &WallHit))
	{
		return;
	}
	if (FVector::DotProduct(HorizVel, -WallNormal) < ScrambleMinEntrySpeed)
	{
		return;   // skimming along the face, not running into it
	}
	if (!SpentWallNormal.IsZero()
		&& FVector::DotProduct(WallNormal, SpentWallNormal) > 0.9f)
	{
		return;   // this face just timed out; leave it before climbing it again
	}
	if (!IsScrambleSurface(WallHit))
	{
		return;   // not every wall is climbable — level authoring decides
	}

	// Tall enough to be worth climbing. A low block is a mantle, and the mantle
	// detector already had first refusal this tick.
	{
		// Start OUTSIDE the face and trace inward. Starting inside the geometry returns
		// a start-penetrating hit with a garbage normal — the same trap that railed the
		// foot IK at wall contact (2026-07-21).
		FCollisionQueryParams Params(FName(TEXT("CatScrambleHeight")), false, Cat);
		const FVector HighStart = WallPoint + WallNormal * 20.0f
			+ FVector(0, 0, ScrambleMinWallHeight);
		FHitResult HighHit;
		if (!GetWorld()->LineTraceSingleByChannel(HighHit, HighStart,
				HighStart - WallNormal * 40.0f, ECC_Visibility, Params)
			|| HighHit.bStartPenetrating || HighHit.ImpactNormal.Z >= 0.5f)
		{
			return;
		}
	}

	// Rise budget from entry speed — a harder run-up climbs higher. Height gained is
	// RiseSpeed × RiseTime / 2, since DriveWallAttach decays the budget linearly.
	const float RiseSpeed = FMath::GetMappedRangeValueClamped(
		FVector2D(ScrambleMinEntrySpeed, ScrambleEntrySpeedForMaxRise),
		FVector2D(ScrambleRiseSpeedMin, ScrambleRiseSpeedMax), EntrySpeed);

	// Entering from the ground: hand the capsule to falling physics first, or the
	// attach's own grounded exit would fire on its very first drive tick.
	if (CMC->IsMovingOnGround())
	{
		CMC->SetMovementMode(MOVE_Falling);
	}

	StartWallAttach(WallNormal, RiseSpeed, ScrambleRiseTime);
	if (!Cat->HasAuthority())
	{
		Cat->Server_SetWallAttach(true, WallNormal, RiseSpeed, ScrambleRiseTime);
	}

	// Per-scramble spec line: entry speed → climb height is the authoring spec for the
	// scramble loop clip (traversal batch).
	UE_LOG(LogCatVentures, Log,
		TEXT("[%s] Scramble START — entry %.0f cm/s, rise %.0f cm/s over %.2f s (~%.0f uu)"),
		*Cat->GetName(), EntrySpeed, RiseSpeed, ScrambleRiseTime,
		RiseSpeed * ScrambleRiseTime * 0.5f);
}

void UCatTraversalComponent::TryWallCling()
{
	ACatBase* Cat = GetCat();
	if (!Cat || !bEnableWallCling || WallAttachCooldownTimer > 0.0f)
	{
		return;
	}
	if (IsMantling() || IsWallAttached() || Cat->IsGrabbing())
	{
		return;
	}
	UCharacterMovementComponent* CMC = Cat->GetCharacterMovement();
	if (!CMC || !CMC->IsFalling())
	{
		return;
	}

	// Never cancel motion that is closing on the mantle band. TryDetectMantle ran this
	// same tick, so LastReject is current: a lip reject means a real ledge is there and
	// only its height disqualified it — and height is what the vertical motion is
	// fixing, from whichever side. Catching here would pin Vz to 0 and kill it.
	const float Vz = CMC->Velocity.Z;
	const bool bClosingOnBand =
		   (LastReject == ETraversalReject::LipTooHigh && Vz >  WallClingLedgeSuppressSpeed)
		|| (LastReject == ETraversalReject::LipTooLow  && Vz < -WallClingLedgeSuppressSpeed);
	if (bClosingOnBand)
	{
		return;
	}

	FVector WallPoint, WallNormal;
	bool bAnyHit = false;
	if (!ProbeWalls(WallClingReach, /*NumDirections=*/4, WallPoint, WallNormal, bAnyHit))
	{
		return;
	}

	// Must be CLOSING on the wall. This is also the anti-re-stick guard: immediately
	// after a kick the cat is travelling away from the wall it left, so that wall fails
	// here by construction and only the wall being approached can catch.
	FVector HorizVel = CMC->Velocity;
	HorizVel.Z = 0.0f;
	if (FVector::DotProduct(HorizVel, -WallNormal) < WallClingMinApproachSpeed)
	{
		return;
	}

	// A face that just timed out stays spent until the cat leaves it — held input plus
	// air control would otherwise re-grip it within a couple of frames.
	if (!SpentWallNormal.IsZero()
		&& FVector::DotProduct(WallNormal, SpentWallNormal) > 0.9f)
	{
		return;
	}

	StartWallAttach(WallNormal, /*RiseSpeed=*/0.0f, /*RiseTime=*/0.0f);
	if (!Cat->HasAuthority())
	{
		Cat->Server_SetWallAttach(true, WallNormal, 0.0f, 0.0f);
	}
}

void UCatTraversalComponent::StartWallAttach(const FVector& WallNormal, float RiseSpeed, float RiseTime)
{
	ACatBase* Cat = GetCat();
	if (!Cat || TraversalState != ECatTraversalState::None)
	{
		return;   // a mantle outranks an attach; never stack takeovers
	}

	FVector N = WallNormal;
	N.Z = 0.0f;
	if (!N.Normalize())
	{
		return;
	}

	TraversalState  = ECatTraversalState::WallAttach;
	AttachNormal    = N;
	AttachElapsed   = 0.0f;
	AttachRiseSpeed = RiseSpeed;
	AttachRiseTime  = RiseTime;
	AttachStartZ    = Cat->GetActorLocation().Z;
	Cat->SetWallAttachAnimState(true);

	UE_LOG(LogCatVentures, Log, TEXT("[%s] Wall attach START — normal (%.2f, %.2f)%s"),
		*Cat->GetName(), N.X, N.Y,
		RiseSpeed > 0.0f ? TEXT(" [scramble]") : TEXT(" [cling]"));
}

void UCatTraversalComponent::DriveWallAttach(float DeltaTime)
{
	ACatBase* Cat = GetCat();
	UCharacterMovementComponent* CMC = Cat ? Cat->GetCharacterMovement() : nullptr;
	if (!Cat || !CMC)
	{
		EndWallAttach(EWallAttachEnd::Aborted);
		return;
	}

	AttachElapsed += DeltaTime;

	// ── Exits ────────────────────────────────────────────────────────
	// NOTE: steering away is NOT an exit. See EndWallAttach's comment — the wall the
	// player aims at sits along the held wall's normal, so an away-release fired on the
	// frame intent was expressed and ate the kick.
	if (Cat->IsGrabbing())
	{
		EndWallAttach(EWallAttachEnd::Grabbed);
		return;
	}
	if (CMC->IsMovingOnGround())
	{
		EndWallAttach(EWallAttachEnd::Grounded);
		return;
	}
	// The cap budgets the HANGING portion, so it starts after the rise. Charging a
	// scramble's climb against a cling's budget left only 0.7 s of slide and dumped the
	// cat mid-wall (2026-07-25). A cling has RiseTime 0, so its 1.4 s is unchanged.
	if (AttachElapsed > AttachRiseTime + WallClingMaxTime)
	{
		EndWallAttach(EWallAttachEnd::Timeout);
		return;
	}

	// The wall must still be there — trace straight at the face we are holding.
	{
		FHitResult Hit;
		FCollisionQueryParams Params(FName(TEXT("CatWallAttach")), false, Cat);
		const FVector Center = Cat->GetActorLocation();
		if (!GetWorld()->LineTraceSingleByChannel(Hit, Center,
				Center - AttachNormal * (WallClingReach + 10.0f), ECC_Visibility, Params)
			|| Hit.ImpactNormal.Z >= 0.5f)
		{
			EndWallAttach(EWallAttachEnd::WallLost);
			return;
		}
	}

	// ── Vertical profile: rise → catch → slide ───────────────────────
	float VzTarget;
	if (AttachRiseSpeed > 0.0f && AttachElapsed < AttachRiseTime)
	{
		VzTarget = AttachRiseSpeed * (1.0f - AttachElapsed / FMath::Max(AttachRiseTime, KINDA_SMALL_NUMBER));
	}
	else if (AttachElapsed < AttachRiseTime + WallClingCatchTime)
	{
		VzTarget = 0.0f;
	}
	else
	{
		VzTarget = -WallClingSlideSpeed;
	}

	// Hold the profile and kill horizontal drift: the cat is stuck to the face, so the
	// into-wall component would grind through it and the tangential component would slide
	// it along a wall it is supposed to be gripping.
	FVector Vel = CMC->Velocity;
	Vel.X = 0.0f;
	Vel.Y = 0.0f;
	Vel.Z = VzTarget;
	CMC->Velocity = Vel;

	// Face the wall (cosmetic; orient-to-movement is inert at zero horizontal velocity).
	Cat->SetActorRotation(FRotator(0.0f, (-AttachNormal).Rotation().Yaw, 0.0f));

	if (Cat->PawPrint)
	{
		static const FName ChAttach(TEXT("WallAttachPhase"));
		static const FName ChClimb(TEXT("WallAttachClimb"));
		const float Phase = (AttachRiseSpeed > 0.0f && AttachElapsed < AttachRiseTime) ? 1.0f
			: (AttachElapsed < AttachRiseTime + WallClingCatchTime) ? 2.0f : 3.0f;
		Cat->PawPrint->SampleChannel(ChAttach, Phase);
		Cat->PawPrint->SampleChannel(ChClimb, Cat->GetActorLocation().Z - AttachStartZ);
	}
}

void UCatTraversalComponent::EndWallAttach(EWallAttachEnd Reason)
{
	if (TraversalState != ECatTraversalState::WallAttach)
	{
		return;
	}
	TraversalState = ECatTraversalState::None;
	WallAttachCooldownTimer = WallAttachCooldown;

	// A timeout means "this face is spent" — remember it so the held input that got the
	// cat here cannot immediately re-grip it. Cleared on ground / kick / mantle.
	SpentWallNormal = (Reason == EWallAttachEnd::Timeout) ? AttachNormal : FVector::ZeroVector;

	if (ACatBase* Cat = GetCat())
	{
		Cat->SetWallAttachAnimState(false);
		// Per-attach spec line — hold duration is the authoring spec for the cling clip
		// (and later the scramble loop's length). The reason makes a cling that let go
		// on its own distinguishable from one the player kicked out of.
		UE_LOG(LogCatVentures, Log,
			TEXT("[%s] Wall attach END — held %.2f s, climbed %.0f uu (%s)"),
			*Cat->GetName(), AttachElapsed, Cat->GetActorLocation().Z - AttachStartZ,
			WallAttachEndToString(Reason));
		if (!Cat->HasAuthority())
		{
			Cat->Server_SetWallAttach(false, FVector::ZeroVector, 0.0f, 0.0f);
		}
	}
	AttachNormal = FVector::ZeroVector;
}

void UCatTraversalComponent::EndReboundWindow()
{
	WallBounceReboundTimer = 0.0f;
	WallBounceReboundDir   = FVector::ZeroVector;
	if (ACatBase* Cat = GetCat())
	{
		if (UCharacterMovementComponent* CMC = Cat->GetCharacterMovement())
		{
			CMC->FallingLateralFriction = Cat->MovementFallingLateralFriction;
		}
	}
}

bool UCatTraversalComponent::TryWallBounce()
{
	ACatBase* Cat = GetCat();
	if (!Cat || !bEnableWallBounce)
	{
		return false;
	}
	if (WallBounceCooldownTimer > 0.0f || IsMantling() || Cat->IsGrabbing())
	{
		return false;
	}
	const UCharacterMovementComponent* CMC = Cat->GetCharacterMovement();
	if (!CMC || !CMC->IsFalling())
	{
		return false;   // grounded presses are ordinary jumps
	}

	FVector WallNormal;
	if (IsWallAttached())
	{
		// Clinging: kick off the face we are HOLDING, not whatever the radial probe
		// finds. In a chimney both walls are usually in reach, so re-probing here could
		// kick off the wrong one — and the whole point of the cling is that the player
		// chooses the moment, having already chosen the wall.
		WallNormal = AttachNormal;
		EndWallAttach(EWallAttachEnd::Kick);
	}
	else
	{
		FVector WallPoint;
		bool bAnyHit = false;
		if (!ProbeWalls(WallBounceReach, /*NumDirections=*/4, WallPoint, WallNormal, bAnyHit))
		{
			return false;
		}
	}

	DoWallBounce(WallNormal);
	if (!Cat->HasAuthority())
	{
		Cat->Server_WallBounce(WallNormal);
	}
	return true;
}

void UCatTraversalComponent::DoWallBounce(const FVector& WallNormal)
{
	ACatBase* Cat = GetCat();
	if (!Cat)
	{
		return;
	}

	FVector Lateral = WallNormal;
	Lateral.Z = 0.0f;
	if (!Lateral.Normalize())
	{
		return;
	}

	// Kicking off is a deliberate departure — whatever face was spent is forgiven, which
	// is what keeps a chimney (alternating normals) working after any timeout.
	SpentWallNormal = FVector::ZeroVector;

	const FVector Launch = Lateral * WallBounceLateralSpeed
		+ FVector::UpVector * WallBounceVerticalSpeed;

	// Override both planes: a bounce should feel decisive, not additive to whatever
	// the fall had accumulated (an additive kick off a fast fall barely registers).
	Cat->LaunchCharacter(Launch, /*bXYOverride=*/true, /*bZOverride=*/true);

	if (bWallBounceReorientBody)
	{
		Cat->SetActorRotation(FRotator(0.0f, Lateral.Rotation().Yaw, 0.0f));
	}

	// Re-enter the Launch phase so the jump SM plays the launch pose again — the Fall
	// case has no "started rising" edge of its own, so without this the cat would sail
	// back up still in the fall pose. JumpAirTime resets because JumpRiseProgress scrubs
	// the launch clip by it; a bounce late in a long fall would otherwise start the clip
	// already finished. bLeftGroundByJumping stays true — a bounce is not a ledge-walk,
	// so it must not hand back coyote time.
	Cat->JumpAirTime = 0.0f;
	Cat->SetJumpPhase(ECatJumpPhase::Launch);

	// SNAP the gravity interpolator to the rising value (2026-07-24). UpdateJumpGravity
	// already does this on ground phases "so the next airborne jump starts from the
	// correct baseline, not a stale fall value" — a wall bounce is a mid-air relaunch,
	// the one case that comment never had to cover. Without the snap the interpolator is
	// still near GravityScaleFalling 5.5 and takes ~0.1 s to ramp down, so the first
	// third of every kick's rise is fought by 2-3x the intended gravity: PawPrint
	// measured apex at +0.22 s instead of the +0.32 s a 620 launch should give, cutting
	// peak height ~98 -> ~60 cm. In a chimney that made each cycle net roughly zero —
	// the "fighting the mechanic to keep the cat going up" read (Sean, 2026-07-24).
	Cat->GravityScaleInterp = Cat->GravityScaleRising;
	if (UCharacterMovementComponent* CMC = Cat->GetCharacterMovement())
	{
		CMC->GravityScale = Cat->GravityScaleRising;

		// Rebound window: drop lateral air friction so the kick actually carries.
		if (WallBounceReboundTime > 0.0f)
		{
			CMC->FallingLateralFriction = WallBounceReboundLateralFriction;
		}
	}
	WallBounceReboundTimer = WallBounceReboundTime;
	WallBounceReboundDir   = Lateral;

	WallBounceCooldownTimer = WallBounceCooldown;

	// Per-bounce spec line (the stop/start/mantle log doctrine) — these numbers are the
	// authoring spec for the real plant-and-spring clip in the traversal batch.
	UE_LOG(LogCatVentures, Log,
		TEXT("[%s] Wall bounce — normal (%.2f, %.2f), launch %.0f lateral / %.0f up"),
		*Cat->GetName(), Lateral.X, Lateral.Y, WallBounceLateralSpeed, WallBounceVerticalSpeed);
}

// ══════════════════════════════════════════════════════════════════════════
// ── Fence Trot (verb 4) ───────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════
//
// Detection is IMPLICIT and geometric: scan across the cat's right vector and
// look for a narrow band of support with the ground falling away on both
// sides. No tags, no volumes — a fence announces itself by being a fence,
// which is the opposite of the scramble's authored gate (a climbable wall
// looks exactly like an unclimbable one).
//
// Probing across FACING means detection only fires when the cat is already
// roughly aligned with the surface, which is the behaviour we want: crossing
// a wall perpendicular should not drop you into balance mode. It also gives
// the edge axis for free — it IS the facing, once both flanks read as drops.

bool UCatTraversalComponent::IsSupportedAt(const FVector& Probe, float FloorZ) const
{
	const ACatBase* Cat = GetCat();
	if (!Cat || !GetWorld())
	{
		return false;
	}
	FCollisionQueryParams Params(FName(TEXT("CatFenceSupport")), false, Cat);
	FHitResult Hit;
	const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, Probe,
		Probe - FVector(0, 0, FenceMinDropDepth + 10.0f), ECC_Visibility, Params);
	return bHit && (FloorZ - Hit.ImpactPoint.Z) < FenceMinDropDepth;
}

bool UCatTraversalComponent::FindBalanceAxis(FVector& OutAxis) const
{
	const ACatBase* Cat = GetCat();
	const UCapsuleComponent* Capsule = Cat ? Cat->GetCapsuleComponent() : nullptr;
	if (!Cat || !Capsule)
	{
		return false;
	}

	// Ring of support samples in WORLD directions. Deriving the axis from geometry
	// rather than from the cat is the whole point: the first build probed across the
	// actor's right vector, but entering balance mode rotates the cat (projected input
	// + a lower yaw rate), which rotated the probe, which failed, which exited, which
	// turned the cat back — a 1-2 frame enter/exit oscillation (2026-07-25, 120 events
	// in one session). A detector must never depend on a quantity its own mode changes.
	const FVector Centre = Cat->GetActorLocation();
	const float FloorZ = Centre.Z - Capsule->GetScaledCapsuleHalfHeight();

	constexpr int32 Ring = 12;                 // 30° granularity, refined by the mean below
	double SumX = 0.0, SumY = 0.0;
	int32 SupportedCount = 0;
	for (int32 i = 0; i < Ring; ++i)
	{
		const float Angle = (2.0f * PI * i) / Ring;
		const FVector Dir(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);
		if (IsSupportedAt(Centre + Dir * FenceAxisProbeRadius, FloorZ))
		{
			// Doubled-angle mean: the surface direction is a LINE, not a vector, so
			// opposite samples must reinforce rather than cancel.
			SumX += FMath::Cos(2.0f * Angle);
			SumY += FMath::Sin(2.0f * Angle);
			++SupportedCount;
		}
	}

	// All round = open ground. None = not standing on anything we can read.
	if (SupportedCount == 0 || SupportedCount >= Ring - 1)
	{
		return false;
	}

	const float AxisAngle = FMath::Atan2(static_cast<float>(SumY), static_cast<float>(SumX)) * 0.5f;
	OutAxis = FVector(FMath::Cos(AxisAngle), FMath::Sin(AxisAngle), 0.0f);
	return !OutAxis.IsNearlyZero();
}

bool UCatTraversalComponent::ProbeBalanceSurface(const FVector& Axis,
	float& OutCentreOffset, float& OutSpan) const
{
	const ACatBase* Cat = GetCat();
	const UCapsuleComponent* Capsule = Cat ? Cat->GetCapsuleComponent() : nullptr;
	if (!Cat || !Capsule || !GetWorld())
	{
		return false;
	}

	const FVector Centre = Cat->GetActorLocation();
	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();

	// Across the SURFACE's perpendicular, not the cat's — same decoupling as above.
	FVector Right = FVector::CrossProduct(FVector::UpVector, Axis);
	Right.Z = 0.0f;
	if (!Right.Normalize())
	{
		return false;
	}

	const int32 Samples = FMath::Max(FenceProbeSamples, 3);

	// Sample support across the width. "Supported" means floor at roughly the height
	// the cat is standing at — a deeper hit is the ground far below, i.e. a drop.
	const float FloorZ = Centre.Z - HalfHeight;
	TArray<bool, TInlineAllocator<16>> Supported;
	Supported.SetNum(Samples);
	for (int32 i = 0; i < Samples; ++i)
	{
		const float Alpha  = (Samples == 1) ? 0.5f : static_cast<float>(i) / (Samples - 1);
		const float Offset = FMath::Lerp(-FenceProbeHalfWidth, FenceProbeHalfWidth, Alpha);
		Supported[i] = IsSupportedAt(Centre + Right * Offset, FloorZ);
	}

	// Both flanks must be dropping, or this is a wide ledge / open ground.
	if (Supported[0] || Supported[Samples - 1])
	{
		return false;
	}

	// The supported run through the middle is the surface. Walk out from the centre so
	// a second surface elsewhere in the scan (a neighbouring rail) cannot widen it.
	const int32 Mid = Samples / 2;
	if (!Supported[Mid])
	{
		return false;   // not actually standing on it
	}
	int32 Lo = Mid, Hi = Mid;
	while (Lo > 0 && Supported[Lo - 1])            { --Lo; }
	while (Hi < Samples - 1 && Supported[Hi + 1])  { ++Hi; }

	const float Step = (2.0f * FenceProbeHalfWidth) / (Samples - 1);

	// Sample indices give the edges only to within one Step, so the centre offset lands
	// on a Step/2 grid — 6.67 uu with the shipped scan. That quantum is a DEAD ZONE: any
	// true offset under half of it reports exactly 0 and the assist does nothing at all,
	// then jumps a whole step. Measured 2026-07-25: 45% of frames reported 0 offset and
	// 50% reported 6.67, with literally nothing in between, which reads as an assist that
	// stutters rather than one that is too weak. Bisect the real edges instead — the
	// transition is bracketed by a known supported/unsupported pair, so a few extra
	// traces per side buy sub-uu resolution for a continuous correction.
	auto RefineEdge = [&](int32 SupportedIdx, int32 OutsideIdx) -> float
	{
		float In  = -FenceProbeHalfWidth + SupportedIdx * Step;   // known ON the surface
		float Out = -FenceProbeHalfWidth + OutsideIdx  * Step;    // known past the edge
		for (int32 It = 0; It < 4; ++It)                          // Step/16 ≈ 0.8 uu
		{
			const float Mid = (In + Out) * 0.5f;
			(IsSupportedAt(Centre + Right * Mid, FloorZ) ? In : Out) = Mid;
		}
		return (In + Out) * 0.5f;
	};

	// Lo/Hi never sit at the scan ends — a supported flank was rejected above — so the
	// outside neighbours always exist.
	const float LoEdge = RefineEdge(Lo, Lo - 1);
	const float HiEdge = RefineEdge(Hi, Hi + 1);

	OutSpan = HiEdge - LoEdge;
	if (OutSpan > FenceMaxSurfaceWidth)
	{
		return false;
	}
	OutCentreOffset = (LoEdge + HiEdge) * 0.5f;
	return true;
}

void UCatTraversalComponent::UpdateBalanceAssist(float DeltaTime)
{
	ACatBase* Cat = GetCat();
	UCharacterMovementComponent* CMC = Cat ? Cat->GetCharacterMovement() : nullptr;
	if (!Cat || !CMC || !bEnableBalanceAssist)
	{
		return;
	}
	// Grounded, and not inside another verb's takeover. No state of its own beyond that.
	if (!CMC->IsMovingOnGround() || TraversalState != ECatTraversalState::None
		|| Cat->IsGrabbing())
	{
		return;
	}

	FVector Axis;
	if (!FindBalanceAxis(Axis))
	{
		return;
	}
	float CentreOffset = 0.0f, Span = 0.0f;
	if (!ProbeBalanceSurface(Axis, CentreOffset, Span))
	{
		return;
	}

	FVector Right = FVector::CrossProduct(FVector::UpVector, Axis);
	Right.Z = 0.0f;
	if (!Right.Normalize())
	{
		return;
	}

	// Narrower surfaces get more help; a wall top wide enough to stand on comfortably
	// should not feel assisted at all.
	float Scale = 1.0f;
	if (bScaleAssistByNarrowness && FenceMaxSurfaceWidth > KINDA_SMALL_NUMBER)
	{
		Scale = 1.0f - FMath::Clamp(Span / FenceMaxSurfaceWidth, 0.0f, 1.0f);
	}

	// +CentreOffset: the probe reports where the surface centre lies relative to the CAT
	// along Right, so closing the gap means moving toward it. (The mode shipped with this
	// inverted and shoved the cat off the rail it was meant to be holding.)
	const float Target = FMath::Clamp(CentreOffset * FenceAssistStrength,
		-FenceAssistMaxSpeed, FenceAssistMaxSpeed) * Scale;

	// Blend the lateral component toward the target rather than setting it: the cat's own
	// sideways motion must never be erased, or this is a rail again. The cap sits far
	// under the cat's own lateral authority so deliberate steering always wins.
	const float Lateral = FVector::DotProduct(CMC->Velocity, Right);
	const float NewLateral = FMath::FInterpTo(Lateral, Target, DeltaTime, FenceAssistBlendRate);
	CMC->Velocity += Right * (NewLateral - Lateral);

	if (Cat->PawPrint)
	{
		static const FName ChOffset(TEXT("FenceOffset"));
		static const FName ChSpan(TEXT("FenceSpan"));
		Cat->PawPrint->SampleChannel(ChOffset, CentreOffset);
		Cat->PawPrint->SampleChannel(ChSpan, Span);
	}
}


void UCatTraversalComponent::StartMantle(const FVector& InStart, const FVector& InTarget)
{
	ACatBase* Cat = GetCat();
	if (!Cat || TraversalState == ECatTraversalState::Mantle)
	{
		return;
	}

	TraversalState = ECatTraversalState::Mantle;
	MantleStart    = InStart;
	MantleTarget   = InTarget;
	MantleElapsed  = 0.0f;
	const float Height = FMath::Max(InTarget.Z - InStart.Z, 0.0f);
	MantleDuration = MantleBaseDuration + Height * MantleDurationPerCm;

	// Face the ledge. Since the chest probe went radial the caught face is often not
	// the one the body faces, and the clamber clip is authored as a straight-ahead
	// pull-up — without this a sideways catch plays it crabbing. Derived from the
	// start→target vector rather than the normal so the owner and the server (which
	// receives only those two points) turn identically.
	FVector Facing = InTarget - InStart;
	Facing.Z = 0.0f;
	if (Facing.Normalize())
	{
		Cat->SetActorRotation(FRotator(0.0f, Facing.Rotation().Yaw, 0.0f));
	}

	// The single CMC takeover point for this component: Flying kills gravity while
	// the curve owns the capsule; the restore lives in EndMantle and nowhere else.
	if (UCharacterMovementComponent* CMC = Cat->GetCharacterMovement())
	{
		CMC->StopMovementImmediately();
		CMC->SetMovementMode(MOVE_Flying);
	}
	Cat->SetMantleAnimState(true, 0.0f);

	UE_LOG(LogCatVentures, Log, TEXT("[%s] Mantle START — ledge %.0f uu, duration %.2f s"),
		*Cat->GetName(), MantleTarget.Z - MantleStart.Z, MantleDuration);
}

void UCatTraversalComponent::DriveMantle(float DeltaTime)
{
	ACatBase* Cat = GetCat();
	if (!Cat)
	{
		EndMantle(false);
		return;
	}

	// External takeover (grab, mode change) aborts — restore and get out of the way.
	UCharacterMovementComponent* CMC = Cat->GetCharacterMovement();
	if (!CMC || CMC->MovementMode != MOVE_Flying || Cat->IsGrabbing())
	{
		EndMantle(false);
		return;
	}

	MantleElapsed += DeltaTime;
	const float Alpha = FMath::Clamp(MantleElapsed / FMath::Max(MantleDuration, 0.05f), 0.0f, 1.0f);

	// Vertical-first curve: the pull-up leads (paws hook, body rises), the forward
	// swing finishes over the lip. Both smoothstepped; windows overlap in the middle.
	const float ZAlpha  = FMath::SmoothStep(0.0f, 0.55f, Alpha);
	const float XYAlpha = FMath::SmoothStep(0.35f, 1.0f, Alpha);
	FVector NewLoc;
	NewLoc.X = FMath::Lerp(MantleStart.X, MantleTarget.X, XYAlpha);
	NewLoc.Y = FMath::Lerp(MantleStart.Y, MantleTarget.Y, XYAlpha);
	NewLoc.Z = FMath::Lerp(MantleStart.Z, MantleTarget.Z, ZAlpha);
	Cat->SetActorLocation(NewLoc, false);

	Cat->SetMantleAnimState(true, Alpha);

	if (Alpha >= 1.0f)
	{
		EndMantle(true);
	}
}

void UCatTraversalComponent::EndMantle(bool bCompleted)
{
	if (TraversalState != ECatTraversalState::Mantle)
	{
		return;
	}
	TraversalState = ECatTraversalState::None;
	CooldownTimer  = MantleCooldown;

	ACatBase* Cat = GetCat();
	if (Cat)
	{
		if (UCharacterMovementComponent* CMC = Cat->GetCharacterMovement())
		{
			if (CMC->MovementMode == MOVE_Flying)
			{
				// Restore to FALLING, not Walking: the target floats the capsule ~2 uu
				// above the ledge, so the CMC immediately processes a REAL landing —
				// Landed() runs the whole normal path (JumpPhase Land → recovery → None,
				// cushion micro-kick, foot-IK landing snap). A direct Walking switch
				// skipped Landed() and left JumpPhase parked at Fall when the mantle
				// caught a descending cat (stuck fall pose, Sean's 2026-07-22 repro).
				// An abort mid-air wants Falling anyway.
				CMC->SetMovementMode(MOVE_Falling);
			}
			if (bCompleted)
			{
				FVector Fwd = Cat->GetActorForwardVector();
				Fwd.Z = 0.0f;
				CMC->Velocity = Fwd.GetSafeNormal() * MantleExitSpeed;
			}
		}
		Cat->SetMantleAnimState(false, 1.0f);

		// Per-mantle spec line — the clamber-clip authoring numbers (the stop/start
		// log-line doctrine): ledge height + takeover duration.
		UE_LOG(LogCatVentures, Log, TEXT("[%s] Mantle %s — ledge %.0f uu in %.2f s"),
			*Cat->GetName(), bCompleted ? TEXT("END") : TEXT("ABORT"),
			MantleTarget.Z - MantleStart.Z, MantleElapsed);
	}
}
