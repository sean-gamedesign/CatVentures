# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

CatVentures is an Unreal Engine 5.7 multiplayer third-person cat game. There is a single C++ module (`CatVentures`) with a Blueprint layer on top. The primary C++ class is `ACatBase`, a multiplayer-ready character; alongside it the C++ layer provides the multiplayer framework (`ACatGameMode`/`ACatGameState`/`ACatPlayerState`/`ACatPlayerController`) and a Steam session backend (`UCatGameInstance`). The core gameplay loop is **"chaos"**: players smash Geometry-Collection props to fill a Chaos Meter, which triggers a cinematic match-end sequence and scoreboard. Most gameplay actors, the AnimBP, and all UI are Blueprints under `Content/`, edited live through the **VibeUE** MCP editor-control layer (see *VibeUE* below).

## Character Controller — current feel & tuning (reference)

The bullets below document the **current state** of the movement/camera/turn/jump feel — the values that shipped from each iteration pass and the reasoning behind them. Nothing here is locked; the controller is open to iteration (as of 2026-07-04 an AnimX-kit-informed pass is planned: quadruped IK port, predictive landing, additive layers — see the migration doc in the AnimXLowPolyCats project). The value of this section is *deliberateness*: know what the current numbers are and why before changing them, so feel changes are intentional rather than side effects. Known open bugs in the jump **landing animation**: the `Land_stop`→Idle snap and front-paw penetration (see *Jump State Machine → OPEN*).

- **Camera-relative movement**: `Move()` projects input onto the camera's yaw plane; the character orients toward movement via `bOrientRotationToMovement`. (Tank controls existed in an earlier iteration and were deliberately replaced — do not reintroduce them.)
- **Walk speed 400 cm/s** (`MovementMaxWalkSpeed` override on PrimeCatBase; the C++ default is 600 but the intended feel is 400 — the constructor-baking bug meant playtesting always happened at 400, so 400 was kept deliberately on 2026-06-09). PrimeCatBase overrides two *movement-component* values from their C++ defaults: this and the turn rotation rate (next bullet). Acceleration/friction/braking ship at C++ defaults.
- **Turn feel — updated 2026-06-19**: `MovementRotationRateYaw 200`°/s on PrimeCatBase (down from the 360 C++ default) for a weightier, *arcing* turn instead of an instant snap toward the movement vector. The locomotion **lean** banks the spine from the **signed angle between velocity and input acceleration** (`UpdateCosmeticInterpolation` section D — not the old yaw-rate signal), so the bank *holds through the turn* while the body catches up; wired to `Spine_2` **Pitch ×18** in ABP_Cat_V2 (accel/decel lean is on `Spine_2` Roll). This is still an **orient-to-movement** model (the cat faces its motion). A true aim-facing **strafe** rework (2D blendspace) is a known future option — NOT done; flagged for a later refactor.
- **Turn-in-place — procedural, added 2026-06-19**: when the cat is **idle** and the camera yaw diverges from the body past `TurnInPlaceThreshold` (35° on PrimeCatBase; 50 C++ default), `ACatBase::UpdateTurnInPlace` rotates the body toward the camera by the **shortest signed angle** at a capped rate (constexpr `TurnSpeedDegPerSec` 150, eases out via `FMath::FixedTurn`), with hysteresis (disengages within ~4°, so it can't wrap or chatter). It drives the **in-place** `BS1_Cat_Turn` blendspace (in the `Locomotion_v2` Turn state) via a signed `TurnRateAnim` (−1..+1) computed from the applied yaw rate, and sets `SpeedType=Turn` + `bGoTurn` (the latter replicated `COND_SkipOwner` so proxies enter the Turn state and read the replicated `TurnRateAnim`; the body rotation itself replicates through the CMC). The four turn clips (`A_Cat_Move_Turn-45/90_L/R`) have **`force_root_lock=True`** so they render as pure in-place footwork — without it, with the AnimBP on `RootMotionFromMontagesOnly` the clips' baked rotation plays in the pose *on top of* the procedural rotation and pops each loop. The AnimGraph's `DefaultSlot` node was also wired into the pose chain (it had been orphaned). This **replaces** a shelved root-motion-turn-**montage** attempt — discrete 45/90 montages couldn't keep pace with camera input (lagged seconds behind on fast turns, wrapped at ±180); the procedural model tracks continuously. Turn-speed/disengage/interp are constexpr in `UpdateTurnInPlace`; `TurnInPlaceThreshold` is the only exposed UPROPERTY.
- **Jump tuning — current values (platforming rework, 2026-06-18)**: `JumpLaunchVelocity 700`, `GravityScaleRising 2.0`, `GravityScaleApex 3.4`, `GravityScaleFalling 5.5`, `GravityScaleInterpSpeed 25`, `ApexVelocityThreshold 30`, `JumpAirControl 0.7`, `JumpMaxHoldTimeTuning 0.18`, `CoyoteTime 0.12`, `JumpBufferTime 0.15`, `JumpCooldown 0.05`, `LandRecoveryDuration 0.25`, `MinFallTransitionHoldTime 0.05` (reduced from 0.30 on 2026-06-20 — the 0.30 hold kept the cat in the apex pose through a fast descent and snapped the fall blendspace; this only gates the *anim* phase, not the physics arc), `HardLandSpeedThreshold 900`; added 2026-07-05 (header-only defaults, cosmetic — no PrimeCatBase mirrors): `LandPredictTimeMoving 0.08`, `LandPredictTimeStopping 0.12` (predictive-landing lookahead), `LandCushionMaxDip 10`, `LandCushionDipPerImpact 0.01`, `LandCushionFrequency 14`, `LandCushionDampingRatio 0.85` (+ `bEnableLandCushion`, see *Jump State Machine*). Yields apex ~125 cm in ~0.36 s rise. These are baked into the C++ header defaults **and** mirrored as redundant PrimeCatBase overrides (equal values) — if you change one, change both so they stay in sync. (`MinFallTransitionHoldTime` is now header-only — the redundant PrimeCatBase override was cleared 2026-06-20; setting an inherited C++ UPROPERTY default via the CDO does **not** propagate to spawned pawns, so change it in the header.) Coyote time + jump buffer are shipped (see *Jump State Machine*).

- **Additive idle-life layer — AnimX pass Step 3, 2026-07-06 (awaiting Sean's PIE feel check).** `ABP_Cat_V2`'s post-locomotion chain is now kit-ordered: spine lean → **ears/blink** (Apply Mesh Space Additive ← `AddEarsBlink` SM) → **breath** (Apply Additive, unchanged from June) → **aim** (Apply Mesh Space Additive ← `BS1_Cat_Aim` Blendspace *Evaluator*) → foot IK → out. Ears/blink SM: empty `Neutral` state (an empty state result is the additive identity — no contribution) → `Eyes_Blink` (`A_Cat_Add_Blink`) on `bPlayBlink` / `Ears_Twitch` (Blend-by-Int over `A_Cat_Add_Ears_1/2/3` by `IndexEars`) on `bPlayEarsTwitch`; 0.2 blends, automatic return at clip end, all four players **loop=False** (one-shots). C++ (`UpdateCosmeticInterpolation` B/B2): blink every 3–7 s, twitch every 6–12 s with no back-to-back clip repeat (kit CharBP config values; constexpr, seeded per-machine so cats don't sync), pulse 0.2 s. Aim: the kit bakes the **yaw sweep into the aim clips' timeline** — `AimBSYawTime` (yaw ±180°→0..1, 0.5 centered) scrubs the evaluator's NormalizedTime while `AimBSPitch` (±60°→−1..+1) is the blendspace axis; weight = existing `AlphaAimInterp` (fades 1→0 over speed 0→800). Head-look only tracks on the locally controlled cat (proxies have no ControlRotation → neutral head). Spine-incline additives (`A_Cat_Add_Incline`/`P_Squat`/`P_UpTail`) shipped with the Step 3b spine/incline block (see *Foot IK → Milestone 1b + Step 3b*, 2026-07-06).

All movement tuning UPROPERTYs are re-applied to the CMC in `ACatBase::BeginPlay` (the constructor bakes C++ defaults into the CMC *before* Blueprint serialization, so BeginPlay re-application is what makes PrimeCatBase overrides work — keep new tuning knobs on that list).

## Build & Development

**Generate project files** (required after adding/removing .h/.cpp files):
- Right-click `CatVentures.uproject` → *Generate Visual Studio project files*
- Or: `"C:\Program Files (x86)\Epic Games\Launcher\Engine\Binaries\Win64\UnrealVersionSelector.exe" -projectfiles "C:\Projects\CatVentures\CatVentures.uproject"`
- Note: UE 5.7 removed `GenerateProjectFiles.bat` from `Engine\Build\BatchFiles\`. Use `UnrealVersionSelector.exe` instead. It runs silently — no output on success.

**Build** (Development Editor config):
- Open `CatVentures.sln` in Visual Studio and build `CatVentures` target
- Or: `"C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" CatVenturesEditor Win64 Development "C:\Projects\CatVentures\CatVentures.uproject"`
- Live Coding (in-editor): `LiveCoding.Compile` via console, or Ctrl+Alt+F11. Requires the editor to be open.
- External build requires the editor to be **closed** — UE will refuse to build while Live Coding is active.

**Sandbox note for agent-driven builds:**
- The harness sandbox blocks execution of binaries under `C:\Program Files\...`. Build/regen invocations require `dangerouslyDisableSandbox: true` on the tool call. Reads (`Test-Path`, etc.) work without it.

**Run/Test:** Launch from the UE 5.7 Editor (PIE). For multiplayer tests, use *Play → Number of Players: 2* with *Net Mode: Play As Listen Server*.

There are no automated tests in this project. Verification is done via PIE.

**Logging:** project log category is `LogCatVentures` (declared in `Public/CatVenturesLog.h`, defined in `CatVentures.cpp` — include `CatVenturesLog.h` where you log). Verbose = per-tick spam, Log = state transitions, Warning = real problems. Do not add `LogTemp`, on-screen debug messages, or BP Print Strings to shipping paths — that cruft was purged in the 2026-06 cleanup pass.

## C++ Module Structure

```
Source/CatVentures/
  CatVentures.h/.cpp             — Module entry (LogCatVentures defined in the .cpp)
  Public/
    CatVenturesLog.h             — LogCatVentures category declaration (include this to log)
    CatBase.h                    — Core character class (all player systems declared here)
    CatAnimationTypes.h          — Native enums (ECatMoveType, ECatMovementStage, ECatJumpPhase)
    AnimNotifyState_SwatTrace.h  — Stateless CDO-safe AnimNotifyState for swat hit window
    InteractableInterface.h      — BlueprintNativeEvent UInterface for interactive objects
    InteractableLoot.h           — Concrete test interactable actor
    SeesawToy.h                  — Physics seesaw prop
    CatMatchTypes.h              — Match enums/structs (ECatMatchPhase, FChaosRewardData, ...)
    CatGameMode.h                — Server-authoritative match state machine + scoring
    CatGameState.h               — Replicated match state (chaos score, phase, scoreboard)
    CatPlayerState.h             — Per-player replicated state (bWantsRematch)
    CatPlayerController.h        — Pause menu + client-side match-end cinematics
    CatGameInstance.h            — Steam Online Subsystem session backend
    PauseMenuWidget.h            — Pause menu C++ base
  Private/
    (matching .cpp for each of the above)
```

Dependencies (Build.cs):
- **Public**: `Core, CoreUObject, Engine, InputCore, EnhancedInput, OnlineSubsystem, OnlineSubsystemUtils, UMG, Slate, SlateCore, GeometryCollectionEngine`
- **Private**: `Chaos`
- **DynamicallyLoaded**: `OnlineSubsystemSteam`

## Architecture: ACatBase

`ACatBase` is the only character class. Everything runs through it. Blueprint subclass `PrimeCatBase` (at `/Game/PrimeCatBase`) assigns input assets, montages, and tuning overrides via exposed UPROPERTYs.

### Networking Model

- **Tick() runs on ALL roles** — replicated gameplay state is derived server-side, cosmetic variables locally on every machine. (An earlier "hard tick gate" that early-returned on non-local instances no longer exists.)
- **Replicated gameplay state** (server-authoritative): `SpeedType`, `MovementStage`, `JumpPhase`, `bIsGrabbing`, `bGoTurn`, `TurnRateAnim` — each with an `OnRep_` callback where needed.
- **Local cosmetic variables** (NOT replicated): `Speed`, `AimYaw/AimPitch`, `LeanAmount`, `bHasMovementInput`, etc. — computed locally on every machine including simulated proxies, fed to the AnimBP.
- **The exact ABP consumption surface** (ABP_Cat_V2 polls these every update — do not rename without updating the ABP): `Speed`, `SpeedType`, `JumpPhase`, `NormalizedFallSpeed`, `AimYawInterp`, `AimPitchInterp`, `AlphaAimInterp`, `AlphaPlayBreathInterp`, `PlayRateInterp`, `TurnRateAnim`, `bGoTurn`, `LeanAmount`, `AimBSYawTime`, `AimBSPitch`, `bPlayBlink`, `bPlayEarsTwitch`, `IndexEars` (additive layer, 2026-07-06), `FootIKRot_HandL/HandR/FootL/FootR`, `SpineInclineF`, `SpineInclineS`, `UpTailAlpha`, `PelvisDropZ`, `ChestDropZ`, `SpineIKAlpha` (spine/incline block, 2026-07-06), `JumpRiseProgress` (standstill launch scrub, 2026-07-06); plus the `OnMeow` delegate. (`PlayRate` is a reserved source that is currently never written — `PlayRateInterp` always reads 0.)
- **Network initialization**: `PossessedBy` and `OnRep_PlayerState` both call `ForceWalkingMovementMode()` to prevent the "frozen client" problem.

### Input System

Enhanced Input (IMC_Cat mapping context), camera-relative (see *Character Controller* above):
- **Move**: WASD camera-relative on the yaw plane; character orients to movement
- **Look**: Mouse/stick rotates the spring arm / camera pitch-yaw (pitch clamped per tick)
- **Jump**: Space / Gamepad Face Bottom — `ACharacter::Jump/StopJumping` with variable-height hold
- **Swat**: LMB / RT — local-predicted montage + server-authoritative sphere sweep
- **Interact**: F / Gamepad Face Left — server-authoritative sphere trace
- **Grab**: mouth grab (IA_Grab) — server-authoritative physics-constraint tow

`IA_ToggleMenu` is bound on `ACatPlayerController` (pause menu), not on `ACatBase`.

### Jump State Machine

The jump uses asymmetric gravity (tunable `GravityScaleRising`, `GravityScaleApex`, `GravityScaleFalling`, interpolated by `GravityScaleInterpSpeed` to kill the Apex→Fall velocity spike). The `ECatJumpPhase` enum (None → Launch → Apex → Fall → Land) drives the AnimBP via `OnJumpPhaseChanged` delegate. `LandRecoveryTimer` enforces the Land phase duration; `JumpCooldownTimer` gates re-jump after landing.

**Jump anim — movement-aware set (Step 1, 2026-06-20, jump-feel pass; wiring INVERSION fixed 2026-07-05).** Each Jump phase state in `ABP_Cat_V2` (`Jump_Launch/Apex/Fall/Land`) is a Blend-Poses-by-Bool between the **Run** variant and the **InPlace** variant, driven by `Speed > 200` — so a standing hop and a running leap use coherent clip sets (and the matching `Land_stop`/`Land_run`). **Gotcha that bit for 2 weeks:** in `AnimNode_BlendListByBool`, **BlendPose_0 is the TRUE pose** — the set originally shipped with InPlace/stop on pose 0, so standstill jumps played the Run set and running jumps the InPlace set from 2026-06-20 until 2026-07-05 (found via live per-frame PIE sampling; it had masqueraded as the "`Land_stop`→Idle snap"). `A_Cat_Jump_Land_run` is **loop=False** (a one-shot land clip that loops pops back to its impact pose if the state outlives it) and `A_Cat_Jump_Land_stop` has **start_position 0.08** (skips the deep authored crouch — landing weight now comes from the cushion spring + foot IK, see below).

**Fall-pose axis is HEIGHT-ABOVE-GROUND (2026-07-05).** The fall blendspaces (`BS1_Cat_Fall_inPlace/run`, apex@0 / `Fall_low`@0.5 / apex@1 — symmetric grid, eval-grid rebuild verified via re-open+save) are fed `1 - NormalizedFallSpeed` (Subtract node in `Jump_Fall`/`Jump_Apex` — both states share the feed so Apex→Fall crossfades identical poses). `NormalizedFallSpeed` is **no longer a speed ratio** (at `GravityScaleFalling 5.5` any fall saturated it in ~0.2 s, sweeping every hop through the `Fall_low` skydive splay): `UpdateJumpPhase` probes the ground below each airborne frame — ≤150 cm above ground → N=1 (feed 0, gathered apex pose; all hops live here), ≥350 cm → N=0.5 (feed 0.5, full skydive), ramping between, so tall falls always gather over the last 1.5 m (the kit's `SetMaxHight` model).

**Predictive landing (AnimX pass Step 2, 2026-07-05).** The ABP consumes **`AnimJumpPhase`** (via `Anim_JumpPhase`), NOT the replicated `JumpPhase`: identical, except it anticipates **Land** while falling when `UpdateLandPrediction` (trace from feet along velocity, length `|V| ×` `LandPredictTimeMoving` 0.08 / `LandPredictTimeStopping` 0.12 — header-only defaults) predicts contact — so the land clip starts pre-impact (kit `bGoLand`). Gameplay (recovery timer, cooldown, gravity, foot-IK landing snap) stays anchored to the real `JumpPhase`. A `Jump_Land → Jump_Fall` escape transition (`Anim_JumpPhase == Fall`, 0.15 blend) backs out of a land pose if the prediction misses (slid off a lip); `Jump_Fall → Jump_Land` crossfade is 0.10. **Missed-phase robustness (2026-07-08):** the three transitions *into* `Jump_Land` (from Fall/Apex/Launch) fire on `Anim_JumpPhase == Land` **OR `== None`** — on a simulated proxy the replicated phase can skip the 0.25 s Land window (easiest around ramp landings), and `Jump_Fall`'s only exit used to be `== Land`, parking the proxy in the fall pose forever with foot IK vibrating the legs (the pawn's own state read a healthy idle the whole time). Any missed phase now funnels through the land state and its normal exits. Local cats never see `None` mid-jump and the anticipation coil holds `Launch`, so local feel is untouched. (Apex/Launch had the same latent trap — Apex has a legitimate C++ Apex→None ledge-catch path.)

**Landing cushion — mesh-Z spring (AnimX pass Step 4, 2026-07-05).** `Landed()` kicks a damped spring on the mesh's relative Z (`LandCushionDipPerImpact` 0.01 × impact |Vz|, clamp `LandCushionMaxDip` 10 cm, `LandCushionFrequency` 14 rad/s, `LandCushionDampingRatio` 0.85, kick ×0.4 when moving since foot IK is speed-gated off at a run; master switch `bEnableLandCushion`). `UpdateLandCushion` integrates in the cosmetic tick **before** `UpdateFootIK`, so the dip pushes the paws into the ground and the same-frame upward-only IK plants them — the body dips while the paws stay put and the legs read as springs absorbing the landing. Cosmetic + local, never replicated.

**Step 2 (procedural squash/stretch) was attempted and REVERTED — do not reintroduce as built.** It scaled the **Root** bone, which stretches the whole skeleton including the legs, dragging the paws through the ground on landing; it read as a "play-bow" stretch + paw penetration. Scaling the Root is the wrong rig for cat squash/stretch. (Also note: its `bEnableSquashStretch` toggle could never disable it from the Blueprint — see the foot-IK gotcha below.)

**Foot IK — RE-ENABLED 2026-07-05 at the AnimX-kit chain spec (AnimX controller pass, Step 1; PIE-confirmed same day: penetration gone, no warp).** `bEnableFootIK` C++ header default is **`true`**. The 2026-06-22 landing warp that got foot IK disabled was root-caused *then* as "engine Leg IK is unfit for the digitigrade legs" — VibeUE inspection of the kit's `AnimBP_Cat` IK layer (2026-07-05) **refined that**: the kit uses the *same* engine Leg IK warp-free; our warp came from the **chain definition** — we solved the front leg *through the paw joint* (IK goal `VB Hand`, NumBonesInLimb=4) and the back leg one bone too deep (n=4 past the Thigh). Kit shipping spec, now replicated in `ABP_Cat_V2`: **front = `VB Pastern` goal, FK `Pastern`, n=3** (Forearm/UpperArm/Shoulder; the Hand paw stays FK and rides the solved pastern), **back = `VB Foot` goal, n=3** (Hook/Shin/Thigh), MaxIterations 15, MinRotationAngle 3–5°. `UpdateFootIK` (C++): per-paw vertical offsets measured at the paw, fed to additive ModifyBones on the goal VBs; upward-only conform + swing-phase fade + over-extension guard + landing-frame alpha snap. **Gotcha (still true):** toggle `bEnableFootIK` via the **C++ header default**, not a BP override (inherited-default-doesn't-propagate). Full kit-math inventory in `Saved/.Aura/plans/quadruped-ik-port.md`.

**Milestone 1b + Step 3b — slope/spine system (built 2026-07-06; converged over a same-day fix pass with Sean in the loop; awaiting final feel check).** Final architecture — coarse-to-fine, each layer shrinking what the next must do:
1. **Whole-body slope pitch** (`MeshSlopePitch`, `UpdateLandCushion` §C): the mesh's relative rotation pitches to the fore/aft ground slope (two probes ±30 uu along facing, clamp ±25°, interp 6, composed in parent space ahead of the rig's −90° yaw via `MeshBaseRelRot`). This is THE slope mechanism — the Spline IK spine **cannot** pitch a quadruped (the hips aren't in its Spine→Spine_3 chain; bending it kinks at the spine root — that kink was the "weird back arch").
2. **Continuous mesh ground-conform** (`MeshGroundConformZ`, §B — kit HeightFixer role): the capsule contacts a slope uphill of center and floats the mesh a few cm; a center probe eases the mesh down (≤10 cm). Composed with the landing-cushion spring in one relative-transform write.
3. **Chest Spline-IK control point** (`ChestDropZ`, signed): the front legs hang off Spine_3, so a rising chest CP is what gives them slope reach; from the lowest front-paw ground-delta minus the conform. **Pelvis CP is held at 0** (`PelvisDropZ` writes 0) — back legs hang off the Pelvis bone the spline never moves, so a pelvis CP can't plant anything and only sags the back. Spline IK on the Mesh Ref Pose → Make Dynamic Additive → Apply Additive × `SpineIKAlpha` (MapClamped(Speed, 0..800 → 1..0)).
4. **Paw residual IK**: with 1–3 doing the coarse work, per-paw offsets return to the direct measurement ((ground+`FootIKPawHeight`)−paw) as SMALL residuals — the direct form's post-IK feedback (halves the correction) is invisible at ≤3 cm; it was only pathological when offsets carried the full slope delta (a terrain-delta model and an error integrator were both tried and rejected — see plan doc). **No swing fade** (both fade bases each break one slope direction); downhill reach clamped to the real terrain drop below the capsule plane (flat-ground lower bound 0 → the June "never yank a lifted paw down" protection holds by construction); over-extension guard + landing snap unchanged. **`FootIKAlpha` has NO speed taper** — the kit's 1.0..0.5-over-0..400 was tuned for its MaxWalkSpeed 250; at our 400 a walk sat at the floor and halved every offset (floating downhill paws / penetrating uphill paws **only while moving** — the diagnostic signature to remember).
5. **Paw rotation from the surface normal** (`FootIKRot_*`, mesh-component space, Roll=atan2(N.Y,N.Z) / Pitch=−atan2(N.X,N.Z), ±45° × local-ground fade, RInterp 30 → Rotation pins of the goal-VB ModifyBones, Additive/Component). Self-corrects toward 0 once the body pitch aligns the mesh.
6. **Spine incline additives**: `SpineInclineF` (1.0-neutral ±0.2) scrubs the 2 s `A_Cat_Add_Incline` Sequence Evaluator; `UpTailAlpha` = MapClamped(F, 1.0..1.2 → 0..0.3) → `A_Cat_Add_P_UpTail`; `SpineInclineS` = |lateral ±20 uu ×0.2| → `A_Cat_Add_P_Squat`.
Verified 20° ramp, facing uphill: all four paws 1.5–3.1 cm above surface (= paw height), mesh pitched −20 (rel-rot roll), offsets single-digit residuals.

**Multiplayer pass deltas (2026-07-06, commit `a1f3a70` — first 2-player test of all of the above):** (1) **turn-in-place body yaw never replicated** (orient-to-movement servers derive rotation from acceleration; a client's `SetActorRotation` at zero accel never leaves its machine) — the yaw now rides the turn RPCs (`Server_SetTurnActive/SetTurnRate` carry `BodyYaw`; throttle = rate Δ 0.05 OR 5° drift) and the server copy pursues it with **exponential** smoothing (`ClientTurnTargetYaw`, speed 12 — a fixed-rate pursuit stalls between RPC steps and jitters); (2) slope-stack experiments (dual-estimator pitch veto, idle pitch fraction, rise-only chest) were tried against a persistent "bow" and **REVERTED at session end (`e9fa937`)** — the bow's root cause was the railed accel-lean spring, and the layers cost leg reach (paws floated on the ramp). LIVE config = the approved single-probe full pitch + signed chest CP. If the known residual cases resurface (ramp-base phantom pitch; idle face-plant on steep slopes), see the handoff plan §6 before re-adding layers — the failure modes of each candidate fix are documented (paw-floor estimators feed back through the pose to ~2× the slope; idle fractions starve the Leg IK's reach); (4) **foot IK off during turn-in-place footwork** (`SpeedType == Turn`); (5) both cosmetic **springs integrate on a clamped step** (≤1/30 s) **with rail anti-windup** — a frame hitch made damping×dt > 1, exploding the accel-lean spring onto its ±1.5 rail with stored velocity = a PERMANENT forward bow ("stuck landing pose") on both machines; (6) **RESOLVED 2026-07-08:** `AccelLeanStiffness` is back at 120 on PrimeCatBase and the accel-lean spring is exonerated — the residual "stuck landing/fall pose" was the jump SM's missing `None` exit on simulated proxies (see *Predictive landing* above; live snapshots showed `AccelLeanAmount ≈ 0` on the stuck cat). Accel/decel lean feel Sean-approved same day. Still parked: simulated-proxy motion jitter at a run (pre-existing; standard `NetworkSimulatedSmooth*` tuning). Session gotchas: Live Coding reported success while old code kept running (verification rounds = editor-closed rebuilds ONLY); the editor-quit-via-Python TaskGraph assertion is harmless quit-path noise.

**RESOLVED landing-bug history (all closed 2026-07-05, PIE-confirmed):** the big "stretch/warp" was the mis-specced Leg IK chains (fixed by the kit-spec retarget — see *Foot IK*); **paw penetration** is fixed by foot IK + the cushion; the **"`Land_stop`→Idle snap"** (June's ~58-spread overshoot, then blamed on a rotation-blend candy-wrapper) was actually the **inverted Blend-by-bool** playing `Land_run`'s stride-extension end on standstill landings (see *Jump anim* above — live spread measured 66.7 ≈ `Land_run`@0.333); and the standstill **descent splay** was the Run-apex pose (37.3 vs InPlace 25.2) from the same inversion, compounded by the speed-saturating fall axis (now height-based). Diagnostic lesson: **live per-frame PIE sampling + single-variable isolation** (slate post-tick Python sampler reading pawn UPROPERTYs + live socket spreads) cracked in one session what pose-data archaeology circled for weeks. The three **Jump→Land** transitions remain Standard Blend (`Fall→Land` 0.10, others 0.05).

**Standstill jump — anticipation coil + scrubbed launch (2026-07-06, Sean-approved).** A grounded near-stationary jump (Speed ≤ 200) plays the authored crouch of `A_Cat_Jump_InPlace` (clip 0→0.51) for `JumpAnticipationDuration` (0.12 s, header-only tuning knob, 0 disables) **before** the physics `Jump()` fires — a real input-to-launch delay, deliberately standstill-only (running jumps stay instant for platforming). The Launch state's InPlace branch is a Sequence **Evaluator** scrubbed by `AnimJumpPhase`-gated `JumpRiseProgress` (0..0.425 = coil across the anticipation window, 0.425..1 = rise mapped over the measured 0.53 s full-hold rise **time** — deliberately TIME-uniform, not height-faithful: height-scrubbing compressed the push-off into an unreadable flash because the hold front-loads the height). Mechanics: `OnJumpInputPressed` arms `JumpAnticipationTimer` instead of jumping; the buffered retry is gated on it; expiry fires `Jump()`. **Measured jump heights (correcting the earlier docs): tap ≈ 125 cm / 0.36 s, FULL HOLD ≈ 240 cm / 0.53 s** — the hold nearly doubles the arc; anything tuned to "jump height" must use the held numbers. Fall-blendspace gather thresholds raised 150/350 → **260/460** so a held jump's apex stays in the gathered pose (150 was tap-era and blended the fall splay in at the top — the "apex leg kickout").

**OPEN — running jump launch anim is an ASSET GAP, not a tuning gap (2026-07-06).** `A_Cat_Jump_Run` is authored for the kit's shallow root-motion leap (layout via AnimPose sampling: extension@0 → tuck@0.15 → flying stretch@0.25–0.35 → descent → landing-reach@0.5+). A full scrub/remap iteration cycle (extension-first, gather-entry, longer inertial melt — all tried with Sean 2026-07-06) could not make it read on our steep 240 cm arc from arbitrary stride phases, and was **deliberately reverted to the original looping player @ rate 1** (Sean: iterated past the point of improvement). Next step when picked up: acquire/author a dedicated run-jump-start clip (~0.5 s: stride → push-off extension → tuck), or stride-phase-synced launch variants. Camera juice and any physics-arc/air-control change are also NOT done.

**Coyote time + jump buffer** (shipped 2026-06; tuning values listed under *Character Controller*):
- **Coyote time** (`CoyoteTimer`/`CoyoteTime`): re-armed every grounded frame; `CanJumpInternal` allows a jump shortly after walking off a ledge. Gated by `bLeftGroundByJumping` so it does **not** apply after an actual jump.
- **Jump buffer** (`JumpBufferTimer`/`JumpBufferTime`): the input is bound to `OnJumpInputPressed` (Started), which arms the timer; a per-frame retry in `UpdateJumpPhase` re-fires once the jump is legal. `OnJumped` clears the buffer → exactly one jump per press. `Landed()` calls `StopJumping()` so merely *holding* the button doesn't auto-bounce.
- **Stale-binary trap**: this logic lives in `CatBase.cpp`, so edits don't take effect in PIE until a `LiveCoding.Compile`. New `UPROPERTY`s showing up in reflection only proves the *header* compiled — verify the `.dll` mtime vs the `.cpp` if behavior seems unchanged.

### The Swat — Combat

Local prediction pattern: client plays montage immediately + fires `Server_Swat()` RPC. Server validates, then `Multicast_Swat()` plays the montage on all other machines. `Server_Swat` deliberately has NO `bIsSwatting` guard: the server's montage ends ~RTT/2 later than the client's, so a guard would eat legitimate rapid re-swats; a spam restart is cosmetic-only.

`UAnimNotifyState_SwatTrace` is a **stateless CDO** — holds zero mutable data. It delegates to `ACatBase::BeginSwatTrace / ProcessSwatTraceTick / EndSwatTrace`. All per-instance trace data (`SwatPreviousPawLocation`, `SwatAlreadyHitActors`) lives on `ACatBase`. `bIsSwatting` is reset exclusively via `FOnMontageEnded` (`OnSwatMontageEnded`) — not from `NotifyEnd` — to handle interruption safely. The server-side hit applies impulse + 1 point of `ApplyPointDamage`; receivers (BPC_ChaosItem) decide how to respond.

### Interaction System

`IInteractableInterface` (`BlueprintNativeEvent`). `TriggerInteract()` routes: listen-server host calls `PerformInteractTrace()` directly; clients fire `Server_Interact()` RPC. The trace uses `ECC_Visibility` and calls `IInteractableInterface::Execute_Interact(HitActor, this)`.

### Mouth Grab

`Server_Grab` → `Multicast_Grab` creates a dynamic `UPhysicsConstraintComponent` from the `socket_mouth` anchor to the grabbed body on **every machine's** local Chaos solver. `UpdateGrab()` auto-releases on drift past `MaxGrabDistance`. CMC swaps to drag settings (`DragWalkSpeed`, orient-to-movement off) while held; the client predicts the drag settings on press, and `Client_GrabFailed` rolls the prediction back when the server trace misses (otherwise a missed grab would leave the player slow until release). `bIsGrabbing` is replicated so the AnimBP can drive a jaw-open blend on all machines.

### Physics Bumper & Destruction

A forward-facing `UBoxComponent` (`PhysicsBumper`) pushes physics bodies (`BumperPushForce`) before the capsule reaches them. When the contacted body is a **Geometry Collection**, bumper contact is a **guaranteed full shatter** (`ForceShatterGC`) by design — there are no strain/threshold knobs on this path. See *Destruction* below.

### Tick Subsystems (called from `Tick`)

| Function | Runs on |
|---|---|
| `UpdateAnimationStates()` | All roles |
| `UpdateJumpGravity()` | Authority + autonomous proxy |
| `UpdateCosmeticInterpolation()` | Skipped on dedicated server |

## Match Flow: the Chaos loop

Server-authoritative match state machine living in `ACatGameMode`, replicated to clients via `ACatGameState`.

**The one and only GameMode Blueprint is `GM_CatVentures` (`/Game/Core/GM_CatVentures`)** — it is both the global default (DefaultEngine.ini) and the TestMap_02 World Settings override. It assigns `DT_ChaosRewards`, `PrimeCatBase` as the pawn, BP_CatPlayerController/BP_CatGameState, and `ChaosThreshold = 100`. (A duplicate `BP_CatGameMode` with a broken pawn class was deleted in the 2026-06 cleanup — don't recreate it.)

**Data-driven scoring** (do NOT hardcode prop scores):
1. A breakable prop (component Blueprint `BPC_ChaosItem` on `BP_Destructible_Base`) carries an `FName ChaosRewardKey` naming a row in the `DT_ChaosRewards` DataTable (rows are `FChaosRewardData`). The prop stores only the key, not a score.
2. On break (OnChaosBreakEvent → `Native_Shatter`) it calls `ACatGameMode::ReportItemDestroyed(Item, Location, Key)` (server only — the GameMode cast naturally fails on clients) and `RegisterDebrisActor` on the local PlayerController (every machine).
3. GameMode does `ChaosRewardTable->FindRow<FChaosRewardData>(Key)` to resolve `ChaosValue` + `DisplayName`. A missing/`None` key falls back to `DefaultChaosValue`.
4. `TotalChaosScore` accumulates and is pushed to `ACatGameState::ChaosScore` (replicated; `ChaosThreshold` is pushed once in `BeginPlay` so clients compute `GetChaosPercent()`).
5. At `TotalChaosScore >= ChaosThreshold`, GameMode latches `FinalBreakLocation`/`FinalBreakActor` and starts the match-end phase sequence.

**Phases** (`ECatMatchPhase`): `Playing → Warning` (slow-mo, movement stripped via `IMC_LookOnly`, look preserved) `→ FinalCut` (cinematic camera on break location) `→ Fade` (fade-to-black with a real-time settle hold) `→ Aftermath` (orbit/director-cut cameras over the densest debris cluster + scoreboard). GameMode drives transitions and calls `Client_OnMatchPhaseChanged(Phase, Location, TargetActor)` on every PlayerController — the RPC carries the value to dodge a property-replication race.

**Client cinematics** live in `ACatPlayerController`: `HandlePhase_*` methods plus `BlueprintImplementableEvent`s `OnMeowTimeTriggered`, `OnCinematicTakeover`, `OnShowScoreboard`. The Aftermath director frames the densest debris cluster (chunk-centroid math with abyss/floor filters); tunables are the `Match|Tuning` UPROPERTYs. Each client tracks its own broken props via `RegisterDebrisActor`. The orbit tick measures real elapsed world time (not a fixed 1/60 step).

**Rematch**: `ACatPlayerState::bWantsRematch` (set via `Server_SetRematchReady`, only honored during Aftermath); `ACatGameState::AllPlayersReadyForRematch()` gates the host's Play Again button **in WBP_RaidScoreboard** (Branch before `Host_ServerTravel("?Restart")`). `PlayerScores` is currently an MVP even-split of the total across `PlayerArray`.

## Destruction: Chaos Geometry Collections

Breakable props are Unreal **Geometry Collections** (`UGeometryCollectionComponent`) simulated by the **Chaos** solver. Note the name collision: the *physics* "Chaos" solver vs. the *gameplay* "Chaos score" above are different things.

Three break paths, all funneling into `ForceShatterGC` (deterministic, bypasses the asset's Damage Threshold) and fracturing **locally on every machine** via multicast:

1. **Physics Bumper** (C++): bumper contact with a GC → `Server_BumperHitGC` → `Multicast_BumperHitGC` → guaranteed shatter on every machine's solver.
2. **Swat accumulation** (BPC_ChaosItem): `OnTakePointDamage` (server) counts swats; the **4th swat** triggers `Multicast_TriggerShatter` → `ForceShatterGC`. Earlier swats apply `Multicast_ApplySwatImpulse` knockback (ShotFromDirection × 800, bVelChange).
3. **Hard impact** (BPC_ChaosItem): `OnComponentHit` with component velocity **> 600** (server) → `Multicast_TriggerShatter`.

Fracture is simulated **locally per client** (the trigger is multicast, not the resulting chunks), so debris differs slightly per machine — hence the Aftermath camera uses per-client `RegisterDebrisActor` lists, not replicated chunk transforms. `ApplyExternalStrain` is a no-op on plain Static Mesh Actors (no cluster graph). The swat count (4) and velocity threshold (600) are currently hardcoded in the BPC_ChaosItem graph.

## Steam Sessions (`UCatGameInstance`)

C++ session backend only — no UI or `ServerTravel` here. Uses Online Subsystem Steam (currently AppID 480 dev mode: lobbies + P2P). `UCatGameInstance` is assigned directly in DefaultEngine.ini — there is **no Blueprint GameInstance subclass**.

- BP-callable: `HostSession(MaxPlayers, bIsLAN)`, `FindSessions(MaxResults, bIsLAN)`, `GetFoundSessions()`, `JoinFoundSession(Result)`. Each completes via a BlueprintAssignable delegate (`OnHostSessionResult`, `OnFindSessionsComplete`, `OnJoinSessionResult`).
- **Re-hosting**: `DestroySession` is async — `HostSession` defers the create into `HandleDestroySessionComplete` when a session already exists. Never call `CreateSession` immediately after `DestroySession`.
- **Join → travel**: on success, `HandleJoinSessionComplete` resolves the connect string and calls `PC->ClientTravel(...)`, then broadcasts `OnJoinSessionResult` with the URL. BP must NOT re-issue `ClientTravel` — that caused a false "JOIN TRUE" with no actual travel.
- **Search key**: UE 5.7 OSS Steam routes `FindSessions` to the Steam lobby list only when the **`SEARCH_LOBBIES`** key (`Online/OnlineSessionNames.h`, value `"LOBBYSEARCH"`) is set. The old `SEARCH_PRESENCE` key was removed from the engine; hand-rolled FName strings silently fall through to the internet *server* query and find nothing. Always use the engine macro.
- Session settings use `bUsesPresence`, `bUseLobbiesIfAvailable`, `bAllowJoinViaPresence` (overlay "Join Game"). Overlay invites route through `HandleSessionUserInviteAccepted` → `JoinFoundSession`.
- `bForceLANMatch` is a debug-only toggle that forces the LAN path. Single shared session name: `"CatVenturesSession"`.
- **Session UI** lives in `WBP_MainMenu` (host: `HostSession` → on success → `Open Level TestMap_02?listen`; find: busy-guard + timeout safeguard + throbber → `FindSessions(20)`; rows in `WBP_ServerRow` call `JoinFoundSession`).

## VibeUE (Blueprint/asset editor-control layer)

The Blueprint and asset layer is edited live through the **VibeUE MCP server**, which controls the running UE 5.7 Editor (`http://127.0.0.1:8088/mcp`, registered as server `VibeUE` in repo-root `.mcp.json`). This is the established, active workflow — used to inspect `ABP_Cat_V2`, edit widgets, and tune Blueprint CDO defaults live.

- **Editor must be OPEN** — the server auto-starts with the Editor and dies when it closes. If tools are unreachable, open the Editor (it's not a code problem).
- **Auth**: every `tools/call` requires a valid API key. The Editor's startup log prints a "NO API key set / any local process can connect" SECURITY WARNING that is **misleading** — `initialize`/`tools/list` work keyless, but all real tool calls are gated. Key lives in `Saved/Config/VibeUE.ini` (UE side) + injected via `.mcp.json` (client side); both gitignored. Never commit or paste the key.
- **Tools**: `manage_asset` is the discovery/CRUD layer; `execute_python_code` is the deep-edit surface. The plugin registers rich Python services on the `unreal` module — **`unreal.BlueprintService`** is the node-level Blueprint API (`get_blueprint_info`, `get_nodes_in_graph`, `get_connections`, `get_node_details`, `connect_nodes`, `disconnect_pin`, `delete_node`, `add_function_call_node`, `add_cast_node`, `add_branch_node`, `set_node_pin_value`, `remove_variable`, `compile_blueprint`, ...). Also `WidgetService`, `AnimGraphService`, `DataTableService`, `ActorService`, etc. CDO defaults: `unreal.get_default_object(bp.generated_class())` + `get/set_editor_property`.
- **Limitations**: `add_function_call_node` cannot create custom-event *call* nodes (silent no-op) — rewire the caller's exec into the event's chain instead. `EdGraph.Nodes` is not readable via vanilla Python — use BlueprintService. When BP can't express something, add a `BlueprintCallable` C++ helper (e.g. `ACatPlayerController::PopulateScoreboard`).
- For BP/asset/AnimBP/widget/DataTable work, use VibeUE against the live Editor — don't hand-edit `.uasset` files or assume the change belongs in C++.

## Key Blueprint Assets (Content/)

- **`PrimeCatBase`** (`/Game/PrimeCatBase`) — Blueprint child of `ACatBase`. Assigns all input assets (IMC_Cat, IA_*), SwatMontage (AM_Cat_Swat), and tuning defaults. `MovementMaxWalkSpeed = 400` here is the intended feel (overrides the 600 C++ default).
- **`GM_CatVentures`** (`/Game/Core`) — the single GameMode BP (see *Match Flow*).
- **`ABP_Cat_V2`** — Animation Blueprint. Polls the 12-variable ABP consumption surface from the owning `ACatBase` (see *Networking Model*) and binds `OnMeow`.
- **`BPC_ChaosItem`** — ActorComponent on breakable props (`BP_Destructible_Base`). Carries `ChaosRewardKey`, owns the swat-count/impact shatter logic, reports destruction (see *Destruction*).
- **`DT_ChaosRewards`** (`/Game/Data`) — DataTable of `FChaosRewardData` rows (currently `Vase`, `TV`). Assigned on `GM_CatVentures`.
- **`WBP_*`** (`/Game/Blueprints/WBP` + `WBP_ChaosHUD` in `/Game/Blueprints`) — UI widgets: MainMenu (sessions), ServerRow, PauseMenu, MeowTime, RaidScoreboard (rematch gate), ScoreRow, ChaosHUD.
- **`AnimX`** asset pack — source animations under `Content/AnimX/`. (Its `CharBP_Base`/`AnimBP_Cat` are the original pack assets, not part of the game.)
- **`Content/Input/`** — `IMC_Cat` (+ `IMC_LookOnly`), `IA_Move`, `IA_Look`, `IA_Jump`, `IA_Meow`, `IA_Swat`, `IA_Interact`, `IA_Grab`, `IA_ToggleMenu`.

## Aura Plans

Design plans are stored in `Saved/.Aura/plans/` as Markdown files. These document the architecture, quirks, and step-by-step implementation decisions for each feature. Consult them when modifying existing systems. (Plans predating the 2026-06 cleanup may reference deleted scaffolding — trust this file and the code over old plans.)

## Common Gotchas

- **CDO trap**: `UAnimNotifyState` subclasses are CDOs shared across all skeleton instances. Never put mutable per-instance state in a `UAnimNotifyState`. Put it on the owning character.
- **CMC tuning trap**: actor-level tuning UPROPERTYs must be re-applied to the CMC in `BeginPlay` — constructor-only application silently ignores Blueprint overrides (this caused a 600→400 walk-speed bug). If you add a tuning knob, add it to the BeginPlay block.
- **`FOnMontageEnded` is non-dynamic**: Bind with `BindUObject`, not `AddDynamic`. Call `AnimInstance->Montage_SetEndDelegate(Delegate, Montage)`.
- **Listen server host**: Must bypass Server RPCs with `if (HasAuthority())` check before calling `Server_X()`, otherwise the RPC call is a no-op on the host and the action silently fails.
- **Replicated vs cosmetic split**: When adding new animation-driving variables, decide whether they need replication (gameplay-authoritative) or can be derived locally (cosmetic). Prefer local derivation for anything the AnimBP uses for blending. Don't add speculative replicated state — the 2026-06 cleanup deleted eight never-written replicated properties.
- **UHT after header changes**: Adding new UCLASS/UPROPERTY/UFUNCTION requires a full rebuild (not just incremental compile) if reflection data changes.
- **Two "Chaos"**: the physics solver (Geometry Collection fracture) and the gameplay "Chaos score" share the name but are unrelated. Don't conflate them.
- **Never hardcode prop scores**: chaos values are authored in `DT_ChaosRewards` rows, keyed by each prop's `ChaosRewardKey`. An unknown/`None` key silently scores `DefaultChaosValue`.
- **Engine OSS keys drift between versions**: `SEARCH_PRESENCE` existed in older engines and is gone in 5.7 (`SEARCH_LOBBIES` replaced it for the Steam lobby path). Always use the engine macros from `Online/OnlineSessionNames.h`, never hand-rolled FName strings — a wrong key fails silently.
- **BP/asset edits need the Editor open**: the VibeUE MCP server dies when the UE Editor closes. "Tools unreachable" almost always means "open the Editor," not a code change.
