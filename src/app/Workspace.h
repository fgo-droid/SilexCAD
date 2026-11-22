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
#include "model/RenderModel.h"

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
    void startMeasure();       ///< v0.3 (I)
    void startInterference();  ///< v0.3 (Shift+I)
    /// Ergonomics lot (Q): closed profile -> Extrude, planar face of a solid -> OffsetFace.
    void startPressPull();
    void startPrintability();  ///< v0.4 lot 2
    /// v0.4 lot 3: plain-language explanation of a failed / warned step (empty if none).
    QString explainStep(const Uuid& featureId) const;
    /// v0.4 lot 1: Move / Copy session started in Align mode ("face" or "axis").
    void startPlane(const std::string& mode);  ///< Construct > Offset / Angled / Midplane
    void startDeleteFace();                     ///< Modify > Delete Face (deletes the selected faces)
    void startAlign(const std::string& mode = "face");
    /// v0.5 lot 1: inserts a library component (locked reference body) and selects it.
    bool insertComponent(const std::string& id);

    // ---- v0.5 lot 4: 3D printing helpers
    struct Bed
    {
        bool on = false;
        QString name = "Prusa MK4 / Ender 3";
        Vec3 size{220, 220, 250};
    };
    void setBed(const Bed& bed);
    const Bed& bed() const { return m_bed; }
    /// Names of the visible printable bodies leaving the build volume (empty = everything fits).
    QStringList bodiesOutsideBed() const;
    /// Lay flat on bed: the selected planar face, or the next face clicked.
    void startLayFlat();
    void layFlat(const SelItem& face);
    /// Enclosure wizard (dialog, then one undo step).
    void startEnclosureWizard();

    // ---- v0.5 lot 2: "before / after" ghost (pale wireframe of the previous state)
    /// While held (editing an existing step, Parameters dialog open) the ghost shows the state before;
    /// released, it fades out 3 s after the new geometry arrives.
    void holdGhost(bool on);
    bool ghostEnabled() const { return m_ghostOn; }
    void setGhostEnabled(bool on);

    // ---- v0.4 lot 0: direct management of bodies & faces
    /// Del: whole bodies -> RemoveBody step, faces -> DefeatureFace step.
    void deleteSelection();
    void toggleVisibility();  ///< V: hide / show the selected bodies (nothing selected: show all)
    void setBodyHidden(const Uuid& id, bool hidden);
    bool bodyHidden(const Uuid& id) const { return m_hl.hiddenBodies.count(id) > 0; }
    void isolateSelection();
    void unisolate();
    bool isolated() const { return m_isolated; }

    // ---- v0.3 section view (display only) & X-ray
    struct SectionState
    {
        bool on = false;
        std::string plane = "XZ";  ///< "XY" | "XZ" | "YZ" | "Face"
        TopoRef face;
        Placement facePlane;
        double offset = 0;         ///< mm along the plane normal
        bool flip = false;
    };
    const SectionState& section() const { return m_section; }
    void setSection(const SectionState& s);
    void setSectionFace(const SelItem& face);
    Placement sectionBasePlacement(const SectionState& s) const;
    void setGhostBodies(bool on);
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

    // ---- Sketcher Pro: magnetic grid (toggle + step in the status bar, Alt bypasses it)
    bool gridSnap() const { return m_gridSnap; }
    double gridStep() const { return m_gridStep; }
    void setGridSnap(bool on) { m_gridSnap = on; }
    void setGridStep(double mm) { m_gridStep = mm > 0 ? mm : 1.0; }
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
    void sectionToggleRequested();
    void visibilityChanged();
    void bedChanged();  ///< v0.5: the build plate was switched on / off by a tool

private:
    void onSnapshot();
    void updateProfileVisibility();
    void endSession();
    void beginSession(std::unique_ptr<EditSession> s);
    void paintSketches(QPainter& p);
    void paintPlanes(QPainter& p);
    /// v0.4: construction plane under the cursor (screen-space test on its displayed square).
    SelItem constructionPlaneAt(QPointF px) const;

    ModelController* m_model;
    Viewport* m_vp;
    std::unique_ptr<EditSession> m_session;
    HighlightState m_hl;
    Uuid m_lastSketch;
    bool m_pickingPlane = false;
    bool m_layFlatPick = false;  ///< v0.5: next planar face clicked is laid on the bed
    std::shared_ptr<const ModelSnapshot> m_ghost;  ///< v0.5: previous state shown as a pale wireframe
    bool m_ghostHold = false, m_ghostOn = true;
    uint64_t m_ghostRev = 0;     ///< revision of the ghost state (fade out once the model moved on)
    int m_ghostGen = 0;          ///< cancels stale fade-out timers
    void armGhostFade();
    void paintGhost(QPainter& p);
    Bed m_bed;
    QStringList m_outside;       ///< bodies leaving the build volume
    void updateBed();
    SectionState m_section;
    QPoint m_rightPressPos;
    bool m_boxing = false;  ///< box selection of bodies (left->right window, right->left crossing)
    QPointF m_boxFrom, m_boxTo;
    void finishBoxSelect(bool additive);
    bool m_gridSnap = false;
    bool m_isolated = false;
    std::set<Uuid> m_preIsolateHidden;
    void visibilityUpdated();
    double m_gridStep = 1.0;
};

}  // namespace silex::ui
