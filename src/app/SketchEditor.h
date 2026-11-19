#pragma once
// In-viewport sketch editor: drawing tools with live auto-constraints, direct manipulation
// (drag solved by PlaneGCS in real time), dimensions edited in place, DOF colour code.

#include <QPointer>
#include <optional>
#include <set>
#include <vector>

#include "Workspace.h"
#include "model/Features.h"
#include "sketch/AutoConstraint.h"
#include "sketch/SketchSolver.h"

class QLabel;
class QLineEdit;
class QPushButton;

namespace silex::ui {

class SketchEditor final : public EditSession
{
public:
    enum class Tool { Select, Line, Rectangle, Circle, Arc, Dimension };

    SketchEditor(Workspace& ws, FeaturePtr existing, SketchPlaneRef planeRef, Placement plane);
    ~SketchEditor() override;

    QString title() const override;
    QWidget* createPanel(QWidget* parent) override;
    bool mousePress(QMouseEvent* e) override;
    bool mouseMove(QMouseEvent* e) override;
    bool mouseRelease(QMouseEvent* e) override;
    bool mouseDoubleClick(QMouseEvent* e) override;
    bool keyPress(QKeyEvent* e) override;
    void paintOverlay(QPainter& p) override;
    bool commit() override;
    void cancel() override;
    bool undo() override;
    bool redo() override;
    std::vector<MenuAction> menuActions() override;
    Uuid editedFeature() const override { return m_existing ? m_id : Uuid{}; }
    bool isSketch() const override { return true; }
    Uuid hiddenSketch() const override { return m_id; }

    void setTool(Tool t);
    void applyConstraint(ConstraintKind k);
    void toggleConstruction();
    void deleteSelection();

private:
    struct Hit
    {
        enum Kind { None, Point, Curve, Constraint } kind = None;
        GeoRef ref;
        Uuid constraint;
    };

    // geometry helpers
    std::optional<Vec2> toLocal(QPointF px) const;
    bool toScreen(Vec2 p, QPointF* out) const;
    double modelPerPixel() const;
    Hit hitTest(QPointF px) const;
    QColor colorOf(const SketchGeo& g) const;

    // editing helpers
    void snapshotUndo();
    void solve(bool diagnose = true);
    void changed();
    void addGeo(const SketchGeo& g);
    void addConstraint(SketchConstraint c);
    void addSnapConstraints(const Snap& s, const GeoRef& p);
    void resetTool();
    void openValueEditor(const Uuid& constraint);
    void closeValueEditor(bool apply);
    std::optional<Vec2> labelAnchor(const SketchConstraint& c, QPointF* screen) const;
    void updatePanel();

    // tools
    void clickLine(Vec2 p);
    void clickRect(Vec2 p);
    void clickCircle(Vec2 p);
    void clickArc(Vec2 p);
    void clickDimension(const Hit& h, Vec2 p);
    void clickSelect(const Hit& h, QMouseEvent* e);

    // painting
    void paintGeometry(QPainter& p);
    void paintConstraints(QPainter& p);
    void paintToolPreview(QPainter& p);
    void paintStatus(QPainter& p);

    FeaturePtr m_existing;
    Uuid m_id;
    std::string m_name;
    SketchPlaneRef m_planeRef;
    Placement m_plane;
    Sketch m_sketch;
    SolveResult m_status;
    Tool m_tool = Tool::Select;

    // tool state
    std::vector<Vec2> m_clicks;
    std::vector<Snap> m_clickSnaps;
    std::optional<GeoRef> m_chain;  ///< end of the previous line in a polyline
    Snap m_snap;
    LineHint m_hint = LineHint::None;
    bool m_cursorValid = false;
    std::optional<Hit> m_dimFirst;

    // selection
    std::vector<GeoRef> m_selPoints;
    std::vector<Uuid> m_selCurves;
    std::vector<Uuid> m_selConstraints;
    Hit m_hover;

    // drag
    bool m_pressed = false;
    bool m_dragging = false;
    QPointF m_pressPx;
    GeoRef m_dragRef;
    Vec2 m_grabOffset;

    // inline value editor
    QPointer<QLineEdit> m_edit;
    Uuid m_editConstraint;

    std::vector<Sketch> m_undo, m_redo;
    mutable std::vector<std::pair<QRectF, Uuid>> m_labelRects;

    // panel
    QPointer<QLabel> m_statusLabel;
    std::vector<std::pair<Tool, QPointer<QPushButton>>> m_toolButtons;
};

}  // namespace silex::ui
