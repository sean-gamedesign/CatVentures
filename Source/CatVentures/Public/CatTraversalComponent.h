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
