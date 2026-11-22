#pragma once
// Sketcher Pro: smart (pre-constrained) primitives and 2D editing tools.
//
// Pure functions working on the OCCT-free sketch model: they add / modify geometry and
// constraints in place. Geometry is always left consistent with its constraints, so the caller
// only has to run SketchSolver (and may drop `optional` constraints if the result is
// over-constrained). Shared by the sketch editor (Qt), the IPC / MCP bridge and the tests.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "model/Sketch.h"

namespace silex::sketchtools {

struct ToolResult
{
    bool ok = false;
    std::string error;
    std::vector<Uuid> geos;                ///< created geometry
    std::vector<Uuid> constraints;         ///< created constraints
    std::map<std::string, Uuid> named;     ///< named pieces (edges of a rectangle, arcs of a slot...)
    Uuid driving;                          ///< main driving dimension (fillet radius, offset distance...)
    int removedConstraints = 0;            ///< constraints that could not survive the edit

    explicit operator bool() const { return ok; }
};

// ------------------------------------------------------------------ smart primitives

/// Rectangle from its center and one corner: 4 closed H/V lines, 2 construction diagonals and a
/// construction center point held at the middle of the diagonals (symmetric when dimensioned).
/// named: bottom, right, top, left, diagonal1, diagonal2, center (Point geo).
ToolResult addCenterRectangle(Sketch& sk, Vec2 center, Vec2 corner, bool construction = false);

/// Slot (obround) from the two arc centers and the radius (half width): 2 lines tangent to 2
/// half-circles of equal radius, plus the construction axis between the centers.
/// overallCenter: also creates a construction center point at the middle of the axis.
/// named: axis, arc1, arc2, side1, side2 (+ center).
ToolResult addSlot(Sketch& sk, Vec2 c1, Vec2 c2, double radius, bool overallCenter = false,
                   bool construction = false);

/// Regular polygon (3..64 sides). inscribed: `vertex` is a vertex on the construction circle;
/// circumscribed: `vertex` is the middle of a side, a construction incircle is tangent to the sides.
/// named: circle (circumcircle, its center is the polygon center), incircle, side0..sideN-1.
ToolResult addPolygon(Sketch& sk, Vec2 center, Vec2 vertex, int sides, bool inscribed = true,
                      bool construction = false);

// ------------------------------------------------------------------ editing tools

/// A piece of curve (for previews).
struct CurvePiece
{
    GeoKind kind = GeoKind::Line;
    Vec2 p1, p2;           ///< line
    Vec2 c;                ///< arc / circle
    double r = 0, a0 = 0, a1 = 0;
};

/// Portion of `geo` that trim() would remove when clicked at `pick`.
std::optional<CurvePiece> trimPreview(const Sketch& sk, const Uuid& geo, Vec2 pick);
/// Cuts `geo` at the intersections surrounding `pick` and removes that portion (the whole curve if
/// it has no intersection). Valid constraints are kept, the new ends are attached to the cutting
/// curves (coincident / point-on-object).
ToolResult trim(Sketch& sk, const Uuid& geo, Vec2 pick);

/// Extends the end of a line / arc closest to `pick` up to the next curve on its way.
ToolResult extend(Sketch& sk, const Uuid& geo, Vec2 pick);
/// Geometry of the curve once extended (for previews).
std::optional<CurvePiece> extendPreview(const Sketch& sk, const Uuid& geo, Vec2 pick);

/// Tangent arc of `radius` between two lines sharing an end point. The lines are trimmed, length
/// dimensions are converted to point-line distances (virtual sharp), driving = Radius dimension.
ToolResult filletLines(Sketch& sk, const Uuid& lineA, const Uuid& lineB, double radius);
/// Same, from a corner point (end of a line shared with another line).
ToolResult filletAtPoint(Sketch& sk, const GeoRef& corner, double radius);

/// Parallel copy of chains of lines / arcs (and circles) at `distance`: > 0 = outwards for closed
/// contours (left of the chain for open ones). Driven by one Distance dimension (driving).
ToolResult offsetCurves(Sketch& sk, const std::vector<Uuid>& geos, double distance);

/// Mirrored copies about a line (or the X / Y axis), tied by Symmetric constraints.
ToolResult mirrorCurves(Sketch& sk, const std::vector<Uuid>& geos, const GeoRef& axis);

/// count - 1 translated copies along `dir`; pitch held by construction lines (equal & parallel),
/// driving = spacing dimension.
ToolResult linearPattern(Sketch& sk, const std::vector<Uuid>& geos, Vec2 dir, int count, double spacing);
/// count - 1 rotated copies around `center` over `totalAngle` (radians; 2 pi = full circle,
/// equally spaced), tied by equal radial construction lines and angle dimensions.
ToolResult circularPattern(Sketch& sk, const std::vector<Uuid>& geos, const GeoRef& center, int count,
                           double totalAngle);

// ------------------------------------------------------------------ constraint helpers

/// Empty when the constraint is applicable to the geometry it references, else the reason.
std::string checkConstraint(const Sketch& sk, const SketchConstraint& c);

/// Symmetric constraint of a / b about `axis` (pairs line ends the way they are drawn).
SketchConstraint makeSymmetric(const Sketch& sk, const GeoRef& a, const GeoRef& b, const GeoRef& axis);

/// Adds each candidate unless it over-constrains the sketch (redundant / conflicting): used for
/// inferred constraints that may already be implied. Returns the ids actually added.
std::vector<Uuid> addIfIndependent(Sketch& sk, const std::vector<SketchConstraint>& candidates);

/// Mirror image of p about the line (a, a + dir).
Vec2 reflect(Vec2 p, Vec2 a, Vec2 dir);

}  // namespace silex::sketchtools
