#!/usr/bin/env python3
"""Builds the annotated "cas d'école" of the guided mode (examples/*.silex) through the IPC/MCP API.

Usage:  python tools/examples/build_examples.py [name ...]      (default: all)

Each example is modelled exactly as the matching lesson asks (resources/tutorials/NN_*.json), then saved
with bilingual design-intent annotations (guided tour). tests/test_examples checks that every example
completes its lesson, so this script doubles as a reference solution of the lessons.
"""
from __future__ import annotations

import json
import math
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = Path(os.environ.get("SILEX_EXE", ROOT / "build" / "bin" / "Release" / "silex.exe"))
OUT = ROOT / "examples"


class Silex:
    """Minimal JSON-lines client on the Windows named pipe (no pywin32 needed)."""

    def __init__(self, design: str | None = None) -> None:
        self.pipe = f"silexex{os.getpid()}"
        self.proc = subprocess.Popen([str(EXE), "--headless", "--pipe", self.pipe] + ([design] if design else []),
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                     creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        deadline = time.time() + 30
        while True:
            try:
                self.f = open(rf"\\.\pipe\{self.pipe}", "r+b", buffering=0)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.1)
        self.buf = b""
        self.n = 0

    def __call__(self, method: str, **params):
        self.n += 1
        self.f.write(json.dumps({"id": self.n, "method": method, "params": params}).encode() + b"\n")
        while b"\n" not in self.buf:
            chunk = self.f.read(1 << 20)
            if not chunk:
                raise RuntimeError("silex closed the pipe")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        reply = json.loads(line)
        if "error" in reply:
            raise RuntimeError(f"{method}: {reply['error'].get('message')}")
        return reply.get("result")

    def close(self) -> None:
        try:
            self("shutdown")
        except Exception:
            pass
        try:
            self.proc.wait(10)
        except Exception:
            self.proc.kill()


def faces(s: Silex, body: str, flt: str = "all"):
    return s("query_topology", body_id=body, entity_type="face", filter=flt)["entities"]


def edges(s: Silex, body: str, flt: str = "all"):
    return s("query_topology", body_id=body, entity_type="edge", filter=flt)["entities"]


def near(a, b, tol=1e-3):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def sketch_data(s: Silex, sketch_id: str) -> dict:
    """Geometry and constraints of a sketch, as stored in the document (read through a scratch save)."""
    tmp = OUT / f".dof_{os.getpid()}.silex"
    s("save_design", file_path=str(tmp))
    try:
        doc = json.loads(tmp.read_text(encoding="utf-8"))
    finally:
        tmp.unlink(missing_ok=True)
    for f in doc["features"]:
        if f["id"] == sketch_id:
            return f["params"]["sketch"]
    raise RuntimeError(f"unknown sketch {sketch_id}")


def close_dof(s: Silex, sketch_id: str, first: list[dict] | None = None) -> int:
    """Fully constrains a sketch (white sketch, 0 DOF) without moving anything.

    The `first` constraints (design intent) are added as given; then candidates measured on the current
    geometry are tried one by one in a readable order: coincident ends, points on the origin, horizontal /
    vertical lines, radii, position of each horizontal / vertical line from the origin, then the x / y of the
    remaining points. The solver rejects redundant ones, so only independent dimensions are kept."""
    def add(c: dict):
        try:
            return s("sketch_add_constraints", sketch_id=sketch_id, constraints=[c])
        except RuntimeError:
            return None  # redundant (already implied) or not applicable

    dof = None
    for c in first or []:
        dof = s("sketch_add_constraints", sketch_id=sketch_id, constraints=[c])["dof"]
    sk = sketch_data(s, sketch_id)
    geos = [g for g in sk["geos"] if "ref" not in g]  # projected edges are fixed

    def pts(g):
        k, i = g["kind"], g["id"]
        if k == "Line":
            return [(f"{i}:start", g["p1"]), (f"{i}:end", g["p2"])]
        if k == "Circle":
            return [(f"{i}:center", g["c"])]
        if k == "Arc":
            c, r = g["c"], g["r"]
            return [(f"{i}:start", [c[0] + r * math.cos(g["a0"]), c[1] + r * math.sin(g["a0"])]),
                    (f"{i}:end", [c[0] + r * math.cos(g["a1"]), c[1] + r * math.sin(g["a1"])]),
                    (f"{i}:center", c)]
        if k == "Point":
            return [(f"{i}:start", g["p"])]
        return []

    points = [p for g in geos for p in pts(g)]
    tol = 1e-6
    cands: list[dict] = []
    for i, (a, pa) in enumerate(points):  # shared ends drawn separately (polylines)
        for b, pb in points[i + 1:]:
            if a.split(":")[0] != b.split(":")[0] and near(pa, pb, tol):
                cands.append({"type": "coincident", "a": a, "b": b})
    cands += [{"type": "coincident", "a": a, "b": "origin"} for a, p in points if near(p, (0, 0), tol)]
    for g in geos:
        if g["kind"] == "Line":
            (x1, y1), (x2, y2) = g["p1"], g["p2"]
            if abs(y1 - y2) < tol:
                cands.append({"type": "horizontal", "a": g["id"]})
            elif abs(x1 - x2) < tol:
                cands.append({"type": "vertical", "a": g["id"]})
        elif g["kind"] == "Circle":
            cands.append({"type": "diameter", "a": g["id"], "value": 2 * g["r"]})
        elif g["kind"] == "Arc":
            cands.append({"type": "radius", "a": g["id"], "value": g["r"]})
    for g in geos:
        if g["kind"] == "Line":
            (x1, y1), (x2, y2) = g["p1"], g["p2"]
            if abs(y1 - y2) < tol:
                cands.append({"type": "distance_y", "a": "origin", "b": f"{g['id']}:start", "value": y1})
            elif abs(x1 - x2) < tol:
                cands.append({"type": "distance_x", "a": "origin", "b": f"{g['id']}:start", "value": x1})
    for a, p in points:
        cands.append({"type": "distance_x", "a": "origin", "b": a, "value": p[0]})
        cands.append({"type": "distance_y", "a": "origin", "b": a, "value": p[1]})

    if dof is None:
        dof = s("sketch_add_constraints", sketch_id=sketch_id, constraints=[])["dof"]
    for c in cands:
        if dof == 0:
            break
        r = add(c)
        if r is not None:
            dof = r["dof"]
    if dof != 0:
        raise RuntimeError(f"sketch {sketch_id} still has {dof} DOF")
    return dof


def save(s: Silex, name: str, annotations: dict[str, tuple[str, str]]) -> Path:
    """Saves examples/<name>.silex and adds the bilingual annotations (keyed by step name)."""
    path = OUT / f"{name}.silex"
    s("save_design", file_path=str(path))
    doc = json.loads(path.read_text(encoding="utf-8"))
    names = {f["name"] for f in doc["features"]}
    missing = [k for k in annotations if k not in names]
    if missing:
        raise RuntimeError(f"{name}: annotations for unknown steps {missing} (steps: {sorted(names)})")
    for f in doc["features"]:
        if f["name"] in annotations:
            fr, en = annotations[f["name"]]
            f["annotation"] = fr
            f["annotation_en"] = en
    path.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return path


# =================================================================== Parcours 4 — parametric dice

def dice(s: Silex) -> Path:
    s("set_parameter", name="cote", expression="20", comment="Arête du dé", unit="mm")
    sk = s("create_sketch", plane_or_face_id="XY", name="Esquisse_cube")["sketch_id"]
    r = s("sketch_add_geometry", sketch_id=sk,
          elements=[{"type": "center_rectangle", "center": [0, 0], "width": 20, "height": 20, "dimension": True}])
    st = s("sketch_add_constraints", sketch_id=sk, constraints=[])
    assert st["dof"] == 0, st
    cube = s("extrude", sketch_id=sk, distance_mm=20)
    body = cube["body_id"]
    s("update_feature_parameter", feature_id=cube["feature_id"], param_name="distance", expression="cote")

    # pips: Ø3, 1 mm deep. Opposite faces add up to 7: top 1 / bottom 6, front 2 / back 5, right 3 / left 4.
    def face_sketch(name: str, plane: str, offset: float):
        return s("create_sketch", plane_or_face_id=plane, name=name, offset_mm=offset)["sketch_id"]

    def pips(name: str, plane: str, offset: float, pts, depth: float, pattern=None):
        k = face_sketch(name, plane, offset)
        els = [{"type": "circle", "center": list(p), "radius": 1.5} for p in pts]
        res = s("sketch_add_geometry", sketch_id=k, elements=els)
        if pattern:
            ids = [e["geo_ids"][0] for e in res["elements"][: pattern["first"]]]
            s("sketch_add_geometry", sketch_id=k,
              elements=[{"type": "linear_pattern", "geo_ids": ids, "count": pattern["count"],
                         "spacing": pattern["spacing"], "direction": pattern["direction"]}])
        close_dof(s, k)
        return s("extrude", sketch_id=k, distance_mm=depth, operation="cut", profile_index=-1)

    pips("Face_1", "XY", 20, [(0, 0)], -1)
    # 6 = two columns of 3: one circle, a 2D linear pattern (×3), mirrored by a second column
    pips("Face_6", "XY", 0, [(-5, -5), (5, -5)], 1, {"first": 2, "count": 3, "spacing": 5, "direction": [0, 1]})
    pips("Face_2", "XZ", -10, [(-5, 15), (5, 5)], 1)
    pips("Face_5", "XZ", 10, [(-5, 5), (5, 5), (0, 10), (-5, 15), (5, 15)], -1)
    pips("Face_3", "YZ", 10, [(-5, 15), (0, 10), (5, 5)], -1)
    pips("Face_4", "YZ", -10, [(-5, 5), (5, 5), (-5, 15), (5, 15)], 1)

    # the 12 straight edges of the cube (length 20 minus nothing: the pips do not touch them)
    es = edges(s, body)
    cube_edges = [e["id"] for e in es if e.get("geom_type") == "line"]
    s("fillet_or_chamfer", type="fillet", edge_ids=cube_edges, radius_or_distance_mm=2)
    return save(s, "de_parametrique", {
        "Esquisse_cube": ("Un rectangle par le centre (Maj+R) : centré sur l'origine, il reste symétrique quand la cote change.",
                          "A center rectangle (Shift+R): centered on the origin, it stays symmetric when the size changes."),
        "Extrude1": ("Le cube : la distance est liée au paramètre « cote » (Ctrl+P) — change-le et tout suit.",
                     "The cube: the distance is bound to the parameter 'cote' (Ctrl+P) — change it and everything follows."),
        "Face_1": ("Face du dessus : 1 point. Les faces opposées font toujours 7 (1-6, 2-5, 3-4).",
                   "Top face: 1 pip. Opposite faces always add up to 7 (1-6, 2-5, 3-4)."),
        "Face_6": ("Face du dessous : 6 points = 2 cercles répétés 3 fois par une répétition linéaire 2D.",
                   "Bottom face: 6 pips = 2 circles repeated 3 times with a 2D linear pattern."),
        "Fillet1": ("Les 12 arêtes arrondies (R2) : un dé roule mieux et s'imprime sans arêtes vives.",
                    "The 12 edges rounded (R2): a dice rolls better and prints without sharp edges."),
    })


# Frames of the origin planes (sketch x, y -> world): XY (X, Y) normal +Z; XZ (X, Z) normal -Y;
# YZ (Y, Z) normal +X. A negative offset on XZ is therefore on the +Y side.

def polyline(points, closed=True):
    n = len(points)
    segs = [(points[i], points[(i + 1) % n]) for i in range(n if closed else n - 1)]
    return [{"type": "line", "start": list(a), "end": list(b)} for a, b in segs]


def line_ids(res):
    return [e["geo_ids"][0] for e in res["elements"]]


def cyl_face(s: Silex, body: str, where, area: float | None = None, tol=0.02):
    """Cylindrical face closest to `where` (its center lies on the axis), optionally of a given area."""
    cands = [f for f in faces(s, body, "circular")
             if area is None or abs(f["area_mm2"] - area) <= tol * area]
    if not cands:
        raise RuntimeError("no such cylindrical face: " + json.dumps(
            [(f["center"], f["area_mm2"]) for f in faces(s, body, "circular")]))
    cands.sort(key=lambda f: sum((a - b) ** 2 for a, b in zip(f["center"], where)))
    return cands[0]["id"]


def hole_sketch(s: Silex, name: str, plane: str, offset: float, center, radius: float, depth: float):
    """A hole drawn in a sketch and cut by an extrusion (a feature that patterns can repeat)."""
    k = s("create_sketch", plane_or_face_id=plane, name=name, offset_mm=offset)["sketch_id"]
    s("sketch_add_geometry", sketch_id=k, elements=[{"type": "circle", "center": list(center), "radius": radius}])
    close_dof(s, k)
    return s("extrude", sketch_id=k, distance_mm=depth, operation="cut", profile_index=-1)


# =================================================================== Parcours 5 — C-clamp frame

def clamp(s: Silex) -> Path:
    sk = s("create_sketch", plane_or_face_id="XY", name="Profil_C")["sketch_id"]
    pts = [(0, 0), (60, 0), (60, 15), (15, 15), (15, 65), (60, 65), (60, 80), (0, 80)]
    ids = line_ids(s("sketch_add_geometry", sketch_id=sk, elements=polyline(pts)))
    # 2D fillets (Shift+F): inner corners R5 (stress), outer back corners R8
    s("sketch_add_geometry", sketch_id=sk, elements=[
        {"type": "sketch_fillet", "a": ids[2], "b": ids[3], "radius": 5},
        {"type": "sketch_fillet", "a": ids[3], "b": ids[4], "radius": 5},
        {"type": "sketch_fillet", "a": ids[7], "b": ids[0], "radius": 8},
        {"type": "sketch_fillet", "a": ids[6], "b": ids[7], "radius": 8},
    ])
    close_dof(s, sk)  # 0 DOF: corners, H/V, then the position of each side from the origin
    body = s("extrude", sketch_id=sk, distance_mm=12)["body_id"]
    # ribs: a line from the spine to the jaw on the mid plane (Z = 6) -> T section in the inner corners
    for name, a, b in (("Nervure_bas", (15, 32), (32, 15)), ("Nervure_haut", (15, 48), (32, 65))):
        r = s("create_sketch", plane_or_face_id="XY", name=name, offset_mm=6)["sketch_id"]
        s("sketch_add_geometry", sketch_id=r, elements=[{"type": "line", "start": list(a), "end": list(b)}])
        close_dof(s, r)
        s("create_rib", sketch_id=r, thickness_mm=4, body_id=body)
    # screw boss on the upper jaw (axis along Y), M8 tapped: Ø18 boss, 6.8 mm tap drill
    s("create_cylinder", diameter_mm=18, height_mm=25, center=[45, 6], plane_or_face_id="XZ", offset_mm=-85,
      operation="join", target_body_id=body)
    s("create_cylinder", diameter_mm=6.8, height_mm=25, center=[45, 6], plane_or_face_id="XZ", offset_mm=-85,
      operation="cut", target_body_id=body)
    s("create_thread", face_id=cyl_face(s, body, (45, 72.5, 6), area=math.pi * 6.8 * 25), size="M8", clearance_mm=0.15)
    return save(s, "pince_serrage", {
        "Profil_C": ("Le profil en C : un seul contour fermé. Les congés 2D (Maj+F) dans les angles intérieurs évitent la concentration de contraintes.",
                     "The C profile: one closed contour. 2D fillets (Shift+F) in the inner corners avoid stress concentration."),
        "Extrude1": ("12 mm d'épaisseur : le cadre doit rester rigide quand on serre.",
                     "12 mm thick: the frame must stay stiff when clamping."),
        "Nervure_bas": ("Une simple ligne sur le plan milieu (Z = 6) : la nervure (Rib) remplit jusqu'aux parois et forme un T qui rigidifie l'angle.",
                        "A simple line on the mid plane (Z = 6): the rib fills up to the walls and forms a T that stiffens the corner."),
        "Cylinder1": ("Le bossage Ø18 donne assez de matière autour du taraudage.",
                      "The Ø18 boss leaves enough material around the tapped hole."),
        "Cylinder2": ("Perçage au diamètre de taraudage : 6,8 mm pour du M8 (et non 8 mm !).",
                      "Hole at the tap drill diameter: 6.8 mm for M8 (not 8 mm!)."),
        "Thread1": ("Taraudage M8 réel avec 0,15 mm de jeu FDM : la vis imprimée entrera sans forcer.",
                    "Real M8 thread with 0.15 mm FDM clearance: the printed screw goes in without forcing."),
    })


# =================================================================== Parcours 6 — V-belt pulley

def pulley(s: Silex) -> Path:
    sk = s("create_sketch", plane_or_face_id="XY", name="Demi_profil")["sketch_id"]
    # half profile above the X axis (x = along the axis, y = radius): hub, web, rim with a 40° V groove
    pts = [(-5, 0), (25, 0), (25, 15), (14, 15), (14, 30), (20, 30), (20, 40), (15.5, 40), (12.5, 32),
           (7.5, 32), (4.5, 40), (0, 40), (0, 30), (6, 30), (6, 15), (-5, 15)]
    s("sketch_add_geometry", sketch_id=sk, elements=polyline(pts))
    close_dof(s, sk)
    rev = s("revolve", sketch_id=sk, axis_geo_id="X", angle_deg=360)
    body = rev["body_id"]
    s("create_cylinder", diameter_mm=12, height_mm=40, center=[0, 0], plane_or_face_id="YZ", offset_mm=-10,
      operation="cut", target_body_id=body)
    k = s("create_sketch", plane_or_face_id="YZ", name="Clavette", offset_mm=-10)["sketch_id"]
    s("sketch_add_geometry", sketch_id=k,
      elements=[{"type": "slot", "center": [0, 6], "length": 2, "width": 4, "angle": 90, "dimension": True}])
    close_dof(s, k)
    s("extrude", sketch_id=k, distance_mm=40, operation="cut", profile_index=-1)
    hole = hole_sketch(s, "Trou_allegement", "YZ", 0, (0, 22.5), 5, 25)
    s("circular_pattern", target_type="feature", target_ids=[hole["feature_id"]], axis_id="X", count=6)
    return save(s, "poulie_gorge", {
        "Demi_profil": ("Seule la moitié du profil est dessinée, au-dessus de l'axe X : la révolution fera le reste. La gorge en V fait 40°.",
                        "Only half of the profile is drawn, above the X axis: the revolve does the rest. The V groove is 40°."),
        "Revolve1": ("Révolution 360° autour de X : moyeu, voile et jante en une seule fonction.",
                     "360° revolve around X: hub, web and rim in a single feature."),
        "Cylinder1": ("L'alésage Ø12 pour l'arbre.", "The Ø12 bore for the shaft."),
        "Clavette": ("La rainure de clavette : une oblongue (U) de 4 mm de large qui mord dans l'alésage.",
                     "The keyway: a 4 mm wide slot (U) cutting into the bore."),
        "CircularPattern1": ("Un seul trou d'allègement, répété 6 fois autour de X : change le nombre et tout se recalcule.",
                             "One lightening hole, repeated 6 times around X: change the count and everything updates."),
    })


# =================================================================== Parcours 7 — pillow block bearing

def pillow(s: Silex) -> Path:
    base = s("create_box", length_mm=45, width_mm=30, height_mm=10, center=[22.5, 0])
    body = base["body_id"]
    s("create_box", length_mm=20, width_mm=30, height_mm=30, center=[10, 0], operation="join", target_body_id=body)
    s("create_cylinder", diameter_mm=44, height_mm=30, center=[0, 30], plane_or_face_id="XZ", offset_mm=-15,
      operation="join", target_body_id=body)
    k = s("create_sketch", plane_or_face_id="XY", name="Oblong", offset_mm=10)["sketch_id"]
    s("sketch_add_geometry", sketch_id=k,
      elements=[{"type": "slot", "center": [35, 0], "length": 8, "width": 9, "angle": 0, "dimension": True}])
    close_dof(s, k)
    s("extrude", sketch_id=k, distance_mm=-10, operation="cut", profile_index=-1)
    s("mirror", target_type="body", target_ids=[body], mirror_plane_id="YZ", merge_result=True)
    b = s("create_sketch", plane_or_face_id="XZ", name="Logement", offset_mm=-15)["sketch_id"]
    r = s("sketch_add_geometry", sketch_id=b, elements=[
        {"type": "circle", "center": [0, 30], "radius": 22, "construction": True},
        {"type": "circle", "center": [0.5, 30.5], "radius": 13}])
    outer, bore = line_ids(r)
    s("sketch_add_constraints", sketch_id=b, constraints=[
        {"type": "concentric", "a": outer, "b": bore},
        {"type": "diameter", "a": bore, "value": 26}])
    close_dof(s, b)
    s("extrude", sketch_id=b, distance_mm=30, operation="cut", profile_index=-1)
    s("create_cylinder", diameter_mm=4, height_mm=-12, center=[0, 0], plane_or_face_id="XY", offset_mm=52,
      operation="cut", target_body_id=body)
    return save(s, "palier_semelle", {
        "Box1": ("On ne modélise que la moitié droite : la semelle.", "Only the right half is modelled: the base."),
        "Cylinder1": ("Le corps du palier, centré sur l'axe Z : le plan de symétrie YZ le coupe en deux.",
                      "The bearing housing, centered on the Z axis: the YZ symmetry plane cuts it in half."),
        "Oblong": ("Un trou oblong (U) : il laisse ± 4 mm de réglage pour aligner l'arbre au montage.",
                   "A slot (U): it leaves ± 4 mm of adjustment to align the shaft at assembly."),
        "Mirror1": ("La symétrie (Mirror) crée l'autre moitié, oblong compris : une seule cote à changer.",
                    "The Mirror creates the other half, slot included: a single dimension to change."),
        "Logement": ("Logement du roulement Ø26, rendu concentrique (O) au corps Ø44 : l'épaisseur reste uniforme.",
                     "Ø26 bearing seat, made concentric (O) with the Ø44 housing: the wall stays uniform."),
        "Cylinder2": ("Le trou de graissage, sur le dessus, débouche dans le logement.",
                      "The grease hole, on top, opens into the bearing seat."),
    })


# =================================================================== Parcours 8 — square-to-round duct

def duct(s: Silex) -> Path:
    pl = s("create_construction_plane", mode="offset", base="XY", offset_mm=60, name="Plan_sortie")
    plane = pl.get("plane_id", pl.get("feature_id"))
    a = s("create_sketch", plane_or_face_id="XY", name="Entree_carree")["sketch_id"]
    s("sketch_add_geometry", sketch_id=a,
      elements=[{"type": "center_rectangle", "center": [0, 0], "width": 60, "height": 60, "dimension": True}])
    b = s("create_sketch", plane_or_face_id=plane, name="Sortie_ronde")["sketch_id"]
    r = s("sketch_add_geometry", sketch_id=b, elements=[{"type": "circle", "center": [0, 0], "radius": 20}])
    c = line_ids(r)[0]
    s("sketch_add_constraints", sketch_id=b, constraints=[
        {"type": "coincident", "a": f"{c}:center", "b": "origin"}, {"type": "diameter", "a": c, "value": 40}])
    lo = s("loft", profile_sketch_ids=[a, b])
    body = lo["body_id"]
    ends = [f["id"] for f in faces(s, body, "top")] + [f["id"] for f in faces(s, body, "bottom")]
    s("shell", body_id=body, removed_face_ids=ends, thickness_mm=2)
    pr = s("check_printability", build_direction="+Z")
    print("duct printability:", json.dumps(pr)[:300])
    return save(s, "raccord_carre_rond", {
        "Plan_sortie": ("Un plan de construction décalé de 60 mm : c'est là que sera la sortie ronde.",
                        "A construction plane offset by 60 mm: the round outlet goes there."),
        "Entree_carree": ("Entrée carrée 60 × 60, rectangle par le centre (Maj+R) : symétrique par construction.",
                          "Square 60 × 60 inlet, center rectangle (Shift+R): symmetric by construction."),
        "Sortie_ronde": ("Sortie Ø40, centrée : le lissage reliera le carré au cercle.",
                         "Ø40 outlet, centered: the loft will connect the square to the circle."),
        "Loft1": ("Le lissage (Maj+L) passe d'une section à l'autre sans arête brusque.",
                  "The loft (Shift+L) blends one section into the other without a sharp break."),
        "Shell1": ("Coque de 2 mm en retirant les deux faces d'extrémité : le raccord est creux et ouvert des deux côtés.",
                   "2 mm shell removing both end faces: the adapter is hollow and open at both ends."),
    })


# =================================================================== Parcours 9 — flanged pipe elbow

def elbow(s: Silex) -> Path:
    s("set_parameter", name="diametre_tube", expression="30", comment="Diamètre extérieur du tube", unit="mm")
    s("set_parameter", name="epaisseur_paroi", expression="3", comment="Épaisseur de la paroi", unit="mm")
    path = s("create_sketch", plane_or_face_id="XZ", name="Trajectoire")["sketch_id"]
    s("sketch_add_geometry", sketch_id=path,
      elements=[{"type": "arc", "center": [45, 0], "radius": 45, "start_angle": 90, "end_angle": 180}])
    close_dof(s, path)
    sec = s("create_sketch", plane_or_face_id="XY", name="Section")["sketch_id"]
    r = s("sketch_add_geometry", sketch_id=sec, elements=[
        {"type": "circle", "center": [0, 0], "radius": 15}, {"type": "circle", "center": [0, 0], "radius": 12}])
    outer, inner = line_ids(r)
    cr = s("sketch_add_constraints", sketch_id=sec, constraints=[
        {"type": "coincident", "a": f"{outer}:center", "b": "origin"},
        {"type": "concentric", "a": outer, "b": inner},
        {"type": "diameter", "a": outer, "value": 30},
        {"type": "diameter", "a": inner, "value": 24}])
    cids = cr.get("constraint_ids") or cr.get("ids")
    s("update_feature_parameter", feature_id=sec, param_name=cids[-2], expression="diametre_tube")
    s("update_feature_parameter", feature_id=sec, param_name=cids[-1], expression="diametre_tube - 2 * epaisseur_paroi")
    sw = None
    for idx in (0, 1):
        sw = s("sweep", profile_sketch_id=sec, path_sketch_id=path, profile_index=idx)
        v = s("inspect_body", body_id=sw["body_id"])["volume_mm3"]
        if abs(v - math.pi * (15 ** 2 - 12 ** 2) * (math.pi / 2 * 45)) < 200:
            break
        s("undo")
    body = sw["body_id"]
    # flanges Ø70 × 8 at both ends, bored to the inner diameter
    s("create_cylinder", diameter_mm=70, height_mm=-8, center=[0, 0], plane_or_face_id="XY", operation="join", target_body_id=body)
    s("create_cylinder", diameter_mm=24, height_mm=-8, center=[0, 0], plane_or_face_id="XY", operation="cut", target_body_id=body)
    s("create_cylinder", diameter_mm=70, height_mm=8, center=[0, 45], plane_or_face_id="YZ", offset_mm=45, operation="join", target_body_id=body)
    s("create_cylinder", diameter_mm=24, height_mm=8, center=[0, 45], plane_or_face_id="YZ", offset_mm=45, operation="cut", target_body_id=body)
    # 4 × M5 clearance holes (Ø5.5) on a Ø50 bolt circle, circular pattern on each flange
    h1 = hole_sketch(s, "Trou_bride_1", "XY", 0, (25, 0), 2.75, -8)
    s("circular_pattern", target_type="feature", target_ids=[h1["feature_id"]], axis_id="Z", count=4)
    h2 = hole_sketch(s, "Trou_bride_2", "YZ", 45, (25, 45), 2.75, 8)
    s("circular_pattern", target_type="feature", target_ids=[h2["feature_id"]],
      axis_id=cyl_face(s, body, (49, 0, 45), area=math.pi * 70 * 8), count=4)
    return save(s, "coude_brides", {
        "Trajectoire": ("La trajectoire : un arc de 90° de rayon 45 mm, dans le plan XZ.",
                        "The path: a 90° arc of radius 45 mm, in the XZ plane."),
        "Section": ("La section annulaire, perpendiculaire au départ de la trajectoire. Ses diamètres suivent les paramètres diametre_tube et epaisseur_paroi (Ctrl+P).",
                    "The ring section, perpendicular to the start of the path. Its diameters follow the parameters diametre_tube and epaisseur_paroi (Ctrl+P)."),
        "Sweep1": ("Le balayage (Maj+W) pousse la section le long de l'arc : un coude creux d'un seul coup.",
                   "The sweep (Shift+W) pushes the section along the arc: a hollow elbow in one go."),
        "Cylinder1": ("Bride d'entrée Ø70 × 8, puis alésée au diamètre intérieur.",
                      "Inlet flange Ø70 × 8, then bored to the inner diameter."),
        "Cylinder3": ("Bride de sortie, sur le plan YZ décalé à l'extrémité de l'arc.",
                      "Outlet flange, on the YZ plane offset to the end of the arc."),
        "CircularPattern1": ("4 trous M5 (Ø5,5 de passage) sur un cercle Ø50 : un trou + une répétition circulaire.",
                             "4 M5 holes (Ø5.5 clearance) on a Ø50 circle: one hole + a circular pattern."),
    })


EXAMPLES = {"dice": dice, "clamp": clamp, "pulley": pulley, "pillow": pillow, "duct": duct, "elbow": elbow}


def main(argv: list[str]) -> int:
    names = argv or list(EXAMPLES)
    for n in names:
        s = Silex()
        try:
            p = EXAMPLES[n](s)
            print(f"{n}: {p}")
        finally:
            s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
