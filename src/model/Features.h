#pragma once
// Typed parameter sets of the built-in parametric features (OCCT-free).
// They are stored as JSON inside FeatureNode::params so that the document format stays
// open, diffable and forward compatible; these structs are the typed view of that JSON.

#include <optional>
#include <string>
#include <vector>

#include "model/Json.h"
#include "model/Sketch.h"
#include "model/TopoRef.h"
#include "model/Uuid.h"

namespace silex {

namespace FeatureType {
inline constexpr const char* Sketch = "Sketch";
inline constexpr const char* Extrude = "Extrude";
inline constexpr const char* Revolve = "Revolve";
inline constexpr const char* Fillet = "Fillet";
inline constexpr const char* Chamfer = "Chamfer";
inline constexpr const char* ImportStep = "ImportStep";
inline constexpr const char* Shell = "Shell";
inline constexpr const char* Combine = "Combine";
inline constexpr const char* Mirror = "Mirror";
inline constexpr const char* LinearPattern = "LinearPattern";
inline constexpr const char* CircularPattern = "CircularPattern";
inline constexpr const char* Sweep = "Sweep";
inline constexpr const char* Loft = "Loft";
// Ergonomics lot: direct modelling without a sketch
inline constexpr const char* OffsetFace = "OffsetFace";  ///< Press / Pull on a planar face
inline constexpr const char* Box = "Box";
inline constexpr const char* Cylinder = "Cylinder";
// v0.4 lot 0: direct management of bodies and faces
inline constexpr const char* RemoveBody = "RemoveBody";
inline constexpr const char* DefeatureFace = "DefeatureFace";
inline constexpr const char* SplitBody = "SplitBody";
inline constexpr const char* TransformBody = "TransformBody";
// v0.4 lot 1: mechanical productivity
inline constexpr const char* ConstructionPlane = "ConstructionPlane";
inline constexpr const char* Rib = "Rib";
// v0.4 lot 2: printable threads
inline constexpr const char* Thread = "Thread";
// v0.5 lot 1: standard holes and inserts
inline constexpr const char* FastenerHole = "FastenerHole";
// v0.7 lot A: parametric text (relief / engraving)
inline constexpr const char* EmbossText = "EmbossText";
}  // namespace FeatureType

enum class BooleanOp { NewBody, Join, Cut, Intersect };
const char* toString(BooleanOp op);
BooleanOp booleanOpFromString(const std::string& s);

/// Where a sketch lives: one of the origin planes, or a (persistently named) planar face.
struct SketchPlaneRef
{
    std::string kind = "XY";  ///< "XY", "XZ", "YZ", "Face" or "Plane" (v0.4 construction plane)
    TopoRef face;             ///< used when kind == "Face"
    Uuid planeId;             ///< used when kind == "Plane": ConstructionPlane feature id
    double offset = 0;        ///< v0.3: plane shifted along its normal (mm), e.g. loft sections
};
Placement originPlacement(const std::string& kind);
/// Placement of an origin-plane reference including its offset (kind must not be "Face").
Placement offsetPlacement(Placement pl, double offset);

struct SketchParams
{
    SketchPlaneRef plane;
    Sketch sketch;
    std::optional<SketchCanvas> canvas;  ///< v0.7 lot C: calibrated reference image (display only)
};

/// Closed regions of a sketch used as profiles. Each region is identified by the
/// sorted ids of the sketch curves bounding it (stable across dimension changes).
struct ProfileRef
{
    Uuid sketch;
    std::vector<std::string> regions;  ///< empty = every region of the sketch
};

struct ExtrudeParams
{
    ProfileRef profile;
    double distance = 10.0;
    bool symmetric = false;
    bool reversed = false;
    BooleanOp op = BooleanOp::NewBody;
    Uuid targetBody;  ///< empty = last body
};

struct RevolveParams
{
    ProfileRef profile;
    std::string axis = "Y";  ///< "X"/"Y" (sketch axes) or the id of a sketch line
    double angle = 360.0;    ///< degrees
    BooleanOp op = BooleanOp::NewBody;
    Uuid targetBody;
};

struct EdgeBlendParams
{
    std::vector<TopoRef> edges;
    double value = 1.0;  ///< fillet radius / chamfer distance
};

/// Hollows a body: the removed faces become openings, walls get `thickness`.
struct ShellParams
{
    Uuid body;                       ///< empty = body owning the faces / last body
    std::vector<TopoRef> removedFaces;
    double thickness = 2.0;
    bool outward = false;            ///< false: walls grow inside the body; true: outside
};

/// An axis, a direction or a plane: world axes "X" "Y" "Z", origin planes "XY" "XZ" "YZ",
/// or a persistent topological entity ("Edge": straight or circular edge, "Face": planar or
/// cylindrical face).
struct GeomRef
{
    std::string kind = "X";
    TopoRef ref;  ///< used when kind is "Edge" or "Face"
};

/// What a mirror / pattern copies: whole bodies, or the tool body of features (a hole, a boss).
struct TransformTargets
{
    std::string type = "body";  ///< "body" | "feature"
    std::vector<Uuid> ids;
};

struct MirrorParams
{
    TransformTargets targets;
    GeomRef plane{"YZ", {}};
    bool merge = true;  ///< "Join if touching"
};

struct PatternDirection
{
    GeomRef ref{"X", {}};
    int count = 1;          ///< occurrences along this direction, original included
    double spacing = 10.0;  ///< mm between occurrences
    bool reverse = false;
    bool symmetric = false; ///< occurrences on both sides of the original
};

struct LinearPatternParams
{
    TransformTargets targets;
    PatternDirection dir1{{"X", {}}, 2, 10.0, false, false};
    PatternDirection dir2{{"Y", {}}, 1, 10.0, false, false};  ///< count 1 = no second direction
};

struct CircularPatternParams
{
    TransformTargets targets;
    GeomRef axis{"Z", {}};
    int count = 4;
    double totalAngle = 360.0;  ///< degrees
    bool equalSpacing = true;   ///< true: spread over totalAngle; false: totalAngle is the step
    bool reverse = false;
};

/// Trajectory of a sweep: curves of a sketch (open or closed chain), or a chain of model edges.
struct PathRef
{
    Uuid sketch;                  ///< sketch holding the path curves
    std::vector<Uuid> curves;     ///< geo ids; empty = every non-construction curve of the sketch
    std::vector<TopoRef> edges;   ///< alternative: persistent model edges (used when sketch is empty)
};

/// Sweep (BRepOffsetAPI_MakePipeShell): closed profile moved along a path.
struct SweepParams
{
    ProfileRef profile;
    PathRef path;
    std::string orientation = "CorrectedFrenet";  ///< "CorrectedFrenet" | "Frenet" | "Fixed"
    BooleanOp op = BooleanOp::NewBody;
    Uuid targetBody;
};

/// Loft (BRepOffsetAPI_ThruSections) through ordered closed profiles on distinct planes.
struct LoftParams
{
    std::vector<ProfileRef> sections;  ///< ordered, at least 2 (one region each)
    bool solid = true;
    bool ruled = false;                ///< straight ruled surfaces between sections
    BooleanOp op = BooleanOp::NewBody;
    Uuid targetBody;
};

/// Boolean between existing bodies: target op= tools (Join / Cut / Intersect).
struct CombineParams
{
    Uuid target;
    std::vector<Uuid> tools;
    BooleanOp op = BooleanOp::Join;
    bool keepTools = false;  ///< false: tool bodies are consumed
};

/// Press / Pull on a planar face of a solid: the face is extruded along its outward normal
/// (BRepPrimAPI_MakePrism), then fused (distance > 0) or cut (distance < 0) with its body.
struct OffsetFaceParams
{
    TopoRef face;
    double distance = 5.0;  ///< mm along the outward normal
    Uuid body;              ///< body owning the face (informative: the face reference decides)
};

/// Quick parametric primitive (Box / Cylinder) placed on an origin plane or a planar face,
/// without a sketch. Box: length along the plane X, width along Y, height along the normal.
struct PrimitiveParams
{
    SketchPlaneRef plane;
    double centerX = 0, centerY = 0;  ///< center of the base, in plane coordinates
    double length = 20, width = 20;   ///< Box
    double diameter = 20;             ///< Cylinder
    double height = 10;               ///< along the plane normal (negative = other side)
    BooleanOp op = BooleanOp::NewBody;
    Uuid targetBody;
};

/// v0.4: bodies made inactive from this step on (the creating features stay untouched).
struct RemoveBodyParams
{
    std::vector<Uuid> bodies;
};

/// v0.4: removes faces (fillets, chamfers, pockets, holes) and heals the solid by extending the
/// neighbouring faces (BRepAlgoAPI_Defeaturing). Works on imported STEP bodies too.
struct DefeatureParams
{
    std::vector<TopoRef> faces;
    Uuid body;  ///< informative: the faces decide
};

/// v0.4: splits a body with a plane (BOPAlgo_Splitter) into "<name>_1" (below the plane) and
/// "<name>_2" (above). Part 1 keeps the body id.
struct SplitBodyParams
{
    Uuid body;
    SketchPlaneRef plane;  ///< origin plane (+ offset) or planar face
    // v0.5 lot 4: alignment pins on part 1 (below the plane) and matching holes in part 2
    bool pegs = false;
    int pegCount = 2;
    double pegDiameter = 4.0;   ///< pin diameter (mm)
    double pegLength = 8.0;     ///< total pin length: half protrudes from part 1, half goes into part 2
    double pegClearance = 0.2;  ///< radial FDM clearance of the holes
};

/// v0.4: moves / rotates (or copies) bodies. Rotations (degrees, X then Y then Z) turn around the
/// center of the bounding box of the bodies, then the translation is applied.
struct TransformBodyParams
{
    std::vector<Uuid> bodies;
    double dx = 0, dy = 0, dz = 0;
    double rx = 0, ry = 0, rz = 0;
    bool copy = false;
    Json mate;  ///< v0.4 lot 1: alignment computed at recompute time (empty = plain transform)
};

/// v0.4 lot 1: reference plane of the timeline ("Construction" folder of the inspector).
///   offset: `base` (origin plane, face or another construction plane) shifted by base.offset;
///   angle:  `base` turned by `angle` degrees around `axis` (world X / Y / Z through the origin,
///           or a straight model edge);
///   mid:    halfway between two parallel planar faces face1 / face2.
struct ConstructionPlaneParams
{
    std::string mode = "offset";  ///< "offset" | "angle" | "mid"
    SketchPlaneRef base;
    GeomRef axis{"X", {}};
    double angle = 45.0;
    TopoRef face1, face2;
};

/// v0.4 lot 1: reinforcing rib from an open sketch line: a wall of `thickness` (centered on the
/// sketch plane) filling the space between the line and the neighbouring walls of the body.
struct RibParams
{
    Uuid sketch;
    Uuid curve;  ///< line of the sketch (empty = first non-construction line)
    double thickness = 2.0;
    Uuid body;   ///< empty = last body
};

/// v0.4 lot 2: ISO metric thread on a cylindrical face (external = screw, internal = nut / tapped
/// hole). The helix phase is tied to the world frame, so coaxial screws and nuts always mesh.
/// `clearance` (FDM play, mm) shrinks a screw / enlarges a nut so printed parts screw together.
struct ThreadParams
{
    TopoRef face;
    std::string size = "M6";   ///< M3 M4 M5 M6 M8 M10 M12
    bool fine = false;          ///< fine pitch
    double pitch = 0;           ///< 0 = from the ISO table
    double clearance = 0.15;    ///< radial clearance (mm)
    double length = 0;          ///< 0 = the whole face, else from the end of the face
    bool modeled = true;        ///< false = cosmetic (no geometry, annotation only)
};

struct MetricThread
{
    double diameter = 0, pitch = 0, finePitch = 0;
};
/// ISO 261 / 262 series used by makers (coarse and usual fine pitch). nullopt for an unknown size.
std::optional<MetricThread> metricThread(const std::string& size);
std::vector<std::string> metricThreadSizes();

/// v0.5 lot 1: standard fastener hole drilled into a planar face (or an origin / construction
/// plane), along the inward normal. kind:
///   "clearance"   through / blind hole, close (+0.2 mm) or normal (+0.4 mm) fit;
///   "counterbore" clearance hole + cylindrical counterbore for socket head cap screws (DIN 912);
///   "countersink" clearance hole + 90 degree cone for countersunk screws (DIN 7991);
///   "insert"      heat-set brass insert pocket (CNC Kitchen / Ruthex type) with a 0.4 mm entry chamfer;
///   "nut"         hexagonal nut trap (ISO 4032 + FDM clearance) followed by the clearance hole.
struct FastenerHoleParams
{
    SketchPlaneRef plane;
    std::vector<Vec2> centers{{0, 0}};  ///< in plane coordinates (several holes = one step)
    std::string kind = "clearance";
    std::string size = "M3";            ///< M2 M2.5 M3 M4 M5 M6
    std::string fit = "normal";         ///< "close" (+0.2) | "normal" (+0.4)
    double depth = 0;                   ///< total depth; 0 = through all
    double nutClearance = 0.15;         ///< hex nut trap: play on the width across flats
    double angle = 0;                   ///< hex nut trap: rotation in the plane (degrees)
    Uuid targetBody;                    ///< empty = body owning the face / last editable body
};

/// ISO metric dimensions used by the hole tool (mm).
struct FastenerSpec
{
    double d = 3;                         ///< nominal diameter
    double headCap = 5.5, headCapH = 3;   ///< DIN 912 head diameter / height
    double headCsk = 6;                   ///< DIN 7991 head diameter (90 degrees)
    double nutS = 5.5, nutM = 2.4;        ///< ISO 4032 width across flats / height
    double insertD = 4.0, insertL = 5.7;  ///< heat-set insert pocket
};
std::optional<FastenerSpec> fastenerSpec(const std::string& size);
std::vector<std::string> fastenerSizes();
std::vector<std::string> fastenerKinds();

/// Resolved geometry of a fastener hole (what is actually cut), shared by the kernel and the UI.
struct FastenerHoleDims
{
    double holeD = 0;      ///< main hole diameter
    double headD = 0;      ///< counterbore / countersink diameter, insert pocket, nut circumcircle
    double headDepth = 0;  ///< counterbore / nut / insert depth (countersink: cone height)
    double chamfer = 0;    ///< insert entry chamfer
    double nutS = 0;       ///< nut trap width across flats (with clearance)
    std::string summary;   ///< e.g. "Ø3.4 through, counterbore Ø6.1 × 3.2"
};
std::optional<FastenerHoleDims> fastenerHoleDims(const FastenerHoleParams& p);

/// v0.7 lot A: text embossed (depth > 0, Join) or engraved (depth < 0, Cut) on a planar face or
/// plane. `text` may hold {expressions} over the global parameters ("W={largeur} mm"); the
/// parametric layer writes the evaluated string into `resolved`, which the kernel reads.
struct EmbossTextParams
{
    SketchPlaneRef plane;
    std::string text = "Silex";
    std::string resolved;          ///< computed from `text` (never edited by hand)
    std::string font = "Liberation Sans";  ///< family name, or a .ttf / .otf file name or path
    double height = 6.0;           ///< capital letter height (mm)
    double depth = 1.0;            ///< > 0 relief, < 0 engraving (mm)
    double angle = 0;              ///< degrees, counter-clockwise in the plane
    std::string align = "center";  ///< "left" | "center" | "right"
    double x = 0, y = 0;           ///< anchor in plane coordinates (face center = 0, 0)
    double lineSpacing = 1.4;      ///< baseline-to-baseline distance, in letter heights
    double nozzle = 0.4;           ///< FDM nozzle: thinner strokes raise a warning
    Uuid targetBody;               ///< empty = body owning the face / last editable body
};

struct ImportParams
{
    std::string sourcePath;
    std::string brep;  ///< embedded B-Rep (OCCT text format) => the project stays self-contained
    // v0.5 lot 1: electronic component of the built-in library (reference body, locked)
    std::string component;  ///< catalog id ("rpi4b", "esp32_devkit"...); empty = plain STEP import
    bool locked = false;    ///< reference body: booleans, fillets... refuse to modify it
    std::string material;   ///< default material preset of the body (e.g. "PCB Vert")
};

void to_json(Json& j, const SketchPlaneRef& v);
void from_json(const Json& j, SketchPlaneRef& v);
void to_json(Json& j, const SketchParams& v);
void from_json(const Json& j, SketchParams& v);
void to_json(Json& j, const ProfileRef& v);
void from_json(const Json& j, ProfileRef& v);
void to_json(Json& j, const ExtrudeParams& v);
void from_json(const Json& j, ExtrudeParams& v);
void to_json(Json& j, const RevolveParams& v);
void from_json(const Json& j, RevolveParams& v);
void to_json(Json& j, const EdgeBlendParams& v);
void from_json(const Json& j, EdgeBlendParams& v);
void to_json(Json& j, const CombineParams& v);
void from_json(const Json& j, CombineParams& v);
void to_json(Json& j, const GeomRef& v);
void from_json(const Json& j, GeomRef& v);
void to_json(Json& j, const TransformTargets& v);
void from_json(const Json& j, TransformTargets& v);
void to_json(Json& j, const MirrorParams& v);
void from_json(const Json& j, MirrorParams& v);
void to_json(Json& j, const PatternDirection& v);
void from_json(const Json& j, PatternDirection& v);
void to_json(Json& j, const LinearPatternParams& v);
void from_json(const Json& j, LinearPatternParams& v);
void to_json(Json& j, const CircularPatternParams& v);
void from_json(const Json& j, CircularPatternParams& v);
void to_json(Json& j, const ShellParams& v);
void from_json(const Json& j, ShellParams& v);
void to_json(Json& j, const PathRef& v);
void from_json(const Json& j, PathRef& v);
void to_json(Json& j, const SweepParams& v);
void from_json(const Json& j, SweepParams& v);
void to_json(Json& j, const LoftParams& v);
void from_json(const Json& j, LoftParams& v);
void to_json(Json& j, const OffsetFaceParams& v);
void from_json(const Json& j, OffsetFaceParams& v);
void to_json(Json& j, const PrimitiveParams& v);
void from_json(const Json& j, PrimitiveParams& v);
void to_json(Json& j, const RemoveBodyParams& v);
void from_json(const Json& j, RemoveBodyParams& v);
void to_json(Json& j, const DefeatureParams& v);
void from_json(const Json& j, DefeatureParams& v);
void to_json(Json& j, const SplitBodyParams& v);
void from_json(const Json& j, SplitBodyParams& v);
void to_json(Json& j, const TransformBodyParams& v);
void from_json(const Json& j, TransformBodyParams& v);
void to_json(Json& j, const ConstructionPlaneParams& v);
void from_json(const Json& j, ConstructionPlaneParams& v);
void to_json(Json& j, const RibParams& v);
void from_json(const Json& j, RibParams& v);
void to_json(Json& j, const ThreadParams& v);
void from_json(const Json& j, ThreadParams& v);
void to_json(Json& j, const FastenerHoleParams& v);
void from_json(const Json& j, FastenerHoleParams& v);
void to_json(Json& j, const EmbossTextParams& v);
void from_json(const Json& j, EmbossTextParams& v);
void to_json(Json& j, const ImportParams& v);
void from_json(const Json& j, ImportParams& v);

}  // namespace silex
