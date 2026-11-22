"""Builds examples/boitier_raspberry_pi4.silex through the IPC bridge (README main image).

    python tools/examples/build_rpi_case.py [output.silex]

Raspberry Pi 4 enclosure (enclosure wizard, lip closure) with standoffs, every connector opening (Ethernet,
2 x USB, USB-C, 2 x micro HDMI, audio, micro SD), ventilation slots and an engraved logo; lid lifted 30 mm.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "build" / "bin" / "Release"))
import mcp_server as m  # noqa: E402

OUT = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "examples" / "boitier_raspberry_pi4.silex")
c = m.SilexClient("silex_rpi_case")
def cut_box(cx, cy, z0, lx, wy, h, target):
    return c.call("create_box", length_mm=lx, width_mm=wy, height_mm=h, center=[cx, cy], plane_or_face_id="XY",
                  offset_mm=z0, operation="cut", target_body_id=target)
try:
    e = c.call("create_enclosure", inner_length_mm=90, inner_width_mm=60, inner_height_mm=26, wall_mm=2,
               corner_radius_mm=4, closure="lip")
    base, lid = e["base_body_id"], e["lid_body_id"]
    for s in e["steps"]: print(s)
    for b in (base, lid): print(c.call("inspect_body", body_id=b)["bbox"])
    DX, DY, DZ = -41.3, -30.0, 5.0          # board corner in the world; PCB bottom on 3 mm standoffs
    pi = c.call("insert_component", component_id="rpi4b", position=[DX, DY, DZ])["body_id"]
    print("pi", c.call("inspect_body", body_id=pi)["bbox"])
    top = DZ + 1.4
    # standoffs + M2.5 pilot holes
    for hx, hy in ((3.5, 3.5), (61.5, 3.5), (3.5, 52.5), (61.5, 52.5)):
        c.call("create_cylinder", diameter_mm=6, height_mm=3.2, center=[DX + hx, DY + hy], plane_or_face_id="XY",
               offset_mm=1.8, operation="join", target_body_id=base)
        c.call("create_cylinder", diameter_mm=2.3, height_mm=3.5, center=[DX + hx, DY + hy], plane_or_face_id="XY",
               offset_mm=1.6, operation="cut", target_body_id=base)
    # +X wall: Ethernet, USB 3.0, USB 2.0
    cut_box(46, DY + 45.75, top - 0.4, 8, 16.9, 14.3, base)
    cut_box(46, DY + 27, top - 0.4, 8, 14.1, 16.8, base)
    cut_box(46, DY + 9, top - 0.4, 8, 14.1, 16.8, base)
    # -Y wall: USB-C power, 2 x micro HDMI, audio jack
    cut_box(DX + 11.2, -31, top - 0.6, 10.5, 6, 4.4, base)
    cut_box(DX + 26, -31, top - 0.6, 8.7, 6, 4.7, base)
    cut_box(DX + 39.5, -31, top - 0.6, 8.7, 6, 4.7, base)
    cut_box(DX + 53.5, -31, top - 0.4, 7.4, 6, 6.6, base)
    # -X wall: micro SD card slot under the board
    cut_box(-46, DY + 28, 2.4, 8, 13, 3.2, base)
    # lid: ventilation slots over the SoC + engraved logo
    for k in range(8):
        cut_box(-28 + 4.5 * k, 4, 26, 2.2, 30, 8, lid)
    faces = c.call("query_topology", body_id=lid, entity_type="face", filter="top")["entities"]
    topf = max(faces, key=lambda f: f.get("area", 0))["id"]
    t = c.call("create_emboss_text", face_id=topf, text="Raspberry Pi 4", height_mm=5, depth_mm=-0.6,
               offset_xy=[0, -21])
    print("text", t["status"], round(t["stroke_width_mm"], 2))
    c.call("set_body_material", body_id=base, color="#ECEDEF", name="Boitier")
    c.call("set_body_material", body_id=lid, color="#F08A24", name="Couvercle")
    c.call("transform_body", body_ids=[lid], dz_mm=30)
    for s in c.call("get_model_tree")["features"]:
        if s.get("status") != "OK": print("!!", s.get("name"), s.get("status"), s.get("message"))
    print(c.call("save_design", file_path=OUT)["file_path"])
finally:
    c.call("shutdown")
