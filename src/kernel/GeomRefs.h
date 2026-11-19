#pragma once
// Resolution of axes / directions / planes (world references or persistent TNP entities) and
// name-preserving transformed copies, shared by Mirror and the pattern features.

#include <functional>
#include <string>

#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Trsf.hxx>

#include "kernel/Feature.h"

namespace silex::kernel {

/// "X" "Y" "Z" world axes; straight edge (its line); circular edge (its axis);
/// planar face (normal through the centroid); cylindrical / conical face (its axis).
gp_Ax1 resolveAxis(ExecContext& ctx, const GeomRef& ref);
/// "X" "Y" "Z"; straight edge direction; planar face normal.
gp_Dir resolveDirection(ExecContext& ctx, const GeomRef& ref);
/// "XY" "XZ" "YZ" origin planes or a planar face. The Ax2 main direction is the plane normal.
gp_Ax2 resolvePlane(ExecContext& ctx, const GeomRef& ref);

/// Copy of `src` moved by `trsf` (BRepBuilderAPI_Transform, copy mode: valid orientation even
/// for mirrors). Every face name n becomes rename(n); edges are renamed from their faces.
NamedShape transformedCopy(const NamedShape& src, const gp_Trsf& trsf,
                           const std::function<std::string(const std::string&)>& rename);

/// One copy of the targets (bodies or feature tools) per transform, combined into the model.
/// tags[i] prefixes the TNP names of copy i, e.g. "pattern_circ(<feat>,3,".
void replicateTargets(ExecContext& ctx, const TransformTargets& targets, const std::vector<gp_Trsf>& trsfs,
                      const std::vector<std::string>& tags);

}  // namespace silex::kernel
