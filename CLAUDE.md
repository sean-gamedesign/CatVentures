# CLAUDE.md

## Specific Instructions

**THESE ARE THE GOLDEN RULES. They outrank every other instruction in this file.**

Before changing code explain what you've discorved in your investigations. I want to know whats up before you make edits.

**Precedence.** Parts of this file are GENERATED — currently the `<!-- BEGIN VibeUE -->` … `<!-- END VibeUE -->` block at the bottom, written by `VibeUE.GenerateAgentConfig` and overwritten wholesale on every re-run (so never edit inside it, and never put anything you want to keep there). Generated guidance is a *reference for how to drive the tools*, not a mandate about how to work with me. **Wherever it conflicts with this section, this section wins.** Three conflicts exist today and are resolved as follows:

- It says *"execute multi-step tasks straight through — don't pause"*. **No.** Report findings and wait for my go before making edits, per the rule above. The checkpoint is the point.
- It says *"commit at milestones"*. **No.** Commit only when I ask.
- It says *"append gotchas to this file"* as you go. **No.** This file and everything in `Docs/` are curated; propose additions and let me decide.

If a future generated block adds a directive that contradicts this section, follow this section and tell me about the conflict.

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

CatVentures is an Unreal Engine 5.8 multiplayer third-person cat game (upgraded from 5.7 on 2026-08-14 — see *Build & Development*). There is a single C++ module (`CatVentures`) with a Blueprint layer on top. The primary C++ class is `ACatBase`, a multiplayer-ready character; alongside it the C++ layer provides the multiplayer framework (`ACatGameMode`/`ACatGameState`/`ACatPlayerState`/`ACatPlayerController`) and a Steam session backend (`UCatGameInstance`). The core gameplay loop is **"chaos"**: players smash Geometry-Collection props to fill a Chaos Meter, which triggers a cinematic match-end sequence and scoreboard. Most gameplay actors, the AnimBP, and all UI are Blueprints under `Content/`, edited live through the **VibeUE** MCP editor-control layer (see `Docs/tooling.md`).

## Documentation Map — read the owning doc BEFORE you edit

The deep knowledge for each system lives in `Docs/`, split out of this file on 2026-08-16 (it had grown past the 155k limit). **These are not optional background.** Each doc is the accumulated record of what was tried, what shipped, what was reverted and why — this is a feel-driven project where the same fix has been re-derived, re-broken and re-reverted more than once. Read the doc that owns a system *before* touching it, not after something reads wrong.

| Doc | Owns | Read it before |
|---|---|---|
| `Docs/movement-feel.md` | Character Controller — gaits/sprint, turn feel, turn-in-place, moving pivot, weighty starts/stops, skid, camera weight, jump tuning values, the M5 clip batch | any change to movement or camera feel, a tuning knob, a locomotion blendspace, or an authored locomotion clip |
| `Docs/catbase.md` | `ACatBase` — networking model + the exact ABP consumption surface, input, jump state machine, predictive landing, landing cushion, foot IK + the slope/spine system, swat, interaction, mouth grab, physics bumper, tick subsystems | editing `CatBase.h`/`.cpp`, renaming anything the AnimBP reads, or adding replicated state |
| `Docs/traversal.md` | `UCatTraversalComponent` — mantle/clamber, wall bounce, wall attach (cling + vertical scramble), wall transfer, balance assist, the traversal anim batch, detection logging | any traversal verb, any `ProbeWalls` caller, or a new movement-mode takeover |
| `Docs/match-destruction.md` | Match phase machine, data-driven Chaos scoring, match-end cinematics, rematch gate, Geometry Collection break paths | GameMode / GameState / PlayerController match code, or breakable props |
| `Docs/multiplayer-steam.md` | `UCatGameInstance`, SteamSockets transport config, session UI, packaging + the 2-PC test protocol | session code, net-driver config, or packaging a build for a multiplayer test |
| `Docs/tooling.md` | VibeUE MCP workflow and its serialization traps; PawPrint runtime telemetry | any Blueprint / AnimBP / widget / asset edit driven through the editor, or adding telemetry |

**Cross-references.** Italic `see *Section Name*` pointers in this file and inside the docs refer to section titles that may now live in a sibling doc — use the table above to find the owner. `## Common Gotchas` stays in this file because it cuts across every system.

**Where new knowledge goes.** A lesson about one system belongs in that system's doc, not here. This file keeps only what is true regardless of which system you are in.

## Build & Development

**Generate project files** (required after adding/removing .h/.cpp files):
- Right-click `CatVentures.uproject` → *Generate Visual Studio project files*
- Or: `"C:\Program Files (x86)\Epic Games\Launcher\Engine\Binaries\Win64\UnrealVersionSelector.exe" -projectfiles "C:\Projects\CatVentures\CatVentures.uproject"`
- Note: UE 5.7 removed `GenerateProjectFiles.bat` from `Engine\Build\BatchFiles\`. Use `UnrealVersionSelector.exe` instead. It runs silently — no output on success.

**Engine version: UE 5.8** (upgraded 2026-08-14 on branch `chore/ue58-upgrade`; UE_5.7 kept installed until the upgrade is committed). The bump needed exactly **one** code-side change: `BuildSettingsVersion.V6` → **`V7`** in both `Source/CatVentures.Target.cs` and `CatVenturesEditor.Target.cs`. That is not optional — on an installed engine the Editor target shares build products with `UnrealEditor`, which 5.8 builds with `ReturnType`/`Dangling`/`UnreachableCode` promoted to **Error**, and UBT hard-refuses a target that differs. After that: 76 TUs, zero warnings, **zero C++ changes**. `IncludeOrderVersion` is deliberately left at `Unreal5_7` (backward-compatible, warns only; include-order changes are their own class of breakage and belong in their own pass).

**Build** (Development Editor config):
- Open `CatVentures.sln` in Visual Studio and build `CatVentures` target
- Or: `"C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" CatVenturesEditor Win64 Development "C:\Projects\CatVentures\CatVentures.uproject"`
- Live Coding (in-editor): `LiveCoding.Compile` via console, or Ctrl+Alt+F11. Requires the editor to be open.
- External build requires the editor to be **closed** — UE will refuse to build while Live Coding is active.

**Sandbox note for agent-driven builds:**
- The harness sandbox blocks execution of binaries under `C:\Program Files\...`. Build/regen invocations require `dangerouslyDisableSandbox: true` on the tool call. Reads (`Test-Path`, etc.) work without it.

**Run/Test:** Launch from the UE 5.8 Editor (PIE). For multiplayer tests, use *Play → Number of Players: 2* with *Net Mode: Play As Listen Server*.

**Packaging:** `PackageDevelopmentBuild.bat` (UAT BuildCookRun → `ArchivedBuilds\Windows\`). It hardcodes `UE_ROOT`, which **must** track the engine version — it sat at `UE_5.7` after the 5.8 upgrade, which would have packaged a 5.8 project with the old engine's UAT (fixed 2026-08-15). A full Development package takes ~3 min with a warm DDC and produces ~1.1 GB.

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
    PawPrintSubsystem.h          — PawPrint in-memory runtime telemetry (Docs/tooling.md)
    CatTraversalComponent.h      — Traversal verbs component (Docs/traversal.md)
  Private/
    (matching .cpp for each of the above)
```

Dependencies (Build.cs):
- **Public**: `Core, CoreUObject, Engine, InputCore, EnhancedInput, OnlineSubsystem, OnlineSubsystemUtils, UMG, Slate, SlateCore, GeometryCollectionEngine`
- **Private**: `Chaos`
- **DynamicallyLoaded**: `OnlineSubsystemSteam`

## Common Gotchas

- **CDO trap**: `UAnimNotifyState` subclasses are CDOs shared across all skeleton instances. Never put mutable per-instance state in a `UAnimNotifyState`. Put it on the owning character.
- **CMC tuning trap**: actor-level tuning UPROPERTYs must be re-applied to the CMC in `BeginPlay` — constructor-only application silently ignores Blueprint overrides (this caused a 600→400 walk-speed bug). If you add a tuning knob, add it to the BeginPlay block.
- **`FOnMontageEnded` is non-dynamic**: Bind with `BindUObject`, not `AddDynamic`. Call `AnimInstance->Montage_SetEndDelegate(Delegate, Montage)`.
- **Listen server host — Server RPCs are NOT a no-op on the host** (corrected 2026-08-11, BB-13/BB-22; the old rule here claimed the opposite and was wrong). On authority UE resolves a `FUNC_NetServer` call to `FunctionCallspace::Local` and runs it in place. Which of the two shapes to use is decided by **what the server function does**: an **action RPC** (the server function *is* the work — traces, spawns, multicasts: `Server_Swat`, `Server_Interact`, `Server_Grab`/`Server_ReleaseGrab`) is called **unconditionally**; a **mirror RPC** (the owner already applied the change locally and the RPC only tells the server to match — every M1/M2/traversal knob) is guarded with **`if (!HasAuthority())`** so the host doesn't apply it twice. Gating an action RPC behind `!HasAuthority()` would silently break it on the host. The doctrine block lives above `TriggerGrab` in `CatBase.cpp`.
- **CMC overrides must be restorable from outside their own tick path** (2026-08-11, BB-16): stop braking, pivot braking, start-burst acceleration, grab drag and every traversal takeover each restore only from the path that set them, so a pawn that is unpossessed or destroyed mid-state strands the override (an unpossessed cat mid-mantle keeps `MOVE_Flying` and floats). `ACatBase::RestoreAllCMCOverrides()` — called from `EndPlay` and `UnPossessed` — clears the owning flags then restores through each system's own `Apply*(false)`, and delegates traversal to `UCatTraversalComponent::AbortAllTraversal()`, which stays the single restore point for its own takeovers. **Add new CMC-owning systems to both.**
- **Replicated vs cosmetic split**: When adding new animation-driving variables, decide whether they need replication (gameplay-authoritative) or can be derived locally (cosmetic). Prefer local derivation for anything the AnimBP uses for blending. Don't add speculative replicated state — the 2026-06 cleanup deleted eight never-written replicated properties.
- **UHT after header changes**: Adding new UCLASS/UPROPERTY/UFUNCTION requires a full rebuild (not just incremental compile) if reflection data changes.
- **Two "Chaos"**: the physics solver (Geometry Collection fracture) and the gameplay "Chaos score" share the name but are unrelated. Don't conflate them.
- **Never hardcode prop scores**: chaos values are authored in `DT_ChaosRewards` rows, keyed by each prop's `ChaosRewardKey`. An unknown/`None` key silently scores `DefaultChaosValue`.
- **Engine OSS keys drift between versions**: `SEARCH_PRESENCE` existed in older engines and is gone in 5.7 (`SEARCH_LOBBIES` replaced it for the Steam lobby path). Always use the engine macros from `Online/OnlineSessionNames.h`, never hand-rolled FName strings — a wrong key fails silently.
- **BP/asset edits need the Editor open**: the MCP endpoint lives in the editor process and dies with it. "Tools unreachable" almost always means "open the Editor," not a code change — but on 5.8 also check that something is actually **listening on :8000** before suspecting the plugin, since `bAutoStartServer` defaults to false (see `Docs/tooling.md`).

## Key Blueprint Assets (Content/)

- **`PrimeCatBase`** (`/Game/PrimeCatBase`) — Blueprint child of `ACatBase`. Assigns all input assets (IMC_Cat, IA_*), SwatMontage (AM_Cat_Swat), and tuning defaults. `MovementMaxWalkSpeed = 400` here is the intended feel (overrides the 600 C++ default).
- **`GM_CatVentures`** (`/Game/Core`) — the single GameMode BP (see `Docs/match-destruction.md`).
- **`ABP_Cat_V2`** — Animation Blueprint. Polls the ABP consumption surface from the owning `ACatBase` (the exact variable list is in `Docs/catbase.md` — do not rename any of them without updating the ABP) and binds `OnMeow`.
- **`BPC_ChaosItem`** — ActorComponent on breakable props (`BP_Destructible_Base`). Carries `ChaosRewardKey`, owns the swat-count/impact shatter logic, reports destruction (see `Docs/match-destruction.md`).
- **`DT_ChaosRewards`** (`/Game/Data`) — DataTable of `FChaosRewardData` rows (currently `Vase`, `TV`). Assigned on `GM_CatVentures`.
- **`WBP_*`** (`/Game/Blueprints/WBP` + `WBP_ChaosHUD` in `/Game/Blueprints`) — UI widgets: MainMenu (sessions), ServerRow, PauseMenu, MeowTime, RaidScoreboard (rematch gate), ScoreRow, ChaosHUD.
- **`AnimX`** asset pack — source animations under `Content/AnimX/`. (Its `CharBP_Base`/`AnimBP_Cat` are the original pack assets, not part of the game.)
- **`Content/Input/`** — `IMC_Cat` (+ `IMC_LookOnly`), `IA_Move`, `IA_Look`, `IA_Jump`, `IA_Meow`, `IA_Swat`, `IA_Interact`, `IA_Grab`, `IA_ToggleMenu`.

## Aura Plans

Design plans are stored in `Saved/.Aura/plans/` as Markdown files. These document the architecture, quirks, and step-by-step implementation decisions for each feature. Consult them when modifying existing systems. (Plans predating the 2026-06 cleanup may reference deleted scaffolding — trust this file and the code over old plans.)

**`bug-bash-backlog.md` (PARKED 2026-07-24)** is the deferred cleanup list from a full code+editor review pass — 21 findings (BB-01…BB-25) across assets, the anim SM, networking, CMC lifecycle, proxy parity and perf, each with evidence and a fix sketch, plus a suggested order. **Deliberately not being worked**: it runs AFTER the traversal verbs are complete and the M5 stubs have had their polish round. Two things to know before then: (a) **BB-14** — the mantle drives the capsule via `SetActorLocation` outside the CMC on *both* owner and server while in `MOVE_Flying`, and every remaining verb is planned on that same takeover pattern, so a 2-player PIE check is worth doing before the shape is copied four more times; (b) §9 of that doc records what was **verified clean** (M5 root tracks flat, all four jump Blend-by-bool nodes still un-inverted, pivot blend tree correct, sprint bindings intact, all 99 tuning mirrors in sync, every blendspace grid live) — don't re-audit those. Several CLAUDE.md claims are themselves on the list as wrong (BB-22…BB-25): the listen-server RPC "no-op on the host" rule, the `PivotFootworkCap` fallback claim, `MaxGrabDistance`'s shipped value, and `A_Cat_Start_Step`'s length.

<!-- BEGIN VibeUE (v5.0) — generated by VibeUE.GenerateAgentConfig; re-run to refresh -->
# VibeUE — AI agent guide (Unreal Engine 5.8)

VibeUE **extends Unreal 5.8's native AI toolset system** — its services, tools, and skills register
into the engine's `ToolsetRegistry` and are reachable through the MCP tools you already have.

**ALWAYS use the MCP tools / Python API for Unreal operations — NEVER read `.uasset` files from disk.**

---

## 1. The efficient interaction model (read this first)

There are two ways to act on the editor. Pick the cheap one:

- **`execute_python_code` — your workhorse.** Runs an arbitrary Python script in the editor in **one
  round-trip**. Every VibeUE service is exposed to Python (`unreal.BlueprintService.build_graph(...)`)
  and sits next to the whole native `unreal.*` API in the same script. **Batch aggressively** — do a
  whole multi-step task (create + edit + compile + verify) in a single call, and `print()` only what
  you need back.
- **`call_tool` — one tool per round-trip.** Genuinely needed only for **skills**
  (`AgentSkillToolset`) and for the few Epic tools whose **result the MCP layer must surface for
  you** (image returns like `CaptureViewport`). **Everything else from Epic's engine toolsets is
  also reachable from inside `execute_python_code`** via `unreal.ToolsetRegistry.execute_tool(...)`
  (see §2), so batch engine-toolset calls with your Python instead of spending a round-trip. Don't
  use `call_tool` for work `execute_python_code` can batch.

**Speed + tokens:** **avoid `describe_toolset` as a habit** — it dumps the full JSON schema of every
tool in a toolset (the most token-heavy thing here); reach for a **skill** plus a narrow
`discover_python_class('unreal.BlueprintService', method_filter='variable')` instead.

---

## 2. Tool roster — what's where

**VibeUE MCP tools (call directly):**
- `execute_python_code` — run Python (must start with `import unreal`). The workhorse.
- `discover_python_module` / `discover_python_class` / `discover_python_function` — inspect the API
  (use `unreal` lowercase; narrow with `name_filter` / `method_filter`).
- `list_python_subsystems` — list editor subsystems.
- `deep_research` — web search / page fetch / geocode (see §5).
- `terrain_data` — real-world heightmaps + water splines (see §5).

**VibeUE services (call from Python inside `execute_python_code`):** `unreal.<Name>Service.<method>()`
— Blueprint, BlueprintGraph (via BlueprintService), Material(+Node), Widget, Skeleton, AnimSequence,
AnimMontage, AnimGraph, Landscape(+Material), Foliage, MetaSound, SoundCue, Niagara(+Emitter,
+ScratchPad), StateTree, BehaviorTree, Blackboard, Input, EnumStruct, UVMapping,
RuntimeVirtualTexture, MapBlockout, GameplayTag, Viewport, Actor, Engine/ProjectSettings,
**Performance** (`unreal.PerformanceService.frame_timing()`).
These overlap-trimmed services keep only what the engine lacks — for plain asset/actor/blueprint
basics the engine's own tools may be simpler (below).

**Calling Epic's engine toolsets from Python (`execute_tool`).** Epic's engine toolset *classes*
exist as `unreal.*` (e.g. `unreal.EditorAppToolset`) but their AICallable functions are **not**
exposed as Python methods — `unreal.EditorAppToolset.get_selected_assets()` fails. Invoke them
through the registry instead (same dispatch `call_tool` uses, but in-process and batchable):
```python
import unreal, json
res = unreal.ToolsetRegistry.execute_tool(
    "EditorToolset.EditorAppToolset",   # registered (namespaced) toolset name
    "GetSelectedAssets",                # tool name
    "{}")                               # args as a JSON string
assert res.is_complete and not res.error, res.error
out = json.loads(res.get_value_as_json_string())     # -> {"returnValue": ...}
```
Discover exact names/schemas from Python: `unreal.ToolsetRegistry.get_all_toolset_json_schemas()`
(all of them) or `get_toolset_json_schema("EditorToolset.EditorAppToolset")` (one). Names are
namespaced — `EditorToolset.EditorAppToolset`, `NiagaraToolsets.NiagaraToolset_System`, etc. (a bare
`"EditorAppToolset"` returns "Toolset not found"). Note `execute_tool` returns an **async** result:
the editor tools above complete synchronously (`is_complete=True`), but for a long-running tool check
`is_complete` / bind `on_completed` rather than assuming `value` is ready.

**Use the engine's native tools for these (VibeUE intentionally doesn't duplicate them):**
- **Assets** (find / save / move / delete / duplicate / metadata): native Python
  `unreal.EditorAssetLibrary` / `EditorAssetSubsystem` inside `execute_python_code` (batchable), or
  Epic's `AssetTools` toolset (via `execute_tool` in-Python, or `call_tool`).
- **Screenshots / vision**: Epic's `EditorAppToolset` — `CaptureViewport` (returns a PNG, and can
  overlay a world grid + actor labels), `CaptureEditorImage`, `CaptureAssetImage`. **Use `call_tool`
  for these** so the MCP layer surfaces the image for you to view (`execute_tool` would only hand
  back a base64 string).
- **PIE**: `EditorAppToolset.StartPIE` / `StopPIE` / `IsPIERunning` — batchable via `execute_tool`
  (`"EditorToolset.EditorAppToolset"`), or `call_tool`.
- **Logs**: `LogsToolset.GetLogEntries` — via `execute_tool` or `call_tool` (or read the `.log` file).
- **DataTables / DataAssets / enum-struct basics**: Epic's `DataTableTools` / `DataAssetTools` /
  `ObjectTools` (via `execute_tool` or `call_tool`). (VibeUE keeps only `EnumStructService` for
  create/edit of user enums & structs.)

---

## 3. Skills — native `AgentSkill` (lazy domain knowledge)

VibeUE's ~88 skill packs are registered as Unreal **AgentSkills** and served by the engine's
`AgentSkillToolset`. Skills tell you **what to do and why**; they do **not** replace discovery of exact
signatures.

**Discover + load (both are `call_tool` on `ToolsetRegistry.AgentSkillToolset`):**
```
call_tool(tool_name="ListSkills", toolset_name="ToolsetRegistry.AgentSkillToolset")
  → { "/VibeUE/Python/init_unreal_PY.VibeUE_blueprints": "Create and modify Blueprint assets…", … }

call_tool(tool_name="GetSkills", toolset_name="ToolsetRegistry.AgentSkillToolset",
          arguments={"skillPaths": ["/VibeUE/Python/init_unreal_PY.VibeUE_blueprints"]})
  → full markdown for that pack
```
- `ListSkills` returns **summaries only** (cheap) — call it once per session to see what exists. VibeUE
  packs are `/VibeUE/Python/init_unreal_PY.VibeUE_<name>`; the engine's own skills appear alongside.
- `GetSkills` returns full instructions **lazily** — request only the packs you need.
- **Sub-docs are their own skill entries** (e.g. `…VibeUE_blueprint_graphs__build_graph`,
  `…VibeUE_state_trees__api_reference`) — load them by path the same way; no `skill/section` argument.

**When to load a skill:** the user names a domain ("create a blueprint", "build a state tree"), or you
hit a non-obvious workflow. Then: read the pack → `discover_python_class` the classes it names → write
the Python. Don't reload a pack you already loaded this session.

---

## 4. Python basics

```python
import unreal  # lowercase

# Editor subsystems:
sub = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)

# VibeUE services are static classes, called directly:
info = unreal.BlueprintService.get_blueprint_info("/Game/MyBP")

# Batch a whole task in ONE execute_python_code call, printing evidence as you go.
# Blueprint create / variables / compile are ENGINE-side (native libs + BlueprintTools toolset),
# NOT BlueprintService (it owns graphs/components/timelines — see discover_python_class):
import json
factory = unreal.BlueprintFactory(); factory.set_editor_property("ParentClass", unreal.Actor)
bp = unreal.AssetToolsHelpers.get_asset_tools().create_asset("BP_Enemy", "/Game/Blueprints", unreal.Blueprint, factory); print("CREATED:", bp)
unreal.ToolsetRegistry.execute_tool("editor_toolset.toolsets.blueprint.BlueprintTools", "add_variable",
    json.dumps({"blueprint": {"refPath": "/Game/Blueprints/BP_Enemy.BP_Enemy"},   # refPath = full object path
                "name": "Health", "type_name": "float"})); print("ADDED: Health")
unreal.BlueprintEditorLibrary.compile_blueprint(bp); print("COMPILED: BP_Enemy")
```

---

## 5. When to use `deep_research` and `terrain_data`

**`deep_research`** — when you need information that isn't in the editor:
- `action="search"` / `action="fetch_page"` — research a UE topic, API, or technique before writing code.
- `action="geocode"` / `action="reverse_geocode"` — turn a place name into lat/lng (feeds `terrain_data`).

**`terrain_data`** — when the user wants terrain from a **real-world location**:
- `preview_elevation` → use the suggested `base_level`/`height_scale` → `generate_heightmap`
  (`resolution` MUST match the landscape) → import via `unreal.LandscapeService` → `get_water_features`
  for rivers/lakes.

**The real-world-terrain chain:** `deep_research(geocode "Mount Fuji")` → `terrain_data(generate_heightmap, lng/lat)`
→ `LandscapeService` import → `terrain_data(get_water_features)` → landscape splines. Load the
`terrain-data` and `landscape` skills for the resolution formulas and water workflow.

---

## 6. See what you built (screenshots)

After any **visible** change, capture and actually look before claiming success:
```
call_tool(tool_name="CaptureViewport", toolset_name="EditorToolset.EditorAppToolset")
```
It returns a PNG (base64) and can overlay a world-space grid + actor labels for spatial awareness. For
a running game, `StartPIE` first. **Open/read the image, judge it against the request, fix, re-capture.**

---

## 7. Diagnose performance

`PerformanceService` is VibeUE's net-new capability (the engine has no perf tooling). **STEP 0 is
always CPU-bound vs GPU-bound** — optimising the GPU does nothing on a CPU-bound frame:
```python
import unreal, json
print(unreal.PerformanceService.frame_timing())            # game/render/gpu ms + bound verdict — RUN FIRST
unreal.PerformanceService.start_trace("cap", "")           # Unreal Insights trace
# … reproduce the workload (ideally under PIE / standalone) …
unreal.PerformanceService.stop_trace()
print(unreal.PerformanceService.analyse("both", ""))       # frame stats + worst frames + log hitches
```
Load the `profiling` and `frame-rate` skills for the full drill-down.

---

## 8. Build & launch

When asked to rebuild / relaunch / test, use the project script — not manual `Build.bat`/editor commands:
- `./Plugins/VibeUE/BuildAndLaunchGame.ps1` (stops the editor, builds, relaunches).
- `-StrictRebuild` for a full plugin recompile under warnings-as-errors; `-Clean` to wipe artifacts;
  `-SkipBuild` to relaunch only.
- On Linux or macOS: `./Plugins/VibeUE/BuildAndLaunchGame.sh --engine /path/to/UE5`.
  Use `--strict-rebuild`, `--clean`, or `--skip-build` for the corresponding operations.

**Readiness gate (required after launch, both platforms):**
- Parse `Editor-PID=<pid>` from the launch script's output.
- Check once, then watch `<ProjectDir>/Saved/VibeUE/Signals/editor-<pid>-true.json` using filesystem events.
- Wait at most 180 seconds; do not poll MCP. Fail if that Editor process exits or the timeout expires.
- Ignore signal files for other or dead PIDs. The signal only means `RegisterToolsets()` reached its end;
  Python, World, and level readiness remain separate checks.
- The file is JSON (`signal`, `pid`, `createdUtc`, `sessionStartUtc`, `pluginVersion`) and is written
  atomically, so it is complete as soon as it appears. PIDs get recycled: the launch scripts clear a
  stale same-PID signal on start, but if you launch the Editor yourself, check that `sessionStartUtc`
  is later than your launch time before trusting it.

---

## 9. Critical rules (evergreen)

- **Log every change for rollback.** Python has no auto-rollback — `print("CREATED:/ADDED:/MODIFIED:/DELETED:", path)`
  after each op so a mid-script failure can be undone.
- **Idempotent: check before create.** Use the service `*_exists()` (or `unreal.EditorAssetLibrary.does_asset_exist`)
  before creating, to avoid duplicates.
- **Compile after structure changes.** `unreal.BlueprintEditorLibrary.compile_blueprint(unreal.EditorAssetLibrary.load_asset(path))`
  after adding variables/functions/components (there is no `BlueprintService.compile_blueprint`;
  `build_graph`'s compile flag also works for graph edits).
- **Verify success with evidence.** For Blueprint/Widget/Material/AnimGraph/StateTree edits, a successful
  tool call isn't proof — re-read the asset (`get_nodes_in_graph`, `get_connections`, compile result)
  and report brief evidence.
- **Non-destructive.** Never remove-and-recreate to change a value, clear data to make a write succeed,
  or replace a whole object to change one field. Discover the supported setter; if none exists, report
  the gap. (StateTree reparenting: `move_state`, never remove+add.)
- **Loop prevention.** Track *outcomes*. Never repeat the same call with the same args >2× when output
  is unchanged; after 2 failed attempts at a goal, stop and report — don't try a 3rd variation.
- **Never** use modal dialogs, `input()`, blocking ops, long `time.sleep()`, or infinite loops.
- **Full asset paths** (`/Game/Blueprints/BP_Name`). **Colors are 0.0–1.0** (`{"R":1.0,"G":0.5,"B":0.0,"A":1.0}`).
- **`unreal.EditorLevelLibrary` is deprecated** — use `EditorActorSubsystem` (`get_all_level_actors()`
  + `isinstance` filtering; `get_all_level_actors_of_class` does not exist).

---

## 10. Communication & working style

- **Be concise** — this is an IDE tool. Before each tool call, one sentence on what/why; after, 1–2 on
  the result. Execute multi-step tasks straight through — don't pause for "continue".
- **Discover before you call.** Method signatures come from `discover_python_class`, not memory or skill
  prose. Skills say *which* class and *why*; discovery gives the exact call shape.
- **Commit at milestones** if the project is a git repo, so a bad experiment reverts cleanly.
- **Living gotchas:** when you solve a real problem, append a one-line gotcha+fix to this file so the
  next session doesn't relearn it.
<!-- END VibeUE -->
