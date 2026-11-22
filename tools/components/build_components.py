#!/usr/bin/env python3
"""Builds the simplified STEP models of the component library (resources/components/*.step).

Usage:  python tools/components/build_components.py [id ...]      (default: all)

Each resources/components/<id>.json holds the catalog metadata (bilingual name, mounting holes, screw,
openings) and a geometry recipe: an optional PCB (size, thickness, corner radius, mounting holes) and
parts (boxes, cylinders, a USB-C "stadium") placed from the top of the PCB. Silex models it through the
IPC API and exports the STEP that the application embeds. The models are deliberately light: board
outline, mounting holes at the official positions and connector envelopes — enough to place standoffs,
project edges in a sketch and cut the openings of an enclosure.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "examples"))
from build_examples import Silex, edges  # noqa: E402

LIB = ROOT / "resources" / "components"


def build(s: Silex, c: dict) -> Path:
    body = None

    def add(kind: str, mode: str, **kw):
        nonlocal body
        op = "new_body" if mode == "new" or body is None else mode
        r = s(kind, operation=op, **({"target_body_id": body} if body and op != "new_body" else {}), **kw)
        if body is None:
            body = r["body_id"]
        return r

    def round_vertical(radius: float):
        es = [e["id"] for e in s("query_topology", body_id=body, entity_type="edge", filter="vertical")["entities"]]
        if radius > 0 and es:
            s("fillet_or_chamfer", type="fillet", edge_ids=es, radius_or_distance_mm=radius)

    base = 0.0
    if "pcb" in c:
        (L, W), t = c["pcb"]["size"], c["pcb"]["thickness"]
        add("create_box", "new", length_mm=L, width_mm=W, height_mm=t, center=[L / 2, W / 2])
        round_vertical(c["pcb"].get("corner_radius", 0))
        for h in c.get("mounting_holes", []):
            add("create_cylinder", "cut", diameter_mm=h["d"], height_mm=t + 2, center=[h["x"], h["y"]], offset_mm=-1)
        base = t
    for p in c["parts"]:
        mode = p.get("mode", "join")
        if "box" in p:
            cx, cy, z0, lx, ly, h = p["box"]
            add("create_box", mode, length_mm=lx, width_mm=ly, height_mm=h, center=[cx, cy], offset_mm=base + z0)
            if p.get("corner_radius"):
                round_vertical(p["corner_radius"])
        elif "cylinder" in p:
            cx, cy, z0, d, h = p["cylinder"]
            add("create_cylinder", mode, diameter_mm=d, height_mm=h, center=[cx, cy], offset_mm=base + z0)
        elif "stadium" in p:
            # USB-C shell: rounded slot (w x h) extruded along +Y from y = y0 (XZ plane: normal -Y)
            cx, y0, w, h, depth = p["stadium"]
            add("create_box", mode, length_mm=w - h, width_mm=h, height_mm=-depth, center=[cx, h / 2],
                plane_or_face_id="XZ", offset_mm=-y0)
            for sx in (-1, 1):
                add("create_cylinder", "join", diameter_mm=h, height_mm=-depth, center=[cx + sx * (w - h) / 2, h / 2],
                    plane_or_face_id="XZ", offset_mm=-y0)
    path = LIB / c["step"]
    s("export_step", file_path=str(path), body_id=body)
    info = s("inspect_body", body_id=body)
    print(f"{c['id']:18} {info.get('volume_mm3', 0):9.0f} mm3  -> {path.name}")
    return path


def main(argv: list[str]) -> int:
    specs = sorted(LIB.glob("*.json"))
    for f in specs:
        c = json.loads(f.read_text(encoding="utf-8"))
        if argv and c["id"] not in argv:
            continue
        s = Silex()
        try:
            build(s, c)
        finally:
            s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
