#pragma once
// v0.4 lot 1 — Align / Mate: transform that positions a moving body against a fixed one.
//
// mate JSON (stored in TransformBodyParams::mate, recomputed at every rebuild):
//   {"mode": "face" | "axis", "moving": TopoRef, "fixed": TopoRef, "offset": mm, "flip": bool}
//   face: planar faces put face to face (normals opposed; flip = same direction), the moving face
//         centroid lands on the fixed face centroid shifted by `offset` along the fixed normal
//         (e.g. jeu_montage = 0.25 mm);
//   axis: cylindrical faces or circular edges made coaxial, centers shifted by `offset` along the axis.

#include <gp_Trsf.hxx>

#include "kernel/Feature.h"

namespace silex::kernel {

gp_Trsf mateTransform(ExecContext& ctx, const Json& mate, const Uuid& movingBody);

}  // namespace silex::kernel
