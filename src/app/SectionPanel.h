#pragma once
// v0.3 Lot 1.3 — dynamic section view (Alt+S). Purely a display state: the viewport discards the
// material beyond the cutting plane in the shaders and paints the cut material (seen through the
// cut) with a solid colour. The B-Rep model and the timeline are never modified.

#include <QWidget>

class QComboBox;
class QCheckBox;
class QSlider;
class QLabel;

namespace silex::ui {

class Workspace;
class ModelController;
class ExprSpinBox;

class SectionPanel : public QWidget
{
    Q_OBJECT
public:
    SectionPanel(Workspace* ws, ModelController* model, QWidget* parent = nullptr);

    void activate(bool on);
    bool isActive() const;

private:
    void apply();
    void updateRange();

    Workspace* m_ws;
    ModelController* m_model;
    QComboBox* m_plane = nullptr;
    QSlider* m_slider = nullptr;
    ExprSpinBox* m_offset = nullptr;
    QCheckBox* m_flip = nullptr;
    QLabel* m_info = nullptr;
    double m_min = -100, m_max = 100;
    bool m_updating = false;
};

}  // namespace silex::ui
