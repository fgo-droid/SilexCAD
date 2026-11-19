#pragma once
// Thin, exception-safe wrapper around FreeCAD's PlaneGCS solver.

#include <optional>
#include <set>
#include <string>

#include "model/Sketch.h"

namespace silex {

enum class SketchDofState {
    UnderConstrained,  ///< blue
    FullyConstrained,  ///< white (dark theme) / black
    OverConstrained,   ///< red: conflicting or redundant constraints
    Failed,            ///< red: solver could not converge
};

struct SolveResult
{
    bool converged = false;
    int dof = -1;
    SketchDofState state = SketchDofState::UnderConstrained;
    std::set<Uuid> conflicting;           ///< constraint ids
    std::set<Uuid> redundant;             ///< constraint ids
    std::set<Uuid> fullyConstrainedGeos;  ///< geometry with no remaining freedom
    std::string message;
};

/// Optional "drag" target: the solver keeps every constraint and moves `point` as close
/// as possible to `to` (used for direct manipulation in the sketch editor).
struct DragTarget
{
    GeoRef point;  ///< pos None on a line/circle = drag the whole curve
    Vec2 to;
    Vec2 grabOffset;  ///< for whole-curve drags: cursor - reference point at grab time
};

class SketchSolver
{
public:
    /// Solves in place. On failure the sketch is left untouched.
    static SolveResult solve(Sketch& sketch, const std::optional<DragTarget>& drag = std::nullopt,
                             bool diagnose = true);
};

}  // namespace silex
