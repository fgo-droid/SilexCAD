#include "PrintTools.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

#include "Icons.h"
#include "ModelController.h"
#include "Viewport.h"

namespace silex::ui {

namespace {
const QColor kWarn(0xf0, 0x9a, 0x30), kFail(0xff, 0x40, 0x30), kThin(0xd0, 0x40, 0xff);
const Vec3 kDirs[6] = {{0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {-1, 0, 0}};
}  // namespace

PrintabilityTool::PrintabilityTool(Workspace& ws) : EditSession(ws)
{
    m_bodies = ws.selectedBodies();
    ws.clearSelection();
    compute();
}

PrintabilityTool::~PrintabilityTool() { m_ws.viewport()->setOverlayMeshes({}); }

void PrintabilityTool::compute()
{
    m_opt.buildDir = kDirs[m_dirIndex];
    m_total = {};
    m_total.minThickness = -1;
    std::vector<Viewport::OverlayMesh> meshes;
    if (auto snap = m_ws.model()->snapshot())
        for (const auto& b : snap->bodies) {
            if (m_ws.bodyHidden(b.id) || b.locked())  // reference components are not printed
                continue;
            if (!m_bodies.empty() && std::find(m_bodies.begin(), m_bodies.end(), b.id) == m_bodies.end())
                continue;
            const PrintabilityResult r = analyzePrintability(b.mesh, m_opt);
            m_total.area += r.area;
            m_total.warnArea += r.warnArea;
            m_total.failArea += r.failArea;
            m_total.thinArea += r.thinArea;
            if (r.minThickness >= 0 && (m_total.minThickness < 0 || r.minThickness < m_total.minThickness))
                m_total.minThickness = r.minThickness;
            std::vector<bool> warn(r.overhang.size()), fail(r.overhang.size()), thin(r.thin.size());
            for (size_t i = 0; i < r.overhang.size(); ++i) {
                warn[i] = r.overhang[i] == TriClass::Warn;
                fail[i] = r.overhang[i] == TriClass::Fail;
                thin[i] = r.thin[i] != 0;
            }
            for (auto [keep, col, lift] : {std::tuple{warn, kWarn, 0.02}, std::tuple{fail, kFail, 0.02}, std::tuple{thin, kThin, 0.04}}) {
                RenderMesh m = extractTriangles(b.mesh, keep, lift);
                if (!m.indices.empty())
                    meshes.push_back({std::move(m), col, 0.85f});
            }
        }
    m_ws.viewport()->setOverlayMeshes(std::move(meshes));
    if (m_total.area > 0 && m_total.failArea < 1e-6)
        m_ws.model()->noteEvent("printability_ok");  // lesson criterion: no overhang needing supports
    updatePanel();
    m_ws.viewport()->refreshOverlay();
}

void PrintabilityTool::onPicked(const SelItem& item, bool)
{
    if (!item.valid() || item.owner.empty())
        return;
    auto it = std::find(m_bodies.begin(), m_bodies.end(), item.owner);
    if (it != m_bodies.end())
        m_bodies.erase(it);
    else
        m_bodies.push_back(item.owner);
    compute();
}

QWidget* PrintabilityTool::createPanel(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* lay = new QVBoxLayout(w);
    auto* title = new QLabel(QObject::tr("<b>Printability (FDM)</b>"), w);
    title->setStyleSheet("font-size: 15px;");
    lay->addWidget(title);
    auto* form = new QFormLayout();
    auto* dir = new QComboBox(w);
    dir->addItems({QObject::tr("+Z (plate = XY)"), QObject::tr("-Z (upside down)"), "+Y", "-Y", "+X", "-X"});
    dir->setCurrentIndex(m_dirIndex);
    dir->setToolTip(QObject::tr("Build direction: the normal of the printer plate"));
    QObject::connect(dir, &QComboBox::currentIndexChanged, dir, [this](int i) {
        m_dirIndex = i;
        compute();
    });
    form->addRow(QObject::tr("Build direction"), dir);
    auto spin = [&](double v, double lo, double hi, const QString& suffix, double* target) {
        auto* b = new QDoubleSpinBox(w);
        b->setRange(lo, hi);
        b->setDecimals(2);
        b->setSuffix(suffix);
        b->setValue(v);
        QObject::connect(b, &QDoubleSpinBox::valueChanged, b, [this, target](double x) {
            *target = x;
            compute();
        });
        return b;
    };
    form->addRow(QObject::tr("Orange from"), spin(m_opt.warnAngle, 10, 89, QString(QChar(0xB0)), &m_opt.warnAngle));
    form->addRow(QObject::tr("Red above"), spin(m_opt.failAngle, 10, 89, QString(QChar(0xB0)), &m_opt.failAngle));
    form->addRow(QObject::tr("Minimum wall"), spin(m_opt.minWall, 0.1, 10, " mm", &m_opt.minWall));
    lay->addLayout(form);
    m_label = new QLabel(w);
    m_label->setWordWrap(true);
    m_label->setTextFormat(Qt::RichText);
    lay->addWidget(m_label);
    auto* help = new QLabel(QObject::tr("<small>Overhang angle measured from the vertical: up to 45° prints without "
                                        "supports. Faces lying on the plate are ignored. Minimum wall 0.8 mm = 2 lines "
                                        "of a 0.4 mm nozzle. Click a body to restrict the analysis to it.</small>"),
                            w);
    help->setWordWrap(true);
    help->setStyleSheet("color: #9aa3ad;");
    lay->addWidget(help);
    lay->addStretch(1);
    auto* close = new QPushButton(icon(IconId::Finish), QObject::tr("Close"), w);
    QObject::connect(close, &QPushButton::clicked, close, [ws = &m_ws] { ws->finishSession(); });
    lay->addWidget(close);
    updatePanel();
    return w;
}

void PrintabilityTool::updatePanel()
{
    if (!m_label)
        return;
    auto num = [](double v, int d = 1) { return QLocale().toString(v, 'f', d); };
    auto row = [](const QColor& c, const QString& t, const QString& v) {
        return QString("<tr><td><span style='color:%1'>&#9632;</span> %2</td><td align='right'><b>%3</b></td></tr>")
            .arg(c.name(), t.toHtmlEscaped(), v);
    };
    QString html = "<table width='100%'>";
    html += row(kWarn, QObject::tr("Overhang %1-%2°").arg(num(m_opt.warnAngle, 0), num(m_opt.failAngle, 0)),
                num(m_total.warnArea) + " mm²");
    html += row(kFail, QObject::tr("Overhang > %1° (supports)").arg(num(m_opt.failAngle, 0)), num(m_total.failArea) + " mm²");
    html += row(kThin, QObject::tr("Walls < %1 mm").arg(num(m_opt.minWall, 2)), num(m_total.thinArea) + " mm²");
    html += "</table>";
    if (m_total.minThickness >= 0)
        html += QObject::tr("<br>Thinnest wall: <b>%1 mm</b>").arg(num(m_total.minThickness, 2));
    const bool good = m_total.failArea < 1e-6 && m_total.thinArea < 1e-6;
    html += QString("<br><span style='color:%1'><b>%2</b></span>")
                .arg(good ? "#5cd68a" : "#f09a30",
                     good ? QObject::tr("Printable without supports in this orientation")
                          : QObject::tr("Supports or a thicker wall needed — try another build direction"));
    m_label->setText(html);
}

void PrintabilityTool::paintOverlay(QPainter& p)
{
    QFont f = p.font();
    f.setPixelSize(12);
    f.setBold(true);
    p.setFont(f);
    QRectF box(12, 12, 360, 52);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(20, 24, 30, 200));
    p.drawRoundedRect(box, 6, 6);
    p.setPen(QColor(230, 233, 237));
    p.drawText(box.adjusted(10, 6, -10, -26), Qt::AlignLeft | Qt::AlignVCenter, QObject::tr("Printability (FDM)"));
    f.setBold(false);
    f.setPixelSize(11);
    p.setFont(f);
    p.setPen(QColor(170, 178, 188));
    p.drawText(box.adjusted(10, 28, -10, -4), Qt::AlignLeft | Qt::AlignVCenter,
               QObject::tr("orange 45-60° · red > 60° (supports) · magenta: wall too thin"));
    p.setBrush(Qt::NoBrush);
}

}  // namespace silex::ui
