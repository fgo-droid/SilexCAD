"""Regenerates THIRD-PARTY-NOTICES.txt at the root of the repository.

The summary below is maintained by hand; the appendix copies, verbatim, the licence files that
vcpkg installs with each dependency (vcpkg_installed/<triplet>/share/<port>/copyright), so the
notices always match the libraries actually shipped.

    python tools/legal/build_notices.py [--vcpkg-installed DIR] [--triplet x64-windows-release]
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# (vcpkg port, display name, licence, how Silex uses it)
COMPONENTS = [
    ("opencascade", "Open CASCADE Technology (OCCT)",
     "LGPL-2.1 with the Open CASCADE Exception 1.0", "dynamically linked (TK*.dll)"),
    ("qtbase", "Qt 6 (Core, Gui, Widgets, Network)", "LGPL-3.0 (also GPL-2.0/GPL-3.0)",
     "dynamically linked (Qt6*.dll, plugins/)"),
    ("qtshadertools", "Qt Shader Tools", "GPL-3.0 with Qt exceptions (build tool only)",
     "build time only: shaders are compiled to .qsb files"),
    (None, "PlaneGCS (FreeCAD 1.0.0, src/Mod/Sketcher/App/planegcs)", "LGPL-2.1-or-later",
     "statically linked; complete sources in third_party/planegcs"),
    ("eigen3", "Eigen", "MPL-2.0", "header-only, used by PlaneGCS"),
    ("boost-graph", "Boost (Graph, Math and their dependencies)", "BSL-1.0", "header-only / static"),
    ("nlohmann-json", "nlohmann/json", "MIT", "header-only"),
    ("icu", "ICU (International Components for Unicode)", "Unicode-3.0", "dynamically linked, used by Qt"),
    ("freetype", "FreeType", "FTL (FreeType License) or GPL-2.0",
     "dynamically linked, used by Qt and by the parametric text (glyph outlines)"),
    ("@font", "Liberation Sans 2.1.5 (Regular)", "SIL Open Font License 1.1",
     "bundled font of the parametric text (resources/fonts), unmodified"),
    ("harfbuzz", "HarfBuzz", "MIT (Old MIT)", "dynamically linked, used by Qt"),
    ("openssl", "OpenSSL", "Apache-2.0", "dynamically linked, used by Qt Network"),
    ("libpng", "libpng", "libpng-2.0", "dynamically linked, used by Qt"),
    ("zlib", "zlib", "Zlib", "dynamically linked (Qt, 3MF export)"),
    ("bzip2", "bzip2", "bzip2-1.0.6", "dynamically linked, used by FreeType"),
    ("brotli", "Brotli", "MIT", "dynamically linked, used by FreeType"),
    ("double-conversion", "double-conversion", "BSD-3-Clause", "dynamically linked, used by Qt"),
    ("md4c", "MD4C", "MIT", "dynamically linked, used by Qt"),
    ("pcre2", "PCRE2", "BSD-3-Clause with PCRE2 exception", "dynamically linked, used by Qt"),
    ("zstd", "Zstandard", "BSD-3-Clause (dual BSD / GPL-2.0)", "dynamically linked, used by Qt"),
    ("gtest", "GoogleTest", "BSD-3-Clause", "test executables only (not needed by silex.exe)"),
]

HEADER = """\
THIRD-PARTY NOTICES — Silex
===========================

Silex itself is released under the MIT License (see LICENSE). It embeds or links the
third-party components listed below, each under its own licence. The full licence texts
follow in the appendix.

Redistribution obligations (summary, not legal advice)
-----------------------------------------------------
* LGPL components (OCCT, Qt, PlaneGCS): this notice and the LGPL text must accompany every
  binary distribution. OCCT and Qt are shipped as separate DLLs that users may replace with
  their own compatible builds. PlaneGCS is linked statically: its complete source code is
  distributed in this repository (third_party/planegcs), so Silex can be rebuilt and relinked
  against a modified PlaneGCS with the provided CMake build (LGPL-2.1 section 6).
* A binary distribution must offer the corresponding source code of the LGPL libraries
  (or a written offer); the upstream sources are those of the versions listed below.
* MIT / BSD / BSL / Apache / Unicode / FTL components: keep their copyright notices and
  licence texts (this file) with the binaries. Apache-2.0 (OpenSSL): see its NOTICE terms.
* MPL-2.0 (Eigen): the source of Eigen files is available from the upstream project; Silex
  does not modify them.

Components
----------
"""


def port_version(installed: Path, port: str) -> str:
    info = installed / "vcpkg" / "info"
    for f in sorted(info.glob(f"{port}_*.list")):
        m = re.match(rf"{re.escape(port)}_(.+?)_[^_]+\.list$", f.name)
        if m:
            return m.group(1)
    return "?"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--vcpkg-installed", default=str(ROOT / "vcpkg_installed"))
    ap.add_argument("--triplet", default="x64-windows-release")
    ap.add_argument("--out", default=str(ROOT / "THIRD-PARTY-NOTICES.txt"))
    a = ap.parse_args()
    installed = Path(a.vcpkg_installed)
    share = installed / a.triplet / "share"

    lines = [HEADER]
    for port, name, lic, use in COMPONENTS:
        version = ("1.0.0 (FreeCAD)" if port is None else "2.1.5" if port == "@font"
                   else port_version(installed, port))
        lines.append(f"* {name}\n    version {version} — licence: {lic}\n    {use}\n")

    lines.append("\n\nAPPENDIX — LICENCE TEXTS\n========================\n")
    planegcs = ROOT / "third_party" / "planegcs" / "README.md"
    lines.append("\n" + "-" * 78 + "\nPlaneGCS (FreeCAD) — LGPL-2.1-or-later\n" + "-" * 78 + "\n")
    lines.append(planegcs.read_text(encoding="utf-8").strip() + "\n")
    lines.append("Copyright (c) 2011 Konstantinos Poulios, (c) 2014 Victor Titov (DeepSOIC) and the FreeCAD contributors.\n"
                 "The text of the GNU Lesser General Public License version 2.1 is reproduced in the\n"
                 "Open CASCADE Technology section below.\n")
    font_license = ROOT / "resources" / "fonts" / "LiberationSans-OFL.txt"
    lines.append("\n" + "-" * 78 + "\nLiberation Sans (font) — SIL Open Font License 1.1\n" + "-" * 78 + "\n")
    lines.append(font_license.read_text(encoding="utf-8").strip() + "\n")
    for port, name, _lic, _use in COMPONENTS:
        if port is None or port == "@font":
            continue
        f = share / port / "copyright"
        lines.append("\n" + "-" * 78 + f"\n{name} ({port})\n" + "-" * 78 + "\n")
        lines.append(f.read_text(encoding="utf-8", errors="replace").strip() + "\n" if f.is_file()
                     else f"(licence file not found in {f.parent})\n")

    Path(a.out).write_text("".join(lines), encoding="utf-8", newline="\n")
    print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
