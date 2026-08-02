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
class UPawPrintSubsystem;

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

	// The traversal component is an extension of the character (owns traversal verbs'
	// movement takeovers) — it reaches the mantle mirror RPC and anim-state setters.
	friend class UCatTraversalComponent;

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

	// ── Camera Weight (M3, weighty-movement pass) ──────────────────────
	// Per-gait spring-arm lag, landing dip, sprint FOV push, stop-settle.
	// Local player's camera only — purely cosmetic, zero replication surface.
	// Header-only defaults (no PrimeCatBase mirrors); all live-tunable in PIE.

	/** Master switch for the M3 camera-weight layer. Disabling mid-play resets the
	 *  dip but leaves lag/FOV at their last blended values until re-set. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Weight")
	bool bEnableCameraWeight = true;

	/** Positional lag speed at full sprint speed (blends from CameraLagSpeed by actual
	 *  speed). Lower than the base = the camera trails at sprint — the speed read. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Weight", meta = (ClampMin = "0.0"))
	float SprintCameraLagSpeed = 6.0f;

	/** Rotational lag speed at full sprint speed (blends from CameraRotationLagSpeed).
	 *  Lower = the camera swings wide on sprint turns. NOTE: the AnimX kit goes the
	 *  OTHER direction (snappier at run, 5.0 vs 3.0 walk) — if trailing reads wrong
	 *  in PIE, flip this above the base value instead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Weight", meta = (ClampMin = "0.0"))
	float SprintCameraRotationLagSpeed = 7.0f;

	/** Max camera dip (cm) on a saturated hard landing (scaled by LandImpactIntensity —
	 *  which saturates fast at GravityScaleFalling 5.5: most real jumps land near 1.0). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Weight", meta = (ClampMin = "0.0", ClampMax = "50.0"))
	float CameraLandDipMax = 8.0f;

	/** Camera dip spring angular frequency (rad/s). Higher = faster dip-and-recover. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Weight", meta = (ClampMin = "1.0", ClampMax = "40.0"))
	float CameraDipFrequency = 9.0f;

	/** Camera dip spring damping ratio (≥1 = no overshoot past rest). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Weight", meta = (ClampMin = "0.1", ClampMax = "2.0"))
	float CameraDipDampingRatio = 0.9f;

	/** FOV push (degrees) at full sprint speed, eased by actual speed. 0 disables.
	 *  (6 read as too much on the 2026-07-11 first pass — halved.) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Weight", meta = (ClampMin = "0.0", ClampMax = "30.0"))
	float SprintFOVPush = 3.0f;

	/** Interp speed for the sprint FOV push. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Weight", meta = (ClampMin = "0.5", ClampMax = "20.0"))
	float CameraFOVInterpSpeed = 5.0f;

	/** Camera dip (cm) kicked when a sprint comes to a stop, scaled by how fast the
	 *  run was (a trot stop is silent — only sprint-speed stops settle). 0 disables. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Weight", meta = (ClampMin = "0.0", ClampMax = "20.0"))
	float StopSettleDip = 4.0f;

	/** Interp speed for the gait blend RELAXING back to base (lag speeds + FOV target)
	 *  after slowing down; the rise tracks raw speed directly. Lower = gentler camera
	 *  catch-up when a sprint stops (2026-07-14: instantaneous release read as a lurch). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Weight", meta = (ClampMin = "0.2", ClampMax = "20.0"))
	float CameraGaitRelaxSpeed = 2.0f;

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

	/** Lateral (horizontal) friction applied while airborne. UE's default is 0; this ships 3.0,
	 *  which decays horizontal air speed with a ~0.33 s time constant. Was a bare constructor
	 *  write until 2026-07-24 — exposed (and added to the BeginPlay re-apply list) when the
	 *  wall bounce showed it eating two thirds of the kick inside 0.2 s. The wall bounce
	 *  temporarily overrides it during its rebound window; this is the value restored after. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning", meta = (ClampMin = "0.0", ClampMax = "8.0"))
	float MovementFallingLateralFriction = 3.0f;

	// ── Sprint Gait (M1, weighty-movement pass) ────────────────────────
	// W alone = trot at MovementMaxWalkSpeed; W+Shift = sprint. The weight comes
	// from differentiation: the sprint turns wider and reads as a distinct gait.

	/** Max ground speed while sprinting (cm/s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Sprint", meta = (ClampMin = "100.0", ClampMax = "2000.0"))
	float SprintMaxWalkSpeed = 650.0f;

	/** Yaw rotation rate (°/s) while sprinting. Lower than the trot rate = wider arcs at speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Sprint", meta = (ClampMin = "60.0", ClampMax = "1080.0"))
	float SprintRotationRateYaw = 150.0f;

	// ── Moving Pivot (M2 part 2, weighty-movement pass) ────────────────
	// A hard sustained steer at speed (input far off the velocity direction) plants
	// the cat and turns it with BS1_Cat_Turn footwork instead of letting the
	// orient-to-movement arc swing the body through. Header-only defaults,
	// live-tunable; the plant itself is cosmetic but the input suppression and
	// braking boost are movement-affecting (braking mirrored to the server).

	/** Master switch for the moving pivot (plant-and-turn on a hard steer). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Pivot")
	bool bEnableMovingPivot = true;

	/** Scale on the pivot crouch drop — the Blender-authored Pivot90 clips carry their crouch
	 *  in Spine translation, which the skeleton's all-SKELETON translation retargeting discards
	 *  at evaluation (measured 2026-07-22: hips pinned at ref 30.17 for every clip). The drop
	 *  curve sampled from the authored hip restores it procedurally while bGoPivot, composed
	 *  into the same §B2 transform write as TurnStanceDrop. The clip legs were IK-solved
	 *  against the crouched hip, so drop + rotation-only pose = the authored pose. 0 disables. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Pivot", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float PivotCrouchScale = 1.0f;

	/** Cosmetic mesh drop (cm) while SpeedType == Turn (turn-in-place AND pivot footwork).
	 *  The kit turn clips' OUTSIDE forepaw never reaches contact (min hover 3.5–4.3 vs ~2
	 *  planted stance, measured 2026-07-20) — easing the body down trades that visible
	 *  hover for slight stance-paw penetration, which doesn't read. 0 disables. Live-tunable.
	 *  3.5 = Sean-approved live-tune 2026-07-20 (1.8 left the outside paw hovering; known
	 *  accepted residual: back paws clip the ground a touch, minor rise on close inspection —
	 *  candidate polish = re-enable upward-only foot IK during Turn to lift the clippers). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Turn", meta = (ClampMin = "0.0", ClampMax = "6.0"))
	float TurnStanceDrop = 3.5f;

	/** Engage interp speed for the turn stance drop — must beat the ~0.2 s Turn-state blend-in
	 *  or the first footwork steps play before the body is down (2026-07-20 PIE read).
	 *  Release always eases out at a gentle 8. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Turn", meta = (ClampMin = "5.0", ClampMax = "60.0"))
	float TurnStanceDropEngageSpeed = 22.0f;

	/** SWEEP-tier steering angle (deg, velocity vs input): at or beyond this, a pivot arms IF the
	 *  input direction has also rotated ≥ PivotSweepMinDeg during the sustain window. Velocity
	 *  CHASES the input at the CMC rotation rate, so camera sweeps live in the 40–65° band
	 *  (2026-07-18 diag rounds) — but ordinary 90° direction taps pass through that same band
	 *  for ~0.2 s, so angle alone can't discriminate; the input-rotation requirement does
	 *  (a sweep keeps rotating, a tap's input is constant after the flip). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Pivot", meta = (ClampMin = "30.0", ClampMax = "180.0"))
	float PivotAngleThreshold = 55.0f;

	/** FLIP-tier steering angle (deg): at or beyond this the pivot arms with NO input-rotation
	 *  requirement — a single hard reversal (S-flick 180) has a constant input direction and
	 *  must not be filtered by the sweep requirement. Ordinary 90° taps never reach it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Pivot", meta = (ClampMin = "90.0", ClampMax = "180.0"))
	float PivotFlipAngle = 110.0f;

	/** Minimum input-direction rotation (deg, accumulated over the sustain window) for the
	 *  sweep tier to fire. Filters direction taps (≈0°) from deliberate camera sweeps — a sweep
	 *  that can hold the steer angle up must out-rotate the velocity chase, giving ≥ ~18°/0.12 s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Pivot", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float PivotSweepMinDeg = 15.0f;

	/** Minimum ground speed (cm/s) to ARM a pivot — checked only on the first over-threshold
	 *  frame. Deliberately NOT enforced while the sustain timer runs: a hard steer brakes the
	 *  cat, so a continuous check voided the trigger right as the angle peaked (diag round). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Pivot", meta = (ClampMin = "0.0", ClampMax = "1000.0"))
	float PivotMinSpeed = 240.0f;

	/** Seconds the steering angle must stay past the threshold before the pivot fires. Filters
	 *  ordinary 90° direction taps (their angle decays below the threshold in ~0.1 s at the
	 *  trot rotation rate) while a genuinely held hard steer survives it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Pivot", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float PivotSustainTime = 0.12f;

	/** Body rotation rate (deg/s) during the pivot turn — the master weight-vs-response knob.
	 *  The scrub ties footfall cadence to this directly: playback speed ≈ rate ÷ ~82 (the
	 *  Pivot90/180 clips' authored turning rate). 200 played every footfall at ~2.4× — the
	 *  "tippy-tap" 2026-07-22 PIE read; ~120 ≈ 1.45×; ~80 ≈ authored 1× (90° in ~0.95 s,
	 *  180° in ~2 s). Live-tunable; mouse flick speed never raises it — the body turns at
	 *  this rate no matter how hard the camera whips. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Pivot", meta = (ClampMin = "60.0", ClampMax = "720.0"))
	float PivotTurnSpeedDegPerSec = 120.0f;

	/** Scale on the pivot's footwork blend param (TurnRateAnim). The A_Cat_Move_Turn clips
	 *  are the kit's MOVING-turn clips — stride-elevated footwork through their WHOLE range
	 *  (2026-07-18 paw sampler: paws hover 3–7 cm and never fully plant at any sustained
	 *  blend position; ground contact −22.5, "planted" paw ≥ −20). There is no cap value
	 *  that reads planted — lower slides, higher paddles; 0.4 is the accepted interim.
	 *  Real fix = a dedicated CtrlRig-authored plant-pivot clip (M5 asset batch). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Pivot", meta = (ClampMin = "0.1", ClampMax = "1.0"))
	float PivotFootworkCap = 0.4f;

	/** Remaining angle (deg) to the input direction at which the pivot releases and input flows again. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Pivot", meta = (ClampMin = "2.0", ClampMax = "60.0"))
	float PivotExitAngle = 15.0f;

	/** Seconds after a pivot before another may arm (stops oscillating steers from chaining plants). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Pivot", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float PivotCooldown = 0.3f;

	/** Braking deceleration (cm/s²) while planted — the speed kill. Restored to
	 *  MovementBrakingDeceleration on exit; mirrored to the server so move replay agrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Pivot", meta = (ClampMin = "500.0", ClampMax = "8000.0"))
	float PivotBrakingDeceleration = 2000.0f;

	/** Plant dip (cm) kicked into the landing-cushion spring on pivot entry, scaled by entry speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Pivot", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float PivotPlantDip = 5.0f;

	// ── Weighty Stop (M2 part 2b, weighty-movement pass) ───────────────
	// Releasing input at speed swaps the CMC's exponential friction braking
	// (GroundFriction-dominated — a sprint halts in ~52 cm / 0.26 s) for a
	// CONSTANT deceleration, so stop distance scales with v²: the sprint run-out
	// carries ~3× a trot's from the single deceleration knob. When the speed
	// collapses (the plant) the landing-cushion spring takes an entry-speed-scaled
	// kick and a short re-acceleration ramp keeps a fresh press from instantly
	// cancelling the halt. The braking swap is movement-affecting (mirrored to the
	// server, pivot pattern); the plant dip and ramp are local cosmetics.

	/** Master switch for weighty stops (constant-deceleration run-out + plant on input release). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Stop")
	bool bEnableWeightyStops = true;

	/** Minimum ground speed (cm/s) at input release to start a run-out. Below it (the walk
	 *  band) the ordinary friction stop already reads fine. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Stop", meta = (ClampMin = "0.0", ClampMax = "1000.0"))
	float StopMinSpeed = 300.0f;

	/** Constant braking deceleration (cm/s²) during the run-out — THE stop-distance knob.
	 *  Distance = v²/2a: at 1000, sprint 650 ≈ 209 cm / 0.57 s, trot 400 ≈ 77 cm / 0.32 s.
	 *  (First pass shipped 1400 — the sprint glide was too short to register on screen;
	 *  1000 Sean-approved 2026-07-19, PIE-measured 170 cm / 0.52 s from a 592 entry.) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Stop", meta = (ClampMin = "400.0", ClampMax = "4000.0"))
	float StopBrakingDeceleration = 1000.0f;

	/** Braking friction during the run-out (applied via bUseSeparateBrakingFriction). Near-zero
	 *  keeps the deceleration constant so the v² distance scaling actually holds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Stop", meta = (ClampMin = "0.0", ClampMax = "8.0"))
	float StopBrakingFriction = 0.0f;

	/** Speed (cm/s) at which the run-out counts as planted — fires the cushion kick + re-accel ramp. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Stop", meta = (ClampMin = "0.0", ClampMax = "200.0"))
	float StopPlantSpeed = 80.0f;

	/** Minimum entry speed (cm/s) for a run-out to play the skid-stop CLIP (Skid state) instead
	 *  of the plain gait run-out. Sprint-band only by default — trot stops staying visually
	 *  ordinary is a design decision (differentiation lives at sprint). Live-tunable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Stop", meta = (ClampMin = "300.0", ClampMax = "700.0"))
	float SkidMinEntrySpeed = 500.0f;

	/** Scale on the skid crouch drop — same doctrine as PivotCrouchScale: the skid clip's brace
	 *  crouch lives in Spine translation, which the skeleton's all-SKELETON translation
	 *  retargeting discards; the §B2 envelope (rise→hold→release, peak 7.8 cm) restores it
	 *  synced to SkidProgress. The clip legs were IK-solved against the crouched hip. 0 disables. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Stop", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float SkidCrouchScale = 1.0f;

	/** Plant dip (cm) kicked into the landing-cushion spring, scaled by the stop's entry speed.
	 *  SHIPS 0 (2026-07-19 isolation probe): the kick read as a funky spine pop at the halt —
	 *  at 10 it slammed the spring's clamp, and even moderate values fight the accel-lean
	 *  release overshoot, which already gives the stop its rear-up settle for free. Keep the
	 *  knob: a skid clip (M5) may want a small dip composed under its contact frames. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Stop", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float StopPlantDip = 0.0f;

	/** Post-plant re-acceleration ramp (s): Move input scales 0→1 over this window so a fresh
	 *  press eases back in instead of instantly cancelling the halt. Scaled by entry speed
	 *  (a trot plant's ramp is shorter than a sprint plant's); 0 disables. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Stop", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float StopReaccelDelay = 0.18f;

	// ── Weighty Start (M2 part 2c, weighty-movement pass) ──────────────
	// The launch-step into a sprint: a fresh sprint start from near-standstill
	// COILS for a beat (input suppressed — the jump-anticipation doctrine: a real
	// input-to-launch delay, standstill-only so trot starts stay instant for
	// platforming), then BURSTS out with boosted acceleration until it punches
	// through the trot band; normal acceleration earns the sprint top end. The
	// accel-lean spring rails forward through the burst (the lunge pose, free) and
	// the coil takes a small cushion dip (the load). Triggers are EDGE-based —
	// fresh-input edge while sprinting, or sprint-engage edge while input is fresh
	// — so sprinting into a wall (speed collapses, input held) can never re-coil.
	// The accel boost is movement-affecting (mirrored to the server, braking RPC
	// pattern); the coil suppression is owner input shaping, no replication.
	// Also home of the M4 input ramp: fresh input from idle eases 0→1 over
	// StartInputRampTime so keyboard taps don't twitch.

	/** Master switch for the weighty sprint start (coil + acceleration burst). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Start")
	bool bEnableWeightyStarts = true;

	/** Seconds the coil holds (input suppressed, body loading) before the burst releases.
	 *  0 disables the coil but keeps the burst. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Start", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float StartCoilTime = 0.12f;

	/** Max ground speed (cm/s) counting as "standstill" for a coil — above it the cat is
	 *  already moving and a sprint engage stays instant (moving launch-step = M5 asset). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Start", meta = (ClampMin = "0.0", ClampMax = "300.0"))
	float StartCoilMaxSpeed = 100.0f;

	/** Coil dip (cm) kicked into the landing-cushion spring as the coil loads. Deliberately
	 *  modest (the stop's plant-dip lesson: big kicks at pose-transition moments read as pops);
	 *  isolation-probe live and zero it if it fights the burst's accel-lean lunge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Start", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float StartCoilDip = 4.0f;

	/** MaxAcceleration multiplier during the burst — the spring out of the coil. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Start", meta = (ClampMin = "1.0", ClampMax = "6.0"))
	float StartBurstAccelMultiplier = 2.5f;

	/** Speed (cm/s) at which the burst ends and normal acceleration takes over — the
	 *  launch-step covers the idle→trot band; the sprint top end is earned normally. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Start", meta = (ClampMin = "100.0", ClampMax = "1000.0"))
	float StartBurstEndSpeed = 400.0f;

	/** Scale on the start-step coil crouch drop (§B2 envelope synced to StartProgress, peak
	 *  4.5 cm at the loaded coil, released as the launch rises — same retargeting doctrine as
	 *  the pivot/skid tables). When > 0 it REPLACES the springy StartCoilDip cushion kick
	 *  (authored crouch + spring kick would double-dip). 0 disables and restores the kick. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Start", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float StartCrouchScale = 1.0f;

	/** M4 input shaping: fresh input from idle ramps 0→1 over this window (s) so a keyboard
	 *  tap doesn't twitch the cat. Skipped on the coil path (the burst is its own envelope);
	 *  0 disables. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Start", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float StartInputRampTime = 0.12f;

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

	/** Traversal verbs (mantle first; wall bounce/scramble/fence trot/curtain climb later).
	 *  Owns its verbs' movement takeovers — the one CMC apply/restore point for traversal. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Traversal")
	TObjectPtr<class UCatTraversalComponent> Traversal;

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

	/** Continuous slope ground-conform on the mesh's relative Z (≤0, interp state) — the kit
	 *  HeightFixer role: on an incline the capsule contacts the slope uphill of center and the
	 *  mesh hangs a few cm above the surface; this drops the whole body onto it. The spine
	 *  control-point drops are computed RELATIVE to this so the Spline IK only bends for the
	 *  fore/aft remainder instead of arching the whole correction. */
	float MeshGroundConformZ = 0.0f;

	/** Interp state for the TurnStanceDrop ease (0 → −TurnStanceDrop while turning). */
	float TurnStanceDropZ = 0.0f;

	/** Wall-hug conform state — mesh rel-X (actor-forward) slide toward the attached wall,
	 *  and the mesh's authored relative X captured in BeginPlay to offset from. */
	float WallHugForward = 0.0f;

	/** Rendered wall-transfer trajectory pitch (deg, nose-up positive) — eases toward
	 *  the traversal component's weighted target in the §C compose; the interp is also
	 *  the abort path (a mid-arc abort drops the target to 0 in one frame and the
	 *  interp walks the mesh back level instead of snapping). Cosmetic, local. */
	float MeshBaseRelLocX = 0.0f;

	/** Mantle-phase hug latch (−1 = unset): the hug value is frozen at mantle start so a
	 *  corner-straddling trace can't flap the mesh (see the corner-flap guard in §B3). */
	float MantleHugLatch = -1.0f;

	/** Post-mantle anim hold (s): bGoMantle stays true with MantleProgress pinned at 1 through
	 *  the ~2-uu exit drop, so the anim SM goes Mantle → Jump_Land directly instead of flashing
	 *  Jump_Fall for the 2–3 frames the land prediction can't cover (its trace runs along the
	 *  mostly-horizontal exit velocity and misses the floor 2 uu below — PawPrint 2026-07-30).
	 *  Cleared by Landed(); this timer is the slid-off-the-lip failsafe. */
	float MantleAnimHoldTimer = 0.0f;

	/** True when last frame's §B2 drop came from a scrub curve (pivot/skid) — sustained
	 *  curve mode tracks the table exactly; interp smooths only the enter/exit edges. */
	bool bCurveDropWasActive = false;

	/** Whole-body slope pitch on the mesh's relative rotation (degrees, interp state).
	 *  Bending Spine→Spine_3 can't pitch a quadruped — the hips aren't in the chain, so the
	 *  bend kinks at the spine root (the "weird back arch"). Pitching the whole mesh aligns
	 *  hips AND shoulders to the slope; paw goals, spine CPs, and paw rotation then shrink
	 *  to micro-residuals automatically. */
	float MeshSlopePitch = 0.0f;

	/** The mesh's authored relative rotation, captured in BeginPlay (the −90° yaw rig offset);
	 *  the slope pitch composes onto this. */
	FRotator MeshBaseRelRot = FRotator::ZeroRotator;

	/** Camera-weight layer (M3): per-gait lag blend, dip spring, sprint FOV push,
	 *  stop-settle. Runs for the locally controlled cat only (it owns the camera). */
	void UpdateCameraWeight(float DeltaTime);

	/** Camera dip spring state (cm, cm/s) on the spring arm's SocketOffset.Z —
	 *  kicked by Landed() (impact dip) and the stop-settle detector. */
	float CamDipOffset = 0.0f;
	float CamDipVelocity = 0.0f;

	/** The spring arm's authored SocketOffset.Z / camera FOV, captured in BeginPlay;
	 *  the dip and FOV push offset from these. */
	float CamBaseSocketOffsetZ = 0.0f;
	float CameraBaseFOV = 90.0f;

	/** Peak ground speed since the cat last stopped — drives the stop-settle kick scale. */
	float CamPeakSpeedSinceStop = 0.0f;

	/** Smoothed sprint blend (0..1) for the lag/FOV gait lerp — rises with raw speed,
	 *  relaxes at CameraGaitRelaxSpeed so braking doesn't snap the lag back to base. */
	float CamGaitAlpha = 0.0f;

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

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> SprintAction;

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

	// ── Moving Pivot (M2 part 2) ────────────────────────────────────────
	/** At speed, detects a hard sustained steer and runs the plant-and-turn. Local owner only;
	 *  runs BEFORE UpdateTurnInPlace so its SpeedType=Turn write gates turn-in-place off and
	 *  its footwork target rides the shared TurnRateAnim interp + turn RPC block. */
	void UpdateMovingPivot();

	/** Pivot entry: suppress input, boost braking (predicted + server RPC), kick the plant dip. */
	void EnterPivot();

	/** Pivot exit: restore braking, arm the cooldown. Safe to call redundantly. */
	void ExitPivot();

	/** Applies (true) or restores (false) the pivot braking deceleration on the CMC. */
	void ApplyPivotBraking(bool bApply);

	// ── Weighty Stop (M2 part 2b) ───────────────────────────────────────
	/** Detects the input-release edge at speed and runs the stop run-out + plant. Local owner
	 *  only; runs AFTER UpdateMovingPivot (a pivot supersedes a stop — its input suppression
	 *  must not read as a release). */
	void UpdateWeightyStop();

	/** Stop entry: swap to constant-deceleration braking (predicted + server RPC). */
	void EnterStop();

	/** Stop exit: restore gait braking (deferring to a live pivot). bPlanted = the run-out
	 *  completed — fires the cushion plant kick and arms the re-accel ramp. Safe to call redundantly. */
	void ExitStop(bool bPlanted);

	/** Applies (true) or restores (false) the stop braking: separate braking friction + the
	 *  constant StopBrakingDeceleration. The restore path re-asserts whichever braking the
	 *  CMC should be under (pivot boost if one is live, else the normal gait braking). */
	void ApplyStopBraking(bool bApply);

	// ── Weighty Start (M2 part 2c) ──────────────────────────────────────
	/** Detects a fresh sprint start from near-standstill and runs the coil → burst. Local
	 *  owner only; runs after UpdateWeightyStop in the tick. */
	void UpdateWeightyStart();

	/** Coil entry: suppress input for StartCoilTime, kick the load dip. */
	void EnterStartCoil();

	/** Burst entry: boost MaxAcceleration (predicted + server RPC). */
	void EnterStartBurst();

	/** Burst exit: restore MovementAcceleration, log the launch numbers. Safe to call redundantly. */
	void EndStartBurst();

	/** Applies (true) or restores (false) the burst MaxAcceleration on the CMC. */
	void ApplyStartBurstAccel(bool bApply);

	/** Fires on IA_Interact Started — server-authoritative trace. */
	void TriggerInteract();

	/** Fires on IA_Grab Started — initiates mouth grab. */
	void TriggerGrab();

	/** Fires on IA_Grab Completed — releases mouth grab. */
	void TriggerRelease();

	/** Fires on IA_Jump Started — records the jump-buffer timestamp, then forwards to ACharacter::Jump.
	 *  The buffer lets a press made just before landing still fire on touchdown (see Landed). */
	void OnJumpInputPressed();

	/** Fires on IA_Sprint Started/Completed — owner-predicted gait switch (see SetSprinting). */
	void OnSprintPressed();
	void OnSprintReleased();

	// ── Sprint Gait ─────────────────────────────────────────────────────

	/** Local entry point for sprint intent: predicts the gait switch on the owning
	 *  client (same pattern as the grab drag-settings prediction) and routes to the
	 *  server for authority. Listen-server hosts set state directly (RPC no-op gotcha). */
	void SetSprinting(bool bNewSprinting);

	/** Client → Server: sprint intent. */
	UFUNCTION(Server, Reliable)
	void Server_SetSprinting(bool bNewSprinting);

	/** Applies the active gait's CMC tuning (MaxWalkSpeed, yaw rotation rate) from
	 *  bIsSprinting. Skipped while grabbing — drag settings own the CMC then; the
	 *  grab-release restore re-applies the current gait. */
	void ApplyGaitMovementSettings();

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
	void Server_SetTurnActive(bool bNewGoTurn, float BodyYaw);

	/** Client → Server: continuous blendspace update during active turn. Unreliable is fine — loss just holds last value. */
	UFUNCTION(Server, Unreliable)
	void Server_SetTurnRate(float NewTurnRateAnim, float BodyYaw);

	/** Applies an owning client's in-place body yaw to this (non-locally-controlled) copy. */
	void ApplyClientTurnYaw(float BodyYaw);

	/** Client → Server: mirror the pivot braking boost so server-side move replay brakes the
	 *  same as the owning client (the grab drag-settings prediction pattern). Reliable —
	 *  a lost restore would leave the server braking hard forever. EntryAngleDeg carries the
	 *  latched pivot angle on the enter edge (0 on exit) so proxies select the same clip variant. */
	UFUNCTION(Server, Reliable)
	void Server_SetPivotBraking(bool bApply, float EntryAngleDeg);

	/** Client → Server: throttled pivot-clip scrub position (the Server_SetTurnRate
	 *  streaming pattern). Unreliable — loss just holds the last scrub for a frame. */
	UFUNCTION(Server, Unreliable)
	void Server_SetPivotProgress(float NewProgress);

	/** Client → Server: mirror the stop braking swap so server-side move replay coasts the same
	 *  as the owning client. Reliable — a lost restore would leave the server coasting forever.
	 *  bSkid carries the skid-clip engage on the enter edge (sprint-band stops only) so proxies
	 *  enter the Skid state too. */
	UFUNCTION(Server, Reliable)
	void Server_SetStopBraking(bool bApply, bool bSkid);

	/** Client → Server: throttled skid-clip scrub position (the Server_SetPivotProgress
	 *  streaming pattern). Unreliable — loss just holds the last scrub for a frame. */
	UFUNCTION(Server, Unreliable)
	void Server_SetSkidProgress(float NewProgress);

	/** Client → Server: mirror the start-burst acceleration boost so server-side move replay
	 *  accelerates the same as the owning client. Reliable — a lost restore would leave the
	 *  server permanently over-accelerating. */
	UFUNCTION(Server, Reliable)
	void Server_SetStartBurstAccel(bool bApply);

	/** Client → Server: start-step clip engage edges (the skid pattern — coil enter has no
	 *  existing mirror RPC to ride, the burst one fires 0.12 s too late for the coil pose). */
	UFUNCTION(Server, Reliable)
	void Server_SetStartStep(bool bActive);

	/** Client → Server: mirror the mantle takeover (the grab pattern) — the server drives
	 *  its own copy deterministically from the same start/target. */
	UFUNCTION(Server, Reliable)
	void Server_StartMantle(FVector_NetQuantize InStart, FVector_NetQuantize InTarget);

	/** Client → Server: mirror a wall bounce. The WALL NORMAL is what crosses the wire,
	 *  not the resulting velocity, so both machines derive the launch from the same
	 *  tuning knobs (the pivot-braking mirror pattern). Reliable — a dropped bounce
	 *  would desync the arc for the whole airborne span. */
	UFUNCTION(Server, Reliable)
	void Server_WallBounce(FVector_NetQuantizeNormal WallNormal, bool bKickRight);

	/** Client → Server: mirror a chimney wall-transfer takeover. The server drives its
	 *  own copy deterministically from the same start/target/normal — the mantle model,
	 *  no progress streaming. Reliable: a dropped transfer would teleport-desync. */
	UFUNCTION(Server, Reliable)
	void Server_WallTransfer(FVector_NetQuantize InStart, FVector_NetQuantize InTarget,
		FVector_NetQuantizeNormal InTargetNormal, bool bKickRight);

	/** Client → Server: mirror both edges of a wall attach (cling or scramble). The server
	 *  runs the same deterministic vertical profile from the same entry parameters, so no
	 *  progress streaming is needed — the mantle model. Reliable: a dropped enter would
	 *  leave the server falling while the owner hangs, and a dropped exit would strand it. */
	UFUNCTION(Server, Reliable)
	void Server_SetWallAttach(bool bActive, FVector_NetQuantizeNormal WallNormal,
	                          float RiseSpeed, float RiseTime);

	/** Written by the traversal component on owner and server; replicated to proxies. */
	void SetWallAttachAnimState(bool bActive);

	/** Written by the traversal component on owner and server; replicated to proxies. */
	void SetMantleAnimState(bool bActive, float Progress);

	/** Completed-mantle handoff: keep the Mantle anim state alive (progress pinned 1) until
	 *  the real landing clears it — see MantleAnimHoldTimer. Called from EndMantle. */
	void BeginMantleAnimHold();

	/** Accessors for the traversal component's detection gates. */
	bool IsGrabbing() const { return bIsGrabbing; }
	bool HasMovementInput() const { return bHasMovementInput; }

	/** Client → Server: throttled start-step scrub position. Unreliable — loss holds a frame. */
	UFUNCTION(Server, Unreliable)
	void Server_SetStartProgress(float NewProgress);

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

	/** True while sprinting (server-authoritative gait state, owner-predicted). Replicated to
	 *  every machine so all copies agree on gait CMC tuning and SpeedType classification. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, ReplicatedUsing = OnRep_bIsSprinting, Category = "Animation State")
	bool bIsSprinting = false;

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

	UFUNCTION()
	void OnRep_bIsSprinting();

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

	/** BS1_Cat_Aim scrub time — camera yaw vs body mapped from ±180° to 0..1 (0.5 = centered).
	 *  The aim clips bake the full yaw sweep into their timeline; the ABP's Blendspace Evaluator
	 *  scrubs it with this (kit AnimBP wiring: NormalizedTime = yaw, X axis = pitch). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float AimBSYawTime = 0.5f;

	/** BS1_Cat_Aim X axis — camera pitch mapped from ±60° (kit pitch-max) to −1..+1. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float AimBSPitch = 0.0f;

	/** Short pulse telling the ABP's ears/blink additive SM to fire the blink one-shot. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	bool bPlayBlink = false;

	/** Short pulse telling the ABP's ears/blink additive SM to fire an ear-twitch one-shot. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	bool bPlayEarsTwitch = false;

	/** Which ear-twitch clip the ABP plays (0..2 → A_Cat_Add_Ears_1/2/3; never repeats back-to-back). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	int32 IndexEars = 0;

	/** Normalized position on the standstill jump's anim timeline, 0..1 = [anticipation coil |
	 *  rise]: 0→0.425 scrubs the authored crouch (clip 0→0.51 of A_Cat_Jump_InPlace) across the
	 *  anticipation window, 0.425→1 scrubs the rise (clip 0.51→1.2) uniformly over the expected
	 *  rise TIME (deliberately not height — height-faithful scrubbing compressed the push-off
	 *  into an unreadable flash). The run branch maps only the 0.425..1 span (no coil at a run).
	 *  Sequence Evaluators in Jump_Launch consume it via MapRangeClamped nodes. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float JumpRiseProgress = 0.0f;

	/** Standstill-jump anticipation: how long the cat coils (playing the authored crouch)
	 *  between the jump press and the actual launch. Running jumps fire instantly (an input
	 *  delay at a run would hurt platforming). 0 disables the coil entirely. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning", meta = (ClampMin = "0.0", ClampMax = "0.3"))
	float JumpAnticipationDuration = 0.12f;

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

	/** True while a moving pivot owns the Turn state — the ABP swaps the BS1_Cat_Turn
	 *  footwork for the dedicated plant-pivot clip. Owner-predicted; rides the existing
	 *  Server_SetPivotBraking mirror (fires on exactly the pivot enter/exit edges). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Animation|Cosmetic")
	bool bGoPivot = false;

	/** 0→1 pivot completion: accumulated applied rotation / reachable rotation (the
	 *  denominator grows if the live target drifts — a remaining-angle form stalled
	 *  mid-clip during camera sweeps), clamped monotonic. Scrubs the plant-pivot clip
	 *  evaluators in the ABP so plant/steps stretch across the whole rotation and the
	 *  push-off lands exactly at rotation completion, regardless of pivot size (a
	 *  fixed-time one-shot desynced on ~0.8 s pivots).
	 *  Owner-computed; streamed via Server_SetPivotProgress (the TurnRateAnim pattern). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Animation|Cosmetic")
	float PivotProgress = 0.0f;

	/** Pivot rotation size (deg) latched at entry — the ABP selects the 90° vs 180° plant-pivot
	 *  clip variant on it (latched, not live: the denominator grows on camera sweeps and a
	 *  mid-pivot clip switch would pop). Rides the Server_SetPivotBraking enter edge. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Animation|Cosmetic")
	float PivotAngleDeg = 0.0f;

	/** True while a sprint-band weighty stop drives the Skid state — the ABP swaps the gait
	 *  run-out for the skid-stop clip. Trot-band stops keep the plain gait run-out (their
	 *  imperceptibility is a design decision). Rides the Server_SetStopBraking enter edge. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Animation|Cosmetic")
	bool bGoSkid = false;

	/** 0→1 skid completion: (1 − Speed/entry) / (1 − plant/entry) — linear in time under the
	 *  stop's constant decel, exactly 1 at the plant (reachable-mapped so the clip tail can't
	 *  snap; the pivot lesson). Clamped monotonic; frozen (not snapped) on an abort so the
	 *  blend-out leaves from the pose the skid was actually in.
	 *  Owner-computed; streamed via Server_SetSkidProgress. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Animation|Cosmetic")
	float SkidProgress = 0.0f;

	/** True while the traversal component's mantle owns the character — the ABP plays the
	 *  clamber clip scrubbed by MantleProgress (placeholder anim until the clip lands).
	 *  Owner + server both write it from their own mantle drive (deterministic from the
	 *  RPC'd start/target — no progress streaming); proxies read the replicated values. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Animation|Cosmetic")
	bool bGoMantle = false;

	/** 0→1 mantle completion along the takeover curve. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Animation|Cosmetic")
	float MantleProgress = 0.0f;

	/** True when the active mantle is the low-ledge VAULT bucket (ledge ≤ the traversal
	 *  component's MantleVaultMaxHeight): fores plant on top, quick hind hop — vs the
	 *  deep-hang pull-up for tall ledges. Latched in StartMantle on owner + server from
	 *  the same deterministic params (the PivotAngleDeg precedent); selects the clip in
	 *  the ABP Mantle state and the §B2/curve tables in C++. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Animation|Cosmetic")
	bool bMantleVault = false;

	/** The active mantle's ledge height (uu), latched with bMantleVault — scales the §B2
	 *  drop tables to the actual wall (each is contact-calibrated at one height: climb 68,
	 *  vault 40) so bucket-edge ledges stay planted instead of floating (2026-07-31). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Animation|Cosmetic")
	float MantleLedgeHeight = 0.0f;

	/** True while the traversal component holds the cat against a wall (cling or scramble).
	 *  Owner + server both write it from their own attach drive; proxies read the replicated
	 *  value. Drives the ABP WallHang state (cling/scramble clips since 2026-07-31). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Animation|Cosmetic")
	bool bGoWallAttach = false;

	/** True for the wall-kick anim window after a wall bounce — the ABP plays the
	 *  Climb_JumpDown plant-and-twist clip (WallKick state) instead of re-entering
	 *  Jump_Launch's run-start scrub. Set in DoWallBounce on owner + server (the RPC
	 *  mirror runs the same code, the mantle model — no streaming); cleared by
	 *  Landed(), a new wall attach, a mantle, or WallKickAnimDuration expiring. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Animation|Cosmetic")
	bool bGoWallKick = false;

	/** Which twist variant the active kick plays (true = Climb_JumpDown_Right, which
	 *  renders a right-shoulder twist — Sean-verified by the round-2 always-Left read).
	 *  Chosen on the OWNER from the lateral component of the held input across the
	 *  launch line (the twist leads the arc the player is steering); neutral input
	 *  ALTERNATES shoulders for the chimney rhythm. The bit rides Server_WallBounce so
	 *  both machines pick the same clip. Rounds 1-2 lesson: any signal derived from
	 *  actor-vs-launch geometry is CONSTANT for cling kicks — DriveWallAttach re-faces
	 *  the wall every frame, so the yaw delta sat pinned on the ±180° wrap boundary
	 *  and every kick turned the same way. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Animation|Cosmetic")
	bool bWallKickRight = false;

	/** True once a wall transfer's push segment has handed off — the WallKick state
	 *  blends from the JumpDown push clip to the authored sailing pose
	 *  (A_Cat_Transfer_Arc) for the crossing. Round 13: the crossing previously
	 *  borrowed Jump_Fall's apex pose, which is authored as the tip-over moment of
	 *  a ballistic jump (nose-down −20°, a 92° attitude drop at the handoff from
	 *  the +71° push extension) — the pack has no "sailing between walls" pose, so
	 *  one was authored. Only ever true during a transfer (bGoWallKick spans the
	 *  whole crossing there); raw bounces and solitary cling kicks keep the
	 *  push-window expiry hand-off to the real airborne poses. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Animation|Cosmetic")
	bool bWallKickArcPhase = false;

	/** Seconds bGoWallKick stays up after the press — the PUSH SEGMENT of the JumpDown
	 *  clip only (load → spring → extension; the authored lean starts ~0.4). The clip's
	 *  back half (in-pose 180° twist + dive-away tail) is deliberately UNUSED (round 5,
	 *  2026-08-01): it is dismount choreography, and composing its in-pose twist with
	 *  any interrupt (re-cling, landing) produced the flip-back — the turn now lives
	 *  entirely on the actor-yaw ease (WallKickTurnRate). Expiry hands off through the
	 *  WallKick → Jump_Apex/Jump_Fall rules onto the normal airborne poses. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning|Wall Kick", meta = (ClampMin = "0.1", ClampMax = "2.0"))
	float WallKickAnimDuration = 0.55f;   // 0.45 → 0.55 round 6: the clip's first hint of
	                                      // authored lean plays during the swing (head and
	                                      // shoulders lead the turn) and the frozen-pose gap
	                                      // before the apex handoff closes.

	/** Body compression toward the wall (uu) during the kick's load beat — composed into
	 *  the wall-hug mesh slide on a SmoothStep over the anticipation window, released by
	 *  the hug's own decay as the body springs away. This restores the clip's authored
	 *  load-sink, which lives in ROOT translation and is discarded by the skeleton (the
	 *  §B2 lesson in wall-normal costume): without it the legs fold around a stationary
	 *  body and the load never reads. Small paw-into-wall cost during the load accepted
	 *  (same class as the skid's blend-window penetration). */
	// (Rounds 6-7's WallKickCoilDepth / WallKickLoadDip are GONE, not zeroed: the wall
	// transfer's capsule curve carries the clip's own root-arc load dip and spring, so
	// procedural load fakes are no longer needed — and a knob whose only correct value
	// is 0 is a trap for a later session, the away-release lesson.)

	/** Actor-yaw swing rate (deg/s) from the wall facing to the launch line after a
	 *  kick — THE single rotation source for the kick turn (the clip contributes pose,
	 *  not yaw). ~650 swings a chimney 180° in ~0.28 s ≈ the measured wall-to-wall
	 *  flight. Direction comes from bWallKickRight (input steer / alternation), never
	 *  from shortest-path — a chimney 180 sits exactly on the angle-wrap boundary. At
	 *  a re-cling the attach's own face-ease continues the turn from wherever this one
	 *  is: one continuous rotation from launch to grip, no duty swap. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning|Wall Kick", meta = (ClampMin = "90.0", ClampMax = "3000.0"))
	float WallKickTurnRate = 650.0f;

	/** +1 = swing right (yaw increasing), −1 = left. Latched with bWallKickRight. */
	float WallKickTurnDir = 1.0f;

	/** Sweep state (round 6 — SmoothStep profile instead of constant rate: constant
	 *  angular velocity is the definition of mechanical). Latched in DoWallBounce:
	 *  yaw = Start + Angle × SmoothStep(Elapsed / Duration), Duration = |Angle| / rate. */
	float WallKickSweepStartYaw = 0.0f;
	float WallKickSweepAngle    = 0.0f;   // signed, along WallKickTurnDir — never shortest-path
	float WallKickSweepDuration = 0.1f;
	float WallKickSweepElapsed  = 0.0f;

	/** Countdown for the kick anim window. Armed only where DoWallBounce runs (owner +
	 *  server); proxies read the replicated flag. */
	float WallKickAnimTimer = 0.0f;

	/** Clip time for the ABP's JumpDown evaluators (round 15) — advances while the
	 *  kick's push segment plays, FREEZES at the arc handoff, holds through blend-out.
	 *  Derived on every machine from bGoWallKick/bWallKickArcPhase — not replicated. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float WallKickClipTime = 0.0f;

	// (No separate sail time: the ABP derives it as WallKickClipTime − the spring
	//  clip's length, and each evaluator clamps at its own ends. The 1.2333 literal
	//  in the WallKick graph is that clip length — if A_Cat_Wall_Spring is ever
	//  re-authored to a different duration, WallTransferPushTime AND that literal
	//  both follow it.)

	/** Edge detector for the clip-time reset (all roles). */
	bool bPrevGoWallKickForClipTime = false;

	/** Kick-press hug latch (§B3, round 15) — the hug may decay during a kick's push
	 *  but never grow past its at-press value (the paws are leaving the wall). */
	float KickPressHug = 0.0f;
	bool bKickPressHugValid = false;

	/** Kick yaw-ease state (round 5, 2026-08-01 — replaces the hold-then-late-flip
	 *  model, whose flip never arrived in chimneys: every kick was re-clung at ~0.33 s,
	 *  mid-twist, and the pose-untwist + shortest-path attach turn composed as the
	 *  "180 flip back the other way"). While bWallKickHoldingYaw, Tick sweeps the actor
	 *  yaw toward WallKickFaceYaw at WallKickTurnRate along WallKickTurnDir (never
	 *  shortest-path — a chimney 180 sits on the wrap boundary), orient-to-movement
	 *  off; arrival or an interrupt releases it. */
	float WallKickFaceYaw = 0.0f;
	bool  bWallKickHoldingYaw = false;

	/** Stop the kick yaw-ease: restore orient-to-movement (unless a grab owns it).
	 *  Interrupts (attach / mantle / landing) pass false and leave the yaw wherever the
	 *  ease got to — the attach face-ease or grounded orient-to-movement continues the
	 *  turn naturally from there. The snap branch is a legacy safety, currently unused. */
	void FinishWallKickYawHold(bool bSnapToLaunchYaw);

	/** True while a sprint start (coil + burst) drives the Start state — the ABP plays the
	 *  start-step clip scrubbed by StartProgress. Rides Server_SetStartStep on the edges. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Animation|Cosmetic")
	bool bGoStartStep = false;

	/** 0→1 start-step completion: the coil hold maps to the clip's coil portion
	 *  (StartClipCoilFrac) by timer, the burst maps the remainder by Speed/StartBurstEndSpeed —
	 *  the launch pose tracks the physical acceleration. Snap-1 at burst end, frozen on abort.
	 *  Owner-computed; streamed via Server_SetStartProgress. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Animation|Cosmetic")
	float StartProgress = 0.0f;

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

	/** Scale on the mantle deep-hang drop — same doctrine as PivotCrouchScale, but sourced from
	 *  the retargeted `Ledge_Climb_Up` clip: its pelvis translation dives to ~4 uu (body hanging
	 *  low against the wall face through the pull) and the skeleton's all-SKELETON translation
	 *  retargeting discards it, which rendered the tucked hind legs as a mid-air kangaroo sit
	 *  (frame-verified vs the pack showcase, 2026-07-30). The §B2 valley envelope (peak −25.4 uu
	 *  at progress 0.4) restores it synced to MantleProgress. 0 disables. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement Tuning|Mantle", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float MantleCrouchScale = 1.0f;

	// ── Wall-hug conform (traversal wall clips, 2026-07-30) ─────────────
	/** Master switch: while wall-attached (cling/scramble) or mantling, trace along actor
	 *  forward and slide the MESH toward the wall so the wall clips' paw plane meets the
	 *  surface. The attach freezes the capsule wherever the probe caught the wall (the gap
	 *  varies per catch, up to the probe reach) and the skeleton discards body translation,
	 *  so no clip can close this gap — the §B2 procedural-drop doctrine rotated 90°.
	 *  Cosmetic + local on every machine (proxies derive it from the replicated flags and
	 *  the replicated rotation; no RPC surface). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning|Wall Hug")
	bool bEnableWallHug = true;

	/** Where the wall clips' paw-contact plane sits along mesh forward relative to the capsule
	 *  center (uu, negative = behind it). Measured −7 on the retargeted climb set (the authored
	 *  A_Cat_Wall_Hang sat at −2). Slide = wall distance − WallHugSurfaceBias − this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning|Wall Hug", meta = (ClampMin = "-20.0", ClampMax = "20.0"))
	float WallHugPawPlane = -7.0f;

	/** Stop the paw plane this far short of the surface (paw thickness). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning|Wall Hug", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float WallHugSurfaceBias = 2.0f;

	/** Max forward mesh slide (uu). An extreme catch distance keeps some float past this —
	 *  preferable to rendering the body a half-capsule away from where gameplay says it is. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning|Wall Hug", meta = (ClampMin = "0.0", ClampMax = "60.0"))
	float WallHugMaxSlide = 40.0f;

	/** Engage/release interp speed (1/s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning|Wall Hug", meta = (ClampMin = "1.0", ClampMax = "50.0"))
	float WallHugInterpSpeed = 10.0f;

	/** Build speed while a wall CATCH is landing (1/s) — the transfer's pre-seed window
	 *  (progress > 0.8, facing has swung to the far wall) and the first 0.3 s of any
	 *  attach. Round 14: at the base 10/s the hug was 0 at every catch and took ~0.2 s
	 *  to build (0 → 26 measured) — paws gripping air for ~40% of a chimney-rhythm
	 *  cling. Rising moves only; release keeps the gentle base speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation Tuning|Wall Hug", meta = (ClampMin = "1.0", ClampMax = "60.0"))
	float WallHugCatchInterpSpeed = 25.0f;

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

	/** Per-paw surface-conform rotation (actor-space additive, fed to the same goal-VB ModifyBones
	 *  as the Z offsets — kit SetToeRot: Roll=atan2(N.Y,N.Z), Pitch=−atan2(N.X,N.Z) on the trace
	 *  normal, RInterp 30). Zeroed while the paw swings or the trace misses. Cosmetic, local. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	FRotator FootIKRot_HandL = FRotator::ZeroRotator;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	FRotator FootIKRot_HandR = FRotator::ZeroRotator;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	FRotator FootIKRot_FootL = FRotator::ZeroRotator;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	FRotator FootIKRot_FootR = FRotator::ZeroRotator;

	/** Fore/aft slope scrub for the A_Cat_Add_Incline evaluator (2 s clip, time = incline axis):
	 *  1.0 = flat, 0.8 = full downhill, 1.2 = full uphill (kit multiplier 0.2 around neutral). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float SpineInclineF = 1.0f;

	/** Lateral slope magnitude → A_Cat_Add_P_Squat additive weight, 0..0.2 (kit |±20 uu → ±0.2|). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float SpineInclineS = 0.0f;

	/** A_Cat_Add_P_UpTail additive weight — kit MapClamped(SpineInclineF, 1.0..1.2 → 0..0.3). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float UpTailAlpha = 0.0f;

	/** Spline-IK control-point drops (cm, ≤0): the body sinks toward the lowest UNCLAMPED paw
	 *  offset (pelvis = back paws, chest = front) so the upward-only foot IK bends the uphill
	 *  legs — the kit's "pelvis follows lowest foot" with our anti-penetration offsets. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float PelvisDropZ = 0.0f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float ChestDropZ = 0.0f;

	/** Spine-block weight: grounded × MapClamped(Speed, 0..800 → 1..0) (kit taper), interp 10. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Animation|Cosmetic")
	float SpineIKAlpha = 0.0f;

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

	/** Last body yaw sent via the turn RPCs — turn-in-place rotation does NOT replicate
	 *  through the CMC (SetActorRotation with zero acceleration never leaves the client;
	 *  found in the 2026-07-06 MP pass), so the yaw rides the turn RPCs explicitly. */
	float LastSentBodyYaw = 0.0f;

	/** Server-side target for a remote client's in-place body yaw. RPCs arrive in ~5° steps;
	 *  snapping to each read as jitter on the host — the server copy interps toward this at
	 *  the turn speed instead (smooth locally AND a smooth source for proxy replication). */
	float ClientTurnTargetYaw = 0.0f;
	bool bHasClientTurnTarget = false;

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

	// ── Moving Pivot State (local owner only) ──────────────────────────

	/** True while a plant-and-turn pivot is running (input suppressed, body rotating to input). */
	bool bIsPivoting = false;

	/** Accumulates time the steering angle has been past PivotAngleThreshold. */
	float PivotSustainTimer = 0.0f;

	/** Time remaining before another pivot may arm. */
	float PivotCooldownTimer = 0.0f;

	/** This frame's footwork rate target (±1), consumed by the shared TurnRateAnim interp in UpdateTurnInPlace. */
	float PivotTurnRateTarget = 0.0f;

	/** World-space input direction cached by Move() — the live pivot target (input events fire before Tick). */
	FVector PivotLiveInputDir = FVector::ZeroVector;

	/** Input-direction yaw last tick (for the sweep accumulator). */
	float PivotPrevInputYaw = 0.0f;

	/** Accumulated |input-direction yaw change| (deg) while the sustain window runs — the
	 *  sweep-tier discriminator (taps accumulate ~0, camera sweeps keep adding). */
	float PivotSweepAccumDeg = 0.0f;

	/** Seconds since Move() last supplied non-zero input; past ~0.15 s the input counts as released. */
	float PivotInputStaleTime = 1.0f;

	/** Total |angle| (deg) to the input target, latched at pivot entry — PivotProgress's denominator. */
	float PivotInitialAngleDeg = 1.0f;

	/** Last PivotProgress sent via Server_SetPivotProgress (throttle: send on Δ ≥ 0.05). */
	float LastSentPivotProgress = 0.0f;

	/** Rotation (deg) actually applied so far this pivot — the scrub numerator. Grows at the
	 *  real turn rate, so the scrub cannot stall when a sweeping camera drags the target. */
	float PivotAccumDeg = 0.0f;

	/** Fraction of the start-step clip timeline occupied by the authored coil — the coil
	 *  timer maps onto it, the burst maps onto the remainder. Must match the authored clip
	 *  (coil keypose at frame 5 of 16 ≈ 0.24 of the 0.5 s clip). */
	static constexpr float StartClipCoilFrac = 0.24f;

	/** Arm/disarm the start-step clip scrub on both machines; optionally snap progress to 1. */
	void SetStartStepActive(bool bActive, bool bSnapProgress);

	/** Throttled owner→server StartProgress stream (send on Δ ≥ 0.05). */
	void StreamStartProgress();

	// ── Weighty Stop State (local owner only) ──────────────────────────

	/** True while a stop run-out is active (constant-deceleration braking applied). */
	bool bIsStopping = false;

	/** Ground speed when the run-out began — scales the plant dip and the re-accel ramp. */
	float StopEntrySpeed = 0.0f;

	/** Skid scrub denominator latched at stop entry: 1 − StopPlantSpeed/StopEntrySpeed. */
	float SkidReachableFrac = 1.0f;

	/** Last SkidProgress sent via Server_SetSkidProgress (throttle: send on Δ ≥ 0.05). */
	float LastSentSkidProgress = 0.0f;

	/** Last StartProgress sent via Server_SetStartProgress (throttle: send on Δ ≥ 0.05). */
	float LastSentStartProgress = 0.0f;

	/** World location where the run-out began (distance logging — the M5 skid-clip spec numbers). */
	FVector StopStartLocation = FVector::ZeroVector;

	/** Seconds the current run-out has been active (failsafe timeout on steep downhill). */
	float StopElapsed = 0.0f;

	/** Seconds remaining of the post-plant re-acceleration ramp (consumed by Move). */
	float StopReaccelTimer = 0.0f;

	/** The armed ramp's full duration (denominator of Move's 0→1 input scale). */
	float StopReaccelDuration = 0.0f;

	/** bHasMovementInput last frame — a stop triggers on the had-input → released EDGE only,
	 *  so external shoves at speed with no prior input can't start a run-out. */
	bool bHadMovementInputLastFrame = false;

	/** PawPrint telemetry sink (null outside game/PIE worlds). Cached in BeginPlay;
	 *  the locally controlled cat pushes its channel set every tick. */
	UPROPERTY(Transient)
	TObjectPtr<UPawPrintSubsystem> PawPrint = nullptr;

	// ── Weighty Start State (local owner only) ─────────────────────────

	/** True while the coil holds (input suppressed, StartCoilTimer running). */
	bool bIsStartCoiling = false;

	/** Seconds remaining of the coil hold. */
	float StartCoilTimer = 0.0f;

	/** True while the burst acceleration boost is applied. */
	bool bIsStartBursting = false;

	/** Seconds the burst has been running (failsafe timeout). */
	float StartBurstElapsed = 0.0f;

	/** World location at coil entry (launch-distance logging — the M5 start-clip spec numbers). */
	FVector StartCoilLocation = FVector::ZeroVector;

	/** Seconds before another coil may arm (keeps tap-tap sprint presses from chain-coiling). */
	float StartCooldownTimer = 0.0f;

	/** Move-input freshness last frame (for the fresh-input trigger edge). */
	bool bInputWasFreshLastFrame = false;

	/** bIsSprinting last frame (for the sprint-engage trigger edge). */
	bool bWasSprintingLastFrame = false;

	/** Seconds remaining of the M4 fresh-input ramp (consumed by Move alongside the stop's ramp). */
	float InputRampTimer = 0.0f;

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

	// ── Additive idle-life timers (blink / ear twitch) — cosmetic, local ──
	// Countdowns to the next blink / ear twitch; re-rolled on fire (intervals are
	// constexpr in UpdateCosmeticInterpolation, mirroring the kit's 3–7 s / 6–12 s).
	float BlinkCountdown = 0.0f;
	float EarsCountdown = 0.0f;
	/** Remaining hold time on the bPlayBlink / bPlayEarsTwitch pulses. */
	float BlinkPulseRemaining = 0.0f;
	float EarsPulseRemaining = 0.0f;
	/** First-tick flag: seeds the countdowns randomly so all cats don't fire in sync at spawn. */
	bool bAdditiveTimersSeeded = false;

	/** Actor Z captured on entering the Launch phase — the JumpRiseProgress baseline. */
	float JumpRiseStartZ = 0.0f;
	bool bJumpRiseTracking = false;

	/** Counts down the standstill-jump coil; the actual Jump() fires when it expires. */
	float JumpAnticipationTimer = 0.0f;
};
