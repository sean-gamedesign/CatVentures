# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

CatVentures is an Unreal Engine 5.7 multiplayer third-person cat game. There is a single C++ module (`CatVentures`) with a Blueprint layer on top. The primary C++ class is `ACatBase`, a multiplayer-ready character; alongside it the C++ layer provides the multiplayer framework (`ACatGameMode`/`ACatGameState`/`ACatPlayerState`/`ACatPlayerController`) and a Steam session backend (`UCatGameInstance`). The core gameplay loop is **"chaos"**: players smash Geometry-Collection props to fill a Chaos Meter, which triggers a cinematic match-end sequence and scoreboard. Most gameplay actors, the AnimBP, and all UI are Blueprints under `Content/`, edited live through the **VibeUE** MCP editor-control layer (see *VibeUE* below).

## ⚠️ Character Controls Are FINAL

The current movement/camera feel is designer-approved and locked:

- **Camera-relative movement**: `Move()` projects input onto the camera's yaw plane; the character orients toward movement via `bOrientRotationToMovement`. (Tank controls existed in an earlier iteration and were deliberately replaced — do not reintroduce them.)
- **Walk speed 400 cm/s** (`MovementMaxWalkSpeed` override on PrimeCatBase; the C++ default is 600 but the approved feel is 400 — the constructor-baking bug meant playtesting always happened at 400, so 400 was locked in deliberately on 2026-06-09). PrimeCatBase overrides two *movement-component* values from their C++ defaults: this and the turn rotation rate (next bullet). Acceleration/friction/braking ship at C++ defaults.
- **Turn feel — updated 2026-06-19** (designer sign-off): `MovementRotationRateYaw 200`°/s on PrimeCatBase (down from the 360 C++ default) for a weightier, *arcing* turn instead of an instant snap toward the movement vector. The locomotion **lean** banks the spine from the **signed angle between velocity and input acceleration** (`UpdateCosmeticInterpolation` section D — not the old yaw-rate signal), so the bank *holds through the turn* while the body catches up; wired to `Spine_2` **Pitch ×18** in ABP_Cat_V2 (accel/decel lean is on `Spine_2` Roll). This is still an **orient-to-movement** model (the cat faces its motion). A true aim-facing **strafe** rework (2D blendspace) is a known future option — NOT done; flagged for a later refactor.
- **Turn-in-place — procedural, added 2026-06-19** (designer sign-off): when the cat is **idle** and the camera yaw diverges from the body past `TurnInPlaceThreshold` (35° on PrimeCatBase; 50 C++ default), `ACatBase::UpdateTurnInPlace` rotates the body toward the camera by the **shortest signed angle** at a capped rate (constexpr `TurnSpeedDegPerSec` 150, eases out via `FMath::FixedTurn`), with hysteresis (disengages within ~4°, so it can't wrap or chatter). It drives the **in-place** `BS1_Cat_Turn` blendspace (in the `Locomotion_v2` Turn state) via a signed `TurnRateAnim` (−1..+1) computed from the applied yaw rate, and sets `SpeedType=Turn` + `bGoTurn` (the latter replicated `COND_SkipOwner` so proxies enter the Turn state and read the replicated `TurnRateAnim`; the body rotation itself replicates through the CMC). The four turn clips (`A_Cat_Move_Turn-45/90_L/R`) have **`force_root_lock=True`** so they render as pure in-place footwork — without it, with the AnimBP on `RootMotionFromMontagesOnly` the clips' baked rotation plays in the pose *on top of* the procedural rotation and pops each loop. The AnimGraph's `DefaultSlot` node was also wired into the pose chain (it had been orphaned). This **replaces** a shelved root-motion-turn-**montage** attempt — discrete 45/90 montages couldn't keep pace with camera input (lagged seconds behind on fast turns, wrapped at ±180); the procedural model tracks continuously. Turn-speed/disengage/interp are constexpr in `UpdateTurnInPlace`; `TurnInPlaceThreshold` is the only exposed UPROPERTY.
- **Jump tuning — LOCKED 2026-06-18** (platforming rework final). Locked values: `JumpLaunchVelocity 700`, `GravityScaleRising 2.0`, `GravityScaleApex 3.4`, `GravityScaleFalling 5.5`, `GravityScaleInterpSpeed 25`, `ApexVelocityThreshold 30`, `JumpAirControl 0.7`, `JumpMaxHoldTimeTuning 0.18`, `CoyoteTime 0.12`, `JumpBufferTime 0.15`, `JumpCooldown 0.05`, `LandRecoveryDuration 0.25`, `MinFallTransitionHoldTime 0.05` (reduced from 0.30 on 2026-06-20, designer sign-off — the 0.30 hold kept the cat in the apex pose through a fast descent and snapped the fall blendspace; this only gates the *anim* phase, not the physics arc), `HardLandSpeedThreshold 900`. Yields apex ~125 cm in ~0.36 s rise. These are baked into the C++ header defaults **and** mirrored as redundant PrimeCatBase overrides (equal values) — change neither without designer sign-off. (`MinFallTransitionHoldTime` is now header-only — the redundant PrimeCatBase override was cleared 2026-06-20; setting an inherited C++ UPROPERTY default via the CDO does **not** propagate to spawned pawns, so change it in the header.) Coyote time + jump buffer are shipped (see *Jump State Machine*).
- **Do not change** movement, camera, jump, or input behavior — including "fixes" that alter feel — without explicit designer sign-off.

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
- **The exact ABP consumption surface** (ABP_Cat_V2 polls these every update — do not rename without updating the ABP): `Speed`, `SpeedType`, `JumpPhase`, `NormalizedFallSpeed`, `AimYawInterp`, `AimPitchInterp`, `AlphaAimInterp`, `AlphaPlayBreathInterp`, `PlayRateInterp`, `TurnRateAnim`, `bGoTurn`, `LeanAmount`; plus the `OnMeow` delegate. (`PlayRate` is a reserved source that is currently never written — `PlayRateInterp` always reads 0.)
- **Network initialization**: `PossessedBy` and `OnRep_PlayerState` both call `ForceWalkingMovementMode()` to prevent the "frozen client" problem.

### Input System

Enhanced Input (IMC_Cat mapping context), camera-relative (see *Controls Are FINAL* above):
- **Move**: WASD camera-relative on the yaw plane; character orients to movement
- **Look**: Mouse/stick rotates the spring arm / camera pitch-yaw (pitch clamped per tick)
- **Jump**: Space / Gamepad Face Bottom — `ACharacter::Jump/StopJumping` with variable-height hold
- **Swat**: LMB / RT — local-predicted montage + server-authoritative sphere sweep
- **Interact**: F / Gamepad Face Left — server-authoritative sphere trace
- **Grab**: mouth grab (IA_Grab) — server-authoritative physics-constraint tow

`IA_ToggleMenu` is bound on `ACatPlayerController` (pause menu), not on `ACatBase`.

### Jump State Machine

The jump uses asymmetric gravity (tunable `GravityScaleRising`, `GravityScaleApex`, `GravityScaleFalling`, interpolated by `GravityScaleInterpSpeed` to kill the Apex→Fall velocity spike). The `ECatJumpPhase` enum (None → Launch → Apex → Fall → Land) drives the AnimBP via `OnJumpPhaseChanged` delegate. `LandRecoveryTimer` enforces the Land phase duration; `JumpCooldownTimer` gates re-jump after landing.

**Jump anim — movement-aware set (Step 1, 2026-06-20, jump-feel pass).** Each Jump phase state in `ABP_Cat_V2` (`Jump_Launch/Apex/Fall/Land`) is a Blend-Poses-by-Bool between the **InPlace** variant (false) and the **Run** variant (true), driven by `Speed > 200` — so a standing hop and a running leap use coherent clip sets (and the matching `Land_stop`/`Land_run`). The fall blendspaces (`BS1_Cat_Fall_inPlace/run`) are authored `Fall_low @0`, apex-pose `@1`, so they're fed **`1 - NormalizedFallSpeed`** (a Subtract node in `Jump_Fall`) — apex pose at the top, easing to `Fall_low` as it accelerates. `NormalizedFallSpeed` is computed **continuously while descending** in `UpdateJumpPhase` (not only in the Fall case) so the blendspace axis is correct the instant Fall begins. The **`Jump_Apex` state mirrors the `Jump_Fall` blendspace** (same `1 - NormalizedFallSpeed` feed into `BS1_Cat_Fall_inPlace/run`) instead of a standalone apex clip — so Apex→Fall is a crossfade between identical poses, killing the post-apex snap (2026-06-20).

**Step 2 (procedural squash/stretch) was attempted and REVERTED — do not reintroduce as built.** It scaled the **Root** bone, which stretches the whole skeleton including the legs, dragging the paws through the ground on landing; it read as a "play-bow" stretch + paw penetration. Scaling the Root is the wrong rig for cat squash/stretch. (Also note: its `bEnableSquashStretch` toggle could never disable it from the Blueprint — see the foot-IK gotcha below.)

**Foot IK gotcha (fixed 2026-06-20):** `bEnableFootIK` C++ header default is now **`false`**. A Blueprint override of an inherited C++ UPROPERTY default does **not** reliably propagate to the spawned pawn in this project, so the BP "off" never reached the pawn and foot IK (`UpdateFootIK` + the AnimGraph Leg IK nodes) was **silently running** on every landing — paws penetrating then conforming to the ground. The same gotcha applies to *any* inherited-default bool toggled on a BP child (it bit squash too): toggle these via the **C++ header default**, not the BP.

**OPEN / tracked for the next jump pass:** (1) **standstill landing still pops** — the land pose (`Land_stop`) doesn't match the idle stance and nothing anchors the paws (foot IK off), so the Land→Idle blend swings the unanchored legs through the ground; needs foot-locking/IK or matched clips, not more blend tweaks. (2) **"up doesn't match the anim"** — the variable-height hold keeps launch `Vz` ~constant for ~0.18 s (rises ~123 cm of the ~125 cm jump near-instantly), so no fixed-rate rise clip matches that rocket; consider play-rate scaling the launch/apex clips. Camera juice and any physics-arc/air-control change are NOT done.

**Coyote time + jump buffer** (shipped 2026-06, tuning LOCKED — see *Controls Are FINAL*):
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

- **`PrimeCatBase`** (`/Game/PrimeCatBase`) — Blueprint child of `ACatBase`. Assigns all input assets (IMC_Cat, IA_*), SwatMontage (AM_Cat_Swat), and tuning defaults. `MovementMaxWalkSpeed = 400` here is the locked feel (overrides the 600 C++ default).
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
