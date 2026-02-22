// CatBase.h — Multiplayer-ready Character base for CatVentures

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "CatAnimationTypes.h"
#include "CatBase.generated.h"

class UInputMappingContext;
class UInputAction;
struct FInputActionValue;
class USpringArmComponent;
class UCameraComponent;
class UAnimMontage;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnMeowDelegate);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSwatHitDelegate, AActor*, HitActor, FVector, HitLocation);

/**
 * Base C++ Character for all Cat pawns.
 *
 * Multiplayer features:
 *  - Hard Tick Gate: Tick() early-returns on non-locally-controlled instances
 *    so Blueprint tick logic only runs where it matters.
 *  - PossessedBy / OnRep_PlayerState force Walking movement mode immediately,
 *    preventing the "frozen client" problem.
 *  - Server_Meow RPC → NetMulticast_Meow → OnMeow broadcast for networked meowing.
 *  - The Swat: local-predicted montage with server-authoritative active-frame sweep.
 */
UCLASS()
class CATVENTURES_API ACatBase : public ACharacter
{
	GENERATED_BODY()

public:
	ACatBase();

	//~ Begin AActor Interface
	virtual void Tick(float DeltaTime) override;
	//~ End AActor Interface

	/** Registers replicated properties for the net driver. */
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Broadcast on all machines when this cat meows. */
	UPROPERTY(BlueprintAssignable, Category = "Cat")
	FOnMeowDelegate OnMeow;

	// ── Camera ─────────────────────────────────────────────────────────

	/** Spring arm that holds the follow camera behind the cat. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<USpringArmComponent> CameraBoom;

	/** Third-person follow camera. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<UCameraComponent> FollowCamera;

	// ── Camera Tuning ──────────────────────────────────────────────────

	/** Sensitivity multiplier applied to mouse/stick look input. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.1", ClampMax = "10.0"))
	float LookSensitivity = 1.0f;

	/** Pitch clamp (degrees) — how far the camera can look up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.0", ClampMax = "89.0"))
	float PitchClampUp = 60.0f;

	/** Pitch clamp (degrees) — how far the camera can look down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.0", ClampMax = "89.0"))
	float PitchClampDown = 70.0f;

	/** Enable positional camera lag on the spring arm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	bool bEnableCameraLag = true;

	/** Speed of positional camera lag (higher = snappier). Only used when bEnableCameraLag is true. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.0", EditCondition = "bEnableCameraLag"))
	float CameraLagSpeed = 10.0f;

	/** Enable rotational camera lag on the spring arm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	bool bEnableCameraRotationLag = true;

	/** Speed of rotational camera lag (higher = snappier). Only used when bEnableCameraRotationLag is true. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.0", EditCondition = "bEnableCameraRotationLag"))
	float CameraRotationLagSpeed = 8.0f;

	// ── Tank Controls ──────────────────────────────────────────────────

	/** Yaw turn speed in degrees/second when A/D are held. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta = (ClampMin = "30.0", ClampMax = "720.0"))
	float TurnRate = 180.0f;

	// ── Combat — The Swat ──────────────────────────────────────────────

	/** Impulse magnitude applied to physics objects hit by the swat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat", meta = (ClampMin = "0.0"))
	float SwatImpulseStrength = 800.0f;

	/** Montage to play when the cat swats. Must contain an AnimNotifyState_SwatTrace on the active frames. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat")
	TObjectPtr<UAnimMontage> SwatMontage;

	// ── Interaction ─────────────────────────────────────────────────────

	/** How far forward (cm) the interaction sphere trace reaches. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interaction", meta = (ClampMin = "50.0"))
	float InteractTraceLength = 200.0f;

	/** Broadcast on authority when the swat hits a physics actor. */
	UPROPERTY(BlueprintAssignable, Category = "Combat")
	FOnSwatHitDelegate OnSwatHit;

	// ── Swat Trace Interface (called by UAnimNotifyState_SwatTrace) ──

	/** Called by NotifyBegin — caches initial paw position and clears hit set (authority only). */
	void BeginSwatTrace(USkeletalMeshComponent* MeshComp, FName SocketName);

	/** Called by NotifyTick — performs sphere sweep from previous to current paw position (authority only). */
	void ProcessSwatTraceTick(USkeletalMeshComponent* MeshComp, FName SocketName, float SweepRadius, float DeltaTime);

	/** Called by NotifyEnd — clears the hit set. Does NOT reset bIsSwatting (that's handled by OnSwatMontageEnded). */
	void EndSwatTrace();

protected:
	//~ Begin AActor Interface
	virtual void BeginPlay() override;
	//~ End AActor Interface

	//~ Begin APawn Interface
	virtual void PossessedBy(AController* NewController) override;
	virtual void OnRep_PlayerState() override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
	//~ End APawn Interface

	// ── Tick Subsystems ────────────────────────────────────────────────

	/** Derives gameplay state (SpeedType, MovementStage, etc.) from the CharacterMovementComponent. Runs on ALL roles. */
	void UpdateAnimationStates();

	/** Interpolates cosmetic-only variables (aim, breath, mesh offsets). Skipped on dedicated servers. */
	void UpdateCosmeticInterpolation(float DeltaTime);

	// ── Enhanced Input Assets ────────────────────────────────────────────

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputMappingContext> DefaultMappingContext;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> MoveAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> MeowAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> LookAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> JumpAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> SwatAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> InteractAction;

	// ── Input Handlers ──────────────────────────────────────────────────

	/** Tank-style input: Y axis (W/S) moves along ActorForward, X axis (A/D) yaw-rotates the character. */
	void Move(const FInputActionValue& Value);

	/** Processes IA_Look (Axis2D) — applies yaw/pitch to the controller rotation. */
	void Look(const FInputActionValue& Value);

	/** Fires on IA_Swat Started — local prediction + Server RPC. */
	void TriggerSwat();

	/** Fires on IA_Interact Started — server-authoritative trace. */
	void TriggerInteract();

	// ── Networked Meow ──────────────────────────────────────────────────

	/** Client → Server: request a meow. */
	UFUNCTION(Server, Reliable)
	void Server_Meow();

	/** Server → All: replicate the meow to every machine. */
	UFUNCTION(NetMulticast, Reliable)
	void NetMulticast_Meow();

	// ── Networked Swat ─────────────────────────────────────────────────

	/** Client → Server: request a swat. */
	UFUNCTION(Server, Reliable)
	void Server_Swat();

	/** Server → All: play swat montage on all machines (instigator skips — already predicted). */
	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_Swat();

	// ── Networked Interact ──────────────────────────────────────────────

	/** Client → Server: request an interaction trace. */
	UFUNCTION(Server, Reliable)
	void Server_Interact();

	// ══════════════════════════════════════════════════════════════════
	// ── Replicated Gameplay State (server-authoritative) ────────────────
	// ══════════════════════════════════════════════════════════════════

	/** Current locomotion speed tier. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, ReplicatedUsing = OnRep_SpeedType, Category = "Animation State")
	ECatMoveType SpeedType = ECatMoveType::Idle;

	/** Current special action the cat is performing. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, ReplicatedUsing = OnRep_CurrentAction, Category = "Animation State")
	ECatAction CurrentAction = ECatAction::None;

	/** Camera/input control scheme. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, ReplicatedUsing = OnRep_ControlMode, Category = "Animation State")
	ECatControlMode ControlMode = ECatControlMode::Looking;

	/** High-level locomotion surface (ground, air, swimming, ragdoll). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, ReplicatedUsing = OnRep_MovementStage, Category = "Animation State")
	ECatMovementStage MovementStage = ECatMovementStage::OnGround;

	/** Head/body aim mode for look-at blendspaces. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, ReplicatedUsing = OnRep_AimMode, Category = "Animation State")
	ECatAim AimMode = ECatAim::Aim;

	/** Which blendspace set the AnimBP should use. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, ReplicatedUsing = OnRep_AnimBSMode, Category = "Animation State")
	ECatAnimBSMode AnimBSMode = ECatAnimBSMode::Looking;

	/** Priority action override (attack, damage, death, etc.). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, ReplicatedUsing = OnRep_BaseAction, Category = "Animation State")
	ECatBaseAction BaseAction = ECatBaseAction::None;

	/** Idle rest progression state. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, ReplicatedUsing = OnRep_RestState, Category = "Animation State")
	ECatRest RestState = ECatRest::None;

	/** True while the cat is in crouch mode. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, ReplicatedUsing = OnRep_bCrouchMode, Category = "Animation State")
	bool bCrouchMode = false;

	/** True when the cat has died. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, ReplicatedUsing = OnRep_bDied, Category = "Animation State")
	bool bDied = false;

	// ── OnRep Callbacks ─────────────────────────────────────────────────

	UFUNCTION()
	void OnRep_SpeedType();

	UFUNCTION()
	void OnRep_CurrentAction();

	UFUNCTION()
	void OnRep_ControlMode();

	UFUNCTION()
	void OnRep_MovementStage();

	UFUNCTION()
	void OnRep_AimMode();

	UFUNCTION()
	void OnRep_AnimBSMode();

	UFUNCTION()
	void OnRep_BaseAction();

	UFUNCTION()
	void OnRep_RestState();

	UFUNCTION()
	void OnRep_bCrouchMode();

	UFUNCTION()
	void OnRep_bDied();

	// ══════════════════════════════════════════════════════════════════
	// ── Local Cosmetic Variables (NOT replicated) ─────────────────────
	// ══════════════════════════════════════════════════════════════════
	// Computed locally on every machine (including simulated proxies).
	// Used by the Animation Blueprint for blendspaces and additive layers.

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float Speed = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float SpeedDelay = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float PlayRate = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float PlayRateInterp = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float AlphaPlayBreath = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float AlphaPlayBreathInterp = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float TimeInRun = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float TimeInRunCache = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float AimYaw = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float AimYawInterp = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float AimYawClamped = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float AimPitch = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float AimPitchInterp = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float AimPitchClamped = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float AlphaAim = 1.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float AlphaAimInterp = 1.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float AlphaLookAt = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float LeanDrink = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float LeanDrinkClamp = 1.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float FixedLocationMesh = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float FixedLocationCamera = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float FixedLocationSwim = 0.0f;

	/** Derived locally from CharacterMovement acceleration — NOT replicated. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	bool bHasMovementInput = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	bool bIsFalling = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	bool bIsOnGround = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	bool bBackwards = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float SpeedMultiplierFinale = 0.75f;

	/** Animation turn rate (renamed from BP "Turn Rate" to avoid collision with TurnRate). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float TurnRateAnim = 0.0f;

	/** True while the cat is performing a turn-in-place (|AimYaw| > 45 while idle on ground). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	bool bGoTurn = false;

	/** Procedural lean amount during locomotion (-1 = banking left, +1 = banking right). Drives Modify Bone Roll. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float LeanAmount = 0.0f;

	/** True while the capsule is being procedurally rotated to commit a turn-in-place. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	bool bIsCommittingTurn = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float PlayerDontMoveFor = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float DeltaTimeCached = 0.0f;

private:
	/** Forces the CharacterMovementComponent into Walking mode if it is currently None. */
	void ForceWalkingMovementMode();

	// ── Swat State (per-instance — CDO-safe) ───────────────────────────

	/** Paw socket location from the previous tick (for sweep start point). */
	FVector SwatPreviousPawLocation = FVector::ZeroVector;

	/** Actors already hit during this swat (prevents double-hits in one swipe). */
	TSet<TWeakObjectPtr<AActor>> SwatAlreadyHitActors;

	/** True while a swat montage is playing — blocks re-entry. */
	bool bIsSwatting = false;

	/** Server-authoritative hit processing: applies impulse + broadcasts OnSwatHit. */
	void HandleSwatHit(const FHitResult& HitResult);

	/** Shared helper: plays the swat montage and binds FOnMontageEnded for interruption-safe cleanup. */
	void PlaySwatMontageAndBindEnd();

	/** Montage end callback — fires on both natural completion and interruption. Resets bIsSwatting. */
	UFUNCTION()
	void OnSwatMontageEnded(UAnimMontage* Montage, bool bInterrupted);

	/** Performs the sphere trace and calls Interact on any hit IInteractableInterface actor. Authority only. */
	void PerformInteractTrace();

	// ── Turn Commitment & Lean ──────────────────────────────────────
	FRotator TargetTurnRotation = FRotator::ZeroRotator;
	float PreviousYaw = 0.0f;
};
