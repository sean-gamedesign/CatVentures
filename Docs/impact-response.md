# Cat Impact Response — flinch / stagger / ragdoll

**MUST READ before touching `UCatImpactResponseComponent`, `CatImpactTypes.h`, `Multicast_ImpactReaction`, the swat's cat-vs-cat branch, or the stagger's place in the `Move()` suppression chain / `RestoreAllCMCOverrides`.** Written 2026-09-13 from the shipped code (`7ed8ddd`) and the two PIE rounds; the design record is `Saved/.Aura/plans/cat-impact-response-v1.2.md` (gitignored — review CLOSED 2026-08-19 after two same-day rounds; do not reopen its six ruled questions).

**Status: FROZEN at step 2 for Playtest 1 (Sean, 2026-08-24).** Steps 1 (Flinch) and 2 (Stagger) are shipped and PIE-passed; steps 3–6 (Physics-Asset spike, Ragdoll + get-up, authored impacts, hardening) resume after PT1. Do not start them.

## The shape

One system, one classifier, ordinal severity tiers `ECatImpactTier { None, Flinch, Stagger, Ragdoll }` — rules speak in "tier ≥ X" terms so a future tier slots in without touching call sites. Lives in its own component, `UCatImpactResponseComponent` (`ImpactResponse` on `ACatBase`, the traversal-component precedent per the CatBase growth-watch ruling). The component owns the classifier, every reaction's enter/drive/exit, and its own CMC apply/restore — **the one restore point for impact state**.

**Locked by Sean's brief:** swats never ragdoll (source cap = Stagger — ragdolling a rival is always indirect via launched props/traffic; no stunlock tool); vehicle hits will be **authored** impulses, not solver collisions; ragdoll is cosmetic per-machine — server tier + impulse + rest position are truth. Cats can never be grabbed (the grab query excludes `ECC_Pawn`), so "ragdolled while carried" is impossible by construction.

**Sources** (`ECatImpactSource`), each with its own report entry point that funnels to `ClassifyAndReact` (server only):
- **Swat** → `ReportSwatImpact(Attacker, ImpulseDir)`, called directly from `ACatBase::HandleSwatHit` when the swat sweep (which already hits `ECC_Pawn`) lands on a cat. **Deliberately a direct C++ call, NOT a damage-event binding** — the sweep's `ApplyPointDamage` stays a no-op sink on cats; binding a damage handler would be the first step toward the health system this project explicitly does not build. Swat = **Flinch always** for v1 (upgrades are post-playtest tuning).
- **Measured** → capsule `OnComponentHit` (`OnCapsuleHit`, bound on authority only). The capsule's `bNotifyRigidBodyCollision` is flipped ON in the component's `BeginPlay` — the Pawn profile defaults it off, so a simulating prop striking the capsule would otherwise report nothing. Two filters before classification: the other component must be **simulating physics** (walking into static furniture arrives non-simulating — that is the self-inflicted case; landing has its own channel), and the victim's **own held prop** pressing on the capsule is ignored.
- **Authored** → schema-present (`AuthoredTier` / `AuthoredLaunch` / `AuthoredAngular` in `FCatImpactParams`), no producer yet (step 5).

**Classifier quantity = impactor mass × contact speed** (kg × cm/s), deterministic; `|NormalImpulse|` is logged alongside (the Heavy-tier verify-in-build pattern — see `Docs/objectives.md`). Thresholds on the component, `Impact|Tuning`: **`FlinchImpactThreshold` 3000 / `StaggerImpactThreshold` 12000 / `RagdollImpactThreshold` 60000**. **Validated 2026-09-12** from a bracketed prop-hurl sweep: every observed flinch fell in 3.3k–11.5k, every stagger in 15.7k–20.4k, so the 12k line sits in a real gap; the ragdoll value stays provisional until step 4. A Ragdoll-class measured hit is **DOWNGRADED to Stagger** (logged `reason=ragdoll-pending`) until step 4 lands. Hit direction (`ECatImpactDirection` Front/Back/Left/Right, in the victim's frame — which side it came FROM) is quantized once, server-side, so every machine picks the same clip.

## The two shipped tiers

**Flinch (step 1, 2-player PIE-passed 2026-08-19 — "honestly funny as hell").** Directional react as a **dynamic montage on `DefaultSlot`** from the four retargeted `Cat_Damage_*` clips (`FlinchFront/Back/Left/RightSequence`, soft refs, `EditAnywhere` for swaps; `FlinchPlayRate` is the stub dial if 1.7–1.9 s reads sluggish). `FlinchRetriggerCooldown` 0.4 s: a re-hit inside the window is **dropped** (logged with reason), never queued — the anti-spam floor under the no-stunlock rule. The `FlinchNudgeSpeed` knob was deleted in review (trap-knob pattern). Zero ABP surgery — every reaction so far is a dynamic montage.

**Stagger (step 2, PIE-passed 2026-09-12).** On the victim, every machine: `AbortAllTraversal` (the knock-off — a stagger DOES knock a cat off a wall cling/scramble and that clip is the marquee moment), gravity-interpolator snap, `LaunchCharacter` (horizontal `StaggerLaunchMinSpeed` 320 → `StaggerLaunchMaxSpeed` 560 mapped by severity between the stagger and ragdoll thresholds, plus `StaggerLaunchUpSpeed` 260 to unstick from ground friction — both planes overridden, the wall-bounce precedent), `StaggerControlLossTime` 0.45 s of input suppression, the `Cat_Stun_C` dizzy loop as a dynamic montage through the whole arc (deliberate — a dizzy tumbling cat is the comedy read), then the **M4 ramp-back** on the gameplay exit. `StaggerImmunityTime` 1.5 s after a stagger ends: further stagger-class hits downgrade to Flinch (never queued, never stacked). A held prop is dropped via `Multicast_ReleaseGrab` on the same frame. **Ragdoll-class hits will NOT respect the immunity once step 4 lands** (a car must not bounce off a recently-staggered cat).

## Networking

Server-only classification, **value-carrying reliable multicast** execution: `ACatBase::Multicast_ImpactReaction(Tier, Direction, LaunchVelocity)` → `HandleImpactReactionMulticast`. The RPC carries its values (no property-replication race — the `Client_OnMatchPhaseChanged` doctrine) and **nobody skips, the victim's owner included** — nothing was predicted, so every machine renders the reaction from the multicast. Reliable because a reaction is a rare server-classified one-shot, not predicted spam like the swat montage. The fracture model applies: each machine plays its own copy. Per-machine timers (`LastStaggerEndTime`) exist on every copy, but only the server's gate classification.

## Precedence and the restore contract

- **`Move()`:** `IsStaggerSuppressing()` is consulted **FIRST** in the input-suppression chain — impact outranks pivot / coil / traversal. The steering cache stays live, same contract as every other suppression.
- **Traversal:** the same window also gates traversal *re-engaging* — `UCatTraversalComponent` skips detection and refuses wall bounces while `IsStaggerSuppressing()` (PR-02, fixed 2026-09-24; the gate lives on the traversal side, so this component's step-2 freeze is untouched). Found because the 09-12 tripwire round had no held input and only checked the abort; the 09-24 re-test held W into the wall on the client's cat — see `Docs/traversal.md`.
- **`RestoreAllCMCOverrides`:** `AbortAllImpactReactions()` runs **first of all**, above `AbortAllTraversal`. **CONTRACT (plan §2, v1.2):** it touches ONLY impact-owned state and must not clear or write any flag the traversal or grounded restores consult — the chain's ordering is deliberate, and an early write here would re-introduce the BB-16 stranded-override class. That is why the abort path does **not** arm the M4 input ramp the gameplay exit uses.

## Content

Seven pack clips retargeted to `/Game/Drafts/Impact` (stubs by the M5 doctrine; **the directory is in `DirectoriesToAlwaysCook` since 2026-10-03** — the component's defaults are soft paths set in its constructor, so the cook never reached them and every package before then played no flinch/stun at all): `A_Cat_Damage_Front_Left_C`, `_Front_Right_C`, `_Back_Left_IP`, `_Back_Right_IP`, `_Left_IP`, `_Right_IP`, `A_Cat_Stun_C` (4.03 s). Still in the pack for later tiers: `Cat_Death_Recovery_L/R` 1.73 s (the get-up), `Cat_Rolling_C` / `Cat_Shake_Water_C` flourishes. **Measure `_RM` variants before judging content** — the `_IP` clips flatten the root track (the wall-kick lesson). `PA_CatMnq` exists (36 bodies / 35 constraints, full skeleton incl. tail chain + ears + a sphere on Root) — the ragdoll tier is not an asset gap, it needs the step-3 validation spike.

## What the PIE rounds established (don't re-verify)

- **08-19, 2-player:** 14/14 multicast parity, all four directions, both host/client directions, zero warnings.
- **09-12, solo via MCP live-PIE tripwire** (`WallClingMaxTime` raised to 30 s on all cats so a cling could be parked hands-off; a post-tick script auto-hurled props at the clinging cat): knock-off-the-wall (the attach aborts the same tick, even mid-scramble at 276 uu climbed → `Wall attach END reason=aborted`); immunity (a 164k ragdoll-class hit 0.13 s after a stagger came out Flinch via the double downgrade); cooldown spam-drop (a rolling prop threw 6× `REJECT cooldown` — the 08-19 solo-untestable check, now discharged); held-prop drop (Grab RELEASE on SRV + SIM the same frame as the stagger).
- **Still untested:** sprint-flinch foot-skate (needs two humans); the client-victim net direction via tripwiring `PrimeCatBase_C_1` (receive side is source-independent and already proven, so it was skipped as unneeded).
- **mass×speed vs NormalImpulse** agree at the band edges but diverged on one hard-decelerating light prop (massXspeed 11.5k / impulse 65k). A Heavy-tier / hardening decision, deliberately not acted on.

## Gotchas

- **Geometry-Collection debris never reaches the classifier — by construction since Phase B.** Fractured chunks are `ECC_Pawn = Overlap` (Layer 1 in `ForceShatterGC`, see `Docs/match-destruction.md`), so they cannot generate a capsule `OnComponentHit`. The 09-12 "GC chunks fire ZERO impacts" finding was observed *before* Phase B (then it was the capsule↔chunk defect; Sean's "super stuck" moment was that, not a stagger bug — proven: no impact fired, so stagger was never active). The thresholds are therefore anchored to controlled prop hurls of intact physics bodies, not romp. If debris is ever meant to stagger cats, that is a deliberate policy change on the collision flip, not a threshold retune.
- **`GetMass()` on a Geometry Collection may report the whole collection, not the chunk** — read the `[Impact]` log lines rather than trusting the number.
- Every meaningful report logs one `[Impact]` line with its inputs and verdict, and a reaction that DIDN'T fire says why (the `ETraversalReject` doctrine). Filter PawPrint on `[Impact]` for a round.

## Open findings — 2026-09-22 project review (NOT fixed)

From the full code + Blueprint review on `feat/convergence-loop` (`1bceac2`). IDs are **PR-xx** in `Saved/.Aura/plans/project-review-2026-09-22.md` (file:line, evidence tag, fix sketch). Nothing below has been fixed yet — check that file for status before re-deriving.

- `CatImpactTypes.h` still says "v1 implements Flinch only" (Stagger shipped).
