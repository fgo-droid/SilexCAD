#include "model/Features.h"

namespace silex {

const char* toString(BooleanOp op)
{
    switch (op) {
        case BooleanOp::NewBody: return "NewBody";
        case BooleanOp::Join: return "Join";
        case BooleanOp::Cut: return "Cut";
        case BooleanOp::Intersect: return "Intersect";
    }
    return "NewBody";
}

BooleanOp booleanOpFromString(const std::string& s)
{
    if (s == "Join") return BooleanOp::Join;
    if (s == "Cut") return BooleanOp::Cut;
    if (s == "Intersect") return BooleanOp::Intersect;
    return BooleanOp::NewBody;
}

Placement originPlacement(const std::string& kind)
{
    // Z-up world. Each frame satisfies yDir = normal x xDir.
    if (kind == "XZ")
        return {{0, 0, 0}, {1, 0, 0}, {0, -1, 0}};  // y -> +Z
    if (kind == "YZ")
        return {{0, 0, 0}, {0, 1, 0}, {1, 0, 0}};  // y -> +Z
    return {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
}

void to_json(Json& j, const SketchPlaneRef& v)
{
    j = Json{{"kind", v.kind}};
    if (v.kind == "Face")
        j["face"] = v.face;
}
void from_json(const Json& j, SketchPlaneRef& v)
{
    v.kind = j.value("kind", std::string("XY"));
    v.face = j.value("face", TopoRef{});
}

void to_json(Json& j, const SketchParams& v) { j = Json{{"plane", v.plane}, {"sketch", v.sketch}}; }
void from_json(const Json& j, SketchParams& v)
{
    v.plane = j.value("plane", SketchPlaneRef{});
    v.sketch = j.value("sketch", Sketch{});
}

void to_json(Json& j, const ProfileRef& v) { j = Json{{"sketch", v.sketch}, {"regions", v.regions}}; }
void from_json(const Json& j, ProfileRef& v)
{
    v.sketch = j.value("sketch", std::string{});
    v.regions = j.value("regions", std::vector<std::string>{});
}

void to_json(Json& j, const ExtrudeParams& v)
{
    j = Json{{"profile", v.profile},     {"distance", v.distance}, {"symmetric", v.symmetric},
             {"reversed", v.reversed},   {"op", toString(v.op)},   {"targetBody", v.targetBody}};
}
void from_json(const Json& j, ExtrudeParams& v)
{
    v.profile = j.value("profile", ProfileRef{});
    v.distance = j.value("distance", 10.0);
    v.symmetric = j.value("symmetric", false);
    v.reversed = j.value("reversed", false);
    v.op = booleanOpFromString(j.value("op", std::string("NewBody")));
    v.targetBody = j.value("targetBody", std::string{});
}

void to_json(Json& j, const RevolveParams& v)
{
    j = Json{{"profile", v.profile}, {"axis", v.axis},          {"angle", v.angle},
             {"op", toString(v.op)}, {"targetBody", v.targetBody}};
}
void from_json(const Json& j, RevolveParams& v)
{
    v.profile = j.value("profile", ProfileRef{});
    v.axis = j.value("axis", std::string("Y"));
    v.angle = j.value("angle", 360.0);
    v.op = booleanOpFromString(j.value("op", std::string("NewBody")));
    v.targetBody = j.value("targetBody", std::string{});
}

void to_json(Json& j, const EdgeBlendParams& v) { j = Json{{"edges", v.edges}, {"value", v.value}}; }
void from_json(const Json& j, EdgeBlendParams& v)
{
    v.edges = j.value("edges", std::vector<TopoRef>{});
    v.value = j.value("value", 1.0);
}

void to_json(Json& j, const CombineParams& v)
{
    j = Json{{"target", v.target}, {"tools", v.tools}, {"op", toString(v.op)}, {"keepTools", v.keepTools}};
}
void from_json(const Json& j, CombineParams& v)
{
    v.target = j.value("target", std::string{});
    v.tools = j.value("tools", std::vector<std::string>{});
    v.op = booleanOpFromString(j.value("op", std::string("Join")));
    v.keepTools = j.value("keepTools", false);
}

void to_json(Json& j, const ShellParams& v)
{
    j = Json{{"body", v.body}, {"removedFaces", v.removedFaces}, {"thickness", v.thickness}, {"outward", v.outward}};
}
void from_json(const Json& j, ShellParams& v)
{
    v.body = j.value("body", std::string{});
    v.removedFaces = j.value("removedFaces", std::vector<TopoRef>{});
    v.thickness = j.value("thickness", 2.0);
    v.outward = j.value("outward", false);
}

void to_json(Json& j, const GeomRef& v)
{
    j = Json{{"kind", v.kind}};
    if (v.kind == "Edge" || v.kind == "Face")
        j["ref"] = v.ref;
}
void from_json(const Json& j, GeomRef& v)
{
    v.kind = j.value("kind", std::string("X"));
    v.ref = j.value("ref", TopoRef{});
}

void to_json(Json& j, const TransformTargets& v) { j = Json{{"type", v.type}, {"ids", v.ids}}; }
void from_json(const Json& j, TransformTargets& v)
{
    v.type = j.value("type", std::string("body"));
    v.ids = j.value("ids", std::vector<std::string>{});
}

void to_json(Json& j, const MirrorParams& v)
{
    j = Json{{"targets", v.targets}, {"plane", v.plane}, {"merge", v.merge}};
}
void from_json(const Json& j, MirrorParams& v)
{
    v.targets = j.value("targets", TransformTargets{});
    v.plane = j.value("plane", GeomRef{"YZ", {}});
    v.merge = j.value("merge", true);
}

void to_json(Json& j, const PatternDirection& v)
{
    j = Json{{"ref", v.ref},         {"count", v.count},        {"spacing", v.spacing},
             {"reverse", v.reverse}, {"symmetric", v.symmetric}};
}
void from_json(const Json& j, PatternDirection& v)
{
    v.ref = j.value("ref", GeomRef{});
    v.count = j.value("count", 1);
    v.spacing = j.value("spacing", 10.0);
    v.reverse = j.value("reverse", false);
    v.symmetric = j.value("symmetric", false);
}

void to_json(Json& j, const LinearPatternParams& v)
{
    j = Json{{"targets", v.targets}, {"dir1", v.dir1}, {"dir2", v.dir2}};
}
void from_json(const Json& j, LinearPatternParams& v)
{
    v.targets = j.value("targets", TransformTargets{});
    v.dir1 = j.value("dir1", PatternDirection{{"X", {}}, 2, 10.0, false, false});
    v.dir2 = j.value("dir2", PatternDirection{{"Y", {}}, 1, 10.0, false, false});
}

void to_json(Json& j, const CircularPatternParams& v)
{
    j = Json{{"targets", v.targets},           {"axis", v.axis},
             {"count", v.count},               {"totalAngle", v.totalAngle},
             {"equalSpacing", v.equalSpacing}, {"reverse", v.reverse}};
}
void from_json(const Json& j, CircularPatternParams& v)
{
    v.targets = j.value("targets", TransformTargets{});
    v.axis = j.value("axis", GeomRef{"Z", {}});
    v.count = j.value("count", 4);
    v.totalAngle = j.value("totalAngle", 360.0);
    v.equalSpacing = j.value("equalSpacing", true);
    v.reverse = j.value("reverse", false);
}

void to_json(Json& j, const ImportParams& v) { j = Json{{"sourcePath", v.sourcePath}, {"brep", v.brep}}; }
void from_json(const Json& j, ImportParams& v)
{
    v.sourcePath = j.value("sourcePath", std::string{});
    v.brep = j.value("brep", std::string{});
}

}  // namespace silex
