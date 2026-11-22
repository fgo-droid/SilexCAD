#pragma once
// Bridge between the immutable document (History) and the asynchronous kernel.

#include <QObject>
#include <QString>
#include <deque>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include "kernel/KernelEngine.h"
#include "model/Document.h"
#include "model/RenderModel.h"

namespace silex::ui {

class ModelController : public QObject
{
    Q_OBJECT
public:
    explicit ModelController(QObject* parent = nullptr);
    ~ModelController() override;

    const DocState& doc() const { return m_history.current(); }
    const History& history() const { return m_history; }
    /// Kernel services on the last computed geometry (measure, interference...).
    const KernelEngine& engine() const { return *m_engine; }
    std::shared_ptr<const ModelSnapshot> snapshot() const { return m_snapshot; }

    /// Intent -> Command -> new immutable state -> async recompute.
    void execute(const Command& cmd);
    void undo();
    void redo();

    /// Live preview of a not-yet-committed state (editing session). Not recorded in history.
    void preview(const DocState& state, const Uuid& featureId);
    void endPreview();
    bool inPreview() const { return m_previewState.has_value(); }
    const DocState& displayedDoc() const { return m_previewState ? *m_previewState : doc(); }

    void newDocument();
    bool load(const QString& path, QString* error);
    /// Crash recovery: replaces the document by a recovered state. The document keeps `sourcePath`
    /// (empty = untitled) and is marked modified: the user must save it explicitly.
    void restore(DocState state, const QString& sourcePath);
    /// MCP begin_command_group / end_command_group: the commands executed in between are ONE undo step.
    bool beginCommandGroup(const QString& label) { return m_history.beginGroup(label.toStdString()); }
    int endCommandGroup() { return m_history.endGroup(); }
    bool save(const QString& path, QString* error);
    QString filePath() const { return m_path; }
    bool isDirty() const { return m_history.dirty(); }
    bool busy() const { return m_pending > m_received; }

    // ---- v0.4 lot 3: what the guided mode / the AI tutor can observe of the session
    /// Session events validated by lessons ("interference_ok", "drawing_exported"...).
    void noteEvent(const std::string& e);
    const std::set<std::string>& events() const { return m_events; }
    /// Recent mistakes of the user (refused constraints, failed steps), newest last (max 20).
    void noteUserError(const std::string& e);
    const std::deque<std::string>& userErrors() const { return m_userErrors; }
    /// Active lesson / step, published by the learn panel (null when no lesson is running).
    void setTutorialState(Json s) { m_tutorial = std::move(s); }
    const Json& tutorialState() const { return m_tutorial; }
    /// French or English pedagogical texts (coach, diagnostics).
    bool pedagogyFrench() const { return m_pedagogyFr; }
    void setPedagogyFrench(bool fr) { m_pedagogyFr = fr; }
    /// v0.5 lot 5: Socratic AI tutor — the IPC / MCP server only accepts observation commands.
    bool tutorMode() const { return m_tutorMode; }
    void setTutorMode(bool on)
    {
        if (on == m_tutorMode)
            return;
        m_tutorMode = on;
        emit tutorModeChanged(on);
    }

signals:
    void documentChanged();
    void snapshotChanged();
    void busyChanged(bool busy);
    void eventsChanged();
    void tutorModeChanged(bool on);

private:
    void submit();
    void onResult(std::shared_ptr<const ModelSnapshot> snap);

    History m_history;
    std::optional<DocState> m_previewState;
    Uuid m_previewFeature;
    std::shared_ptr<const ModelSnapshot> m_snapshot;
    uint64_t m_pending = 0;
    uint64_t m_received = 0;
    QString m_path;
    std::set<std::string> m_events;
    std::deque<std::string> m_userErrors;
    Json m_tutorial;
    bool m_pedagogyFr = true;
    bool m_tutorMode = false;
    std::unique_ptr<KernelEngine> m_engine;  // last: destroyed first (joins the worker)
};

}  // namespace silex::ui
