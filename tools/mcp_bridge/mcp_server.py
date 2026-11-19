"""Silex MCP bridge.

Exposes the Silex CAD application to MCP clients (Claude Code, Gemini CLI, ...) over stdio.
It forwards every tool call to the running application through its local IPC endpoint
(Windows named pipe \\\\.\\pipe\\cad_mcp, Unix socket /tmp/cad_mcp.sock) using
newline-delimited JSON-RPC. If no instance is listening, it starts `silex --headless`.

stdout belongs to the MCP protocol: nothing else is ever printed there. The CAD process is
started with its stdout/stderr redirected away, so OpenCascade / Qt logs cannot leak in.

Environment variables:
    SILEX_EXE   path to silex(.exe)            (default: <repo>/build/bin/Release/silex.exe)
    SILEX_PIPE  IPC endpoint name              (default: cad_mcp)
"""

from __future__ import annotations

import base64
import itertools
import json
import os
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any, Literal

from mcp.server.fastmcp import FastMCP, Image

PIPE_NAME = os.environ.get("SILEX_PIPE", "cad_mcp")
IS_WINDOWS = sys.platform == "win32"
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_EXE = REPO_ROOT / "build" / "bin" / "Release" / ("silex.exe" if IS_WINDOWS else "silex")


def log(msg: str) -> None:
    """Diagnostics go to stderr only (stdout is the MCP channel)."""
    print(f"[silex-mcp] {msg}", file=sys.stderr, flush=True)


class IpcError(RuntimeError):
    pass


# --------------------------------------------------------------------------- transport

class _Connection:
    """One duplex line-oriented connection to the application."""

    def __init__(self) -> None:
        self._buf = b""
        if IS_WINDOWS:
            import pywintypes  # noqa: F401  (pywin32)
            import win32file

            self._win32file = win32file
            self._h = win32file.CreateFile(
                rf"\\.\pipe\{PIPE_NAME}",
                win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                0, None, win32file.OPEN_EXISTING, 0, None,
            )
        else:
            self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self._sock.connect(f"/tmp/{PIPE_NAME}.sock")

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
    def __init__(self) -> None:
        self._conn: _Connection | None = None
        self._ids = itertools.count(1)
        self._lock = threading.Lock()
        self._proc: subprocess.Popen | None = None

    def _connect(self) -> _Connection:
        if self._conn is not None:
            return self._conn
        try:
            self._conn = _Connection()
            return self._conn
        except Exception:
            pass
        self._start_application()
        deadline = time.time() + 60
        last: Exception | None = None
        while time.time() < deadline:
            try:
                self._conn = _Connection()
                return self._conn
            except Exception as e:  # not listening yet
                last = e
                if self._proc is not None and self._proc.poll() is not None:
                    raise IpcError(f"silex exited with code {self._proc.returncode}") from e
                time.sleep(0.25)
        raise IpcError(f"Could not connect to Silex on '{PIPE_NAME}': {last}")

    def _start_application(self) -> None:
        exe = Path(os.environ.get("SILEX_EXE", DEFAULT_EXE))
        if not exe.exists():
            raise IpcError(f"No Silex instance is listening and {exe} does not exist (set SILEX_EXE)")
        log(f"starting {exe} --headless --pipe {PIPE_NAME}")
        flags = subprocess.CREATE_NO_WINDOW if IS_WINDOWS else 0
        self._proc = subprocess.Popen(
            [str(exe), "--headless", "--pipe", PIPE_NAME],
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
                raise IpcError(reply["error"].get("message", "unknown error"))
            return reply.get("result")


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
def create_sketch(plane_or_face_id: str, name: str = "") -> dict:
    """Create an empty sketch on 'XY', 'XZ', 'YZ' or on a planar face id (from query_topology).
    Returns sketch_id and the plane frame (sketch x/y map to plane x_dir and normal x x_dir)."""
    return client.call("create_sketch", plane_or_face_id=plane_or_face_id, name=name)


@mcp.tool()
def sketch_add_geometry(sketch_id: str, elements: list[dict]) -> dict:
    """Add 2D geometry (sketch coordinates, mm). Elements:
      {"type":"line","start":[x,y],"end":[x,y]}
      {"type":"rectangle","corner":[x,y],"width":w,"height":h}  or  {"type":"rectangle","centered":true,"center":[x,y],"width":w,"height":h}
      {"type":"circle","center":[x,y],"radius":r}
      {"type":"arc","center":[x,y],"radius":r,"start_angle":deg,"end_angle":deg}   (counter-clockwise)
    Any element accepts "construction": true. Returns geo_ids and point_ids ("<geo>:start|end|center")."""
    return client.call("sketch_add_geometry", sketch_id=sketch_id, elements=elements)


@mcp.tool()
def sketch_add_constraints(sketch_id: str, constraints: list[dict]) -> dict:
    """Add constraints and solve with PlaneGCS. Each constraint: {"type": ..., "a": ref, "b": ref, "value": v}
    types: coincident, horizontal, vertical, parallel, perpendicular, tangent, equal, distance,
    distance_x, distance_y, radius, diameter, angle (degrees), fixed, point_on_object.
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
def combine(target_body_id: str, tool_body_ids: list[str],
            operation: Literal["join", "cut", "intersect"] = "join", keep_tools: bool = False) -> dict:
    """Boolean between existing bodies: target ∪ tools (join), target − tools (cut) or target ∩ tools
    (intersect). Tool bodies are consumed unless keep_tools is true. Returns the remaining body ids."""
    return client.call("combine", target_body_id=target_body_id, tool_body_ids=tool_body_ids,
                       operation=operation, keep_tools=keep_tools)

# --------------------------------------------------------------------------- D. editing, history, I/O

@mcp.tool()
def update_feature_parameter(feature_id: str, param_name: str, new_value: float) -> dict:
    """Change a numeric parameter and rebuild incrementally. Extrude: distance; Revolve: angle; Shell: thickness;
    CircularPattern: count, total_angle_deg; LinearPattern: count_1, spacing_1_mm, count_2, spacing_2_mm;
    Fillet/Chamfer: radius|distance; Shell: thickness; Sketch: param_name = id of a dimensional
    constraint (angles in degrees). A change that breaks the rebuild is rejected and undone."""
    return client.call("update_feature_parameter", feature_id=feature_id, param_name=param_name,
                       new_value=new_value)


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


if __name__ == "__main__":
    mcp.run(transport="stdio")
