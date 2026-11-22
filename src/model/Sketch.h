#pragma once
// 2D sketch data model (OCCT-free). Geometry is stored *solved*: the solver
// (sketch/SketchSolver) updates coordinates in place, the kernel only reads them.

#include <optional>
#include <string>
#include <vector>

#include "model/Json.h"
#include "model/Math.h"
#include "model/TopoRef.h"
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
    /// v0.3: projected model edge ("external" geometry). Always construction, fixed for the solver,
    /// re-projected by the kernel at every recompute so that it follows the model.
    TopoRef ext;

    bool external() const { return !ext.name.empty(); }

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
    // Sketcher Pro
    Midpoint,    ///< a = point, b = line (middle) or arc (curvilinear middle)
    Symmetric,   ///< a, b symmetric about c (line / axis, or a point); value = 1: line ends paired crosswise
    Concentric,  ///< a, b circles / arcs sharing their center
    Collinear,   ///< a, b lines on the same infinite line
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
    GeoRef c;           ///< Sketcher Pro: third reference (symmetry axis)
    double value = 0;   ///< dimension value (mm / radians); Fixed: x
    double value2 = 0;  ///< Fixed: y
    Vec2 labelOffset{0, 0};
    std::string expr;   ///< v0.3: expression driving `value` (global parameters), empty = literal
    std::string group;  ///< v0.7: internal constraint of a rigid block ("block:<anchor id>"), not drawn

    bool references(const Uuid& geo) const { return a.geo == geo || b.geo == geo || c.geo == geo; }
    bool referencesPoint(const GeoRef& p) const { return a == p || b == p || c == p; }
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

// ------------------------------------------------------------------ v0.7 lot C: calibrated image

/// Reference photo / drawing shown under a sketch (reverse engineering, repairs). Pixel coordinates
/// have their origin at the top-left corner of the image, Y down; sketch coordinates are mm, Y up.
struct SketchCanvas
{
    std::string imagePath;   ///< original file (informative, may be relative)
    std::string imageData;   ///< base64 of the image file (embedded: the .silex stays portable)
    std::string mime = "image/png";
    int pxWidth = 0, pxHeight = 0;  ///< image size in pixels
    double opacity = 0.5;
    double widthMm = 100, heightMm = 100;
    Vec2 center{0, 0};       ///< image center in sketch coordinates
    double angleDeg = 0;     ///< counter-clockwise
    bool flipH = false, flipV = false;
    bool visible = true;
};

/// A new canvas `widthMm` wide (height from the pixel aspect ratio), centered on `center`.
SketchCanvas makeCanvas(int pxWidth, int pxHeight, double widthMm, Vec2 center = {0, 0});
/// Pixel (x right, y down) -> sketch point (mm). Throws std::invalid_argument without pixel size.
Vec2 canvasPixelToSketch(const SketchCanvas& c, Vec2 px);
/// Sketch point -> pixel.
Vec2 canvasSketchToPixel(const SketchCanvas& c, Vec2 p);
/// Two-click calibration: the pixels px1 / px2 are `distanceMm` apart in reality. Scales the image
/// uniformly (square pixels) to 1:1; the point px1 keeps its place in the sketch. Returns false
/// (with `error`) when the points coincide or the distance is not positive.
bool calibrateCanvas(SketchCanvas& c, Vec2 px1, Vec2 px2, double distanceMm, std::string* error = nullptr);

void to_json(Json& j, const SketchCanvas& c);
void from_json(const Json& j, SketchCanvas& c);

void to_json(Json& j, const SketchGeo& g);
void from_json(const Json& j, SketchGeo& g);
void to_json(Json& j, const SketchConstraint& c);
void from_json(const Json& j, SketchConstraint& c);
void to_json(Json& j, const Sketch& s);
void from_json(const Json& j, Sketch& s);

}  // namespace silex
