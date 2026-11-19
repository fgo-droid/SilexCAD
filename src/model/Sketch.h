#pragma once
// 2D sketch data model (OCCT-free). Geometry is stored *solved*: the solver
// (sketch/SketchSolver) updates coordinates in place, the kernel only reads them.

#include <optional>
#include <string>
#include <vector>

#include "model/Json.h"
#include "model/Math.h"
#include "model/Uuid.h"

namespace silex {

enum class GeoKind { Line, Circle, Arc, Point };
enum class PointPos { None = 0, Start = 1, End = 2, Center = 3 };

/// Reserved geometry ids for the implicit, fixed sketch origin and axes.
inline const Uuid kOriginId = "@O";
inline const Uuid kXAxisId = "@X";
inline const Uuid kYAxisId = "@Y";
inline bool isReservedGeo(const Uuid& id) { return !id.empty() && id[0] == '@'; }

struct SketchGeo
{
    Uuid id;
    GeoKind kind = GeoKind::Line;
    bool construction = false;
    Vec2 p1, p2;          ///< Line: endpoints. Point: p1.
    Vec2 c;               ///< Circle/Arc: center.
    double r = 0;         ///< Circle/Arc: radius.
    double a0 = 0, a1 = 0;///< Arc: start/end angles (radians, CCW from a0 to a1).

    Vec2 point(PointPos pos) const;
    bool hasPoint(PointPos pos) const;
    bool isCurve() const { return kind != GeoKind::Point; }
};

enum class ConstraintKind {
    Coincident,
    Horizontal,
    Vertical,
    Parallel,
    Perpendicular,
    Tangent,
    Equal,
    Fixed,
    PointOnObject,
    Distance,
    DistanceX,
    DistanceY,
    Radius,
    Diameter,
    Angle,
};

const char* toString(ConstraintKind k);
std::optional<ConstraintKind> constraintKindFromString(const std::string& s);
bool isDimensional(ConstraintKind k);

struct GeoRef
{
    Uuid geo;
    PointPos pos = PointPos::None;
    bool valid() const { return !geo.empty(); }
    bool operator==(const GeoRef&) const = default;
};

struct SketchConstraint
{
    Uuid id;
    ConstraintKind kind = ConstraintKind::Coincident;
    GeoRef a, b;
    double value = 0;   ///< dimension value (mm / radians); Fixed: x
    double value2 = 0;  ///< Fixed: y
    Vec2 labelOffset{0, 0};
};

struct Sketch
{
    std::vector<SketchGeo> geos;
    std::vector<SketchConstraint> constraints;

    const SketchGeo* find(const Uuid& id) const;
    SketchGeo* find(const Uuid& id);
    /// Resolves reserved ids (origin/axes) too.
    std::optional<SketchGeo> geoOrReserved(const Uuid& id) const;
    std::optional<Vec2> pointOf(const GeoRef& ref) const;

    /// Removes geometry and every constraint referencing it.
    void removeGeo(const Uuid& id);
    void removeConstraint(const Uuid& id);
    /// Current measured value of a dimensional constraint (used when creating it).
    double measure(const SketchConstraint& c) const;
};

void to_json(Json& j, const SketchGeo& g);
void from_json(const Json& j, SketchGeo& g);
void to_json(Json& j, const SketchConstraint& c);
void from_json(const Json& j, SketchConstraint& c);
void to_json(Json& j, const Sketch& s);
void from_json(const Json& j, Sketch& s);

}  // namespace silex
