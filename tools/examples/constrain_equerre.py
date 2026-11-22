#!/usr/bin/env python3
"""Fully constrains the sketches of the hand-made example equerre_annotee.silex (0 DOF, white sketches).

Usage:  python tools/examples/constrain_equerre.py

The file is opened headless, close_dof() (build_examples.py) constrains every sketch without moving it,
then only the sketch parameters are copied back into the original JSON: annotations, marker and the
rest of the file stay exactly as they were. Both copies (examples/ and docs/manual/) are updated.
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_examples import OUT, ROOT, Silex, close_dof  # noqa: E402

FILES = [OUT / "equerre_annotee.silex", ROOT / "docs" / "manual" / "equerre_annotee.silex"]


def main() -> int:
    src = FILES[0]
    s = Silex(str(src))
    tmp = OUT / f".equerre_{os.getpid()}.silex"
    try:
        for sk in s("get_pedagogical_context", lang="fr")["sketches"]:
            if sk["dof"]:
                close_dof(s, sk["sketch_id"])
                print(f"{sk['name']}: {sk['dof']} -> 0 DOF")
        s("save_design", file_path=str(tmp))
        solved = {f["id"]: f["params"]["sketch"] for f in json.loads(tmp.read_text(encoding="utf-8"))["features"]
                  if "sketch" in f.get("params", {})}
    finally:
        s.close()
        tmp.unlink(missing_ok=True)
    for path in FILES:
        doc = json.loads(path.read_text(encoding="utf-8"))
        for f in doc["features"]:
            if f["id"] in solved:
                f["params"]["sketch"] = solved[f["id"]]
        path.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print("updated", path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
