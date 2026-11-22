#include "model/Features.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

Placement offsetPlacement(Placement pl, double offset)
{
    pl.origin = pl.origin + pl.normal * offset;
    return pl;
}

void to_json(Json& j, const SketchPlaneRef& v)
{
    j = Json{{"kind", v.kind}};
    if (v.kind == "Face")
        j["face"] = v.face;
    if (v.kind == "Plane")
        j["planeId"] = v.planeId;
    if (v.offset != 0)
        j["offset"] = v.offset;
}
void from_json(const Json& j, SketchPlaneRef& v)
{
    v.kind = j.value("kind", std::string("XY"));
    v.face = j.value("face", TopoRef{});
    v.planeId = j.value("planeId", std::string{});
    v.offset = j.value("offset", 0.0);
}

void to_json(Json& j, const SketchParams& v)
{
    j = Json{{"plane", v.plane}, {"sketch", v.sketch}};
    if (v.canvas)
        j["canvas"] = *v.canvas;
}
void from_json(const Json& j, SketchParams& v)
{
    v.plane = j.value("plane", SketchPlaneRef{});
    v.sketch = j.value("sketch", Sketch{});
    v.canvas.reset();
    if (j.contains("canvas") && j["canvas"].is_object())
        v.canvas = j["canvas"].get<SketchCanvas>();
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

void to_json(Json& j, const OffsetFaceParams& v)
{
    j = Json{{"face", v.face}, {"distance", v.distance}};
    if (!v.body.empty())
        j["body"] = v.body;
}
void from_json(const Json& j, OffsetFaceParams& v)
{
    v.face = j.value("face", TopoRef{});
    v.distance = j.value("distance", 5.0);
    v.body = j.value("body", std::string{});
}

void to_json(Json& j, const PrimitiveParams& v)
{
    j = Json{{"plane", v.plane},   {"centerX", v.centerX}, {"centerY", v.centerY}, {"length", v.length},
             {"width", v.width},   {"diameter", v.diameter}, {"height", v.height},  {"op", toString(v.op)}};
    if (!v.targetBody.empty())
        j["targetBody"] = v.targetBody;
}
void from_json(const Json& j, PrimitiveParams& v)
{
    v.plane = j.value("plane", SketchPlaneRef{});
    v.centerX = j.value("centerX", 0.0);
    v.centerY = j.value("centerY", 0.0);
    v.length = j.value("length", 20.0);
    v.width = j.value("width", 20.0);
    v.diameter = j.value("diameter", 20.0);
    v.height = j.value("height", 10.0);
    v.op = booleanOpFromString(j.value("op", std::string("NewBody")));
    v.targetBody = j.value("targetBody", std::string{});
}

void to_json(Json& j, const RemoveBodyParams& v) { j = Json{{"bodies", v.bodies}}; }
void from_json(const Json& j, RemoveBodyParams& v) { v.bodies = j.value("bodies", std::vector<std::string>{}); }

void to_json(Json& j, const DefeatureParams& v)
{
    j = Json{{"faces", v.faces}};
    if (!v.body.empty())
        j["body"] = v.body;
}
void from_json(const Json& j, DefeatureParams& v)
{
    v.faces = j.value("faces", std::vector<TopoRef>{});
    v.body = j.value("body", std::string{});
}

void to_json(Json& j, const SplitBodyParams& v)
{
    j = Json{{"body", v.body}, {"plane", v.plane}};
    if (v.pegs)
        j["pegs"] = Json{{"count", v.pegCount}, {"diameter", v.pegDiameter}, {"length", v.pegLength}, {"clearance", v.pegClearance}};
}
void from_json(const Json& j, SplitBodyParams& v)
{
    v.body = j.value("body", std::string{});
    v.plane = j.value("plane", SketchPlaneRef{});
    v.pegs = j.contains("pegs") && j["pegs"].is_object();
    if (v.pegs) {
        const Json& g = j["pegs"];
        v.pegCount = g.value("count", 2);
        v.pegDiameter = g.value("diameter", 4.0);
        v.pegLength = g.value("length", 8.0);
        v.pegClearance = g.value("clearance", 0.2);
    }
}

void to_json(Json& j, const TransformBodyParams& v)
{
    j = Json{{"bodies", v.bodies}, {"dx", v.dx}, {"dy", v.dy}, {"dz", v.dz},
             {"rx", v.rx},         {"ry", v.ry}, {"rz", v.rz}, {"copy", v.copy}};
    if (!v.mate.is_null() && !v.mate.empty())
        j["mate"] = v.mate;
}
void from_json(const Json& j, TransformBodyParams& v)
{
    v.bodies = j.value("bodies", std::vector<std::string>{});
    v.dx = j.value("dx", 0.0);
    v.dy = j.value("dy", 0.0);
    v.dz = j.value("dz", 0.0);
    v.rx = j.value("rx", 0.0);
    v.ry = j.value("ry", 0.0);
    v.rz = j.value("rz", 0.0);
    v.copy = j.value("copy", false);
    v.mate = j.value("mate", Json());
}

void to_json(Json& j, const ConstructionPlaneParams& v)
{
    j = Json{{"mode", v.mode}, {"base", v.base}};
    if (v.mode == "angle") {
        j["axis"] = v.axis;
        j["angle"] = v.angle;
    }
    if (v.mode == "mid") {
        j["face1"] = v.face1;
        j["face2"] = v.face2;
    }
}
void from_json(const Json& j, ConstructionPlaneParams& v)
{
    v.mode = j.value("mode", std::string("offset"));
    v.base = j.value("base", SketchPlaneRef{});
    v.axis = j.value("axis", GeomRef{"X", {}});
    v.angle = j.value("angle", 45.0);
    v.face1 = j.value("face1", TopoRef{});
    v.face2 = j.value("face2", TopoRef{});
}

void to_json(Json& j, const RibParams& v)
{
    j = Json{{"sketch", v.sketch}, {"thickness", v.thickness}};
    if (!v.curve.empty())
        j["curve"] = v.curve;
    if (!v.body.empty())
        j["body"] = v.body;
}
void from_json(const Json& j, RibParams& v)
{
    v.sketch = j.value("sketch", std::string{});
    v.curve = j.value("curve", std::string{});
    v.thickness = j.value("thickness", 2.0);
    v.body = j.value("body", std::string{});
}

std::optional<MetricThread> metricThread(const std::string& size)
{
    static const std::pair<const char*, MetricThread> table[] = {
        {"M3", {3, 0.5, 0.35}},   {"M4", {4, 0.7, 0.5}},    {"M5", {5, 0.8, 0.5}},  {"M6", {6, 1.0, 0.75}},
        {"M8", {8, 1.25, 1.0}},   {"M10", {10, 1.5, 1.25}}, {"M12", {12, 1.75, 1.5}},
    };
    for (const auto& [n, t] : table)
        if (size == n)
            return t;
    return std::nullopt;
}

std::vector<std::string> metricThreadSizes() { return {"M3", "M4", "M5", "M6", "M8", "M10", "M12"}; }

void to_json(Json& j, const ThreadParams& v)
{
    j = Json{{"face", v.face},           {"size", v.size},       {"fine", v.fine},     {"pitch", v.pitch},
             {"clearance", v.clearance}, {"length", v.length},   {"modeled", v.modeled}};
}
void from_json(const Json& j, ThreadParams& v)
{
    v.face = j.value("face", TopoRef{});
    v.size = j.value("size", std::string("M6"));
    v.fine = j.value("fine", false);
    v.pitch = j.value("pitch", 0.0);
    v.clearance = j.value("clearance", 0.15);
    v.length = j.value("length", 0.0);
    v.modeled = j.value("modeled", true);
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

void to_json(Json& j, const PathRef& v)
{
    j = Json{{"sketch", v.sketch}, {"curves", v.curves}};
    if (!v.edges.empty())
        j["edges"] = v.edges;
}
void from_json(const Json& j, PathRef& v)
{
    v.sketch = j.value("sketch", std::string{});
    v.curves = j.value("curves", std::vector<std::string>{});
    v.edges = j.value("edges", std::vector<TopoRef>{});
}

void to_json(Json& j, const SweepParams& v)
{
    j = Json{{"profile", v.profile}, {"path", v.path},          {"orientation", v.orientation},
             {"op", toString(v.op)}, {"targetBody", v.targetBody}};
}
void from_json(const Json& j, SweepParams& v)
{
    v.profile = j.value("profile", ProfileRef{});
    v.path = j.value("path", PathRef{});
    v.orientation = j.value("orientation", std::string("CorrectedFrenet"));
    v.op = booleanOpFromString(j.value("op", std::string("NewBody")));
    v.targetBody = j.value("targetBody", std::string{});
}

void to_json(Json& j, const LoftParams& v)
{
    j = Json{{"sections", v.sections}, {"solid", v.solid},          {"ruled", v.ruled},
             {"op", toString(v.op)},   {"targetBody", v.targetBody}};
}
void from_json(const Json& j, LoftParams& v)
{
    v.sections = j.value("sections", std::vector<ProfileRef>{});
    v.solid = j.value("solid", true);
    v.ruled = j.value("ruled", false);
    v.op = booleanOpFromString(j.value("op", std::string("NewBody")));
    v.targetBody = j.value("targetBody", std::string{});
}

void to_json(Json& j, const ImportParams& v)
{
    j = Json{{"sourcePath", v.sourcePath}, {"brep", v.brep}};
    if (!v.component.empty())
        j["component"] = v.component;
    if (v.locked)
        j["locked"] = true;
    if (!v.material.empty())
        j["material"] = v.material;
}
void from_json(const Json& j, ImportParams& v)
{
    v.sourcePath = j.value("sourcePath", std::string{});
    v.brep = j.value("brep", std::string{});
    v.component = j.value("component", std::string{});
    v.locked = j.value("locked", false);
    v.material = j.value("material", std::string{});
}

// ------------------------------------------------------------------ v0.5 lot 1: fastener holes

std::optional<FastenerSpec> fastenerSpec(const std::string& size)
{
    // d, DIN 912 dk / k, DIN 7991 dk, ISO 4032 s / m, heat-set insert pocket (CNC Kitchen / Ruthex)
    static const std::pair<const char*, FastenerSpec> table[] = {
        {"M2", {2.0, 3.8, 2.0, 4.0, 4.0, 1.6, 3.2, 4.0}},
        {"M2.5", {2.5, 4.5, 2.5, 5.0, 5.0, 2.0, 3.6, 5.7}},
        {"M3", {3.0, 5.5, 3.0, 6.0, 5.5, 2.4, 4.0, 5.7}},
        {"M4", {4.0, 7.0, 4.0, 8.0, 7.0, 3.2, 5.6, 8.1}},
        {"M5", {5.0, 8.5, 5.0, 10.0, 8.0, 4.7, 6.4, 9.5}},
        {"M6", {6.0, 10.0, 6.0, 12.0, 10.0, 5.2, 8.0, 12.7}},
    };
    for (const auto& [n, s] : table)
        if (size == n)
            return s;
    return std::nullopt;
}

std::vector<std::string> fastenerSizes() { return {"M2", "M2.5", "M3", "M4", "M5", "M6"}; }
std::vector<std::string> fastenerKinds() { return {"clearance", "counterbore", "countersink", "insert", "nut"}; }

std::optional<FastenerHoleDims> fastenerHoleDims(const FastenerHoleParams& p)
{
    const auto s = fastenerSpec(p.size);
    if (!s)
        return std::nullopt;
    auto mm = [](double v) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", std::round(v * 100) / 100);
        return std::string(buf);
    };
    const std::string dia = "\xC3\x98", times = " \xC3\x97 ";
    FastenerHoleDims d;
    d.holeD = s->d + (p.fit == "close" ? 0.2 : 0.4);
    const std::string depth = p.depth > 0 ? "depth " + mm(p.depth) : "through";
    if (p.kind == "clearance") {
        d.summary = dia + mm(d.holeD) + " " + depth;
    }
    else if (p.kind == "counterbore") {
        d.headD = s->headCap + 0.6;
        d.headDepth = s->headCapH + 0.2;
        d.summary = dia + mm(d.holeD) + " " + depth + ", counterbore " + dia + mm(d.headD) + times + mm(d.headDepth);
    }
    else if (p.kind == "countersink") {
        d.headD = s->headCsk + 0.4;
        d.headDepth = (d.headD - d.holeD) / 2;  // 90 degrees: cone height = radial step
        d.summary = dia + mm(d.holeD) + " " + depth + ", countersink 90\xC2\xB0 " + dia + mm(d.headD);
    }
    else if (p.kind == "insert") {
        d.holeD = s->insertD;
        d.headD = s->insertD;
        d.headDepth = p.depth > 0 ? p.depth : s->insertL;
        d.chamfer = 0.4;
        d.summary = "insert " + dia + mm(d.holeD) + times + mm(d.headDepth) + ", chamfer " + mm(d.chamfer);
    }
    else if (p.kind == "nut") {
        d.nutS = s->nutS + 2 * std::max(0.0, p.nutClearance);
        d.headD = d.nutS * 2 / std::sqrt(3.0);
        d.headDepth = s->nutM + 0.2;
        d.summary = dia + mm(d.holeD) + " " + depth + ", hex nut trap " + mm(d.nutS) + times + mm(d.headDepth);
    }
    else
        return std::nullopt;
    return d;
}

void to_json(Json& j, const FastenerHoleParams& v)
{
    j = Json{{"plane", v.plane}, {"centers", v.centers}, {"kind", v.kind}, {"size", v.size},
             {"fit", v.fit}, {"depth", v.depth}, {"nutClearance", v.nutClearance}, {"angle", v.angle}};
    if (!v.targetBody.empty())
        j["targetBody"] = v.targetBody;
}
void from_json(const Json& j, FastenerHoleParams& v)
{
    v.plane = j.value("plane", SketchPlaneRef{});
    v.centers = j.value("centers", std::vector<Vec2>{{0, 0}});
    v.kind = j.value("kind", std::string("clearance"));
    v.size = j.value("size", std::string("M3"));
    v.fit = j.value("fit", std::string("normal"));
    v.depth = j.value("depth", 0.0);
    v.nutClearance = j.value("nutClearance", 0.15);
    v.angle = j.value("angle", 0.0);
    v.targetBody = j.value("targetBody", std::string{});
}

void to_json(Json& j, const EmbossTextParams& v)
{
    j = Json{{"plane", v.plane},   {"text", v.text},   {"resolved", v.resolved}, {"font", v.font},
             {"height", v.height}, {"depth", v.depth}, {"angle", v.angle},       {"align", v.align},
             {"x", v.x},           {"y", v.y},         {"lineSpacing", v.lineSpacing}, {"nozzle", v.nozzle}};
    if (!v.targetBody.empty())
        j["targetBody"] = v.targetBody;
}
void from_json(const Json& j, EmbossTextParams& v)
{
    v.plane = j.value("plane", SketchPlaneRef{});
    v.text = j.value("text", std::string("Silex"));
    v.resolved = j.value("resolved", std::string{});
    v.font = j.value("font", std::string("Liberation Sans"));
    v.height = j.value("height", 6.0);
    v.depth = j.value("depth", 1.0);
    v.angle = j.value("angle", 0.0);
    v.align = j.value("align", std::string("center"));
    v.x = j.value("x", 0.0);
    v.y = j.value("y", 0.0);
    v.lineSpacing = j.value("lineSpacing", 1.4);
    v.nozzle = j.value("nozzle", 0.4);
    v.targetBody = j.value("targetBody", std::string{});
}
}  // namespace silex
