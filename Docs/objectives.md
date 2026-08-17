# Objective System — checklist loop & Heavy tier

**MUST READ before touching `CatObjectiveTypes.h`, `UCatObjectiveTargetComponent`, the GameMode's objective registry/evaluator, the checklist HUD, or the Heavy-tier impact path.** Italic `see *Section*` pointers may refer to a sibling doc; the Documentation Map in `CLAUDE.md` says which one owns it.

The design plan lives at `Saved/.Aura/plans/objective-system-v1.md` (gitignored — the doc is the spec, this file records what is actually BUILT). Started 2026-08-16; the game still ships the Chaos-Meter match-ender alongside it.

## Two-tier model (the load-bearing decision)

**Conditions** are self-contained world-state checks (atoms). **Objectives** are a display string + an ANDed array of conditions (molecules). **AND is not a condition type — it is the layer above them.** Future operators (OR, ordering, temporal) become new cases in `ACatGameMode::EvaluateObjective` and *nothing else*; if a new operator needs to touch anything else, the two-tier split has been violated somewhere.

Objectives are declarative **world-state** conditions, never player actions — that is what makes every verb (grab, mantle, swat, bumper) a valid solution and gets emergence for free.

Three condition types exist in the schema; **only `Destroy` is evaluated so far.** `Relocate` and `KnockOff` are in the enum so the table and HUD do not change shape when they land — they share one overlap machinery (KnockOff is Relocate's exited-volume inverse).

## What is built

- **`CatObjectiveTypes.h`** — `EObjectiveConditionType`, `FObjectiveCondition`, `FMapObjectiveRow` (a `FTableRowBase`, cloned from `FChaosRewardData`'s pattern), `FObjectiveState`.
- **`UCatObjectiveTargetComponent`** — carries `ObjectiveTag`, registers with the GameMode at BeginPlay, reports destruction. C++ rather than Blueprint because the registry, validation and evaluator are C++; that means no graph surgery to add a condition type.
- **`ACatGameMode`** — registry, startup validation, `EvaluateObjective`, push to GameState.
- **`ACatGameState`** — replicated `TArray<FObjectiveState>` + `GetObjectiveChecklistText()` + `AreNonFinaleObjectivesComplete()`.
- **`WBP_ChaosHUD`** — a `Txt_Objectives` TextBlock driven from Tick.

## Contracts you must not break

**Registration closes in `ACatGameMode::StartPlay`, immediately after `Super::StartPlay()`.** That call runs `GetWorldSettings()->NotifyBeginPlay()`, which dispatches `BeginPlay` to every spawned actor — so the point just after it is the concrete "world init settled" anchor, and no condition is ever evaluated against a partial registry. **NOT `HandleMatchHasStarted`**: that hook is declared on `AGameMode` and `ACatGameMode` derives from `AGameModeBase` (`CatGameMode.h:13`). Do **not** reparent to obtain it — `AGameMode` drags in a `MatchState` machine that would sit alongside `ECatMatchPhase` as a second owner of "what phase is the match in".

**Latch-on-true.** Conditions are edge-triggered and sticky; bits only ever go false→true. The naive level-triggered re-check is WRONG — physics props settle unpredictably and un-completing reads as a bug. Verified in play: after an objective latched at 3/3, eight further matching props were destroyed with no re-latch and no double-count.

**The target guards its own report.** A Geometry Collection **survives its own fracture** (BB-17), so the break path can fire more than once for one prop; without the guard a count-of condition would double-count a single target.

**Startup validation closes the vacuous-completion hole.** "All matching tags" over an EMPTY set is trivially true, so a typo'd `TargetTag` would complete conditions at spawn and unlock the finale before anyone moved. Validation warns (never hard-fails — a bad table must not kill a playtest, but it must be loud) when a tag resolves to zero targets, when `CountRequired` exceeds the number that exist, when a row has no conditions, and when there is not exactly one finale row.

**`FObjectiveState` carries per-condition progress COUNTS, not just the latched bitmask.** The mask holds one bit per CONDITION, but a count-of condition is a *single* condition whose progress lives inside it — without the counts, "any 3 of 23" renders `0/1` until it snaps to `1/1`, which is exactly the "did that count?" contamination the HUD fraction exists to prevent. Decide shape changes here BEFORE step 1 replicates, not after (the BB-14 lesson: verify the shape before it is copied).

**Display text is copied into `FObjectiveState`.** `DT_MapObjectives` lives on the GameMode, which clients do not have, and the plan (§11) requires nothing else hold a table reference. Copying `DisplayName` and `bIsFinale` once makes the state self-describing.

**Targets never know which objective they belong to.** They report condition-state changes; the GameMode owns evaluation. Keeping that boundary is what lets new operators land in one function.

## Destroy reporting — how it actually arrives

`ACatGameMode::ReportItemDestroyed` finds a `UCatObjectiveTargetComponent` on the reported actor and routes to it. Every objective target today is also a chaos prop, so the existing break path is a free, **zero-Blueprint-edit** report. Targets that are NOT chaos props (Relocate/KnockOff props that never shatter) call `ReportDestroyed()` directly — the pipeline must not assume "objective target" implies "scores chaos".

## Debug HUD (deliberately ugly, by design)

One newline-joined block in a single TextBlock: `[X]` complete, `[ ] name (n/m)` incomplete, `[LOCKED]` on the finale until every non-finale objective completes, `(close enough)` on forgiven. **The fraction is playtest instrumentation, not polish** — without it players cannot tell a multi-part objective from a broken one, and reads get contaminated by "did that count?".

Two deliberate shortcuts, both flagged in code:
- **Formatting lives in C++.** One `SetText` is a far smaller Blueprint surface than building TextBlocks per row. `GetObjectiveChecklistText()` is the function to DELETE when real UI lands, not to grow.
- **The widget polls Tick** rather than binding `OnObjectiveStatesChanged`. The delegate exists and is broadcast correctly (including a manual broadcast on the listen server, since replication does not fire OnRep locally), but `K2Node_CreateDelegate` binds silently-not-at-all from Python and could not be verified — see `Docs/tooling.md`.

## Heavy tier (§3) — PARTIAL

Config lives on `DT_ChaosRewards` rows (Sean's call): `bHeavyTier`, `ImpactVelocityThreshold`, `ImpactImpulseThreshold`.

**Why two named columns and not one:** a single float whose units flip on a sibling bool, differing by orders of magnitude between rows, is a designer trap — and moving every prop to impulse would silently re-tune every shipped breakable's playtested threshold.

**The impact respec.** The path measured the VICTIM's own speed. That inverts the tier: a high-mass prop struck by a flowerpot barely moves, and massive props are precisely the ones that never reach high velocities — that is what makes them heavy. Per-prop tuning cannot fix a wrong quantity. Non-Heavy props keep victim-speed (so shipped breakables keep their feel); **Heavies measure the impactor**.

**Verify-in-build, not a separate spike:** `ShouldImpactShatter` logs `NormalImpulse` *and* the impactor's mass×speed on every Heavy impact. `NormalImpulse` is solver/substep dependent and may read zero-or-noise against an unfractured, effectively-static Geometry Collection; mass×speed is less physically pure but deterministic. One PIE round picks the winner; the fallback is pre-approved so no design call is pending. `DefaultImpactImpulseThreshold` is a provisional **25000** until then.

**Helpers are static with a WorldContext** so each Blueprint call site is ONE node instead of a `Get Game Mode → Cast` chain spliced into a working graph.

### ⚠ Known gaps in the Heavy tier

- **The bumper lockout is written but INERT.** `Multicast_BumperHitGC` refuses actors tagged `HeavyProp` — gated there rather than in `Server_BumperHitGC` because the listen-server host calls the multicast directly and would bypass a server-only check. **Nothing sets that tag yet**, so a Heavy is still bumper-breakable. Closing it needs a `RegisterPropKey` helper called from `BPC_ChaosItem`'s BeginPlay.
- No Heavy row is authored in `DT_ChaosRewards` yet, and no prop is tagged as one.
- The impulse-vs-mass×speed PIE round has not been run.

## Live test scaffolding (not shipping content)

- `DT_MapObjectives` — two rows: `PileOfShame` (Destroy `ArenaProp`, `CountRequired = 3`) and `TheCenterpiece` (Destroy `FinaleProp`, finale).
- `ObjectiveTarget` component on **`BP_Destructible_Base`** — so ALL destructibles register (32 in TestMap_02). Tagging is inherited, not opt-in; if that gets noisy, make it opt-in.
- 24 debris props on the terrain east of the platform in TestMap_02 (`SpikeDebris_01`–`24`), 23 tagged `ArenaProp`, `_24` tagged `FinaleProp`.
- **`GM_CatVentures` `ChaosThreshold` is raised 100 → 1000000** so match-end does not fire during objective testing. **Revert to 100 to restore the chaos-meter loop.**

## The finale gate does not exist yet

`bIsFinaleSetPiece` is a flag the HUD reads and *nothing else*. The set piece is breakable whenever — observed in play, where the centerpiece was completed before the ordinary checklist. Making it mechanically unbreakable until unlocked is step 4, and it depends on the Heavy tier landing first.
