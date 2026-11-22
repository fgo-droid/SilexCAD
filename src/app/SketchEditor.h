#pragma once
// In-viewport sketch editor: drawing tools with live auto-constraints, direct manipulation
// (drag solved by PlaneGCS in real time), dimensions edited in place, DOF colour code.
// Sketcher Pro: smart primitives, trim / extend / fillet / offset / mirror / pattern, floating
// dimension inputs while drawing (Tab / Enter), constraint badges, magnetic grid, DOF diagnosis.

#include <QElapsedTimer>
#include <QImage>
#include <QPointer>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "Icons.h"
#include "Workspace.h"
#include "model/Features.h"
#include "sketch/AutoConstraint.h"
#include "sketch/SketchSolver.h"
#include "sketch/SketchTools.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QAbstractButton;

namespace silex::ui {

class SketchEditor final : public EditSession
{
public:
    enum class Tool {
        Select, Line, Rectangle, CenterRectangle, Polygon, Circle, Arc, Slot, Dimension, Project,
        Trim, Extend, Fillet,
    };

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

    /// Normal To / Look At (N): camera flat in front of the sketch plane.
    void lookAt();
    void selectAllCurves();  ///< Ctrl+A
    /// Constraints that make sense for the current selection (right-click menu).
    std::vector<ConstraintKind> compatibleConstraints() const;
    static QString constraintLabel(ConstraintKind k);  ///< "Midpoint / Milieu (M)"
    static IconId constraintIcon(ConstraintKind k);
    Uuid editedFeature() const override { return m_existing ? m_id : Uuid{}; }
    bool isSketch() const override { return true; }
    Uuid hiddenSketch() const override { return m_id; }

    void setTool(Tool t);
    void applyConstraint(ConstraintKind k);
    void toggleConstruction();
    void deleteSelection();
    // Sketcher Pro commands (act on the selection)
    void runOffset();
    void runMirror();
    void runPattern();
    void showUnderConstrained();
    // v0.7 "Trio Makers"
    void importVector();     ///< Import SVG / DXF (Ctrl+Shift+I)
    void insertCanvas();     ///< Insert Image (Canvas)
    void startCalibration(); ///< 2-click calibration of the image
    void removeCanvas();
    bool hasCanvas() const { return m_canvas.has_value(); }

private:
    struct Hit
    {
        enum Kind { None, Point, Curve, Constraint } kind = None;
        GeoRef ref;
        Uuid constraint;
    };

    /// Floating input next to the cursor (value typed while drawing, Tab to switch, Enter to apply).
    struct Field
    {
        QString key;    ///< "length", "angle", "width", "height", "diameter", "sides", "radius", ...
        QString label;
        QString text;   ///< typed text (empty = follows the cursor)
        double live = 0;
        bool angle = false;
        bool integer = false;
    };
    enum class Pending { None, Offset, LinearPattern, CircularPattern };

    // geometry helpers
    std::optional<Vec2> toLocal(QPointF px) const;
    bool toScreen(Vec2 p, QPointF* out) const;
    double modelPerPixel() const;
    Hit hitTest(QPointF px) const;
    QColor colorOf(const SketchGeo& g) const;
    Snap snapAt(Vec2 lp, Qt::KeyboardModifiers mods) const;
    std::set<Uuid> constraintGeos(const Uuid& constraint) const;

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
    /// Applies the result of a sketch tool: solve, undo entry, status; reverts if it fails.
    bool applyTool(const sketchtools::ToolResult& r, const Sketch& before, const QString& done);

    // floating inputs
    void refreshFields();
    std::optional<double> fieldValue(const QString& key) const;
    Field* field(const QString& key);
    Vec2 constrainedCursor() const;
    void updateLiveFields();
    bool fieldKey(QKeyEvent* e);
    void commitFields();
    void dispatchClick(const Hit& h, Vec2 lp, QMouseEvent* e);
    void addTypedDimensions();
    void updatePendingPreview();

    // tools
    void clickLine(Vec2 p);
    void clickRect(Vec2 p);
    void clickCenterRect();
    void clickPolygon();
    void clickCircle(Vec2 p);
    void clickArc(Vec2 p);
    void clickSlot();
    void clickDimension(const Hit& h, Vec2 p);
    void clickSelect(const Hit& h, QMouseEvent* e);
    void clickProject(QPointF px);  ///< v0.3: projects a model edge into the sketch
    void clickTrimExtend(const Hit& h, Vec2 p);
    void clickFillet(const Hit& h);

    // painting
    void paintGrid(QPainter& p);
    void paintGeometry(QPainter& p);
    void paintConstraints(QPainter& p);
    void paintToolPreview(QPainter& p);
    void paintPiece(QPainter& p, const sketchtools::CurvePiece& piece, const QPen& pen);
    void paintFields(QPainter& p);
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
    QPointF m_cursorPx;
    std::optional<Hit> m_dimFirst;
    std::optional<Uuid> m_filletFirst;
    std::optional<sketchtools::CurvePiece> m_piece;  ///< trim (removed) / extend (result) preview
    int m_polySides = 6;
    bool m_polyInscribed = true;
    bool m_slotOverall = false;
    double m_filletRadius = 2.0;

    // floating inputs & pending commands
    std::vector<Field> m_fields;
    int m_activeField = 0;
    int m_fieldStage = -1;                    ///< tool / click count / pending the fields were built for
    std::map<QString, double> m_typedDims;    ///< values typed in previous clicks of the same shape
    std::map<std::string, Uuid> m_lastNamed;  ///< pieces of the last created shape (for typed dimensions)
    Pending m_pending = Pending::None;
    std::vector<Uuid> m_pendingGeos;
    GeoRef m_pendingCenter;
    std::vector<SketchGeo> m_previewGeos;

    // selection
    std::vector<GeoRef> m_selPoints;
    std::vector<Uuid> m_selCurves;
    std::vector<Uuid> m_selConstraints;
    Hit m_hover;

    // box selection (left->right: window, right->left: crossing)
    bool m_boxing = false;
    QPointF m_boxFrom, m_boxTo;
    void finishBoxSelect(bool additive);

    // drag
    bool m_pressed = false;
    bool m_dragging = false;
    QPointF m_pressPx;
    GeoRef m_dragRef;
    Vec2 m_grabOffset;

    // inline value editor
    QPointer<QLineEdit> m_edit;
    Uuid m_editConstraint;

    // "show under-constrained" pulse
    QPointer<QTimer> m_pulseTimer;
    QElapsedTimer m_pulseClock;
    bool m_pulsing = false;

    std::vector<Sketch> m_undo, m_redo;

    // v0.7 lot C: calibrated reference image
    std::optional<SketchCanvas> m_canvas;
    QImage m_canvasImage;
    size_t m_canvasKey = 0;   ///< hash of the decoded image data
    int m_calibStage = 0;     ///< 0 none, 1 first point, 2 second point
    Vec2 m_calibA;
    void paintCanvas(QPainter& p);
    bool calibrationClick(Vec2 lp);
    mutable std::vector<std::pair<QRectF, Uuid>> m_labelRects;

    // panel
    QPointer<QLabel> m_statusLabel;
    QPointer<QLabel> m_coachLabel;  ///< v0.4 lot 3: SketchCoach explanation
    void explain();
    std::vector<std::pair<Tool, QPointer<QAbstractButton>>> m_toolButtons;
};

}  // namespace silex::ui
