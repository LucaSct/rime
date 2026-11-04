# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Deterministic generator for `samples/venice/venice.rscene`.

Reads cooked mesh and material content IDs from `build/asset-cache/venice/manifest.txt`
(produced by `python3 assets/tools/cook_venice.py`) and writes `samples/venice/venice.rscene`.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path
import sys

# Reflection component type IDs (copied from existing samples/*.rscene).
TYPE_LOCAL_TRANSFORM = "rime::ecs::LocalTransform 0xe7d60f5a66770fe9"
TYPE_CAMERA = "rime::render::Camera 0x42cdb0b7ddc97dbc"
TYPE_DIRECTIONAL_LIGHT = "rime::render::DirectionalLight 0x0279ad2e8762ff9a"
TYPE_SKY = "rime::render::Sky 0x9fe7bd14c159bdab"
TYPE_PLAYER_SPAWN = "rime::block_play::PlayerSpawn 0x5bb141d781156549"
TYPE_GROUND_SURFACE = "rime::ground::GroundSurface 0xd6ea82acd7add462"
TYPE_MATERIAL_ASSET = "rime::render::MaterialAsset 0x6031d5a8e12eb6b5"
TYPE_MESH_ASSET = "rime::render::MeshAsset 0x892fe7631c8aa199"
TYPE_MESH_REF = "rime::render::MeshRef 0xa2a14d16cfd75985"
TYPE_MATERIAL_REF = "rime::render::MaterialRef 0xc6b4f8ca9fd52bed"

# Exact measured opening bounds from the glTF POSITION accessors (metres, Y up).
# Each opening has local z=0 at the wall plane, recess toward -z, and surround toward +z.
DOOR_BBOX = {
    "x_min": -0.802344,
    "x_max": 0.787061,
    "y_min": -0.223350,
    "y_max": 3.561396,
}
BARRED_BBOX = {
    "x_min": -0.828614,
    "x_max": 0.802305,
    "y_min": -0.156135,
    "y_sill_ledge": -0.015800,  # full-width surround starts here; below is notched projecting sill
    "y_max": 1.790132,
}
SHUTTER_BBOX = {
    "x_min": -0.678091,
    "x_max": 0.635271,
    "y_min": -0.206217,
    "y_max": 1.906692,
}

WALL_SLAB_W = 4.0
WALL_SLAB_H = 3.0
WALL_HALF_THICK = 0.125


def fmt(val: float) -> str:
    """Format a float deterministically without `-0` or excessive trailing zeros."""
    if abs(val) < 1e-9:
        return "0"
    rounded = round(val, 6)
    if rounded == int(rounded):
        return str(int(rounded))
    return f"{rounded:.6f}".rstrip("0").rstrip(".")


def quat_mul(
    a: tuple[float, float, float, float],
    b: tuple[float, float, float, float],
) -> tuple[float, float, float, float]:
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def quat_from_yaw_deg(yaw_deg: float) -> tuple[float, float, float, float]:
    """Quaternion rotating around +Y by `yaw_deg` degrees."""
    half = math.radians(yaw_deg) * 0.5
    return (0.0, math.sin(half), 0.0, math.cos(half))


def quat_yaw_pitch_deg(yaw_deg: float, pitch_deg: float) -> tuple[float, float, float, float]:
    """Camera/light orientation: yaw around world +Y, then local pitch around +X."""
    qy = quat_from_yaw_deg(yaw_deg)
    hp = math.radians(pitch_deg) * 0.5
    qp = (math.sin(hp), 0.0, 0.0, math.cos(hp))
    return quat_mul(qy, qp)


def load_manifest(manifest_path: Path) -> tuple[dict[str, int], dict[str, int]]:
    """Return `(meshes_by_cooked_name, materials_by_cooked_name)` as decimal u64 IDs."""
    if not manifest_path.is_file():
        raise RuntimeError(
            f"Cooked manifest not found at {manifest_path}. "
            f"Run `python3 assets/tools/cook_venice.py` first."
        )

    meshes: dict[str, int] = {}
    materials: dict[str, int] = {}
    with open(manifest_path, "r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, start=1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 4:
                raise RuntimeError(f"Malformed manifest line {lineno}: {raw!r}")
            _src, kind, hex_id, cooked_file = parts[:4]
            aid = int(hex_id, 16)
            if kind == "mesh":
                meshes[cooked_file] = aid
            elif kind == "material":
                materials[cooked_file] = aid
    return meshes, materials


class SceneBuilder:
    def __init__(self) -> None:
        self.lines: list[str] = [
            "# Rime scene v1 (.rscene) — reflection-serialized world. Human-diffable; hand-edit or regenerate.",
            "# Generated deterministically by samples/venice/make_scene.py.",
            "rime_scene 1",
            "",
        ]
        self.next_entity_id = 0

    def add_comment(self, comment: str) -> None:
        for line in comment.splitlines():
            self.lines.append(f"# {line}" if line else "#")

    def add_entity(self, components: list[str]) -> int:
        eid = self.next_entity_id
        self.next_entity_id += 1
        self.lines.append(f"entity {eid} {{")
        for comp in components:
            for cline in comp.splitlines():
                self.lines.append(f"  {cline}")
        self.lines.append("}")
        self.lines.append("")
        return eid

    def transform_block(
        self,
        tx: float,
        ty: float,
        tz: float,
        rot: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 1.0),
        scale: tuple[float, float, float] = (1.0, 1.0, 1.0),
    ) -> str:
        rx, ry, rz, rw = rot
        sx, sy, sz = scale
        return "\n".join(
            [
                f"{TYPE_LOCAL_TRANSFORM} {{",
                "  value {",
                f"    translation {{ x {fmt(tx)} y {fmt(ty)} z {fmt(tz)} }}",
                f"    rotation {{ x {fmt(rx)} y {fmt(ry)} z {fmt(rz)} w {fmt(rw)} }}",
                f"    scale {{ x {fmt(sx)} y {fmt(sy)} z {fmt(sz)} }}",
                "  }",
                "}",
            ]
        )

    def add_mesh_asset(
        self,
        asset_id: int,
        tx: float,
        ty: float,
        tz: float,
        rot: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 1.0),
        scale: tuple[float, float, float] = (1.0, 1.0, 1.0),
    ) -> int:
        mesh_block = f"{TYPE_MESH_ASSET} {{\n  asset {asset_id}\n}}"
        return self.add_entity([self.transform_block(tx, ty, tz, rot, scale), mesh_block])

    def render(self) -> str:
        return "\n".join(self.lines).rstrip() + "\n"


def build_scene(meshes: dict[str, int], materials: dict[str, int], camera: str = "north") -> str:
    def req_mesh(stem: str) -> int:
        key = f"{stem}.rmesh"
        if key not in meshes:
            raise RuntimeError(f"Required cooked mesh '{key}' missing from manifest")
        return meshes[key]

    def req_mat(name: str) -> int:
        if name not in materials:
            raise RuntimeError(f"Required cooked material '{name}' missing from manifest")
        return materials[name]

    m_wall = req_mesh("venice_intonaco_wall")
    m_door = req_mesh("door_planks_transom")
    m_barred = req_mesh("window_barred_sill")
    m_shutter = req_mesh("window_shutters_istrian")
    m_workboat = req_mesh("boat_workboat")
    m_hull = req_mesh("boat_hull_only")
    m_sand = req_mesh("sand_ground")
    m_log = req_mesh("driftwood_log")
    m_grass = req_mesh("grass_clump")
    m_cockle_banded = req_mesh("cockle_banded")
    m_cockle_orange = req_mesh("cockle_orange")
    m_top_snail = req_mesh("top_snail")
    m_rose = req_mesh("rose")
    mat_masegni = req_mat("venice_masegni_ground.mat0.rmat")

    sb = SceneBuilder()

    # 1. Camera + PlayerSpawn on the quay at (-12, 1.7, 3) looking along +x with slight downward pitch.
    # Identity camera looks along -Z; yaw -90 deg rotates (0, 0, -1) -> (+1, 0, 0) (+X).
    # The "opposite" review viewpoint stands on the south fondamenta and looks back along -x (yaw +90),
    # so one generator yields both banks' views without hand-editing the committed scene.
    if camera == "opposite":
        cam_pos, cam_yaw = (12.0, 1.7, -10.0), 90.0
        cam_text = "Review camera on the opposite quay at (12, 1.7, -10), looking along -x (-7 deg pitch)."
    else:
        cam_pos, cam_yaw = (-12.0, 1.7, 3.0), -90.0
        cam_text = "Camera on the north quay at (-12, 1.7, 3), looking along +x (-7 deg pitch)."
    cam_rot = quat_yaw_pitch_deg(cam_yaw, -7.0)
    sb.add_comment(cam_text)
    sb.add_entity(
        [
            sb.transform_block(*cam_pos, cam_rot),
            "\n".join(
                [
                    f"{TYPE_CAMERA} {{",
                    "  fov_y 0.94",
                    "  z_near 0.1",
                    "  z_far 1000",
                    "  active true",
                    "}",
                ]
            ),
        ]
    )

    # Late-afternoon DirectionalLight from +x+y (slightly toward -z over the canal so the -z facade is lit).
    # Ray travel direction = q * (0, 0, -1); yaw +125 deg -> (-0.819, 0, 0.574), pitch -26 deg -> rays travel
    # toward (-x, -y, +z), i.e. sun sits in the (+x, +y, -z) sky.
    sun_rot = quat_yaw_pitch_deg(125.0, -26.0)
    sb.add_comment("Late-afternoon warm sun from +x+y and procedural Sky.")
    sb.add_entity(
        [
            sb.transform_block(0.0, 0.0, 0.0, sun_rot),
            "\n".join(
                [
                    f"{TYPE_DIRECTIONAL_LIGHT} {{",
                    "  color_r 1",
                    "  color_g 0.86",
                    "  color_b 0.68",
                    "  intensity 3.6",
                    "}",
                ]
            ),
        ]
    )

    sb.add_entity(
        [
            "\n".join(
                [
                    f"{TYPE_SKY} {{",
                    "  zenith_r 0.14",
                    "  zenith_g 0.28",
                    "  zenith_b 0.62",
                    "  horizon_r 0.78",
                    "  horizon_g 0.72",
                    "  horizon_b 0.68",
                    "  intensity 0.06",
                    "  sun_angular_radius 0.012",
                    "  cloud_coverage 0.35",
                    "  cloud_density 1",
                    "  cloud_altitude 1400",
                    "  cloud_scale 0.00035",
                    "  cloud_sharpness 0.28",
                    "  wind_x 0",
                    "  wind_z 0",
                    "  clouds true",
                    "}",
                ]
            )
        ]
    )

    spawn_rot = quat_from_yaw_deg(cam_yaw)
    sb.add_entity(
        [
            sb.transform_block(*cam_pos, spawn_rot),
            "\n".join(
                [
                    f"{TYPE_PLAYER_SPAWN} {{",
                    "  enabled true",
                    "}",
                ]
            ),
        ]
    )

    # 2. Fondamenta (quay): x -16..16, z 0..8 (centre z=4) at y=0, masegni MaterialAsset, tile_metres 8.
    sb.add_comment("Fondamenta (quay): x -16..16, z 0..8 (centre z=4) at y=0, masegni paving.")
    sb.add_entity(
        [
            sb.transform_block(0.0, 0.0, 4.0),
            f"{TYPE_MATERIAL_ASSET} {{\n  asset {mat_masegni}\n}}",
            "\n".join(
                [
                    f"{TYPE_GROUND_SURFACE} {{",
                    "  half_x 16",
                    "  half_z 4",
                    "  cells_x 8",
                    "  cells_z 2",
                    "  tile_metres 8",
                    "  thickness 1",
                    "}",
                ]
            ),
        ]
    )

    # 3. Lagoon: ONE huge water plane under everything. Anything the camera can see past the quays
    # (the open ends of the rio, the far side of the buildings) is then water to the horizon instead of
    # the sky's black void, and no tiling or seams need managing. A `MaterialRef` is only an index into
    # the host's runtime material table (slot 0 is white), so the water's colour and roughness come
    # from a cooked glTF instead: `canal_water.gltf` (make_water.py), a 1 x 1 m quad with factors only.
    # Its size is set by the camera's far plane (z_far 1000 m), not by the map: the sky draws black
    # BELOW the horizon, so wherever the plane ends a dark band shows between its edge and the sky.
    # From eye height (2.3 m above the water) an edge 200 m away sits 0.66 deg below the horizon,
    # ~6 px at 540 px; at 980 m it is 0.13 deg, ~1 px. Any farther and the far plane clips it.
    sb.add_comment(
        "STUB: lagoon water, one 1960 x 1960 m plane at y = -0.6 under the whole scene. There is no water\n"
        "simulation; a dark, smooth (roughness 0.05) factor-only material from samples/venice/canal_water.gltf."
    )
    sb.add_mesh_asset(req_mesh("canal_water"), tx=0.0, ty=-0.6, tz=0.0, scale=(1960.0, 1.0, 1960.0))

    rot_neg_z = quat_from_yaw_deg(180.0)

    def add_quay_face(sb: SceneBuilder, z_edge: float, facing_sign: int) -> None:
        # Slab centre is pushed 0.125 m outside the paving so its top cap at y = 0 never overlaps the
        # GroundSurface top (coplanar overlap would z-fight). facing_sign = -1 faces -z, +1 faces +z.
        rot = quat_from_yaw_deg(180.0 if facing_sign < 0 else 0.0)
        for bay_idx in range(8):
            cx = -14.0 + 4.0 * bay_idx
            sb.add_mesh_asset(
                m_wall,
                tx=cx,
                ty=-0.75,
                tz=z_edge + facing_sign * WALL_HALF_THICK,
                rot=rot,
                scale=(1.0, 0.25, 1.0),
            )

    # Storey boundaries shared by every facade: ground 3.6 m (tall doors), later storeys 3.0 m.
    storey_y = [0.0, 3.6, 6.6, 9.6]
    ground_pattern = ["door", "barred", "barred", "door", "barred", "door", "barred", "barred"]
    barred_sill_y = 0.9

    def add_facade(
        sb: SceneBuilder,
        z_wall: float,
        facing_sign: int,
        storeys_per_bay: list[int],
        opening_shift: int,
    ) -> float:
        """Add a facade whose front face is the plane z = z_wall, facing `facing_sign` (-1: -z, +1: +z).

        The wall body lies behind the front face (away from the canal) so the opening recesses, which
        extend toward the wall interior, are never covered by a slab. Returns the x of the second
        barred window (where the north row puts its rose).
        """
        rot = quat_from_yaw_deg(180.0 if facing_sign < 0 else 0.0)
        wall_z = z_wall - facing_sign * WALL_HALF_THICK

        def add_wall_rect(x0: float, x1: float, y0: float, y1: float) -> None:
            w = x1 - x0
            h = y1 - y0
            if w <= 1e-5 or h <= 1e-5:
                return
            sb.add_mesh_asset(
                m_wall,
                tx=0.5 * (x0 + x1),
                ty=y0,
                tz=wall_z,
                rot=rot,
                scale=(w / WALL_SLAB_W, h / WALL_SLAB_H, 1.0),
            )

        def opening_tx(bay_cx: float, x_mid: float) -> float:
            # A yaw-180 mesh mirrors local x (x_world = tx - x_local); a yaw-0 mesh does not.
            return bay_cx - facing_sign * x_mid

        barred_count = 0
        second_barred_cx = 0.0
        for bay_idx in range(8):
            kind = ground_pattern[(bay_idx - opening_shift) % 8]
            bay_x0 = -16.0 + 4.0 * bay_idx
            bay_x1 = bay_x0 + 4.0
            bay_cx = 0.5 * (bay_x0 + bay_x1)
            ground_y0, ground_y1 = storey_y[0], storey_y[1]

            if kind == "door":
                w = DOOR_BBOX["x_max"] - DOOR_BBOX["x_min"]
                x_mid = 0.5 * (DOOR_BBOX["x_min"] + DOOR_BBOX["x_max"])
                pier_w = 0.5 * (4.0 - w)
                open_x0 = bay_x0 + pier_w
                open_x1 = bay_x1 - pier_w
                add_wall_rect(bay_x0, open_x0, ground_y0, ground_y1)
                add_wall_rect(open_x1, bay_x1, ground_y0, ground_y1)
                add_wall_rect(open_x0, open_x1, DOOR_BBOX["y_max"], ground_y1)
                sb.add_mesh_asset(m_door, tx=opening_tx(bay_cx, x_mid), ty=0.0, tz=z_wall, rot=rot)
            else:
                barred_count += 1
                if barred_count == 2:
                    second_barred_cx = bay_cx
                w = BARRED_BBOX["x_max"] - BARRED_BBOX["x_min"]
                x_mid = 0.5 * (BARRED_BBOX["x_min"] + BARRED_BBOX["x_max"])
                pier_w = 0.5 * (4.0 - w)
                open_x0 = bay_x0 + pier_w
                open_x1 = bay_x1 - pier_w
                add_wall_rect(bay_x0, open_x0, ground_y0, ground_y1)
                add_wall_rect(open_x1, bay_x1, ground_y0, ground_y1)
                add_wall_rect(open_x0, open_x1, ground_y0, barred_sill_y + BARRED_BBOX["y_sill_ledge"])
                add_wall_rect(open_x0, open_x1, barred_sill_y + BARRED_BBOX["y_max"], ground_y1)
                sb.add_mesh_asset(
                    m_barred, tx=opening_tx(bay_cx, x_mid), ty=barred_sill_y, tz=z_wall, rot=rot
                )

            # Every storey above the ground floor gets one shutter window, sill 0.7 m above the floor.
            uw = SHUTTER_BBOX["x_max"] - SHUTTER_BBOX["x_min"]
            ux_mid = 0.5 * (SHUTTER_BBOX["x_min"] + SHUTTER_BBOX["x_max"])
            upier_w = 0.5 * (4.0 - uw)
            uopen_x0 = bay_x0 + upier_w
            uopen_x1 = bay_x1 - upier_w
            for s in range(1, storeys_per_bay[bay_idx]):
                y0, y1 = storey_y[s], storey_y[s + 1]
                sill_y = y0 + 0.7
                add_wall_rect(bay_x0, uopen_x0, y0, y1)
                add_wall_rect(uopen_x1, bay_x1, y0, y1)
                add_wall_rect(uopen_x0, uopen_x1, y0, sill_y + SHUTTER_BBOX["y_min"])
                add_wall_rect(uopen_x0, uopen_x1, sill_y + SHUTTER_BBOX["y_max"], y1)
                sb.add_mesh_asset(
                    m_shutter, tx=opening_tx(bay_cx, ux_mid), ty=sill_y, tz=z_wall, rot=rot
                )
        return second_barred_cx

    sb.add_comment("Quay canal face: 8 intonaco wall slabs along z = 0 facing -z, 0.75 m tall, top at y = 0.")
    add_quay_face(sb, 0.0, -1)

    # 4. Facade along z = 8, facing -z, x -16..16 = 8 bays of 4 m, two storeys (0..3.6 m, 3.6..6.6 m).
    # Wall front face sits at z = 8.0 (wall slab centres at z = 8.125); opening z = 0 planes sit at z = 8.0
    # so recesses extend into the wall's [8.0, 8.25] thickness without any wall slab covering them.
    sb.add_comment("Facade along z = 8 facing -z: 8 bays x 4 m, ground storey 0..3.6 m and upper storey 3.6..6.6 m.")
    second_barred_cx = add_facade(sb, 8.0, -1, [2] * 8, 0)
    facade_front_z = 8.0

    # 5. Rose on the sill of the second barred window (Bay 2, x = -6).
    assert second_barred_cx is not None
    sb.add_comment("Rose resting on the stone sill ledge of the second barred window.")
    sb.add_mesh_asset(
        m_rose,
        tx=second_barred_cx,
        ty=barred_sill_y + BARRED_BBOX["y_sill_ledge"],
        tz=facade_front_z - 0.01,
        rot=rot_neg_z,
    )

    def add_end_walls(sb: SceneBuilder, z_wall: float, facing_sign: int, storeys_per_bay: list[int]) -> None:
        # The facade is a single slab, paper-thin seen end-on. A return wall at each end, perpendicular
        # to the facade and running 8 m back (away from the canal), gives the block a believable depth.
        # Its height follows the end bay's storeys, and the outer face sits on x = +-16 facing outward.
        back = -facing_sign
        for x_end, bay_idx, yaw in ((-16.0, 0, -90.0), (16.0, 7, 90.0)):
            rot = quat_from_yaw_deg(yaw)
            tx = x_end - WALL_HALF_THICK if x_end > 0 else x_end + WALL_HALF_THICK
            for s in range(storeys_per_bay[bay_idx]):
                y0, y1 = storey_y[s], storey_y[s + 1]
                for k in range(2):
                    sb.add_mesh_asset(
                        m_wall,
                        tx=tx,
                        ty=y0,
                        tz=z_wall + back * (2.0 + WALL_SLAB_W * k),
                        rot=rot,
                        scale=(1.0, (y1 - y0) / WALL_SLAB_H, 1.0),
                    )

    def add_quay_ends(sb: SceneBuilder, z_near: float, z_far: float) -> None:
        # Without end faces the paving is a floating sheet when seen from the lagoon. Same 0.75 m,
        # top-at-y=0 slabs as the canal faces, pushed 0.125 m outside the paving to avoid z-fighting.
        for x_end, yaw in ((-16.0, -90.0), (16.0, 90.0)):
            tx = x_end + (WALL_HALF_THICK if x_end > 0 else -WALL_HALF_THICK)
            for k in range(2):
                z = min(z_near, z_far) + 2.0 + WALL_SLAB_W * k
                sb.add_mesh_asset(
                    m_wall,
                    tx=tx,
                    ty=-0.75,
                    tz=z,
                    rot=quat_from_yaw_deg(yaw),
                    scale=(1.0, 0.25, 1.0),
                )

    sb.add_comment("North facade end walls: x = -16 and x = +16, 8 m back (z 8..16), full end-bay height.")
    add_end_walls(sb, 8.0, -1, [2] * 8)

    # 5b. Opposite bank: fondamenta z -14..-6 so the rio reads as a canal between two rows of houses.
    sb.add_comment("South fondamenta (quay): x -16..16, z -14..-6 (centre z=-10) at y=0, masegni paving.")
    sb.add_entity(
        [
            sb.transform_block(0.0, 0.0, -10.0),
            f"{TYPE_MATERIAL_ASSET} {{\n  asset {mat_masegni}\n}}",
            "\n".join(
                [
                    f"{TYPE_GROUND_SURFACE} {{",
                    "  half_x 16",
                    "  half_z 4",
                    "  cells_x 8",
                    "  cells_z 2",
                    "  tile_metres 8",
                    "  thickness 1",
                    "}",
                ]
            ),
        ]
    )
    sb.add_comment("South quay canal face: 8 intonaco slabs along z = -6 facing +z, 0.75 m tall, top at y = 0.")
    add_quay_face(sb, -6.0, 1)
    # Bays 0-3 get a third storey so the skyline steps down; shifting the openings by one bay stops the
    # two banks mirroring each other. No rose here: it stays a single hero detail on the north row.
    south_storeys = [3, 3, 3, 3, 2, 2, 2, 2]
    sb.add_comment("South facade along z = -14 facing +z: bays 0-3 three storeys, bays 4-7 two; openings shifted one bay.")
    add_facade(sb, -14.0, 1, south_storeys, 1)
    sb.add_comment("South facade end walls: x = -16 and x = +16, 8 m back (z -14..-22).")
    add_end_walls(sb, -14.0, 1, south_storeys)

    sb.add_comment("Quay end faces at x = -16 and x = +16 for both fondamentas (0.75 m, top at y = 0).")
    add_quay_ends(sb, 0.0, 8.0)
    add_quay_ends(sb, -14.0, -6.0)

    # 6. Boats moored in the canal (y = 0 at water plane y = -0.6; 0.24 m draft is an assumption).
    sb.add_comment("Moored boats: workboat at x=-6 along +x (z=-2.5), hull_only at x=8 yawed 180 deg (z=-4.0).")
    sb.add_mesh_asset(m_workboat, tx=-6.0, ty=-0.6, tz=-2.5)
    sb.add_mesh_asset(m_hull, tx=8.0, ty=-0.6, tz=-4.0, rot=quat_from_yaw_deg(180.0))

    # 7. Lido patch at the canal's +x end: sand_ground (4x4 m tiles) covering x 16..24, z -6..2 at y = -0.05.
    sb.add_comment("Lido sand patch (x 16..24, z -6..2 at y = -0.05) with driftwood, 4 grass clumps, and 3 shells.")
    for sx in (18.0, 22.0):
        for sz in (-4.0, 0.0):
            sb.add_mesh_asset(m_sand, tx=sx, ty=-0.05, tz=sz)

    log_x, log_y, log_z = 18.5, -0.05, -1.5
    sb.add_mesh_asset(m_log, tx=log_x, ty=log_y, tz=log_z, rot=quat_from_yaw_deg(25.0))

    # 4 grass clumps (yaws 0/50/110/200 deg) and 3 shells, all within 0.5 m of the driftwood log.
    grass_placements = [
        (-0.30, 0.30, 0.0),
        (0.30, 0.28, 50.0),
        (-0.32, -0.28, 110.0),
        (0.32, -0.25, 200.0),
    ]
    for dx, dz, yaw in grass_placements:
        assert math.hypot(dx, dz) <= 0.5
        sb.add_mesh_asset(m_grass, tx=log_x + dx, ty=log_y, tz=log_z + dz, rot=quat_from_yaw_deg(yaw))

    shell_placements = [
        (m_cockle_banded, -0.08, 0.22, 15.0),
        (m_cockle_orange, 0.06, 0.25, 135.0),
        (m_top_snail, -0.01, 0.18, 260.0),
    ]
    for mesh_id, dx, dz, yaw in shell_placements:
        assert math.hypot(dx, dz) <= 0.5
        sb.add_mesh_asset(mesh_id, tx=log_x + dx, ty=log_y, tz=log_z + dz, rot=quat_from_yaw_deg(yaw))

    return sb.render()


def main() -> None:
    repo_root = Path(__file__).resolve().parent.parent.parent
    default_manifest = repo_root / "build" / "asset-cache" / "venice" / "manifest.txt"
    default_out = repo_root / "samples" / "venice" / "venice.rscene"

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=default_manifest,
        help=f"Path to cooked Venice manifest.txt (default: {default_manifest})",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=default_out,
        help=f"Output .rscene path (default: {default_out})",
    )
    parser.add_argument(
        "--camera",
        choices=("north", "opposite"),
        default="north",
        help="Viewpoint for camera + PlayerSpawn: north quay (default, committed scene) or the "
        "opposite fondamenta at (12, 1.7, -10) for review.",
    )
    args = parser.parse_args()

    meshes, materials = load_manifest(args.manifest)
    content = build_scene(meshes, materials, args.camera)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(content, encoding="utf-8")
    print(f"Wrote {args.out} ({content.count('entity ')} entities).")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        sys.stderr.write(f"make_scene.py: ERROR: {exc}\n")
        sys.exit(1)
