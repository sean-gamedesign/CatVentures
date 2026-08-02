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
// ── The wall-transfer leap, as an AUTHORED CONSTANT (round 20c) ───────────
// ══════════════════════════════════════════════════════════════════════════
//
// The crossing is no longer fitted to whatever gap it finds: the leap itself is
// the design, and level geometry is built to match it (Sean's call — he blocks
// levels to these dimensions). Three numbers define it, and the CLIPS ARE
// AUTHORED TO THEM — change one and A_Cat_Wall_Spring/Sail must be re-authored,
// which is why they are constants here and not tuning knobs.
//
// Why 45°: the launch angle has to be the same number as the cat's body pitch
// leaving the wall, or the pose and the path visibly disagree. The donor clip
// (Climb_Start) reared to ~73° because it is a cat leaping UP AT a wall from the
// ground, and matching THAT would have demanded ~293 uu of capsule rise during
// the 0.37 s push (≈4 m of climb per crossing, capsule outrunning the pose 3×).
// At 45° pose and path agree and the climb lands at ~143 uu per crossing.
namespace CatTransferArc
{
	constexpr float LaunchDeg   = 55.0f;   // == the authored body pitch at departure
	constexpr float ApexFrac    = 2.0f / 3.0f;
	constexpr float ArrivalDeg  = 15.0f;   // the "bit of dip" onto the far wall

	// Wall phase (the turn's own root motion; the spring is scaled separately).
	constexpr float RootZ[11]      = { 0.00f, -2.60f, 4.40f, 17.70f, 28.80f, 32.40f, 31.50f, 31.40f, 31.40f, 31.40f, 31.40f };
	constexpr float Sway[11]       = { 0.00f, 1.70f, 5.70f, 6.60f, 1.50f, 0.30f, 1.20f, 1.60f, 1.60f, 1.60f, 1.60f };
	constexpr float SpringNorm[11] = { 0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 0.052f, 0.474f, 1.000f };
	constexpr float YawShape[11]   = { 0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 0.194f, 0.661f, 0.781f, 0.974f, 1.000f, 1.000f };

	float Sample(const float (&T)[11], float X)
	{
		const float S = FMath::Clamp(X, 0.0f, 1.0f) * 10.0f;
		const int32 Seg = FMath::Min(FMath::FloorToInt32(S), 9);
		const float U = S - Seg;
		const float P0 = T[FMath::Max(Seg - 1, 0)], P1 = T[Seg];
		const float P2 = T[Seg + 1], P3 = T[FMath::Min(Seg + 2, 10)];
		return 0.5f * ((2.f*P1) + (-P0+P2)*U + (2.f*P0-5.f*P1+4.f*P2-P3)*U*U + (-P0+3.f*P1-3.f*P2+P3)*U*U*U);
	}

	/** Everything the crossing needs, solved from the gap alone. */
	struct FSolve
	{
		float SpringAcross = 0.f, SpringZ = 0.f;   // the scaled push
		float AcrossEnd = 0.f, ZEnd = 0.f;         // where the wall phase leaves the cat
		float HorizSpan = 0.f, FlightClimb = 0.f;  // the sail
		float TotalClimb = 0.f;                    // the level-design metric
		float B = 0.f, C = 0.f;                    // flight cubic (with the u³ term below)
		float D = 0.f;
	};

	FSolve Solve(float Gap, float PushTime, float FlightTime)
	{
		FSolve S;
		// Terminal slope of the normalised push, read off the curve so the tables
		// stay the single source of truth.
		constexpr float Eps = 0.02f;
		const float SlopeU = (1.0f - Sample(SpringNorm, 1.0f - Eps)) / Eps;
		const float SlopeSec = SlopeU / FMath::Max(PushTime, 0.05f);
		S.AcrossEnd = Sample(Sway, 1.0f);
		// Lateral continuity: the push must EXIT at the sail's crossing speed.
		S.SpringAcross = FMath::Max(Gap - S.AcrossEnd, 0.0f)
			/ FMath::Max(1.0f + SlopeSec * FlightTime, 0.01f);
		S.AcrossEnd += S.SpringAcross;
		S.HorizSpan = FMath::Max(Gap - S.AcrossEnd, 1.0f);
		// Vertical continuity: leave along the authored launch line, so the rise
		// scales to whatever the lateral ended up being.
		const float SlopeZU = (1.0f - Sample(SpringNorm, 1.0f - Eps)) / Eps;   // same shape
		S.SpringZ = S.SpringAcross * FMath::Tan(FMath::DegreesToRadians(LaunchDeg))
			* (SlopeU / FMath::Max(SlopeZU, 0.01f));
		S.ZEnd = Sample(RootZ, 1.0f) + S.SpringZ;
		// Flight cubic z(u) = B·u + C·u² + D·u³ over u = 0..1 of the horizontal span,
		// with z'(0) = tan(launch), z'(ApexFrac) = 0, z'(1) = −tan(arrival). Height
		// at the catch falls out as 0.25·tan(launch)·span — the arc is authored, the
		// climb is emergent, which is exactly backwards from every previous round.
		const float S0 = FMath::Tan(FMath::DegreesToRadians(LaunchDeg));
		const float S1 = -FMath::Tan(FMath::DegreesToRadians(ArrivalDeg));
		S.B = S0 * S.HorizSpan;
		S.D = (S1 + 0.5f * S0) * S.HorizSpan;
		S.C = (-1.25f * S0 - S1) * S.HorizSpan;
		S.FlightClimb = S.B + S.C + S.D;
		S.TotalClimb = S.ZEnd + S.FlightClimb;
		return S;
	}
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

	// Pending cling-kick: the grip held through the clip's load-and-push beat — fire
	// the launch. If the attach already ended on its own (timeout / wall lost), the
	// kick still fires off the stored normal: the player pressed, the kick happens.
	// (Grab / mantle / landing cancel it in EndWallAttach instead.)
	if (PendingKickTimer > 0.0f)
	{
		PendingKickTimer -= DeltaTime;
		if (PendingKickTimer <= 0.0f)
		{
			if (IsWallAttached())
			{
				EndWallAttach(EWallAttachEnd::Kick);
			}
			DoWallBounce(PendingKickNormal, bPendingKickRight);
			if (ACatBase* KickCat = GetCat())
			{
				if (!KickCat->HasAuthority())
				{
					KickCat->Server_WallBounce(PendingKickNormal, bPendingKickRight);
				}
			}
		}
	}

	// Deferred mantle exit step-out (see EndMantle): input was suppressed through the
	// exit frame, so the held-input check waits here for the first tick with real
	// acceleration. Applied along the INPUT direction — the player may already be
	// steering off the ledge line, and shoving them along the old facing would fight
	// the stick they are holding.
	if (MantleExitBoostTimer > 0.0f)
	{
		MantleExitBoostTimer -= DeltaTime;
		if (TraversalState != ECatTraversalState::None)
		{
			MantleExitBoostTimer = 0.0f;   // a new verb owns the CMC — stand down
		}
		else if (ACatBase* BoostCat = GetCat())
		{
			if (UCharacterMovementComponent* BoostCMC = BoostCat->GetCharacterMovement())
			{
				const FVector Accel = BoostCMC->GetCurrentAcceleration();
				if (Accel.SizeSquared2D() > 1.0f && BoostCMC->IsMovingOnGround())
				{
					BoostCMC->Velocity = Accel.GetSafeNormal2D() * MantleExitSpeed;
					MantleExitBoostTimer = 0.0f;
				}
			}
		}
	}

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
	if (TraversalState == ECatTraversalState::WallTransfer)
	{
		DriveWallTransfer(DeltaTime);
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

	// Remember HOW far out of band, not just that it was: the cling's suppression needs
	// to know whether a rise could ever close the gap (see WallClingLedgeSuppressSpeed).
	const float LedgeHeight = LipHit.ImpactPoint.Z - CapsuleBottomZ;
	LastLipHeight = LedgeHeight;
	if (LedgeHeight < MantleMinLedgeHeight) { NoteReject(ETraversalReject::LipTooLow);  return; }
	if (LedgeHeight > MantleMaxLedgeHeight) { NoteReject(ETraversalReject::LipTooHigh); return; }

	// 2b. Corner coverage: at the lateral END of a face (inside corners, block edges)
	// the single heading ray can hit the wall a whisker from its edge, leaving half
	// the cat overhanging air through the whole climb (Sean's repro 2026-07-31: the
	// body floats beside the block, paws reach sideways to the lip, and the hug trace
	// flaps across the corner). Probe the face ± a half-body along the wall tangent;
	// if exactly one side finds no wall, shift the landing target toward the covered
	// side — the XY curve then carries the capsule laterally onto the face.
	FVector CornerShift = FVector::ZeroVector;
	{
		constexpr float SideProbe = 12.0f;
		const FVector Tangent = FVector::CrossProduct(IntoWall, FVector::UpVector).GetSafeNormal();
		auto SideHasWall = [&](float Sign)
		{
			FHitResult SideHit;
			const FVector Origin = Center + Tangent * (Sign * SideProbe);
			return GetWorld()->LineTraceSingleByChannel(SideHit, Origin,
				Origin + IntoWall * MantleReachDistance, ECC_Visibility, Params)
				&& SideHit.ImpactNormal.Z < 0.5f;
		};
		const bool bLeft = SideHasWall(-1.0f), bRight = SideHasWall(1.0f);
		if (bLeft != bRight)
		{
			CornerShift = Tangent * (bLeft ? -SideProbe : SideProbe);
		}
	}

	// 3. Headroom: the landing spot must fit the capsule.
	const FVector Target = LipHit.ImpactPoint + IntoWall * 12.0f + CornerShift
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
	// only its height disqualified it — and height is what the vertical motion is fixing.
	const float Vz = CMC->Velocity.Z;
	bool bClosingOnBand = false;
	if (LastReject == ETraversalReject::LipTooHigh && Vz > WallClingLedgeSuppressSpeed)
	{
		// ONLY defer if the rise can actually deliver the ledge. The first version
		// assumed it always would, which is true of a 90 uu ledge and meaningless on a
		// 400–675 uu wall — and tall walls are exactly the ones worth hanging off. That
		// unbounded rule suppressed the cling for entire ascents and got nothing back:
		// measured 2026-07-25, every head-on running jump reported LipTooHigh and never
		// caught, while a diagonal jump caught fine purely because its reject happened
		// to be NoInput instead. The cling must not depend on HOW the mantle failed.
		const float G = FMath::Abs(CMC->GetGravityZ());
		const float RemainingRise = (G > KINDA_SMALL_NUMBER) ? (Vz * Vz) / (2.0f * G) : 0.0f;
		bClosingOnBand = (LastLipHeight - MantleMaxLedgeHeight) <= RemainingRise;
	}
	else if (LastReject == ETraversalReject::LipTooLow && Vz < -WallClingLedgeSuppressSpeed)
	{
		// The falling side needs no bound: a lip below the band is at most
		// MantleMinLedgeHeight away and a fall closes that in a few frames.
		bClosingOnBand = true;
	}
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

	// Intent to hold this wall. Closing speed is the primary signal, and it doubles as
	// the anti-re-stick guard: right after a kick the cat travels AWAY from the wall it
	// left, so that wall fails by construction and only the wall being approached wins.
	//
	// But a wall you are already pressed against EATS your input — the capsule cannot
	// advance, so there is no closing velocity to measure even though the player is
	// plainly asking for the wall. A standing jump up a wall measured 9–14 cm/s against
	// a 20 gate and could never catch (2026-07-25). Same lesson the mantle detector
	// taught the same day: intent and velocity disagree when something is in the way.
	// So held input counts too — gated on NOT separating, which is what preserves the
	// anti-re-stick guard, since a kick leaves at ~420 and fails that outright.
	FVector HorizVel = CMC->Velocity;
	HorizVel.Z = 0.0f;
	const float ClosingSpeed = FVector::DotProduct(HorizVel, -WallNormal);
	bool bWantsWall = ClosingSpeed >= WallClingMinApproachSpeed;
	if (!bWantsWall)
	{
		const bool bPressingIn = Cat->PivotInputStaleTime < 0.15f
			&& FVector::DotProduct(Cat->PivotLiveInputDir, -WallNormal) >= WallClingPressDot;
		const bool bNotSeparating = ClosingSpeed > -WallClingMinApproachSpeed;
		bWantsWall = bPressingIn && bNotSeparating;
	}
	if (!bWantsWall)
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

	// A fresh grip ends any in-flight kick anim (chimney: kick → cross → re-cling).
	// No snap on the yaw-hold release — DriveWallAttach faces the NEW wall on its
	// first tick, which is the rotation this state actually wants.
	Cat->FinishWallKickYawHold(/*bSnapToLaunchYaw=*/false);
	Cat->bGoWallKick        = false;
	Cat->bWallKickArcPhase  = false;
	Cat->WallKickAnimTimer  = 0.0f;

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
	// EASE-OUT, wrap-safe — see WallAttachFaceInterpSpeed: settles the tail of the kick
	// sweep onto the face; near no-op for ordinary already-facing approaches.
	{
		const float CurYaw = Cat->GetActorRotation().Yaw;
		const float Delta  = FMath::FindDeltaAngleDegrees(CurYaw, (-AttachNormal).Rotation().Yaw);
		const float Alpha  = FMath::Clamp(DeltaTime * WallAttachFaceInterpSpeed, 0.0f, 1.0f);
		Cat->SetActorRotation(FRotator(0.0f, CurYaw + Delta * Alpha, 0.0f));
	}

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

	// A pending anticipated kick dies with the attach when something that owns the cat
	// outright ends it — grounded (a launch from the floor would be a superjump),
	// grabbed, mantled, or the server mirror. Timeout / wall-lost do NOT cancel: the
	// player pressed, so the kick in flight still fires from TickComponent.
	if (PendingKickTimer > 0.0f
		&& (Reason == EWallAttachEnd::Grounded || Reason == EWallAttachEnd::Grabbed
			|| Reason == EWallAttachEnd::Mantled || Reason == EWallAttachEnd::Remote
			|| Reason == EWallAttachEnd::Aborted))
	{
		PendingKickTimer = 0.0f;
		if (ACatBase* KickCat = GetCat())
		{
			KickCat->bGoWallKick       = false;   // the press's anim arm rolls back too
			KickCat->bWallKickArcPhase = false;
			KickCat->WallKickAnimTimer = 0.0f;
		}
	}

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
	if (PendingKickTimer > 0.0f)
	{
		return true;   // a kick is already loading — consume the press, don't double-arm
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
	const bool bFromCling = IsWallAttached();
	if (bFromCling)
	{
		// Clinging: kick off the face we are HOLDING, not whatever the radial probe
		// finds. In a chimney both walls are usually in reach, so re-probing here could
		// kick off the wrong one — and the whole point of the cling is that the player
		// chooses the moment, having already chosen the wall.
		WallNormal = AttachNormal;
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

	// Twist side = the lateral component of the HELD INPUT across the launch line —
	// the twist leads the arc the player is about to steer. Neutral input alternates
	// shoulders (the chimney rhythm). This is the only signal that varies per kick:
	// anything derived from actor-vs-launch geometry is constant from a cling, because
	// DriveWallAttach re-faces the wall every frame (the rounds-1/2 always-one-way bug).
	FVector Lateral = WallNormal; Lateral.Z = 0.0f; Lateral.Normalize();
	const FVector RightDir = FVector::CrossProduct(FVector::UpVector, Lateral);
	float Side = 0.0f;
	if (Cat->PivotInputStaleTime < 0.15f && !Cat->PivotLiveInputDir.IsNearlyZero())
	{
		Side = FVector::DotProduct(Cat->PivotLiveInputDir, RightDir);
	}
	const bool bKickRight = (FMath::Abs(Side) > 0.3f) ? (Side > 0.0f) : !bLastKickRight;
	bLastKickRight = bKickRight;

	// Chimney transfer: a cling kick with an opposite wall in range rides the clip's
	// timeline instead of ballistics (see ECatTraversalState::WallTransfer). Probe
	// along the HELD normal — the wall the kick leaves toward is by definition the
	// one the cling faces away from.
	if (bFromCling && bEnableWallTransfer && GetWorld())
	{
		const FVector P0 = Cat->GetActorLocation();
		FHitResult Hit;
		FCollisionQueryParams Params(FName(TEXT("CatWallTransfer")), false, Cat);
		if (GetWorld()->LineTraceSingleByChannel(Hit, P0, P0 + AttachNormal * WallTransferMaxGap,
				ECC_Visibility, Params)
			&& !Hit.bStartPenetrating && Hit.ImpactNormal.Z < 0.5f)
		{
			FVector NormalB = Hit.ImpactNormal;
			NormalB.Z = 0.0f;
			if (NormalB.Normalize())
			{
				FVector Target = Hit.ImpactPoint + NormalB * WallTransferStandOff;
				// The climb is DERIVED from the authored leap, not dialled in: at a
				// fixed launch angle the arc's height follows from the span, so the
				// gap decides the climb (and that pairing is the level-blocking spec).
				{
					const float G = FVector::Dist2D(P0, Target);
					Target.Z = P0.Z + CatTransferArc::Solve(G, GetWallTransferPushTime(),
						FMath::Max(GetWallTransferDuration() - GetWallTransferPushTime(), 0.05f)).TotalClimb;
				}
				StartWallTransfer(P0, Target, NormalB, bKickRight);
				if (!Cat->HasAuthority())
				{
					Cat->Server_WallTransfer(P0, Target, NormalB, bKickRight);
				}
				return true;
			}
		}
	}

	if (bFromCling && WallBounceAnticipation > 0.0f)
	{
		// ANTICIPATION: the clip's first ~0.13 s is the load-and-push ON the wall, so
		// the grip holds (DriveWallAttach keeps pinning velocity and facing) while the
		// clip plays, and the launch fires when the clip's paws actually leave. The
		// anim starts NOW on the owner; the server/proxies start it at the RPC.
		PendingKickTimer  = WallBounceAnticipation;
		PendingKickNormal = WallNormal;
		bPendingKickRight = bKickRight;
		Cat->bGoWallKick        = true;
		Cat->bWallKickRight     = bKickRight;
		Cat->bWallKickArcPhase  = false;
		Cat->WallKickAnimTimer  = Cat->WallKickAnimDuration;
		return true;
	}

	// Raw mid-air bounce (or anticipation disabled): instant, as before.
	DoWallBounce(WallNormal, bKickRight);
	if (!Cat->HasAuthority())
	{
		Cat->Server_WallBounce(WallNormal, bKickRight);
	}
	return true;
}

void UCatTraversalComponent::DoWallBounce(const FVector& WallNormal, bool bKickRight)
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

	// Kick anim: side chosen by the caller (held-input lateral, alternate on neutral —
	// see TryWallBounce) and carried through the RPC so every machine picks the same
	// clip. An anticipated cling kick armed the window at the PRESS (the clip has been
	// playing its load-and-push since then) — don't restart it here.
	if (!Cat->bGoWallKick)
	{
		Cat->WallKickAnimTimer = Cat->WallKickAnimDuration;
	}
	Cat->bGoWallKick        = true;
	Cat->bWallKickRight     = bKickRight;
	Cat->bWallKickArcPhase  = false;

	// The wall-hug slide is deliberately NOT zeroed here (round-4 fix). A cling hug
	// rides up to ~40 uu, and the round-2 hard zero was a one-frame mesh snap at
	// every kick press (PawPrint 2026-08-01: WallHugSlide 39.5 → 0 in one sample).
	// With the actor frame HELD through the kick there is no flip to protect against
	// any more — the hug decays through its own interp as the trace stops finding
	// the wall, which renders as paw contact easing off while the body springs away.
	// FinishWallKickYawHold zeroes any residual when the frame finally rotates.

	// No snap here — arm the YAW EASE (round 5): the actor sweeps to the launch line
	// at WallKickTurnRate along the chosen shoulder while the clip contributes only
	// its push-segment POSE. One rotation source end to end: the ease runs through
	// the flight, and a re-cling's own face-ease (or grounded orient-to-movement)
	// continues the turn from wherever this one is. Prior models both failed on
	// composition: frame-1 snap (rounds 1-2) double-rotated against the in-pose
	// twist; hold-then-late-flip (round 3-4) never reached its flip in chimneys and
	// re-clings flipped back through the un-twisting pose blend.
	if (bWallBounceReorientBody)
	{
		Cat->WallKickFaceYaw = Lateral.Rotation().Yaw;
		Cat->WallKickTurnDir = bKickRight ? 1.0f : -1.0f;

		// Latch the SmoothStep sweep: total angle measured ALONG the chosen shoulder
		// (never shortest-path), duration from the rate so bigger turns take longer.
		Cat->WallKickSweepStartYaw = Cat->GetActorRotation().Yaw;
		float Angle = FMath::Fmod(Cat->WallKickTurnDir > 0.0f
			? (Cat->WallKickFaceYaw - Cat->WallKickSweepStartYaw)
			: (Cat->WallKickSweepStartYaw - Cat->WallKickFaceYaw), 360.0f);
		if (Angle < 0.0f)
		{
			Angle += 360.0f;
		}
		Cat->WallKickSweepAngle    = Cat->WallKickTurnDir > 0.0f ? Angle : -Angle;
		Cat->WallKickSweepDuration = FMath::Max(Angle / FMath::Max(Cat->WallKickTurnRate, 90.0f), 0.05f);
		Cat->WallKickSweepElapsed  = 0.0f;
		Cat->bWallKickHoldingYaw   = true;
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

		// Yaw ease owns rotation: air steering feeds acceleration, and orient-to-
		// movement would fight the sweep. Restored by FinishWallKickYawHold at ease
		// arrival / landing / new attach / mantle.
		if (bWallBounceReorientBody)
		{
			CMC->bOrientRotationToMovement = false;
		}

		// Rebound window: drop lateral air friction so the kick actually carries.
		if (WallBounceReboundTime > 0.0f)
		{
			CMC->FallingLateralFriction = WallBounceReboundLateralFriction;
		}
	}
	WallBounceReboundTimer = WallBounceReboundTime;
	WallBounceReboundDir   = Lateral;

	WallBounceCooldownTimer = WallBounceCooldown;

	// Per-bounce spec line (the stop/start/mantle log doctrine).
	UE_LOG(LogCatVentures, Log,
		TEXT("[%s] Wall bounce — normal (%.2f, %.2f), launch %.0f lateral / %.0f up, kick %s"),
		*Cat->GetName(), Lateral.X, Lateral.Y, WallBounceLateralSpeed, WallBounceVerticalSpeed,
		bKickRight ? TEXT("R") : TEXT("L"));
}

// ══════════════════════════════════════════════════════════════════════════
// ── Wall Transfer (the chimney crossing as a takeover) ────────────────────
// ══════════════════════════════════════════════════════════════════════════

float UCatTraversalComponent::GetWallTransferProgress() const
{
	return IsWallTransferring()
		? FMath::Clamp(TransferElapsed / FMath::Max(GetWallTransferDuration(), 0.05f), 0.0f, 1.0f)
		: 0.0f;
}

// (GetWallTransferPitchTarget and the whole procedural transfer-pitch layer are GONE.
//  Sean A/B'd it live over 26 crossings — held pose with NO trajectory pitch won, and
//  a knob whose chosen value is "off" is a trap for a later session. The launch
//  attitude is authored into the clip instead, which is why the layer became
//  redundant: rotating the mesh to the tangent AND baking the tangent into the pose
//  are two owners of one quantity, and the whole saga is a catalogue of what that
//  costs. If a future round wants the body to track the arc over, resurrect it from
//  git — but author the clip flat first, or it double-counts.)

void UCatTraversalComponent::StartWallTransfer(const FVector& InStart, const FVector& InTarget,
	const FVector& InTargetNormal, bool bKickRight)
{
	ACatBase* Cat = GetCat();
	if (!Cat || TraversalState == ECatTraversalState::Mantle
		|| TraversalState == ECatTraversalState::WallTransfer)
	{
		return;
	}
	if (IsWallAttached())
	{
		EndWallAttach(EWallAttachEnd::Kick);
	}

	TraversalState  = ECatTraversalState::WallTransfer;
	TransferStart   = InStart;
	TransferTarget  = InTarget;
	TransferNormalB = InTargetNormal;
	TransferElapsed = 0.0f;

	// The single CMC takeover point (the mantle doctrine): Flying kills gravity
	// while the curve owns the capsule; the restore lives in EndWallTransfer only.
	if (UCharacterMovementComponent* CMC = Cat->GetCharacterMovement())
	{
		CMC->StopMovementImmediately();
		CMC->SetMovementMode(MOVE_Flying);
	}

	// The clip plays ONLY its push segment (round 10): the anim window is the push
	// length, after which the ACatBase::Tick expiry flips bWallKickArcPhase and the
	// WallKick state blends to the authored sailing pose (A_Cat_Transfer_Arc) for
	// the crossing — bGoWallKick spans the WHOLE transfer (round 13; the crossing
	// previously exited to Jump_Fall and borrowed its nose-down apex pose). The
	// JumpDown clip's back half is dismount acrobatics that somersaulted on a
	// rising crossing — parked for a future high-dismount move.
	Cat->bGoWallKick        = true;
	Cat->bWallKickRight     = bKickRight;
	Cat->bWallKickArcPhase  = false;
	Cat->WallKickAnimTimer  = GetWallTransferPushTime();

	// Latch the yaw swing: from the cling facing to the far-wall facing, measured
	// ALONG the chosen shoulder (a chimney 180 sits on the wrap boundary — the
	// round-5 lesson). DriveWallTransfer sweeps it on a SmoothStep of progress, so
	// the turn completes just before the catch and cannot be interrupted.
	TransferStartYaw = Cat->GetActorRotation().Yaw;
	const float EndYaw = (-InTargetNormal).Rotation().Yaw;
	const float Dir = bKickRight ? 1.0f : -1.0f;
	float Angle = FMath::Fmod(Dir > 0.0f ? (EndYaw - TransferStartYaw)
	                                     : (TransferStartYaw - EndYaw), 360.0f);
	if (Angle < 0.0f)
	{
		Angle += 360.0f;
	}
	TransferSwingAngle = Dir > 0.0f ? Angle : -Angle;

	UE_LOG(LogCatVentures, Log,
		TEXT("[%s] Wall transfer START — gap %.0f uu, climb %.0f uu, %.2f s, kick %s"),
		*Cat->GetName(), FVector::Dist2D(InStart, InTarget), InTarget.Z - InStart.Z,
		GetWallTransferDuration(), bKickRight ? TEXT("R") : TEXT("L"));
}

void UCatTraversalComponent::DriveWallTransfer(float DeltaTime)
{
	ACatBase* Cat = GetCat();
	if (!Cat)
	{
		EndWallTransfer(false);
		return;
	}
	UCharacterMovementComponent* CMC = Cat->GetCharacterMovement();
	if (!CMC || CMC->MovementMode != MOVE_Flying || Cat->IsGrabbing())
	{
		EndWallTransfer(false);
		return;
	}

	TransferElapsed += DeltaTime;
	const float P = FMath::Clamp(TransferElapsed / FMath::Max(GetWallTransferDuration(), 0.05f), 0.0f, 1.0f);

	// ── THE WALL PHASE IS THE SOURCE CLIP'S OWN ROOT MOTION (round 19b) ──────
	// The whole 19-round saga came from playing the *_IP variant, which has the
	// clip's motion FLATTENED into the root track and then discarded: measured on
	// the IP the clip looks like it has no turn, so every round tried to
	// re-synthesise one procedurally. Sampling the SOURCE *_RM with root motion
	// incorporated shows the real move, all of it authored (60 fps frames):
	//   f0–12   settle/coil — hips sink 2.5, all four paws planted
	//   f12–33  SPRING UP THE WALL — root rises 31 uu with BOTH HANDS FIXED IN
	//           WORLD SPACE (a pull-up); hinds re-plant higher at f27
	//   f33–48  THE TURN — root yaw swings 144° (L) / 128° (R) while the HIND FEET
	//           STAY PLANTED; hands release at f42–45
	//   f48+    the falling dismount — root drops away. THE ONLY WRONG PART: cut.
	// So the capsule follows the root curve and the ABP renders the root-relative
	// (IP) pose — the mantle model, and by definition it reconstructs the source.
	// Verified before shipping: composed against our retarget, hands hold 65–71 uu
	// through a 32 uu body rise and hinds hold 53–56 through the whole turn.
	// Round 20 — the wall phase is now TURN + BRIDGE + SPRING (A_Cat_Wall_Spring_*,
	// 1.2333 s / 74 f): JumpDown f0–45 verbatim (coil, 31 uu pull-up on fixed hands,
	// 124° of turn on planted hinds) → a 10-frame authored bridge → Climb_Start
	// f12–30 (crouch bottom → explosive extension → tuck), which is a real ~40°
	// leap in the source and so matches our launch angle. These tables are the
	// COMBINED root motion: JumpDown's root through the turn, the bridge holding
	// position while the remaining yaw sweeps, then Climb_Start's authored +33 rise
	// / +33 drive as the actual push-off — so the cat leaves the wall on the spring
	// rather than on a formula.
	using namespace CatTransferArc;

	FVector AcrossDir = TransferTarget - TransferStart;
	AcrossDir.Z = 0.0f;
	const float Gap = AcrossDir.Size();
	AcrossDir = Gap > 1.0f ? AcrossDir / Gap : FVector::ZeroVector;

	const float PushFrac = FMath::Clamp(GetWallTransferPushTime() / FMath::Max(GetWallTransferDuration(), 0.1f), 0.1f, 0.85f);
	const float LatU = FMath::Clamp((P - PushFrac) / (1.0f - PushFrac), 0.0f, 1.0f);
	const float WallU = FMath::Clamp(P / FMath::Max(PushFrac, 0.01f), 0.0f, 1.0f);
	const float FlightTime = FMath::Max(GetWallTransferDuration() - GetWallTransferPushTime(), 0.05f);
	const FSolve Arc = Solve(Gap, GetWallTransferPushTime(), FlightTime);

	const float Across = (P < PushFrac)
		? Sample(Sway, WallU) + Arc.SpringAcross * Sample(SpringNorm, WallU)
		: Arc.AcrossEnd + Arc.HorizSpan * LatU;       // constant speed, lands exactly on Gap
	FVector Pos = TransferStart + AcrossDir * Across;
	Pos.Z = TransferStart.Z + (P < PushFrac
		? Sample(RootZ, WallU) + Arc.SpringZ * Sample(SpringNorm, WallU)
		: Arc.ZEnd + Arc.B * LatU + Arc.C * LatU * LatU + Arc.D * LatU * LatU * LatU);
	Cat->SetActorLocation(Pos);

	// (No trajectory tangent any more — nothing consumes it. The pose owns attitude.)

	// The body turn — the ACTOR follows the clip's own root yaw (round 19b). The
	// rotation is authored: it is flat for the first 60% (coil + pull-up, no turn
	// at all) and then swings through in the last 40% ON PLANTED HINDS. Magnitudes
	// are per-side because the pack's two takes differ (L 144.4°, R 128.5°); the
	// remainder to the far-wall facing sweeps over the flight, which is a small
	// mid-air correction rather than the "dead cat spinning" rotisserie.
	// The yaw shape now carries the WHOLE turn: the clip's authored 124° swing on
	// planted hinds, then the bridge sweeping the remainder so the cat is aimed at
	// the far wall BEFORE the spring fires (a launch must be aimed, and leftover
	// rotation in flight is the "spinning" of round 16). The flight therefore has
	// essentially nothing left to sweep.
	// NO seam flip: the clip ends in a full extension and the sail is a gathered
	// sail, so pose-distance alignment has no minimum worth trusting (RMS 34.8 at
	// its best angle — noise). Extension → sail is a legitimate animation blend and
	// the 0.10 s crossfade owns it; a flip here would be inventing a rotation to
	// cancel a difference that is not a rotation. (The 153° flip of rounds 10–18 was
	// exactly that mistake.)
	const float TurnSign = FMath::Sign(TransferSwingAngle);
	const float TurnMag  = FMath::Min(180.0f, FMath::Abs(TransferSwingAngle));
	const float PushEndYaw = TransferStartYaw + TurnSign * TurnMag;
	const float S = FMath::SmoothStep(PushFrac, 0.9f, P);
	const float Yaw = (P < PushFrac)
		? TransferStartYaw + TurnSign * TurnMag * Sample(YawShape, WallU)
		: PushEndYaw + (TransferStartYaw + TransferSwingAngle - PushEndYaw) * S;
	Cat->SetActorRotation(FRotator(0.0f, Yaw, 0.0f));

	// One-shot launch report: the derived numbers that decide whether the crossing
	// reads continuous. Exit and flight speeds should MATCH by construction — if a
	// future table edit breaks that, this line says so instead of the eye having to.
	if (TransferElapsed <= DeltaTime * 1.5f)
	{
		UE_LOG(LogCatVentures, Log,
			TEXT("[%s] Transfer arc — gap %.0f -> climb %.0f (wall %.0f + flight %.0f), "
			     "launch %.0f deg, apex %.0f%% at +%.0f, arrival %.0f deg  [LEVEL SPEC]"),
			*Cat->GetName(), Gap, Arc.TotalClimb, Arc.ZEnd, Arc.FlightClimb,
			CatTransferArc::LaunchDeg, CatTransferArc::ApexFrac * 100.0f,
			Arc.B * ApexFrac + Arc.C * ApexFrac * ApexFrac + Arc.D * ApexFrac * ApexFrac * ApexFrac,
			CatTransferArc::ArrivalDeg);
	}

	if (Cat->PawPrint)
	{
		static const FName ChTransfer(TEXT("TransferProgress"));
		Cat->PawPrint->SampleChannel(ChTransfer, P);
	}

	if (P >= 1.0f)
	{
		EndWallTransfer(true);
	}
}

void UCatTraversalComponent::EndWallTransfer(bool bCompleted)
{
	if (TraversalState != ECatTraversalState::WallTransfer)
	{
		return;
	}
	TraversalState = ECatTraversalState::None;

	ACatBase* Cat = GetCat();
	if (Cat)
	{
		if (UCharacterMovementComponent* CMC = Cat->GetCharacterMovement())
		{
			CMC->SetMovementMode(MOVE_Falling);
			CMC->Velocity = FVector::ZeroVector;
		}
		if (bCompleted)
		{
			// THE SEAM FLIP: the clip's end pose faces actor-BACKWARD, so flipping the
			// actor to face the far wall on the same frame the cling pose (actor-
			// forward) takes over keeps the rendered facing continuous — the takeover
			// guarantees the clip reached its end, which is what made this composition
			// impossible in the ballistic rounds. The WallKick → WallHang transition
			// is a near-cut for the same reason.
			Cat->SetActorRotation(FRotator(0.0f, (-TransferNormalB).Rotation().Yaw, 0.0f));
			StartWallAttach(TransferNormalB, 0.0f, 0.0f);
		}
		else
		{
			Cat->bGoWallKick        = false;
			Cat->bWallKickArcPhase  = false;
			Cat->WallKickAnimTimer  = 0.0f;
		}
		UE_LOG(LogCatVentures, Log, TEXT("[%s] Wall transfer END — %s"),
			*Cat->GetName(), bCompleted ? TEXT("caught far wall") : TEXT("aborted"));
	}
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
	// EASED, not snapped (2026-07-31): the detector aims down the HEADING, which
	// camera-relative input + air control pull away from the facing, so an oblique
	// catch teleport-rotated the body in one frame under a camera that doesn't
	// follow — Sean's camera-positional "scramble" repro. DriveMantle swings the
	// yaw over the first MantleFaceAlpha of the mantle instead.
	MantleStartYaw  = Cat->GetActorRotation().Yaw;
	MantleTargetYaw = MantleStartYaw;
	FVector Facing = InTarget - InStart;
	Facing.Z = 0.0f;
	if (Facing.Normalize())
	{
		MantleTargetYaw = Facing.Rotation().Yaw;
	}
	const float SnapDeg = FMath::Abs(FMath::FindDeltaAngleDegrees(MantleStartYaw, MantleTargetYaw));

	// The single CMC takeover point for this component: Flying kills gravity while
	// the curve owns the capsule; the restore lives in EndMantle and nowhere else.
	if (UCharacterMovementComponent* CMC = Cat->GetCharacterMovement())
	{
		CMC->StopMovementImmediately();
		CMC->SetMovementMode(MOVE_Flying);
	}
	// Height bucket: latched here so a mid-mantle nothing can switch clips (the
	// PivotAngleDeg precedent). Owner and server both derive it from the same RPC'd
	// params; proxies read the replicated bMantleVault. The +3 margin covers the
	// ~2 uu target float above the lip: a catch at the detection band's very top
	// measures 71–72 here and slipped past a bare <= 70 into the PARKED climb
	// composition (Sean's one unreproducible "flapping" mantle, log 2026-07-31 —
	// ledge 72 [climb] in an otherwise all-vault session).
	bMantleIsVault = (MantleTarget.Z - MantleStart.Z) <= MantleVaultMaxHeight + 3.0f;
	Cat->bMantleVault = bMantleIsVault;
	Cat->MantleLedgeHeight = MantleTarget.Z - MantleStart.Z;
	Cat->SetMantleAnimState(true, 0.0f);

	// A mantle outranks any in-flight kick anim (bounce into a ledge → clamber).
	// No snap — the mantle latched MantleStartYaw above and eases to the ledge line.
	Cat->FinishWallKickYawHold(/*bSnapToLaunchYaw=*/false);
	Cat->bGoWallKick        = false;
	Cat->bWallKickArcPhase  = false;
	Cat->WallKickAnimTimer  = 0.0f;

	UE_LOG(LogCatVentures, Log, TEXT("[%s] Mantle START — ledge %.0f uu, duration %.2f s, face-swing %.0f deg [%s]"),
		*Cat->GetName(), MantleTarget.Z - MantleStart.Z, MantleDuration, SnapDeg,
		bMantleIsVault ? TEXT("vault") : TEXT("climb"));
	if (Cat->PawPrint)
	{
		static const FName ChSnap(TEXT("MantleFaceDeg"));
		Cat->PawPrint->SampleChannel(ChSnap, SnapDeg);
	}
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

	// Capsule path = the clamber clip's own root track (2026-07-30), sampled from
	// Cat_Ledge_Climb_Up_RM at 11 points: a near-linear pull-up completing by
	// progress 0.4, THEN a pure forward walk-out — strictly sequential, no overlap.
	// The original overlapped smoothsteps fought the clip's choreography (limbs are
	// authored against exactly this trajectory; §B2's MantleCrouchDrop carries the
	// pelvis-local remainder). Change these tables only alongside a clip change.
	static constexpr float MantleRootZ[11] =
		{ 0.0f, 0.2623f, 0.565f, 0.8319f, 0.9864f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
	static constexpr float MantleRootXY[11] =
		{ 0.0f, 0.0f, 0.0f, 0.0f, 0.0294f, 0.1968f, 0.3616f, 0.5222f, 0.7042f, 0.9131f, 1.0f };
	// Vault bucket (Ledge_050M root track): crouch-load holds the start, the hop rises
	// through the middle while the forward walk runs the WHOLE clip near-linearly.
	static constexpr float VaultRootZ[11] =
		{ 0.0f, 0.0f, 0.0f, 0.0f, 0.3445f, 0.6876f, 0.9809f, 1.0f, 1.0f, 1.0f, 1.0f };
	static constexpr float VaultRootXY[11] =
		{ 0.0f, 0.088f, 0.1527f, 0.2718f, 0.3813f, 0.4904f, 0.6048f, 0.7174f, 0.8195f, 0.9106f, 1.0f };
	// Catmull-Rom, not linear: knot-velocity kinks in the capsule path beat against
	// the smooth clip pose and pump the paws (the "flapping" diagnosis, 2026-07-31).
	auto SampleCurve = [](const float (&Table)[11], float P)
	{
		const float S = FMath::Clamp(P, 0.0f, 1.0f) * 10.0f;
		const int32 Seg = FMath::Min(FMath::FloorToInt32(S), 9);
		const float U = S - Seg;
		const float P0 = Table[FMath::Max(Seg - 1, 0)], P1 = Table[Seg];
		const float P2 = Table[Seg + 1], P3 = Table[FMath::Min(Seg + 2, 10)];
		return 0.5f * ((2.f*P1) + (-P0+P2)*U + (2.f*P0-5.f*P1+4.f*P2-P3)*U*U + (-P0+3.f*P1-3.f*P2+P3)*U*U*U);
	};
	const float ZAlpha  = SampleCurve(bMantleIsVault ? VaultRootZ  : MantleRootZ,  Alpha);
	float XYAlpha = SampleCurve(bMantleIsVault ? VaultRootXY : MantleRootXY, Alpha);

	// Body-swing progress (see StartMantle — the oblique-catch snap fix). Used for
	// the yaw ease below AND to gate the face crossing on big-swing catches: while
	// the body is still swinging it is SIDEWAYS to the ledge, and a sideways cat is
	// far longer than the capsule is wide — crossing the face mid-swing buried half
	// the body in the wall (Sean's 87–92° clip-through repro, 2026-07-31). Normal
	// catches (face-swing ≤ 35°) are untouched; the gate ramps in by 80°.
	const float FaceAlpha = FMath::SmoothStep(0.0f, FMath::Max(MantleFaceAlpha, 0.05f), Alpha);
	const float SwingDeg  = FMath::Abs(FMath::FindDeltaAngleDegrees(MantleStartYaw, MantleTargetYaw));
	const float SwingGate = FMath::SmoothStep(35.0f, 80.0f, SwingDeg);
	if (SwingGate > 0.0f)
	{
		XYAlpha = FMath::Lerp(XYAlpha, XYAlpha * FaceAlpha, SwingGate);
	}

	// Climb bucket: close to a standard wall stand-off DURING the rise. Detection arms
	// up to 45 uu out, but the drop tables/hug are contact-calibrated with the capsule
	// ~at the face — a far catch left every paw ~20 uu off the wall for the whole pull
	// (MantlePawGapMin, 2026-07-31: ledge 64, sentinel-pegged progress 0.12–0.53). The
	// mesh hug can't close it (capped at 5 for chest clipping), so the CAPSULE does:
	// XY path = catch → 12 uu off the face → over the lip. Derived purely from the
	// replicated Start/Target, so owner and server stay identical.
	FVector XYFrom = MantleStart;
	if (!bMantleIsVault)
	{
		// Target sits 12 inside the face along the approach; 24 back = 12 outside it.
		constexpr float StandoffPullback = 24.0f;
		FVector Dir = MantleTarget - MantleStart;
		Dir.Z = 0.0f;
		const float Dist2D = Dir.Size();
		if (Dist2D > StandoffPullback)
		{
			const FVector Standoff = MantleTarget - (Dir / Dist2D) * StandoffPullback;
			const float CloseAlpha = FMath::SmoothStep(0.0f, 0.35f, Alpha);
			XYFrom.X = FMath::Lerp(MantleStart.X, Standoff.X, CloseAlpha);
			XYFrom.Y = FMath::Lerp(MantleStart.Y, Standoff.Y, CloseAlpha);
		}
	}
	FVector NewLoc;
	NewLoc.X = FMath::Lerp(XYFrom.X, MantleTarget.X, XYAlpha);
	NewLoc.Y = FMath::Lerp(XYFrom.Y, MantleTarget.Y, XYAlpha);
	NewLoc.Z = FMath::Lerp(MantleStart.Z, MantleTarget.Z, ZAlpha);
	Cat->SetActorLocation(NewLoc, false);

	// Swing the body onto the ledge line: shortest signed arc, FaceAlpha from above.
	const float NewYaw = MantleStartYaw
		+ FMath::FindDeltaAngleDegrees(MantleStartYaw, MantleTargetYaw) * FaceAlpha;
	Cat->SetActorRotation(FRotator(0.0f, NewYaw, 0.0f));

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
				// Step-out speed ONLY if the player is actually steering: with no input,
				// handing 150 forward shoved the cat ~15 cm and braked it dead in 0.1 s —
				// a physical lurch right as the anim settles (2026-07-30). BUT the check
				// cannot run HERE: Move() input is suppressed until this very call, so
				// GetCurrentAcceleration is always zero on the exit frame and the boost
				// never fired — every mantle ended in a dead stop and running exits
				// re-accelerated from 0 (the residual "long jump end jitter", PawPrint
				// 2026-07-31: Speed 0 → ramp on every END). Defer it: arm a short window
				// and let TickComponent apply the step-out on the first tick that sees
				// real acceleration (owner input / server replayed moves both qualify).
				CMC->Velocity = FVector::ZeroVector;
				MantleExitBoostTimer = 0.12f;
			}
		}
		if (bCompleted)
		{
			// Hold the Mantle anim state through the ~2-uu exit drop so the SM never
			// flashes Jump_Fall between Mantle and Jump_Land (the land prediction traces
			// along the mostly-horizontal exit velocity and misses the floor 2 uu below).
			// Landed() clears it; a 0.5 s failsafe covers a slid-off-the-lip exit.
			Cat->BeginMantleAnimHold();
		}
		else
		{
			// Abort: clear immediately but FREEZE the scrub at its last value — the skid
			// lesson: the blend-out must leave from the pose actually on screen, and a
			// reset-to-0 (or snap-to-1) here would yank the evaluator mid-blend.
			Cat->SetMantleAnimState(false, Cat->MantleProgress);
		}

		// Per-mantle spec line — the clamber-clip authoring numbers (the stop/start
		// log-line doctrine): ledge height + takeover duration.
		UE_LOG(LogCatVentures, Log, TEXT("[%s] Mantle %s — ledge %.0f uu in %.2f s"),
			*Cat->GetName(), bCompleted ? TEXT("END") : TEXT("ABORT"),
			MantleTarget.Z - MantleStart.Z, MantleElapsed);
	}
}
