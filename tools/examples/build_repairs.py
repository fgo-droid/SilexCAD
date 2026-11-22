#!/usr/bin/env python3
"""Builds the broken parts of the repair challenges ("Défis de dépannage", Learn panel).

Usage:  python tools/examples/build_repairs.py

For each challenge N it writes
  resources/tutorials/repair_N_*.silex   the broken part the student opens (embedded in the application)
  tests/data/repair_N_fixed.silex        one correct repair (ctest: the broken part fails the challenge,
                                         the repaired one passes it, robustness test included)
The parts are modelled through the IPC API, then broken on purpose by editing the saved JSON when the
API refuses to create a failing step (that is the point of the exercise).
"""
from __future__ import annotations

import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "examples"))
from build_examples import Silex, close_dof, faces, line_ids  # noqa: E402

TUT = ROOT / "resources" / "tutorials"
FIX = ROOT / "tests" / "data"
TMP = ROOT / "build" / "repairs_tmp"


def load(p: Path) -> dict:
    return json.loads(p.read_text(encoding="utf-8"))


def dump(doc: dict, p: Path) -> None:
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(doc, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")


def feature(doc: dict, name: str) -> dict:
    return next(f for f in doc["features"] if f["name"] == name)


# ------------------------------------------------------------------ 1. the elastic sketch

def elastic(fixed: bool) -> dict:
    """Plate whose width is the parameter `largeur`; the holes are dimensioned from the LEFT edge only,
    so at 80 mm they are off-center. Fix: dimension the second hole from the right edge (or symmetry)."""
    s = Silex()
    try:
        s("set_parameter", name="largeur", expression="60", comment="Largeur de la plaque", unit="mm")
        sk = s("create_sketch", plane_or_face_id="XY", name="Plaque")["sketch_id"]
        r = s("sketch_add_geometry", sketch_id=sk, elements=[
            {"type": "rectangle", "corner": [0, 0], "width": 60, "height": 40},
            {"type": "circle", "center": [15, 20], "radius": 3},
            {"type": "circle", "center": [45, 20], "radius": 3}])
        rect = r["elements"][0]["edges"]
        c1, c2 = r["elements"][1]["geo_ids"][0], r["elements"][2]["geo_ids"][0]
        cs = s("sketch_add_constraints", sketch_id=sk, constraints=[
            {"type": "coincident", "a": f"{rect['bottom']}:start", "b": "origin"},
            {"type": "distance", "a": rect["bottom"], "value": 60},
            {"type": "distance", "a": rect["left"], "value": 40},
            {"type": "diameter", "a": c1, "value": 6},
            {"type": "equal", "a": c1, "b": c2},
            {"type": "distance_y", "a": "origin", "b": f"{c1}:center", "value": 20},
            {"type": "distance_y", "a": "origin", "b": f"{c2}:center", "value": 20},
            {"type": "distance_x", "a": "origin", "b": f"{c1}:center", "value": 15},
            ({"type": "distance_x", "a": f"{c2}:center", "b": f"{rect['right']}:start", "value": 15} if fixed else
             {"type": "distance_x", "a": "origin", "b": f"{c2}:center", "value": 45})])
        assert cs["dof"] == 0, cs
        s("update_feature_parameter", feature_id=sk, param_name=cs["constraint_ids"][1], expression="largeur")
        # only the plate region (the circles are holes, not discs): the region whose volume is right
        for idx in range(3):
            ex = s("extrude", sketch_id=sk, distance_mm=8, profile_index=idx)
            if abs(s("inspect_body", body_id=ex["body_id"])["volume_mm3"] - (60 * 40 - 2 * math.pi * 9) * 8) < 1:
                break
            s("undo")
        else:
            raise RuntimeError("plate region not found")
        out = TMP / f"elastic_{fixed}.silex"
        s("save_design", file_path=str(out))
        return load(out)
    finally:
        s.close()


# ------------------------------------------------------------------ 2. the stuck lid

def lid(fixed: bool) -> dict:
    """Tray + lid with a lip sized by `jeu_montage`: a negative play makes the lip collide."""
    s = Silex()
    try:
        s("set_parameter", name="longueur", expression="60", comment="Longueur extérieure", unit="mm")
        s("set_parameter", name="jeu_montage", expression="0.2" if fixed else "-0.2", comment="Jeu entre lèvre et bac", unit="mm")
        base = s("create_box", length_mm=60, width_mm=40, height_mm=20, center=[0, 0])
        body = base["body_id"]
        s("update_feature_parameter", feature_id=base["feature_id"], param_name="length", expression="longueur")
        pocket = s("create_box", length_mm=56, width_mm=36, height_mm=18, center=[0, 0], offset_mm=2,
                   operation="cut", target_body_id=body)
        s("update_feature_parameter", feature_id=pocket["feature_id"], param_name="length", expression="longueur - 4")
        top = s("create_box", length_mm=60, width_mm=40, height_mm=2, center=[0, 0], offset_mm=20)
        lidb = top["body_id"]
        s("update_feature_parameter", feature_id=top["feature_id"], param_name="length", expression="longueur")
        lip = s("create_box", length_mm=56, width_mm=36, height_mm=-3, center=[0, 0], offset_mm=20,
                operation="join", target_body_id=lidb)
        s("update_feature_parameter", feature_id=lip["feature_id"], param_name="length", expression="longueur - 4 - 2 * jeu_montage")
        s("update_feature_parameter", feature_id=lip["feature_id"], param_name="width", expression="36 - 2 * jeu_montage")
        hollow = s("create_box", length_mm=52, width_mm=32, height_mm=-3.5, center=[0, 0], offset_mm=20,
                   operation="cut", target_body_id=lidb)
        s("update_feature_parameter", feature_id=hollow["feature_id"], param_name="length", expression="longueur - 8")
        out = TMP / f"lid_{fixed}.silex"
        s("save_design", file_path=str(out))
        return load(out)
    finally:
        s.close()


# ------------------------------------------------------------------ 3. the impossible fillet

def fillet(fixed: bool) -> dict:
    """Plate of thickness `epaisseur` with a fillet larger than the plate (red step)."""
    s = Silex()
    try:
        s("set_parameter", name="epaisseur", expression="10", comment="Épaisseur de la plaque", unit="mm")
        b = s("create_box", length_mm=60, width_mm=40, height_mm=10, center=[0, 0])
        s("update_feature_parameter", feature_id=b["feature_id"], param_name="height", expression="epaisseur")
        top_edges = [e["id"] for e in s("query_topology", body_id=b["body_id"], entity_type="edge", filter="top")["entities"]]
        f = s("fillet_or_chamfer", type="fillet", edge_ids=top_edges, radius_or_distance_mm=3)
        out = TMP / f"fillet_{fixed}.silex"
        s("save_design", file_path=str(out))
        doc = load(out)
        if not fixed:
            feature(doc, "Fillet1")["params"]["value"] = 12.0  # larger than the plate: impossible
        return doc
    finally:
        s.close()


# ------------------------------------------------------------------ 4. the open contour

def contour(fixed: bool) -> dict:
    """L profile whose last corner is not closed: the extrusion has no profile (red step)."""
    s = Silex()
    try:
        s("set_parameter", name="hauteur", expression="40", comment="Hauteur du profil en L", unit="mm")
        sk = s("create_sketch", plane_or_face_id="XY", name="Profil_L")["sketch_id"]
        pts = [(0, 0), (50, 0), (50, 10), (10, 10), (10, 40), (0, 40)]
        ids = line_ids(s("sketch_add_geometry", sketch_id=sk, elements=[
            {"type": "line", "start": list(pts[i]), "end": list(pts[(i + 1) % 6])} for i in range(6)]))
        close_dof(s, sk)
        out0 = TMP / "contour_dims.silex"
        s("save_design", file_path=str(out0))
        # bind the vertical position of the top edge to `hauteur`
        d = load(out0)
        top = ids[4]
        cid = next(c["id"] for c in feature(d, "Profil_L")["params"]["sketch"]["constraints"]
                   if c["kind"] == "DistanceY" and c["b"][0] == top)
        s("update_feature_parameter", feature_id=sk, param_name=cid, expression="hauteur")
        s("extrude", sketch_id=sk, distance_mm=20, profile_index=-1)
        out = TMP / f"contour_{fixed}.silex"
        s("save_design", file_path=str(out))
        doc = load(out)
        if not fixed:
            # open the corner (0, 0): the last line stops 3 mm short and the coincidence is removed
            sk_j = feature(doc, "Profil_L")["params"]["sketch"]
            last = ids[5]
            for g in sk_j["geos"]:
                if g["id"] == last:
                    g["p2"] = [0.0, 3.0]
            sk_j["constraints"] = [c for c in sk_j["constraints"]
                                   if not (c["kind"] == "Coincident" and any(r[0] == last and r[1] == 2 for r in (c.get("a"), c.get("b")) if r))]
        return doc
    finally:
        s.close()


# ------------------------------------------------------------------ 5. the STEP part to modify

def step_part(fixed: bool) -> dict:
    """Imported support (no history) with a useless boss and a hole at the wrong place."""
    TMP.mkdir(parents=True, exist_ok=True)
    stp = TMP / "support.step"
    s = Silex()
    try:
        b = s("create_box", length_mm=80, width_mm=40, height_mm=8, center=[0, 0])
        body = b["body_id"]
        s("create_box", length_mm=8, width_mm=40, height_mm=40, center=[36, 0], offset_mm=8, operation="join", target_body_id=body)
        s("create_cylinder", diameter_mm=16, height_mm=10, center=[10, 0], offset_mm=8, operation="join", target_body_id=body)
        s("create_cylinder", diameter_mm=6.4, height_mm=8, center=[-25, 0], operation="cut", target_body_id=body)
        s("export_step", file_path=str(stp), body_id=body)
    finally:
        s.close()
    s = Silex()
    try:
        imp = s("import_step", file_path=str(stp))
        body = imp["body_id"]
        if fixed:
            # delete the boss (its side + top faces) and the old hole, drill the new one at x = -15
            fs = faces(s, body)
            boss = [f["id"] for f in fs if abs(f["center"][0] - 10) < 0.5 and abs(f["center"][1]) < 0.5 and f["center"][2] > 8.01]
            s("defeature_faces", face_ids=boss)
            fs = faces(s, body, "circular")
            hole = [f["id"] for f in fs if abs(f["center"][0] + 25) < 0.5]
            s("defeature_faces", face_ids=hole)
            base_top = max((f for f in faces(s, body, "planar") if abs(f["center"][2] - 8) < 1e-3),
                           key=lambda f: f["area_mm2"])
            top, fc = base_top["id"], base_top["center"]
            s("create_fastener_hole", plane_or_face_id=top, centers=[[-15 - fc[0], -fc[1]]], kind="clearance", size="M6", fit="close")
        out = TMP / f"step_{fixed}.silex"
        s("save_design", file_path=str(out))
        doc = load(out)
        for f in doc["features"]:
            if f["type"] == "ImportStep":
                f["name"] = "Support_importe"
                f["params"]["sourcePath"] = "support.step"
        return doc
    finally:
        s.close()


CHALLENGES = [
    ("repair_1_esquisse_elastique", elastic),
    ("repair_2_couvercle_bloque", lid),
    ("repair_3_conge_impossible", fillet),
    ("repair_4_contour_ouvert", contour),
    ("repair_5_piece_step", step_part),
]


def main() -> int:
    TMP.mkdir(parents=True, exist_ok=True)
    for i, (name, build) in enumerate(CHALLENGES, 1):
        dump(build(False), TUT / f"{name}.silex")
        dump(build(True), FIX / f"repair_{i}_fixed.silex")
        print(name, "ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
