#include "SectionPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <cmath>

#include "ExprSpinBox.h"
#include "ModelController.h"
#include "Workspace.h"

namespace silex::ui {

SectionPanel::SectionPanel(Workspace* ws, ModelController* model, QWidget* parent)
    : QWidget(parent), m_ws(ws), m_model(model)
{
    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    m_plane = new QComboBox(this);
    m_plane->addItem(tr("Plane XY"), "XY");
    m_plane->addItem(tr("Plane XZ"), "XZ");
    m_plane->addItem(tr("Plane YZ"), "YZ");
    m_plane->addItem(tr("Selected planar face"), "Face");
    m_plane->setCurrentIndex(1);
    form->addRow(tr("Cutting plane"), m_plane);

    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(0, 1000);
    m_offset = new ExprSpinBox(this);
    m_offset->setRange(-1e5, 1e5);
    m_offset->setDecimals(2);
    m_offset->setSuffix(" mm");
    m_offset->setKeyboardTracking(true);
    auto* row = new QHBoxLayout();
    row->addWidget(m_slider, 1);
    row->addWidget(m_offset);
    form->addRow(tr("Offset"), row);
    m_flip = new QCheckBox(tr("Flip"), this);
    form->addRow("", m_flip);
    lay->addLayout(form);
    m_info = new QLabel(this);
    m_info->setWordWrap(true);
    m_info->setStyleSheet("color: #9aa3ad;");
    lay->addWidget(m_info);
    auto* close = new QPushButton(tr("Close section (Alt+S)"), this);
    lay->addWidget(close);

    connect(m_plane, &QComboBox::currentIndexChanged, this, [this](int) {
        updateRange();
        apply();
    });
    connect(m_slider, &QSlider::valueChanged, this, [this](int v) {
        if (m_updating)
            return;
        m_updating = true;
        m_offset->setValue(m_min + (m_max - m_min) * v / 1000.0);
        m_updating = false;
        apply();
    });
    connect(m_offset, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (m_updating)
            return;
        m_updating = true;
        m_slider->setValue(int(std::lround((v - m_min) / std::max(m_max - m_min, 1e-9) * 1000.0)));
        m_updating = false;
        apply();
    });
    connect(m_flip, &QCheckBox::toggled, this, [this] { apply(); });
    connect(close, &QPushButton::clicked, this, [this] { activate(false); });
    connect(m_model, &ModelController::snapshotChanged, this, [this] {
        if (isActive())
            updateRange();
    });
}

bool SectionPanel::isActive() const { return m_ws->section().on; }

void SectionPanel::activate(bool on)
{
    if (on) {
        // a planar face in the selection becomes the cutting plane
        for (const SelItem& s : m_ws->selection())
            if (s.kind == SelKind::Face && s.planar) {
                m_ws->setSectionFace(s);
                m_plane->setCurrentIndex(3);
                break;
            }
        updateRange();
        // start in the middle of the part
        m_updating = true;
        m_offset->setValue((m_min + m_max) / 2);
        m_slider->setValue(500);
        m_updating = false;
        apply();
    }
    else {
        Workspace::SectionState st = m_ws->section();
        st.on = false;
        m_ws->setSection(st);
    }
    if (QWidget* dock = parentWidget())
        dock->setVisible(on);
}

void SectionPanel::updateRange()
{
    auto snap = m_model->snapshot();
    Workspace::SectionState st = m_ws->section();
    st.plane = m_plane->currentData().toString().toStdString();
    const Placement pl = m_ws->sectionBasePlacement(st);
    double lo = -50, hi = 50;
    if (snap && snap->bbox.valid()) {
        lo = 1e300;
        hi = -1e300;
        const BBox& b = snap->bbox;
        for (int i = 0; i < 8; ++i) {
            const Vec3 c{(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z};
            const double t = (c - pl.origin).dot(pl.normal);
            lo = std::min(lo, t);
            hi = std::max(hi, t);
        }
    }
    m_min = lo;
    m_max = hi;
    m_updating = true;
    m_slider->setValue(int(std::lround((m_offset->value() - m_min) / std::max(m_max - m_min, 1e-9) * 1000.0)));
    m_updating = false;
    m_info->setText(st.plane == "Face" && st.face.name.empty()
                        ? tr("Select a planar face in the 3D view, then choose this option again.")
                        : tr("Range %1 … %2 mm. The model and the timeline are not modified.")
                              .arg(m_min, 0, 'f', 1)
                              .arg(m_max, 0, 'f', 1));
}

void SectionPanel::apply()
{
    Workspace::SectionState st = m_ws->section();
    st.on = true;
    st.plane = m_plane->currentData().toString().toStdString();
    if (st.plane == "Face")
        for (const SelItem& s : m_ws->selection())
            if (s.kind == SelKind::Face && s.planar) {
                m_ws->setSectionFace(s);
                st = m_ws->section();
                st.on = true;
                st.plane = "Face";
                break;
            }
    st.offset = m_offset->value();
    st.flip = m_flip->isChecked();
    m_ws->setSection(st);
}

}  // namespace silex::ui
