# World Metrics — the card C-1 is built against

**Ruled by Sean in `/Game/Maps/ScaleCalibration` over 2026-09-13 → 09-18 (C-0). The cat's numbers are LOCKED (2026-08-24); the world scales to the cat.** Every number here is in Unreal units (cm). Anything placed in `JPN_UrbanCity` should be checkable against this page; if a new height/gap/opening isn't on it, it either fits an existing band or it needs a ruling — don't invent a fourth tier.

## 1. Scale rules (three multipliers, not one)

| Rule | Value | Meaning |
|---|---|---|
| **Assets** | **2.0×** | Every mesh/kit piece is placed at uniform 2× real size. One scale for props, furniture, walls, doors. |
| **Plan** | **3.0×** | A *layout* rule, not an asset scale: rooms get ~1.5× more wall modules per side than a 2× room would. Reference room 1080 × 810 (a 6-tatami room at 3×). 3.5× also read well; 4× too big. |
| **Ceiling** | **2.5×** | Interior clear height **600** (from floor top to ceiling underside). 480 (2×) felt cramped standing on furniture; 720 too tall. |
| **Grid** | **180** | 2× the kit's 90 module. Reference room = 6 × 4.5 modules. Snap walls/openings to it. |

## 2. Openings

| Element | Size | Note |
|---|---|---|
| Door | **180 wide × 360 tall** | 2× kit door. Lintel above it is 240 tall under a 600 ceiling. |
| Hallway | **180 wide** | Camera boom is 150 — fits without clipping. |
| Room (reference) | 1080 × 810 | Furnished test cell. |

## 3. Standard heights (above floor top)

**Interior**

| Element | Height | Cat read |
|---|---|---|
| Step | 50 | walk-over |
| Low table | 70 | vault (mantle band) |
| Sofa / chair / TV stand / engawa | 90 | tap-jump vault |
| Bed | 100 | tap-jump vault |
| Dining table | 140 | tap-jump mantle |
| **Counter** | **170** | **the everyday counter — tap-jump mantle. 200/220 tested, not adopted.** |
| Bookshelf | 240 | held-jump mantle |
| Fridge | 360 | two-step: counter → fridge (190 rise) |
| Ceiling | 600 | — |

**Exterior** (approved on the round-1 2× cell)

| Element | Height | Cat read |
|---|---|---|
| Engawa / porch | 90 | tap-jump vault |
| AC unit / bins / low step | 180 | hop |
| Fence (thin) | 240 | held-jump mantle; walkable on top (≥20 wide is proven) |
| Block wall, vending machine | 360 | scramble + top-out (sprint run-up) |
| Low roof — engawa roof, carport, shed | 540 | hop from a wall top (+180) |
| 1-story eave (the hero house) | 720 | floor 90 + ceiling 600 + slab 30; hop from a low roof (+180) |
| Stair bulkhead / mid-step | 900 | hop (+180) |
| 2-story eave | 1080 | hop from a bulkhead (+180) |
| Roof / wall gap | 240 | wall-transfer (≤250) — also the house↔block-wall setback |

**The vertical ladder rule (C-1, 2026-09-18):** every exterior tier is a multiple of **180**. **+180 = a tap-jump hop, +360 = a sprint-scramble where there is runway, any two tall faces 240 apart = a kick chain.** The card's original "roof eave 540" was approved under the 480 ceiling; ceiling 600 pushes an *enterable* eave to 720, so 540 is now the low-roof tier (engawa roofs, carports, sheds). House floors sit at **90** (= the engawa), so interior heights are measured from 90.

## 4. Cat interaction bands (why the heights above work — verify new numbers against these)

| Verb | Band | Source |
|---|---|---|
| Capsule | r 14 / half-height 22 | `PrimeCatBase` |
| Trot / sprint | 400 / 650 cm/s | `MovementMaxWalkSpeed` (BP override) / `SprintMaxWalkSpeed` |
| Jump apex | ~125 tap / ~240 held | measured (Docs/catbase.md) |
| Mantle catch | lip **25–70 above the capsule bottom** at any airborne frame → tap reaches lips ≤ ~190, held ≤ ~300 | `MantleMinLedgeHeight` / `MantleMaxLedgeHeight` |
| Vault vs climb clip | ≤ 70 = vault (ships vault-for-all) | `MantleVaultMaxHeight` |
| Scramble | wall ≥ 120 tall, entry ≥ 450 cm/s (sprint), climbs ~290 then mantles the top → walls ≤ ~360 | `ScrambleMinWallHeight` / `ScrambleMinEntrySpeed` |
| Wall transfer | gap ≤ 250; climb ≈ gap + 3% per crossing | `WallTransferMaxGap` |
| Wall bounce | 420 lateral / 620 up | `WallBounce*Speed` |
| Balance assist | surfaces ≤ 60 wide get centreline help | `FenceMaxSurfaceWidth` |
| Camera boom | 150 | `PrimeCatBase` |

## 5. Breakable prop bands (placeholders = `BP_Destructible_S/M/L`, `GC_Cylinder` at 0.5 / 1.0 / 1.6)

| Band | Ø in world | Real object | Row (`DT_ChaosRewards`) | Placeholder mass | Bump feel |
|---|---|---|---|---|---|
| **S** | 50 | 25 cm — vase, bottle, dish | `Small`: 5 pts · impact 400 · 1 swat | ~245 kg | slides / topples freely |
| **M** | 100 | 50 cm — lamp, plant pot, TV | `Medium`: 10 · 650 · 2 swats | ~1960 kg | rocks (~13% push) |
| **L** | 160 | 80 cm — low table, shelf | `Large`: 25 · 950 · 4 swats | ~8000 kg | barely (~3% push) |

Sean 09-18: bands + bump feel are a good place to start for stubs. Bump = `BumpPushAccel` 400 × min(mass, `BumpPushRefMass` 250); charge-smash at ≥ `ChargeShatterSpeed` 500 (sprint only). Masses come from the GC asset's density 2500 — **real props need their own density or the ref mass retuned**, and the S-on-counter fall (≈580 cm/s) is what the `Small` impact threshold 400 is tuned to.

## 6. Exterior plan numbers (C-1 block, `Tools/Blockout/jpn_c1_blockout.py`)

Plan 3× applies to exteriors too: **street 1440**, **alley 540**, **lot 3240 × 3720**, house footprint **2700 × 2340**, yard **~1080** deep on the street side, house↔block-wall setback **240**, block wall 30 thick, fence 20 thick, gates/doors 180. `JPN_UrbanCity` is generated from that script — change a number there and re-run rather than hand-moving actors.

**What the block contains (first pass, Sean: "feels pretty good", 2026-09-18):** 2 × 3 lots, streets on four sides, a N–S alley between columns 2 and 3, a backdrop ring beyond the streets. **S2 = HERO** (X 3240–6480, middle of the S street): gate → yard → engawa 90 under a 540 roof → main roof 720; enterable — LDK, washitsu, bedroom, study, hall, solid bath block; front + back doors, one bedroom window (sill 180); furniture at the card heights; 9 S / 8 M / 2 L props. **S1 = the roof route:** hero roof 720 → carport 540 (225 gap, −180) → wing 720 → bulkhead 900 → 2-story 1080 → down its engawa roof. **N2 = the kick chimney:** house + kura, both 1080, exactly 240 apart. S3 coin parking + shed 540; N1/N3 2-story masses; poles tagged `Scrambleable`, vending 360, AC units 180, bins, bikes. PlayerStarts ×4 on the S street facing the hero gate. Deliberately absent: pitched roofs, more windows, neighbour interiors, real props (C-2).

## 7. Where these were tested

`/Game/Maps/ScaleCalibration`: round 1 (`Cell_1.0x…2.5x`, uniform) → exterior heights; round 2 (`R2_plan3.0x/3.5x/4.0x`, heights 2×, ceilings 480/600/720) → plan + ceiling; `R2_plan3.0x_furnished` (now at ceiling 600, door/hall 180) → furniture, counter 170/200/220, 17 S/M/L stubs. Keep the map — it is the prop test arena.
