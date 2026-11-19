#pragma once
// Editing session for Mirror, Linear Pattern and Circular Pattern: targets (bodies picked in the
// viewport, or features ticked in a list), a reference (plane / direction / axis: world axes and
// origin planes, or an edge / face picked in the viewport) and live semi-transparent preview.

#include <QPointer>
#include <string>
#include <vector>

#include "Workspace.h"
#include "model/Features.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace silex::ui {

class TransformEditor final : public EditSession
{
public:
    TransformEditor(Workspace& ws, std::string type, FeaturePtr existing);

    QString title() const override { return QString::fromStdString(m_name); }
    QWidget* createPanel(QWidget* parent) override;
    uint32_t pickFilter() const override { return SelFilter::Faces | SelFilter::Edges; }
    void onPicked(const SelItem& item, bool additive) override;
    void onSnapshot() override;
    void paintOverlay(QPainter& p) override;
    bool commit() override;
    void cancel() override {}
    std::vector<MenuAction> menuActions() override;
    Uuid editedFeature() const override { return m_existing ? m_id : Uuid{}; }

private:
    enum class Slot { Targets, Ref1, Ref2 };

    bool isMirror() const { return m_type == FeatureType::Mirror; }
    bool isLinear() const { return m_type == FeatureType::LinearPattern; }
    bool isCircular() const { return m_type == FeatureType::CircularPattern; }
    FeaturePtr buildNode() const;
    DocState previewDoc() const;
    void pushPreview();
    void syncSelection();
    void updatePanel();
    void fillFeatureList();
    QComboBox* makeRefCombo(QWidget* parent, bool planes);
    void syncRefCombo(QComboBox* box, const GeomRef& ref, bool planes);
    void onRefCombo(QComboBox* box, GeomRef& ref);
    QString refText(const GeomRef& r) const;
    int insertIndex() const;

    std::string m_type;
    FeaturePtr m_existing;
    Uuid m_id;
    std::string m_name;

    TransformTargets m_targets;
    GeomRef m_ref1{"X", {}};  ///< mirror plane / direction 1 / axis
    GeomRef m_ref2{"Y", {}};  ///< direction 2
    bool m_merge = true;
    PatternDirection m_d1{{"X", {}}, 3, 20.0, false, false};
    PatternDirection m_d2{{"Y", {}}, 1, 20.0, false, false};
    int m_count = 6;
    double m_angle = 360.0;
    bool m_equal = true;
    bool m_reverse = false;
    Slot m_slot = Slot::Targets;
    bool m_updating = false;

    QPointer<QComboBox> m_typeBox;
    QPointer<QLabel> m_targetsLabel;
    QPointer<QListWidget> m_featureList;
    QPointer<QComboBox> m_ref1Box;
    QPointer<QComboBox> m_ref2Box;
    QPointer<QPushButton> m_pick1;
    QPointer<QPushButton> m_pick2;
    QPointer<QLabel> m_message;
};

}  // namespace silex::ui
