#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "CatTraversalComponent.generated.h"

class ACatBase;

/** Traversal verb currently owning the character (the JumpPhase pattern — one enum,
 *  one owner). Mantle first; wall bounce / scramble / fence trot / curtain climb
 *  join here per the traversal-verbs plan. */
UENUM(BlueprintType)
enum class ECatTraversalState : uint8
{
	None,
	Mantle,
	/** Attached to a wall face with a controlled vertical profile. Serves BOTH the wall
	 *  cling (enter with no rise → catch → slow slide) and the vertical scramble (enter
	 *  with an upward speed budget that decays through zero into the same slide). They
	 *  are the same state machine with different entry parameters and the same exits —
	 *  collapsing them was the 2026-07-24 design call. */
	WallAttach,
};

/** Why an attach ended. Logged per release — the cling has several silent exits and
 *  "it let go and I do not know why" is exactly the class of bug that cost the
 *  2026-07-25 round (see the away-release note on EndWallAttach). */
enum class EWallAttachEnd : uint8
{
	Kick,       // consumed by a wall bounce — the intended exit
	Timeout,    // WallClingMaxTime elapsed
	WallLost,   // the held face is no longer under the trace
	Grounded,   // landed while attached
	Grabbed,    // a mouth grab outranks the attach
	Remote,     // mirrored from the owner via Server_SetWallAttach
	Aborted,    // lost the pawn/CMC — should not happen in practice
};

/**
 * Traversal component — owns the wall-detection trace library and every traversal
 * verb's movement takeover (growth-watch decision: new large systems live in
 * components, and there is ONE CMC apply/restore point here so five verbs don't
 * each fight the grab/stop/start/pivot restore precedence).
 *
 * V1 ships the MANTLE (cat clamber): airborne, pushing toward a wall whose lip is
 * within paw reach → the cat catches the ledge and pulls itself up-and-over on a
 * vertical-first curve. Mechanics-first doctrine: runs on placeholder anim, logs
 * its own clip-authoring spec (height/duration per mantle via LogCatVentures).
 *
 * MP model: the local owner detects and drives; Server_StartMantle (on the pawn,
 * the grab pattern) mirrors the takeover server-side, which drives its own copy
 * deterministically from the same start/target — no progress streaming needed.
 * Proxies read the pawn's replicated bGoMantle/MantleProgress for the anim.
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class CATVENTURES_API UCatTraversalComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCatTraversalComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	/** Begin the mantle takeover (local owner on detection; server via the pawn RPC). */
	void StartMantle(const FVector& InStart, const FVector& InTarget);

	bool IsMantling() const { return TraversalState == ECatTraversalState::Mantle; }

	// ── Tuning (header-only defaults, live-tunable) ─────────────────────

	/** Master switch for the mantle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle")
	bool bEnableMantle = true;

	/** Forward chest-probe reach (uu) — how far ahead a wall can be caught. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "20.0", ClampMax = "100.0"))
	float MantleReachDistance = 45.0f;

	/** Ledge-lip height band ABOVE the capsule bottom (uu). Below min a step-up handles
	 *  it; above max the wall is a scramble/climb candidate, not a mantle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "10.0", ClampMax = "80.0"))
	float MantleMinLedgeHeight = 25.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "40.0", ClampMax = "200.0"))
	float MantleMaxLedgeHeight = 70.0f;   // 105 shipped v1 — Sean live-tuned: 103+ catches read superhero, 64–74 read cat

	/** How far past the wall face the lip probe starts (uu) — the paw-hook depth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "5.0", ClampMax = "50.0"))
	float MantleForwardClearance = 18.0f;

	/** Takeover duration = base + height × per-cm (taller ledges pull longer). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "0.1", ClampMax = "1.5"))
	float MantleBaseDuration = 0.35f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "0.0", ClampMax = "0.01"))
	float MantleDurationPerCm = 0.0015f;

	/** Forward speed handed to the CMC at mantle exit — a soft step-out, the gait
	 *  earns the rest (the start-burst boundary lesson: bake no lunge). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "0.0", ClampMax = "400.0"))
	float MantleExitSpeed = 150.0f;

	/** Re-mantle cooldown (s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float MantleCooldown = 0.25f;

	// ── Wall Bounce (verb 2) ────────────────────────────────────────────
	// Airborne + jump pressed + a wall within reach of the radial chest probe →
	// kick off it. Instantaneous (a LaunchCharacter impulse), so unlike the mantle
	// there is no takeover state and no CMC apply/restore — the launch hands the
	// cat straight back to the CMC's falling physics. Chimney climbing between two
	// close walls falls out for free from repeated bounces off alternating normals.

	/** Master switch for the wall bounce. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallBounce")
	bool bEnableWallBounce = true;

	/** Radial chest-probe reach (uu). Four traces at 90° from the body facing; nearest
	 *  wall-like hit wins, so the wall can be to either side (the chimney case) — the
	 *  mantle's single forward probe would miss it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallBounce", meta = (ClampMin = "20.0", ClampMax = "100.0"))
	float WallBounceReach = 40.0f;

	/** Speed (cm/s) imparted ALONG the wall normal — the push-off away from the wall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallBounce", meta = (ClampMin = "0.0", ClampMax = "1200.0"))
	float WallBounceLateralSpeed = 420.0f;

	/** Vertical speed (cm/s) imparted on a bounce. Near JumpLaunchVelocity so a bounce
	 *  reads as a real second jump rather than a nudge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallBounce", meta = (ClampMin = "0.0", ClampMax = "1500.0"))
	float WallBounceVerticalSpeed = 620.0f;

	/** Seconds before another bounce may fire. Stops a single press from chaining and
	 *  keeps a chimney climb to one kick per wall contact. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallBounce", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float WallBounceCooldown = 0.25f;

	/** Turn the body to face the launch direction on a bounce (cosmetic; the CMC's
	 *  orient-to-movement takes back over once the launch velocity is applied). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallBounce")
	bool bWallBounceReorientBody = true;

	/** Seconds after a kick during which the rebound is protected: Move() cancels the
	 *  into-the-wall component of input, and lateral air friction drops to
	 *  WallBounceReboundLateralFriction. 0 disables both.
	 *
	 *  This is the CROSSING-CRISPNESS knob, not WallBounceLateralSpeed. At 0.15 the kick
	 *  held 420 cm/s for the first 62 cm of a 117 cm chimney and then bled to 81 cm/s
	 *  under falling friction — the far half of every crossing was a drift, which reads
	 *  as "the bounce doesn't go far enough" even though 49/49 crossings connected
	 *  (2026-07-25 PawPrint). Raising launch speed cannot fix that shape: post-window
	 *  reach asymptotes at v0/FallingLateralFriction, so more speed just makes the
	 *  already-fast half faster. 0.25 carries ~105 of the 117 cm at full speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallBounce", meta = (ClampMin = "0.0", ClampMax = "0.6"))
	float WallBounceReboundTime = 0.25f;

	/** CMC FallingLateralFriction during the rebound window. The shipped 3.0 halves the
	 *  kick in ~0.1 s (PawPrint-measured 397 → 215 cm/s); 0 lets the launch actually
	 *  carry. Restored to ACatBase::MovementFallingLateralFriction on expiry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallBounce", meta = (ClampMin = "0.0", ClampMax = "8.0"))
	float WallBounceReboundLateralFriction = 0.0f;

	/** True while the post-kick rebound window is running (read by ACatBase::Move). */
	bool IsRebounding() const { return WallBounceReboundTimer > 0.0f; }

	/** Horizontal direction the last kick pushed AWAY from the wall (the wall normal). */
	FVector GetReboundDirection() const { return WallBounceReboundDir; }

	// ── Wall Attach (shared cling / scramble state) ─────────────────────
	// One state, two entry parameterisations:
	//   CLING    — RiseSpeed 0: catch (Vz pinned to 0) then a slow slide. The cat
	//              sticks where it lands and the player owns the tempo; without it a
	//              chimney demands the jump press land inside a ~0.1 s window after
	//              apex, which reads as "spamming jump to keep momentum" (Sean,
	//              2026-07-24, comparing against Jedi: Survivor).
	//   SCRAMBLE — RiseSpeed > 0: the budget decays through zero into the SAME slide,
	//              so a run-up climbs then settles. Verb 3 supplies its own detection
	//              (sprint entry speed, tall/tagged wall) and the top-out → mantle
	//              handoff; the state machine below already serves it.
	// NOT a movement-mode takeover: the cat stays in MOVE_Falling and this constrains
	// velocity per tick, so landing detection, the jump SM and the cushion all keep
	// working untouched. That is why it can be entered and left at any time.

	/** Master switch for the wall cling (the scramble entry is a separate verb switch). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallAttach")
	bool bEnableWallCling = true;

	/** Chest-probe reach (uu) for catching a wall to cling to. Slightly longer than the
	 *  bounce reach so arriving at a wall sticks before the kick probe would miss it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallAttach", meta = (ClampMin = "20.0", ClampMax = "100.0"))
	float WallClingReach = 46.0f;

	/** Minimum closing speed (cm/s) toward the wall to stick. Doubles as the anti-re-stick
	 *  guard: right after a kick the cat is moving AWAY from the wall it left, so that wall
	 *  fails this test by construction and only the wall being approached can catch. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallAttach", meta = (ClampMin = "0.0", ClampMax = "300.0"))
	float WallClingMinApproachSpeed = 20.0f;

	/** The "stick" beat: seconds with vertical velocity pinned to 0 before the slide
	 *  begins. This is the beat that makes the wall feel grabbed rather than grazed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallAttach", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float WallClingCatchTime = 0.15f;

	/** Downward slide speed (cm/s) once the catch expires. Slow enough to read as gripping
	 *  and losing purchase, fast enough that hanging forever is not an option. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallAttach", meta = (ClampMin = "0.0", ClampMax = "400.0"))
	float WallClingSlideSpeed = 70.0f;

	/** Hard cap (s) on a single attach before it releases to falling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallAttach", meta = (ClampMin = "0.1", ClampMax = "5.0"))
	float WallClingMaxTime = 1.4f;

	/** Cooldown (s) before another attach may start. Backstop for the one frame after a
	 *  kick where velocity has not yet flipped away from the wall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallAttach", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float WallAttachCooldown = 0.12f;

	/** Begin a wall attach. RiseSpeed/RiseTime are the SCRAMBLE parameters (0/0 = cling):
	 *  vertical velocity eases RiseSpeed → 0 across RiseTime, then the catch, then the
	 *  slide. Owner on detection; server via ACatBase::Server_SetWallAttach. */
	void StartWallAttach(const FVector& WallNormal, float RiseSpeed, float RiseTime);

	/** Release the attach. The cling is deliberately STICKY: steering away does NOT let
	 *  go (2026-07-25 Sean call). In a chimney the wall you are aiming at lies along the
	 *  held wall's own normal, so an away-release dropped the cat on the exact frame the
	 *  player expressed intent — holding D toward the far wall quit the cling before the
	 *  jump could consume it. Exits are now kick / timeout / wall lost / grounded /
	 *  grabbed only, matching the Celeste-Ori-Jedi wall-kick convention where direction
	 *  steers the arc and the jump press alone decides when to leave. */
	void EndWallAttach(EWallAttachEnd Reason);

	bool IsWallAttached() const { return TraversalState == ECatTraversalState::WallAttach; }

	/** Fires on a jump press while airborne. Returns true if a bounce was consumed —
	 *  the caller then skips the normal jump path. Local owner only; mirrors to the
	 *  server via ACatBase::Server_WallBounce (the grab/pivot prediction pattern). */
	bool TryWallBounce();

	/** Applies the bounce impulse. Runs on the owner (predicted) and on the server
	 *  (RPC mirror) from the same wall normal, so both copies launch identically. */
	void DoWallBounce(const FVector& WallNormal);

private:
	/** Airborne ledge detection for the locally controlled cat; starts the mantle on a hit. */
	void TryDetectMantle();

	/** Shared radial wall probe (the traversal detection library — mantle uses the
	 *  forward ray, the bounce uses all four). Returns the NEAREST wall-like hit. */
	bool ProbeWalls(float Reach, int32 NumDirections, FVector& OutPoint, FVector& OutNormal,
	                bool& bOutAnyHit) const;

	/** Why the last mantle-detection frame bailed — logged on CHANGE only (a sustained
	 *  reject would otherwise be a per-frame firehose) and pushed to the PawPrint
	 *  "MantleReject" channel so a session leaves a histogram instead of a hunch.
	 *  Shared with the wall bounce, which reuses the same chest-probe gates. */
	enum class ETraversalReject : uint8
	{
		None = 0, Disabled, Cooldown, NotAirborne, Grabbing, NoInput, NoFacing,
		NoWall, WallTooFlat, NoLip, LipNotFloor, LipTooLow, LipTooHigh, NoHeadroom
	};
	static const TCHAR* RejectToString(ETraversalReject R);
	static const TCHAR* WallAttachEndToString(EWallAttachEnd R);

	/** Records a reject reason: logs it if it changed, and always samples the channel. */
	void NoteReject(ETraversalReject R);

	ETraversalReject LastReject = ETraversalReject::None;

	/** Advance the takeover curve on owner and server alike. */
	void DriveMantle(float DeltaTime);

	/** Restore the CMC and clear state. bCompleted = reached the target (vs external abort). */
	void EndMantle(bool bCompleted);

	ACatBase* GetCat() const;

	ECatTraversalState TraversalState = ECatTraversalState::None;

	FVector MantleStart = FVector::ZeroVector;
	FVector MantleTarget = FVector::ZeroVector;
	float MantleDuration = 0.4f;
	float MantleElapsed = 0.0f;
	float CooldownTimer = 0.0f;

	/** Seconds before another wall bounce may fire (separate from the mantle cooldown —
	 *  a bounce into a ledge should be able to mantle immediately). */
	float WallBounceCooldownTimer = 0.0f;

	/** Post-kick rebound-protection window; see WallBounceReboundTime. */
	float WallBounceReboundTimer = 0.0f;
	FVector WallBounceReboundDir = FVector::ZeroVector;

	/** Restores the CMC's lateral air friction when the rebound window closes. */
	void EndReboundWindow();

	/** Cling detection for the locally controlled cat; starts an attach on a hit. */
	void TryWallCling();

	/** Applies the attach velocity constraint for one frame; also runs the exit checks. */
	void DriveWallAttach(float DeltaTime);

	// ── Wall Attach state ───────────────────────────────────────────────
	FVector AttachNormal   = FVector::ZeroVector;   // outward from the wall face
	float   AttachElapsed  = 0.0f;
	float   AttachRiseSpeed = 0.0f;                 // >0 = scramble entry
	float   AttachRiseTime  = 0.0f;
	float   WallAttachCooldownTimer = 0.0f;
};
