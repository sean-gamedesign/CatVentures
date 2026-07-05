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
class UBoxComponent;
class UPhysicsConstraintComponent;
class UGeometryCollectionComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnMeowDelegate);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSwatHitDelegate, AActor*, HitActor, FVector, HitLocation);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnJumpPhaseChanged, ECatJumpPhase, NewPhase);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnCatLanded, float, ImpactIntensity, float, AirTime);

/**
 * Base C++ Character for all Cat pawns.
 *
 * CONTROLS ARE FINAL: camera-relative movement (Move()) with orient-to-movement
 * rotation and the current tuning values are the shipped feel. Do not change
 * movement, camera, or input behavior without explicit designer sign-off.
 *
 * Multiplayer features:
 *  - Tick() runs on ALL roles: replicated state is derived on the server, while
 *    cosmetic animation variables are computed locally on every machine.
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

	// ── Movement Tuning ────────────────────────────────────────────
	// Applied to the CharacterMovementComponent in BeginPlay so per-instance
	// Blueprint overrides take effect. These values define the LOCKED control
	// feel — do not retune without designer sign-off.

	/** Max ground speed (cm/s). PrimeCatBase overrides this to 400 — the locked, designer-approved feel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning", meta = (ClampMin = "100.0", ClampMax = "2000.0"))
	float MovementMaxWalkSpeed = 600.0f;

	/** How fast the cat accelerates to max speed (cm/s²). Lower = heavier ramp-up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning", meta = (ClampMin = "50.0", ClampMax = "4000.0"))
	float MovementAcceleration = 500.0f;

	/** How fast the cat decelerates when input is released (cm/s²). Lower = longer slide. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning", meta = (ClampMin = "50.0", ClampMax = "4000.0"))
	float MovementBrakingDeceleration = 500.0f;

	/** Ground friction multiplier. Lower = more slide. Default CMC is 8.0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning", meta = (ClampMin = "0.0", ClampMax = "16.0"))
	float MovementGroundFriction = 5.0f;

	/** Friction applied while braking (separate from ground friction). Higher = stronger stop. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning", meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float MovementBrakingFriction = 0.5f;

	/** Yaw rotation rate (°/s) when moving. Lower = wider turning arcs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning", meta = (ClampMin = "60.0", ClampMax = "1080.0"))
	float MovementRotationRateYaw = 360.0f;

	// ── Jump Tuning ───────────────────────────────────────────────────

	/** Initial vertical launch velocity (cm/s). Wired to CMC->JumpZVelocity. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jump Tuning", meta = (ClampMin = "200.0", ClampMax = "1500.0"))
	float JumpLaunchVelocity = 700.0f;

	/** Gravity scale while ascending (Vz > ApexVelocityThreshold). LOCKED 2026-06-18. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jump Tuning", meta = (ClampMin = "1.0", ClampMax = "10.0"))
	float GravityScaleRising = 2.0f;

	/** Gravity scale near the peak (|Vz| <= ApexVelocityThreshold). Above GravityScaleRising
	 *  to push through the apex for a crisp, readable platforming peak. LOCKED 2026-06-18. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jump Tuning", meta = (ClampMin = "1.0", ClampMax = "10.0"))
	float GravityScaleApex = 3.4f;

	/** Gravity scale while falling (Vz < -ApexVelocityThreshold). The key "weight" knob. LOCKED 2026-06-18. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jump Tuning", meta = (ClampMin = "1.0", ClampMax = "10.0"))
	float GravityScaleFalling = 5.5f;

	/** How fast GravityScale ramps between phases — eliminates the Apex→Fall velocity spike.
	 *  Higher = snappier (less apex smear). Lower = slower ramp. Tune live in PIE. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jump Tuning", meta = (ClampMin = "1.0", ClampMax = "50.0"))
	float GravityScaleInterpSpeed = 25.0f;

	/** |Velocity.Z| (cm/s) below which the character is considered at the apex. Narrower = less loiter. LOCKED 2026-06-18. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jump Tuning", meta = (ClampMin = "10.0", ClampMax = "200.0"))
	float ApexVelocityThreshold = 30.0f;

	/** Air control while jumping. Higher = tighter mid-air steering for precision platforming. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jump Tuning", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float JumpAirControl = 0.7f;

	/** Max hold time (seconds) for variable-height jump. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jump Tuning", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float JumpMaxHoldTimeTuning = 0.18f;

	/** Coyote time (seconds): grace window to still jump just after walking off a ledge
	 *  (does NOT apply if the cat left the ground by jumping). 0 disables. LOCKED 2026-06-18. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jump Tuning", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float CoyoteTime = 0.12f;

	/** Jump buffer (seconds): a jump pressed this long before landing still fires on touchdown. 0 disables. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jump Tuning", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float JumpBufferTime = 0.15f;

	/** How long (seconds) the Land phase persists for the landing animation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jump Tuning", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float LandRecoveryDuration = 0.25f;

	/** |Velocity.Z| at impact that saturates LandImpactIntensity to 1.0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jump Tuning", meta = (ClampMin = "200.0", ClampMax = "2000.0"))
	float HardLandSpeedThreshold = 900.0f;

	/** Minimum seconds after Land phase before another jump is allowed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jump Tuning", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float JumpCooldown = 0.05f;

	/** Minimum seconds in Launch/Apex before the Fall phase is allowed to fire.
	 *  Kept short so Fall commits near the apex while vertical speed is still low —
	 *  otherwise the cat holds the apex pose through a fast descent and the fall
	 *  blendspace snaps when Fall finally fires. Reduced 0.30→0.05 on 2026-06-20
	 *  (jump anim pass, designer sign-off). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jump Tuning", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float MinFallTransitionHoldTime = 0.05f;

	/** Predictive landing (AnimX pass Step 2, kit "bGoLand" port): while falling, a trace runs
	 *  ahead along velocity; on a predicted ground hit the ANIM-facing phase (AnimJumpPhase)
	 *  enters Land before physical impact, so the land clip's crouch-prep plays pre-contact.
	 *  Lookahead is TIME-based (distance = |Velocity| × time) because our asymmetric-gravity
	 *  descent (GravityScaleFalling 5.5, impacts up to ~900 cm/s) is far faster than the kit's
	 *  −800 default — the kit's fixed 30/50 uu works out to roughly these windows at its speeds.
	 *  Moving lands (Land_run) want less anticipation than standstill lands (Land_stop, which
	 *  crouch-preps), mirroring the kit's 30 (moving) vs 50 (stopping). Header-only defaults. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jump Tuning", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float LandPredictTimeMoving = 0.08f;

	/** Standstill-landing lookahead window (seconds). See LandPredictTimeMoving. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jump Tuning", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float LandPredictTimeStopping = 0.12f;

	// ── Combat — The Swat ──────────────────────────────────────────────

	/** Impulse (kg·cm/s) applied to physics objects hit by the swat.
	 *  bVelChange = false, so heavier objects resist more.
	 *  Rule of thumb: value / object_mass_kg = launch speed in cm/s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat", meta = (ClampMin = "0.0"))
	float SwatImpulseForce = 5000.0f;

	/** Montage to play when the cat swats. Must contain an AnimNotifyState_SwatTrace on the active frames. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat")
	TObjectPtr<UAnimMontage> SwatMontage;

	// ── Turn-In-Place (procedural) ──────────────────────────────────────
	/** AimYaw (deg) beyond which an idle cat rotates in place to face the camera. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Turn In Place", meta = (ClampMin = "10.0", ClampMax = "120.0"))
	float TurnInPlaceThreshold = 50.0f;

	// ── Interaction ─────────────────────────────────────────────────────

	/** How far forward (cm) the interaction sphere trace reaches. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interaction", meta = (ClampMin = "50.0"))
	float InteractTraceLength = 200.0f;

	// ── Physics Bumper ───────────────────────────────────────────────────

	/** Forward-facing box that detects and pushes PhysicsBody objects before the capsule would.
	 *  Resize and reposition in the PrimeCatBase Blueprint viewport. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Physics Bumper")
	TObjectPtr<UBoxComponent> PhysicsBumper;

	/** Impulse magnitude (N·s) applied to physics objects on bumper contact.
	 *  bVelChange = false, so heavier objects move less. Start around 50000. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics Bumper", meta = (ClampMin = "0.0"))
	float BumperPushForce = 50000.0f;

	/** How far (cm) above the cat's foot level an object's top can be and still
	 *  be suppressed as 'underneath' when airborne. Only used as a fallback when
	 *  the CMC floor check doesn't match (i.e. the cat is not currently grounded
	 *  on that component). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics Bumper", meta = (ClampMin = "0.0", ClampMax = "60.0"))
	float UnderFootTolerance = 12.0f;

	// NOTE: Bumper contact with a Geometry Collection is a GUARANTEED full shatter
	// (ForceShatterGC) by design — there is no strain/threshold tuning on this path.
	// To make a prop harder to break, gate it on the BP side, not here.

	// ── Mouth Grab ───────────────────────────────────────────────────────

	/** Dynamically created physics constraint linking the mouth socket anchor to the
	 *  grabbed body. Created on grab, destroyed on release. nullptr when idle. */
	UPROPERTY()
	TObjectPtr<UPhysicsConstraintComponent> GrabConstraint;

	/** World-space follow point for the grab handle. Attached to socket_mouth so it
	 *  tracks the jaw automatically as the skeleton animates. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mouth Grab")
	TObjectPtr<USceneComponent> GrabTargetLocation;

	/** Radius (cm) of the mouth sphere trace used to detect grabbable objects. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mouth Grab", meta = (ClampMin = "5.0"))
	float GrabTraceRadius = 35.0f;

	/** Reach (cm) of the mouth sphere trace along the socket_mouth X-axis. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mouth Grab", meta = (ClampMin = "10.0"))
	float GrabTraceLength = 175.0f;

	/** Auto-release distance (cm). If the grabbed object's centre drifts further
	 *  than this from GrabTargetLocation, the grab is dropped. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mouth Grab", meta = (ClampMin = "50.0"))
	float MaxGrabDistance = 250.0f;

	/** Walk speed (cm/s) while a mouth grab is active. Simulates the effort of dragging weight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mouth Grab", meta = (ClampMin = "50.0", ClampMax = "400.0"))
	float DragWalkSpeed = 150.0f;

	/** Linear slack (cm) — how far the grabbed object can drift from the mouth anchor
	 *  before the constraint limits kick in. Lower = tighter tow cable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mouth Grab", meta = (ClampMin = "1.0", ClampMax = "200.0"))
	float GrabLinearLimit = 30.0f;

	/** Drive spring stiffness — how hard the constraint pulls the object toward the anchor.
	 *  Higher = snappier tracking. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mouth Grab", meta = (ClampMin = "100.0"))
	float GrabConstraintStiffness = 5000.0f;

	/** Drive damping — resists oscillation around the target. Higher = less bounce. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mouth Grab", meta = (ClampMin = "0.0"))
	float GrabConstraintDamping = 500.0f;

	/** Maximum force (N) the drive can exert. Caps the pull on very heavy objects. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mouth Grab", meta = (ClampMin = "0.0"))
	float GrabConstraintMaxForce = 100000.0f;

	/** Broadcast on authority when the swat hits a physics actor. */
	UPROPERTY(BlueprintAssignable, Category = "Combat")
	FOnSwatHitDelegate OnSwatHit;

	// ── Jump Delegates ─────────────────────────────────────────────────

	/** Fires on every jump phase transition — AnimBP binds to drive state machine. */
	UPROPERTY(BlueprintAssignable, Category = "Jump")
	FOnJumpPhaseChanged OnJumpPhaseChanged;

	/** Fires once on landing with impact data — AnimBP can scale landing animation weight. */
	UPROPERTY(BlueprintAssignable, Category = "Jump")
	FOnCatLanded OnCatLanded;

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

	//~ Begin ACharacter Interface
	virtual void OnJumped_Implementation() override;
	virtual void Landed(const FHitResult& Hit) override;
	virtual bool CanJumpInternal_Implementation() const override;
	//~ End ACharacter Interface

	// ── Tick Subsystems ────────────────────────────────────────────────

	/** Derives gameplay state (SpeedType, MovementStage, etc.) from the CharacterMovementComponent. Runs on ALL roles. */
	void UpdateAnimationStates();

	/** Applies asymmetric gravity scaling based on vertical velocity direction. Authority + autonomous proxy only. */
	void UpdateJumpGravity();

	/** Derives JumpPhase from CMC velocity and movement mode. Called from UpdateAnimationStates(). */
	void UpdateJumpPhase(float DeltaTime);

	/** Predictive landing trace (Fall phase only): sets bLandPredicted when ground lies within
	 *  |Velocity| × LandPredictTime along the fall path. Skipped on dedicated servers. */
	void UpdateLandPrediction();

	/** Integrates the landing-cushion spring and applies it to the mesh's relative Z.
	 *  Runs in the cosmetic tick block (non-dedicated only), BEFORE UpdateFootIK so the
	 *  dipped paw positions are what the foot-IK traces see (paws pushed into the ground
	 *  get snap-lifted the same frame -> legs compress). */
	void UpdateLandCushion(float DeltaTime);

	/** Landing-cushion spring state (cm, cm/s) — mesh rel-Z offset from MeshCushionBaseZ. */
	float MeshCushionOffset = 0.0f;
	float MeshCushionVelocity = 0.0f;

	/** The mesh's authored relative Z, captured in BeginPlay; the cushion offsets from this. */
	float MeshCushionBaseZ = 0.0f;

	/** Interpolates cosmetic-only variables (aim, breath, mesh offsets). Skipped on dedicated servers. */
	void UpdateCosmeticInterpolation(float DeltaTime);

	/** Ground-traces each paw and publishes the per-foot vertical offset the AnimBP adds to its VB
	 *  Hand/Foot goals before Leg IK. Cosmetic, local-only; skipped on dedicated servers. */
	void UpdateFootIK(float DeltaTime);

	// ── Enhanced Input Assets ────────────────────────────────────────────
public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputMappingContext> DefaultMappingContext;
protected:

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

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> GrabAction;

	// ── Input Handlers ──────────────────────────────────────────────────

	/** Camera-relative movement: input is projected onto the camera's yaw plane;
	 *  the character orients toward movement via CMC bOrientRotationToMovement.
	 *  This is the FINAL control scheme — do not change. */
	void Move(const FInputActionValue& Value);

	/** Processes IA_Look (Axis2D) — applies yaw/pitch to the controller rotation. */
	void Look(const FInputActionValue& Value);

	/** Fires on IA_Swat Started — local prediction + Server RPC. */
	void TriggerSwat();

	// ── Turn-In-Place ───────────────────────────────────────────────────
	/** When idle, procedurally rotates the body toward the camera and drives the BS1_Cat_Turn footwork. */
	void UpdateTurnInPlace();

	/** Fires on IA_Interact Started — server-authoritative trace. */
	void TriggerInteract();

	/** Fires on IA_Grab Started — initiates mouth grab. */
	void TriggerGrab();

	/** Fires on IA_Grab Completed — releases mouth grab. */
	void TriggerRelease();

	/** Fires on IA_Jump Started — records the jump-buffer timestamp, then forwards to ACharacter::Jump.
	 *  The buffer lets a press made just before landing still fire on touchdown (see Landed). */
	void OnJumpInputPressed();

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

	/** Client → Server: initiate a mouth grab at the current mouth socket position. */
	UFUNCTION(Server, Reliable)
	void Server_Grab();

	/** Client → Server: release the currently grabbed component. */
	UFUNCTION(Server, Reliable)
	void Server_ReleaseGrab();

	/** Server → owning client: the grab trace missed or failed validation.
	 *  Rolls back the client-side drag-settings prediction applied in TriggerGrab,
	 *  so a missed grab doesn't leave the player stuck at DragWalkSpeed. */
	UFUNCTION(Client, Reliable)
	void Client_GrabFailed();

	/** Server → All: create the physics constraint on every machine's local Chaos solver. */
	UFUNCTION(NetMulticast, Reliable)
	void Multicast_Grab(UPrimitiveComponent* GrabbedComp, FName BoneName);

	/** Server → All: destroy the constraint and re-enable strain on every machine. */
	UFUNCTION(NetMulticast, Reliable)
	void Multicast_ReleaseGrab();

	// ── Networked Physics Bumper (GC Fracture) ────────────────────────────

	/** Locally-controlled client → Server: validate a GC bumper hit and multicast the shatter. */
	UFUNCTION(Server, Reliable)
	void Server_BumperHitGC(AActor* GCActor, FVector Origin);

	/** Server → All: force-shatter the GC on every machine's local physics solver. */
	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_BumperHitGC(AActor* GCActor, FVector Origin);

	/** Deterministic GC fracture — wakes the Chaos solver and injects overwhelming strain
	 *  to guarantee immediate cluster-bond breakage. Call from Blueprints on high-speed
	 *  impact events. Hardcoded radius/strain values bypass the asset's Damage Threshold. */
	UFUNCTION(BlueprintCallable, Category = "Chaos")
	static void ForceShatterGC(UGeometryCollectionComponent* GCC, FVector HitLocation);

	// ── Networked Turn State ───────────────────────────────────────────

	/** Client → Server: edge-trigger for turn on/off. Reliable guarantees ordered delivery. */
	UFUNCTION(Server, Reliable)
	void Server_SetTurnActive(bool bNewGoTurn);

	/** Client → Server: continuous blendspace update during active turn. Unreliable is fine — loss just holds last value. */
	UFUNCTION(Server, Unreliable)
	void Server_SetTurnRate(float NewTurnRateAnim);

	// ══════════════════════════════════════════════════════════════════
	// ── Replicated Gameplay State (server-authoritative) ────────────────
	// ══════════════════════════════════════════════════════════════════

	/** Current locomotion speed tier. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, ReplicatedUsing = OnRep_SpeedType, Category = "Animation State")
	ECatMoveType SpeedType = ECatMoveType::Idle;

	/** High-level locomotion surface (ground, air, swimming, ragdoll). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, ReplicatedUsing = OnRep_MovementStage, Category = "Animation State")
	ECatMovementStage MovementStage = ECatMovementStage::OnGround;

	/** True while a mouth grab is active. Replicated so the AnimBP can drive a jaw-open blend on all machines. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, ReplicatedUsing = OnRep_bIsGrabbing, Category = "Animation State")
	bool bIsGrabbing = false;

	/** Current jump phase for AnimBP state machine transitions. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, ReplicatedUsing = OnRep_JumpPhase, Category = "Animation State")
	ECatJumpPhase JumpPhase = ECatJumpPhase::None;

	// ── OnRep Callbacks ─────────────────────────────────────────────────

	UFUNCTION()
	void OnRep_SpeedType();

	UFUNCTION()
	void OnRep_MovementStage();

	UFUNCTION()
	void OnRep_JumpPhase();

	UFUNCTION()
	void OnRep_bIsGrabbing();

	// ══════════════════════════════════════════════════════════════════
	// ── Local Cosmetic Variables (NOT replicated) ─────────────────────
	// ══════════════════════════════════════════════════════════════════
	// Computed locally on every machine (including simulated proxies).
	// Used by the Animation Blueprint for blendspaces and additive layers.

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float Speed = 0.0f;

	/** Source for PlayRateInterp. NOTE: currently never written (always 0) — the
	 *  ABP consumes PlayRateInterp, so populate this if anim play-rate scaling is wanted. */
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
	float AimYaw = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float AimYawInterp = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float AimYawClamped = 0.0f;

	/** Local-controller camera pitch relative to the horizon — drives the ABP head look-up/down. */
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

	/** Derived locally from CharacterMovement acceleration — NOT replicated. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	bool bHasMovementInput = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	bool bIsFalling = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	bool bIsOnGround = false;

	/** Normalized fall speed [0,1] — 0 = just started falling, 1 = terminal. Drives fall blendspace. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float NormalizedFallSpeed = 0.0f;

	/** Landing impact intensity [0,1] — driven by |Vz| at impact. Drives landing animation weight. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float LandImpactIntensity = 0.0f;

	/** Time spent in air (seconds). Accumulates while airborne, resets on land. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float JumpAirTime = 0.0f;

	/** Animation turn rate sent to the server while turning in place. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Animation|Cosmetic")
	float TurnRateAnim = 0.0f;

	/** True while the cat is performing a turn-in-place (|AimYaw| > 45 while idle on ground). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Animation|Cosmetic")
	bool bGoTurn = false;

	/** Procedural lean amount during locomotion (-1 = banking left, +1 = banking right). Drives Modify Bone Roll. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float LeanAmount = 0.0f;

	/** Longitudinal weight lean [-1.5, 1.5]. The body pitches into acceleration (+) and braces
	 *  back on deceleration (-). Spring-damped, so a hard stop overshoots past neutral and settles —
	 *  that overshoot is the felt "weight". Cosmetic, local-only. Wire to the free Roll pin of the
	 *  Spine_2 Modify Bone in ABP_Cat_V2 (scale to taste; sign flips forward/back). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float AccelLeanAmount = 0.0f;

	/** Reference acceleration (cm/s^2) mapping to full lean (±1). <= 0 falls back to CMC MaxAcceleration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning", meta = (ClampMin = "0.0"))
	float AccelLeanReference = 0.0f;

	/** Accel-lean spring stiffness — higher = snappier brace. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning", meta = (ClampMin = "1.0", ClampMax = "500.0"))
	float AccelLeanStiffness = 120.0f;

	/** Accel-lean spring damping — lower = more overshoot/bounce, higher = more settle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning", meta = (ClampMin = "0.0", ClampMax = "50.0"))
	float AccelLeanDamping = 14.0f;

	// ── Foot IK (quadruped ground placement) ────────────────────────────
	/** Master switch for quadruped foot-IK ground placement. RE-ENABLED 2026-07-05 at the
	 *  AnimX-kit chain spec. The 2026-06-22 warp was NOT "engine Leg IK is unfit" — it was our
	 *  chain definition: we solved the FRONT leg through the paw joint (IK goal VB Hand,
	 *  NumBonesInLimb=4) and the back leg one bone too deep (n=4 past the Thigh). The kit uses
	 *  the same engine Leg IK warp-free by never solving the front paw: front = VB Pastern goal,
	 *  FK Pastern, n=3 (Forearm/UpperArm/Shoulder), Hand stays FK; back = VB Foot goal, n=3
	 *  (Hook/Shin/Thigh); MaxIterations 15, MinRotationAngle 3–5°. ABP_Cat_V2 now matches that
	 *  spec (see Saved/.Aura/plans/quadruped-ik-port.md). FootIKAlpha drives the Leg IK Alpha
	 *  pins, so this=false -> alpha 0 -> both Leg IK nodes pass through (no solve).
	 *  Set via the C++ header default, NOT a BP override (the inherited-default-doesn't-
	 *  propagate gotcha). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning")
	bool bEnableFootIK = true;

	/** Draw the per-paw ground traces + hit points for tuning/debugging. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning")
	bool bFootIKDebugDraw = false;

	// ── Landing cushion — mesh-Z spring (AnimX pass Step 4) ─────────────
	/** Master switch for the landing cushion: a damped vertical spring on the mesh's relative Z.
	 *  Landed() kicks the spring down proportional to impact speed; the BODY dips and recovers
	 *  while the (upward-only) foot IK keeps the PAWS planted — so the legs visibly compress
	 *  and act like springs absorbing the landing weight, replacing authored crouch/splay
	 *  poses as the source of landing weight. Code port of the kit's HeightFixer physics rig
	 *  (migration doc §1.6: replace the simulated sphere+constraint with a code spring).
	 *  Cosmetic + local (every non-dedicated machine); never replicated. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning")
	bool bEnableLandCushion = true;

	/** Max body dip (cm) the cushion can produce (kit constraint limit was ±10). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning", meta = (ClampMin = "0.0", ClampMax = "20.0"))
	float LandCushionMaxDip = 10.0f;

	/** Dip depth per unit of impact speed (cm of dip per cm/s of |Vz|). 0.01 → a 900 cm/s
	 *  landing targets a 9 cm dip; a soft step-off barely registers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning", meta = (ClampMin = "0.0", ClampMax = "0.1"))
	float LandCushionDipPerImpact = 0.01f;

	/** Spring frequency (rad/s) — higher = snappier compress/recover. 14 ≈ peak dip ~0.07 s
	 *  after impact, settled in ~0.3 s (inside the land recovery window). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning", meta = (ClampMin = "4.0", ClampMax = "40.0"))
	float LandCushionFrequency = 14.0f;

	/** Damping ratio: 1 = no rebound; <1 = slight springy overshoot on recovery. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning", meta = (ClampMin = "0.3", ClampMax = "1.5"))
	float LandCushionDampingRatio = 0.85f;

	/** Ground-trace start height above each paw (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning", meta = (ClampMin = "0.0"))
	float FootIKTraceUpDistance = 25.0f;

	/** How far below each paw the trace reaches (cm) — also clamps how far a paw drops. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning", meta = (ClampMin = "0.0"))
	float FootIKTraceDownDistance = 45.0f;

	/** Vertical lift added to the trace hit so the paw rests on the surface (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning")
	float FootIKPawHeight = 2.0f;

	/** Interp speed for the alpha and the per-paw Z offsets — higher = snappier, lower = smoother. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning", meta = (ClampMin = "1.0"))
	float FootIKInterpSpeed = 15.0f;

	/** Per-paw vertical ground-conform offsets (cm, mesh component up). The AnimBP feeds each to a
	 *  Modify Bone (Add, component space, +Z) on the IK goal VB — front: VB Pastern (offset is
	 *  still MEASURED at the Hand paw; lifting the pastern goal lifts the FK paw rigidly with it),
	 *  back: VB Foot — then Leg IK solves. Additive to the stride, so it never pins the foot.
	 *  Cosmetic, local-only (computed on every non-dedicated machine for every pawn). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float FootIKOffsetZ_HandL = 0.0f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float FootIKOffsetZ_HandR = 0.0f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float FootIKOffsetZ_FootL = 0.0f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float FootIKOffsetZ_FootR = 0.0f;

	/** Master foot-IK blend [0..1]; eased to 0 while airborne so the legs free up for the jump. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float FootIKAlpha = 0.0f;

	/** True while falling and the lookahead trace predicts ground contact within the window
	 *  (see LandPredictTimeMoving/Stopping). Local + cosmetic — recomputed every frame on
	 *  every non-dedicated machine in UpdateJumpPhase; cleared when leaving the Fall phase. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	bool bLandPredicted = false;

	/** The ANIM-facing jump phase the ABP consumes (via Anim_JumpPhase): equals the replicated
	 *  gameplay JumpPhase except it anticipates Land while falling with a positive landing
	 *  prediction (bLandPredicted). Local + cosmetic, derived every frame — gameplay (recovery
	 *  timer, cooldown, gravity, foot-IK landing snap) stays anchored to the real JumpPhase. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	ECatJumpPhase AnimJumpPhase = ECatJumpPhase::None;

	/** True while the capsule is being procedurally rotated to commit a turn-in-place. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	bool bIsCommittingTurn = false;

	/** Last TurnRateAnim value sent via Server_SetTurnRate RPC. Used to throttle sends. */
	float LastSentTurnRateAnim = 0.0f;

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

	/** True while a procedural turn-in-place is engaged (hysteresis flag; see UpdateTurnInPlace). */
	bool bIsTurningInPlace = false;

	/** Fires when PhysicsBumper overlaps a PhysicsBody. Applies BumperPushForce on authority. */
	UFUNCTION()
	void OnBumperOverlapBegin(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
	    UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep,
	    const FHitResult& SweepResult);

	/** Performs the sphere trace and calls Interact on any hit IInteractableInterface actor. Authority only. */
	void PerformInteractTrace();

	/** Checks auto-release conditions (destroyed or drifted too far). Authority only. */
	void UpdateGrab(float DeltaTime);

	/** Sets CMC to drag-movement state: reduced MaxWalkSpeed, bOrientRotationToMovement disabled. */
	void ApplyDragMovementSettings();

	/** Restores CMC to normal state using MovementMaxWalkSpeed and bOrientRotationToMovement = true. */
	void RestoreNormalMovementSettings();

	// ── Jump State (per-instance) ──────────────────────────────────────

	/** Gates phase changes and broadcasts OnJumpPhaseChanged on actual transitions. */
	void SetJumpPhase(ECatJumpPhase NewPhase);

	/** Counts down from LandRecoveryDuration to zero during Land phase. */
	float LandRecoveryTimer = 0.0f;

	/** Cached launch Vz for NormalizedFallSpeed mapping. */
	float LaunchVelocityZ = 0.0f;

	/** Cooldown timer — counts down after Land->None before jump is re-allowed. */
	float JumpCooldownTimer = 0.0f;

	/** Countdown timer that gates SetJumpPhase(Fall). Starts at MinFallTransitionHoldTime
	 *  when Fall condition is first detected; Fall only fires when this reaches zero. */
	float FallTransitionHoldTimer = 0.0f;

	/** True when the Fall condition has been detected and we're waiting for FallTransitionHoldTimer. */
	bool bFallPending = false;

	/** Runtime-smoothed gravity scale — interpolates toward the target each tick to prevent
	 *  the Apex→Fall velocity spike. Not replicated; purely a physics-smoothing value. */
	float GravityScaleInterp = 2.8f;

	/** Counts down from CoyoteTime once the cat leaves the ground; while > 0 a ledge-walk jump
	 *  is still permitted (see CanJumpInternal). Reset to CoyoteTime every grounded frame. */
	float CoyoteTimer = 0.0f;

	/** True once the cat has left the ground by jumping — suppresses coyote-time for that airborne span. */
	bool bLeftGroundByJumping = false;

	/** Counts down from JumpBufferTime after a jump press; if still > 0 on Landed, the jump re-fires. */
	float JumpBufferTimer = 0.0f;

	// ── Mouth Grab State ────────────────────────────────────────────

	/** The physics component currently held. Valid only on authority while bIsGrabbing. */
	TWeakObjectPtr<UPrimitiveComponent> GrabbedComponent;

	/** Previous-frame horizontal speed, for the accel-lean velocity derivative. */
	float PreviousLeanSpeed = 0.0f;
	/** Spring velocity state for AccelLeanAmount. */
	float AccelLeanVelocity = 0.0f;
};
