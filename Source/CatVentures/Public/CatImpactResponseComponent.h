#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "CatImpactTypes.h"
#include "CatImpactResponseComponent.generated.h"

class ACatBase;
class UAnimMontage;
class UAnimSequenceBase;
class UPawPrintSubsystem;
class UPrimitiveComponent;

/**
 * Cat Impact Response — one system with severity tiers (flinch / stagger / ragdoll),
 * per Saved/.Aura/plans/cat-impact-response-v1.2.md. This component owns the
 * classifier, every reaction's enter/drive/exit, and the CMC apply/restore for its
 * takeovers — the ONE restore point for impact state (the traversal-component
 * precedent, per the CatBase growth-watch ruling).
 *
 * BUILT: Flinch (step 1) + Stagger (step 2). Ragdoll is schema-present, unbuilt —
 * measured impacts that classify Ragdoll are DOWNGRADED to Stagger (logged) until
 * step 4 lands.
 *
 * Networking: server-only classification, value-carrying multicast execution
 * (ACatBase::Multicast_ImpactReaction — the RPC carries tier/direction/launch so
 * no property-replication race exists; the fracture model: each machine plays its
 * own copy of the reaction).
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class CATVENTURES_API UCatImpactResponseComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCatImpactResponseComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

	// ── Server-side report entry points (one per source, all funnel to the classifier) ──

	/** Swat hit on this cat. Called directly from ACatBase::HandleSwatHit (server-only
	 *  by construction — the swat trace only runs on authority). Deliberately a direct
	 *  C++ call and NOT a damage-event binding: wiring a damage handler on the cat
	 *  would be the first step toward the health system this project does not build.
	 *  @param ImpulseDir  world-space push direction (attacker toward victim). */
	void ReportSwatImpact(ACatBase* Attacker, const FVector& ImpulseDir);

	// ── Per-machine execution (routed from ACatBase::Multicast_ImpactReaction) ──

	/** Play the classified reaction on THIS machine. Runs on every machine including
	 *  the victim's owner — nothing was predicted, so nobody skips. */
	void HandleImpactReactionMulticast(ECatImpactTier Tier, ECatImpactDirection Direction,
	                                   const FVector& LaunchVelocity);

	// ── State queries ──

	/** True while the stagger control-loss window is live — ACatBase::Move() consults
	 *  this FIRST in its suppression chain (impact outranks pivot/coil/traversal). */
	bool IsStaggerSuppressing() const { return StaggerWindowRemaining > 0.0f; }

	// ── Restore contract ──

	/** Abort every live impact reaction. Called FIRST inside
	 *  ACatBase::RestoreAllCMCOverrides (impact sits above traversal in precedence).
	 *  CONTRACT (plan §2, v1.2): this must not clear or write any flag the traversal
	 *  or grounded restores consult — it touches ONLY impact-owned state (which is
	 *  why the abort path does NOT arm the M4 input ramp the gameplay exit uses). */
	void AbortAllImpactReactions();

protected:
	virtual void BeginPlay() override;

	/** Capsule OnComponentHit — the MEASURED impact source (plan §4.1). Bound on
	 *  authority only; the capsule's bNotifyRigidBodyCollision is flipped on in
	 *  BeginPlay (the Pawn profile defaults it OFF, so a simulating prop striking
	 *  the capsule would otherwise report nothing). */
	UFUNCTION()
	void OnCapsuleHit(UPrimitiveComponent* HitComponent, AActor* OtherActor,
	                  UPrimitiveComponent* OtherComp, FVector NormalImpulse,
	                  const FHitResult& Hit);

	// ── Content (retargeted pack clips, /Game/Drafts/Impact — stubs by the M5
	//    doctrine). Soft refs with header-path defaults; EditAnywhere for swaps. ──

	UPROPERTY(EditAnywhere, Category = "Impact|Flinch")
	TSoftObjectPtr<UAnimSequenceBase> FlinchFrontSequence;

	UPROPERTY(EditAnywhere, Category = "Impact|Flinch")
	TSoftObjectPtr<UAnimSequenceBase> FlinchBackSequence;

	UPROPERTY(EditAnywhere, Category = "Impact|Flinch")
	TSoftObjectPtr<UAnimSequenceBase> FlinchLeftSequence;

	UPROPERTY(EditAnywhere, Category = "Impact|Flinch")
	TSoftObjectPtr<UAnimSequenceBase> FlinchRightSequence;

	/** Dizzy loop played (dynamic montage, DefaultSlot) through the stagger window —
	 *  including the launch arc, which is deliberate: a dizzy tumbling cat is the
	 *  comedy read. A grounded-only stun state is the iterate-round option. */
	UPROPERTY(EditAnywhere, Category = "Impact|Stagger")
	TSoftObjectPtr<UAnimSequenceBase> StunSequence;

	// ── Classifier thresholds (Measured source; units = impactor mass (kg) × contact
	//    speed (cm/s) — the deterministic quantity; NormalImpulse is logged alongside
	//    per the Heavy-tier verify-in-build pattern). VALIDATED from the step-2 PIE
	//    round (2026-09-12): a bracketed hurl sweep produced a clean split — every
	//    observed flinch fell in 3.3k–11.5k, every stagger in 15.7k–20.4k, so the two
	//    thresholds below bracket a real gap. NOTE: GC-fracture chunks fire NO impacts
	//    at all yet (the Phase-B capsule↔chunk collision defect), so these are anchored
	//    to controlled prop hurls, not romp data — re-tune once Phase B lands and real
	//    chunk hits reach the classifier. NormalImpulse and mass×speed agree at the
	//    band edges but can diverge on a hard-decelerating light prop (one hit logged
	//    massXspeed 11.5k with impulse 65k) — a Heavy-tier/hardening decision, not this
	//    round's. ──

	UPROPERTY(EditAnywhere, Category = "Impact|Tuning", meta = (ClampMin = "0.0"))
	float FlinchImpactThreshold = 3000.0f;   // validated: flinches observed 3.3k–11.5k

	UPROPERTY(EditAnywhere, Category = "Impact|Tuning", meta = (ClampMin = "0.0"))
	float StaggerImpactThreshold = 12000.0f; // validated: clean gap 11.5k ↔ 15.7k

	UPROPERTY(EditAnywhere, Category = "Impact|Tuning", meta = (ClampMin = "0.0"))
	float RagdollImpactThreshold = 60000.0f; // provisional: real tuning is step-4 (ragdoll) work

	// ── Flinch tuning ──

	/** Seconds between accepted flinches on one victim. A re-hit inside the window is
	 *  DROPPED (logged with reason), never queued — the anti-spam floor under the
	 *  locked no-stunlock rule. */
	UPROPERTY(EditAnywhere, Category = "Impact|Tuning", meta = (ClampMin = "0.0"))
	float FlinchRetriggerCooldown = 0.4f;

	/** Play rate for the flinch react (the Damage clips run 1.7-1.9 s authored — the
	 *  established stub dial if that reads sluggish, live-tunable). */
	UPROPERTY(EditAnywhere, Category = "Impact|Tuning", meta = (ClampMin = "0.1"))
	float FlinchPlayRate = 1.0f;

	// ── Stagger tuning ──

	/** Knockback speed at the stagger threshold / at the ragdoll threshold — severity
	 *  maps between them. Horizontal component; up is separate. */
	UPROPERTY(EditAnywhere, Category = "Impact|Tuning", meta = (ClampMin = "0.0"))
	float StaggerLaunchMinSpeed = 320.0f;

	UPROPERTY(EditAnywhere, Category = "Impact|Tuning", meta = (ClampMin = "0.0"))
	float StaggerLaunchMaxSpeed = 560.0f;

	/** Vertical kick on the knockback — unsticks the launch from ground friction
	 *  (the wall-bounce precedent: both planes overridden, not additive). */
	UPROPERTY(EditAnywhere, Category = "Impact|Tuning", meta = (ClampMin = "0.0"))
	float StaggerLaunchUpSpeed = 260.0f;

	/** Control-loss window (seconds). Input is suppressed while it runs, then ramps
	 *  back via the M4 envelope. */
	UPROPERTY(EditAnywhere, Category = "Impact|Tuning", meta = (ClampMin = "0.0"))
	float StaggerControlLossTime = 0.45f;

	/** Seconds after a stagger ENDS during which further stagger-class measured
	 *  impacts are downgraded to Flinch (never queued, never stacked — anti-stunlock).
	 *  Ragdoll-class impacts will NOT respect this once step 4 lands (a car must not
	 *  bounce off a recently-staggered cat). */
	UPROPERTY(EditAnywhere, Category = "Impact|Tuning", meta = (ClampMin = "0.0"))
	float StaggerImmunityTime = 1.5f;

private:
	/** The one classifier (plan §4): source caps -> tier, gates, direction quantization
	 *  — server only. Every meaningful report logs one [Impact] line with its inputs
	 *  and the verdict; a reaction that DIDN'T fire says why (the ETraversalReject
	 *  doctrine). */
	void ClassifyAndReact(const FCatImpactParams& Params);

	/** Quantize the hit direction into the victim's frame — which side it came FROM. */
	ECatImpactDirection ComputeHitDirection(const FVector& ImpactNormal) const;

	/** Per-machine flinch playback: dynamic montage on DefaultSlot. */
	void PlayFlinch(ECatImpactDirection Direction);

	/** Per-machine stagger: abort traversal (the knock-off), snap the gravity
	 *  interpolator, launch, start the control-loss window + stun loop. */
	void StartStagger(const FVector& LaunchVelocity);

	/** Close the control-loss window. Gameplay end arms the M4 ramp-back; the abort
	 *  path (bAbort) must not touch it (the §2 restore contract). */
	void EndStaggerWindow(bool bAbort);

	ACatBase* GetCat() const;

	/** Server timestamp of the last ACCEPTED flinch (cooldown basis). */
	float LastFlinchTime = -1000.0f;

	/** Timestamp the last stagger window closed (immunity basis). Per-machine, but
	 *  only the server's copy gates classification. */
	float LastStaggerEndTime = -1000.0f;

	/** Seconds left on the live control-loss window (0 = not staggered). */
	float StaggerWindowRemaining = 0.0f;

	/** Live dynamic montages on this machine (weak — the anim instance owns them). */
	TWeakObjectPtr<UAnimMontage> ActiveFlinchMontage;
	TWeakObjectPtr<UAnimMontage> ActiveStunMontage;

	/** PawPrint telemetry sink (null outside game/PIE worlds). Cached in BeginPlay. */
	UPROPERTY(Transient)
	TObjectPtr<UPawPrintSubsystem> PawPrint = nullptr;
};
