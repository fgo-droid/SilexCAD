#pragma once
// Real-time constraint inference while drawing (snap to points / curves, horizontal,
// vertical and tangent detection). Pure functions, unit-testable.

#include <optional>
#include <vector>

#include "model/Sketch.h"

namespace silex {

struct Snap
{
    Vec2 pos;
    std::optional<GeoRef> point;  ///< snapped onto an existing point -> Coincident
    std::optional<Uuid> curve;    ///< snapped onto a curve -> PointOnObject
};

/// Snaps `raw` to the closest existing point (priority) or curve within `tol`.
/// Geometry listed in `exclude` is ignored (the entity being drawn).
Snap snapPoint(const Sketch& sk, Vec2 raw, double tol, const std::vector<Uuid>& exclude = {});

enum class LineHint { None, Horizontal, Vertical };
/// Horizontal / vertical inference for a line from a to b (angular tolerance in degrees).
LineHint inferLineDirection(Vec2 a, Vec2 b, double tolDeg = 3.0);
/// Snaps b so that the hint becomes exact.
Vec2 applyHint(Vec2 a, Vec2 b, LineHint hint);

/// If a line starting at `start` with direction `dir` leaves the end-point of an arc
/// tangentially (within tolDeg), returns that arc id.
std::optional<Uuid> inferTangentAtPoint(const Sketch& sk, const GeoRef& start, Vec2 dir, double tolDeg = 4.0);

/// Sketcher Pro: rounds p to the nearest node of a square grid of `step` (no-op if step <= 0).
Vec2 snapToGrid(Vec2 p, double step);

/// Builds the constraints implied by a snap for a point of a newly drawn geometry.
std::vector<SketchConstraint> constraintsFromSnap(const Snap& s, const GeoRef& newPoint);

}  // namespace silex
