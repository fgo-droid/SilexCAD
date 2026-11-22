#include "AnalysisTools.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "Icons.h"
#include "ModelController.h"
#include "Theme.h"
#include "Viewport.h"

namespace silex::ui {

namespace {

QString mm(double v, int dec = 3) { return QString::number(v, 'f', dec) + " mm"; }
QString vecText(const Vec3& v, int dec = 3)
{
    return QString("(%1, %2, %3)").arg(v.x, 0, 'f', dec).arg(v.y, 0, 'f', dec).arg(v.z, 0, 'f', dec);
}

const QColor kTight(0xff, 0x9f, 0x2e);
const QColor kOk(0x4c, 0xd9, 0x7b);

QColor statusColor(const std::string& s)
{
    if (s == "collision")
        return Colors::Error;
    if (s == "tight")
        return kTight;
    return kOk;
}

void drawTag(QPainter& p, QPointF at, const QString& text, const QColor& border)
{
    QFont f = p.font();
    f.setPixelSize(12);
    f.setBold(true);
    p.setFont(f);
    const QRectF r = QFontMetricsF(f).boundingRect(text).adjusted(-6, -3, 6, 3).translated(at - QPointF(0, 0));
    const QRectF box(at.x() - r.width() / 2, at.y() - r.height() / 2, r.width(), r.height());
    p.setPen(QPen(border, 1.2));
    p.setBrush(QColor(16, 20, 26, 225));
    p.drawRoundedRect(box, 4, 4);
    p.setPen(QColor(240, 242, 245));
    p.drawText(box, Qt::AlignCenter, text);
    p.setBrush(Qt::NoBrush);
}

void drawHeader(QPainter& p, const QString& title, const QString& hint)
{
    QFont f = p.font();
    f.setPixelSize(12);
    f.setBold(true);
    p.setFont(f);
    const QRectF box(12, 12, 400, 52);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(20, 24, 30, 200));
    p.drawRoundedRect(box, 6, 6);
    p.setPen(QColor(230, 233, 237));
    p.drawText(box.adjusted(10, 6, -10, -26), Qt::AlignLeft | Qt::AlignVCenter, title);
    f.setBold(false);
    f.setPixelSize(11);
    p.setFont(f);
    p.setPen(QColor(170, 178, 188));
    p.drawText(box.adjusted(10, 28, -10, -4), Qt::AlignLeft | Qt::AlignVCenter, hint);
    p.setBrush(Qt::NoBrush);
}

}  // namespace

// ================================================================== MeasureTool

MeasureTool::MeasureTool(Workspace& ws) : EditSession(ws)
{
    // Start from the current selection (one or two faces / edges / bodies).
    for (const SelItem& s : ws.selection())
        if ((s.kind == SelKind::Face || s.kind == SelKind::Edge || s.kind == SelKind::Body) && m_items.size() < 2)
            m_items.push_back(s);
    syncSelection();
    compute();
}

void MeasureTool::onPicked(const SelItem& picked, bool additive)
{
    if (!picked.valid())
        return;
    SelItem item = picked;
    if (m_wholeBodies)
        item = m_ws.bodyItem(picked.owner);
    auto same = std::find_if(m_items.begin(), m_items.end(), [&](const SelItem& s) { return s.sameAs(item); });
    if (same != m_items.end())
        m_items.erase(same);
    else if (m_items.size() >= 2 && !additive)
        m_items = {item};  // a third click starts a new measurement
    else {
        if (m_items.size() >= 2)
            m_items.erase(m_items.begin());
        m_items.push_back(item);
    }
    syncSelection();
    compute();
}

void MeasureTool::syncSelection() { m_ws.setSelection(m_items); }

void MeasureTool::compute()
{
    m_result = {};
    std::vector<MeasureRef> refs;
    for (const SelItem& s : m_items) {
        MeasureRef r;
        r.kind = s.kind == SelKind::Body   ? MeasureRef::Kind::Body
                 : s.kind == SelKind::Edge ? MeasureRef::Kind::Edge
                                           : MeasureRef::Kind::Face;
        r.body = s.owner;
        r.ref = s.ref();
        refs.push_back(r);
    }
    if (!refs.empty())
        m_result = m_ws.model()->engine().measureLast(refs);
    updatePanel();
    m_ws.viewport()->refreshOverlay();
}

void MeasureTool::onSnapshot()
{
    // geometry changed (undo...): re-resolve the persistent references
    std::vector<SelItem> kept;
    for (const SelItem& s : m_items) {
        SelItem r = s.kind == SelKind::Edge ? m_ws.findEdge(s.ref()) : s.kind == SelKind::Body ? m_ws.bodyItem(s.owner) : s;
        if (r.valid())
            kept.push_back(r);
    }
    m_items = kept;
    syncSelection();
    compute();
}

std::vector<MenuAction> MeasureTool::menuActions()
{
    return {
        {QObject::tr("Clear"), icon(IconId::Cancel), [this] {
             m_items.clear();
             syncSelection();
             compute();
         }},
        {QObject::tr("Close"), icon(IconId::Finish), [ws = &m_ws] { ws->finishSession(); }},
    };
}

QWidget* MeasureTool::createPanel(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* lay = new QVBoxLayout(w);
    auto* title = new QLabel(QObject::tr("<b>Measure</b>"), w);
    title->setStyleSheet("font-size: 15px;");
    lay->addWidget(title);
    auto* bodies = new QCheckBox(QObject::tr("Pick whole bodies"), w);
    bodies->setChecked(m_wholeBodies);
    QObject::connect(bodies, &QCheckBox::toggled, bodies, [this](bool on) {
        m_wholeBodies = on;
        m_items.clear();
        syncSelection();
        compute();
    });
    lay->addWidget(bodies);
    m_label = new QLabel(w);
    m_label->setWordWrap(true);
    m_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(m_label);
    lay->addStretch(1);
    auto* clear = new QPushButton(icon(IconId::Cancel), QObject::tr("Clear"), w);
    auto* close = new QPushButton(icon(IconId::Finish), QObject::tr("Close"), w);
    QObject::connect(clear, &QPushButton::clicked, clear, [this] {
        m_items.clear();
        syncSelection();
        compute();
    });
    QObject::connect(close, &QPushButton::clicked, close, [ws = &m_ws] { ws->finishSession(); });
    lay->addWidget(clear);
    lay->addWidget(close);
    updatePanel();
    return w;
}

void MeasureTool::updatePanel()
{
    if (!m_label)
        return;
    if (m_items.empty()) {
        m_label->setText(QObject::tr("Click 1 or 2 faces / edges (Ctrl: add).<br><br>"
                                     "<span style='color:#9aa3ad'>Distance, ΔX ΔY ΔZ, angle between planes or "
                                     "lines, Ø / R / centre of circular edges and cylinders.</span>"));
        return;
    }
    if (!m_result.ok) {
        m_label->setText(QString("<span style='color:%1'>%2</span>")
                             .arg(Colors::Error.name(), QString::fromStdString(m_result.error).toHtmlEscaped()));
        return;
    }
    QString s;
    QStringList types;
    for (const auto& t : m_result.types)
        types << QString::fromStdString(t);
    s += QObject::tr("<b>Entities</b>: %1<br>").arg(types.join(" / "));
    if (m_result.hasDistance) {
        s += QObject::tr("<br><b>Minimal distance</b>: <span style='font-size:15px'>%1</span><br>").arg(mm(m_result.distance));
        s += QObject::tr("ΔX = %1<br>ΔY = %2<br>ΔZ = %3<br>")
                 .arg(mm(std::abs(m_result.delta.x)), mm(std::abs(m_result.delta.y)), mm(std::abs(m_result.delta.z)));
    }
    if (m_result.hasAngle)
        s += QObject::tr("<br><b>Angle</b>: %1°<br>").arg(m_result.angleDeg, 0, 'f', 3);
    for (const auto& c : m_result.circles)
        s += QObject::tr("<br><b>Ø</b> %1 &nbsp; <b>R</b> %2<br>Centre %3<br>")
                 .arg(mm(2 * c.radius), mm(c.radius), vecText(c.center));
    if (m_result.length > 0)
        s += QObject::tr("<br><b>Length</b>: %1<br>").arg(mm(m_result.length));
    if (m_result.area > 0)
        s += QObject::tr("<br><b>Area</b>: %1 mm²<br>").arg(m_result.area, 0, 'f', 3);
    m_label->setText(s);
}

void MeasureTool::paintOverlay(QPainter& p)
{
    drawHeader(p, QObject::tr("Measure"),
               m_items.size() < 2 ? QObject::tr("Click %1 entity(ies) — Esc to close").arg(2 - int(m_items.size()))
                                  : QObject::tr("Click again to start a new measurement"));
    if (!m_result.ok)
        return;
    Viewport* vp = m_ws.viewport();
    // temporary dimension line between the closest points
    if (m_result.hasDistance) {
        QPointF a, b;
        if (vp->toScreen(m_result.p1, &a) && vp->toScreen(m_result.p2, &b)) {
            p.setPen(QPen(QColor(0xff, 0xd0, 0x40), 1.8, Qt::DashLine));
            p.drawLine(a, b);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0xff, 0xd0, 0x40));
            p.drawEllipse(a, 3.5, 3.5);
            p.drawEllipse(b, 3.5, 3.5);
            p.setBrush(Qt::NoBrush);
            QString t = QString::number(m_result.distance, 'f', 3) + " mm";
            if (m_result.hasAngle)
                t += QString("  ∠ %1°").arg(m_result.angleDeg, 0, 'f', 2);
            drawTag(p, (a + b) / 2 + QPointF(0, -14), t, QColor(0xff, 0xd0, 0x40));
        }
    }
    for (const auto& c : m_result.circles) {
        QPointF o;
        if (!vp->toScreen(c.center, &o))
            continue;
        p.setPen(QPen(QColor(0xff, 0x6a, 0x5a), 1.4));
        p.drawLine(o - QPointF(7, 0), o + QPointF(7, 0));
        p.drawLine(o - QPointF(0, 7), o + QPointF(0, 7));
        drawTag(p, o + QPointF(0, -18), QString("Ø %1").arg(2 * c.radius, 0, 'f', 3), QColor(0xff, 0x6a, 0x5a));
    }
}

// ================================================================== InterferenceTool

InterferenceTool::InterferenceTool(Workspace& ws) : EditSession(ws)
{
    m_bodies = ws.selectedBodies();
    if (m_bodies.size() < 2)
        m_bodies.clear();  // all bodies
    m_ws.setGhostBodies(m_xray);
    m_ws.clearSelection();
    compute();
}

InterferenceTool::~InterferenceTool()
{
    m_ws.setGhostBodies(false);
    m_ws.viewport()->setOverlayMeshes({});
}

void InterferenceTool::compute()
{
    m_report = m_ws.model()->engine().checkInterferenceLast(m_bodies, true);
    if (m_report.error.empty() && !m_report.pairs.empty() && !m_report.collision)
        m_ws.model()->noteEvent(std::all_of(m_report.pairs.begin(), m_report.pairs.end(),
                                            [](const InterferencePair& p) { return p.status == "ok"; })
                                    ? "interference_ok"
                                    : "interference_tight");
    std::vector<Viewport::OverlayMesh> meshes;
    for (const auto& pr : m_report.pairs)
        if (pr.status == "collision" && !pr.overlap.indices.empty())
            meshes.push_back({pr.overlap, QColor(0xff, 0x20, 0x20), 0.8f});
    m_ws.viewport()->setOverlayMeshes(std::move(meshes));
    updatePanel();
    m_ws.viewport()->refreshOverlay();
}

void InterferenceTool::onPicked(const SelItem& item, bool)
{
    if (!item.valid())
        return;
    // clicking a body toggles it in the analysed set
    if (m_bodies.empty())
        if (auto snap = m_ws.model()->snapshot())
            for (const auto& b : snap->bodies)
                m_bodies.push_back(b.id);
    auto it = std::find(m_bodies.begin(), m_bodies.end(), item.owner);
    if (it != m_bodies.end())
        m_bodies.erase(it);
    else
        m_bodies.push_back(item.owner);
    fillBodyList();
    compute();
}

void InterferenceTool::onSnapshot()
{
    if (auto snap = m_ws.model()->snapshot())
        std::erase_if(m_bodies, [&](const Uuid& id) {
            return std::none_of(snap->bodies.begin(), snap->bodies.end(), [&](const BodyView& b) { return b.id == id; });
        });
    fillBodyList();
    compute();
}

std::vector<MenuAction> InterferenceTool::menuActions()
{
    return {
        {QObject::tr("All bodies"), icon(IconId::Body), [this] {
             m_bodies.clear();
             fillBodyList();
             compute();
         }},
        {QObject::tr("Close"), icon(IconId::Finish), [ws = &m_ws] { ws->finishSession(); }},
    };
}

void InterferenceTool::fillBodyList()
{
    if (!m_bodyList)
        return;
    m_updating = true;
    m_bodyList->clear();
    if (auto snap = m_ws.model()->snapshot())
        for (const auto& b : snap->bodies) {
            auto* it = new QListWidgetItem(QString::fromStdString(b.name), m_bodyList);
            it->setData(Qt::UserRole, QString::fromStdString(b.id));
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            const bool on = m_bodies.empty() || std::find(m_bodies.begin(), m_bodies.end(), b.id) != m_bodies.end();
            it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
        }
    m_updating = false;
}

QWidget* InterferenceTool::createPanel(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* lay = new QVBoxLayout(w);
    auto* title = new QLabel(QObject::tr("<b>Interference &amp; clearance</b>"), w);
    title->setStyleSheet("font-size: 15px;");
    lay->addWidget(title);
    lay->addWidget(new QLabel(QObject::tr("Bodies analysed (every pair):"), w));
    m_bodyList = new QListWidget(w);
    m_bodyList->setMaximumHeight(140);
    lay->addWidget(m_bodyList);
    QObject::connect(m_bodyList, &QListWidget::itemChanged, m_bodyList, [this](QListWidgetItem*) {
        if (m_updating)
            return;
        m_bodies.clear();
        for (int i = 0; i < m_bodyList->count(); ++i)
            if (m_bodyList->item(i)->checkState() == Qt::Checked)
                m_bodies.push_back(m_bodyList->item(i)->data(Qt::UserRole).toString().toStdString());
        if (m_bodies.size() == size_t(m_bodyList->count()))
            m_bodies.clear();
        compute();
    });
    auto* xray = new QCheckBox(QObject::tr("X-ray view (translucent bodies)"), w);
    xray->setChecked(m_xray);
    QObject::connect(xray, &QCheckBox::toggled, xray, [this](bool on) {
        m_xray = on;
        m_ws.setGhostBodies(on);
    });
    lay->addWidget(xray);
    m_label = new QLabel(w);
    m_label->setWordWrap(true);
    m_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(m_label);
    auto* legend = new QLabel(QString("<small><span style='color:%1'>&#9632;</span> collision &nbsp; "
                                      "<span style='color:%2'>&#9632;</span> 0 &le; clearance &lt; 0.15 mm (tight) &nbsp; "
                                      "<span style='color:%3'>&#9632;</span> &ge; 0.15 mm</small>")
                                  .arg(Colors::Error.name(), kTight.name(), kOk.name()),
                              w);
    legend->setWordWrap(true);
    lay->addWidget(legend);
    lay->addStretch(1);
    auto* close = new QPushButton(icon(IconId::Finish), QObject::tr("Close"), w);
    QObject::connect(close, &QPushButton::clicked, close, [ws = &m_ws] { ws->finishSession(); });
    lay->addWidget(close);
    fillBodyList();
    updatePanel();
    return w;
}

void InterferenceTool::updatePanel()
{
    if (!m_label)
        return;
    if (!m_report.error.empty()) {
        m_label->setText(QString("<span style='color:%1'>%2</span>")
                             .arg(Colors::Warning.name(), QString::fromStdString(m_report.error).toHtmlEscaped()));
        return;
    }
    QString s;
    int collisions = 0;
    for (const auto& pr : m_report.pairs) {
        const QColor c = statusColor(pr.status);
        QString what;
        if (pr.status == "collision") {
            ++collisions;
            what = QObject::tr("collision — %1 mm³").arg(pr.overlapVolume, 0, 'f', 4);
        }
        else
            what = QObject::tr("clearance %1").arg(mm(pr.clearance));
        s += QString("<span style='color:%1'>&#9679;</span> %2 / %3: <b>%4</b><br>")
                 .arg(c.name(), QString::fromStdString(pr.nameA).toHtmlEscaped(),
                      QString::fromStdString(pr.nameB).toHtmlEscaped(), what);
    }
    s += "<br>";
    if (collisions)
        s += QString("<span style='color:%1'><b>%2</b></span><br>")
                 .arg(Colors::Error.name(), QObject::tr("%n collision(s) — shown in red", nullptr, collisions));
    else if (!m_report.pairs.empty())
        s += QObject::tr("No collision. Minimal clearance: <b>%1</b>").arg(mm(m_report.minClearance));
    m_label->setText(s);
}

void InterferenceTool::paintOverlay(QPainter& p)
{
    int collisions = 0;
    for (const auto& pr : m_report.pairs)
        collisions += pr.status == "collision";
    drawHeader(p, QObject::tr("Interference check"),
               collisions ? QObject::tr("%n collision(s) in red — click bodies to include / exclude", nullptr, collisions)
                          : QObject::tr("No collision — clearances shown in the view"));
    Viewport* vp = m_ws.viewport();
    for (const auto& pr : m_report.pairs) {
        if (pr.status == "collision")
            continue;
        // with many bodies only the close pairs are worth a label
        if (m_report.pairs.size() > 3 && pr.status == "ok" && pr.clearance > 5.0)
            continue;
        QPointF a, b;
        if (!vp->toScreen(pr.p1, &a) || !vp->toScreen(pr.p2, &b))
            continue;
        const QColor c = statusColor(pr.status);
        p.setPen(QPen(c, 2));
        p.drawLine(a, b);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(a, 3, 3);
        p.drawEllipse(b, 3, 3);
        p.setBrush(Qt::NoBrush);
        drawTag(p, (a + b) / 2 + QPointF(0, -14), QString::number(pr.clearance, 'f', 3) + " mm", c);
    }
}

}  // namespace silex::ui
