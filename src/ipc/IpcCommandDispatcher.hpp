#pragma once
// Executes IPC/MCP tool calls against the document. Runs on the Qt main thread; every
// mutation goes through the same immutable Command + History path as the UI (so undo/redo,
// the timeline and the 3D view stay consistent when the GUI is visible).

#include <QObject>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "kernel/KernelEngine.h"
#include "model/Document.h"
#include "model/Json.h"

namespace silex::ui {
class ModelController;
class Viewport;
}  // namespace silex::ui

namespace silex::ipc {

/// Every IPC failure is reported as a structured JSON-RPC error:
///   {"code": -32602, "error_code": "UNKNOWN_ID", "message": "...", "hint": "..."}
/// `code` is the numeric JSON-RPC code, `error_code` a stable symbolic name agents can test, and
/// `hint` says what to do next. When `errorCode` / `hint` are empty they are derived from the
/// numeric code and the message (see IpcCommandDispatcher::errorJson).
class IpcError : public std::runtime_error
{
public:
    explicit IpcError(const std::string& msg, int code = -32000, std::string errorCode = {}, std::string hint = {})
        : std::runtime_error(msg), m_code(code), m_errorCode(std::move(errorCode)), m_hint(std::move(hint))
    {
    }
    int code() const { return m_code; }
    const std::string& errorCode() const { return m_errorCode; }
    const std::string& hint() const { return m_hint; }

private:
    int m_code;
    std::string m_errorCode;
    std::string m_hint;
};

/// Version of the IPC protocol, reported by `ping` (the MCP bridge checks it with --check).
/// 2: structured errors, document cycle, command groups, verify_dimensions.
inline constexpr int kIpcProtocolVersion = 2;

class IpcCommandDispatcher : public QObject
{
    Q_OBJECT
public:
    explicit IpcCommandDispatcher(ui::ModelController* model, QObject* parent = nullptr);
    ~IpcCommandDispatcher() override;

    /// Parses one JSON-RPC line and returns the serialized response (no trailing newline).
    std::string handleLine(const std::string& line);
    /// Executes a method; throws IpcError.
    Json dispatch(const std::string& method, const Json& params);
    bool quitRequested() const { return m_quit; }
    static std::vector<std::string> methods();
    /// Structured error object of a JSON-RPC response.
    static Json errorJson(const IpcError& e);

private:
    using Handler = Json (IpcCommandDispatcher::*)(const Json&);
    static const std::map<std::string, Handler>& table();

    // A. inspection & topology
    Json getModelTree(const Json& p);
    Json inspectBody(const Json& p);
    Json listSolids(const Json& p);
    Json queryTopology(const Json& p);
    Json captureViewport(const Json& p);
    // B. sketches
    Json createSketch(const Json& p);
    Json sketchAddGeometry(const Json& p);
    Json sketchAddConstraints(const Json& p);
    // C. 3D operations
    Json extrude(const Json& p);
    Json revolve(const Json& p);
    Json filletOrChamfer(const Json& p);
    Json shell(const Json& p);
    // ergonomics lot: direct modelling without a sketch
    Json createBox(const Json& p);
    Json createCylinder(const Json& p);
    Json pressPull(const Json& p);
    Json createPrimitive(const Json& p, const char* type);
    // v0.4 lot 0
    Json removeBody(const Json& p);
    Json defeatureFaces(const Json& p);
    Json splitBody(const Json& p);
    Json transformBody(const Json& p);
    // v0.4 lot 1
    Json createConstructionPlane(const Json& p);
    Json createRib(const Json& p);
    Json alignBodies(const Json& p);
    // v0.4 lot 2
    Json checkPrintability(const Json& p);
    Json createThread(const Json& p);
    // v0.5 lot 1
    Json createFastenerHole(const Json& p);
    Json listComponents(const Json& p);
    Json insertComponent(const Json& p);
    // v0.5 lot 4
    Json layFlatOnBed(const Json& p);
    Json createEnclosure(const Json& p);
    Json checkBuildVolume(const Json& p);
    // v0.5 lot 5
    Json recordLesson(const Json& p);
    // v0.4 lot 3
    Json getPedagogicalContext(const Json& p);
    SketchPlaneRef parsePlaneRef(const Json& p, const char* key);
    Json bodiesSummary(const Analysis& a) const;
    Json combine(const Json& p);
    Json mirror(const Json& p);
    Json linearPattern(const Json& p);
    Json circularPattern(const Json& p);
    GeomRef geomRefFromId(const std::string& id, bool planes) const;
    TransformTargets targetsFrom(const Json& p) const;
    Json patternResult(const Analysis& a, const Uuid& fid) const;
    // v0.3
    Json sweep(const Json& p);
    Json loft(const Json& p);
    Json getParameters(const Json& p);
    Json setParameter(const Json& p);
    Json deleteParameter(const Json& p);
    Json measure(const Json& p);
    Json checkInterference(const Json& p);
    Json setBodyMaterial(const Json& p);
    Json export3mf(const Json& p);
    Json exportDrawing(const Json& p);
    // D. editing, history, I/O
    Json updateFeatureParameter(const Json& p);
    Json undo(const Json& p);
    Json redo(const Json& p);
    Json importStep(const Json& p);
    Json exportStep(const Json& p);
    Json exportStl(const Json& p);
    Json saveDesign(const Json& p);
    Json shutdown(const Json& p);
    // sprint "Fiabilité": document cycle, undo groups, dimensional check
    Json newDocument(const Json& p);
    Json openDocument(const Json& p);
    Json getActiveDocumentInfo(const Json& p);
    Json beginCommandGroup(const Json& p);
    Json endCommandGroup(const Json& p);
    Json verifyDimensions(const Json& p);
    // v0.7 "Trio Makers"
    Json createEmbossText(const Json& p);
    Json sketchImportVector(const Json& p);
    Json sketchSetCanvas(const Json& p);
    void requireDiscardable(const Json& p, const char* what) const;
    Json ping(const Json& p);

    // helpers
    const DocState& doc() const;
    FeaturePtr requireFeature(const std::string& id, const char* type = nullptr) const;
    /// Executes a command; if the feature `fid` fails to rebuild the command is undone and an
    /// IpcError carrying the kernel message is thrown (agents never leave broken features).
    Analysis commit(const Command& cmd, const Uuid& fid);
    Json bodySummary(const Analysis& a, const Uuid& bodyId) const;

    ui::ModelController* m_model;
    std::unique_ptr<ui::Viewport> m_capture;
    bool m_quit = false;
};

}  // namespace silex::ipc
