#pragma once
// Typed parameter sets of the built-in parametric features (OCCT-free).
// They are stored as JSON inside FeatureNode::params so that the document format stays
// open, diffable and forward compatible; these structs are the typed view of that JSON.

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
}  // namespace FeatureType

enum class BooleanOp { NewBody, Join, Cut, Intersect };
const char* toString(BooleanOp op);
BooleanOp booleanOpFromString(const std::string& s);

/// Where a sketch lives: one of the origin planes, or a (persistently named) planar face.
struct SketchPlaneRef
{
    std::string kind = "XY";  ///< "XY", "XZ", "YZ" or "Face"
    TopoRef face;             ///< used when kind == "Face"
};
Placement originPlacement(const std::string& kind);

struct SketchParams
{
    SketchPlaneRef plane;
    Sketch sketch;
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

/// Boolean between existing bodies: target op= tools (Join / Cut / Intersect).
struct CombineParams
{
    Uuid target;
    std::vector<Uuid> tools;
    BooleanOp op = BooleanOp::Join;
    bool keepTools = false;  ///< false: tool bodies are consumed
};

struct ImportParams
{
    std::string sourcePath;
    std::string brep;  ///< embedded B-Rep (OCCT text format) => the project stays self-contained
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
void to_json(Json& j, const ImportParams& v);
void from_json(const Json& j, ImportParams& v);

}  // namespace silex
