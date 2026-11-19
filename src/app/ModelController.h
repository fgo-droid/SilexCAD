#pragma once
// Bridge between the immutable document (History) and the asynchronous kernel.

#include <QObject>
#include <QString>
#include <memory>
#include <optional>

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
    bool save(const QString& path, QString* error);
    QString filePath() const { return m_path; }
    bool isDirty() const { return m_history.dirty(); }
    bool busy() const { return m_pending > m_received; }

signals:
    void documentChanged();
    void snapshotChanged();
    void busyChanged(bool busy);

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
    std::unique_ptr<KernelEngine> m_engine;  // last: destroyed first (joins the worker)
};

}  // namespace silex::ui
