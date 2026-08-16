# Match Flow & Destruction

**MUST READ before touching the Chaos score, the match phase machine / match-end cinematics, the scoreboard/rematch path, or breakable props.** Split out of CLAUDE.md on 2026-08-16 — text unchanged. Italic `see *Section*` pointers may refer to a sibling doc; the Documentation Map in `CLAUDE.md` says which one owns it.

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

