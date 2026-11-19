#include "IpcCommandDispatcher.hpp"

#include <QApplication>
#include <QBuffer>
#include <QCoreApplication>
#include <QFileInfo>
#include <QImage>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>

#include "ModelController.h"
#include "Viewport.h"
#include "sketch/SketchSolver.h"

namespace silex::ipc {

namespace {

// ------------------------------------------------------------------ small JSON helpers

double reqNum(const Json& p, const char* key)
{
    if (!p.contains(key) || !p[key].is_number())
        throw IpcError(std::string("Missing numeric parameter '") + key + "'", -32602);
    return p[key].get<double>();
}

std::string reqStr(const Json& p, const char* key)
{
    if (!p.contains(key) || !p[key].is_string())
        throw IpcError(std::string("Missing string parameter '") + key + "'", -32602);
    return p[key].get<std::string>();
}

Vec2 reqVec2(const Json& p, const char* key, std::optional<Vec2> def = std::nullopt)
{
    if (!p.contains(key)) {
        if (def)
            return *def;
        throw IpcError(std::string("Missing point parameter '") + key + "' ([x, y])", -32602);
    }
    const Json& v = p[key];
    if (!v.is_array() || v.size() != 2)
        throw IpcError(std::string("Parameter '") + key + "' must be [x, y]", -32602);
    return {v[0].get<double>(), v[1].get<double>()};
}

Json vec(const Vec3& v) { return Json::array({v.x, v.y, v.z}); }

const char* statusName(FeatureStatus s)
{
    switch (s) {
        case FeatureStatus::Ok: return "OK";
        case FeatureStatus::Warning: return "Warning";
        case FeatureStatus::Error: return "Error";
        case FeatureStatus::Suppressed: return "Suppressed";
        case FeatureStatus::RolledBack: return "RolledBack";
    }
    return "?";
}

const char* surfaceTypeName(int t)
{
    static const char* n[] = {"plane",   "cylinder",           "cone",                "sphere",
                              "torus",   "bezier_surface",     "bspline_surface",     "surface_of_revolution",
                              "surface_of_extrusion", "offset_surface", "other_surface"};
    return t >= 0 && t <= 10 ? n[t] : "unknown";
}

const char* curveTypeName(int t)
{
    static const char* n[] = {"line",         "circle",        "ellipse",       "hyperbola", "parabola",
                              "bezier_curve", "bspline_curve", "offset_curve", "other_curve"};
    return t >= 0 && t <= 8 ? n[t] : "unknown";
}

const char* dofStateName(SketchDofState s)
{
    switch (s) {
        case SketchDofState::UnderConstrained: return "under_constrained";
        case SketchDofState::FullyConstrained: return "fully_constrained";
        case SketchDofState::OverConstrained: return "over_constrained";
        case SketchDofState::Failed: return "solver_failed";
    }
    return "?";
}

BooleanOp parseOp(const std::string& s)
{
    if (s == "new_body" || s == "NewBody") return BooleanOp::NewBody;
    if (s == "join" || s == "Join") return BooleanOp::Join;
    if (s == "cut" || s == "Cut") return BooleanOp::Cut;
    if (s == "intersect" || s == "Intersect") return BooleanOp::Intersect;
    throw IpcError("operation must be new_body, join, cut or intersect", -32602);
}

/// "origin" | "x_axis" | "y_axis" | "<geo_id>" | "<geo_id>:start|end|center"
GeoRef parseRef(const std::string& s)
{
    if (s == "origin") return {kOriginId, PointPos::Start};
    if (s == "x_axis") return {kXAxisId, PointPos::None};
    if (s == "y_axis") return {kYAxisId, PointPos::None};
    const auto c = s.rfind(':');
    if (c == std::string::npos)
        return {s, PointPos::None};
    const std::string suffix = s.substr(c + 1);
    PointPos pos = PointPos::None;
    if (suffix == "start") pos = PointPos::Start;
    else if (suffix == "end") pos = PointPos::End;
    else if (suffix == "center") pos = PointPos::Center;
    else throw IpcError("Bad point id '" + s + "' (expected <geo_id>:start|end|center)", -32602);
    return {s.substr(0, c), pos};
}

Json pointIds(const SketchGeo& g)
{
    switch (g.kind) {
        case GeoKind::Line: return Json::array({g.id + ":start", g.id + ":end"});
        case GeoKind::Circle: return Json::array({g.id + ":center"});
        case GeoKind::Arc: return Json::array({g.id + ":start", g.id + ":end", g.id + ":center"});
        case GeoKind::Point: return Json::array({g.id + ":start"});
    }
    return Json::array();
}

SketchConstraint mkC(ConstraintKind k, GeoRef a, GeoRef b = {}, double v = 0)
{
    SketchConstraint c;
    c.id = newUuid();
    c.kind = k;
    c.a = std::move(a);
    c.b = std::move(b);
    c.value = v;
    return c;
}

const MeshEdge* findEdge(const ModelSnapshot& s, const std::string& name, Uuid* owner = nullptr)
{
    for (const auto& b : s.bodies)
        for (const auto& e : b.mesh.edges)
            if (e.name == name) {
                if (owner)
                    *owner = b.id;
                return &e;
            }
    return nullptr;
}

const MeshFace* findFace(const ModelSnapshot& s, const std::string& name, Uuid* owner = nullptr)
{
    for (const auto& b : s.bodies)
        for (const auto& f : b.mesh.faces)
            if (f.name == name) {
                if (owner)
                    *owner = b.id;
                return &f;
            }
    return nullptr;
}

}  // namespace

// ------------------------------------------------------------------ plumbing

IpcCommandDispatcher::IpcCommandDispatcher(ui::ModelController* model, QObject* parent)
    : QObject(parent), m_model(model)
{
}

IpcCommandDispatcher::~IpcCommandDispatcher() = default;

std::vector<std::string> IpcCommandDispatcher::methods()
{
    return {"ping",          "get_model_tree",        "inspect_body",           "list_solids",           "query_topology",
            "capture_viewport", "create_sketch",       "sketch_add_geometry",    "sketch_add_constraints",
            "extrude",       "revolve",               "fillet_or_chamfer",      "shell",       "combine",  "mirror",  "linear_pattern",  "circular_pattern",
            "update_feature_parameter", "undo",        "redo",                   "import_step",
            "export_step",   "export_stl",            "save_design",            "shutdown"};
}

std::string IpcCommandDispatcher::handleLine(const std::string& line)
{
    Json req;
    try {
        req = Json::parse(line);
    }
    catch (const std::exception&) {
        return Json{{"id", nullptr}, {"error", {{"code", -32700}, {"message", "Parse error"}}}}.dump();
    }
    const Json id = req.contains("id") ? req["id"] : Json();
    try {
        if (!req.contains("method") || !req["method"].is_string())
            throw IpcError("Invalid request: 'method' is required", -32600);
        const Json params = req.contains("params") && req["params"].is_object() ? req["params"] : Json::object();
        Json result = dispatch(req["method"].get<std::string>(), params);
        return Json{{"id", id}, {"result", std::move(result)}}.dump();
    }
    catch (const IpcError& e) {
        return Json{{"id", id}, {"error", {{"code", e.code()}, {"message", e.what()}}}}.dump();
    }
    catch (const std::exception& e) {
        return Json{{"id", id}, {"error", {{"code", -32000}, {"message", e.what()}}}}.dump();
    }
}

Json IpcCommandDispatcher::dispatch(const std::string& method, const Json& p)
{
    using Fn = Json (IpcCommandDispatcher::*)(const Json&);
    static const std::map<std::string, Fn> table{
        {"ping", &IpcCommandDispatcher::ping},
        {"get_model_tree", &IpcCommandDispatcher::getModelTree},
        {"inspect_body", &IpcCommandDispatcher::inspectBody},
        {"list_solids", &IpcCommandDispatcher::listSolids},
        {"query_topology", &IpcCommandDispatcher::queryTopology},
        {"capture_viewport", &IpcCommandDispatcher::captureViewport},
        {"create_sketch", &IpcCommandDispatcher::createSketch},
        {"sketch_add_geometry", &IpcCommandDispatcher::sketchAddGeometry},
        {"sketch_add_constraints", &IpcCommandDispatcher::sketchAddConstraints},
        {"extrude", &IpcCommandDispatcher::extrude},
        {"revolve", &IpcCommandDispatcher::revolve},
        {"fillet_or_chamfer", &IpcCommandDispatcher::filletOrChamfer},
        {"shell", &IpcCommandDispatcher::shell},
        {"combine", &IpcCommandDispatcher::combine},
        {"mirror", &IpcCommandDispatcher::mirror},
        {"linear_pattern", &IpcCommandDispatcher::linearPattern},
        {"circular_pattern", &IpcCommandDispatcher::circularPattern},
        {"update_feature_parameter", &IpcCommandDispatcher::updateFeatureParameter},
        {"undo", &IpcCommandDispatcher::undo},
        {"redo", &IpcCommandDispatcher::redo},
        {"import_step", &IpcCommandDispatcher::importStep},
        {"export_step", &IpcCommandDispatcher::exportStep},
        {"export_stl", &IpcCommandDispatcher::exportStl},
        {"save_design", &IpcCommandDispatcher::saveDesign},
        {"shutdown", &IpcCommandDispatcher::shutdown},
    };
    auto it = table.find(method);
    if (it == table.end())
        throw IpcError("Method not found: " + method, -32601);
    return (this->*(it->second))(p);
}

const DocState& IpcCommandDispatcher::doc() const { return m_model->doc(); }

FeaturePtr IpcCommandDispatcher::requireFeature(const std::string& id, const char* type) const
{
    FeaturePtr f = doc().find(id);
    if (!f)
        throw IpcError("Unknown feature id: " + id, -32602);
    if (type && f->type != type)
        throw IpcError("Feature " + id + " is a " + f->type + ", expected " + type, -32602);
    return f;
}

Analysis IpcCommandDispatcher::commit(const Command& cmd, const Uuid& fid)
{
    m_model->execute(cmd);
    Analysis a = KernelEngine::analyze(doc());
    if (const FeatureResult* r = a.result(fid); r && r->status == FeatureStatus::Error) {
        const std::string msg = r->message;
        m_model->undo();
        throw IpcError("Rebuild failed: " + msg + " (the change was undone)");
    }
    return a;
}

Json IpcCommandDispatcher::bodySummary(const Analysis& a, const Uuid& bodyId) const
{
    for (const auto& b : a.bodies)
        if (b.id == bodyId)
            return Json{{"body_id", b.id}, {"volume_mm3", b.volume}, {"valid", b.valid}};
    return Json();
}

// ------------------------------------------------------------------ A. inspection

Json IpcCommandDispatcher::ping(const Json&)
{
    return Json{{"name", "Silex"},
                {"version", QCoreApplication::applicationVersion().toStdString()},
                {"pid", QCoreApplication::applicationPid()},
                {"methods", methods()}};
}

Json IpcCommandDispatcher::getModelTree(const Json&)
{
    const Analysis a = KernelEngine::analyze(doc());
    Json features = Json::array();
    const DocState& d = doc();
    for (size_t i = 0; i < d.features.size(); ++i) {
        const FeatureNode& f = *d.features[i];
        Json params = f.params;
        if (f.type == FeatureType::ImportStep && params.contains("brep"))
            params["brep"] = "<embedded B-Rep, " + std::to_string(params["brep"].get<std::string>().size()) + " bytes>";
        Json jf{{"id", f.id},           {"type", f.type},     {"name", f.name},
                {"index", int(i)},      {"suppressed", f.suppressed}, {"active", int(i) < d.marker},
                {"params", params}};
        if (const FeatureResult* r = a.result(f.id)) {
            jf["status"] = statusName(r->status);
            if (!r->message.empty())
                jf["message"] = r->message;
        }
        features.push_back(std::move(jf));
    }
    Json bodies = Json::array();
    for (const auto& b : a.bodies)
        bodies.push_back({{"id", b.id}, {"name", b.name}});
    Json planes = Json::array();
    for (const char* k : {"XY", "XZ", "YZ"}) {
        const Placement pl = originPlacement(k);
        planes.push_back({{"id", k}, {"normal", vec(pl.normal)}, {"x_dir", vec(pl.xDir)}});
    }
    return Json{{"features", features}, {"marker", d.marker}, {"bodies", bodies}, {"reference_planes", planes}};
}

Json IpcCommandDispatcher::inspectBody(const Json& p)
{
    const Analysis a = KernelEngine::analyze(doc());
    std::string id = p.value("body_id", std::string{});
    if (id.empty()) {
        if (a.bodies.size() != 1)
            throw IpcError("body_id is required when the model has " + std::to_string(a.bodies.size()) + " bodies",
                           -32602);
        id = a.bodies.front().id;
    }
    for (const auto& b : a.bodies)
        if (b.id == id)
            return Json{{"body_id", b.id},
                        {"name", b.name},
                        {"volume_mm3", b.volume},
                        {"area_mm2", b.area},
                        {"center_of_mass", vec(b.centerOfMass)},
                        {"bbox", {{"min", vec(b.bbox.min)}, {"max", vec(b.bbox.max)}}},
                        {"valid", b.valid},
                        {"solids", b.solids},
                        {"faces", b.faces},
                        {"edges", b.edges}};
    throw IpcError("Unknown body id: " + id, -32602);
}

Json IpcCommandDispatcher::listSolids(const Json& p)
{
    const Analysis a = KernelEngine::analyze(doc());
    std::string id = p.value("body_id", std::string{});
    if (id.empty() && a.bodies.size() == 1)
        id = a.bodies.front().id;
    for (const auto& b : a.bodies) {
        if (b.id != id)
            continue;
        Json solids = Json::array();
        for (size_t i = 0; i < b.solidList.size(); ++i) {
            const SolidMetrics& s = b.solidList[i];
            solids.push_back({{"index", i},
                              {"volume_mm3", s.volume},
                              {"center", vec(s.center)},
                              {"bbox", {{"min", vec(s.bbox.min)}, {"max", vec(s.bbox.max)}}},
                              {"size", vec(s.bbox.max - s.bbox.min)}});
        }
        return Json{{"body_id", b.id}, {"count", solids.size()}, {"solids", solids}};
    }
    throw IpcError("Unknown body id: " + id, -32602);
}

Json IpcCommandDispatcher::queryTopology(const Json& p)
{
    auto snap = KernelEngine::computeSnapshot(doc());
    std::string id = p.value("body_id", std::string{});
    const BodyView* body = nullptr;
    for (const auto& b : snap->bodies)
        if (b.id == id || (id.empty() && snap->bodies.size() == 1))
            body = &b;
    if (!body)
        throw IpcError(id.empty() ? "body_id is required" : "Unknown body id: " + id, -32602);

    const std::string type = p.value("entity_type", std::string("face"));
    const std::string filter = p.value("filter", std::string("all"));
    static const std::vector<std::string> filters{"all", "top", "bottom", "vertical", "planar", "circular"};
    if (std::find(filters.begin(), filters.end(), filter) == filters.end())
        throw IpcError("filter must be all, top, bottom, vertical, planar or circular", -32602);
    const BBox& bb = body->mesh.bbox;
    const double tol = 1e-4 * std::max(1.0, bb.diagonal());

    Json out = Json::array();
    if (type == "face") {
        for (const MeshFace& f : body->mesh.faces) {
            const bool planar = f.sig.type == 0;
            bool keep = filter == "all";
            if (filter == "planar") keep = planar;
            if (filter == "circular") keep = f.sig.type == 1 || f.sig.type == 2;  // cylinder, cone
            if (filter == "top") keep = planar && f.sig.dir.z > 0.999 && std::abs(f.sig.center.z - bb.max.z) < tol;
            if (filter == "bottom") keep = planar && f.sig.dir.z < -0.999 && std::abs(f.sig.center.z - bb.min.z) < tol;
            if (filter == "vertical") keep = std::abs(f.sig.dir.z) < 1e-6;
            if (!keep)
                continue;
            out.push_back({{"id", f.name},
                           {"geom_type", surfaceTypeName(f.sig.type)},
                           {"normal", vec(f.sig.dir)},
                           {"area_mm2", f.sig.size},
                           {"center", vec(f.sig.center)}});
        }
    }
    else if (type == "edge") {
        for (const MeshEdge& e : body->mesh.edges) {
            const bool line = e.sig.type == 0;
            bool keep = filter == "all";
            if (filter == "planar") keep = line;  // straight edges
            if (filter == "circular") keep = e.sig.type == 1;
            if (filter == "top") keep = std::abs(e.sig.center.z - bb.max.z) < tol;
            if (filter == "bottom") keep = std::abs(e.sig.center.z - bb.min.z) < tol;
            if (filter == "vertical") keep = line && std::abs(e.sig.dir.z) > 0.999;
            if (!keep)
                continue;
            out.push_back({{"id", e.name},
                           {"geom_type", curveTypeName(e.sig.type)},
                           {"direction", vec(e.sig.dir)},
                           {"length_mm", e.sig.size},
                           {"center", vec(e.sig.center)}});
        }
    }
    else {
        throw IpcError("entity_type must be 'face' or 'edge'", -32602);
    }
    return Json{{"body_id", body->id}, {"entity_type", type}, {"filter", filter}, {"count", out.size()}, {"entities", out}};
}

Json IpcCommandDispatcher::captureViewport(const Json& p)
{
    const std::string view = p.value("view", std::string("iso"));
    const int w = std::clamp(p.value("width", 1024), 64, 4096);
    const int h = std::clamp(p.value("height", 768), 64, 4096);
    int which = 0;
    if (view == "front") which = 1;
    else if (view == "right") which = 3;
    else if (view == "top") which = 7;
    else if (view != "iso") throw IpcError("view must be iso, front, top or right", -32602);

    if (!m_capture) {
        m_capture = std::make_unique<ui::Viewport>();
        m_capture->setAttribute(Qt::WA_DontShowOnScreen);  // offscreen: rendered, never displayed
        m_capture->setAttribute(Qt::WA_QuitOnClose, false); // must not keep the app alive
    }
    m_capture->resize(w, h);
    if (!m_capture->isVisible())
        m_capture->show();
    m_capture->setSnapshot(KernelEngine::computeSnapshot(doc()));
    m_capture->camera().rotation = ui::Camera::standardView(which);
    m_capture->fitAll(false);
    QImage img = m_capture->grabFramebuffer();
    if (img.isNull())
        throw IpcError("Offscreen rendering is not available on this system");
    if (img.width() != w || img.height() != h)
        img = img.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return Json{{"format", "png"}, {"width", img.width()}, {"height", img.height()},
                {"data", png.toBase64().toStdString()}};
}

// ------------------------------------------------------------------ B. sketches

Json IpcCommandDispatcher::createSketch(const Json& p)
{
    const std::string target = reqStr(p, "plane_or_face_id");
    SketchParams sp;
    Placement pl;
    if (target == "XY" || target == "XZ" || target == "YZ") {
        sp.plane.kind = target;
        pl = originPlacement(target);
    }
    else {
        auto snap = KernelEngine::computeSnapshot(doc());
        const MeshFace* f = findFace(*snap, target);
        if (!f)
            throw IpcError("Unknown face id (use query_topology): " + target, -32602);
        if (!f->planar)
            throw IpcError("The face is not planar", -32602);
        sp.plane.kind = "Face";
        sp.plane.face = TopoRef{f->name, f->sig};
        pl = f->plane;
    }
    std::string name = p.value("name", std::string{});
    if (name.empty())
        name = doc().nextName("Sketch");
    FeaturePtr f = makeFeature(FeatureType::Sketch, name, Json(sp));
    commit(AddFeatureCommand(f), f->id);
    return Json{{"sketch_id", f->id},
                {"name", name},
                {"plane", {{"origin", vec(pl.origin)}, {"x_dir", vec(pl.xDir)}, {"normal", vec(pl.normal)}}}};
}

Json IpcCommandDispatcher::sketchAddGeometry(const Json& p)
{
    FeaturePtr f = requireFeature(reqStr(p, "sketch_id"), FeatureType::Sketch);
    if (!p.contains("elements") || !p["elements"].is_array())
        throw IpcError("'elements' must be a list", -32602);
    SketchParams sp = f->as<SketchParams>();
    Sketch& sk = sp.sketch;
    Json results = Json::array();

    auto newGeo = [&](GeoKind k, bool construction) {
        SketchGeo g;
        g.id = newUuid();
        g.kind = k;
        g.construction = construction;
        return g;
    };

    for (const Json& el : p["elements"]) {
        const std::string type = reqStr(el, "type");
        const bool cons = el.value("construction", false);
        Json r{{"type", type}, {"geo_ids", Json::array()}, {"point_ids", Json::array()}};
        auto add = [&](const SketchGeo& g) {
            sk.geos.push_back(g);
            r["geo_ids"].push_back(g.id);
            for (const auto& pid : pointIds(g))
                r["point_ids"].push_back(pid);
        };
        if (type == "line") {
            SketchGeo g = newGeo(GeoKind::Line, cons);
            g.p1 = reqVec2(el, "start");
            g.p2 = reqVec2(el, "end");
            if ((g.p2 - g.p1).length() < 1e-9)
                throw IpcError("Zero-length line", -32602);
            add(g);
        }
        else if (type == "rectangle") {
            const double w = reqNum(el, "width"), h = reqNum(el, "height");
            if (w <= 0 || h <= 0)
                throw IpcError("Rectangle width and height must be positive", -32602);
            Vec2 c0 = el.value("centered", false) ? reqVec2(el, "center", Vec2{0, 0}) - Vec2{w / 2, h / 2}
                                                  : reqVec2(el, "corner", Vec2{0, 0});
            const Vec2 q[4] = {c0, c0 + Vec2{w, 0}, c0 + Vec2{w, h}, c0 + Vec2{0, h}};
            Uuid ids[4];
            for (int i = 0; i < 4; ++i) {
                SketchGeo g = newGeo(GeoKind::Line, cons);
                g.p1 = q[i];
                g.p2 = q[(i + 1) % 4];
                ids[i] = g.id;
                sk.geos.push_back(g);
                r["geo_ids"].push_back(g.id);
                r["point_ids"].push_back(g.id + ":start");  // corners: bl, br, tr, tl
            }
            for (int i = 0; i < 4; ++i)
                sk.constraints.push_back(mkC(ConstraintKind::Coincident, {ids[i], PointPos::End}, {ids[(i + 1) % 4], PointPos::Start}));
            sk.constraints.push_back(mkC(ConstraintKind::Horizontal, {ids[0]}));
            sk.constraints.push_back(mkC(ConstraintKind::Horizontal, {ids[2]}));
            sk.constraints.push_back(mkC(ConstraintKind::Vertical, {ids[1]}));
            sk.constraints.push_back(mkC(ConstraintKind::Vertical, {ids[3]}));
            r["edges"] = {{"bottom", ids[0]}, {"right", ids[1]}, {"top", ids[2]}, {"left", ids[3]}};
        }
        else if (type == "circle") {
            SketchGeo g = newGeo(GeoKind::Circle, cons);
            g.c = reqVec2(el, "center");
            g.r = reqNum(el, "radius");
            if (g.r <= 0)
                throw IpcError("Circle radius must be positive", -32602);
            add(g);
        }
        else if (type == "arc") {
            SketchGeo g = newGeo(GeoKind::Arc, cons);
            g.c = reqVec2(el, "center");
            g.r = reqNum(el, "radius");
            g.a0 = reqNum(el, "start_angle") * kPi / 180.0;
            g.a1 = reqNum(el, "end_angle") * kPi / 180.0;
            while (g.a1 <= g.a0)
                g.a1 += 2 * kPi;
            if (g.r <= 0)
                throw IpcError("Arc radius must be positive", -32602);
            add(g);
        }
        else {
            throw IpcError("Unknown geometry type '" + type + "' (line, rectangle, circle, arc)", -32602);
        }
        results.push_back(std::move(r));
    }

    const SolveResult res = SketchSolver::solve(sk);
    auto n = std::make_shared<FeatureNode>(*f);
    n->params = Json(sp);
    commit(UpdateFeatureCommand(n), n->id);
    return Json{{"sketch_id", f->id}, {"elements", results}, {"dof", res.dof}, {"state", dofStateName(res.state)}};
}

Json IpcCommandDispatcher::sketchAddConstraints(const Json& p)
{
    FeaturePtr f = requireFeature(reqStr(p, "sketch_id"), FeatureType::Sketch);
    if (!p.contains("constraints") || !p["constraints"].is_array())
        throw IpcError("'constraints' must be a list", -32602);
    SketchParams sp = f->as<SketchParams>();
    Sketch& sk = sp.sketch;

    static const std::map<std::string, ConstraintKind> kinds{
        {"coincident", ConstraintKind::Coincident},       {"horizontal", ConstraintKind::Horizontal},
        {"vertical", ConstraintKind::Vertical},           {"parallel", ConstraintKind::Parallel},
        {"perpendicular", ConstraintKind::Perpendicular}, {"tangent", ConstraintKind::Tangent},
        {"equal", ConstraintKind::Equal},                 {"distance", ConstraintKind::Distance},
        {"distance_x", ConstraintKind::DistanceX},        {"distance_y", ConstraintKind::DistanceY},
        {"radius", ConstraintKind::Radius},               {"diameter", ConstraintKind::Diameter},
        {"angle", ConstraintKind::Angle},                 {"fixed", ConstraintKind::Fixed},
        {"point_on_object", ConstraintKind::PointOnObject},
    };

    Json ids = Json::array();
    for (const Json& cj : p["constraints"]) {
        const std::string type = reqStr(cj, "type");
        auto k = kinds.find(type);
        if (k == kinds.end())
            throw IpcError("Unknown constraint type '" + type + "'", -32602);
        SketchConstraint c = mkC(k->second, parseRef(reqStr(cj, "a")));
        if (cj.contains("b"))
            c.b = parseRef(reqStr(cj, "b"));
        if (!sk.geoOrReserved(c.a.geo) || (c.b.valid() && !sk.geoOrReserved(c.b.geo)))
            throw IpcError("Constraint '" + type + "' references unknown geometry", -32602);
        if (isDimensional(c.kind)) {
            c.value = reqNum(cj, "value");
            if (c.kind == ConstraintKind::Angle)
                c.value *= kPi / 180.0;
        }
        if (c.kind == ConstraintKind::Fixed && c.a.pos != PointPos::None) {
            const auto pt = sk.pointOf(c.a);
            c.value = pt->x;
            c.value2 = pt->y;
        }
        ids.push_back(c.id);
        sk.constraints.push_back(std::move(c));
    }

    const SolveResult res = SketchSolver::solve(sk);
    if (res.state == SketchDofState::OverConstrained || res.state == SketchDofState::Failed) {
        Json details{{"conflicting", Json(std::vector<std::string>(res.conflicting.begin(), res.conflicting.end()))},
                     {"redundant", Json(std::vector<std::string>(res.redundant.begin(), res.redundant.end()))}};
        throw IpcError(std::string("Constraints rejected: sketch would be ") + dofStateName(res.state) + " " +
                       details.dump());
    }
    auto n = std::make_shared<FeatureNode>(*f);
    n->params = Json(sp);
    commit(UpdateFeatureCommand(n), n->id);
    return Json{{"sketch_id", f->id},
                {"constraint_ids", ids},
                {"dof", res.dof},
                {"state", dofStateName(res.state)},
                {"fully_constrained", res.state == SketchDofState::FullyConstrained}};
}

// ------------------------------------------------------------------ C. 3D operations

namespace {
std::vector<std::string> sketchRegions(const ModelSnapshot& s, const Uuid& sketch)
{
    std::vector<std::string> keys;
    for (const auto& pv : s.profiles)
        if (pv.sketchId == sketch)
            keys.push_back(pv.regionKey);
    return keys;
}

ProfileRef pickProfile(const Uuid& sketch, int index, const DocState& d)
{
    auto snap = KernelEngine::computeSnapshot(d);
    const auto keys = sketchRegions(*snap, sketch);
    if (keys.empty())
        throw IpcError("The sketch has no closed profile", -32602);
    ProfileRef ref;
    ref.sketch = sketch;
    if (index >= 0) {
        if (index >= int(keys.size()))
            throw IpcError("profile_index out of range: the sketch has " + std::to_string(keys.size()) +
                               " profile(s), use -1 for all",
                           -32602);
        ref.regions = {keys[size_t(index)]};
    }
    return ref;
}
}  // namespace

Json IpcCommandDispatcher::extrude(const Json& p)
{
    FeaturePtr sk = requireFeature(reqStr(p, "sketch_id"), FeatureType::Sketch);
    const double d = reqNum(p, "distance_mm");
    if (std::abs(d) < 1e-9)
        throw IpcError("distance_mm must not be zero", -32602);
    ExtrudeParams ep;
    ep.profile = pickProfile(sk->id, p.value("profile_index", 0), doc());
    ep.distance = std::abs(d);
    ep.reversed = d < 0;
    ep.symmetric = p.value("symmetric", false);
    ep.op = parseOp(p.value("operation", std::string("new_body")));
    FeaturePtr f = makeFeature(FeatureType::Extrude, doc().nextName("Extrude"), Json(ep));
    const Analysis a = commit(AddFeatureCommand(f), f->id);
    const Uuid body = a.bodies.empty() ? Uuid{} : (ep.op == BooleanOp::NewBody ? f->id : a.bodies.back().id);
    return Json{{"feature_id", f->id}, {"body_id", body}, {"status", statusName(a.result(f->id)->status)},
                {"body", bodySummary(a, body)}};
}

Json IpcCommandDispatcher::revolve(const Json& p)
{
    FeaturePtr sk = requireFeature(reqStr(p, "sketch_id"), FeatureType::Sketch);
    RevolveParams rp;
    rp.profile = pickProfile(sk->id, p.value("profile_index", -1), doc());
    rp.axis = p.contains("axis_geo_id") ? reqStr(p, "axis_geo_id") : std::string("Y");
    rp.angle = p.value("angle_deg", 360.0);
    rp.op = parseOp(p.value("operation", std::string("new_body")));
    FeaturePtr f = makeFeature(FeatureType::Revolve, doc().nextName("Revolve"), Json(rp));
    const Analysis a = commit(AddFeatureCommand(f), f->id);
    const Uuid body = a.bodies.empty() ? Uuid{} : (rp.op == BooleanOp::NewBody ? f->id : a.bodies.back().id);
    return Json{{"feature_id", f->id}, {"body_id", body}, {"status", statusName(a.result(f->id)->status)},
                {"body", bodySummary(a, body)}};
}

Json IpcCommandDispatcher::filletOrChamfer(const Json& p)
{
    const std::string type = p.value("type", std::string("fillet"));
    if (type != "fillet" && type != "chamfer")
        throw IpcError("type must be 'fillet' or 'chamfer'", -32602);
    if (!p.contains("edge_ids") || !p["edge_ids"].is_array() || p["edge_ids"].empty())
        throw IpcError("edge_ids must be a non-empty list (ids from query_topology)", -32602);
    auto snap = KernelEngine::computeSnapshot(doc());
    EdgeBlendParams bp;
    bp.value = reqNum(p, "radius_or_distance_mm");
    Uuid owner;
    for (const Json& e : p["edge_ids"]) {
        const MeshEdge* me = findEdge(*snap, e.get<std::string>(), &owner);
        if (!me)
            throw IpcError("Unknown edge id (use query_topology): " + e.get<std::string>(), -32602);
        bp.edges.push_back(TopoRef{me->name, me->sig});
    }
    const char* ft = type == "fillet" ? FeatureType::Fillet : FeatureType::Chamfer;
    FeaturePtr f = makeFeature(ft, doc().nextName(ft), Json(bp));
    const Analysis a = commit(AddFeatureCommand(f), f->id);
    return Json{{"feature_id", f->id}, {"body_id", owner}, {"status", statusName(a.result(f->id)->status)},
                {"body", bodySummary(a, owner)}};
}

Json IpcCommandDispatcher::shell(const Json& p)
{
    auto snap = KernelEngine::computeSnapshot(doc());
    ShellParams sp;
    sp.body = p.value("body_id", std::string{});
    sp.thickness = reqNum(p, "thickness_mm");
    sp.outward = p.value("direction", std::string("inside")) == "outside";
    if (p.contains("removed_face_ids"))
        for (const Json& j : p["removed_face_ids"]) {
            Uuid owner;
            const MeshFace* mf = findFace(*snap, j.get<std::string>(), &owner);
            if (!mf)
                throw IpcError("Unknown face id (use query_topology): " + j.get<std::string>(), -32602);
            if (sp.body.empty())
                sp.body = owner;
            sp.removedFaces.push_back(TopoRef{mf->name, mf->sig});
        }
    FeaturePtr f = makeFeature(FeatureType::Shell, doc().nextName("Shell"), Json(sp));
    const Analysis a = commit(AddFeatureCommand(f), f->id);
    const Uuid body = sp.body.empty() && !a.bodies.empty() ? a.bodies.back().id : sp.body;
    return Json{{"feature_id", f->id}, {"body_id", body}, {"status", statusName(a.result(f->id)->status)},
                {"body", bodySummary(a, body)}};
}

Json IpcCommandDispatcher::combine(const Json& p)
{
    CombineParams cp;
    cp.target = reqStr(p, "target_body_id");
    if (!p.contains("tool_body_ids") || !p["tool_body_ids"].is_array() || p["tool_body_ids"].empty())
        throw IpcError("tool_body_ids must be a non-empty list of body ids", -32602);
    for (const Json& t : p["tool_body_ids"])
        cp.tools.push_back(t.get<std::string>());
    cp.op = parseOp(p.value("operation", std::string("join")));
    if (cp.op == BooleanOp::NewBody)
        throw IpcError("operation must be join, cut or intersect", -32602);
    cp.keepTools = p.value("keep_tools", false);
    const Analysis before = KernelEngine::analyze(doc());
    auto exists = [&](const Uuid& id) {
        return std::any_of(before.bodies.begin(), before.bodies.end(), [&](const BodyMetrics& b) { return b.id == id; });
    };
    if (!exists(cp.target))
        throw IpcError("Unknown target body id: " + cp.target, -32602);
    for (const Uuid& t : cp.tools)
        if (!exists(t))
            throw IpcError("Unknown tool body id: " + t, -32602);
    FeaturePtr f = makeFeature(FeatureType::Combine, doc().nextName("Combine"), Json(cp));
    const Analysis a = commit(AddFeatureCommand(f), f->id);
    Json remaining = Json::array();
    for (const auto& b : a.bodies)
        remaining.push_back(b.id);
    return Json{{"feature_id", f->id}, {"body_id", cp.target}, {"status", statusName(a.result(f->id)->status)},
                {"body", bodySummary(a, cp.target)}, {"bodies", remaining}};
}

GeomRef IpcCommandDispatcher::geomRefFromId(const std::string& id, bool planes) const
{
    static const std::vector<std::string> axes{"X", "Y", "Z"}, origin{"XY", "XZ", "YZ"};
    const auto& world = planes ? origin : axes;
    if (std::find(world.begin(), world.end(), id) != world.end())
        return GeomRef{id, {}};
    auto snap = KernelEngine::computeSnapshot(doc());
    if (const MeshFace* f = findFace(*snap, id)) {
        if (planes && !f->planar)
            throw IpcError("The mirror plane face must be planar", -32602);
        return GeomRef{"Face", TopoRef{f->name, f->sig}};
    }
    if (!planes)
        if (const MeshEdge* e = findEdge(*snap, id))
            return GeomRef{"Edge", TopoRef{e->name, e->sig}};
    std::string accepted;
    for (const auto& w : world)
        accepted += w + " ";
    throw IpcError("Unknown reference '" + id + "' (use " + accepted + "or an id from query_topology)", -32602);
}

TransformTargets IpcCommandDispatcher::targetsFrom(const Json& p) const
{
    TransformTargets t;
    t.type = p.value("target_type", std::string("body"));
    if (t.type != "body" && t.type != "feature")
        throw IpcError("target_type must be 'body' or 'feature'", -32602);
    if (!p.contains("target_ids") || !p["target_ids"].is_array() || p["target_ids"].empty())
        throw IpcError("target_ids must be a non-empty list", -32602);
    for (const Json& id : p["target_ids"]) {
        const std::string s = id.get<std::string>();
        if (t.type == "feature") {
            FeaturePtr f = requireFeature(s);
            if (f->type != FeatureType::Extrude && f->type != FeatureType::Revolve)
                throw IpcError("Only extrusions and revolutions can be copied as features (" + f->name + " is a " +
                                   f->type + ")",
                               -32602);
        }
        t.ids.push_back(s);
    }
    return t;
}

Json IpcCommandDispatcher::patternResult(const Analysis& a, const Uuid& fid) const
{
    Json bodies = Json::array();
    for (const auto& b : a.bodies)
        bodies.push_back({{"body_id", b.id}, {"volume_mm3", b.volume}, {"valid", b.valid}, {"solids", b.solids}});
    const FeatureResult* r = a.result(fid);
    Json out{{"feature_id", fid}, {"status", r ? statusName(r->status) : "?"}, {"bodies", bodies}};
    if (r && !r->message.empty())
        out["message"] = r->message;
    return out;
}

Json IpcCommandDispatcher::mirror(const Json& p)
{
    MirrorParams mp;
    mp.targets = targetsFrom(p);
    mp.plane = geomRefFromId(reqStr(p, "mirror_plane_id"), true);
    mp.merge = p.value("merge_result", true);
    FeaturePtr f = makeFeature(FeatureType::Mirror, doc().nextName("Mirror"), Json(mp));
    return patternResult(commit(AddFeatureCommand(f), f->id), f->id);
}

Json IpcCommandDispatcher::linearPattern(const Json& p)
{
    LinearPatternParams lp;
    lp.targets = targetsFrom(p);
    lp.dir1.ref = geomRefFromId(reqStr(p, "direction_1_id"), false);
    lp.dir1.count = static_cast<int>(reqNum(p, "count_1"));
    lp.dir1.spacing = reqNum(p, "spacing_1_mm");
    lp.dir1.reverse = p.value("reverse_1", false);
    lp.dir1.symmetric = p.value("symmetric_1", false);
    if (lp.dir1.count < 2)
        throw IpcError("count_1 must be >= 2", -32602);
    if (lp.dir1.spacing <= 0)
        throw IpcError("spacing_1_mm must be > 0", -32602);
    lp.dir2.count = p.value("count_2", 1);
    if (lp.dir2.count > 1) {
        if (!p.contains("direction_2_id") || p["direction_2_id"].is_null())
            throw IpcError("direction_2_id is required when count_2 > 1", -32602);
        lp.dir2.ref = geomRefFromId(p["direction_2_id"].get<std::string>(), false);
        lp.dir2.spacing = p.value("spacing_2_mm", 10.0);
        lp.dir2.reverse = p.value("reverse_2", false);
        lp.dir2.symmetric = p.value("symmetric_2", false);
        if (lp.dir2.spacing <= 0)
            throw IpcError("spacing_2_mm must be > 0", -32602);
    }
    FeaturePtr f = makeFeature(FeatureType::LinearPattern, doc().nextName("LinearPattern"), Json(lp));
    return patternResult(commit(AddFeatureCommand(f), f->id), f->id);
}

Json IpcCommandDispatcher::circularPattern(const Json& p)
{
    CircularPatternParams cp;
    cp.targets = targetsFrom(p);
    cp.axis = geomRefFromId(reqStr(p, "axis_id"), false);
    cp.count = static_cast<int>(reqNum(p, "count"));
    cp.totalAngle = p.value("total_angle_deg", 360.0);
    cp.equalSpacing = p.value("equal_spacing", true);
    cp.reverse = p.value("reverse", false);
    if (cp.count < 2)
        throw IpcError("count must be >= 2", -32602);
    FeaturePtr f = makeFeature(FeatureType::CircularPattern, doc().nextName("CircularPattern"), Json(cp));
    return patternResult(commit(AddFeatureCommand(f), f->id), f->id);
}

// ------------------------------------------------------------------ D. editing, history, I/O

Json IpcCommandDispatcher::updateFeatureParameter(const Json& p)
{
    FeaturePtr f = requireFeature(reqStr(p, "feature_id"));
    const std::string name = reqStr(p, "param_name");
    const double v = reqNum(p, "new_value");
    auto n = std::make_shared<FeatureNode>(*f);
    auto is = [&](std::initializer_list<const char*> names) {
        return std::any_of(names.begin(), names.end(), [&](const char* x) { return name == x; });
    };

    if (f->type == FeatureType::Sketch) {
        SketchParams sp = f->as<SketchParams>();
        auto it = std::find_if(sp.sketch.constraints.begin(), sp.sketch.constraints.end(),
                               [&](const SketchConstraint& c) { return c.id == name; });
        if (it == sp.sketch.constraints.end() || !isDimensional(it->kind))
            throw IpcError("For a sketch, param_name must be the id of a dimensional constraint", -32602);
        it->value = it->kind == ConstraintKind::Angle ? v * kPi / 180.0 : v;
        const SolveResult res = SketchSolver::solve(sp.sketch);
        if (!res.converged)
            throw IpcError("The sketch cannot be solved with this value");
        n->params = Json(sp);
    }
    else if (f->type == FeatureType::Extrude && is({"distance", "distance_mm"})) {
        if (std::abs(v) < 1e-9)
            throw IpcError("distance must not be zero", -32602);
        n->params["distance"] = std::abs(v);
        n->params["reversed"] = v < 0;
    }
    else if (f->type == FeatureType::Revolve && is({"angle", "angle_deg"})) {
        n->params["angle"] = v;
    }
    else if ((f->type == FeatureType::Fillet || f->type == FeatureType::Chamfer) &&
             is({"value", "radius", "distance", "radius_or_distance_mm"})) {
        n->params["value"] = v;
    }
    else if (f->type == FeatureType::Shell && is({"thickness", "thickness_mm"})) {
        n->params["thickness"] = v;
    }
    else if (f->type == FeatureType::CircularPattern && is({"count"})) {
        n->params["count"] = static_cast<int>(v);
    }
    else if (f->type == FeatureType::CircularPattern && is({"total_angle_deg", "totalAngle"})) {
        n->params["totalAngle"] = v;
    }
    else if (f->type == FeatureType::LinearPattern && is({"count_1", "spacing_1_mm", "count_2", "spacing_2_mm"})) {
        Json& d = n->params[name.back() == '1' || name.find("_1") != std::string::npos ? "dir1" : "dir2"];
        if (name.rfind("count", 0) == 0)
            d["count"] = static_cast<int>(v);
        else
            d["spacing"] = v;
    }
    else if (n->params.contains(name) && n->params[name].is_number()) {
        n->params[name] = v;
    }
    else {
        throw IpcError("Unknown numeric parameter '" + name + "' for a " + f->type, -32602);
    }
    const Analysis a = commit(UpdateFeatureCommand(n), n->id);
    Json bodies = Json::array();
    for (const auto& b : a.bodies)
        bodies.push_back({{"body_id", b.id}, {"volume_mm3", b.volume}, {"valid", b.valid}});
    return Json{{"feature_id", f->id}, {"status", statusName(a.result(f->id)->status)}, {"bodies", bodies}};
}

Json IpcCommandDispatcher::undo(const Json&)
{
    const bool did = m_model->history().canUndo();
    m_model->undo();
    return Json{{"done", did}, {"can_undo", m_model->history().canUndo()}, {"can_redo", m_model->history().canRedo()},
                {"feature_count", doc().features.size()}};
}

Json IpcCommandDispatcher::redo(const Json&)
{
    const bool did = m_model->history().canRedo();
    m_model->redo();
    return Json{{"done", did}, {"can_undo", m_model->history().canUndo()}, {"can_redo", m_model->history().canRedo()},
                {"feature_count", doc().features.size()}};
}

Json IpcCommandDispatcher::importStep(const Json& p)
{
    const std::string path = reqStr(p, "file_path");
    ImportParams ip;
    ip.sourcePath = QFileInfo(QString::fromStdString(path)).fileName().toStdString();
    try {
        ip.brep = KernelEngine::importStepAsBrep(path);
    }
    catch (const std::exception& e) {
        throw IpcError(e.what());
    }
    FeaturePtr f = makeFeature(FeatureType::ImportStep, QFileInfo(QString::fromStdString(path)).completeBaseName().toStdString(),
                               Json(ip));
    const Analysis a = commit(AddFeatureCommand(f), f->id);
    return Json{{"feature_id", f->id}, {"body_id", f->id}, {"status", statusName(a.result(f->id)->status)},
                {"body", bodySummary(a, f->id)}};
}

Json IpcCommandDispatcher::exportStep(const Json& p)
{
    const std::string path = reqStr(p, "file_path");
    if (auto err = KernelEngine::exportStep(doc(), path, p.value("body_id", std::string{})))
        throw IpcError(*err);
    return Json{{"file_path", path}, {"bytes", QFileInfo(QString::fromStdString(path)).size()}};
}

Json IpcCommandDispatcher::exportStl(const Json& p)
{
    const std::string path = reqStr(p, "file_path");
    if (auto err = KernelEngine::exportStl(doc(), path, p.value("body_id", std::string{}),
                                           p.value("linear_deflection", 0.05)))
        throw IpcError(*err);
    return Json{{"file_path", path}, {"bytes", QFileInfo(QString::fromStdString(path)).size()}};
}

Json IpcCommandDispatcher::saveDesign(const Json& p)
{
    QString path = QString::fromStdString(reqStr(p, "file_path"));
    if (!path.endsWith(".silex", Qt::CaseInsensitive))
        path += ".silex";
    QString err;
    if (!m_model->save(path, &err))
        throw IpcError("Cannot save the design: " + err.toStdString());
    return Json{{"file_path", path.toStdString()}, {"bytes", QFileInfo(path).size()},
                {"feature_count", doc().features.size()}};
}

Json IpcCommandDispatcher::shutdown(const Json&)
{
    m_quit = true;
    QTimer::singleShot(50, qApp, &QCoreApplication::quit);
    return Json{{"ok", true}};
}

}  // namespace silex::ipc
