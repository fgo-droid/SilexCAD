#pragma once
// v0.5 lot 1 — "Hole / Insert" tool (Shift+H): standard fastener holes on a planar face.
// Click a planar face: the hole goes where you clicked, snapped to the face center, sketch points,
// existing hole axes (e.g. the mounting holes of an inserted board) or the grid. Ctrl+click adds
// another hole (one timeline step for all of them). Type / size / fit / depth in the panel, live preview.

#include <QPointer>
#include <optional>
#include <string>
#include <vector>

#include "Workspace.h"
#include "model/Features.h"

class QComboBox;
class QLabel;

namespace silex::ui {

class ExprSpinBox;

class HoleEditor final : public EditSession
{
public:
    HoleEditor(Workspace& ws, FeaturePtr existing);
    ~HoleEditor() override;

    QString title() const override { return QString::fromStdString(m_name); }
    QWidget* createPanel(QWidget* parent) override;
    uint32_t pickFilter() const override;
    bool mousePress(QMouseEvent* e) override;
    bool mouseMove(QMouseEvent* e) override;
    void paintOverlay(QPainter& p) override;
    void onSnapshot() override;
    bool commit() override;
    void cancel() override {}
    std::vector<MenuAction> menuActions() override;
    Uuid editedFeature() const override { return m_existing ? m_id : Uuid{}; }

    /// Translated name of a hole type ("clearance" -> "Clearance hole").
    static QString kindLabel(const std::string& kind);

private:
    std::optional<Placement> plane() const;
    /// Snapped plane coordinates of the cursor (nullopt: not on the plane).
    std::optional<Vec2> snapped(QPointF px, QString* what) const;
    FeaturePtr buildNode() const;
    void pushPreview();
    void syncSelection();
    void updatePanel();
    bool ready() const { return !m_hp.centers.empty() && (m_hp.plane.kind != "Face" || !m_hp.plane.face.name.empty()); }

    FeaturePtr m_existing;
    Uuid m_id;
    std::string m_name;
    FastenerHoleParams m_hp;
    bool m_placed = false;  ///< false until the first click (new hole)
    int m_active = 0;       ///< center edited by the X / Y fields
    std::optional<QPointF> m_hoverPx;
    QString m_snapWhat;

    QPointer<QComboBox> m_kindBox, m_sizeBox, m_fitBox;
    QPointer<ExprSpinBox> m_depthBox, m_xBox, m_yBox, m_nutBox, m_angleBox;
    QPointer<QLabel> m_summary, m_where, m_message;
    bool m_updating = false;
};

}  // namespace silex::ui
