# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

CatVentures is an Unreal Engine 5.7 multiplayer third-person cat game. There is a single C++ module (`CatVentures`) with a Blueprint layer on top. The primary C++ class is `ACatBase`, a multiplayer-ready character. Most gameplay actors and the AnimBP are Blueprints under `Content/`.

## Build & Development

**Generate project files** (required after adding/removing .h/.cpp files):
- Right-click `CatVentures.uproject` → *Generate Visual Studio project files*
- Or: `"C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\GenerateProjectFiles.bat" CatVentures.uproject -Game`

**Build** (Development Editor config):
- Open `CatVentures.sln` in Visual Studio and build `CatVentures` target
- Or: `"C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" CatVenturesEditor Win64 Development "C:\Projects\CatVentures\CatVentures.uproject"`

**Run/Test:** Launch from the UE 5.7 Editor (PIE). For multiplayer tests, use *Play → Number of Players: 2* with *Net Mode: Play As Listen Server*.

There are no automated tests in this project. Verification is done via PIE.

## C++ Module Structure

```
Source/CatVentures/
  Public/
    CatBase.h                    — Core character class (all systems declared here)
    CatAnimationTypes.h          — All native enums (ECatMoveType, ECatJumpPhase, etc.)
    AnimNotifyState_SwatTrace.h  — Stateless CDO-safe AnimNotifyState for swat hit window
    InteractableInterface.h      — BlueprintNativeEvent UInterface for interactive objects
    InteractableLoot.h           — Concrete test interactable actor
  Private/
    CatBase.cpp
    AnimNotifyState_SwatTrace.cpp
    InteractableLoot.cpp
```

Dependencies (Build.cs): `Core, CoreUObject, Engine, InputCore, EnhancedInput`.

## Architecture: ACatBase

`ACatBase` is the only character class. Everything runs through it. Blueprint subclass `PrimeCatBase` assigns input assets and montages via exposed UPROPERTYs.

### Networking Model

- **Hard tick gate**: `Tick()` early-returns on non-locally-controlled instances — Blueprint tick logic only runs where it matters.
- **Replicated gameplay state** (server-authoritative): `SpeedType`, `CurrentAction`, `ControlMode`, `MovementStage`, `JumpPhase`, etc. — each has an `OnRep_` callback.
- **Local cosmetic variables** (NOT replicated): `Speed`, `AimYaw`, `LeanAmount`, `bHasMovementInput`, etc. — computed locally on every machine including simulated proxies, fed directly to the AnimBP.
- **Network initialization**: `PossessedBy` and `OnRep_PlayerState` both call `ForceWalkingMovementMode()` to prevent the "frozen client" problem.

### Input System

Enhanced Input (IMC_Cat mapping context) with tank controls:
- **Move**: W/S moves along `ActorForward`, A/D yaw-rotates the character (not camera-relative)
- **Look**: Mouse/stick rotates the spring arm / camera pitch-yaw
- **Jump**: Space / Gamepad Face Bottom — uses `ACharacter::Jump/StopJumping` with variable-height hold
- **Swat**: LMB / RT — local-predicted montage + server-authoritative sphere sweep
- **Interact**: F / Gamepad Face Left — server-authoritative sphere trace

### Jump State Machine

The jump uses asymmetric gravity (tunable `GravityScaleRising`, `GravityScaleApex`, `GravityScaleFalling`). The `ECatJumpPhase` enum (None → Launch → Apex → Fall → Land) drives the AnimBP via `OnJumpPhaseChanged` delegate. `LandRecoveryTimer` enforces the Land phase duration; `JumpCooldownTimer` gates re-jump after landing.

### The Swat — Combat

Local prediction pattern: client plays montage immediately + fires `Server_Swat()` RPC. Server validates, then `Multicast_Swat()` plays the montage on all other machines.

`UAnimNotifyState_SwatTrace` is a **stateless CDO** — holds zero mutable data. It delegates to `ACatBase::BeginSwatTrace / ProcessSwatTraceTick / EndSwatTrace`. All per-instance trace data (`SwatPreviousPawLocation`, `SwatAlreadyHitActors`) lives on `ACatBase`. `bIsSwatting` is reset exclusively via `FOnMontageEnded` (`OnSwatMontageEnded`) — not from `NotifyEnd` — to handle interruption safely.

### Interaction System

`IInteractableInterface` (`BlueprintNativeEvent`). `TriggerInteract()` routes: listen-server host calls `PerformInteractTrace()` directly; clients fire `Server_Interact()` RPC. The trace uses `ECC_Visibility` and calls `IInteractableInterface::Execute_Interact(HitActor, this)`.

### Tick Subsystems (called from `Tick`)

| Function | Runs on |
|---|---|
| `UpdateAnimationStates()` | All roles |
| `UpdateJumpGravity()` | Authority + autonomous proxy |
| `UpdateCosmeticInterpolation()` | Skipped on dedicated server |

## Key Blueprint Assets (Content/)

- **`PrimeCatBase`** — Blueprint child of `ACatBase`. Assigns all input assets (IMC_Cat, IA_*), montages (SwatMontage), and tuning defaults.
- **`ABP_Cat_V2`** — Animation Blueprint. Reads replicated state and local cosmetic variables from the owning `ACatBase` cast. Drives blendspaces and the jump state machine.
- **`AnimX`** asset pack — source animations under `Content/AnimX/`.
- **`Content/Input/`** — `IMC_Cat` (mapping context), `IA_Move`, `IA_Look`, `IA_Jump`, `IA_Meow`, `IA_Swat`, `IA_Interact`.

## Aura Plans

Design plans are stored in `Saved/.Aura/plans/` as Markdown files. These document the architecture, quirks, and step-by-step implementation decisions for each feature. Consult them when modifying existing systems.

## Common Gotchas

- **CDO trap**: `UAnimNotifyState` subclasses are CDOs shared across all skeleton instances. Never put mutable per-instance state in a `UAnimNotifyState`. Put it on the owning character.
- **`FOnMontageEnded` is non-dynamic**: Bind with `BindUObject`, not `AddDynamic`. Call `AnimInstance->Montage_SetEndDelegate(Delegate, Montage)`.
- **Listen server host**: Must bypass Server RPCs with `if (HasAuthority())` check before calling `Server_X()`, otherwise the RPC call is a no-op on the host and the action silently fails.
- **Replicated vs cosmetic split**: When adding new animation-driving variables, decide whether they need replication (gameplay-authoritative) or can be derived locally (cosmetic). Prefer local derivation for anything the AnimBP uses for blending.
- **UHT after header changes**: Adding new UCLASS/UPROPERTY/UFUNCTION requires a full rebuild (not just incremental compile) if reflection data changes.
