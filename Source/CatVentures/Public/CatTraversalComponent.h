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
	/** Wall-to-wall chimney transfer (round 8 of the kick saga, 2026-08-01): a cling
	 *  kick with an opposite wall in range becomes a TAKEOVER on the JumpDown clip's
	 *  own timeline — the capsule flies the clip's root arc (up the held wall, peel
	 *  over, cross late) over the clip's real duration while the full clip plays, and
	 *  ends in a cling on the far wall. Exists because seven procedural rounds proved
	 *  1.07 s of authored motion cannot fit a 0.30 s ballistic crossing. */
	WallTransfer,
};

/** Why an attach ended. Logged per release — the cling has several silent exits and
 *  "it let go and I do not know why" is exactly the class of bug that cost the
 *  2026-07-25 round (see the away-release note on EndWallAttach). */
enum class EWallAttachEnd : uint8
{
	Kick,       // consumed by a wall bounce — the intended exit
	Mantled,    // a ledge came into band while clinging; the mantle took over
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

	/** Chest-probe reach (uu) — how far away a ledge can be caught. ONE ray, aimed
	 *  down the cat's HEADING (held input, else actual travel) rather than actor
	 *  forward or radially.
	 *
	 *  Both alternatives were tried on 2026-07-25 and both are wrong. Forward-only
	 *  loses ledges the cat is drifting toward but not squarely facing — air control
	 *  and camera-relative input pull heading and facing apart constantly, and the
	 *  cling (4 rays, no input gate) would grab those walls instead, reading as "the
	 *  mantle just stopped working". Radial over-corrects: combined with a
	 *  directionless input gate it fired on walls beside and behind the cat, ~15x the
	 *  mantle rate. Aiming by heading is the narrow answer to the actual question —
	 *  "is there a ledge where I am going" — and it is why the intent test is the
	 *  probe direction itself rather than a filter applied afterward. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "20.0", ClampMax = "100.0"))
	float MantleReachDistance = 45.0f;

	/** Travel speed (cm/s) below which motion no longer counts as a heading when no
	 *  input is held. Raise to demand more commitment before a ledge catches; the cat
	 *  then needs the stick, not just drift, to mantle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "0.0", ClampMax = "300.0"))
	float MantleMinApproachSpeed = 20.0f;

	/** Ledge-lip height band ABOVE the capsule bottom (uu). Below min a step-up handles
	 *  it; above max the wall is a scramble/climb candidate, not a mantle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "10.0", ClampMax = "80.0"))
	float MantleMinLedgeHeight = 25.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "40.0", ClampMax = "200.0"))
	float MantleMaxLedgeHeight = 70.0f;   // 105 shipped v1 — Sean live-tuned: 103+ catches read superhero, 64–74 read cat

	/** How far past the wall face the lip probe starts (uu) — the paw-hook depth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "5.0", ClampMax = "50.0"))
	float MantleForwardClearance = 18.0f;

	/** Ledges at or below this height (uu) use the VAULT bucket (Ledge_050M: fores plant,
	 *  quick hind hop) instead of the deep-hang pull-up (Ledge_Climb_Up) — one clip cannot
	 *  serve both: the pull-up is authored for ~95 uu of climb at our scale and compresses
	 *  into a bunch at the lip on a knee-high step (frame-verified vs the pack showcase,
	 *  2026-07-30). Latched at StartMantle into bMantleVault; selects clip, capsule curve
	 *  and §B2 drop table together. */
	/** Raised 45 → 55 (2026-07-31): ledge 47 [climb] was the first MantlePawGapMin catch —
	 *  all four paws >25 uu from every surface for HALF the mantle. The deep-hang climb is
	 *  calibrated at ~68; 45–55 was a no-man's land where its composition overshoots a short
	 *  wall. The vault's walk-and-hop scales down to this band gracefully.
	 *  Raised 55 → 70 (2026-07-31, vault-for-all, Sean's call): a full session split
	 *  smooth-vs-scramble EXACTLY on the bucket — every [climb] read as leg-flapping, every
	 *  [vault] read smooth — so the vault composition now covers the whole detection band
	 *  and the deep-hang climb is parked until the per-height clip round (Ledge_100M). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "25.0", ClampMax = "70.0"))
	float MantleVaultMaxHeight = 70.0f;

	/** Takeover duration = base + height × per-cm (taller ledges pull longer).
	 *  Raised twice on Sean's feel passes (2026-07-30: 0.35/0.0015 → 0.5/0.0025 → 0.65/0.003):
	 *  a 68 uu climb now runs ~0.85 s ≈ 1.5× the authored clip rate, a 28 uu vault
	 *  ~0.73 s — same ratio both buckets. Much below ~1.3× reads rushed; much above
	 *  ~1 s of control lock starts fighting the platforming. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "0.1", ClampMax = "1.5"))
	float MantleBaseDuration = 0.65f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "0.0", ClampMax = "0.01"))
	float MantleDurationPerCm = 0.003f;

	/** Forward speed handed to the CMC at mantle exit — a soft step-out, the gait
	 *  earns the rest (the start-burst boundary lesson: bake no lunge). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "0.0", ClampMax = "400.0"))
	float MantleExitSpeed = 150.0f;

	/** Fraction of the mantle over which the body EASES to face the ledge. StartMantle
	 *  used to snap the yaw in one frame — the probe aims down the HEADING, which
	 *  camera-relative input + air control pull well away from the facing, so an
	 *  oblique catch teleport-rotated the cat under a camera that doesn't follow
	 *  (Sean's camera-positional "scramble" repro, 2026-07-31). ~0.3 of a 0.85 s
	 *  mantle ≈ a 0.25 s swing onto the ledge line. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Mantle", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float MantleFaceAlpha = 0.3f;

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

	/** Fraction of the ALONG-WALL velocity carried through a kick. The launch is
	 *  otherwise purely normal + up with an XY override, so every bit of forward speed
	 *  died at the kick and a wall run could never chain — you crossed an alley with
	 *  zero along-speed and the far wall could only give you a cling (2026-08-02).
	 *  Needs no per-verb gating: a cling and a scramble both hold their tangential
	 *  velocity at zero, so this term is zero for them and chimney bouncing is
	 *  unchanged. 0.75 of a ~640 run arrives at ~480, comfortably over the
	 *  WallRunMinLateralSpeed needed to run the far face too. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallBounce", meta = (ClampMin = "0.0", ClampMax = "1.2"))
	float WallBounceMomentumRetain = 0.75f;

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

	/** Seconds a CLING kick holds the grip after the jump press before the launch
	 *  fires — the standstill-jump anticipation doctrine applied to the wall. The
	 *  JumpDown clip spends its first ~0.13 s loading and pushing ON the wall; an
	 *  instant launch had the paws leaving on frame 1 while the body language said
	 *  "still pushing" — push-off in mid-air, Sean's "mechanical" (2026-07-31, the
	 *  mantle timeline lesson in kick form). Raw mid-air bounces stay instant: no
	 *  grip to hold, and latency there reads as lag. 0 disables (instant kicks).
	 *  0.13 → 0.22 round 6: the clip's load bottoms ~0.2 and pushes off ~0.22 — at
	 *  0.13 the launch fired mid-load and the spring frames played in mid-air. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallBounce", meta = (ClampMin = "0.0", ClampMax = "0.4"))
	float WallBounceAnticipation = 0.22f;

	// ── Wall Transfer (chimney crossing as a takeover) ──────────────────
	// The cling-kick crossing rides the clip's timeline, not ballistics: capsule on
	// the authored root arc, full clip playing, cling on the far wall at the seam.
	// Solitary-wall cling kicks (no opposite wall in range) keep the anticipation +
	// physics-launch path; raw mid-air bounces are untouched.

	/** Master switch — off restores the pure physics kick for all cling kicks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallTransfer")
	bool bEnableWallTransfer = true;

	/** Max face-to-face gap (uu) the transfer will cross. Beyond it a cling kick is a
	 *  plain physics launch. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallTransfer", meta = (ClampMin = "60.0", ClampMax = "600.0"))
	float WallTransferMaxGap = 250.0f;

	// (Round 20c: WallTransferClimb is GONE. The leap is an authored constant — see
	// the CatTransferArc namespace in the .cpp — so the climb is DERIVED from the gap
	// rather than dialled in, and the two can no longer disagree. Each transfer logs
	// its gap → climb pairing as the level-blocking spec.)

	// (Round 20b: WallTransferLaunchAngleDeg is GONE, and so are the arc-hump table
	// and WallTransferArcHeight before it — trap-knob doctrine. The launch is no
	// longer a number anyone picks: the spring clip's own push sets the exit
	// velocity, and the flight parabola starts on that exact vector, so the arc IS
	// the launch by construction. A knob here could only ever contradict the
	// animation. See DriveWallTransfer.)


	/** Seconds for the whole crossing = WallTransferPushTime (the authored wall
	 *  phase, played 1:1 by the WallKickClipTime scrub) + the flight. 1.13 keeps
	 *  round 17's approved ~0.33 s pounce. Change it together with the push time
	 *  or the flight tempo moves. */
	/** Playback rate for the whole crossing — THE speed dial (1.5 = half again as
	 *  fast, Sean 2026-08-02). Everything else is derived from it and the clip
	 *  lengths below, so the anim scrub and the capsule timeline cannot drift apart:
	 *  the clip time advances at this rate while the wall/flight windows shrink by
	 *  it, which is the same total in CLIP seconds either way. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallTransfer", meta = (ClampMin = "0.25", ClampMax = "4.0"))
	float WallTransferPlayRate = 1.5f;

	/** The asset durations, in seconds of ANIMATION. These are facts about
	 *  A_Cat_Wall_Spring_* / A_Cat_Wall_Sail_*, not tuning — re-author the clips and
	 *  these follow (as does the subtract literal in the ABP's WallKick graph). */
	static constexpr float kSpringClipLen = 1.2333f;
	static constexpr float kSailClipLen   = 0.4333f;

	float GetWallTransferPushTime() const
	{
		return kSpringClipLen / FMath::Max(WallTransferPlayRate, 0.1f);
	}
	float GetWallTransferDuration() const
	{
		return (kSpringClipLen + kSailClipLen) / FMath::Max(WallTransferPlayRate, 0.1f);
	}
	/** Rate the ABP evaluators' scrub must advance at (read by ACatBase::Tick). */
	float GetWallClipPlayRate() const { return FMath::Max(WallTransferPlayRate, 0.1f); }

	// (The procedural transfer-pitch knobs — PitchScale, PitchMax, CatchPitchDeg —
	//  are GONE with the layer itself. The clip carries the launch attitude now.)

	/** Capsule stand-off from the target face at the catch (uu). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallTransfer", meta = (ClampMin = "10.0", ClampMax = "60.0"))
	float WallTransferStandOff = 25.0f;

	// (WallTransferMidClearance is GONE, round 15 — the ballistic linear lateral
	// departs the held wall at push-off, so a clearance floor has nothing to do and
	// a knob whose only correct value is "redundant" is a trap for a later session.)

	// (WallTransferPushTime and WallTransferDuration are GONE as knobs — they were
	//  always "the clip length" and "clip + sail", so storing them separately from
	//  the assets was a desync waiting to happen. Both are now derived above from
	//  the clip lengths ÷ WallTransferPlayRate.)



	/** Begin the transfer takeover (owner on detection; server via the pawn RPC).
	 *  Deterministic from the same params on both machines — the mantle model. */
	void StartWallTransfer(const FVector& InStart, const FVector& InTarget,
		const FVector& InTargetNormal, bool bKickRight);

	bool IsWallTransferring() const { return TraversalState == ECatTraversalState::WallTransfer; }

	/** 0→1 along the crossing — read by the wall-hug (§B3) to keep paw conform alive
	 *  through the spring-up phase while the cat is still at the held wall. */
	float GetWallTransferProgress() const;

	/** Fraction of the crossing spent ON the wall (push time ÷ duration) — the §B3
	 *  hug gate reads this so the paw conform lives through the whole wall phase
	 *  regardless of how the push window is tuned. */
	float GetWallTransferPushFrac() const
	{
		return FMath::Clamp(GetWallTransferPushTime() / FMath::Max(GetWallTransferDuration(), 0.1f), 0.1f, 0.85f);
	}


	/** Seconds since the current wall attach began (0 when not attached) — read by the
	 *  §B3 catch window so a fresh grip builds its hug at catch speed. */
	float GetWallAttachElapsed() const { return TraversalState == ECatTraversalState::WallAttach ? AttachElapsed : 0.0f; }

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

	/** dot(input, into-wall) that counts as asking for the wall when there is no closing
	 *  SPEED to measure — 0.5 is a 60° cone. A wall you are pressed against eats your
	 *  input, so a standing jump up one produces almost no horizontal velocity (measured
	 *  9–14 cm/s against a 20 gate) and could never catch. Only honoured while not
	 *  separating, so it cannot defeat the anti-re-stick guard above. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallAttach", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float WallClingPressDot = 0.5f;

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

	/** Vertical speed (cm/s, either sign) above which the cling defers to a mantle the
	 *  cat is already closing on.
	 *
	 *  The lip band is measured from the capsule bottom, so it is a window the cat
	 *  passes THROUGH, and either direction of travel can be closing on it:
	 *      rising  + LipTooHigh -> the rise walks the lip down into band
	 *      falling + LipTooLow  -> the fall walks the lip up into band
	 *  In both cases the cling's CATCH beat pins Vz to 0 and cancels exactly the
	 *  motion that was about to arm the mantle. Shipping only the rising half (the
	 *  first pass) left the overshoot case wide open — a sprint jump that clears the
	 *  ledge reads LipTooLow on the way down and got caught on that very frame
	 *  (2026-07-25 telemetry: 650 cm/s jump, one frame of LipTooLow at 16.220, cling
	 *  START at 16.220, then 1.4 s of sliding past the ledge to a timeout).
	 *  Deliberately narrow: a wall with no ledge reports NoWall/NoLip and still
	 *  catches normally, so chimney bouncing is untouched. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallAttach", meta = (ClampMin = "0.0", ClampMax = "400.0"))
	float WallClingLedgeSuppressSpeed = 50.0f;

	/** Cooldown (s) before another attach may start. Backstop for the one frame after a
	 *  kick where velocity has not yet flipped away from the wall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallAttach", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float WallAttachCooldown = 0.12f;

	/** Exponential ease-out speed (1/s) of the body turn onto the wall face while
	 *  attached — wrap-safe FInterpTo on the signed yaw delta. Ease-OUT (not a
	 *  constant rate, round 6 — that read robotic): a re-cling out of a kick arrives
	 *  already rotating from the kick sweep, and this settles the remainder onto the
	 *  face instead of re-accelerating. Ordinary approaches arrive already facing the
	 *  wall, so it is a near no-op there. 12 converges a large angle in ~0.25 s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallAttach", meta = (ClampMin = "1.0", ClampMax = "50.0"))
	float WallAttachFaceInterpSpeed = 12.0f;

	// ── Vertical Scramble (verb 3) ──────────────────────────────────────
	// Run at a tall climbable wall fast enough and the cat scrambles UP it: the same
	// WallAttach state entered with a rise budget instead of a downward slide, so it
	// inherits every exit for free — jump kicks off, the budget decays into the cling's
	// slide, and a lip entering band at the top hands off to the mantle (that handoff
	// is the "detection runs during an attach" path added 2026-07-25).
	//
	// Unlike every other traversal verb this one is GATED BY LEVEL AUTHORING (Sean's
	// call): not every wall is climbable. Two sources, OR'd — a tag on the actor or
	// component for per-surface precision, and a tagged volume for "everything in this
	// shaft climbs" without touching meshes. The CLING is deliberately left ungated, so
	// an ordinary wall still means hang-and-kick and only a marked wall means climb.

	/** Master switch for the vertical scramble. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Scramble")
	bool bEnableWallScramble = true;

	/** Tag marking a climbable surface. Put it on the ACTOR or on an individual
	 *  primitive COMPONENT — component tags let one mesh of a multi-part actor climb
	 *  while the rest do not. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Scramble")
	FName ScrambleSurfaceTag = FName(TEXT("Scrambleable"));

	/** Tag marking a volume inside which EVERY wall is climbable. Broad override for
	 *  blocking out a climbable shaft or alley without tagging individual meshes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Scramble")
	FName ScrambleVolumeTag = FName(TEXT("ScrambleZone"));

	/** Probe reach (uu) for finding a climbable wall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Scramble", meta = (ClampMin = "20.0", ClampMax = "100.0"))
	float ScrambleReach = 46.0f;

	/** Closing speed (cm/s) toward the wall required to start a climb. Sits in the
	 *  sprint band so a walk into a wall just stops — the run-up IS the entry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Scramble", meta = (ClampMin = "0.0", ClampMax = "800.0"))
	float ScrambleMinEntrySpeed = 450.0f;

	/** Unbroken wall face (uu) required above the contact point. Stops a run-up at a
	 *  low block — that is a mantle, and the mantle detector gets first refusal anyway. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Scramble", meta = (ClampMin = "40.0", ClampMax = "400.0"))
	float ScrambleMinWallHeight = 120.0f;

	/** Initial climb speed (cm/s), mapped from entry speed across the band below and
	 *  decaying linearly to zero over ScrambleRiseTime — so height gained is
	 *  RiseSpeed × RiseTime / 2 and a faster run-up climbs higher. Gait
	 *  differentiation for free, the same trick the weighty stops use. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Scramble", meta = (ClampMin = "100.0", ClampMax = "1500.0"))
	float ScrambleRiseSpeedMin = 500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Scramble", meta = (ClampMin = "100.0", ClampMax = "1500.0"))
	float ScrambleRiseSpeedMax = 775.0f;

	/** Entry speed at which the rise budget reaches ScrambleRiseSpeedMax (sprint top). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Scramble", meta = (ClampMin = "100.0", ClampMax = "1200.0"))
	float ScrambleEntrySpeedForMaxRise = 650.0f;

	/** Seconds the rise budget takes to decay to zero. Height scales with BOTH this and
	 *  the speed, so prefer lengthening the climb over speeding it up — a short fast
	 *  burst reads as a pop, a longer decay reads as clawing up the wall. Note
	 *  WallClingMaxTime is charged from the END of this, so lengthening the climb does
	 *  not eat the hang budget.
	 *
	 *  Tuning history (all Sean PIE reads, measured not estimated — the achieved climb
	 *  tracks RiseSpeed × RiseTime / 2 within ~4%, since Vz is rewritten every frame and
	 *  gravity only gets one frame to bite): 750 over 0.55 s = ~200 uu peak, too low;
	 *  900 over 0.85 s = ~370 uu peak, too high; ships 775 over 0.75 s = ~290 uu peak.
	 *  Height was taken out of the DURATION rather than the speed — a slower climb over
	 *  the same 0.85 s read sluggish, where a shorter one stays punchy. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Scramble", meta = (ClampMin = "0.1", ClampMax = "2.0"))
	float ScrambleRiseTime = 0.75f;

	// ── Wall run (verb 6 — PHASE 1, mechanics on placeholder anim) ──────
	// Arrive at a marked wall fast but OBLIQUELY and the cat carries its momentum
	// along the face instead of stopping. The discriminator was already sitting in
	// TryDetectScramble as a rejection ("skimming along the face, not running into
	// it") — a head-on arrival is a scramble, a glancing one is a run, and they
	// share the speed gate, the authoring tag, the wall-height probe and the whole
	// exit set. No new state: WallAttach with a lateral budget (see AttachRunSpeed).
	//
	// NO ANIM YET, by the mechanics-first doctrine that has served every verb here:
	// this runs on the cling pose so the MOVE can be judged before any clip work.
	// There is no wall-run clip in the pack's 285 sequences; the plan if it earns
	// one is the ground run cycle rolled 90 deg about its forward axis.

	/** Master switch. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallRun")
	bool bEnableWallRun = true;

	/** Speed ALONG the face (cm/s) needed to run rather than just slap into it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallRun", meta = (ClampMin = "100.0", ClampMax = "900.0"))
	float WallRunMinLateralSpeed = 320.0f;

	/** Minimum closing speed (cm/s) — the cat must actually be ARRIVING at the wall,
	 *  not sprinting past it a metre away. Deliberately low: the whole point is that
	 *  this fires below the scramble's head-on threshold. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallRun", meta = (ClampMin = "10.0", ClampMax = "400.0"))
	float WallRunMinApproachSpeed = 20.0f;   // 60 first pass — with the velocity-aimed
	                                         // probe replaced by a lateral fan, a genuinely
	                                         // parallel run has almost no closing speed and
	                                         // 60 rejected the best case. Proximity is
	                                         // already proven by the probe hitting at all.

	/** How long the lateral budget lasts (s). Speed is HELD for WallRunHoldFrac of it
	 *  and then decays to zero, so distance ≈ speed × time × (1 − HoldFrac/2) rather
	 *  than the speed × time / 2 a decay-from-the-start gives — nearly double the reach
	 *  for the same duration, and it reads as keeping your momentum and then losing it
	 *  instead of bleeding out from the first frame. Height is held for the same window;
	 *  the cling's own slide takes over after, which is the sag. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallRun", meta = (ClampMin = "0.2", ClampMax = "2.5"))
	float WallRunTime = 1.1f;

	/** Fraction of the budget spent at full speed before the decay starts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallRun", meta = (ClampMin = "0.0", ClampMax = "0.9"))
	float WallRunHoldFrac = 0.6f;

	/** Entry lift (cm/s) and how long it lasts. NOT cosmetic — a wall run entered at
	 *  floor level re-lands within a couple of frames and the attach's grounded exit
	 *  kills it in ~0.2 s of a 0.9 s budget (measured 2026-08-02: every run died that
	 *  way, which read as both "hard to trigger" and "doesn't travel far"). You do not
	 *  wall-run along a skirting board; you rise onto the wall. Reuses the scramble's
	 *  rise machinery, so the vertical profile becomes lift → hold → sag. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallRun", meta = (ClampMin = "0.0", ClampMax = "600.0"))
	float WallRunRiseSpeed = 250.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallRun", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float WallRunRiseTime = 0.35f;

	/** Fraction of the arriving lateral speed carried into the run. 1.0 = momentum is
	 *  conserved; below that the wall costs you something to use. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|WallRun", meta = (ClampMin = "0.3", ClampMax = "1.5"))
	float WallRunSpeedScale = 1.0f;

	// ── Balance assist (verb 4, and deliberately NOT a verb) ────────────
	// Walking a fence rail or a block-wall top is NOT a mode. It began as one — speed
	// clamped to a creep, input projected onto the edge, position magnetised to the
	// centreline — and the telemetry indicted it: 97% of frames pinned within 3 uu of
	// centre at a mean speed of 96. Sean: "it's not like I'm playing a balancing game,
	// it's just locking the cat into a forward or backward slow walk." The mode took
	// away speed, steering and lateral position and gave nothing back.
	//
	// What ships instead is an ASSIST with no state at all: detect a narrow surface,
	// apply a gentle lateral correction, and otherwise leave the cat entirely alone.
	// Three things fall out of that.
	//   1. NO REPLICATION SURFACE. The correction derives purely from world geometry and
	//      the pawn's own position, so owner and server compute it identically — no RPC,
	//      no mirrored enter/exit edges, none of the "dropped edge strands the server"
	//      failure mode every other verb had to design around.
	//   2. IT COMPOSES. Sprinting along a wall, skidding to a stop on one, pivoting on
	//      one all just work, using systems already tuned. The mode had to suppress all
	//      four by clamping speed under their entry thresholds — which was also the only
	//      place a traversal verb contended with the grounded CMC precedence chain.
	//   3. FALLING OFF IS TEXTURE, not a tax. It costs a climb back up, occasionally.
	// The assist must stay well under the cat's own lateral authority (~50 uu/s against
	// a 400 walk) or it is just the rail again with extra steps.

	/** Master switch for the balance assist. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Balance")
	bool bEnableBalanceAssist = true;

	/** Half-width (uu) of the lateral scan. MUST clear half of FenceMaxSurfaceWidth by a
	 *  healthy drift margin: the flank samples have to fall past the surface even when
	 *  the cat is walking well off the centreline, or a flank lands ON the surface, the
	 *  "both sides dropping" test fails, and balance mode rejects. Shipped 45 against a
	 *  60 max width — incoherent, since a 60-wide surface spans ±30 and left only 15 uu
	 *  of margin; drifting ~23 off a 45-wide rail was enough to break detection
	 *  (2026-07-25). Keep this at roughly FenceMaxSurfaceWidth × 1.3 or more. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Balance", meta = (ClampMin = "10.0", ClampMax = "200.0"))
	float FenceProbeHalfWidth = 80.0f;

	/** Samples across the scan (odd, so one lands on the centre). More samples resolve
	 *  the supported span and therefore the centreline more precisely — and the scan is
	 *  wide, so a coarse step would quantise the magnet's target badly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Balance", meta = (ClampMin = "3", ClampMax = "21"))
	int32 FenceProbeSamples = 13;

	/** Widest supported span (uu) that still counts as a balance surface. Above this the
	 *  cat is just on a wide ledge and walks normally. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Balance", meta = (ClampMin = "5.0", ClampMax = "150.0"))
	float FenceMaxSurfaceWidth = 60.0f;

	/** How far the ground must fall away past the edge (uu) to count as a drop rather
	 *  than a step down. Stops a kerb or a low step reading as a fence. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Balance", meta = (ClampMin = "20.0", ClampMax = "400.0"))
	float FenceMinDropDepth = 60.0f;

	/** Centreline pull: lateral correction speed per uu of offset (1/s), capped by
	 *  FenceAssistMaxSpeed. Deliberately far under the cat's own lateral authority so
	 *  steering off a fence always wins — this catches drift, it does not hold a line. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Balance", meta = (ClampMin = "0.0", ClampMax = "20.0"))
	float FenceAssistStrength = 4.0f;

	/** Hard cap (uu/s) on the correction. Against a 400 walk this is the difference
	 *  between an assist and a rail; the first build capped at 60 and pinned the cat
	 *  within 3 uu of centre 97% of the time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Balance", meta = (ClampMin = "0.0", ClampMax = "300.0"))
	float FenceAssistMaxSpeed = 50.0f;

	/** How quickly lateral velocity blends toward the correction. Blended rather than
	 *  set, so the cat's own sideways motion is never simply erased. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Balance", meta = (ClampMin = "0.5", ClampMax = "30.0"))
	float FenceAssistBlendRate = 6.0f;

	/** Scale the assist by how narrow the surface is: full strength on a rail, fading to
	 *  nothing as the span approaches FenceMaxSurfaceWidth. A wall top wide enough to
	 *  stand on comfortably should not feel assisted at all. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Balance")
	bool bScaleAssistByNarrowness = true;

	/** Radius (uu) of the ring of support samples that derives the surface axis. Wants
	 *  to sit outside the capsule but well inside the shortest fence worth walking. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal|Balance", meta = (ClampMin = "10.0", ClampMax = "120.0"))
	float FenceAxisProbeRadius = 30.0f;

	/** Begin a wall attach. RiseSpeed/RiseTime are the SCRAMBLE parameters (0/0 = cling):
	 *  vertical velocity eases RiseSpeed → 0 across RiseTime, then the catch, then the
	 *  slide. Owner on detection; server via ACatBase::Server_SetWallAttach. */
	void StartWallAttach(const FVector& WallNormal, float RiseSpeed, float RiseTime,
	                     float RunSpeed = 0.0f, float RunTime = 0.0f, float RunSign = 0.0f);

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
	 *  (RPC mirror) from the same wall normal + kick side, so both copies launch
	 *  identically and pick the same twist clip. */
	void DoWallBounce(const FVector& WallNormal, bool bKickRight);

private:
	/** Airborne ledge detection for the locally controlled cat; starts the mantle on a hit. */
	void TryDetectMantle();

	/** Shared radial wall probe (the traversal detection library). Rays fan out from
	 *  BasisDir at 90° steps; zero BasisDir means actor forward. The bounce and the
	 *  cling take all four from forward — they answer "is there a wall near me". The
	 *  mantle takes ONE along the heading — it answers "is there a ledge where I am
	 *  going", which is a different question and must not be widened into the first
	 *  (2026-07-25: four rays plus a directionless input gate made it fire ~15x too
	 *  often). Returns the NEAREST wall-like hit. */
	bool ProbeWalls(float Reach, int32 NumDirections, FVector& OutPoint, FVector& OutNormal,
	                bool& bOutAnyHit, const FVector& BasisDir = FVector::ZeroVector,
	                FHitResult* OutHit = nullptr) const;

	/** Run-at-a-climbable-wall detection; starts a scramble on a hit. Local owner only. */
	void TryDetectScramble();

	/** Name the wall run's bail-out (logged on change only). A verb that silently fails
	 *  to detect is invisible in a dump — "it will not trigger" has cost a round on two
	 *  verbs now, so every gate says which one it was. */
	void LogWallRunReject(const TCHAR* Reason) const;
	mutable const TCHAR* WallRunRejectLast = nullptr;

	/** One downward support sample: is there floor at Probe within FenceMinDropDepth of
	 *  FloorZ, or has the ground fallen away? */
	bool IsSupportedAt(const FVector& Probe, float FloorZ) const;

	/** Derives the surface's long axis from a ring of support samples in WORLD
	 *  directions — deliberately independent of the cat's own rotation. Probing across
	 *  the actor's right vector instead produced a 1-2 frame enter/exit oscillation,
	 *  because balance mode rotates the cat and so moved its own probe. */
	bool FindBalanceAxis(FVector& OutAxis) const;

	/** Scans across the given axis's perpendicular for a narrow supported span. Returns
	 *  the centre offset (signed, along that perpendicular) and the measured span. */
	bool ProbeBalanceSurface(const FVector& Axis, float& OutCentreOffset, float& OutSpan) const;

	/** Stateless per-tick balance help on a narrow surface. Owner AND server run it —
	 *  identical inputs (world geometry + the pawn's position) give identical results,
	 *  which is why this needs no RPC and cannot desync. */
	void UpdateBalanceAssist(float DeltaTime);

	/** Is this surface climbable? Tag on the hit actor OR the hit component, or the cat
	 *  standing inside a volume tagged ScrambleVolumeTag. Evaluated only after a wall
	 *  probe has already hit, so the overlap query is rare rather than per-frame. */
	bool IsScrambleSurface(const FHitResult& Hit) const;

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

	/** Height of the last lip the mantle probe measured, above the capsule bottom (uu).
	 *  The cling reads it to decide whether a rise could ever bring an out-of-band ledge
	 *  into range, instead of assuming it always will. */
	float LastLipHeight = 0.0f;

	/** Advance the takeover curve on owner and server alike. */
	void DriveMantle(float DeltaTime);

	/** Advance the wall-transfer curve on owner and server alike. */
	void DriveWallTransfer(float DeltaTime);

	/** Restore the CMC and either catch the far wall (completed) or drop to falling. */
	void EndWallTransfer(bool bCompleted);

	FVector TransferStart   = FVector::ZeroVector;
	FVector TransferTarget  = FVector::ZeroVector;
	FVector TransferNormalB = FVector::ZeroVector;   // far wall's outward normal
	float   TransferElapsed = 0.0f;

	/** Yaw swing across the crossing (round 10): the clip contributes only its PUSH
	 *  segment — its back half is dismount acrobatics (a ~75° head-down peel-and-dive
	 *  authored for arcing over a wall's TOP edge) that rendered as a mid-air
	 *  somersault on a rising chimney crossing. The visible turn is instead the actor
	 *  yaw sweeping StartYaw → StartYaw+SwingAngle on a SmoothStep of transfer
	 *  progress, over the normal gathered airborne pose. Signed along the chosen
	 *  shoulder, never shortest-path. */
	float TransferStartYaw   = 0.0f;
	float TransferSwingAngle = 0.0f;

	/** Restore the CMC and clear state. bCompleted = reached the target (vs external abort). */
	void EndMantle(bool bCompleted);

	ACatBase* GetCat() const;

	ECatTraversalState TraversalState = ECatTraversalState::None;

	FVector MantleStart = FVector::ZeroVector;
	FVector MantleTarget = FVector::ZeroVector;
	float MantleDuration = 0.4f;
	float MantleElapsed = 0.0f;

	/** Height bucket latched at StartMantle (mirrors ACatBase::bMantleVault). */
	bool bMantleIsVault = false;

	/** Yaw ease latched at StartMantle (the oblique-catch snap fix, 2026-07-31):
	 *  each machine latches its OWN current yaw and eases to the shared ledge-facing
	 *  yaw over MantleFaceAlpha of the mantle — a few degrees of transient owner/server
	 *  divergence that converges by construction. */
	float MantleStartYaw = 0.0f;
	float MantleTargetYaw = 0.0f;

	/** Deferred exit step-out: EndMantle used to read GetCurrentAcceleration on the
	 *  exit frame, but Move() input is suppressed until the mantle ends, so the
	 *  acceleration was ALWAYS zero and MantleExitSpeed never applied — every mantle
	 *  ended in a dead stop and running exits re-accelerated from 0 (the "jitter at
	 *  the end of the long jump", PawPrint 2026-07-31: Speed 0 → ramp on every END).
	 *  The window lets input flow for a tick or two; the first tick with real
	 *  acceleration gets the step-out along the INPUT direction. */
	float MantleExitBoostTimer = 0.0f;

	float CooldownTimer = 0.0f;

	/** Seconds before another wall bounce may fire (separate from the mantle cooldown —
	 *  a bounce into a ledge should be able to mantle immediately). */
	float WallBounceCooldownTimer = 0.0f;

	/** Post-kick rebound-protection window; see WallBounceReboundTime. */
	float WallBounceReboundTimer = 0.0f;
	FVector WallBounceReboundDir = FVector::ZeroVector;

	/** Pending cling-kick (owner only): armed at the jump press while attached, fires
	 *  DoWallBounce + the RPC when it expires (WallBounceAnticipation later). The clip
	 *  starts at the press; the grip holds through the load-and-push beat. Cancelled if
	 *  a grab, mantle or landing ends the attach first. */
	float   PendingKickTimer  = 0.0f;
	FVector PendingKickNormal = FVector::ZeroVector;
	bool    bPendingKickRight = false;

	/** Shoulder the last kick twisted over — neutral-input kicks alternate from this,
	 *  which is what gives a chimney climb its natural L/R rhythm. */
	bool bLastKickRight = false;

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
	/** Lateral budget — the WALL RUN entry (2026-08-02). Third use of this one state:
	 *  cling enters with no budget, scramble with a VERTICAL one, a wall run with a
	 *  LATERAL one, and all three share the exits (kick / mantle / timeout / wall lost
	 *  / grounded) and the same deterministic MP mirror. Sign selects which way along
	 *  the face; the tangent itself is derived from the normal so both machines get
	 *  the same vector from the same RPC. */
	float   AttachRunSpeed = 0.0f;                  // >0 = wall-run entry
	float   AttachRunTime  = 0.0f;
	float   AttachRunSign  = 0.0f;                  // +1 / −1 along Up × Normal
	float   WallAttachCooldownTimer = 0.0f;

	/** Normal of the wall a timeout just released, so the same face cannot be re-gripped
	 *  immediately. Without this the timeout meant nothing: the player is still holding
	 *  into the wall, air control supplies closing speed, and the cat re-attached ~0.12 s
	 *  later — the "detaches and then reattaches" hitch. Cleared once the cat is grounded,
	 *  kicks off, or mantles, so a chimney (alternating normals) is unaffected. */
	FVector SpentWallNormal = FVector::ZeroVector;

	/** Actor Z at attach start — the climb height actually achieved, which is the number
	 *  the rise budget must be tuned against (the intended figure is arithmetic, the
	 *  achieved one is what gravity and the CMC actually delivered). */
	float   AttachStartZ = 0.0f;
};
