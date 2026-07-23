#include "CatTraversalComponent.h"

#include "CatBase.h"
#include "CatVenturesLog.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"

UCatTraversalComponent::UCatTraversalComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

ACatBase* UCatTraversalComponent::GetCat() const
{
	return Cast<ACatBase>(GetOwner());
}

void UCatTraversalComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	CooldownTimer = FMath::Max(CooldownTimer - DeltaTime, 0.0f);

	if (TraversalState == ECatTraversalState::Mantle)
	{
		DriveMantle(DeltaTime);
		return;
	}

	// Detection runs only for the locally controlled cat (the server and proxies get
	// the takeover via the pawn RPC + replication, never their own detection).
	ACatBase* Cat = GetCat();
	if (Cat && Cat->IsLocallyControlled())
	{
		TryDetectMantle();
	}
}

void UCatTraversalComponent::TryDetectMantle()
{
	ACatBase* Cat = GetCat();
	if (!bEnableMantle || !Cat || CooldownTimer > 0.0f)
	{
		return;
	}

	// Airborne, deliberate (input held), hands free. All grounded verbs are excluded
	// by the airborne gate — no CMC precedence overlap with stop/start/pivot.
	UCharacterMovementComponent* CMC = Cat->GetCharacterMovement();
	if (!CMC || !CMC->IsFalling() || Cat->IsGrabbing() || !Cat->HasMovementInput())
	{
		return;
	}

	UCapsuleComponent* Capsule = Cat->GetCapsuleComponent();
	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	const FVector Center = Cat->GetActorLocation();
	const float CapsuleBottomZ = Center.Z - HalfHeight;
	FVector Fwd = Cat->GetActorForwardVector();
	Fwd.Z = 0.0f;
	if (!Fwd.Normalize())
	{
		return;
	}

	FCollisionQueryParams Params(FName(TEXT("CatMantle")), false, Cat);

	// 1. Chest probe: a wall face ahead.
	FHitResult WallHit;
	if (!GetWorld()->LineTraceSingleByChannel(WallHit, Center,
			Center + Fwd * MantleReachDistance, ECC_Visibility, Params)
		|| WallHit.ImpactNormal.Z >= 0.5f)
	{
		return;
	}

	// 2. Lip probe: floor within the mantleable band past the wall face.
	const FVector LipStart(
		WallHit.ImpactPoint.X + Fwd.X * MantleForwardClearance,
		WallHit.ImpactPoint.Y + Fwd.Y * MantleForwardClearance,
		CapsuleBottomZ + MantleMaxLedgeHeight + 10.0f);
	FHitResult LipHit;
	if (!GetWorld()->LineTraceSingleByChannel(LipHit, LipStart,
			LipStart - FVector(0, 0, MantleMaxLedgeHeight - MantleMinLedgeHeight + 20.0f),
			ECC_Visibility, Params)
		|| LipHit.ImpactNormal.Z <= 0.7f)
	{
		return;
	}

	const float LedgeHeight = LipHit.ImpactPoint.Z - CapsuleBottomZ;
	if (LedgeHeight < MantleMinLedgeHeight || LedgeHeight > MantleMaxLedgeHeight)
	{
		return;
	}

	// 3. Headroom: the landing spot must fit the capsule.
	const FVector Target = LipHit.ImpactPoint + Fwd * 12.0f
		+ FVector(0, 0, HalfHeight + 2.0f);
	FHitResult RoomHit;
	if (GetWorld()->LineTraceSingleByChannel(RoomHit, Target,
			Target + FVector(0, 0, HalfHeight), ECC_Visibility, Params))
	{
		return;
	}

	StartMantle(Center, Target);
	if (!Cat->HasAuthority())
	{
		Cat->Server_StartMantle(Center, Target);
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
