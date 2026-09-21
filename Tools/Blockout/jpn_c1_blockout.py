# JPN_UrbanCity — C-1 block graybox generator (2026-09-18).
#
# Builds one suburban block at the numbers in Docs/world-metrics.md:
#   assets 2x / plan 3x / ceiling 2.5x (600) / grid 180
#   streets 1440, alley 540, lots 3240 x 3720, house 2700 x 2340, setback 240 (= the transfer gap)
#   vertical ladder in 180s: 180 AC/bins . 240 fence . 360 block wall . 540 low roof . 720 hero eave
#   . 900 bulkhead . 1080 two-story eave.  +180 = hop, +360 = sprint-scramble, 240 between faces = kicks.
#
# Run from the editor (VibeUE execute_python_code) with JPN_UrbanCity OPEN:
#   import unreal; exec(open(r"C:\Projects\CatVentures\Tools\Blockout\jpn_c1_blockout.py").read())
#
# Since 2026-09-20 (feat/convergence-loop) the hero yard also holds the finale SHRINE (ACatCenterpiece).
#
# Idempotent: every actor it spawns carries the actor tag C1Gen and is deleted before a rebuild.
# Everything is engine Cube/Cylinder on WorldGridMaterial (streets grey), labelled + foldered,
# with TextRender tier tags exactly like the ScaleCalibration cells.  Change a number, re-run.

import unreal

MAP_NAME = "JPN_UrbanCity"
TAG = "C1Gen"

EAS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
LES = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
UES = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)

assert UES.get_editor_world().get_name() == MAP_NAME, "open %s first" % MAP_NAME
assert UES.get_game_world() is None, "stop PIE first"

CUBE = unreal.load_object(None, "/Engine/BasicShapes/Cube.Cube")
CYL = unreal.load_object(None, "/Engine/BasicShapes/Cylinder.Cylinder")
GREY = unreal.load_object(None, "/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")
BP_S = unreal.load_class(None, "/Game/Blueprints/BP_Destructible_S.BP_Destructible_S_C")
BP_M = unreal.load_class(None, "/Game/Blueprints/BP_Destructible_M.BP_Destructible_M_C")
BP_L = unreal.load_class(None, "/Game/Blueprints/BP_Destructible_L.BP_Destructible_L_C")
PROP_CLS = {"S": BP_S, "M": BP_M, "L": BP_L}
SHRINE_CLS = unreal.load_class(None, "/Script/CatVentures.CatCenterpiece")   # the finale centerpiece (convergence loop)
# AMBIENT breakables (2026-09-20): same placeholders in a different colour whose rows have bFeedsMeter=False —
# they score for the cat but never move the meter, so only the hero lot unlocks the shrine.
AMBIENT_CLS = {s: unreal.load_class(None, "/Game/Blueprints/BP_Ambient_%s.BP_Ambient_%s_C" % (s, s)) for s in ("S", "M", "L")}
PROP_TAG_Z = {"S": 80, "M": 130, "L": 190}

# ---------------------------------------------------------------- metrics
STREET = 1440
ALLEY = 540
LOT_X, LOT_Y = 3240, 3720
WALL_T, WALL_H = 30, 360           # block wall
FENCE_T, FENCE_H = 20, 240         # thin fence (walkable top)
SETBACK = 240                      # house wall <-> block wall = the transfer gap
INT_T = 24                         # interior / house wall thickness
FLOOR = 90                         # house floor top (= engawa top)
CEIL = 600                         # clear interior height
EAVE = FLOOR + CEIL + 30           # 720
LOW_ROOF = 540
STEP = 900
TWO_ST = 1080
DOOR_W, DOOR_H = 180, 360
GATE_W = 180

BLOCK_X0, BLOCK_X1 = 0, 3 * LOT_X + ALLEY          # 0 .. 10260
BLOCK_Y0, BLOCK_Y1 = 0, 2 * LOT_Y                  # 0 .. 7440
COL_X = [0, LOT_X, 2 * LOT_X, 2 * LOT_X + ALLEY]   # col1 0, col2 3240, alley 6480, col3 7020
REAR_Y = LOT_Y                                     # shared rear wall centreline 3720

# ---------------------------------------------------------------- helpers
_n = [0]
def _uid():
    _n[0] += 1
    return _n[0]

def _common(a, label, folder, tags=()):
    a.set_actor_label(label)
    a.set_folder_path(unreal.Name(folder))
    a.set_editor_property("tags", [unreal.Name(TAG)] + [unreal.Name(t) for t in tags])

def box(label, folder, x0, x1, y0, y1, z0, z1, mat=None, shadow=True, tags=()):
    assert x1 > x0 and y1 > y0 and z1 > z0, ("bad box", label, x0, x1, y0, y1, z0, z1)
    a = EAS.spawn_actor_from_class(unreal.StaticMeshActor,
                                   unreal.Vector((x0 + x1) / 2.0, (y0 + y1) / 2.0, (z0 + z1) / 2.0))
    smc = a.static_mesh_component
    smc.set_static_mesh(CUBE)
    if mat:
        smc.set_material(0, mat)
    smc.set_editor_property("cast_shadow", shadow)
    a.set_actor_scale3d(unreal.Vector((x1 - x0) / 100.0, (y1 - y0) / 100.0, (z1 - z0) / 100.0))
    _common(a, label, folder, tags)
    return a

def cyl(label, folder, cx, cy, z0, z1, diam, tags=()):
    a = EAS.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(cx, cy, (z0 + z1) / 2.0))
    a.static_mesh_component.set_static_mesh(CYL)
    a.set_actor_scale3d(unreal.Vector(diam / 100.0, diam / 100.0, (z1 - z0) / 100.0))
    _common(a, label, folder, tags)
    return a

def tag(text, folder, x, y, z, size=22, color=(255, 255, 255)):
    a = EAS.spawn_actor_from_class(unreal.TextRenderActor, unreal.Vector(x, y, z),
                                   unreal.Rotator(roll=0.0, pitch=0.0, yaw=-90.0))
    tr = a.text_render
    tr.set_editor_property("text", unreal.Text(text))
    tr.set_editor_property("world_size", float(size))
    tr.set_editor_property("horizontal_alignment", unreal.HorizTextAligment.EHTA_CENTER)
    tr.set_editor_property("vertical_alignment", unreal.VerticalTextAligment.EVRTA_TEXT_BOTTOM)
    tr.set_editor_property("text_render_color", unreal.Color(r=color[0], g=color[1], b=color[2], a=255))
    _common(a, "Tag_" + text.replace(" ", "_").replace("\n", "_")[:40] + "_%d" % _uid(), folder)
    return a

def tbox(text, label, folder, x0, x1, y0, y1, z0, z1, **kw):
    """box + a tier tag on its top centre."""
    a = box(label, folder, x0, x1, y0, y1, z0, z1, **kw)
    tag(text, folder, (x0 + x1) / 2.0, (y0 + y1) / 2.0, z1 + 15)
    return a

def wall_x(label, folder, x0, x1, y0, y1, z0, z1, openings=(), **kw):
    """wall running along X; openings = (ox0, ox1, oz0, oz1) -> segments + lintels + sills."""
    cur, n = x0, 0
    for (ox0, ox1, oz0, oz1) in sorted(openings):
        if ox0 > cur:
            box("%s_%d" % (label, n), folder, cur, ox0, y0, y1, z0, z1, **kw); n += 1
        if oz1 < z1:
            box("%s_lintel%d" % (label, n), folder, ox0, ox1, y0, y1, oz1, z1, **kw)
        if oz0 > z0:
            box("%s_sill%d" % (label, n), folder, ox0, ox1, y0, y1, z0, oz0, **kw)
        cur = ox1
    if cur < x1:
        box("%s_%d" % (label, n), folder, cur, x1, y0, y1, z0, z1, **kw)

def wall_y(label, folder, x0, x1, y0, y1, z0, z1, openings=(), **kw):
    """wall running along Y; openings = (oy0, oy1, oz0, oz1)."""
    cur, n = y0, 0
    for (oy0, oy1, oz0, oz1) in sorted(openings):
        if oy0 > cur:
            box("%s_%d" % (label, n), folder, x0, x1, cur, oy0, z0, z1, **kw); n += 1
        if oz1 < z1:
            box("%s_lintel%d" % (label, n), folder, x0, x1, oy0, oy1, oz1, z1, **kw)
        if oz0 > z0:
            box("%s_sill%d" % (label, n), folder, x0, x1, oy0, oy1, z0, oz0, **kw)
        cur = oy1
    if cur < y1:
        box("%s_%d" % (label, n), folder, x0, x1, cur, y1, z0, z1, **kw)

def block_wall_x(label, folder, x0, x1, y, gates=()):
    """block wall 360 along X, centred on y; gates = (gx0, gx1) full-height openings."""
    wall_x(label, folder, x0, x1, y - WALL_T / 2.0, y + WALL_T / 2.0, 0, WALL_H,
           [(g0, g1, 0, WALL_H) for (g0, g1) in gates])
    tag("Wall 360", folder, (x0 + x1) / 2.0, y, WALL_H + 15)

def block_wall_y(label, folder, x, y0, y1, gates=()):
    wall_y(label, folder, x - WALL_T / 2.0, x + WALL_T / 2.0, y0, y1, 0, WALL_H,
           [(g0, g1, 0, WALL_H) for (g0, g1) in gates])
    tag("Wall 360", folder, x, (y0 + y1) / 2.0, WALL_H + 15)

def fence_x(label, folder, x0, x1, y, gates=()):
    wall_x(label, folder, x0, x1, y - FENCE_T / 2.0, y + FENCE_T / 2.0, 0, FENCE_H,
           [(g0, g1, 0, FENCE_H) for (g0, g1) in gates])
    tag("Fence 240", folder, (x0 + x1) / 2.0, y, FENCE_H + 15)

_pn = {"S": 0, "M": 0, "L": 0}
def prop(size, folder, x, y, z_surface):
    _pn[size] += 1
    a = EAS.spawn_actor_from_class(PROP_CLS[size], unreal.Vector(x, y, z_surface + 1))
    _common(a, "GC_%s_%02d" % (size, _pn[size]), folder)
    tag("%s %d" % (size, {"S": 50, "M": 100, "L": 160}[size]), folder, x, y, z_surface + PROP_TAG_Z[size],
        size=18, color=(60, 140, 255))
    return a

_an = {"S": 0, "M": 0, "L": 0}
def aprop(size, folder, x, y, z_surface):
    """Ambient breakable: scores, does NOT feed the meter (see AMBIENT_CLS)."""
    _an[size] += 1
    a = EAS.spawn_actor_from_class(AMBIENT_CLS[size], unreal.Vector(x, y, z_surface + 1))
    _common(a, "GCA_%s_%02d" % (size, _an[size]), folder)
    return a

def furn(name, height, folder, x0, x1, y0, y1, floor_z):
    tbox("%s %d" % (name, height), name + "_%d" % _uid(), folder, x0, x1, y0, y1, floor_z, floor_z + height)

def car(label, folder, cx, cy, along_x=True):
    L, W, H = 680, 300, 320
    if along_x:
        tbox("Car 320", label, folder, cx - L / 2, cx + L / 2, cy - W / 2, cy + W / 2, 0, H)
    else:
        tbox("Car 320", label, folder, cx - W / 2, cx + W / 2, cy - L / 2, cy + L / 2, 0, H)

def pole(label, folder, x, y, h=1800):
    cyl(label, folder, x, y, 0, h, 40, tags=("Scrambleable",))
    tag("Pole (scramble)", folder, x, y, 200, size=18)

def shrine(label, folder, x, y):
    """The finale centerpiece (feat/convergence-loop, 2026-09-20): a C++ ACatCenterpiece — a tapered
    stack of kinematic GC sections, LOCKED until the chaos meter reaches its unlock line, then brought
    down one section per all-cats-together stage; its last section ends the match. Sections, scales
    and rules are the actor's own defaults (Finale|* in the Details panel)."""
    a = EAS.spawn_actor_from_class(SHRINE_CLS, unreal.Vector(x, y, 0))
    _common(a, label, folder)
    tag("THE SHRINE\nlocked until the meter hits 60%\nany hit counts - the GLOWING tier is the real damage\nhit together for a bonus - mantle the tiers up", folder, x, y + 300, 40,
        size=26, color=(255, 200, 40))
    # The shrine's own taper is the stair (30 cm ring ledges: tops 280/500/660/760); this is the "bit of
    # fencing" (round 6): a walkable 240 fence from the west block wall to the base ledge (+40 step-up).
    # The engawa roof (540) ~2 m off the north face is the second way up.
    fence_x(label + "_FenceSpur", folder, COL_X[1] + WALL_T, x - 140, y)
    return a

# ---------------------------------------------------------------- wipe + stub cleanup
killed = 0
for a in EAS.get_all_level_actors():
    if any(str(t) == TAG for t in a.get_editor_property("tags")):
        EAS.destroy_actor(a); killed += 1
print("WIPED previous C1Gen actors:", killed)
for a in EAS.get_all_level_actors():
    lab = a.get_actor_label()
    if lab in ("Ground", "PlayerStart") or lab.startswith("Palette_"):
        print("DELETED stub actor:", lab); EAS.destroy_actor(a)

# lighting: keep the stub's rig, aim it like ScaleCalibration
for a in EAS.get_all_level_actors():
    c = a.get_class().get_name()
    if c == "DirectionalLight":
        a.modify(True); a.root_component.modify(True)
        a.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=-50.0, yaw=30.0), False)
        a.get_component_by_class(unreal.DirectionalLightComponent).set_editor_property("intensity", 10.0)
        print("MODIFIED: sun -> pitch -50 yaw 30 @ 10")
    elif c == "SkyLight":
        sc = a.get_component_by_class(unreal.SkyLightComponent); sc.modify(True)
        sc.set_editor_property("real_time_capture", True); sc.set_editor_property("intensity", 1.0)
        print("MODIFIED: skylight realtime @ 1.0")

# ---------------------------------------------------------------- ground + streets
F = "Block/Ground"
box("Ground", F, BLOCK_X0 - 2 * STREET, BLOCK_X1 + 2 * STREET, BLOCK_Y0 - 2 * STREET, BLOCK_Y1 + 2 * STREET, -50, 0)
box("Street_S", F, BLOCK_X0 - STREET, BLOCK_X1 + STREET, BLOCK_Y0 - STREET, BLOCK_Y0, 0, 5, mat=GREY)
box("Street_N", F, BLOCK_X0 - STREET, BLOCK_X1 + STREET, BLOCK_Y1, BLOCK_Y1 + STREET, 0, 5, mat=GREY)
box("Street_W", F, BLOCK_X0 - STREET, BLOCK_X0, BLOCK_Y0, BLOCK_Y1, 0, 5, mat=GREY)
box("Street_E", F, BLOCK_X1, BLOCK_X1 + STREET, BLOCK_Y0, BLOCK_Y1, 0, 5, mat=GREY)
box("Alley", F, COL_X[2], COL_X[3], BLOCK_Y0, BLOCK_Y1, 0, 5, mat=GREY)
tag("S street 1440", F, 5130, -STREET / 2.0, 60, size=70, color=(255, 220, 80))
tag("N street 1440", F, 5130, BLOCK_Y1 + STREET / 2.0, 60, size=70, color=(255, 220, 80))
tag("Alley 540", F, (COL_X[2] + COL_X[3]) / 2.0, 200, 60, size=40, color=(255, 220, 80))
tag("JPN_UrbanCity C-1 blockout\nassets 2x / plan 3x / ceiling 2.5x\nDocs/world-metrics.md", F, 4000, -1000, 60,
    size=70, color=(255, 220, 80))

# player starts on the S street, facing the hero gate
for i in range(4):
    ps = EAS.spawn_actor_from_class(unreal.PlayerStart, unreal.Vector(5700 + 200 * i, -600, 92),
                                    unreal.Rotator(roll=0.0, pitch=0.0, yaw=90.0))
    _common(ps, "PlayerStart_%d" % i, "Block/Ground")

# ---------------------------------------------------------------- perimeter + lot walls
F = "Block/Walls"
block_wall_y("Perim_W", F, BLOCK_X0 + WALL_T / 2.0, WALL_T, BLOCK_Y1 - WALL_T)
block_wall_y("Perim_E", F, BLOCK_X1 - WALL_T / 2.0, WALL_T, BLOCK_Y1 - WALL_T)
block_wall_y("Divider_12", F, COL_X[1], WALL_T, BLOCK_Y1 - WALL_T)                     # col1|col2, both rows
block_wall_y("Alley_W", F, COL_X[2] - WALL_T / 2.0, 0, BLOCK_Y1)                        # hero / N2 side
block_wall_y("Alley_E", F, COL_X[3] + WALL_T / 2.0, 0, BLOCK_Y1)                        # S3 / N3 side
block_wall_x("Rear_W", F, WALL_T, COL_X[2] - WALL_T, REAR_Y)                            # shared rear line
block_wall_x("Rear_E", F, COL_X[3] + WALL_T, BLOCK_X1 - WALL_T, REAR_Y)
# S row fronts (y = 15): S1 gate + open driveway, hero gate; S3 open frontage (parking)
block_wall_x("Front_S1", F, WALL_T, COL_X[1], WALL_T / 2.0, gates=[(1200, 1380), (2250, COL_X[1])])
HERO_GATE = (6006, 6186)
block_wall_x("Front_S2", F, COL_X[1], COL_X[2] - WALL_T, WALL_T / 2.0, gates=[HERO_GATE])
# N row fronts (y = 7425): N1 + N3 thin fences, N2 block wall
fence_x("Front_N1", F, WALL_T, COL_X[1], BLOCK_Y1 - FENCE_T / 2.0, gates=[(1500, 1680)])
block_wall_x("Front_N2", F, COL_X[1], COL_X[2] - WALL_T, BLOCK_Y1 - WALL_T / 2.0, gates=[(4800, 4980)])
fence_x("Front_N3", F, COL_X[3] + WALL_T, BLOCK_X1 - WALL_T, BLOCK_Y1 - FENCE_T / 2.0, gates=[(8500, 8680)])

# ---------------------------------------------------------------- HERO lot S2 (X 3240..6480, Y 0..3720)
HX, HY = 3510, 1125                       # house SW corner (setback 255 W, 240 E; yard 1095 S; setback 240 N)
HW, HD = 2700, 2340
F = "Block/Lot_S2_Hero"
tag("S2  HERO  house + yard\nenterable, furnished", F, 4860, 300, 60, size=70, color=(255, 220, 80))

def hx(x): return HX + x
def hy(y): return HY + y

# shell
box("Hero_FloorSlab", F + "/Shell", hx(0), hx(HW), hy(0), hy(HD), 0, FLOOR)
tbox("Roof 720", "Hero_Roof", F + "/Shell", hx(-60), hx(HW + 60), hy(-60), hy(HD + 60), EAVE - 30, EAVE, shadow=False)
Z0, Z1 = FLOOR, FLOOR + CEIL
D = (Z0, Z0 + DOOR_H)                    # door opening z band
wall_x("Hero_WallS", F + "/Shell", hx(0), hx(HW), hy(0), hy(INT_T), Z0, Z1,
       [(hx(168), hx(1248), D[0], D[1]),          # LDK engawa opening 1080
        (hx(1572), hx(2292), D[0], D[1]),         # washitsu engawa opening 720
        (hx(2496), hx(2676), D[0], D[1])])        # genkan front door
wall_x("Hero_WallN", F + "/Shell", hx(0), hx(HW), hy(HD - INT_T), hy(HD), Z0, Z1,
       [(hx(2496), hx(2676), D[0], D[1]),         # back door -> rear setback
        (hx(300), hx(660), Z0 + 180, Z0 + 540)])  # bedroom window, sill 180
wall_y("Hero_WallW", F + "/Shell", hx(0), hx(INT_T), hy(INT_T), hy(HD - INT_T), Z0, Z1)
wall_y("Hero_WallE", F + "/Shell", hx(HW - INT_T), hx(HW), hy(INT_T), hy(HD - INT_T), Z0, Z1)
# interior partitions
wall_y("Hero_HallWall", F + "/Interior", hx(2472), hx(2496), hy(INT_T), hy(HD - INT_T), Z0, Z1,
       [(hy(339), hy(519), D[0], D[1]), (hy(1854), hy(2034), D[0], D[1])])
wall_y("Hero_LdkEast", F + "/Interior", hx(1368), hx(1392), hy(INT_T), hy(HD - INT_T), Z0, Z1,
       [(hy(339), hy(519), D[0], D[1]), (hy(1854), hy(2034), D[0], D[1])])
wall_x("Hero_LdkNorth", F + "/Interior", hx(INT_T), hx(1368), hy(1104), hy(1128), Z0, Z1,
       [(hx(1080), hx(1260), D[0], D[1])])
box("Hero_BathBlock", F + "/Interior", hx(1392), hx(2472), hy(834), hy(1572), Z0, Z1)
tag("Bath / WC (solid)", F + "/Interior", hx(1932), hy(1200), Z1 - 30)
for name, x, y in (("LDK", 700, 560), ("Washitsu", 1932, 429), ("Bedroom", 700, 1700), ("Study", 1932, 1944), ("Hall", 2586, 1200)):
    tag(name, F + "/Interior", hx(x), hy(y), Z0 + 5, size=40, color=(255, 220, 80))
# engawa, porch, engawa roof
tbox("Engawa 90", "Hero_Engawa", F + "/Shell", hx(INT_T), hx(2472), hy(-180), hy(0), 0, 90)
tbox("Step 50", "Hero_Porch", F + "/Shell", hx(2496), hx(2676), hy(-180), hy(0), 0, 50)
tbox("Roof 540", "Hero_EngawaRoof", F + "/Shell", hx(-60), hx(2496), hy(-240), hy(0), LOW_ROOF - 30, LOW_ROOF)
# furniture (local coords; floor at FLOOR)
FI = F + "/Furniture"
furn("Counter", 170, FI, hx(24), hx(384), hy(984), hy(1104), FLOOR)
furn("Fridge", 360, FI, hx(420), hx(560), hy(964), hy(1104), FLOOR)
furn("TVstand", 90, FI, hx(24), hx(114), hy(444), hy(684), FLOOR)
furn("LowTable", 70, FI, hx(200), hx(380), hy(504), hy(624), FLOOR)
furn("Sofa", 90, FI, hx(420), hx(600), hy(364), hy(764), FLOOR)
furn("DiningTable", 140, FI, hx(840), hx(1080), hy(484), hy(644), FLOOR)
for cx, cy in ((785, 564), (1135, 564), (960, 429), (960, 699)):
    furn("Chair", 90, FI, hx(cx - 45), hx(cx + 45), hy(cy - 45), hy(cy + 45), FLOOR)
furn("LowTable", 70, FI, hx(1842), hx(2022), hy(369), hy(489), FLOOR)
furn("Bookshelf", 240, FI, hx(1440), hx(1620), hy(754), hy(834), FLOOR)
furn("Box", 120, FI, hx(2300), hx(2420), hy(100), hy(220), FLOOR)
furn("Box", 80, FI, hx(2200), hx(2280), hy(120), hy(200), FLOOR)
furn("Bed", 100, FI, hx(24), hx(424), hy(2036), hy(2316), FLOOR)
furn("Bookshelf", 240, FI, hx(1100), hx(1280), hy(2236), hy(2316), FLOOR)
furn("Box", 120, FI, hx(1200), hx(1320), hy(1200), hy(1320), FLOOR)
furn("Desk", 140, FI, hx(1812), hx(2052), hy(2136), hy(2296), FLOOR)
furn("Chair", 90, FI, hx(1887), hx(1977), hy(2000), hy(2090), FLOOR)
furn("Box", 80, FI, hx(1420), hx(1500), hy(1600), hy(1680), FLOOR)
# props (x, y, surface z)
FP = F + "/Props"
for x, y, z in ((120, 1044, FLOOR + 170), (300, 1044, FLOOR + 170), (920, 540, FLOOR + 140), (1000, 590, FLOOR + 140),
                (1932, 429, FLOOR + 70), (1530, 794, FLOOR + 240), (1190, 2276, FLOOR + 240), (224, 2176, FLOOR + 100),
                (1932, 2216, FLOOR + 140)):
    prop("S", FP, hx(x), hy(y), z)
for x, y, z in ((69, 564, FLOOR + 90), (1200, 900, FLOOR), (1700, 600, FLOOR), (700, 1700, FLOOR), (2350, 1700, FLOOR),
                (2586, 1200, FLOOR), (700, -90, 90)):
    prop("M", FP, hx(x), hy(y), z)
prop("M", FP, 5000, 600, 0)                        # yard
for x, y in ((700, 900), (2586, 2000)):
    prop("L", FP, hx(x), hy(y), FLOOR)
# yard dressing
FY = F + "/Yard"
tbox("AC unit 180", "Hero_AC", FY, 6240, 6420, 2300, 2380, 0, 180)          # E setback
tbox("Bins 180", "Hero_Bin1", FY, 5800, 5890, 60, 150, 0, 180)
tbox("Bins 180", "Hero_Bin2", FY, 5700, 5790, 60, 150, 0, 180)
tbox("Rock 120", "Hero_Rock", FY, 3620, 3820, 320, 480, 0, 120)
shrine("Hero_Shrine", FY, 4300, 540)      # the finale centerpiece: west yard, in view from the gate and the street

# ---------------------------------------------------------------- S1 (X 0..3240): 2-story + wing + bulkhead + carport (route)
F = "Block/Lot_S1"
tag("S1  1-story wing + carport\n(roof route from HERO)", F, 1620, 300, 60, size=70, color=(255, 220, 80))
tbox("Roof 1080", "S1_TwoStory", F, 270, 1350, 1125, 3465, 0, TWO_ST)
tbox("Roof 720", "S1_Wing", F, 1350, 2250, 1125, 3465, 0, EAVE)
tbox("Step 900", "S1_Bulkhead", F, 1350, 1710, 1800, 2160, EAVE, STEP)
tbox("Carport 540", "S1_CarportRoof", F, 2250, COL_X[1] - WALL_T / 2.0, WALL_T, 1530, LOW_ROOF - 30, LOW_ROOF)
for i, (px, py) in enumerate(((2280, 60), (2280, 1500), (3195, 60), (3195, 1500))):
    cyl("S1_CarportPost_%d" % i, F, px, py, 0, LOW_ROOF - 30, 30)
car("S1_Car", F, 2737, 780, along_x=False)
tbox("Engawa 90", "S1_Engawa", F, 270, 2250, 945, 1125, 0, 90)
tbox("Roof 540", "S1_EngawaRoof", F, 210, 2250, 885, 1125, LOW_ROOF - 30, LOW_ROOF)
tbox("AC unit 180", "S1_AC", F, 60, 240, 2000, 2080, 0, 180)

# ---------------------------------------------------------------- S3 (X 7020..10260): coin parking + shed
F = "Block/Lot_S3"
tag("S3  coin parking + shed", F, 8640, 300, 60, size=70, color=(255, 220, 80))
box("S3_Asphalt", F, COL_X[3] + WALL_T, BLOCK_X1 - WALL_T, WALL_T, 2000, 0, 6, mat=GREY)
for i, cx in enumerate((7500, 8300, 9100)):
    car("S3_Car_%d" % i, F, cx, 700, along_x=False)
tbox("Machine 180", "S3_Ticket", F, 7250, 7340, 250, 310, 0, 180)
block_wall_x("S3_BackWall", F, COL_X[3] + WALL_T, BLOCK_X1 - WALL_T, 2015, gates=[(9600, 9780)])
tbox("Shed 540", "S3_Shed", F, 8200, 9200, 2500, 3300, 0, LOW_ROOF)
tbox("Bins 180", "S3_Bin", F, 9800, 9890, 3400, 3490, 0, 180)

# ---------------------------------------------------------------- N row (Y 3720..7440, fronts on the N street)
NY0 = REAR_Y + WALL_T / 2.0 + SETBACK      # 3975 house S face
NY1 = NY0 + HD                             # 6315
F = "Block/Lot_N1"
tag("N1  2-story", F, 1620, 7100, 60, size=70, color=(255, 220, 80))
tbox("Roof 1080", "N1_House", F, 270, 2970, NY0, NY1, 0, TWO_ST)
tbox("Engawa 90", "N1_Engawa", F, 270, 2970, NY1, NY1 + 180, 0, 90)
tbox("Roof 540", "N1_EngawaRoof", F, 210, 3030, NY1, NY1 + 240, LOW_ROOF - 30, LOW_ROOF)
tbox("AC unit 180", "N1_AC", F, 3000, 3180, 5000, 5080, 0, 180)

F = "Block/Lot_N2"
tag("N2  house + kura\nkick chimney 240 between", F, 4860, 7100, 60, size=70, color=(255, 220, 80))
tbox("Roof 1080", "N2_House", F, 3510, 5490, NY0, NY1, 0, TWO_ST)
tbox("Kura 1080", "N2_Kura", F, 5730, COL_X[2] - WALL_T, 4800, 5880, 0, TWO_ST)
tag("Gap 240\n(kick chimney)", F, 5610, 5340, 100, size=40, color=(255, 120, 120))
tbox("Engawa 90", "N2_Engawa", F, 3510, 5490, NY1, NY1 + 180, 0, 90)
tbox("Roof 540", "N2_EngawaRoof", F, 3450, 5550, NY1, NY1 + 240, LOW_ROOF - 30, LOW_ROOF)
tbox("Bins 180", "N2_Bin", F, 4700, 4790, 7300, 7390, 0, 180)

F = "Block/Lot_N3"
tag("N3  2-story", F, 8640, 7100, 60, size=70, color=(255, 220, 80))
tbox("Roof 1080", "N3_House", F, 7290, 9990, NY0, NY1, 0, TWO_ST)
tbox("Roof 720", "N3_Garage", F, 7050, 7290, 4200, 5400, 0, EAVE)          # 1-story lean-to on the alley side
tbox("Engawa 90", "N3_Engawa", F, 7290, 9990, NY1, NY1 + 180, 0, 90)
tbox("Roof 540", "N3_EngawaRoof", F, 7230, 10050, NY1, NY1 + 240, LOW_ROOF - 30, LOW_ROOF)
tbox("AC unit 180", "N3_AC", F, 10050, 10230, 5200, 5280, 0, 180)

# ---------------------------------------------------------------- alley + street furniture
F = "Block/Street"
tbox("Bicycle 100", "Alley_Bike", F, 6520, 6580, 3000, 3180, 0, 100)
tbox("Bins 180", "Alley_Bin", F, 6930, 7020, 5000, 5090, 0, 180)
tbox("Vending 360", "Vend_S", F, 1900, 2100, -320, -180, 0, 360)
tbox("Vending 360", "Vend_N", F, 8700, 8900, BLOCK_Y1 + 180, BLOCK_Y1 + 320, 0, 360)
car("Street_Car", F, 4000, -900, along_x=True)
tbox("Bench 90", "Bench", F, -700, -520, 3600, 3660, 0, 90)
tbox("Shrine 540", "Shrine", F, -1100, -860, 3900, 4140, 0, LOW_ROOF)
tbox("Step 180", "ShrineStep", F, -1100, -860, 4140, 4320, 0, 180)
for i, x in enumerate((800, 3200, 5600, 8000, 10400)):
    pole("Pole_S_%d" % i, F, x, -120)
for i, x in enumerate((2000, 6500, 9500)):
    pole("Pole_N_%d" % i, F, x, BLOCK_Y1 + 120)

# ---------------------------------------------------------------- ambient breakables (2026-09-20)
# Filler across the rest of the block: fun to smash, scores for the cat, never moves the meter.
# Kept off the hero lot so its 175 stays the only fuel for the shrine.
F = "Block/Ambient"
for size, x, y, z in (
        # S street (cars / vending / bins side)
        ("S", 2200, -260, 0), ("M", 3400, -700, 0), ("S", 4600, -820, 0), ("M", 7400, -400, 0), ("S", 8400, -900, 0), ("L", 9800, -600, 0),
        # S1 driveway + engawa
        ("M", 2400, 250, 0), ("S", 3050, 1350, 0), ("S", 1200, 1035, 90),
        # S3 coin parking + shed + bins
        ("M", 7900, 1500, 6), ("S", 8700, 300, 6), ("M", 9600, 1200, 6), ("L", 9800, 1700, 6), ("S", 8600, 2400, 0), ("M", 9650, 3450, 0),
        # alley
        ("S", 6750, 1500, 5), ("M", 6750, 4000, 5), ("S", 6600, 6200, 5),
        # N yards (N1 / N2 / N3), off the kick chimney
        ("M", 1600, 6900, 0), ("S", 2500, 7100, 0), ("M", 4500, 6800, 0), ("S", 3800, 7000, 0), ("L", 6100, 6800, 0),
        ("S", 9200, 6900, 0), ("M", 8000, 7000, 0),
        # N / W / E streets
        ("M", 5000, 8200, 0), ("S", 3000, 8000, 0), ("L", 7600, 8300, 0), ("S", -700, 2500, 5), ("M", -900, 5500, 5),
        ("M", 10800, 1500, 5), ("S", 11000, 4500, 5)):
    aprop(size, F, x, y, z)

# ---------------------------------------------------------------- backdrop ring (beyond the streets)
F = "Block/Backdrop"
bd_s0, bd_s1 = BLOCK_Y0 - 2 * STREET, BLOCK_Y0 - STREET        # -2880..-1440
bd_n0, bd_n1 = BLOCK_Y1 + STREET, BLOCK_Y1 + 2 * STREET        # 8880..10320
block_wall_x("Backdrop_WallS", F, BLOCK_X0 - STREET, BLOCK_X1 + STREET, bd_s1 - WALL_T / 2.0)
block_wall_x("Backdrop_WallN", F, BLOCK_X0 - STREET, BLOCK_X1 + STREET, bd_n0 + WALL_T / 2.0)
block_wall_y("Backdrop_WallW", F, BLOCK_X0 - STREET - WALL_T / 2.0, BLOCK_Y0, BLOCK_Y1)
block_wall_y("Backdrop_WallE", F, BLOCK_X1 + STREET + WALL_T / 2.0, BLOCK_Y0, BLOCK_Y1)
for i, (x0, h) in enumerate(((180, TWO_ST), (3690, EAVE), (7200, TWO_ST))):
    box("Backdrop_S_%d" % i, F, x0, x0 + 2880, bd_s0, bd_s0 + 1200, 0, h)
    box("Backdrop_N_%d" % i, F, x0, x0 + 2880, bd_n1 - 1200, bd_n1, 0, h)
for i, (y0, h) in enumerate(((400, TWO_ST), (4200, EAVE))):
    box("Backdrop_W_%d" % i, F, BLOCK_X0 - 2 * STREET, BLOCK_X0 - 2 * STREET + 1200, y0, y0 + 2880, 0, h)
    box("Backdrop_E_%d" % i, F, BLOCK_X1 + 2 * STREET - 1200, BLOCK_X1 + 2 * STREET, y0, y0 + 2880, 0, h)

# ---------------------------------------------------------------- summary + save
from collections import Counter
cnt = Counter()
for a in EAS.get_all_level_actors():
    if any(str(t) == TAG for t in a.get_editor_property("tags")):
        cnt[str(a.get_folder_path()).split("/")[1] if "/" in str(a.get_folder_path()) else str(a.get_folder_path())] += 1
print("BUILT:", dict(cnt), "total", sum(cnt.values()), "props S/M/L =", _pn, "ambient S/M/L =", _an)
print("SAVED:", LES.save_current_level(), "dirty maps:", len(unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages()))
