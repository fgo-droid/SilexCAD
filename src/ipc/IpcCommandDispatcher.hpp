#pragma once
// Executes IPC/MCP tool calls against the document. Runs on the Qt main thread; every
// mutation goes through the same immutable Command + History path as the UI (so undo/redo,
// the timeline and the 3D view stay consistent when the GUI is visible).

#include <QObject>
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

class IpcError : public std::runtime_error
{
public:
    explicit IpcError(const std::string& msg, int code = -32000) : std::runtime_error(msg), m_code(code) {}
    int code() const { return m_code; }

private:
    int m_code;
};

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

private:
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
    Json combine(const Json& p);
    Json mirror(const Json& p);
    Json linearPattern(const Json& p);
    Json circularPattern(const Json& p);
    GeomRef geomRefFromId(const std::string& id, bool planes) const;
    TransformTargets targetsFrom(const Json& p) const;
    Json patternResult(const Analysis& a, const Uuid& fid) const;
    // D. editing, history, I/O
    Json updateFeatureParameter(const Json& p);
    Json undo(const Json& p);
    Json redo(const Json& p);
    Json importStep(const Json& p);
    Json exportStep(const Json& p);
    Json exportStl(const Json& p);
    Json saveDesign(const Json& p);
    Json shutdown(const Json& p);
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
