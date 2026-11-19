#pragma once
// Editing session for Extrude / Revolve / Fillet / Chamfer: selection-driven parameters,
// live transparent preview (recomputed asynchronously, cancellable) and a direct
// manipulation handle for the extrusion distance.

#include <QPointer>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "Workspace.h"
#include "model/Features.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;

namespace silex::ui {

class FeatureEditor final : public EditSession
{
public:
    FeatureEditor(Workspace& ws, std::string type, FeaturePtr existing);
    ~FeatureEditor() override;

    QString title() const override;
    QWidget* createPanel(QWidget* parent) override;
    uint32_t pickFilter() const override;
    bool mousePress(QMouseEvent* e) override;
    bool mouseMove(QMouseEvent* e) override;
    bool mouseRelease(QMouseEvent* e) override;
    bool keyPress(QKeyEvent* e) override;
    void paintOverlay(QPainter& p) override;
    void onSnapshot() override;
    void onPicked(const SelItem& item, bool additive) override;
    bool commit() override;
    void cancel() override;
    std::vector<MenuAction> menuActions() override;
    Uuid editedFeature() const override { return m_existing ? m_id : Uuid{}; }
    std::set<Uuid> sketchesInUse() const override;

private:
    bool isProfileBased() const;
    bool isBlend() const;
    bool isCombine() const;
    bool isShell() const;
    QString bodyName(const Uuid& id) const;
    FeaturePtr buildNode() const;
    DocState previewDoc() const;
    void pushPreview();
    void syncSelection();
    void updatePanel();
    void fillAxisCombo();
    bool gizmo(QPointF* base, QPointF* tip, Vec3* origin, Vec3* dir) const;

    std::string m_type;
    FeaturePtr m_existing;
    Uuid m_id;
    std::string m_name;

    // parameters
    std::vector<std::pair<Uuid, std::string>> m_profiles;  ///< (sketch, region key)
    std::vector<TopoRef> m_edges;
    Uuid m_target;                ///< Combine: body receiving the result
    std::vector<Uuid> m_tools;    ///< Combine: tool bodies
    bool m_keepTools = false;
    std::vector<TopoRef> m_shellFaces;  ///< Shell: removed faces
    bool m_outward = false;             ///< Shell: direction
    double m_value = 10.0;  ///< distance / angle / radius / chamfer distance
    bool m_symmetric = false;
    BooleanOp m_op = BooleanOp::NewBody;
    std::string m_axis = "Y";
    bool m_autoPickPending = false;

    // gizmo drag
    bool m_dragging = false;
    bool m_gizmoHover = false;
    double m_dragStartValue = 0;
    double m_dragStartT = 0;

    QPointer<QLabel> m_selLabel;
    QPointer<QDoubleSpinBox> m_valueBox;
    QPointer<QComboBox> m_opBox;
    QPointer<QComboBox> m_dirBox;
    QPointer<QComboBox> m_axisBox;
    QPointer<QLabel> m_targetLabel;
    QPointer<QLabel> m_message;
    bool m_updatingPanel = false;
};

}  // namespace silex::ui
