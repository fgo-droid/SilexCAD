#pragma once
// v0.3 Lot 2 — editing session for Sweep (Shift+W) and Loft (Shift+L): profiles picked in the
// viewport, path = sketch (open or closed chain) or model edges, ordered loft sections (up / down),
// options and boolean operation, live semi-transparent preview.

#include <QPointer>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Workspace.h"
#include "model/Features.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;

namespace silex::ui {

class SweepLoftEditor final : public EditSession
{
public:
    SweepLoftEditor(Workspace& ws, std::string type, FeaturePtr existing);

    QString title() const override { return QString::fromStdString(m_name); }
    QWidget* createPanel(QWidget* parent) override;
    uint32_t pickFilter() const override;
    void onPicked(const SelItem& item, bool additive) override;
    void onSnapshot() override;
    void paintOverlay(QPainter& p) override;
    bool commit() override;
    void cancel() override {}
    std::vector<MenuAction> menuActions() override;
    Uuid editedFeature() const override { return m_existing ? m_id : Uuid{}; }
    std::set<Uuid> sketchesInUse() const override;

private:
    using Profile = std::pair<Uuid, std::string>;  ///< (sketch, region key)

    bool isSweep() const { return m_type == FeatureType::Sweep; }
    bool ready() const;
    FeaturePtr buildNode() const;
    void pushPreview();
    void syncSelection();
    void updatePanel();
    void fillPathCombo();
    void fillSectionList();
    QString sketchName(const Uuid& id) const;

    std::string m_type;
    FeaturePtr m_existing;
    Uuid m_id;
    std::string m_name;

    std::vector<Profile> m_profiles;  ///< sweep: first only; loft: ordered sections
    PathRef m_path;
    std::string m_orientation = "CorrectedFrenet";
    bool m_solid = true, m_ruled = false;
    BooleanOp m_op = BooleanOp::NewBody;
    bool m_pickPathEdges = false;
    bool m_updating = false;

    QPointer<QLabel> m_profileLabel;
    QPointer<QComboBox> m_pathBox;
    QPointer<QPushButton> m_pathPick;
    QPointer<QLabel> m_pathLabel;
    QPointer<QListWidget> m_sectionList;
    QPointer<QComboBox> m_opBox;
    QPointer<QLabel> m_message;
};

}  // namespace silex::ui
