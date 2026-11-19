#pragma once
// Workspace = the single, context-aware interaction state machine ("zero context break":
// there are no workbenches; the active session and the selection decide what is offered).

#include <QIcon>
#include <QObject>
#include <QPoint>
#include <QString>
#include <functional>
#include <memory>
#include <set>
#include <vector>

#include "Selection.h"
#include "ViewportTool.h"
#include "model/Document.h"

class QWidget;

namespace silex::ui {

class ModelController;
class Viewport;
class Workspace;

struct MenuAction
{
    QString label;
    QIcon icon;
    std::function<void()> run;
    bool enabled = true;
};

/// An editing session (sketch editor, feature editor). Owned by the Workspace.
class EditSession
{
public:
    explicit EditSession(Workspace& ws) : m_ws(ws) {}
    virtual ~EditSession() = default;

    virtual QString title() const = 0;
    virtual QWidget* createPanel(QWidget* parent) = 0;
    virtual uint32_t pickFilter() const { return 0; }
    virtual bool mousePress(QMouseEvent*) { return false; }
    virtual bool mouseMove(QMouseEvent*) { return false; }
    virtual bool mouseRelease(QMouseEvent*) { return false; }
    virtual bool mouseDoubleClick(QMouseEvent*) { return false; }
    virtual bool keyPress(QKeyEvent*) { return false; }
    virtual void paintOverlay(QPainter&) {}
    virtual void onSnapshot() {}
    /// Selection click in the viewport while the session filters picks.
    virtual void onPicked(const SelItem&, bool /*additive*/) {}
    virtual bool commit() = 0;  ///< true when the session can close
    virtual void cancel() = 0;
    virtual bool undo() { return false; }
    virtual bool redo() { return false; }
    virtual std::vector<MenuAction> menuActions() { return {}; }
    virtual Uuid editedFeature() const { return {}; }
    virtual bool isSketch() const { return false; }
    virtual Uuid hiddenSketch() const { return {}; }        ///< sketch drawn by the session itself
    virtual std::set<Uuid> sketchesInUse() const { return {}; }

protected:
    Workspace& m_ws;
};

class Workspace : public QObject, public ViewportClient
{
    Q_OBJECT
public:
    Workspace(ModelController* model, Viewport* vp, QObject* parent = nullptr);
    ~Workspace() override;

    ModelController* model() const { return m_model; }
    Viewport* viewport() const { return m_vp; }

    // ---- intents
    void startSketch();
    void startSketchOn(const SelItem& planeOrFace);
    void startFeature(const std::string& type);
    void editFeature(const Uuid& id);
    void finishSession();
    void cancelSession();
    bool undo();
    bool redo();

    EditSession* session() const { return m_session.get(); }
    bool hasSession() const { return m_session != nullptr; }
    Uuid lastSketch() const { return m_lastSketch; }
    void setLastSketch(const Uuid& id) { m_lastSketch = id; }

    // ---- selection & highlight
    const std::vector<SelItem>& selection() const { return m_hl.selected; }
    void setSelection(std::vector<SelItem> items);
    void clearSelection() { setSelection({}); }
    void setHover(const SelItem& s);
    void touchHighlight();
    uint32_t currentFilter() const;
    /// Snapshot items for persistent references (profiles / edges / faces).
    SelItem findProfile(const Uuid& sketch, const std::string& key) const;
    SelItem findEdge(const TopoRef& ref) const;
    SelItem bodyItem(const Uuid& bodyId) const;

    // ---- whole-body selection & direct booleans
    /// Bodies involved in the selection, in selection order (first = boolean target).
    std::vector<Uuid> selectedBodies() const;
    void toggleBody(const Uuid& bodyId);
    /// Adds a Combine feature: first selected body op= the others (tools consumed).
    void combineSelected(BooleanOp op);

    std::vector<MenuAction> contextActions();
    void status(const QString& msg, int timeoutMs = 4000) { emit statusMessage(msg, timeoutMs); }

    // ---- ViewportClient
    void viewportMousePress(QMouseEvent* e) override;
    void viewportMouseMove(QMouseEvent* e) override;
    void viewportMouseRelease(QMouseEvent* e) override;
    void viewportMouseDoubleClick(QMouseEvent* e) override;
    bool viewportKeyPress(QKeyEvent* e) override;
    void paintOverlay(QPainter& p) override;
    const HighlightState& highlight() const override { return m_hl; }

signals:
    void sessionChanged();
    void selectionChanged();
    void statusMessage(const QString& msg, int timeoutMs);
    void markingMenuRequested(QPoint viewportPos);

private:
    void onSnapshot();
    void updateProfileVisibility();
    void endSession();
    void beginSession(std::unique_ptr<EditSession> s);
    void paintSketches(QPainter& p);

    ModelController* m_model;
    Viewport* m_vp;
    std::unique_ptr<EditSession> m_session;
    HighlightState m_hl;
    Uuid m_lastSketch;
    bool m_pickingPlane = false;
    QPoint m_rightPressPos;
};

}  // namespace silex::ui
