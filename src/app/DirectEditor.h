#pragma once
// v0.4 lot 0 / lot 1 — editing sessions of the direct body / face steps:
//   RemoveBody (bodies list), DefeatureFace (faces to delete), SplitBody (body + plane),
//   TransformBody (Move / Copy with a 3D gizmo: 3 arrows + 3 rotation arcs, numeric fields,
//   "Create copy", and the Align / Mate mode: face to face with a gap, or coaxial).
// All of them are timeline steps with a live preview.

#include <QPointer>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Workspace.h"
#include "model/Features.h"

class QCheckBox;
class QComboBox;
class QLabel;

namespace silex::ui {

class ExprSpinBox;

class DirectEditor final : public EditSession
{
public:
    DirectEditor(Workspace& ws, std::string type, FeaturePtr existing);
    ~DirectEditor() override;

    static bool handles(const std::string& type);

    QString title() const override { return QString::fromStdString(m_name); }
    QWidget* createPanel(QWidget* parent) override;
    uint32_t pickFilter() const override;
    void onPicked(const SelItem& item, bool additive) override;
    void onSnapshot() override;
    bool mousePress(QMouseEvent* e) override;
    bool mouseMove(QMouseEvent* e) override;
    bool mouseRelease(QMouseEvent* e) override;
    void paintOverlay(QPainter& p) override;
    bool commit() override;
    void cancel() override {}
    std::vector<MenuAction> menuActions() override;
    Uuid editedFeature() const override { return m_existing ? m_id : Uuid{}; }

    /// TransformBody: start directly in Align / Mate mode (lot 1 tool).
    void startMate(const std::string& mode);
    /// ConstructionPlane: preset the type ("offset", "angle", "mid") from the Construct menu.
    void setPlaneMode(const std::string& mode);

private:
    bool isTransform() const { return m_type == FeatureType::TransformBody; }
    bool isPlane() const { return m_type == FeatureType::ConstructionPlane; }
    bool isRib() const { return m_type == FeatureType::Rib; }
    bool isThread() const { return m_type == FeatureType::Thread; }
    void fillPlaneCombo(QComboBox* box, const SketchPlaneRef& current) const;
    void applyPlaneCombo(QComboBox* box, SketchPlaneRef& ref);
    void fillRibCombos();
    bool mateActive() const { return !m_mateMode.empty(); }
    FeaturePtr buildNode() const;
    void pushPreview();
    void syncSelection();
    void updatePanel();
    QString bodyName(const Uuid& id) const;
    bool ready() const;

    // gizmo
    bool gizmoCenter(Vec3* c, double* worldLen) const;
    int hitGizmo(QPointF px) const;  ///< 0..2 arrows X Y Z, 3..5 arcs, -1 none
    std::vector<QPointF> arcPoints(int axis, const Vec3& c, double r) const;

    std::string m_type;
    FeaturePtr m_existing;
    Uuid m_id;
    std::string m_name;

    std::vector<Uuid> m_bodies;   // RemoveBody / TransformBody / SplitBody (first)
    std::vector<TopoRef> m_faces; // DefeatureFace
    SketchPlaneRef m_plane;       // SplitBody
    SplitBodyParams m_split;      // SplitBody: v0.5 alignment pins
    bool m_pickSplitFace = false;
    TransformBodyParams m_tp;
    std::map<std::string, std::string> m_exprs;

    // construction plane & rib (lot 1)
    ConstructionPlaneParams m_cp;
    enum class Pick { None, BaseFace, AxisEdge, MidFaces } m_pick = Pick::None;
    RibParams m_rib;
    ThreadParams m_thread;
    QPointer<QComboBox> m_modeBox, m_baseBox, m_axisBox, m_sketchBox, m_lineBox;

    // mate (lot 1)
    std::string m_mateMode;  ///< "" | "face" | "axis"
    TopoRef m_mateMoving, m_mateFixed;
    double m_mateOffset = 0;
    std::string m_mateOffsetExpr;
    bool m_mateFlip = false;

    // gizmo drag
    int m_drag = -1;
    int m_hover = -1;
    double m_dragStart = 0;
    double m_dragT0 = 0;
    QPointF m_dragPx0;

    QPointer<QLabel> m_selLabel;
    QPointer<QLabel> m_message;
    QPointer<QComboBox> m_planeBox;
    QPointer<QComboBox> m_mateBox;
    QPointer<QCheckBox> m_copyBox;
    std::vector<QPointer<ExprSpinBox>> m_fields;  ///< dx dy dz rx ry rz (TransformBody)
    QPointer<ExprSpinBox> m_offsetBox;
    bool m_updating = false;
};

}  // namespace silex::ui
