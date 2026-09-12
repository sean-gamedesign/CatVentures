#include "CatImpactResponseComponent.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "CatBase.h"
#include "CatVenturesLog.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "CatTraversalComponent.h"
#include "PawPrintSubsystem.h"

namespace
{
	// Blend times for the reaction dynamic montages. Constants, not knobs: the read
	// lives in the clips; these only keep the enter/exit from popping.
	constexpr float FlinchBlendIn  = 0.10f;
	constexpr float FlinchBlendOut = 0.30f;
	constexpr float StunBlendIn    = 0.12f;
	constexpr float StunBlendOut   = 0.30f;

	const TCHAR* ImpactTierName(ECatImpactTier Tier)
	{
		switch (Tier)
		{
		case ECatImpactTier::Flinch:  return TEXT("Flinch");
		case ECatImpactTier::Stagger: return TEXT("Stagger");
		case ECatImpactTier::Ragdoll: return TEXT("Ragdoll");
		default:                      return TEXT("None");
		}
	}

	const TCHAR* ImpactDirName(ECatImpactDirection Dir)
	{
		switch (Dir)
		{
		case ECatImpactDirection::Front: return TEXT("Front");
		case ECatImpactDirection::Back:  return TEXT("Back");
		case ECatImpactDirection::Left:  return TEXT("Left");
		default:                         return TEXT("Right");
		}
	}

	const TCHAR* ImpactSourceName(ECatImpactSource Source)
	{
		switch (Source)
		{
		case ECatImpactSource::Swat:     return TEXT("Swat");
		case ECatImpactSource::Measured: return TEXT("Measured");
		default:                         return TEXT("Authored");
		}
	}
}

UCatImpactResponseComponent::UCatImpactResponseComponent()
{
	// Tick exists only to count down the stagger window — enabled on demand,
	// off the rest of the time.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;

	// Stub content defaults (retargeted 2026-08-19, /Game/Drafts/Impact). Soft paths
	// so the CDO carries no hard load; resolved on first use.
	FlinchFrontSequence = TSoftObjectPtr<UAnimSequenceBase>(FSoftObjectPath(
		TEXT("/Game/Drafts/Impact/A_Cat_Damage_Front_Left_C.A_Cat_Damage_Front_Left_C")));
	FlinchBackSequence = TSoftObjectPtr<UAnimSequenceBase>(FSoftObjectPath(
		TEXT("/Game/Drafts/Impact/A_Cat_Damage_Back_Left_IP.A_Cat_Damage_Back_Left_IP")));
	FlinchLeftSequence = TSoftObjectPtr<UAnimSequenceBase>(FSoftObjectPath(
		TEXT("/Game/Drafts/Impact/A_Cat_Damage_Left_IP.A_Cat_Damage_Left_IP")));
	FlinchRightSequence = TSoftObjectPtr<UAnimSequenceBase>(FSoftObjectPath(
		TEXT("/Game/Drafts/Impact/A_Cat_Damage_Right_IP.A_Cat_Damage_Right_IP")));
	StunSequence = TSoftObjectPtr<UAnimSequenceBase>(FSoftObjectPath(
		TEXT("/Game/Drafts/Impact/A_Cat_Stun_C.A_Cat_Stun_C")));
}

void UCatImpactResponseComponent::BeginPlay()
{
	Super::BeginPlay();
	PawPrint = GetWorld() ? GetWorld()->GetSubsystem<UPawPrintSubsystem>() : nullptr;

	// Measured-impact source: bind the capsule's hit events, authority only (the
	// classifier is server-only; clients get their reactions via the multicast).
	// The Pawn collision profile ships bNotifyRigidBodyCollision OFF — without the
	// flip, a simulating prop striking the capsule reports nothing. Destructibles
	// happen to carry the flag for their own impact-shatter path, but plain
	// PhysicsBody props may not, so the robust single point is OUR side.
	ACatBase* Cat = GetCat();
	if (Cat && Cat->HasAuthority())
	{
		if (UCapsuleComponent* Capsule = Cat->GetCapsuleComponent())
		{
			Capsule->SetNotifyRigidBodyCollision(true);
			Capsule->OnComponentHit.AddDynamic(this, &UCatImpactResponseComponent::OnCapsuleHit);
		}
	}
}

ACatBase* UCatImpactResponseComponent::GetCat() const
{
	return Cast<ACatBase>(GetOwner());
}

// ══════════════════════════════════════════════════════════════════════════
// ── Report entry points (server) ─────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void UCatImpactResponseComponent::ReportSwatImpact(ACatBase* Attacker, const FVector& ImpulseDir)
{
	ACatBase* Cat = GetCat();
	if (!Cat || !Cat->HasAuthority())
	{
		return;   // classification is server-only; HandleSwatHit already guarantees this.
	}

	FCatImpactParams Params;
	Params.Source          = ECatImpactSource::Swat;
	Params.InstigatorActor = Attacker;
	Params.ImpactPoint     = Cat->GetActorLocation();
	Params.ImpactNormal    = ImpulseDir;
	ClassifyAndReact(Params);
}

void UCatImpactResponseComponent::OnCapsuleHit(UPrimitiveComponent* /*HitComponent*/, AActor* OtherActor,
                                               UPrimitiveComponent* OtherComp, FVector NormalImpulse,
                                               const FHitResult& Hit)
{
	ACatBase* Cat = GetCat();
	if (!Cat || !Cat->HasAuthority() || !OtherComp || OtherActor == Cat)
	{
		return;
	}

	// Only the world hitting the CAT counts: a simulating body is the only thing
	// that can "hit" a kinematic capsule (walking into static furniture arrives
	// with a non-simulating component and is exactly the self-inflicted case the
	// plan filters out — landing has its own channel, LandImpactIntensity).
	if (!OtherComp->IsSimulatingPhysics())
	{
		return;
	}

	// The victim's own held prop pressing on the capsule is not the world hitting
	// the cat (plan §4.1 self-inflicted filter).
	if (OtherComp == Cat->GrabbedComponent.Get())
	{
		return;
	}

	// The classifier quantity: impactor mass × contact speed — deterministic; the
	// solver's NormalImpulse rides along for the shared Heavy-tier verify round.
	// WATCH ITEM for the PIE read: GetMass() on a Geometry Collection may report the
	// whole collection rather than the striking chunk — the logs will say.
	const float ImpactorMass  = OtherComp->GetMass();
	const float ImpactorSpeed = OtherComp->GetComponentVelocity().Size();
	const float MassSpeed     = ImpactorMass * ImpactorSpeed;

	// Noise gate, BELOW the log line: props resting against the capsule fire hit
	// events every frame at ~zero speed — logging those would flood the PawPrint tap.
	if (MassSpeed < FlinchImpactThreshold * 0.5f)
	{
		return;
	}

	FCatImpactParams Params;
	Params.Source            = ECatImpactSource::Measured;
	Params.InstigatorActor   = OtherActor;
	Params.ImpactPoint       = Hit.ImpactPoint;
	Params.ImpactorMassSpeed = MassSpeed;
	Params.NormalImpulseMag  = NormalImpulse.Size();

	// Push direction: the impactor's travel, not the contact-surface normal — a
	// glancing surface normal points anywhere; the throw's direction is the read.
	FVector Push = OtherComp->GetComponentVelocity().GetSafeNormal();
	if (Push.IsNearlyZero())
	{
		Push = -Hit.ImpactNormal;
	}
	Params.ImpactNormal = Push;

	ClassifyAndReact(Params);
}

// ══════════════════════════════════════════════════════════════════════════
// ── The classifier (server) ──────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void UCatImpactResponseComponent::ClassifyAndReact(const FCatImpactParams& Params)
{
	ACatBase* Cat = GetCat();
	if (!Cat)
	{
		return;
	}

	// ── Tier by source (plan §4). Swat maps to Flinch, full stop (Sean, 2026-08-19:
	// upgrades are a post-playtest tuning question); the STAGGER CAP on the swat
	// source is architecture and stays even if the mapping ever moves. Measured
	// climbs the threshold ladder. Authored is step-5 work.
	ECatImpactTier Tier = ECatImpactTier::None;
	switch (Params.Source)
	{
	case ECatImpactSource::Swat:
		Tier = ECatImpactTier::Flinch;
		Tier = FMath::Min(Tier, ECatImpactTier::Stagger);   // the locked cap, explicit
		break;

	case ECatImpactSource::Measured:
		if      (Params.ImpactorMassSpeed >= RagdollImpactThreshold) Tier = ECatImpactTier::Ragdoll;
		else if (Params.ImpactorMassSpeed >= StaggerImpactThreshold) Tier = ECatImpactTier::Stagger;
		else if (Params.ImpactorMassSpeed >= FlinchImpactThreshold)  Tier = ECatImpactTier::Flinch;
		else return;   // sub-flinch contact; the noise gate upstream catches most of these
		break;

	case ECatImpactSource::Authored:
		UE_LOG(LogCatVentures, Warning,
			TEXT("[Impact] %s REJECT reason=unimplemented-source source=Authored — step-5 work reached the classifier early"),
			*Cat->GetName());
		return;
	}

	const float Now = GetWorld()->GetTimeSeconds();

	// ── Ragdoll is step-4 work: downgrade loudly so a huge hit still reads instead
	// of silently doing nothing.
	if (Tier == ECatImpactTier::Ragdoll)
	{
		UE_LOG(LogCatVentures, Log,
			TEXT("[Impact] %s DOWNGRADE Ragdoll->Stagger reason=ragdoll-pending massXspeed=%.1f"),
			*Cat->GetName(), Params.ImpactorMassSpeed);
		Tier = ECatImpactTier::Stagger;
	}

	// ── A live stagger window swallows everything: re-staggers would reset the
	// window (a stunlock by repetition), and a flinch montage would cancel the stun
	// loop on the same slot. The stun IS the reaction while it runs.
	if (StaggerWindowRemaining > 0.0f)
	{
		UE_LOG(LogCatVentures, Log,
			TEXT("[Impact] %s REJECT reason=already-staggered source=%s massXspeed=%.1f"),
			*Cat->GetName(), ImpactSourceName(Params.Source), Params.ImpactorMassSpeed);
		return;
	}

	// ── Post-stagger immunity: stagger-class hits downgrade to Flinch (dropped,
	// never queued — the anti-stunlock floor). Step 4 note: Ragdoll-class impacts
	// will NOT respect this window.
	if (Tier == ECatImpactTier::Stagger && (Now - LastStaggerEndTime) < StaggerImmunityTime)
	{
		UE_LOG(LogCatVentures, Log,
			TEXT("[Impact] %s DOWNGRADE Stagger->Flinch reason=stagger-immunity sinceEnd=%.2fs (window %.2fs)"),
			*Cat->GetName(), Now - LastStaggerEndTime, StaggerImmunityTime);
		Tier = ECatImpactTier::Flinch;
	}

	// ── Flinch cooldown (flinch-tier results only — a stagger is never gated by it).
	if (Tier == ECatImpactTier::Flinch)
	{
		if (Now - LastFlinchTime < FlinchRetriggerCooldown)
		{
			UE_LOG(LogCatVentures, Log,
				TEXT("[Impact] %s REJECT reason=cooldown source=%s sinceLast=%.2fs (window %.2fs) instigator='%s'"),
				*Cat->GetName(), ImpactSourceName(Params.Source), Now - LastFlinchTime,
				FlinchRetriggerCooldown, *GetNameSafe(Params.InstigatorActor));
			return;
		}
		LastFlinchTime = Now;
	}

	const ECatImpactDirection Direction = ComputeHitDirection(Params.ImpactNormal);

	// ── Stagger launch: severity maps massXspeed across the stagger..ragdoll band
	// onto min..max knockback; direction is the push, flattened, plus the up kick.
	FVector LaunchVelocity = FVector::ZeroVector;
	if (Tier == ECatImpactTier::Stagger)
	{
		const float Denom = FMath::Max(RagdollImpactThreshold - StaggerImpactThreshold, 1.0f);
		const float Alpha = FMath::Clamp((Params.ImpactorMassSpeed - StaggerImpactThreshold) / Denom, 0.0f, 1.0f);
		const float Speed = FMath::Lerp(StaggerLaunchMinSpeed, StaggerLaunchMaxSpeed, Alpha);
		LaunchVelocity = Params.ImpactNormal.GetSafeNormal2D() * Speed + FVector(0, 0, StaggerLaunchUpSpeed);

		// Drops the held prop (ruled at review: yes). Server-side, BEFORE the reaction
		// multicast so every machine tears the grab down ahead of the launch — the
		// same direct-multicast shape UpdateGrab's drift release uses on authority.
		if (Cat->bIsGrabbing)
		{
			Cat->Multicast_ReleaseGrab();
		}
	}

	UE_LOG(LogCatVentures, Log,
		TEXT("[Impact] %s tier=%s dir=%s source=%s instigator='%s' massXspeed=%.1f impulse=%.1f launch=%.0f"),
		*Cat->GetName(), ImpactTierName(Tier), ImpactDirName(Direction), ImpactSourceName(Params.Source),
		*GetNameSafe(Params.InstigatorActor), Params.ImpactorMassSpeed, Params.NormalImpulseMag,
		LaunchVelocity.Size());

	// Value-carrying multicast — ACTION RPC, called unconditionally (the listen-server
	// host resolves it to Local and runs it in place; gating on !HasAuthority would
	// silently break it on the host — the TriggerGrab doctrine).
	Cat->Multicast_ImpactReaction(Tier, Direction, LaunchVelocity);
}

ECatImpactDirection UCatImpactResponseComponent::ComputeHitDirection(const FVector& ImpactNormal) const
{
	const ACatBase* Cat = GetCat();
	if (!Cat)
	{
		return ECatImpactDirection::Front;
	}

	// ImpactNormal is the push (instigator -> victim); the side the hit came FROM is
	// its reverse. Quantized to the dominant axis in the victim's frame — done ONCE,
	// here on the server, so every machine renders the same clip.
	const FVector HitFrom = -ImpactNormal.GetSafeNormal2D();
	const float FwdDot   = FVector::DotProduct(HitFrom, Cat->GetActorForwardVector());
	const float RightDot = FVector::DotProduct(HitFrom, Cat->GetActorRightVector());

	if (FMath::Abs(FwdDot) >= FMath::Abs(RightDot))
	{
		return (FwdDot >= 0.0f) ? ECatImpactDirection::Front : ECatImpactDirection::Back;
	}
	return (RightDot >= 0.0f) ? ECatImpactDirection::Right : ECatImpactDirection::Left;
}

// ══════════════════════════════════════════════════════════════════════════
// ── Per-machine execution ────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void UCatImpactResponseComponent::HandleImpactReactionMulticast(ECatImpactTier Tier, ECatImpactDirection Direction,
                                                                const FVector& LaunchVelocity)
{
	// Per-event telemetry on every machine (the subsystem tags host/client itself).
	if (PawPrint)
	{
		static const FName ChImpactTier(TEXT("ImpactTier"));
		PawPrint->SampleChannel(ChImpactTier, static_cast<float>(Tier));
	}

	switch (Tier)
	{
	case ECatImpactTier::Flinch:
		PlayFlinch(Direction);
		break;

	case ECatImpactTier::Stagger:
		StartStagger(LaunchVelocity);
		break;

	default:
		// Ragdoll arrives with step 4. The RPC shape already carries it.
		break;
	}
}

void UCatImpactResponseComponent::PlayFlinch(ECatImpactDirection Direction)
{
	ACatBase* Cat = GetCat();
	if (!Cat)
	{
		return;
	}

	TSoftObjectPtr<UAnimSequenceBase>* Soft = nullptr;
	switch (Direction)
	{
	case ECatImpactDirection::Front: Soft = &FlinchFrontSequence; break;
	case ECatImpactDirection::Back:  Soft = &FlinchBackSequence;  break;
	case ECatImpactDirection::Left:  Soft = &FlinchLeftSequence;  break;
	default:                         Soft = &FlinchRightSequence; break;
	}

	UAnimSequenceBase* Sequence = Soft->LoadSynchronous();
	if (!Sequence)
	{
		UE_LOG(LogCatVentures, Warning, TEXT("[Impact] %s flinch dir=%s has no sequence assigned — reaction skipped"),
			*Cat->GetName(), ImpactDirName(Direction));
		return;
	}

	UAnimInstance* AnimInst = Cat->GetMesh() ? Cat->GetMesh()->GetAnimInstance() : nullptr;
	if (!AnimInst)
	{
		return;
	}

	// Dynamic montage on DefaultSlot: no montage assets, no notifies, and the slot is
	// already spliced into ABP_Cat_V2's post-SM chain (it carries the swat montage).
	// A flinch landing mid-swat simply interrupts the swat montage on the same slot —
	// bIsSwatting clears through the existing FOnMontageEnded path, which is
	// interruption-safe by design (Docs/catbase.md).
	// KNOWN RISK, untestable solo (plan §3, v1.2): a full-body montage here overrides
	// the locomotion pose — a flinch mid-sprint may foot-skate. The pre-named fallback
	// is an upper-body/spine-up slot variant for the moving case.
	UAnimMontage* Montage = AnimInst->PlaySlotAnimationAsDynamicMontage(
		Sequence, TEXT("DefaultSlot"), FlinchBlendIn, FlinchBlendOut, FlinchPlayRate);
	ActiveFlinchMontage = Montage;
}

void UCatImpactResponseComponent::StartStagger(const FVector& LaunchVelocity)
{
	ACatBase* Cat = GetCat();
	if (!Cat)
	{
		return;
	}

	// ── The knock-off (ruled at review: yes — a launched prop knocking a rival off a
	// wall cling is the point). Each machine aborts ITS OWN copy of any traversal
	// takeover; the aborts restore the movement mode the launch then owns. Runs
	// before the launch, per the §7 precedence (Stagger > Traversal).
	if (Cat->Traversal)
	{
		Cat->Traversal->AbortAllTraversal();
	}

	// ── The launch — the wall-bounce shape: both planes OVERRIDDEN, and the gravity
	// interpolator SNAPPED to the rising baseline (the burned lesson: any mid-air
	// (re)launch otherwise fights a stale GravityScaleFalling for its whole first
	// beat). Every machine applies the same launch from the same multicast params.
	if (UCharacterMovementComponent* CMC = Cat->GetCharacterMovement())
	{
		Cat->GravityScaleInterp = Cat->GravityScaleRising;
		CMC->GravityScale       = Cat->GravityScaleRising;
	}
	Cat->LaunchCharacter(LaunchVelocity, /*bXYOverride=*/true, /*bZOverride=*/true);

	// ── Control-loss window + telemetry edge.
	StaggerWindowRemaining = StaggerControlLossTime;
	SetComponentTickEnabled(true);
	if (PawPrint)
	{
		static const FName ChStaggerSuppress(TEXT("StaggerSuppress"));
		PawPrint->SampleChannel(ChStaggerSuppress, 1.0f);
	}

	// ── Stun loop for the window (dynamic montage — replaces any live flinch on the
	// slot). Deliberately plays through the launch arc: dizzy tumbling IS the read.
	if (UAnimSequenceBase* Sequence = StunSequence.LoadSynchronous())
	{
		if (UAnimInstance* AnimInst = Cat->GetMesh() ? Cat->GetMesh()->GetAnimInstance() : nullptr)
		{
			ActiveStunMontage = AnimInst->PlaySlotAnimationAsDynamicMontage(
				Sequence, TEXT("DefaultSlot"), StunBlendIn, StunBlendOut, 1.0f, /*LoopCount=*/99);
		}
	}
}

void UCatImpactResponseComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                                FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (StaggerWindowRemaining > 0.0f)
	{
		StaggerWindowRemaining -= DeltaTime;
		if (StaggerWindowRemaining <= 0.0f)
		{
			EndStaggerWindow(/*bAbort=*/false);
		}
	}
	else
	{
		SetComponentTickEnabled(false);
	}
}

void UCatImpactResponseComponent::EndStaggerWindow(bool bAbort)
{
	ACatBase* Cat = GetCat();
	StaggerWindowRemaining = 0.0f;
	LastStaggerEndTime = GetWorld() ? GetWorld()->GetTimeSeconds() : LastStaggerEndTime;
	SetComponentTickEnabled(false);

	if (PawPrint)
	{
		static const FName ChStaggerSuppress(TEXT("StaggerSuppress"));
		PawPrint->SampleChannel(ChStaggerSuppress, 0.0f);
	}

	// Stop the stun loop from wherever it is (freeze-and-blend, the skid lesson —
	// never snap a scrubbed/looping pose back to zero on exit).
	if (ActiveStunMontage.IsValid() && Cat)
	{
		if (UAnimInstance* AnimInst = Cat->GetMesh() ? Cat->GetMesh()->GetAnimInstance() : nullptr)
		{
			if (AnimInst->Montage_IsPlaying(ActiveStunMontage.Get()))
			{
				AnimInst->Montage_Stop(bAbort ? 0.1f : StunBlendOut, ActiveStunMontage.Get());
			}
		}
	}
	ActiveStunMontage.Reset();

	// Gameplay end hands control back through the M4 input ramp so steering eases in
	// rather than snapping. The ABORT path must NOT touch it — InputRampTimer belongs
	// to the grounded systems, and the §2 restore contract forbids writing state the
	// other restores consult.
	if (!bAbort && Cat && Cat->IsLocallyControlled())
	{
		Cat->InputRampTimer = Cat->StartInputRampTime;
	}
}

// ══════════════════════════════════════════════════════════════════════════
// ── Restore contract ─────────────────────────────────────────────────────
// ══════════════════════════════════════════════════════════════════════════

void UCatImpactResponseComponent::AbortAllImpactReactions()
{
	// CONTRACT (plan §2, v1.2): impact-owned state ONLY. Nothing here may clear or
	// write a flag the traversal/grounded restores in RestoreAllCMCOverrides consult
	// — that chain's order-dependence is deliberate, and an early abort touching
	// bIsPivoting/traversal state would re-introduce the BB-16 stranded-override
	// class this function exists to prevent.
	if (StaggerWindowRemaining > 0.0f)
	{
		EndStaggerWindow(/*bAbort=*/true);
	}

	if (ActiveFlinchMontage.IsValid())
	{
		ACatBase* Cat = GetCat();
		UAnimInstance* AnimInst = (Cat && Cat->GetMesh()) ? Cat->GetMesh()->GetAnimInstance() : nullptr;
		if (AnimInst && AnimInst->Montage_IsPlaying(ActiveFlinchMontage.Get()))
		{
			AnimInst->Montage_Stop(FlinchBlendOut, ActiveFlinchMontage.Get());
		}
	}
	ActiveFlinchMontage.Reset();
}
