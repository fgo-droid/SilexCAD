"""Silex MCP bridge.

Exposes the Silex CAD application to MCP clients (Claude Code, Gemini CLI, ...) over stdio.
It forwards every tool call to the running application through its local IPC endpoint
(Windows named pipe \\\\.\\pipe\\cad_mcp, Unix socket /tmp/cad_mcp.sock) using
newline-delimited JSON-RPC. If no instance is listening, it starts `silex --headless`.

stdout belongs to the MCP protocol: nothing else is ever printed there. The CAD process is
started with its stdout/stderr redirected away, so OpenCascade / Qt logs cannot leak in.

Environment variables:
    SILEX_EXE   path to silex(.exe). Without it the bridge looks, in order, next to this script,
                in the PATH, then in the build tree of a source checkout (../../build/bin/Release).
    SILEX_PIPE  IPC endpoint name              (default: cad_mcp)

Self-test (no MCP client needed):
    python mcp_server.py --check   # binary, Python modules, IPC pipe and protocol version
"""

from __future__ import annotations

import base64
import itertools
import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any, Literal

try:
    from mcp.server.fastmcp import FastMCP, Image
    MCP_IMPORT_ERROR: Exception | None = None
except ImportError as _e:  # --check must work even before `pip install -r requirements.txt`
    MCP_IMPORT_ERROR = _e
    Image = Any  # type: ignore[misc,assignment]

    class FastMCP:  # type: ignore[no-redef]
        def __init__(self, *_a: Any, **_k: Any) -> None:
            pass

        def tool(self, *_a: Any, **_k: Any):
            return lambda fn: fn

PIPE_NAME = os.environ.get("SILEX_PIPE", "cad_mcp")
# v0.5 lot 5: Socratic tutor — only observation tools are exposed, the application refuses the others
TUTOR_MODE = "--tutor-mode" in sys.argv or os.environ.get("SILEX_TUTOR") == "1"
READ_ONLY_TOOLS = {
    "get_model_tree", "inspect_body", "list_solids", "query_topology", "capture_viewport", "get_pedagogical_context",
    "get_parameters", "measure", "check_interference", "check_printability", "list_components", "check_build_volume",
    "get_active_document_info", "verify_dimensions",
}
# protocol of the application this bridge was written for (ping -> protocol_version)
REQUIRED_PROTOCOL = 2
IS_WINDOWS = sys.platform == "win32"
SCRIPT_DIR = Path(__file__).resolve().parent
EXE_NAME = "silex.exe" if IS_WINDOWS else "silex"


def find_silex_exe() -> tuple[Path | None, str]:
    """Locates the application without any absolute path: (path or None, how it was found)."""
    env = os.environ.get("SILEX_EXE")
    if env:
        return (Path(env), "SILEX_EXE") if Path(env).is_file() else (None, f"SILEX_EXE={env} does not exist")
    beside = SCRIPT_DIR / EXE_NAME
    if beside.is_file():
        return beside, "next to mcp_server.py"
    on_path = shutil.which("silex")
    if on_path:
        return Path(on_path), "PATH"
    # source checkout: tools/mcp_bridge/ -> <repo>/build/bin/<config>/
    for config in ("Release", "RelWithDebInfo"):
        dev = SCRIPT_DIR.parents[1] / "build" / "bin" / config / EXE_NAME
        if dev.is_file():
            return dev, f"source build tree ({config})"
    return None, "not found (set SILEX_EXE, or put mcp_server.py next to silex.exe)"


def log(msg: str) -> None:
    """Diagnostics go to stderr only (stdout is the MCP channel)."""
    print(f"[silex-mcp] {msg}", file=sys.stderr, flush=True)


class IpcError(RuntimeError):
    """An IPC failure. Errors returned by Silex carry a stable `error_code` and a `hint`."""

    def __init__(self, message: str, error_code: str = "IPC_FAILURE", hint: str = "") -> None:
        self.error_code = error_code
        self.hint = hint
        text = f"[{error_code}] {message}"
        if hint:
            text += f"\nHint: {hint}"
        super().__init__(text)


# --------------------------------------------------------------------------- transport

class _Connection:
    """One duplex line-oriented connection to the application."""

    def __init__(self, pipe: str = PIPE_NAME) -> None:
        self._buf = b""
        if IS_WINDOWS:
            import pywintypes  # noqa: F401  (pywin32)
            import win32file

            self._win32file = win32file
            self._h = win32file.CreateFile(
                rf"\\.\pipe\{pipe}",
                win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                0, None, win32file.OPEN_EXISTING, 0, None,
            )
        else:
            self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self._sock.connect(f"/tmp/{pipe}.sock")

    def send_line(self, data: bytes) -> None:
        if IS_WINDOWS:
            self._win32file.WriteFile(self._h, data + b"\n")
        else:
            self._sock.sendall(data + b"\n")

    def read_line(self) -> bytes:
        while b"\n" not in self._buf:
            if IS_WINDOWS:
                _, chunk = self._win32file.ReadFile(self._h, 1 << 20)
            else:
                chunk = self._sock.recv(1 << 20)
            if not chunk:
                raise IpcError("Silex closed the connection")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return line

    def close(self) -> None:
        try:
            if IS_WINDOWS:
                self._win32file.CloseHandle(self._h)
            else:
                self._sock.close()
        except Exception:
            pass


class SilexClient:
    def __init__(self, pipe: str = PIPE_NAME, extra_args: list[str] | None = None) -> None:
        self.pipe = pipe
        self.extra_args = extra_args or []
        self.started = False  # True when this client launched its own headless instance
        self._conn: _Connection | None = None
        self._ids = itertools.count(1)
        self._lock = threading.Lock()
        self._proc: subprocess.Popen | None = None

    def _connect(self) -> _Connection:
        if self._conn is not None:
            return self._conn
        try:
            self._conn = _Connection(self.pipe)
            return self._conn
        except Exception:
            pass
        self._start_application()
        deadline = time.time() + 60
        last: Exception | None = None
        while time.time() < deadline:
            try:
                self._conn = _Connection(self.pipe)
                return self._conn
            except Exception as e:  # not listening yet
                last = e
                if self._proc is not None and self._proc.poll() is not None:
                    raise IpcError(f"silex exited with code {self._proc.returncode}") from e
                time.sleep(0.25)
        raise IpcError(f"Could not connect to Silex on '{self.pipe}': {last}", "IPC_UNAVAILABLE")

    def _start_application(self) -> None:
        exe, how = find_silex_exe()
        if exe is None:
            raise IpcError(f"No Silex instance is listening and silex was {how}", "SILEX_NOT_FOUND",
                           "Start Silex, or set SILEX_EXE to the full path of silex.exe.")
        log(f"starting {exe} ({how}) --headless --pipe {self.pipe}")
        flags = subprocess.CREATE_NO_WINDOW if IS_WINDOWS else 0
        self.started = True
        self._proc = subprocess.Popen(
            [str(exe), "--headless", "--pipe", self.pipe] + (["--tutor-mode"] if TUTOR_MODE else []) + self.extra_args,
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=flags, cwd=str(exe.parent),
        )

    def call(self, method: str, **params: Any) -> Any:
        with self._lock:
            for attempt in (1, 2):
                conn = self._connect()
                req_id = next(self._ids)
                try:
                    conn.send_line(json.dumps({"id": req_id, "method": method, "params": params}).encode())
                    reply = json.loads(conn.read_line())
                    break
                except (OSError, IpcError) as e:
                    conn.close()
                    self._conn = None
                    if attempt == 2:
                        raise IpcError(f"IPC failure: {e}") from e
            if "error" in reply:
                err = reply["error"]
                raise IpcError(err.get("message", "unknown error"), err.get("error_code", "ERROR"), err.get("hint", ""))
            return reply.get("result")

    def close(self) -> None:
        if self._conn is not None:
            self._conn.close()
            self._conn = None


client = SilexClient()
mcp = FastMCP("silex")

# --------------------------------------------------------------------------- A. inspection

@mcp.tool()
def get_model_tree() -> dict:
    """Return the parametric timeline (id, type, name, parameters, rebuild status OK/Warning/Error),
    the bodies and the reference planes XY, XZ, YZ."""
    return client.call("get_model_tree")


@mcp.tool()
def inspect_body(body_id: str = "") -> dict:
    """Volume (mm3), area (mm2), center of mass, bounding box and B-Rep validity
    (BRepCheck_Analyzer) of a body. body_id may be omitted when there is a single body."""
    return client.call("inspect_body", body_id=body_id)


@mcp.tool()
def list_solids(body_id: str = "") -> dict:
    """List every solid of a body (index, volume, center, bounding box, size). Useful to measure the
    components of an imported STEP assembly (e.g. a PCB with its connectors) before designing around it."""
    return client.call("list_solids", body_id=body_id)


@mcp.tool()
def query_topology(body_id: str = "",
                   entity_type: Literal["face", "edge"] = "face",
                   filter: Literal["all", "top", "bottom", "vertical", "planar", "circular"] = "all") -> dict:
    """List faces or edges with their persistent topological id (stable across rebuilds — use it
    for sketches on faces, fillets, chamfers and shells), geometric type, normal/direction,
    area/length and center."""
    return client.call("query_topology", body_id=body_id, entity_type=entity_type, filter=filter)


@mcp.tool()
def capture_viewport(view: Literal["iso", "front", "top", "right"] = "iso",
                     width: int = 1024, height: int = 768) -> Image:
    """Render the model offscreen from a standard view and return a PNG image."""
    r = client.call("capture_viewport", view=view, width=width, height=height)
    return Image(data=base64.b64decode(r["data"]), format="png")

# --------------------------------------------------------------------------- B. sketches

@mcp.tool()
def create_sketch(plane_or_face_id: str, name: str = "", offset_mm: float = 0.0) -> dict:
    """Create an empty sketch on 'XY', 'XZ', 'YZ' or on a planar face id (from query_topology).
    offset_mm shifts the plane along its normal (e.g. stacked loft sections, a lid above a box).
    Returns sketch_id and the plane frame (sketch x/y map to plane x_dir and normal x x_dir)."""
    return client.call("create_sketch", plane_or_face_id=plane_or_face_id, name=name, offset_mm=offset_mm)


@mcp.tool()
def sketch_add_geometry(sketch_id: str, elements: list[dict]) -> dict:
    """Add 2D geometry (sketch coordinates, mm). Elements:
      {"type":"line","start":[x,y],"end":[x,y]}
      {"type":"rectangle","corner":[x,y],"width":w,"height":h}  or  {"type":"rectangle","centered":true,"center":[x,y],"width":w,"height":h}
      {"type":"circle","center":[x,y],"radius":r}
      {"type":"arc","center":[x,y],"radius":r,"start_angle":deg,"end_angle":deg}   (counter-clockwise)
    Smart (pre-constrained) primitives — "dimension": true also adds their driving dimensions:
      {"type":"center_rectangle","center":[x,y],"width":w,"height":h}   (or "corner":[x,y]) H/V sides, construction
            diagonals and a center point held at their middle; a center at [0,0] is attached to the origin.
            Returns named edges bottom/right/top/left. Fully constrained by 2 dimensions.
      {"type":"slot","center1":[x,y],"center2":[x,y],"width":w}   center-to-center obround, or
      {"type":"slot","center":[x,y],"length":center_distance,"width":w,"angle":deg}   by its overall center.
            2 tangent lines + 2 equal half-circles + construction axis (named axis, arc1, arc2, side1, side2).
      {"type":"polygon","center":[x,y],"sides":6,"radius":r,"mode":"inscribed"|"circumscribed","angle":deg}
            regular polygon (circumscribed: radius = apothem, e.g. hexagonal nut across flats / 2).
    2D editing tools (on existing geometry ids):
      {"type":"sketch_fillet","a":line_id,"b":line_id,"radius":r}   or {"type":"sketch_fillet","point":"<line>:end","radius":r}
            tangent arc at the shared corner; returns driving_constraint_id (the radius dimension).
      {"type":"offset","geo_ids":[...],"distance":d}   parallel copy of chains (d > 0 outwards for closed contours),
            driven by one distance dimension (driving_constraint_id).
      {"type":"trim","geo_id":id,"point":[x,y]}     removes the portion of the curve around point, between intersections.
      {"type":"extend","geo_id":id,"point":[x,y]}   extends the end nearest to point up to the next curve.
      {"type":"mirror","geo_ids":[...],"axis":line_id|"x_axis"|"y_axis"}   mirrored copies tied by symmetry constraints.
      {"type":"linear_pattern","geo_ids":[...],"count":n,"spacing":mm,"direction":[dx,dy] (or "angle":deg)}
      {"type":"circular_pattern","geo_ids":[...],"center":"origin"|point_id,"count":n,"total_angle":360}
    Any element accepts "construction": true. Returns geo_ids and point_ids ("<geo>:start|end|center")."""
    return client.call("sketch_add_geometry", sketch_id=sketch_id, elements=elements)


@mcp.tool()
def sketch_add_constraints(sketch_id: str, constraints: list[dict]) -> dict:
    """Add constraints and solve with PlaneGCS. Each constraint: {"type": ..., "a": ref, "b": ref, "value": v}
    types: coincident, horizontal, vertical, parallel, perpendicular, tangent, equal, distance,
    distance_x, distance_y, radius, diameter, angle (degrees), fixed, point_on_object,
    midpoint (a = point, b = line or arc), symmetry (a, b = two points / lines / circles / arcs, "axis" = line id,
    "x_axis" or "y_axis"), concentric (two circles / arcs), collinear (two lines).
    refs: a geo_id (curve), a point_id ("<geo>:start|end|center"), "origin", "x_axis", "y_axis".
    Returns remaining degrees of freedom (dof) and the constraint state. Over-constraining is refused."""
    return client.call("sketch_add_constraints", sketch_id=sketch_id, constraints=constraints)

# --------------------------------------------------------------------------- C. 3D operations

@mcp.tool()
def extrude(sketch_id: str, distance_mm: float,
            operation: Literal["new_body", "join", "cut", "intersect"] = "new_body",
            profile_index: int = 0, symmetric: bool = False) -> dict:
    """Extrude a closed profile of a sketch along its normal (negative distance = opposite side).
    profile_index -1 extrudes every closed region. Returns feature_id and body_id."""
    return client.call("extrude", sketch_id=sketch_id, distance_mm=distance_mm, operation=operation,
                       profile_index=profile_index, symmetric=symmetric)


@mcp.tool()
def revolve(sketch_id: str, axis_geo_id: str = "Y", angle_deg: float = 360.0,
            operation: Literal["new_body", "join", "cut"] = "new_body") -> dict:
    """Revolve the sketch profiles around 'X' / 'Y' (sketch axes) or around a sketch line geo_id."""
    return client.call("revolve", sketch_id=sketch_id, axis_geo_id=axis_geo_id, angle_deg=angle_deg,
                       operation=operation)


@mcp.tool()
def fillet_or_chamfer(type: Literal["fillet", "chamfer"], edge_ids: list[str],
                      radius_or_distance_mm: float) -> dict:
    """Round (fillet) or bevel (chamfer) edges given by persistent edge ids from query_topology."""
    return client.call("fillet_or_chamfer", type=type, edge_ids=edge_ids,
                       radius_or_distance_mm=radius_or_distance_mm)


@mcp.tool()
def create_box(length_mm: float, width_mm: float, height_mm: float, center: list[float] | None = None,
               plane_or_face_id: str = "XY", offset_mm: float = 0.0,
               operation: Literal["new_body", "join", "cut", "intersect"] = "new_body",
               target_body_id: str | None = None) -> dict:
    """Parametric box without a sketch: base centered on `center` ([x, y] in the plane, default [0, 0]) of
    'XY' / 'XZ' / 'YZ' or of a planar face id; length along the plane x, width along y, height along the
    normal (negative = other side). Editable later in the timeline (update_feature_parameter: length,
    width, height, centerX, centerY)."""
    params: dict[str, Any] = dict(length_mm=length_mm, width_mm=width_mm, height_mm=height_mm,
                                  center=center or [0.0, 0.0], plane_or_face_id=plane_or_face_id,
                                  offset_mm=offset_mm, operation=operation)
    if target_body_id:
        params["target_body_id"] = target_body_id
    return client.call("create_box", **params)


@mcp.tool()
def create_cylinder(diameter_mm: float, height_mm: float, center: list[float] | None = None,
                    plane_or_face_id: str = "XY", offset_mm: float = 0.0,
                    operation: Literal["new_body", "join", "cut", "intersect"] = "new_body",
                    target_body_id: str | None = None) -> dict:
    """Parametric cylinder without a sketch (axis along the plane normal), e.g. a boss (join) or a hole
    (operation="cut") on a face. Same placement rules as create_box."""
    params: dict[str, Any] = dict(diameter_mm=diameter_mm, height_mm=height_mm, center=center or [0.0, 0.0],
                                  plane_or_face_id=plane_or_face_id, offset_mm=offset_mm, operation=operation)
    if target_body_id:
        params["target_body_id"] = target_body_id
    return client.call("create_cylinder", **params)


@mcp.tool()
def press_pull(face_id: str, distance_mm: float) -> dict:
    """Press / Pull a planar face of a solid (face id from query_topology): positive distance pulls it out
    (adds material), negative pushes it in (removes material). Recorded as an editable timeline step."""
    return client.call("press_pull", face_id=face_id, distance_mm=distance_mm)


@mcp.tool()
def remove_body(body_ids: list[str]) -> dict:
    """Remove bodies from this point of the timeline (a 'RemoveBody' step: the features that created them
    are kept, undo restores them). Useful to drop a construction / tool body."""
    return client.call("remove_body", body_ids=body_ids)


@mcp.tool()
def defeature_faces(face_ids: list[str]) -> dict:
    """Delete faces of a solid (fillets, chamfers, pockets, holes) and heal it by extending the neighbouring
    faces (BRepAlgoAPI_Defeaturing). Works on imported STEP bodies without history too."""
    return client.call("defeature_faces", face_ids=face_ids)


@mcp.tool()
def split_body(body_id: str, plane: str = "XY", offset_mm: float = 0.0, pegs: bool = False, peg_count: int = 2,
               peg_diameter_mm: float = 4.0, peg_length_mm: float = 8.0, peg_clearance_mm: float = 0.2) -> dict:
    """Split a body with a plane: 'XY' / 'XZ' / 'YZ' (+ offset_mm along its normal), a planar face id, or a
    construction plane id. The parts are named <name>_1 (below the plane, keeps the body id) and <name>_2.
    Typical use: model a closed box in one piece, then split it into base + lid. pegs=True (large prints cut in
    two): peg_count pins on part 1, spread inside the cut section, and matching holes with the radial FDM
    clearance in part 2 so the halves glue back without offset."""
    return client.call("split_body", body_id=body_id, plane=plane, offset_mm=offset_mm, pegs=pegs,
                       peg_count=peg_count, peg_diameter_mm=peg_diameter_mm, peg_length_mm=peg_length_mm,
                       peg_clearance_mm=peg_clearance_mm)


@mcp.tool()
def transform_body(body_ids: list[str], dx_mm: float = 0.0, dy_mm: float = 0.0, dz_mm: float = 0.0,
                   rx_deg: float = 0.0, ry_deg: float = 0.0, rz_deg: float = 0.0, copy: bool = False) -> dict:
    """Move / rotate bodies (rotations X then Y then Z around the center of their bounding box, then the
    translation), or create moved copies with copy=True. Recorded as an editable 'TransformBody' step."""
    return client.call("transform_body", body_ids=body_ids, dx_mm=dx_mm, dy_mm=dy_mm, dz_mm=dz_mm,
                       rx_deg=rx_deg, ry_deg=ry_deg, rz_deg=rz_deg, copy=copy)


@mcp.tool()
def create_construction_plane(mode: Literal["offset", "angle", "mid"] = "offset", base: str = "XY",
                              offset_mm: float = 0.0, axis: str = "X", angle_deg: float = 45.0,
                              face_ids: list[str] | None = None, name: str = "") -> dict:
    """Parametric reference plane (timeline + "Construction" folder). offset: `base` ('XY'/'XZ'/'YZ', a planar
    face id or another plane id) shifted by offset_mm; angle: `base` turned by angle_deg around `axis`
    ('X'/'Y'/'Z' or a straight edge id); mid: halfway between two parallel planar faces (face_ids).
    The returned plane_id can be used as plane_or_face_id of create_sketch / create_box / split_body."""
    params: dict[str, Any] = dict(mode=mode, base=base, offset_mm=offset_mm, axis=axis, angle_deg=angle_deg)
    if face_ids:
        params["face_ids"] = face_ids
    if name:
        params["name"] = name
    return client.call("create_construction_plane", **params)


@mcp.tool()
def create_rib(sketch_id: str, thickness_mm: float, line_geo_id: str | None = None, body_id: str | None = None) -> dict:
    """Reinforcing rib: an open line drawn in a sketch (e.g. on a plane through an L bracket, from one wall
    to the other) becomes a wall of thickness_mm, centered on the sketch plane, filling the space between
    the line and the walls of the body."""
    params: dict[str, Any] = dict(sketch_id=sketch_id, thickness_mm=thickness_mm)
    if line_geo_id:
        params["line_geo_id"] = line_geo_id
    if body_id:
        params["body_id"] = body_id
    return client.call("create_rib", **params)


@mcp.tool()
def align_bodies(moving_face_id: str, fixed_face_id: str, mode: Literal["face", "axis"] = "face",
                 offset_mm: float = 0.0, flip: bool = False) -> dict:
    """Position a body against another without computing coordinates. face: the planar face of the moving
    body is put face to face with the fixed face, with a gap offset_mm (e.g. 0.25 mm assembly clearance),
    centers aligned; axis: cylindrical faces / circular edges made coaxial (screw holes). flip reverses the
    orientation. Recorded as a parametric TransformBody step (recomputed when the bodies change)."""
    return client.call("align_bodies", moving_face_id=moving_face_id, fixed_face_id=fixed_face_id, mode=mode,
                       offset_mm=offset_mm, flip=flip)


@mcp.tool()
def check_printability(build_direction: Literal["+Z", "-Z", "+Y", "-Y", "+X", "-X"] = "+Z", min_wall_mm: float = 0.8,
                       body_ids: list[str] | None = None) -> dict:
    """FDM printability per body: overhang areas (45-60 deg from vertical = risky, > 60 deg = supports needed;
    faces lying on the plate ignored) and areas of walls thinner than min_wall_mm (0.8 = 2 lines of a 0.4 nozzle).
    Try several build directions to find the orientation without supports."""
    params: dict[str, Any] = dict(build_direction=build_direction, min_wall_mm=min_wall_mm)
    if body_ids:
        params["body_ids"] = body_ids
    return client.call("check_printability", **params)


@mcp.tool()
def create_thread(face_id: str, size: Literal["M3", "M4", "M5", "M6", "M8", "M10", "M12"] = "M6",
                  fine_pitch: bool = False, clearance_mm: float = 0.15, length_mm: float = 0.0,
                  modeled: bool = True) -> dict:
    """Real ISO metric thread on a cylindrical face: a shaft (screw, model it at the nominal diameter) or a
    hole (nut / tapped hole, model it at the tap drill: M6 -> 5 mm). clearance_mm = radial FDM play so a printed
    screw and nut assemble without forcing. length_mm = 0: the whole face. modeled=False: cosmetic (annotation)."""
    return client.call("create_thread", face_id=face_id, size=size, fine_pitch=fine_pitch,
                       clearance_mm=clearance_mm, length_mm=length_mm, modeled=modeled)


@mcp.tool()
def create_fastener_hole(plane_or_face_id: str, centers: list[list[float]],
                         kind: Literal["clearance", "counterbore", "countersink", "insert", "nut"] = "clearance",
                         size: Literal["M2", "M2.5", "M3", "M4", "M5", "M6"] = "M3",
                         fit: Literal["close", "normal"] = "normal", depth_mm: float = 0.0,
                         nut_clearance_mm: float = 0.15, angle_deg: float = 0.0, offset_mm: float = 0.0,
                         target_body_id: str = "") -> dict:
    """Standard fastener hole(s) drilled into a planar face (or XY / XZ / YZ / a construction plane) along the
    inward normal, at `centers` ([x, y] in the plane frame: on a face the origin is the face centroid).
    kind: clearance (close +0.2 / normal +0.4 mm), counterbore (DIN 912 socket head), countersink (DIN 7991,
    90 degrees), insert (heat-set brass insert pocket, e.g. M3 -> 4.0 x 5.7 mm with a 0.4 mm chamfer), nut
    (ISO 4032 hex nut trap + clearance hole). depth_mm = 0: through all. One timeline step (FastenerHole)."""
    return client.call("create_fastener_hole", plane_or_face_id=plane_or_face_id, centers=centers, kind=kind,
                       size=size, fit=fit, depth_mm=depth_mm, nut_clearance_mm=nut_clearance_mm,
                       angle_deg=angle_deg, offset_mm=offset_mm, target_body_id=target_body_id)


@mcp.tool()
def lay_flat_on_bed(face_id: str, center: bool = True) -> dict:
    """3D printing: rotate and move the body owning this planar face so that the face rests on the build plate
    (Z = 0), centered on the plate (X = Y = 0) unless center=False. One timeline step (recomputed with the
    model). A warning tells when part of the body would go below the plate."""
    return client.call("lay_flat_on_bed", face_id=face_id, center=center)


@mcp.tool()
def check_build_volume(preset: Literal["prusa_mk4", "ender3", "bambu", "prusa_mini", "custom"] = "prusa_mk4",
                       x_mm: float = 0, y_mm: float = 0, z_mm: float = 0) -> dict:
    """Does every printable body fit in the printer (MK4 / Ender 3 220x220x250, Bambu P1/X1 256^3, Prusa Mini
    180^3, or custom)? The plate is centered on the origin and starts at Z = 0. Library components are ignored."""
    params = {"preset": preset}
    if preset == "custom":
        params.update(x_mm=x_mm, y_mm=y_mm, z_mm=z_mm)
    return client.call("check_build_volume", **params)


@mcp.tool()
def create_enclosure(inner_length_mm: float, inner_width_mm: float, inner_height_mm: float, wall_mm: float = 2.0,
                     corner_radius_mm: float = 3.0, closure: Literal["lip", "screws", "slide"] = "lip",
                     clearance_mm: float = 0.2) -> dict:
    """Enclosure wizard: writes a commented, parametric timeline (parameters int_longueur / int_largeur /
    int_hauteur / paroi / rayon_coins -> Box -> Fillet -> Shell -> Split -> closure). closure: lip (snap-fit
    lip under the lid), screws (4 M3 posts with heat-set insert pockets + countersunk lid holes), slide (sliding
    lid in grooves). One undo step; returns the base and lid body ids."""
    return client.call("create_enclosure", inner_length_mm=inner_length_mm, inner_width_mm=inner_width_mm,
                       inner_height_mm=inner_height_mm, wall_mm=wall_mm, corner_radius_mm=corner_radius_mm,
                       closure=closure, clearance_mm=clearance_mm)


@mcp.tool()
def list_components(lang: Literal["fr", "en"] = "fr") -> dict:
    """Built-in library of electronic components (Raspberry Pi 4B / 5 / Pico, ESP32 DevKit, Arduino Uno,
    OLED 0.96", 6x6 push button, USB-C receptacle, 40 mm fan): id, name, mounting holes (component frame),
    screw size, recommended standoff, connector openings."""
    return client.call("list_components", lang=lang)


@mcp.tool()
def insert_component(component_id: str, position: list[float] | None = None, rz_deg: float = 0.0) -> dict:
    """Insert a component of the library as a locked reference body (pre-assigned material; it cannot be
    modified, printed or exported with the parts, but it can be moved / aligned, projected in sketches and
    checked for interference). `position` [x, y, z] / rz_deg add a Move step."""
    params = {"component_id": component_id}
    if position is not None:
        params["position"] = position
    if rz_deg:
        params["rz_deg"] = rz_deg
    return client.call("insert_component", **params)


@mcp.tool()
def get_pedagogical_context(lang: Literal["fr", "en"] = "fr") -> dict:
    """Tutor mode ("Active le mode tuteur : guide-moi sans dessiner à ma place"): returns what the student is
    stuck on — remaining degrees of freedom of every sketch explained in plain language (with the tool to
    use), failing timeline steps with an actionable explanation, the recent mistakes of the user, the
    session events and the active lesson step of the guided mode — plus tutor_guidelines. In tutor mode,
    only call read-only tools and let the user do the modelling."""
    return client.call("get_pedagogical_context", lang=lang)


@mcp.tool()
def shell(body_id: str, removed_face_ids: list[str], thickness_mm: float,
          direction: Literal["inside", "outside"] = "inside") -> dict:
    """Hollow a body with the given wall thickness (walls grow inside or outside the body);
    removed faces become openings."""
    return client.call("shell", body_id=body_id, removed_face_ids=removed_face_ids, thickness_mm=thickness_mm,
                       direction=direction)


@mcp.tool()
def mirror(target_type: Literal["body", "feature"], target_ids: list[str], mirror_plane_id: str,
           merge_result: bool = True) -> dict:
    """Symmetric copy of bodies (target_type="body", body ids) or of features (target_type="feature",
    ids of extrusions / revolutions, e.g. a hole) across a plane: 'XY', 'XZ', 'YZ' or a planar face id from
    query_topology. With merge_result the copy is fused with the original when they touch."""
    return client.call("mirror", target_type=target_type, target_ids=target_ids, mirror_plane_id=mirror_plane_id,
                       merge_result=merge_result)


@mcp.tool()
def linear_pattern(target_type: Literal["body", "feature"], target_ids: list[str], direction_1_id: str,
                   count_1: int, spacing_1_mm: float, reverse_1: bool = False, symmetric_1: bool = False,
                   direction_2_id: str | None = None, count_2: int = 1, spacing_2_mm: float = 10.0,
                   reverse_2: bool = False, symmetric_2: bool = False) -> dict:
    """Rectangular array of bodies or features. Directions: 'X', 'Y', 'Z', a straight edge id or a planar face
    id (its normal). count_1 >= 2 (original included); count_2 = 1 disables the second direction. symmetric
    places count-1 copies on each side of the original. Occurrences are TNP-indexed (i, j)."""
    return client.call("linear_pattern", target_type=target_type, target_ids=target_ids,
                       direction_1_id=direction_1_id, count_1=count_1, spacing_1_mm=spacing_1_mm,
                       reverse_1=reverse_1, symmetric_1=symmetric_1, direction_2_id=direction_2_id,
                       count_2=count_2, spacing_2_mm=spacing_2_mm, reverse_2=reverse_2, symmetric_2=symmetric_2)


@mcp.tool()
def circular_pattern(target_type: Literal["body", "feature"], target_ids: list[str], axis_id: str, count: int,
                     total_angle_deg: float = 360.0, equal_spacing: bool = True, reverse: bool = False) -> dict:
    """Polar array of bodies or features around 'X', 'Y', 'Z', a circular edge id or a cylindrical face id.
    equal_spacing spreads count occurrences over total_angle_deg; otherwise total_angle_deg is the step.
    Occurrences are TNP-indexed (k): chamfers / fillets on existing occurrences survive a count change."""
    return client.call("circular_pattern", target_type=target_type, target_ids=target_ids, axis_id=axis_id,
                       count=count, total_angle_deg=total_angle_deg, equal_spacing=equal_spacing, reverse=reverse)

@mcp.tool()
def sweep(profile_sketch_id: str, path_sketch_id: str | None = None, path_edge_ids: list[str] | None = None,
          orientation: Literal["corrected_frenet", "frenet", "fixed"] = "corrected_frenet",
          operation: Literal["new_body", "join", "cut", "intersect"] = "new_body", profile_index: int = 0) -> dict:
    """Sweep a closed profile along a path (BRepOffsetAPI_MakePipeShell): pipes, ducts, handles.
    The path is either a sketch (lines / arcs forming one open or closed chain, e.g. on another plane) or a
    chain of model edge ids. Place the profile at the start of the path, perpendicular to it.
    corrected_frenet (default) avoids twisting the profile along 3D curves."""
    params: dict[str, Any] = dict(profile_sketch_id=profile_sketch_id, orientation=orientation,
                                  operation=operation, profile_index=profile_index)
    if path_sketch_id:
        params["path_sketch_id"] = path_sketch_id
    if path_edge_ids:
        params["path_edge_ids"] = path_edge_ids
    return client.call("sweep", **params)


@mcp.tool()
def loft(profile_sketch_ids: list[str], ruled: bool = False, solid: bool = True,
         operation: Literal["new_body", "join", "cut"] = "new_body") -> dict:
    """Loft (BRepOffsetAPI_ThruSections) through >= 2 closed profiles, in order, lying on distinct planes
    (use create_sketch(..., offset_mm=...)). Typical use: square-to-round adapters, transitions between sections.
    ruled = straight surfaces between consecutive sections."""
    return client.call("loft", profile_sketch_ids=profile_sketch_ids, ruled=ruled, solid=solid, operation=operation)


@mcp.tool()
def combine(target_body_id: str, tool_body_ids: list[str],
            operation: Literal["join", "cut", "intersect"] = "join", keep_tools: bool = False) -> dict:
    """Boolean between existing bodies: target ∪ tools (join), target − tools (cut) or target ∩ tools
    (intersect). Tool bodies are consumed unless keep_tools is true. Returns the remaining body ids."""
    return client.call("combine", target_body_id=target_body_id, tool_body_ids=tool_body_ids,
                       operation=operation, keep_tools=keep_tools)

# --------------------------------------------------------------------------- D. editing, history, I/O

@mcp.tool()
def update_feature_parameter(feature_id: str, param_name: str, new_value: float | None = None,
                             expression: str | None = None) -> dict:
    """Change a numeric parameter and rebuild incrementally. Extrude: distance; Revolve: angle; Shell: thickness;
    CircularPattern: count, total_angle_deg; LinearPattern: count_1, spacing_1_mm, count_2, spacing_2_mm;
    Fillet/Chamfer: radius|distance; Sketch: param_name = id of a dimensional constraint (angles in degrees).
    Give either new_value, or expression to bind the field to global parameters (e.g. "epaisseur_paroi * 2"):
    the feature then follows every change of those parameters. A change that breaks the rebuild is undone."""
    params: dict[str, Any] = dict(feature_id=feature_id, param_name=param_name)
    if expression is not None:
        params["expression"] = expression
    elif new_value is not None:
        params["new_value"] = new_value
    else:
        raise ValueError("give new_value or expression")
    return client.call("update_feature_parameter", **params)


# --------------------------------------------------------------------------- E. v0.3 parameters & analysis

@mcp.tool()
def get_parameters() -> dict:
    """Global parameter table (name, expression, computed value, unit, comment, error) and, for each parameter,
    the feature fields / sketch dimensions bound to it."""
    return client.call("get_parameters")


@mcp.tool()
def set_parameter(name: str, expression: str, comment: str | None = None,
                  unit: Literal["mm", "deg", ""] | None = None) -> dict:
    """Create or modify a named global parameter (e.g. epaisseur_paroi = 2.0, jeu_montage = 0.25,
    entraxe_x = "largeur - 2 * marge"). Expressions: + - * / ^ ( ), sin cos tan (degrees) sqrt abs min max round,
    units mm cm m in deg, references to other parameters. Every bound field is updated and the timeline is
    recomputed in cascade. Cycles and unknown names are rejected (nothing changes)."""
    params: dict[str, Any] = dict(name=name, expression=expression)
    if comment is not None:
        params["comment"] = comment
    if unit is not None:
        params["unit"] = unit
    return client.call("set_parameter", **params)


@mcp.tool()
def delete_parameter(name: str) -> dict:
    """Delete a global parameter (fields still bound to it keep their last value)."""
    return client.call("delete_parameter", name=name)


@mcp.tool()
def measure(entity_ids: list[str]) -> dict:
    """Measure 1 or 2 entities (face / edge ids from query_topology, or body ids): minimal distance
    (BRepExtrema_DistShapeShape) with the closest points and dX dY dZ, angle between two planar faces or straight
    edges, diameter / radius / centre of circular edges and cylindrical faces, edge length or face area."""
    return client.call("measure", entity_ids=entity_ids)


@mcp.tool()
def check_interference(body_ids: list[str] | None = None) -> dict:
    """Check every pair of bodies (all bodies when omitted) for 3D-printing fit: overlap volume
    (BRepAlgoAPI_Common, collision above 1e-4 mm3) and functional clearance (minimal distance).
    status: collision (red) | tight (0 <= clearance < 0.15 mm, risk of fusing after FDM printing) | ok (>= 0.15)."""
    return client.call("check_interference", body_ids=body_ids or [])


@mcp.tool()
def set_body_material(body_id: str, preset: str | None = None, color: str | None = None,
                      opacity: float | None = None, density_g_cm3: float | None = None,
                      name: str | None = None) -> dict:
    """Assign a material to a body: preset among 'PLA Blanc', 'PETG Noir', 'ABS Gris', 'Résine Translucide',
    'PCB Vert', 'Aluminium', 'Acier' (aliases: pla, petg, abs, resine, pcb, alu, steel), and/or a custom colour
    '#RRGGBB' / '#RRGGBBAA', opacity 0.05..1 (translucent enclosure) and density. Returns the estimated mass (g).
    Colours and names are exported to 3MF."""
    params: dict[str, Any] = dict(body_id=body_id)
    for k, v in (("preset", preset), ("color", color), ("opacity", opacity), ("density_g_cm3", density_g_cm3),
                 ("name", name)):
        if v is not None:
            params[k] = v
    return client.call("set_body_material", **params)


@mcp.tool()
def export_3mf(file_path: str, body_ids: list[str] | None = None, linear_deflection: float = 0.05,
               title: str | None = None, designer: str | None = None) -> dict:
    """Export bodies (all by default) to one .3mf archive for Cura / PrusaSlicer / Bambu Studio: separate objects
    with their names and material colours (translucency kept), explicit millimetre unit, watertight meshes,
    project metadata (title, designer, 'Silex v0.3')."""
    params: dict[str, Any] = dict(file_path=file_path, body_ids=body_ids or [], linear_deflection=linear_deflection)
    if title:
        params["title"] = title
    if designer:
        params["designer"] = designer
    return client.call("export_3mf", **params)


@mcp.tool()
def export_drawing(file_path: str, format: Literal["pdf", "svg"] = "pdf",
                   view_layout: Literal["3_views"] = "3_views", sheet: Literal["A4", "A3"] = "A4",
                   scale: str = "auto", title: str | None = None, body_ids: list[str] | None = None) -> dict:
    """Generate a 2D technical drawing: hidden-line projection (HLRBRep) of the bodies in 3 views, European
    first-angle layout (front XZ, top XY below it, right YZ on its left), visible edges solid, hidden edges dashed,
    centre lines, frame and title block (title, scale, unit, date, volume, mass), plus the dimensions placed in
    the Drawing window. scale: 'auto' or '2:1', '1:1', '1:2', '1:5'..."""
    params: dict[str, Any] = dict(file_path=file_path, format=format, view_layout=view_layout, sheet=sheet,
                                  scale=scale, body_ids=body_ids or [])
    if title:
        params["title"] = title
    return client.call("export_drawing", **params)


@mcp.tool()
def undo() -> dict:
    """Undo the last document change."""
    return client.call("undo")


@mcp.tool()
def redo() -> dict:
    """Redo the last undone change."""
    return client.call("redo")


@mcp.tool()
def import_step(file_path: str) -> dict:
    """Import a STEP file as a new body (the B-Rep is embedded in the design)."""
    return client.call("import_step", file_path=file_path)


@mcp.tool()
def export_step(file_path: str, body_id: str = "") -> dict:
    """Export one body (or all bodies when body_id is empty) to STEP AP214."""
    return client.call("export_step", file_path=file_path, body_id=body_id)


@mcp.tool()
def export_stl(file_path: str, body_id: str = "", linear_deflection: float = 0.05) -> dict:
    """Export one body (or all) to binary STL with the given chordal deflection (mm)."""
    return client.call("export_stl", file_path=file_path, body_id=body_id, linear_deflection=linear_deflection)


@mcp.tool()
def save_design(file_path: str) -> dict:
    """Save the current parametric design (timeline, sketches, parameters) as a native .silex file
    that can be reopened and edited in Silex (File > Open)."""
    return client.call("save_design", file_path=file_path)


# --------------------------------------------------------------------------- document cycle

@mcp.tool()
def new_document(discard_changes: bool = False) -> dict:
    """Start a new, empty design. Refused with DOCUMENT_DIRTY when the current design has unsaved
    changes, unless discard_changes=true (ask the user first: the changes may be theirs)."""
    return client.call("new_document", discard_changes=discard_changes)


@mcp.tool()
def open_document(path: str, discard_changes: bool = False) -> dict:
    """Open a native .silex design (absolute path). Same unsaved-changes rule as new_document.
    Returns the same information as get_active_document_info."""
    return client.call("open_document", path=path, discard_changes=discard_changes)


@mcp.tool()
def get_active_document_info() -> dict:
    """Path and name of the open design, schema version, modified flag, number of features (timeline
    steps), sketches, bodies and parameters, rebuild errors / warnings, undo state and open command group."""
    return client.call("get_active_document_info")


@mcp.tool()
def begin_command_group(name: str) -> dict:
    """Group the next operations into ONE undoable step for the user (Ctrl+Z / undo reverts them all),
    e.g. begin_command_group("Mounting bosses") ... end_command_group(). Groups do not nest. A failed
    operation inside the group is still rolled back on its own."""
    return client.call("begin_command_group", name=name)


@mcp.tool()
def end_command_group() -> dict:
    """Close the group opened by begin_command_group; returns how many operations it holds."""
    return client.call("end_command_group")


@mcp.tool()
def verify_dimensions(expected_bbox_mm: list[float], body_id: str = "", tolerance_mm: float = 0.05) -> dict:
    """Check the finished overall size of a body (after fillets / chamfers) against nominal dimensions:
    expected_bbox_mm = [x, y, z] in mm, compared axis by axis with the exact bounding box.
    Returns ok, measured sizes and deviations. body_id may be omitted when there is a single body."""
    return client.call("verify_dimensions", body_id=body_id, expected_bbox_mm=expected_bbox_mm,
                       tolerance_mm=tolerance_mm)


# --------------------------------------------------------------------------- v0.7 "Trio Makers"

@mcp.tool()
def create_emboss_text(face_id: str, text: str, height_mm: float | str = 6.0, depth_mm: float | str = 1.0,
                       font: str = "Liberation Sans", align: Literal["left", "center", "right"] = "center",
                       angle_deg: float | str = 0.0, offset_xy: list[float] | None = None) -> dict:
    """Write text in relief (depth_mm > 0, added material) or engraved (depth_mm < 0) on a planar face
    (query_topology id), an origin plane ("XY", "XZ", "YZ") or a construction plane id.
    The text may contain {expressions} over the global parameters, re-evaluated at every rebuild:
    "Box v{version} - {largeur}x{hauteur} mm"; "{expr:2}" forces 2 decimals. height_mm (capital letter
    height), depth_mm and angle_deg accept a number or an expression. offset_xy = [x, y] from the face
    center (the text block is centered on it). Font: bundled "Liberation Sans" (portable), or a system font
    family / .ttf file. Returns the resolved text, the letters area and the estimated stroke width; a
    Warning status means strokes thinner than the 0.4 mm FDM nozzle."""
    return client.call("create_emboss_text", face_id=face_id, text=text, height_mm=height_mm, depth_mm=depth_mm,
                       font=font, align=align, angle_deg=angle_deg, offset_xy=offset_xy or [0, 0])


@mcp.tool()
def sketch_import_vector(sketch_id: str, file_path: str, scale: float = 1.0, offset_xy: list[float] | None = None,
                         angle_deg: float = 0.0, target_width_mm: float = 0.0, rigid: bool = False,
                         construction: bool = False) -> dict:
    """Import an SVG (path, rect, circle, ellipse, line, polyline, polygon; Béziers approximated within
    0.02 mm) or ASCII DXF (LINE, CIRCLE, ARC, LWPOLYLINE/POLYLINE with bulges, SPLINE, ELLIPSE) into a sketch
    as native lines / arcs / circles. Endpoints closer than 0.01 mm are joined (coincident), so closed
    outlines are immediately extrudable. The drawing's bounding-box center lands on offset_xy; scale or
    target_width_mm sets the size; rigid=true keeps the drawing undeformable (2 DOF: move it by dimensioning
    its anchor point). Returns counts, the number of closed profiles and the sketch DOF."""
    return client.call("sketch_import_vector", sketch_id=sketch_id, file_path=file_path, scale=scale,
                       offset_xy=offset_xy or [0, 0], angle_deg=angle_deg, target_width_mm=target_width_mm,
                       rigid=rigid, construction=construction)


@mcp.tool()
def sketch_set_canvas(sketch_id: str, image_path: str = "", width_mm: float = 0.0,
                      center_xy: list[float] | None = None, opacity: float | None = None,
                      angle_deg: float | None = None,
                      calibrate: dict | None = None, remove: bool = False) -> dict:
    """Attach a reference photo / scan under a sketch (embedded in the design) for reverse engineering,
    or adjust it. calibrate = {"p1_px": [x, y], "p2_px": [x, y], "distance_mm": 50}: two pixels (from the
    top-left corner of the image) whose real distance is known, e.g. both ends of a ruler on the photo;
    the image is then scaled 1:1. remove=true deletes the image."""
    args: dict = {"sketch_id": sketch_id, "remove": remove}
    if opacity is not None:
        args["opacity"] = opacity
    if angle_deg is not None:
        args["angle_deg"] = angle_deg
    if image_path:
        args["image_path"] = image_path
    if width_mm > 0:
        args["width_mm"] = width_mm
    if center_xy:
        args["center_xy"] = center_xy
    if calibrate:
        args["calibrate"] = calibrate
    return client.call("sketch_set_canvas", **args)

# --------------------------------------------------------------------------- --check

def run_check() -> int:
    """`python mcp_server.py --check`: prints [OK] / [WARN] / [FAIL] lines, returns the exit code."""
    failures = 0

    def report(level: str, text: str) -> None:
        nonlocal failures
        failures += level == "FAIL"
        print(f"[{level}] {text}".replace("[OK]", "[OK]  "), flush=True)

    print(f"Silex MCP bridge self-test ({Path(__file__).resolve()})")
    ver = sys.version_info
    report("OK" if ver >= (3, 10) else "FAIL", f"Python {ver.major}.{ver.minor}.{ver.micro} ({sys.executable})")
    if MCP_IMPORT_ERROR is None:
        report("OK", "Python module 'mcp' available")
    else:
        report("FAIL", f"Python module 'mcp' missing ({MCP_IMPORT_ERROR}): pip install -r requirements.txt")
    if IS_WINDOWS:
        try:
            import win32file  # noqa: F401
            report("OK", "Python module 'pywin32' available (named pipes)")
        except ImportError:
            report("FAIL", "Python module 'pywin32' missing: pip install -r requirements.txt")
            print(f"\n{failures} blocking problem(s).")
            return 1

    exe, how = find_silex_exe()
    report("OK" if exe else "FAIL", f"Silex binary: {exe} ({how})" if exe else f"Silex binary {how}")

    # connect to the running instance, or start a private headless one (the user's pipe is left alone)
    probe = SilexClient(PIPE_NAME)
    try:
        _Connection(PIPE_NAME).close()
        target = f"running instance on '{PIPE_NAME}'"
    except Exception:
        if exe is None:
            report("FAIL", f"IPC pipe '{PIPE_NAME}': no instance is listening and Silex cannot be started")
            print(f"\n{failures} blocking problem(s).")
            return 1
        probe = SilexClient(f"silex_check_{os.getpid()}")
        target = f"headless instance started on '{probe.pipe}'"
    try:
        t0 = time.time()
        info = probe.call("ping")
        report("OK", f"IPC connection: {target}, ping {1000 * (time.time() - t0):.0f} ms")
        proto = info.get("protocol_version", 1)
        report("OK" if proto >= REQUIRED_PROTOCOL else "FAIL",
               f"Protocol version {proto} (bridge requires {REQUIRED_PROTOCOL}), Silex {info.get('version')}, "
               f"{len(info.get('methods', []))} methods" + (", tutor mode" if info.get("tutor_mode") else ""))
        missing = [m for m in ("new_document", "open_document", "get_active_document_info", "begin_command_group",
                               "end_command_group", "verify_dimensions") if m not in info.get("methods", [])]
        if missing:
            report("FAIL", "Missing document-cycle methods: " + ", ".join(missing))
    except IpcError as e:
        report("FAIL", f"IPC connection ({target}): {e}")
    finally:
        if probe.started:
            try:
                probe.call("shutdown")
            except Exception:
                pass
        probe.close()

    print("\nAll checks passed." if failures == 0 else f"\n{failures} blocking problem(s).")
    return 0 if failures == 0 else 1

if __name__ == "__main__":
    if "--check" in sys.argv:
        sys.exit(run_check())
    if MCP_IMPORT_ERROR is not None:
        log(f"the Python package 'mcp' is missing ({MCP_IMPORT_ERROR}): pip install -r requirements.txt")
        sys.exit(1)
    if TUTOR_MODE:
        # Socratic tutor: the agent observes (model tree, degrees of freedom, screenshots) and guides with
        # questions; it cannot draw for the student. Silex enforces the same rule on its side.
        tools = getattr(mcp._tool_manager, "_tools", {})
        for name in list(tools):
            if name not in READ_ONLY_TOOLS:
                if hasattr(mcp, "remove_tool"):
                    mcp.remove_tool(name)
                else:
                    tools.pop(name, None)
        log("tutor mode: observation tools only")
    mcp.run(transport="stdio")
