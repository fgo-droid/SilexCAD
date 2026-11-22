#include "IpcCommandDispatcher.hpp"

#include <QApplication>
#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImage>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>

#include "ComponentLibrary.h"
#include "EnclosureWizard.h"
#include "LessonRecorder.h"
#include "DrawingSheet.h"
#include "ModelController.h"
#include "Viewport.h"
#include "model/Bindings.h"
#include "sketch/SketchSolver.h"
#include "sketch/SketchTools.h"
#include "sketch/VectorImporter.h"
#include "model/Printability.h"
#include "model/Pedagogy.h"
#include "sketch/SketchCoach.h"

namespace silex::ipc {

namespace {
/// Body modified by a Join / Cut without explicit target: the last body that is not a locked component.
Uuid lastEditableId(const Analysis& a)
{
    for (auto it = a.bodies.rbegin(); it != a.bodies.rend(); ++it)
        if (!it->locked)
            return it->id;
    return a.bodies.empty() ? Uuid{} : a.bodies.back().id;
}
}  // namespace

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
    std::vector<std::string> out;
    for (const auto& [name, fn] : table())
        out.push_back(name);
    return out;
}

Json IpcCommandDispatcher::errorJson(const IpcError& e)
{
    const std::string msg = e.what();
    std::string code = e.errorCode(), hint = e.hint();
    auto starts = [&](const char* s) { return msg.rfind(s, 0) == 0; };
    if (code.empty()) {
        switch (e.code()) {
        case -32700: code = "PARSE_ERROR"; break;
        case -32600: code = "INVALID_REQUEST"; break;
        case -32601: code = "METHOD_NOT_FOUND"; break;
        case -32602:
            code = starts("Unknown ") ? "UNKNOWN_ID" : starts("Missing ") ? "MISSING_PARAMETER" : "INVALID_PARAMS";
            break;
        case -32003: code = "TUTOR_MODE_REFUSED"; break;
        default: code = "OPERATION_FAILED"; break;
        }
    }
    if (hint.empty()) {
        if (code == "PARSE_ERROR" || code == "INVALID_REQUEST")
            hint = "Send one JSON object per line: {\"id\": 1, \"method\": \"ping\", \"params\": {}}.";
        else if (code == "METHOD_NOT_FOUND")
            hint = "Call ping: its \"methods\" field lists every available method.";
        else if (code == "UNKNOWN_ID")
            hint = "Ids change when the model is rebuilt: call get_model_tree (features, bodies) or query_topology "
                   "(faces, edges) to get the current ones.";
        else if (code == "MISSING_PARAMETER" || code == "INVALID_PARAMS")
            hint = "Check the parameter names, types and units (millimetres, degrees) in the tool description.";
        else if (code == "TUTOR_MODE_REFUSED")
            hint = "Only observation tools are allowed in tutor mode: guide the student with questions.";
        else
            hint = "Inspect the model (get_model_tree, inspect_body, get_pedagogical_context) and retry with "
                   "adjusted values; the document was left unchanged.";
    }
    return Json{{"code", e.code()}, {"error_code", code}, {"message", msg}, {"hint", hint}};
}

std::string IpcCommandDispatcher::handleLine(const std::string& line)
{
    Json req;
    try {
        req = Json::parse(line);
    }
    catch (const std::exception&) {
        return Json{{"id", nullptr}, {"error", errorJson(IpcError("Parse error", -32700))}}.dump();
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
        return Json{{"id", id}, {"error", errorJson(e)}}.dump();
    }
    catch (const Json::exception& e) {
        return Json{{"id", id}, {"error", errorJson(IpcError(std::string("Invalid parameter value: ") + e.what(), -32602,
                                                             "INVALID_PARAMS"))}}.dump();
    }
    catch (const std::exception& e) {
        return Json{{"id", id}, {"error", errorJson(IpcError(e.what(), -32000, "INTERNAL_ERROR"))}}.dump();
    }
}

const std::map<std::string, IpcCommandDispatcher::Handler>& IpcCommandDispatcher::table()
{
    static const std::map<std::string, Handler> table{
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
        {"create_box", &IpcCommandDispatcher::createBox},
        {"create_cylinder", &IpcCommandDispatcher::createCylinder},
        {"press_pull", &IpcCommandDispatcher::pressPull},
        {"remove_body", &IpcCommandDispatcher::removeBody},
        {"defeature_faces", &IpcCommandDispatcher::defeatureFaces},
        {"split_body", &IpcCommandDispatcher::splitBody},
        {"transform_body", &IpcCommandDispatcher::transformBody},
        {"create_construction_plane", &IpcCommandDispatcher::createConstructionPlane},
        {"create_rib", &IpcCommandDispatcher::createRib},
        {"align_bodies", &IpcCommandDispatcher::alignBodies},
        {"check_printability", &IpcCommandDispatcher::checkPrintability},
        {"create_thread", &IpcCommandDispatcher::createThread},
        {"get_pedagogical_context", &IpcCommandDispatcher::getPedagogicalContext},
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
        {"sweep", &IpcCommandDispatcher::sweep},
        {"loft", &IpcCommandDispatcher::loft},
        {"get_parameters", &IpcCommandDispatcher::getParameters},
        {"set_parameter", &IpcCommandDispatcher::setParameter},
        {"delete_parameter", &IpcCommandDispatcher::deleteParameter},
        {"measure", &IpcCommandDispatcher::measure},
        {"check_interference", &IpcCommandDispatcher::checkInterference},
        {"set_body_material", &IpcCommandDispatcher::setBodyMaterial},
        {"export_3mf", &IpcCommandDispatcher::export3mf},
        {"export_drawing", &IpcCommandDispatcher::exportDrawing},
        // v0.5 lot 1
        {"create_fastener_hole", &IpcCommandDispatcher::createFastenerHole},
        {"list_components", &IpcCommandDispatcher::listComponents},
        {"insert_component", &IpcCommandDispatcher::insertComponent},
        // v0.5 lot 4
        {"lay_flat_on_bed", &IpcCommandDispatcher::layFlatOnBed},
        {"create_enclosure", &IpcCommandDispatcher::createEnclosure},
        {"check_build_volume", &IpcCommandDispatcher::checkBuildVolume},
        // v0.5 lot 5
        {"record_lesson", &IpcCommandDispatcher::recordLesson},
        // sprint "Fiabilité": document cycle, undo groups, dimensional check
        {"new_document", &IpcCommandDispatcher::newDocument},
        {"open_document", &IpcCommandDispatcher::openDocument},
        {"get_active_document_info", &IpcCommandDispatcher::getActiveDocumentInfo},
        {"begin_command_group", &IpcCommandDispatcher::beginCommandGroup},
        {"end_command_group", &IpcCommandDispatcher::endCommandGroup},
        {"verify_dimensions", &IpcCommandDispatcher::verifyDimensions},
        // v0.7 "Trio Makers"
        {"create_emboss_text", &IpcCommandDispatcher::createEmbossText},
        {"sketch_import_vector", &IpcCommandDispatcher::sketchImportVector},
        {"sketch_set_canvas", &IpcCommandDispatcher::sketchSetCanvas},
    };
    return table;
}

Json IpcCommandDispatcher::dispatch(const std::string& method, const Json& p)
{
    auto it = table().find(method);
    if (it == table().end())
        throw IpcError("Method not found: " + method, -32601);
    // v0.5 lot 5: Socratic tutor — observation only, the student does the modelling
    static const std::set<std::string> kObservation{
        "ping", "get_model_tree", "inspect_body", "list_solids", "query_topology", "capture_viewport",
        "get_pedagogical_context", "get_parameters", "measure", "check_interference", "check_printability",
        "list_components", "check_build_volume", "record_lesson", "get_active_document_info", "verify_dimensions"};
    if (m_model->tutorMode() && !kObservation.count(method))
        throw IpcError("Tutor mode: Silex refuses modelling commands ('" + method +
                           "'). Guide the student with questions and hints instead (get_pedagogical_context, "
                           "capture_viewport) and let them do the modelling.",
                       -32003, "TUTOR_MODE_REFUSED");
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
        std::string hint;
        if (FeaturePtr f = doc().find(fid)) {
            ErrorContext ctx;
            for (const auto& b : a.bodies) {
                const Vec3 e = b.bbox.max - b.bbox.min;
                const double lo = std::min({e.x, e.y, e.z});
                if (lo > 0)
                    ctx.smallestBodyExtent = ctx.smallestBodyExtent > 0 ? std::min(ctx.smallestBodyExtent, lo) : lo;
                ctx.largestBodyExtent = std::max({ctx.largestBodyExtent, e.x, e.y, e.z});
            }
            hint = explainFeatureError(f->type, msg, f->params, ctx, false);
        }
        m_model->undo();
        throw IpcError("Rebuild failed: " + msg + " (the change was undone)", -32000, "REBUILD_FAILED",
                       hint.empty() ? "The operation could not be built on this geometry: check the referenced "
                                      "faces / edges (query_topology) and try smaller values."
                                    : hint);
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
                {"protocol_version", kIpcProtocolVersion},
                {"methods", methods()},
                {"tutor_mode", m_model->tutorMode()}};
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
                        {"edges", b.edges},
                        {"material", doc().materialOf(b.id)},
                        {"mass_g", massGrams(b.volume, doc().materialOf(b.id).density)}};
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
    else if (FeaturePtr cpf = doc().find(target); cpf && cpf->type == FeatureType::ConstructionPlane) {
        sp.plane.kind = "Plane";  // v0.4: sketch on a construction plane
        sp.plane.planeId = cpf->id;
        auto snap = KernelEngine::computeSnapshot(doc());
        for (const auto& pv : snap->planes)
            if (pv.id == cpf->id)
                pl = pv.plane;
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
    sp.plane.offset = p.value("offset_mm", 0.0);
    pl = offsetPlacement(pl, sp.plane.offset);
    std::string name = p.value("name", std::string{});
    if (name.empty())
        name = doc().nextName("Sketch");
    FeaturePtr f = makeFeature(FeatureType::Sketch, name, Json(sp));
    commit(AddFeatureCommand(f), f->id);
    return Json{{"sketch_id", f->id},
                {"name", name},
                {"plane", {{"origin", vec(pl.origin)}, {"x_dir", vec(pl.xDir)}, {"normal", vec(pl.normal)}}}};
}

namespace {
std::vector<Uuid> reqIds(const Json& el, const char* key)
{
    if (!el.contains(key) || !el[key].is_array() || el[key].empty())
        throw IpcError(std::string("'") + key + "' must be a non-empty list of geo ids", -32602);
    std::vector<Uuid> ids;
    for (const Json& j : el[key])
        ids.push_back(parseRef(j.get<std::string>()).geo);
    return ids;
}

/// Sketcher Pro element types of sketch_add_geometry (smart primitives and 2D editing tools).
void sketchToolElement(Sketch& sk, const Json& el, const std::string& type, bool cons, Json& r)
{
    namespace st = sketchtools;
    auto dims = Json::array();
    auto dim = [&](SketchConstraint c) {
        dims.push_back(c.id);
        sk.constraints.push_back(std::move(c));
    };
    auto attachOrigin = [&](const GeoRef& p, Vec2 at) {
        if (at.length() < 1e-9 && el.value("attach_origin", true))
            sk.constraints.push_back(mkC(ConstraintKind::Coincident, p, {kOriginId, PointPos::Start}));
    };
    const bool dimension = el.value("dimension", false);
    st::ToolResult res;
    if (type == "center_rectangle") {
        const Vec2 c = reqVec2(el, "center", Vec2{0, 0});
        Vec2 corner;
        if (el.contains("corner"))
            corner = reqVec2(el, "corner");
        else {
            const double w = reqNum(el, "width"), h = reqNum(el, "height");
            if (w <= 0 || h <= 0)
                throw IpcError("Rectangle width and height must be positive", -32602);
            corner = c + Vec2{w / 2, h / 2};
        }
        res = st::addCenterRectangle(sk, c, corner, cons);
        if (res) {
            attachOrigin({res.named["center"], PointPos::Start}, c);
            if (dimension) {
                const double w = 2 * std::abs(corner.x - c.x), h = 2 * std::abs(corner.y - c.y);
                dim(mkC(ConstraintKind::Distance, {res.named["bottom"]}, {}, w));
                dim(mkC(ConstraintKind::Distance, {res.named["left"]}, {}, h));
            }
        }
    }
    else if (type == "slot") {
        const double width = reqNum(el, "width");
        Vec2 c1, c2;
        const bool overall = !el.contains("center1");
        if (!overall) {
            c1 = reqVec2(el, "center1");
            c2 = reqVec2(el, "center2");
        }
        else {
            const Vec2 c = reqVec2(el, "center", Vec2{0, 0});
            const double len = reqNum(el, "length");
            const double a = el.value("angle", 0.0) * kPi / 180.0;
            const Vec2 h = Vec2{std::cos(a), std::sin(a)} * (len / 2);
            c1 = c - h;
            c2 = c + h;
        }
        res = st::addSlot(sk, c1, c2, width / 2, overall, cons);
        if (res) {
            if (overall)
                attachOrigin({res.named["center"], PointPos::Start}, (c1 + c2) * 0.5);
            else
                attachOrigin({res.named["axis"], PointPos::Start}, c1);
            if (dimension) {
                dim(mkC(ConstraintKind::Distance, {res.named["axis"]}, {}, (c2 - c1).length()));
                dim(mkC(ConstraintKind::Radius, {res.named["arc1"]}, {}, width / 2));
            }
        }
    }
    else if (type == "polygon") {
        const Vec2 c = reqVec2(el, "center", Vec2{0, 0});
        const int sides = el.value("sides", 6);
        const double R = reqNum(el, "radius");
        const double a = el.value("angle", 0.0) * kPi / 180.0;
        const std::string mode = el.value("mode", std::string("inscribed"));
        if (mode != "inscribed" && mode != "circumscribed")
            throw IpcError("polygon mode must be 'inscribed' or 'circumscribed'", -32602);
        const bool inscribed = mode == "inscribed";
        res = st::addPolygon(sk, c, c + Vec2{std::cos(a), std::sin(a)} * R, sides, inscribed, cons);
        if (res) {
            attachOrigin({res.named["circle"], PointPos::Center}, c);
            if (dimension)
                dim(mkC(ConstraintKind::Diameter, {res.named[inscribed ? "circle" : "incircle"]}, {}, 2 * R));
        }
    }
    else if (type == "sketch_fillet") {
        const double radius = reqNum(el, "radius");
        if (el.contains("point"))
            res = st::filletAtPoint(sk, parseRef(reqStr(el, "point")), radius);
        else
            res = st::filletLines(sk, parseRef(reqStr(el, "a")).geo, parseRef(reqStr(el, "b")).geo, radius);
    }
    else if (type == "offset") {
        res = st::offsetCurves(sk, reqIds(el, "geo_ids"), reqNum(el, "distance"));
    }
    else if (type == "trim" || type == "extend") {
        const Uuid id = parseRef(reqStr(el, "geo_id")).geo;
        const Vec2 at = reqVec2(el, "point");
        res = type == "trim" ? st::trim(sk, id, at) : st::extend(sk, id, at);
    }
    else if (type == "mirror") {
        res = st::mirrorCurves(sk, reqIds(el, "geo_ids"), parseRef(reqStr(el, "axis")));
    }
    else if (type == "linear_pattern") {
        Vec2 dir{1, 0};
        if (el.contains("direction"))
            dir = reqVec2(el, "direction");
        else if (el.contains("angle")) {
            const double a = reqNum(el, "angle") * kPi / 180.0;
            dir = {std::cos(a), std::sin(a)};
        }
        res = st::linearPattern(sk, reqIds(el, "geo_ids"), dir, el.value("count", 2), reqNum(el, "spacing"));
    }
    else if (type == "circular_pattern") {
        res = st::circularPattern(sk, reqIds(el, "geo_ids"), parseRef(el.value("center", std::string("origin"))),
                                  el.value("count", 2), el.value("total_angle", 360.0) * kPi / 180.0);
    }
    if (!res)
        throw IpcError(type + ": " + res.error, -32602);
    for (const Uuid& id : res.geos) {
        r["geo_ids"].push_back(id);
        if (const SketchGeo* g = sk.find(id))
            for (const auto& pid : pointIds(*g))
                r["point_ids"].push_back(pid);
    }
    if (!res.named.empty())
        r["named"] = res.named;
    r["constraint_ids"] = res.constraints;
    if (!dims.empty())
        r["dimension_ids"] = dims;
    if (!res.driving.empty())
        r["driving_constraint_id"] = res.driving;
    if (res.removedConstraints)
        r["removed_constraints"] = res.removedConstraints;
}
}  // namespace

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
        // ------------------------------------------------ Sketcher Pro
        else if (type == "center_rectangle" || type == "slot" || type == "polygon" || type == "sketch_fillet" ||
                 type == "offset" || type == "trim" || type == "extend" || type == "mirror" ||
                 type == "linear_pattern" || type == "circular_pattern") {
            sketchToolElement(sk, el, type, cons, r);
        }
        else {
            throw IpcError("Unknown geometry type '" + type +
                               "' (line, rectangle, circle, arc, center_rectangle, slot, polygon, sketch_fillet, offset, "
                               "trim, extend, mirror, linear_pattern, circular_pattern)",
                           -32602);
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
        // Sketcher Pro
        {"midpoint", ConstraintKind::Midpoint},           {"symmetry", ConstraintKind::Symmetric},
        {"symmetric", ConstraintKind::Symmetric},         {"concentric", ConstraintKind::Concentric},
        {"collinear", ConstraintKind::Collinear},
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
        if (c.kind == ConstraintKind::Symmetric) {
            const std::string axis = cj.contains("axis") ? reqStr(cj, "axis") : cj.value("c", std::string{});
            if (axis.empty())
                throw IpcError("symmetry needs 'axis' (a line geo_id, 'x_axis' or 'y_axis')", -32602);
            const Uuid id = c.id;
            c = sketchtools::makeSymmetric(sk, c.a, c.b, parseRef(axis));
            c.id = id;
        }
        if (std::string why = sketchtools::checkConstraint(sk, c); !why.empty())
            throw IpcError("Constraint '" + type + "': " + why, -32602);
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
    const Uuid body = a.bodies.empty() ? Uuid{} : (ep.op == BooleanOp::NewBody ? f->id : lastEditableId(a));
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
    const Uuid body = a.bodies.empty() ? Uuid{} : (rp.op == BooleanOp::NewBody ? f->id : lastEditableId(a));
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
    const Uuid body = sp.body.empty() && !a.bodies.empty() ? lastEditableId(a) : sp.body;
    return Json{{"feature_id", f->id}, {"body_id", body}, {"status", statusName(a.result(f->id)->status)},
                {"body", bodySummary(a, body)}};
}

// ------------------------------------------------------------------ direct modelling (ergonomics lot)

Json IpcCommandDispatcher::createBox(const Json& p) { return createPrimitive(p, FeatureType::Box); }
Json IpcCommandDispatcher::createCylinder(const Json& p) { return createPrimitive(p, FeatureType::Cylinder); }

Json IpcCommandDispatcher::createPrimitive(const Json& p, const char* type)
{
    PrimitiveParams pp;
    const std::string target = p.value("plane_or_face_id", std::string("XY"));
    if (target == "XY" || target == "XZ" || target == "YZ")
        pp.plane.kind = target;
    else if (FeaturePtr cpf = doc().find(target); cpf && cpf->type == FeatureType::ConstructionPlane) {
        pp.plane.kind = "Plane";
        pp.plane.planeId = cpf->id;
    }
    else {
        auto snap = KernelEngine::computeSnapshot(doc());
        const MeshFace* f = findFace(*snap, target);
        if (!f)
            throw IpcError("Unknown face id (use query_topology): " + target, -32602);
        if (!f->planar)
            throw IpcError("The base face is not planar", -32602);
        pp.plane.kind = "Face";
        pp.plane.face = TopoRef{f->name, f->sig};
    }
    pp.plane.offset = p.value("offset_mm", 0.0);
    const Vec2 c = reqVec2(p, "center", Vec2{0, 0});
    pp.centerX = c.x;
    pp.centerY = c.y;
    pp.height = reqNum(p, "height_mm");
    if (std::abs(pp.height) < 1e-9)
        throw IpcError("height_mm must not be zero", -32602);
    const bool cylinder = std::string(type) == FeatureType::Cylinder;
    if (cylinder) {
        pp.diameter = reqNum(p, "diameter_mm");
        if (pp.diameter <= 0)
            throw IpcError("diameter_mm must be positive", -32602);
    }
    else {
        pp.length = reqNum(p, "length_mm");
        pp.width = reqNum(p, "width_mm");
        if (pp.length <= 0 || pp.width <= 0)
            throw IpcError("length_mm and width_mm must be positive", -32602);
    }
    pp.op = parseOp(p.value("operation", std::string("new_body")));
    pp.targetBody = p.value("target_body_id", std::string{});
    FeaturePtr f = makeFeature(type, doc().nextName(type), Json(pp));
    const Analysis a = commit(AddFeatureCommand(f), f->id);
    const Uuid body = a.bodies.empty() ? Uuid{}
                      : pp.op == BooleanOp::NewBody ? f->id
                      : !pp.targetBody.empty()     ? pp.targetBody
                                                   : lastEditableId(a);
    return Json{{"feature_id", f->id}, {"body_id", body}, {"status", statusName(a.result(f->id)->status)},
                {"body", bodySummary(a, body)}};
}

Json IpcCommandDispatcher::pressPull(const Json& p)
{
    auto snap = KernelEngine::computeSnapshot(doc());
    const std::string id = reqStr(p, "face_id");
    Uuid owner;
    const MeshFace* mf = findFace(*snap, id, &owner);
    if (!mf)
        throw IpcError("Unknown face id (use query_topology): " + id, -32602);
    if (!mf->planar)
        throw IpcError("Press / Pull needs a planar face", -32602);
    OffsetFaceParams op;
    op.face = TopoRef{mf->name, mf->sig};
    op.body = owner;
    op.distance = reqNum(p, "distance_mm");
    if (std::abs(op.distance) < 1e-9)
        throw IpcError("distance_mm must not be zero", -32602);
    FeaturePtr f = makeFeature(FeatureType::OffsetFace, doc().nextName("PressPull"), Json(op));
    const Analysis a = commit(AddFeatureCommand(f), f->id);
    return Json{{"feature_id", f->id}, {"body_id", owner}, {"status", statusName(a.result(f->id)->status)},
                {"body", bodySummary(a, owner)}};
}

// ------------------------------------------------------------------ v0.4 lot 0: bodies & faces

Json IpcCommandDispatcher::bodiesSummary(const Analysis& a) const
{
    Json bodies = Json::array();
    for (const auto& b : a.bodies)
        bodies.push_back({{"body_id", b.id}, {"name", b.name}, {"volume_mm3", b.volume}, {"valid", b.valid}});
    return bodies;
}

SketchPlaneRef IpcCommandDispatcher::parsePlaneRef(const Json& p, const char* key)
{
    SketchPlaneRef ref;
    const std::string target = p.value(key, std::string("XY"));
    if (target == "XY" || target == "XZ" || target == "YZ")
        ref.kind = target;
    else {
        auto snap = KernelEngine::computeSnapshot(doc());
        if (const MeshFace* f = findFace(*snap, target)) {
            if (!f->planar)
                throw IpcError("The face is not planar", -32602);
            ref.kind = "Face";
            ref.face = TopoRef{f->name, f->sig};
        }
        else if (FeaturePtr cp = doc().find(target); cp && cp->type == FeatureType::ConstructionPlane) {
            ref.kind = "Plane";  // v0.4 lot 1: construction plane
            ref.planeId = cp->id;
        }
        else
            throw IpcError(std::string("Unknown plane '") + target + "' (XY, XZ, YZ, a face id or a construction plane id)", -32602);
    }
    ref.offset = p.value("offset_mm", 0.0);
    return ref;
}

static std::vector<Uuid> reqIdList(const Json& p, const char* key)
{
    if (!p.contains(key) || !p[key].is_array() || p[key].empty())
        throw IpcError(std::string(key) + " must be a non-empty list", -32602);
    std::vector<Uuid> ids;
    for (const Json& j : p[key])
        ids.push_back(j.get<std::string>());
    return ids;
}

Json IpcCommandDispatcher::removeBody(const Json& p)
{
    RemoveBodyParams rp;
    rp.bodies = reqIdList(p, "body_ids");
    FeaturePtr f = makeFeature(FeatureType::RemoveBody, doc().nextName("RemoveBody"), Json(rp));
    const Analysis a = commit(AddFeatureCommand(f), f->id);
    return Json{{"feature_id", f->id}, {"status", statusName(a.result(f->id)->status)}, {"bodies", bodiesSummary(a)}};
}

Json IpcCommandDispatcher::defeatureFaces(const Json& p)
{
    auto snap = KernelEngine::computeSnapshot(doc());
    DefeatureParams dp;
    for (const Uuid& id : reqIdList(p, "face_ids")) {
        Uuid owner;
        const MeshFace* mf = findFace(*snap, id, &owner);
        if (!mf)
            throw IpcError("Unknown face id (use query_topology): " + id, -32602);
        dp.faces.push_back(TopoRef{mf->name, mf->sig});
        dp.body = owner;
    }
    FeaturePtr f = makeFeature(FeatureType::DefeatureFace, doc().nextName("DeleteFace"), Json(dp));
    const Analysis a = commit(AddFeatureCommand(f), f->id);
    return Json{{"feature_id", f->id}, {"status", statusName(a.result(f->id)->status)}, {"body", bodySummary(a, dp.body)}};
}

Json IpcCommandDispatcher::splitBody(const Json& p)
{
    SplitBodyParams sp;
    sp.body = reqStr(p, "body_id");
    sp.plane = parsePlaneRef(p, "plane");
    // v0.5 lot 4: alignment pins / pegs
    sp.pegs = p.value("pegs", false);
    sp.pegCount = p.value("peg_count", 2);
    sp.pegDiameter = p.value("peg_diameter_mm", 4.0);
    sp.pegLength = p.value("peg_length_mm", 8.0);
    sp.pegClearance = p.value("peg_clearance_mm", 0.2);
    FeaturePtr f = makeFeature(FeatureType::SplitBody, doc().nextName("Split"), Json(sp));
    const Analysis a = commit(AddFeatureCommand(f), f->id);
    Json out{{"feature_id", f->id}, {"status", statusName(a.result(f->id)->status)}, {"bodies", bodiesSummary(a)}};
    if (const auto* r = a.result(f->id); r && !r->message.empty())
        out["message"] = r->message;
    return out;
}

Json IpcCommandDispatcher::transformBody(const Json& p)
{
    TransformBodyParams tp;
    tp.bodies = reqIdList(p, "body_ids");
    tp.dx = p.value("dx_mm", 0.0);
    tp.dy = p.value("dy_mm", 0.0);
    tp.dz = p.value("dz_mm", 0.0);
    tp.rx = p.value("rx_deg", 0.0);
    tp.ry = p.value("ry_deg", 0.0);
    tp.rz = p.value("rz_deg", 0.0);
    tp.copy = p.value("copy", false);
    FeaturePtr f = makeFeature(FeatureType::TransformBody, doc().nextName(tp.copy ? "Copy" : "Move"), Json(tp));
    const Analysis a = commit(AddFeatureCommand(f), f->id);
    return Json{{"feature_id", f->id}, {"status", statusName(a.result(f->id)->status)}, {"bodies", bodiesSummary(a)}};
}

// ------------------------------------------------------------------ v0.4 lot 1

Json IpcCommandDispatcher::createConstructionPlane(const Json& p)
{
    ConstructionPlaneParams cp;
    cp.mode = p.value("mode", std::string("offset"));
    if (cp.mode != "offset" && cp.mode != "angle" && cp.mode != "mid")
        throw IpcError("mode must be offset, angle or mid", -32602);
    if (cp.mode != "mid")
        cp.base = parsePlaneRef(p, "base");
    if (cp.mode == "angle") {
        cp.angle = reqNum(p, "angle_deg");
        const std::string axis = p.value("axis", std::string("X"));
        if (axis == "X" || axis == "Y" || axis == "Z")
            cp.axis = GeomRef{axis, {}};
        else {
            auto snap = KernelEngine::computeSnapshot(doc());
            const MeshEdge* e = nullptr;
            for (const auto& b : snap->bodies)
                for (const auto& me : b.mesh.edges)
                    if (me.name == axis)
                        e = &me;
            if (!e)
                throw IpcError("axis must be X, Y, Z or a straight edge id (query_topology)", -32602);
            cp.axis = GeomRef{"Edge", TopoRef{e->name, e->sig}};
        }
    }
    if (cp.mode == "mid") {
        if (!p.contains("face_ids") || !p["face_ids"].is_array() || p["face_ids"].size() != 2)
            throw IpcError("mid needs face_ids: two parallel planar faces", -32602);
        auto snap = KernelEngine::computeSnapshot(doc());
        TopoRef* dst[2] = {&cp.face1, &cp.face2};
        for (int i = 0; i < 2; ++i) {
            const MeshFace* f = findFace(*snap, p["face_ids"][size_t(i)].get<std::string>());
            if (!f || !f->planar)
                throw IpcError("Unknown or non planar face id", -32602);
            *dst[i] = TopoRef{f->name, f->sig};
        }
    }
    FeaturePtr f = makeFeature(FeatureType::ConstructionPlane, p.value("name", doc().nextName("Plane")), Json(cp));
    commit(AddFeatureCommand(f), f->id);
    auto snap = KernelEngine::computeSnapshot(doc());
    for (const auto& pv : snap->planes)
        if (pv.id == f->id)
            return Json{{"plane_id", f->id},
                        {"name", f->name},
                        {"origin", {pv.plane.origin.x, pv.plane.origin.y, pv.plane.origin.z}},
                        {"normal", {pv.plane.normal.x, pv.plane.normal.y, pv.plane.normal.z}}};
    return Json{{"plane_id", f->id}, {"name", f->name}};
}

Json IpcCommandDispatcher::createRib(const Json& p)
{
    RibParams rp;
    rp.sketch = requireFeature(reqStr(p, "sketch_id"), FeatureType::Sketch)->id;
    rp.curve = p.value("line_geo_id", std::string{});
    rp.thickness = reqNum(p, "thickness_mm");
    rp.body = p.value("body_id", std::string{});
    FeaturePtr f = makeFeature(FeatureType::Rib, doc().nextName("Rib"), Json(rp));
    const Analysis a = commit(AddFeatureCommand(f), f->id);
    return Json{{"feature_id", f->id}, {"status", statusName(a.result(f->id)->status)}, {"bodies", bodiesSummary(a)}};
}

Json IpcCommandDispatcher::alignBodies(const Json& p)
{
    auto snap = KernelEngine::computeSnapshot(doc());
    const std::string mode = p.value("mode", std::string("face"));
    if (mode != "face" && mode != "axis")
        throw IpcError("mode must be 'face' (face to face) or 'axis' (coaxial)", -32602);
    auto ref = [&](const char* key, Uuid* owner) -> TopoRef {
        const std::string id = reqStr(p, key);
        if (const MeshFace* f = findFace(*snap, id, owner))
            return TopoRef{f->name, f->sig};
        for (const auto& b : snap->bodies)
            for (const auto& e : b.mesh.edges)
                if (e.name == id) {
                    *owner = b.id;
                    return TopoRef{e.name, e.sig};
                }
        throw IpcError(std::string("Unknown face / edge id: ") + id, -32602);
    };
    Uuid moving, fixed;
    TransformBodyParams tp;
    const TopoRef m = ref("moving_face_id", &moving), fx = ref("fixed_face_id", &fixed);
    if (moving == fixed)
        throw IpcError("The two references must be on different bodies", -32602);
    tp.bodies = {moving};
    tp.mate = Json{{"mode", mode}, {"moving", m}, {"fixed", fx}, {"offset", p.value("offset_mm", 0.0)}, {"flip", p.value("flip", false)}};
    FeaturePtr f = makeFeature(FeatureType::TransformBody, doc().nextName("Align"), Json(tp));
    const Analysis a = commit(AddFeatureCommand(f), f->id);
    return Json{{"feature_id", f->id}, {"status", statusName(a.result(f->id)->status)}, {"bodies", bodiesSummary(a)}};
}

// ------------------------------------------------------------------ v0.4 lot 2

Json IpcCommandDispatcher::checkPrintability(const Json& p)
{
    PrintabilityOptions o;
    const std::string dir = p.value("build_direction", std::string("+Z"));
    static const std::map<std::string, Vec3> dirs{{"+Z", {0, 0, 1}}, {"-Z", {0, 0, -1}}, {"+Y", {0, 1, 0}},
                                                  {"-Y", {0, -1, 0}}, {"+X", {1, 0, 0}},  {"-X", {-1, 0, 0}}};
    auto it = dirs.find(dir);
    if (it == dirs.end())
        throw IpcError("build_direction must be +Z, -Z, +Y, -Y, +X or -X", -32602);
    o.buildDir = it->second;
    o.minWall = p.value("min_wall_mm", 0.8);
    o.warnAngle = p.value("warn_angle_deg", 45.0);
    o.failAngle = p.value("fail_angle_deg", 60.0);
    std::vector<std::string> only;
    if (p.contains("body_ids"))
        for (const Json& j : p["body_ids"])
            only.push_back(j.get<std::string>());
    auto snap = KernelEngine::computeSnapshot(doc());
    Json bodies = Json::array();
    for (const auto& b : snap->bodies) {
        if (!only.empty() && std::find(only.begin(), only.end(), b.id) == only.end())
            continue;
        if (only.empty() && b.locked())  // reference components are not printed
            continue;
        const auto r = analyzePrintability(b.mesh, o);
        bodies.push_back({{"body_id", b.id},
                          {"name", b.name},
                          {"overhang_45_60_mm2", r.warnArea},
                          {"overhang_over_60_mm2", r.failArea},
                          {"thin_wall_mm2", r.thinArea},
                          {"min_wall_mm", r.minThickness},
                          {"needs_supports", r.failArea > 1e-6},
                          {"printable", r.failArea < 1e-6 && r.thinArea < 1e-6}});
    }
    if (!bodies.empty() && std::none_of(bodies.begin(), bodies.end(), [](const Json& b) { return b["needs_supports"].get<bool>(); }))
        m_model->noteEvent("printability_ok");  // lesson criterion, as in the application
    return Json{{"build_direction", dir}, {"min_wall_mm", o.minWall}, {"bodies", bodies}};
}

Json IpcCommandDispatcher::createThread(const Json& p)
{
    auto snap = KernelEngine::computeSnapshot(doc());
    const std::string id = reqStr(p, "face_id");
    Uuid owner;
    const MeshFace* f = findFace(*snap, id, &owner);
    if (!f)
        throw IpcError("Unknown face id (use query_topology): " + id, -32602);
    ThreadParams tp;
    tp.face = TopoRef{f->name, f->sig};
    tp.size = p.value("size", std::string("M6"));
    if (!metricThread(tp.size))
        throw IpcError("size must be one of M3 M4 M5 M6 M8 M10 M12", -32602);
    tp.fine = p.value("fine_pitch", false);
    tp.clearance = p.value("clearance_mm", 0.15);
    tp.length = p.value("length_mm", 0.0);
    tp.modeled = p.value("modeled", true);
    FeaturePtr fe = makeFeature(FeatureType::Thread, doc().nextName("Thread"), Json(tp));
    const Analysis a = commit(AddFeatureCommand(fe), fe->id);
    return Json{{"feature_id", fe->id}, {"status", statusName(a.result(fe->id)->status)}, {"body", bodySummary(a, owner)}};
}

// ------------------------------------------------------------------ v0.5 lot 1: holes & components

Json IpcCommandDispatcher::createFastenerHole(const Json& p)
{
    FastenerHoleParams hp;
    hp.plane = parsePlaneRef(p, "plane_or_face_id");
    hp.centers.clear();
    if (p.contains("centers")) {
        if (!p["centers"].is_array() || p["centers"].empty())
            throw IpcError("centers must be a non-empty list of [x, y]", -32602);
        for (const Json& c : p["centers"])
            hp.centers.push_back({c.at(0).get<double>(), c.at(1).get<double>()});
    }
    else
        hp.centers.push_back(reqVec2(p, "center", Vec2{0, 0}));
    hp.kind = p.value("kind", std::string("clearance"));
    const auto kinds = fastenerKinds();
    if (std::find(kinds.begin(), kinds.end(), hp.kind) == kinds.end())
        throw IpcError("kind must be clearance, counterbore, countersink, insert or nut", -32602);
    hp.size = p.value("size", std::string("M3"));
    if (!fastenerSpec(hp.size))
        throw IpcError("Unknown size '" + hp.size + "' (M2, M2.5, M3, M4, M5, M6)", -32602);
    hp.fit = p.value("fit", std::string("normal"));
    if (hp.fit != "close" && hp.fit != "normal")
        throw IpcError("fit must be close (+0.2 mm) or normal (+0.4 mm)", -32602);
    hp.depth = p.value("depth_mm", 0.0);
    hp.nutClearance = p.value("nut_clearance_mm", 0.15);
    hp.angle = p.value("angle_deg", 0.0);
    hp.targetBody = p.value("target_body_id", std::string{});
    FeaturePtr f = makeFeature(FeatureType::FastenerHole, doc().nextName("Hole"), Json(hp));
    const Analysis a = commit(AddFeatureCommand(f), f->id);
    const auto dims = fastenerHoleDims(hp);
    Json out{{"feature_id", f->id}, {"status", statusName(a.result(f->id)->status)}, {"bodies", bodiesSummary(a)}};
    if (dims)
        out["hole"] = {{"diameter_mm", dims->holeD}, {"head_diameter_mm", dims->headD},
                       {"head_depth_mm", dims->headDepth}, {"summary", dims->summary}};
    if (const auto* r = a.result(f->id); r && !r->message.empty())
        out["message"] = r->message;
    return out;
}

Json IpcCommandDispatcher::listComponents(const Json& p)
{
    const bool fr = p.value("lang", std::string(m_model->pedagogyFrench() ? "fr" : "en")) != "en";
    Json list = Json::array();
    for (const auto& c : ui::ComponentLibrary::all()) {
        Json holes = Json::array();
        for (const auto& h : c.holes)
            holes.push_back({{"x", h.x}, {"y", h.y}, {"d", h.d}});
        list.push_back({{"id", c.id},
                        {"name", c.name(fr)},
                        {"category", c.category},
                        {"description", c.description(fr)},
                        {"mounting_holes", holes},
                        {"screw", c.screw},
                        {"standoff_mm", c.standoff},
                        {"material", c.material},
                        {"openings", c.raw.value("openings", Json::array())}});
    }
    return Json{{"components", list}};
}

Json IpcCommandDispatcher::insertComponent(const Json& p)
{
    const std::string id = reqStr(p, "component_id");
    std::string err;
    auto f = ui::ComponentLibrary::makeFeature(doc(), id, m_model->pedagogyFrench(), &err);
    if (!f)
        throw IpcError(err, -32602);
    Analysis a = commit(AddFeatureCommand(*f), (*f)->id);
    Json out{{"feature_id", (*f)->id}, {"body_id", (*f)->id}, {"name", (*f)->name},
             {"status", statusName(a.result((*f)->id)->status)}, {"locked", true}};
    if (p.contains("position") || p.contains("rz_deg")) {
        TransformBodyParams tp;
        tp.bodies = {(*f)->id};
        if (p.contains("position")) {
            const Json& pos = p["position"];
            tp.dx = pos.at(0).get<double>();
            tp.dy = pos.at(1).get<double>();
            tp.dz = pos.size() > 2 ? pos.at(2).get<double>() : 0.0;
        }
        tp.rz = p.value("rz_deg", 0.0);
        FeaturePtr mv = makeFeature(FeatureType::TransformBody, doc().nextName("Move"), Json(tp));
        a = commit(AddFeatureCommand(mv), mv->id);
        out["move_feature_id"] = mv->id;
    }
    out["body"] = bodySummary(a, (*f)->id);
    if (const auto* c = ui::ComponentLibrary::find(id)) {
        Json holes = Json::array();
        for (const auto& h : c->holes)
            holes.push_back({{"x", h.x}, {"y", h.y}, {"d", h.d}});
        out["mounting_holes"] = holes;
        out["screw"] = c->screw;
    }
    return out;
}

// ------------------------------------------------------------------ v0.5 lot 4: 3D printing helpers

Json IpcCommandDispatcher::layFlatOnBed(const Json& p)
{
    const std::string faceId = reqStr(p, "face_id");
    auto snap = KernelEngine::computeSnapshot(doc());
    const MeshFace* f = findFace(*snap, faceId);
    if (!f)
        throw IpcError("Unknown face id (use query_topology): " + faceId, -32602);
    if (!f->planar)
        throw IpcError("Lay flat needs a planar face", -32602);
    Uuid owner;
    for (const auto& b : snap->bodies)
        for (const auto& mf : b.mesh.faces)
            if (mf.name == f->name)
                owner = b.id;
    TransformBodyParams tp;
    tp.bodies = {owner};
    tp.mate = Json{{"mode", "bed"}, {"moving", TopoRef{f->name, f->sig}}, {"center", p.value("center", true)}};
    FeaturePtr fe = makeFeature(FeatureType::TransformBody, doc().nextName("LayFlat"), Json(tp));
    const Analysis a = commit(AddFeatureCommand(fe), fe->id);
    Json out{{"feature_id", fe->id}, {"status", statusName(a.result(fe->id)->status)}, {"body", bodySummary(a, owner)}};
    for (const auto& b : a.bodies)
        if (b.id == owner)
            out["bbox"] = {{"min", {b.bbox.min.x, b.bbox.min.y, b.bbox.min.z}}, {"max", {b.bbox.max.x, b.bbox.max.y, b.bbox.max.z}}};
    if (const auto* r = a.result(fe->id); r && !r->message.empty())
        out["message"] = r->message;
    return out;
}

Json IpcCommandDispatcher::createEnclosure(const Json& p)
{
    ui::EnclosureSpec s;
    s.length = reqNum(p, "inner_length_mm");
    s.width = reqNum(p, "inner_width_mm");
    s.height = reqNum(p, "inner_height_mm");
    s.wall = p.value("wall_mm", 2.0);
    s.cornerRadius = p.value("corner_radius_mm", 3.0);
    s.closure = p.value("closure", std::string("lip"));
    s.clearance = p.value("clearance_mm", 0.2);
    std::string err;
    auto res = ui::buildEnclosure(doc(), s, &err);
    if (!res)
        throw IpcError(err, -32602);
    const Analysis a = commit(*res->command, {});
    m_model->noteEvent("enclosure_created");
    Json steps = Json::array();
    for (const auto& f : doc().features)
        if (a.result(f->id))
            steps.push_back({{"name", f->name}, {"type", f->type}, {"status", statusName(a.result(f->id)->status)}});
    return Json{{"base_body_id", res->baseBody}, {"lid_body_id", res->lidBody}, {"feature_count", res->featureCount},
                {"steps", steps}, {"bodies", bodiesSummary(a)}};
}

Json IpcCommandDispatcher::checkBuildVolume(const Json& p)
{
    std::string preset = p.value("preset", std::string("prusa_mk4"));
    double x = 220, y = 220, z = 250;
    if (preset == "bambu")
        x = y = z = 256;
    else if (preset == "prusa_mini")
        x = y = z = 180;
    else if (preset == "custom") {
        x = reqNum(p, "x_mm");
        y = reqNum(p, "y_mm");
        z = reqNum(p, "z_mm");
    }
    else if (preset != "prusa_mk4" && preset != "ender3")
        throw IpcError("preset must be prusa_mk4, ender3, bambu, prusa_mini or custom", -32602);
    auto snap = KernelEngine::computeSnapshot(doc());
    Json bodies = Json::array();
    bool all = true;
    for (const auto& b : snap->bodies) {
        if (b.locked() || !b.mesh.bbox.valid())
            continue;
        const BBox& bb = b.mesh.bbox;
        const double e = 0.05;
        const bool fits = bb.min.x >= -x / 2 - e && bb.max.x <= x / 2 + e && bb.min.y >= -y / 2 - e &&
                          bb.max.y <= y / 2 + e && bb.min.z >= -e && bb.max.z <= z + e;
        all = all && fits;
        bodies.push_back({{"body_id", b.id}, {"name", b.name}, {"fits", fits},
                          {"size_mm", {bb.max.x - bb.min.x, bb.max.y - bb.min.y, bb.max.z - bb.min.z}},
                          {"on_bed", std::abs(bb.min.z) < 0.05}});
    }
    return Json{{"build_volume_mm", {x, y, z}}, {"all_fit", all}, {"bodies", bodies},
                {"note", "The plate is centered on the origin (X, Y) and starts at Z = 0; lay_flat_on_bed puts a face on it."}};
}

// ------------------------------------------------------------------ v0.5 lot 5: lesson recorder

Json IpcCommandDispatcher::recordLesson(const Json& p)
{
    auto snap = KernelEngine::computeSnapshot(doc());
    const std::string id = p.value("lesson_id", std::string("ma_lecon"));
    Json l = lessonFromProject(doc(), *snap, id, p.value("title_fr", std::string("Ma leçon")),
                               p.value("title_en", std::string("My lesson")));
    const auto problems = validateLessonJson(l);
    Json out{{"lesson", l}, {"valid", problems.empty()}, {"problems", problems}};
    if (p.contains("pack_dir")) {
        if (m_model->tutorMode())
            throw IpcError("Tutor mode: writing a lesson pack is a trainer action", -32003);
        const QString err = ui::LessonRecorder::writePack(m_model, l, QString::fromStdString(reqStr(p, "pack_dir")));
        if (!err.isEmpty())
            throw IpcError(err.toStdString());
        out["pack"] = {{"json", id + ".json"}, {"silex", id + ".silex"}};
    }
    return out;
}

// ------------------------------------------------------------------ v0.4 lot 3: AI tutor

Json IpcCommandDispatcher::getPedagogicalContext(const Json& p)
{
    const std::string lang = p.value("lang", std::string(m_model->pedagogyFrench() ? "fr" : "en"));
    const bool fr = lang != "en";
    auto snap = KernelEngine::computeSnapshot(doc());
    // sketches: remaining freedom explained in plain language
    Json sketches = Json::array();
    for (const auto& f : doc().features) {
        if (f->type != FeatureType::Sketch)
            continue;
        Sketch sk = f->as<SketchParams>().sketch;
        if (const SketchView* sv = snap->sketch(f->id))
            sk = sv->sketch;
        Sketch solved = sk;
        const SolveResult r = SketchSolver::solve(solved);
        Json hints = Json::array();
        if (r.state != SketchDofState::FullyConstrained)
            for (const auto& item : explainSketch(sk, fr))
                hints.push_back({{"text", item.text}, {"dof", item.dof}, {"geo_ids", item.geos}});
        sketches.push_back({{"sketch_id", f->id}, {"name", f->name}, {"dof", r.dof}, {"state", dofStateName(r.state)}, {"explanations", hints}});
    }
    // failing steps with their explanation
    ErrorContext ctx;
    for (const auto& b : snap->bodies)
        if (b.mesh.bbox.valid()) {
            const Vec3 e = b.mesh.bbox.max - b.mesh.bbox.min;
            const double lo = std::min({e.x, e.y, e.z});
            ctx.smallestBodyExtent = ctx.smallestBodyExtent > 0 ? std::min(ctx.smallestBodyExtent, lo) : lo;
        }
    Json problems = Json::array();
    for (const auto& r : snap->results)
        if (r.status == FeatureStatus::Error || r.status == FeatureStatus::Warning)
            if (FeaturePtr f = doc().find(r.id))
                problems.push_back({{"feature_id", f->id},
                                    {"name", f->name},
                                    {"type", f->type},
                                    {"status", statusName(r.status)},
                                    {"message", r.message},
                                    {"explanation", explainFeatureError(f->type, r.message, f->params, ctx, fr)}});
    Json recent = Json::array();
    for (const auto& e : m_model->userErrors())
        recent.push_back(e);
    Json events = Json::array();
    for (const auto& e : m_model->events())
        events.push_back(e);
    return Json{
        {"language", fr ? "fr" : "en"},
        {"sketches", sketches},
        {"problems", problems},
        {"recent_user_errors", recent},
        {"session_events", events},
        {"tutorial", m_model->tutorialState().is_null() ? Json() : m_model->tutorialState()},
        {"tutor_mode", m_model->tutorMode()},  // v0.5: modelling commands locked by Silex
        {"tutor_guidelines",
         fr ? "Mode tuteur : guide l'utilisateur pas à pas en posant des questions et en nommant l'outil Silex à utiliser "
              "(raccourci entre parenthèses). Ne crée ni ne modifie jamais la géométrie à sa place : seules les fonctions "
              "de lecture (get_model_tree, inspect_body, query_topology, capture_viewport, get_pedagogical_context) sont "
              "autorisées. Explique le pourquoi (degrés de liberté, intention de conception) et félicite chaque étape validée."
            : "Tutor mode: guide the user step by step by asking questions and naming the Silex tool to use (shortcut in "
              "brackets). Never create or modify geometry for them: only read-only tools (get_model_tree, inspect_body, "
              "query_topology, capture_viewport, get_pedagogical_context) are allowed. Explain the why (degrees of "
              "freedom, design intent) and praise every validated step."}};
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
            if (f->type != FeatureType::Extrude && f->type != FeatureType::Revolve && f->type != FeatureType::Sweep &&
                f->type != FeatureType::Loft)
                throw IpcError("Only extrusions, revolutions, sweeps and lofts can be copied as features (" + f->name +
                                   " is a " + f->type + ")",
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

// ------------------------------------------------------------------ v0.3: sweep & loft

Json IpcCommandDispatcher::sweep(const Json& p)
{
    FeaturePtr prof = requireFeature(reqStr(p, "profile_sketch_id"), FeatureType::Sketch);
    SweepParams sp;
    sp.profile = pickProfile(prof->id, p.value("profile_index", 0), doc());
    if (p.contains("path_sketch_id") && p["path_sketch_id"].is_string()) {
        FeaturePtr path = requireFeature(p["path_sketch_id"].get<std::string>(), FeatureType::Sketch);
        if (path->id == prof->id)
            throw IpcError("The path must be in another sketch than the profile", -32602);
        sp.path.sketch = path->id;
        if (p.contains("path_geo_ids") && p["path_geo_ids"].is_array())
            for (const Json& g : p["path_geo_ids"])
                sp.path.curves.push_back(g.get<std::string>());
    }
    else if (p.contains("path_edge_ids") && p["path_edge_ids"].is_array() && !p["path_edge_ids"].empty()) {
        auto snap = KernelEngine::computeSnapshot(doc());
        for (const Json& e : p["path_edge_ids"]) {
            const MeshEdge* me = findEdge(*snap, e.get<std::string>());
            if (!me)
                throw IpcError("Unknown edge id (use query_topology): " + e.get<std::string>(), -32602);
            sp.path.edges.push_back(TopoRef{me->name, me->sig});
        }
    }
    else {
        throw IpcError("Give path_sketch_id (a sketch with the trajectory) or path_edge_ids (model edges)", -32602);
    }
    const std::string o = p.value("orientation", std::string("corrected_frenet"));
    if (o == "corrected_frenet" || o == "CorrectedFrenet") sp.orientation = "CorrectedFrenet";
    else if (o == "frenet" || o == "Frenet") sp.orientation = "Frenet";
    else if (o == "fixed" || o == "Fixed") sp.orientation = "Fixed";
    else throw IpcError("orientation must be corrected_frenet, frenet or fixed", -32602);
    sp.op = parseOp(p.value("operation", std::string("new_body")));
    FeaturePtr f = makeFeature(FeatureType::Sweep, doc().nextName("Sweep"), Json(sp));
    const Analysis a = commit(AddFeatureCommand(f), f->id);
    const Uuid body = a.bodies.empty() ? Uuid{} : (sp.op == BooleanOp::NewBody ? f->id : lastEditableId(a));
    return Json{{"feature_id", f->id}, {"body_id", body}, {"status", statusName(a.result(f->id)->status)},
                {"body", bodySummary(a, body)}};
}

Json IpcCommandDispatcher::loft(const Json& p)
{
    if (!p.contains("profile_sketch_ids") || !p["profile_sketch_ids"].is_array() || p["profile_sketch_ids"].size() < 2)
        throw IpcError("profile_sketch_ids must list at least 2 sketches (in order)", -32602);
    LoftParams lp;
    for (const Json& id : p["profile_sketch_ids"]) {
        FeaturePtr sk = requireFeature(id.get<std::string>(), FeatureType::Sketch);
        lp.sections.push_back(pickProfile(sk->id, 0, doc()));
    }
    lp.ruled = p.value("ruled", false);
    lp.solid = p.value("solid", true);
    lp.op = parseOp(p.value("operation", std::string("new_body")));
    FeaturePtr f = makeFeature(FeatureType::Loft, doc().nextName("Loft"), Json(lp));
    const Analysis a = commit(AddFeatureCommand(f), f->id);
    const Uuid body = a.bodies.empty() ? Uuid{} : (lp.op == BooleanOp::NewBody ? f->id : lastEditableId(a));
    return Json{{"feature_id", f->id}, {"body_id", body}, {"status", statusName(a.result(f->id)->status)},
                {"body", bodySummary(a, body)}};
}

// ------------------------------------------------------------------ v0.3: global parameters

namespace {
Json parametersJson(const std::vector<Parameter>& ps)
{
    Json out = Json::array();
    for (const auto& p : ps) {
        Json j{{"name", p.name}, {"expression", p.expression}, {"value", p.value}, {"unit", p.unit}, {"comment", p.comment}};
        if (!p.error.empty())
            j["error"] = p.error;
        out.push_back(std::move(j));
    }
    return out;
}
}  // namespace

Json IpcCommandDispatcher::getParameters(const Json&)
{
    // where each parameter is used
    Json usage = Json::object();
    for (const auto& f : doc().features) {
        auto note = [&](const std::string& expr, const std::string& field) {
            try {
                for (const auto& n : expressionIdentifiers(expr))
                    usage[n].push_back(Json{{"feature_id", f->id}, {"feature", f->name}, {"field", field}, {"expression", expr}});
            }
            catch (...) {
            }
        };
        if (f->params.contains("expr") && f->params["expr"].is_object())
            for (const auto& [ptr, e] : f->params["expr"].items())
                if (e.is_string())
                    note(e.get<std::string>(), ptr);
        if (f->type == FeatureType::Sketch)
            for (const auto& c : f->params.value("sketch", Json::object()).value("constraints", Json::array()))
                if (c.contains("expr"))
                    note(c["expr"].get<std::string>(), "constraint:" + c.value("id", std::string{}));
    }
    return Json{{"parameters", parametersJson(doc().parameters)}, {"usage", usage}};
}

Json IpcCommandDispatcher::setParameter(const Json& p)
{
    const std::string name = reqStr(p, "name");
    if (!isValidParameterName(name))
        throw IpcError("Invalid parameter name '" + name + "' (letters, digits, _; not a function or unit name)", -32602);
    std::vector<Parameter> ps = doc().parameters;
    auto it = std::find_if(ps.begin(), ps.end(), [&](const Parameter& x) { return x.name == name; });
    if (it == ps.end()) {
        ps.push_back(Parameter{});
        it = ps.end() - 1;
        it->name = name;
    }
    if (p.contains("expression")) {
        if (p["expression"].is_number())
            it->expression = Json(p["expression"].get<double>()).dump();
        else
            it->expression = reqStr(p, "expression");
    }
    else if (!p.contains("comment") && !p.contains("unit")) {
        throw IpcError("Missing parameter 'expression'", -32602);
    }
    if (p.contains("comment"))
        it->comment = p.value("comment", std::string{});
    if (p.contains("unit")) {
        const std::string u = p.value("unit", std::string("mm"));
        if (u != "mm" && u != "deg" && !u.empty())
            throw IpcError("unit must be mm, deg or empty", -32602);
        it->unit = u;
    }
    evaluateParameters(ps);
    for (const auto& x : ps)
        if (!x.error.empty())
            throw IpcError("Parameter '" + x.name + "': " + x.error + " (nothing was changed)", -32602);
    const Analysis a = commit(SetParametersCommand(ps), {});
    Json bodies = Json::array();
    for (const auto& b : a.bodies)
        bodies.push_back({{"body_id", b.id}, {"volume_mm3", b.volume}, {"valid", b.valid}});
    Json errors = Json::array();
    for (const auto& r : a.results)
        if (r.status == FeatureStatus::Error)
            errors.push_back({{"feature_id", r.id}, {"message", r.message}});
    return Json{{"parameters", parametersJson(doc().parameters)}, {"bodies", bodies}, {"feature_errors", errors}};
}

Json IpcCommandDispatcher::deleteParameter(const Json& p)
{
    const std::string name = reqStr(p, "name");
    std::vector<Parameter> ps = doc().parameters;
    const auto n = std::erase_if(ps, [&](const Parameter& x) { return x.name == name; });
    if (n == 0)
        throw IpcError("Unknown parameter '" + name + "'", -32602);
    commit(SetParametersCommand(ps), {});
    return Json{{"parameters", parametersJson(doc().parameters)}};
}

// ------------------------------------------------------------------ v0.3: analysis

Json IpcCommandDispatcher::measure(const Json& p)
{
    if (!p.contains("entity_ids") || !p["entity_ids"].is_array() || p["entity_ids"].empty() || p["entity_ids"].size() > 2)
        throw IpcError("entity_ids must list 1 or 2 ids (face / edge ids from query_topology, or body ids)", -32602);
    auto snap = KernelEngine::computeSnapshot(doc());
    std::vector<MeasureRef> refs;
    for (const Json& j : p["entity_ids"]) {
        const std::string id = j.get<std::string>();
        MeasureRef r;
        if (std::any_of(snap->bodies.begin(), snap->bodies.end(), [&](const BodyView& b) { return b.id == id; })) {
            r.kind = MeasureRef::Kind::Body;
            r.body = id;
        }
        else if (const MeshFace* f = findFace(*snap, id, &r.body)) {
            r.kind = MeasureRef::Kind::Face;
            r.ref = TopoRef{f->name, f->sig};
        }
        else if (const MeshEdge* e = findEdge(*snap, id, &r.body)) {
            r.kind = MeasureRef::Kind::Edge;
            r.ref = TopoRef{e->name, e->sig};
        }
        else {
            throw IpcError("Unknown entity id (use query_topology or a body id): " + id, -32602);
        }
        refs.push_back(r);
    }
    const MeasureResult m = KernelEngine::measure(doc(), refs);
    if (!m.ok)
        throw IpcError(m.error);
    Json out{{"types", m.types}};
    if (m.hasDistance)
        out["distance"] = {{"min_distance_mm", m.distance}, {"point_1", vec(m.p1)}, {"point_2", vec(m.p2)},
                           {"dx_mm", std::abs(m.delta.x)}, {"dy_mm", std::abs(m.delta.y)}, {"dz_mm", std::abs(m.delta.z)}};
    if (m.hasAngle)
        out["angle_deg"] = m.angleDeg;
    Json circles = Json::array();
    for (const auto& c : m.circles)
        circles.push_back({{"diameter_mm", 2 * c.radius}, {"radius_mm", c.radius}, {"center", vec(c.center)}, {"axis", vec(c.axis)}});
    if (!circles.empty())
        out["circles"] = circles;
    if (m.length > 0)
        out["length_mm"] = m.length;
    if (m.area > 0)
        out["area_mm2"] = m.area;
    return out;
}

Json IpcCommandDispatcher::checkInterference(const Json& p)
{
    std::vector<Uuid> ids;
    if (p.contains("body_ids") && p["body_ids"].is_array())
        for (const Json& j : p["body_ids"])
            ids.push_back(j.get<std::string>());
    const InterferenceReport r = KernelEngine::checkInterference(doc(), ids, false);
    if (!r.error.empty())
        throw IpcError(r.error, -32602);
    Json pairs = Json::array();
    for (const auto& pr : r.pairs)
        pairs.push_back({{"body_a", pr.a},
                         {"body_b", pr.b},
                         {"name_a", pr.nameA},
                         {"name_b", pr.nameB},
                         {"status", pr.status},
                         {"overlap_volume_mm3", pr.overlapVolume},
                         {"clearance_mm", pr.clearance},
                         {"closest_points", Json::array({vec(pr.p1), vec(pr.p2)})}});
    Json out{{"collision", r.collision}, {"pairs", pairs},
             {"thresholds", {{"collision_volume_mm3", kCollisionVolume}, {"tight_clearance_mm", kTightClearance}}}};
    out["min_clearance_mm"] = r.collision && std::all_of(r.pairs.begin(), r.pairs.end(), [](const InterferencePair& x) {
                                  return x.status == "collision";
                              })
                                  ? Json(nullptr)
                                  : Json(r.minClearance);
    return out;
}

Json IpcCommandDispatcher::setBodyMaterial(const Json& p)
{
    const std::string id = reqStr(p, "body_id");
    const Analysis before = KernelEngine::analyze(doc());
    if (std::none_of(before.bodies.begin(), before.bodies.end(), [&](const BodyMetrics& b) { return b.id == id; }))
        throw IpcError("Unknown body id: " + id, -32602);
    BodyMaterial m = doc().materialOf(id);
    if (p.contains("preset")) {
        const BodyMaterial* pr = findMaterialPreset(reqStr(p, "preset"));
        if (!pr) {
            std::string names;
            for (const auto& x : materialPresets())
                names += (names.empty() ? "" : ", ") + x.name;
            throw IpcError("Unknown material preset (" + names + ")", -32602);
        }
        m = *pr;
    }
    if (p.contains("color")) {
        BodyMaterial tmp;
        from_json(Json{{"color", reqStr(p, "color")}}, tmp);
        m.r = tmp.r;
        m.g = tmp.g;
        m.b = tmp.b;
        if (reqStr(p, "color").size() == 9)
            m.a = tmp.a;
        if (!p.contains("preset"))
            m.name = p.value("name", std::string("Custom"));
    }
    if (p.contains("opacity"))
        m.a = float(std::clamp(reqNum(p, "opacity"), 0.05, 1.0));
    if (p.contains("density_g_cm3")) {
        m.density = reqNum(p, "density_g_cm3");
        if (m.density <= 0)
            throw IpcError("density_g_cm3 must be > 0", -32602);
    }
    if (p.contains("name"))
        m.name = reqStr(p, "name");
    m_model->execute(SetBodyMaterialCommand(id, m));
    double volume = 0;
    for (const auto& b : before.bodies)
        if (b.id == id)
            volume = b.volume;
    return Json{{"body_id", id}, {"material", m}, {"volume_mm3", volume}, {"mass_g", massGrams(volume, m.density)}};
}

Json IpcCommandDispatcher::export3mf(const Json& p)
{
    ThreeMfOptions o;
    const std::string path = reqStr(p, "file_path");
    if (p.contains("body_ids") && p["body_ids"].is_array())
        for (const Json& j : p["body_ids"])
            o.bodies.push_back(j.get<std::string>());
    o.linearDeflection = p.value("linear_deflection", 0.05);
    if (o.linearDeflection <= 0)
        throw IpcError("linear_deflection must be > 0", -32602);
    o.title = p.value("title", m_model->filePath().isEmpty() ? std::string("Silex design")
                                                             : QFileInfo(m_model->filePath()).completeBaseName().toStdString());
    o.designer = p.value("designer", std::string{});
    const ExportSummary s = KernelEngine::export3mf(doc(), path, o);
    if (!s.error.empty())
        throw IpcError(s.error);
    m_model->noteEvent("3mf_exported");
    return Json{{"file_path", path}, {"bytes", s.bytes}, {"objects", s.objects}, {"triangles", s.triangles}, {"unit", "millimeter"}};
}

Json IpcCommandDispatcher::exportDrawing(const Json& p)
{
    const QString path = QString::fromStdString(reqStr(p, "file_path"));
    std::string format = p.value("format", std::string{});
    if (format.empty())
        format = path.endsWith(".svg", Qt::CaseInsensitive) ? "svg" : "pdf";
    if (format != "pdf" && format != "svg")
        throw IpcError("format must be 'pdf' or 'svg'", -32602);
    const std::string layout = p.value("view_layout", std::string("3_views"));
    if (layout != "3_views")
        throw IpcError("view_layout must be '3_views' (front XZ, top XY, right YZ, first-angle projection)", -32602);
    std::vector<Uuid> bodies;
    if (p.contains("body_ids") && p["body_ids"].is_array())
        for (const Json& j : p["body_ids"])
            bodies.push_back(j.get<std::string>());

    DrawingProjection proj = KernelEngine::projectDrawing(doc(), bodies);
    if (!proj.error.empty())
        throw IpcError(proj.error);
    ui::DrawingSettings st = ui::DrawingSettings::fromJson(doc().drawing);
    if (!doc().drawing.is_object()) {
        st.title = m_model->filePath().isEmpty() ? "Silex" : QFileInfo(m_model->filePath()).completeBaseName().toStdString();
        const Analysis a = KernelEngine::analyze(doc());
        if (!a.bodies.empty())
            st.material = doc().materialOf(a.bodies.front().id).name;
    }
    if (p.contains("title")) st.title = reqStr(p, "title");
    if (p.contains("designer")) st.designer = reqStr(p, "designer");
    if (p.contains("sheet")) {
        st.sheet = reqStr(p, "sheet");
        if (st.sheet != "A4" && st.sheet != "A3")
            throw IpcError("sheet must be A4 or A3", -32602);
    }
    if (p.contains("scale")) st.scale = reqStr(p, "scale");
    ui::DrawingSheet sheet(proj, st);
    sheet.resnapDimensions();
    QString err;
    const bool ok = format == "pdf" ? sheet.exportPdf(path, &err) : sheet.exportSvg(path, &err);
    if (!ok)
        throw IpcError("Cannot write the drawing: " + (err.isEmpty() ? path : err).toStdString());
    m_model->noteEvent("drawing_exported");
    Json views = Json::array();
    for (const auto& v : proj.views) {
        int visible = 0, hidden = 0, axes = 0;
        for (const auto& c : v.curves) {
            visible += c.kind == DrawLineKind::Visible;
            hidden += c.kind == DrawLineKind::Hidden;
            axes += c.kind == DrawLineKind::Axis;
        }
        views.push_back({{"name", v.name}, {"label", v.label}, {"visible_edges", visible}, {"hidden_edges", hidden},
                         {"center_lines", axes}, {"circles", v.circles.size()}, {"width_mm", v.width()}, {"height_mm", v.height()}});
    }
    return Json{{"file_path", path.toStdString()}, {"format", format}, {"bytes", QFileInfo(path).size()},
                {"sheet", st.sheet}, {"scale", sheet.scaleText().toStdString()}, {"views", views},
                {"dimensions", st.dims.size()}, {"volume_mm3", proj.volume}, {"mass_g", proj.mass}};
}

// ------------------------------------------------------------------ D. editing, history, I/O

Json IpcCommandDispatcher::updateFeatureParameter(const Json& p)
{
    FeaturePtr f = requireFeature(reqStr(p, "feature_id"));
    const std::string name = reqStr(p, "param_name");
    // v0.3: "expression" binds the field to global parameters (e.g. "epaisseur_paroi * 2")
    std::string expression = p.value("expression", std::string{});
    double v = 0;
    if (!expression.empty()) {
        try {
            v = evaluateExpression(expression, lookupIn(parameterValues(doc().parameters)));
        }
        catch (const ExpressionError& e) {
            throw IpcError(std::string("Invalid expression: ") + e.what(), -32602);
        }
        if (isPlainNumber(expression))
            expression.clear();
    }
    else {
        v = reqNum(p, "new_value");
    }
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
        it->expr = expression;
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
    if (f->type != FeatureType::Sketch) {
        // the JSON pointer of the field that was just written
        static const std::map<std::string, std::string> pointers{
            {"distance", "/distance"},   {"distance_mm", "/distance"}, {"angle", "/angle"},
            {"angle_deg", "/angle"},     {"value", "/value"},          {"radius", "/value"},
            {"radius_or_distance_mm", "/value"},                       {"thickness", "/thickness"},
            {"thickness_mm", "/thickness"}, {"count", "/count"},       {"total_angle_deg", "/totalAngle"},
            {"totalAngle", "/totalAngle"}, {"count_1", "/dir1/count"}, {"spacing_1_mm", "/dir1/spacing"},
            {"count_2", "/dir2/count"},  {"spacing_2_mm", "/dir2/spacing"}};
        std::string ptr = "/" + name;
        if (auto it = pointers.find(name); it != pointers.end())
            ptr = it->second;
        if (f->type == FeatureType::Fillet || f->type == FeatureType::Chamfer)
            ptr = "/value";
        setFieldExpression(n->params, ptr, expression);
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
        throw IpcError("Cannot save the design: " + err.toStdString(), -32000, "IO_ERROR",
                       "Check that the folder exists and is writable, and that the file is not open elsewhere.");
    return Json{{"file_path", path.toStdString()}, {"bytes", QFileInfo(path).size()},
                {"feature_count", doc().features.size()}};
}

Json IpcCommandDispatcher::shutdown(const Json&)
{
    m_quit = true;
    QTimer::singleShot(50, qApp, &QCoreApplication::quit);
    return Json{{"ok", true}};
}

// ------------------------------------------------------------------ document cycle (sprint "Fiabilité")

void IpcCommandDispatcher::requireDiscardable(const Json& p, const char* what) const
{
    if (m_model->isDirty() && !p.value("discard_changes", false))
        throw IpcError(std::string("The current document has unsaved changes: ") + what + " would lose them", -32000,
                       "DOCUMENT_DIRTY",
                       "Save the work first with save_design(file_path), or pass discard_changes=true if the "
                       "changes may be dropped (ask the user when they are not yours).");
}

Json IpcCommandDispatcher::newDocument(const Json& p)
{
    requireDiscardable(p, "new_document");
    m_model->newDocument();
    return getActiveDocumentInfo(Json::object());
}

Json IpcCommandDispatcher::openDocument(const Json& p)
{
    const QString path = QString::fromStdString(reqStr(p, "path"));
    const QFileInfo fi(path);
    if (!fi.isFile())
        throw IpcError("File not found: " + path.toStdString(), -32602, "FILE_NOT_FOUND",
                       "Give an absolute path to an existing .silex file (Silex resolves relative paths from its "
                       "own working directory, not from yours).");
    requireDiscardable(p, "open_document");
    QString err;
    if (!m_model->load(fi.absoluteFilePath(), &err))
        throw IpcError("Cannot open " + path.toStdString() + ": " + err.toStdString(), -32000, "INVALID_DOCUMENT",
                       "Only native .silex designs can be opened; import STEP files with import_step.");
    return getActiveDocumentInfo(Json::object());
}

Json IpcCommandDispatcher::getActiveDocumentInfo(const Json&)
{
    const DocState& d = doc();
    const Analysis a = KernelEngine::analyze(d);
    int sketches = 0, errors = 0, warnings = 0;
    for (const auto& f : d.features)
        sketches += f->type == FeatureType::Sketch;
    for (const auto& r : a.results) {
        errors += r.status == FeatureStatus::Error;
        warnings += r.status == FeatureStatus::Warning;
    }
    const QString path = m_model->filePath();
    const History& h = m_model->history();
    return Json{{"path", path.isEmpty() ? Json() : Json(path.toStdString())},
                {"name", path.isEmpty() ? std::string("Untitled") : QFileInfo(path).completeBaseName().toStdString()},
                {"schema_version", kSchemaVersion},
                {"modified", m_model->isDirty()},
                {"feature_count", d.features.size()},
                {"active_feature_count", d.marker},
                {"sketch_count", sketches},
                {"body_count", a.bodies.size()},
                {"parameter_count", d.parameters.size()},
                {"errors", errors},
                {"warnings", warnings},
                {"can_undo", h.canUndo()},
                {"can_redo", h.canRedo()},
                {"undo_label", h.undoLabel()},
                {"command_group", {{"open", h.groupOpen()}, {"name", h.groupLabel()}, {"commands", h.groupSize()}}}};
}

Json IpcCommandDispatcher::beginCommandGroup(const Json& p)
{
    const std::string name = p.value("name", std::string("AI operation"));
    if (!m_model->beginCommandGroup(QString::fromStdString(name)))
        throw IpcError("A command group is already open ('" + m_model->history().groupLabel() + "')", -32000,
                       "GROUP_ALREADY_OPEN", "Call end_command_group before opening another group (groups do not nest).");
    return Json{{"open", true}, {"name", name}};
}

Json IpcCommandDispatcher::endCommandGroup(const Json&)
{
    if (!m_model->history().groupOpen())
        throw IpcError("No command group is open", -32000, "NO_GROUP_OPEN",
                       "Call begin_command_group(name) first. A group is also closed by redo, new_document and "
                       "open_document.");
    const std::string name = m_model->history().groupLabel();
    const int n = m_model->endCommandGroup();
    emit m_model->documentChanged();  // the undo button / label of the UI
    return Json{{"name", name},
                {"commands", n},
                {"undo_steps", n > 0 ? 1 : 0},
                {"undo_label", m_model->history().undoLabel()},
                {"can_undo", m_model->history().canUndo()}};
}

Json IpcCommandDispatcher::verifyDimensions(const Json& p)
{
    const Analysis a = KernelEngine::analyze(doc());
    std::string id = p.value("body_id", std::string{});
    if (id.empty()) {
        if (a.bodies.size() != 1)
            throw IpcError("body_id is required when the model has " + std::to_string(a.bodies.size()) + " bodies",
                           -32602, "MISSING_PARAMETER", "Call get_model_tree to list the bodies and their ids.");
        id = a.bodies.front().id;
    }
    const BodyMetrics* body = nullptr;
    for (const auto& b : a.bodies)
        if (b.id == id)
            body = &b;
    if (!body)
        throw IpcError("Unknown body id: " + id, -32602);

    double expected[3];
    const Json e = p.value("expected_bbox_mm", Json());
    if (e.is_array() && e.size() == 3 && e[0].is_number() && e[1].is_number() && e[2].is_number())
        for (int i = 0; i < 3; ++i)
            expected[i] = e[size_t(i)].get<double>();
    else if (e.is_object() && e.contains("x") && e.contains("y") && e.contains("z")) {
        expected[0] = e["x"].get<double>();
        expected[1] = e["y"].get<double>();
        expected[2] = e["z"].get<double>();
    }
    else
        throw IpcError("expected_bbox_mm must be [x, y, z] (or {\"x\", \"y\", \"z\"}) in millimetres", -32602,
                       "INVALID_PARAMS", "Example: verify_dimensions(body_id, [80, 50, 30], 0.05).");
    const double tol = p.value("tolerance_mm", 0.05);
    if (!(tol >= 0))
        throw IpcError("tolerance_mm must be >= 0", -32602);

    const Vec3 size = body->bbox.max - body->bbox.min;
    const double measured[3] = {size.x, size.y, size.z};
    const char* names[3] = {"x", "y", "z"};
    Json axes = Json::array();
    bool ok = true;
    double worst = 0;
    for (int i = 0; i < 3; ++i) {
        const double dev = measured[i] - expected[i];
        const bool axisOk = std::abs(dev) <= tol + 1e-9;
        ok = ok && axisOk;
        worst = std::max(worst, std::abs(dev));
        axes.push_back({{"axis", names[i]}, {"measured_mm", measured[i]}, {"expected_mm", expected[i]},
                        {"deviation_mm", dev}, {"ok", axisOk}});
    }
    return Json{{"body_id", body->id},
                {"ok", ok && body->valid},
                {"valid_solid", body->valid},
                {"measured_mm", {measured[0], measured[1], measured[2]}},
                {"expected_mm", {expected[0], expected[1], expected[2]}},
                {"tolerance_mm", tol},
                {"max_deviation_mm", worst},
                {"bbox", {{"min", vec(body->bbox.min)}, {"max", vec(body->bbox.max)}}},
                {"axes", axes}};
}
// ------------------------------------------------------------------ v0.7 lot A: parametric text

Json IpcCommandDispatcher::createEmbossText(const Json& p)
{
    EmbossTextParams tp;
    tp.plane = parsePlaneRef(p, "face_id");
    tp.text = reqStr(p, "text");
    if (tp.text.empty())
        throw IpcError("text must not be empty", -32602);
    tp.font = p.value("font", std::string("Liberation Sans"));
    tp.align = p.value("align", std::string("center"));
    if (tp.align != "left" && tp.align != "center" && tp.align != "right")
        throw IpcError("align must be left, center or right", -32602);
    const Vec2 off = reqVec2(p, "offset_xy", Vec2{0, 0});
    tp.x = off.x;
    tp.y = off.y;
    tp.targetBody = p.value("target_body_id", std::string{});
    // numbers, or expressions over the global parameters ("hauteur_texte", "largeur / 10")
    std::map<std::string, std::string> exprs;
    const auto values = parameterValues(doc().parameters);
    auto number = [&](const char* key, const char* ptr, double def, double& out) {
        if (!p.contains(key)) {
            out = def;
            return;
        }
        if (p[key].is_number()) {
            out = p[key].get<double>();
            return;
        }
        if (!p[key].is_string())
            throw IpcError(std::string(key) + " must be a number or an expression", -32602);
        const std::string e = p[key].get<std::string>();
        try {
            out = evaluateExpression(e, lookupIn(values));
        }
        catch (const std::exception& ex) {
            throw IpcError(std::string(key) + ": " + ex.what(), -32602, "INVALID_PARAMS",
                           "Use get_parameters to list the parameter names.");
        }
        exprs[ptr] = e;
    };
    number("height_mm", "/height", 6.0, tp.height);
    number("depth_mm", "/depth", 1.0, tp.depth);
    number("angle_deg", "/angle", 0.0, tp.angle);
    if (tp.height <= 0)
        throw IpcError("height_mm must be positive", -32602);
    if (std::abs(tp.depth) < 1e-6)
        throw IpcError("depth_mm must not be zero (> 0 relief, < 0 engraving)", -32602);
    Json params = Json(tp);
    for (const auto& [ptr, e] : exprs)
        setFieldExpression(params, ptr, e);
    FeaturePtr f = makeFeature(FeatureType::EmbossText, doc().nextName("Text"), params);
    const Analysis a = commit(AddFeatureCommand(f), f->id);
    const FeaturePtr done = doc().find(f->id);
    const auto metrics = KernelEngine::measureText(done ? done->params.value("resolved", tp.text) : tp.text, tp.font,
                                                   tp.height);
    Json out{{"feature_id", f->id},
             {"status", statusName(a.result(f->id)->status)},
             {"resolved_text", done ? done->params.value("resolved", std::string{}) : std::string{}},
             {"font_file", metrics.fontFile},
             {"letters_area_mm2", metrics.area},
             {"stroke_width_mm", metrics.strokeWidth},
             {"bodies", bodiesSummary(a)}};
    if (const auto* r = a.result(f->id); r && !r->message.empty())
        out["message"] = r->message;
    return out;
}
// ------------------------------------------------------------------ v0.7 lot B: SVG / DXF import

Json IpcCommandDispatcher::sketchImportVector(const Json& p)
{
    FeaturePtr f = requireFeature(reqStr(p, "sketch_id"), FeatureType::Sketch);
    const std::string path = reqStr(p, "file_path");
    if (!QFileInfo::exists(QString::fromStdString(path)))
        throw IpcError("File not found: " + path, -32602, "FILE_NOT_FOUND", "Give an absolute path to a .svg or .dxf file.");
    vectorimport::ImportOptions o;
    o.scale = p.value("scale", 1.0);
    o.targetWidth = p.value("target_width_mm", 0.0);
    o.offset = reqVec2(p, "offset_xy", Vec2{0, 0});
    o.angleDeg = p.value("angle_deg", 0.0);
    o.rigid = p.value("rigid", false);
    o.construction = p.value("construction", false);
    if (!(o.scale > 0))
        throw IpcError("scale must be positive", -32602);
    SketchParams sp = f->as<SketchParams>();
    const auto r = vectorimport::importFile(sp.sketch, std::filesystem::path(QString::fromStdString(path).toStdWString()), o);
    if (!r)
        throw IpcError("Import failed: " + r.error, -32000, "IMPORT_FAILED",
                       "Only ASCII DXF (LINE, CIRCLE, ARC, LWPOLYLINE, POLYLINE, SPLINE, ELLIPSE) and SVG shapes "
                       "(path, rect, circle, ellipse, line, polyline, polygon) are read; convert text to paths.");
    if (o.rigid)
        for (auto& c : sp.sketch.constraints)
            if (std::find(r.constraints.begin(), r.constraints.end(), c.id) != r.constraints.end() &&
                c.kind != ConstraintKind::Coincident)
                c.group = "block:" + r.anchor;
    const SolveResult res = SketchSolver::solve(sp.sketch);
    auto n = std::make_shared<FeatureNode>(*f);
    n->params = Json(sp);
    commit(UpdateFeatureCommand(n), n->id);
    const auto snap = KernelEngine::computeSnapshot(doc());
    return Json{{"sketch_id", f->id},
                {"geo_ids", r.geos},
                {"lines", r.lines},
                {"arcs", r.arcs},
                {"circles", r.circles},
                {"merged_endpoints", r.merged},
                {"anchor_point", r.anchor.empty() ? Json() : Json(r.anchor + ":start")},
                {"bbox_mm", {{"min", {r.bboxMin.x, r.bboxMin.y}}, {"max", {r.bboxMax.x, r.bboxMax.y}}}},
                {"closed_profiles", sketchRegions(*snap, f->id).size()},
                {"warnings", r.warnings},
                {"dof", res.dof},
                {"state", dofStateName(res.state)}};
}

// ------------------------------------------------------------------ v0.7 lot C: calibrated image

Json IpcCommandDispatcher::sketchSetCanvas(const Json& p)
{
    FeaturePtr f = requireFeature(reqStr(p, "sketch_id"), FeatureType::Sketch);
    SketchParams sp = f->as<SketchParams>();
    if (p.value("remove", false))
        sp.canvas.reset();
    else {
        if (p.contains("image_path")) {
            const QString path = QString::fromStdString(reqStr(p, "image_path"));
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                throw IpcError("Cannot read " + path.toStdString(), -32602, "FILE_NOT_FOUND",
                               "Give an absolute path to a .png or .jpg image.");
            const QByteArray bytes = file.readAll();
            const QImage img = QImage::fromData(bytes);
            if (img.isNull())
                throw IpcError(path.toStdString() + " is not a readable image", -32602, "INVALID_PARAMS");
            SketchCanvas c = makeCanvas(img.width(), img.height(), p.value("width_mm", 100.0),
                                        reqVec2(p, "center_xy", Vec2{0, 0}));
            c.imagePath = path.toStdString();
            c.imageData = bytes.toBase64().toStdString();
            c.mime = path.endsWith(".jpg", Qt::CaseInsensitive) || path.endsWith(".jpeg", Qt::CaseInsensitive)
                         ? "image/jpeg"
                         : "image/png";
            if (sp.canvas)
                c.opacity = sp.canvas->opacity;
            sp.canvas = c;
        }
        if (!sp.canvas)
            throw IpcError("The sketch has no image yet", -32602, "INVALID_PARAMS", "Pass image_path first.");
        SketchCanvas& c = *sp.canvas;
        if (p.contains("width_mm") && !p.contains("image_path")) {
            const double w = p["width_mm"].get<double>();
            if (!(w > 0))
                throw IpcError("width_mm must be positive", -32602);
            c.heightMm *= w / c.widthMm;
            c.widthMm = w;
        }
        if (p.contains("center_xy") && !p.contains("image_path"))
            c.center = reqVec2(p, "center_xy");
        c.opacity = std::clamp(p.value("opacity", c.opacity), 0.05, 1.0);
        c.angleDeg = p.value("angle_deg", c.angleDeg);
        c.flipH = p.value("flip_h", c.flipH);
        c.flipV = p.value("flip_v", c.flipV);
        if (p.contains("calibrate")) {
            const Json& k = p["calibrate"];
            std::string err;
            if (!k.is_object() || !calibrateCanvas(c, reqVec2(k, "p1_px"), reqVec2(k, "p2_px"), k.value("distance_mm", 0.0), &err))
                throw IpcError("Calibration failed: " + (err.empty() ? std::string("calibrate needs p1_px, p2_px and distance_mm") : err),
                               -32602, "INVALID_PARAMS",
                               "calibrate = {\"p1_px\": [x, y], \"p2_px\": [x, y], \"distance_mm\": 50} (pixels from the "
                               "top-left corner of the image).");
        }
    }
    auto n = std::make_shared<FeatureNode>(*f);
    n->params = Json(sp);
    commit(UpdateFeatureCommand(n), n->id);
    if (!sp.canvas)
        return Json{{"sketch_id", f->id}, {"canvas", nullptr}};
    const SketchCanvas& c = *sp.canvas;
    return Json{{"sketch_id", f->id},
                {"canvas",
                 {{"image_px", {c.pxWidth, c.pxHeight}},
                  {"width_mm", c.widthMm},
                  {"height_mm", c.heightMm},
                  {"mm_per_pixel", c.pxWidth ? c.widthMm / c.pxWidth : 0.0},
                  {"center_xy", {c.center.x, c.center.y}},
                  {"angle_deg", c.angleDeg},
                  {"opacity", c.opacity},
                  {"embedded_bytes", c.imageData.size()}}}};
}
}  // namespace silex::ipc
