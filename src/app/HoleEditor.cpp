#include "HoleEditor.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <cmath>

#include "ExprSpinBox.h"
#include "Icons.h"
#include "ModelController.h"
#include "Theme.h"
#include "Viewport.h"

namespace silex::ui {

QString HoleEditor::kindLabel(const std::string& kind)
{
    if (kind == "counterbore") return QObject::tr("Counterbore (socket head DIN 912)");
    if (kind == "countersink") return QObject::tr("Countersink 90° (DIN 7991)");
    if (kind == "insert") return QObject::tr("Heat-set insert");
    if (kind == "nut") return QObject::tr("Hex nut trap");
    return QObject::tr("Clearance hole");
}

HoleEditor::HoleEditor(Workspace& ws, FeaturePtr existing) : EditSession(ws), m_existing(std::move(existing))
{
    if (m_existing) {
        m_id = m_existing->id;
        m_name = m_existing->name;
        m_hp = m_existing->as<FastenerHoleParams>();
        m_placed = true;
    }
    else {
        m_id = newUuid();
        m_name = ws.model()->doc().nextName("Hole");
        m_hp.centers.clear();
        // a planar face already selected: the hole starts at its center
        for (const SelItem& s : ws.selection())
            if (s.kind == SelKind::Face && s.planar) {
                m_hp.plane.kind = "Face";
                m_hp.plane.face = s.ref();
                m_hp.centers = {{0, 0}};
                m_placed = true;
                break;
            }
    }
    syncSelection();
    pushPreview();
}

HoleEditor::~HoleEditor() = default;

uint32_t HoleEditor::pickFilter() const { return SelFilter::Faces | SelFilter::PlanarOnly; }

std::optional<Placement> HoleEditor::plane() const
{
    const SketchPlaneRef& r = m_hp.plane;
    if (r.kind == "XY" || r.kind == "XZ" || r.kind == "YZ")
        return offsetPlacement(originPlacement(r.kind), r.offset);
    auto snap = m_ws.model()->snapshot();
    if (!snap)
        return std::nullopt;
    if (r.kind == "Plane") {
        for (const auto& pv : snap->planes)
            if (pv.id == r.planeId)
                return offsetPlacement(pv.plane, r.offset);
        return std::nullopt;
    }
    for (const auto& b : snap->bodies)
        for (const auto& f : b.mesh.faces)
            if (f.name == r.face.name && f.planar) {
                // same frame as the kernel: face centroid, X = world X projected on the face
                Placement pl = f.plane;
                pl.origin = f.sig.center;
                const Vec3 n = pl.normal.normalized();
                Vec3 x = Vec3{1, 0, 0} - n * n.x;
                if (x.length() < 1e-6)
                    x = Vec3{0, 1, 0} - n * n.y;
                pl.xDir = x.normalized();
                return offsetPlacement(pl, r.offset);
            }
    return std::nullopt;
}

std::optional<Vec2> HoleEditor::snapped(QPointF px, QString* what) const
{
    const auto pl = plane();
    if (!pl)
        return std::nullopt;
    const auto hit = m_ws.viewport()->rayPlane(px, *pl);
    if (!hit)
        return std::nullopt;
    Vec2 local = pl->toLocal(*hit);
    // snap candidates (world points), the closest within 12 px wins
    double best = 12.0;
    std::optional<Vec2> snap;
    auto consider = [&](const Vec3& w, const QString& label) {
        const Vec3 onPlane = w - pl->normal.normalized() * (w - pl->origin).dot(pl->normal.normalized());
        QPointF s;
        if (!m_ws.viewport()->toScreen(onPlane, &s))
            return;
        const double d = std::hypot(s.x() - px.x(), s.y() - px.y());
        if (d < best) {
            best = d;
            snap = pl->toLocal(onPlane);
            if (what)
                *what = label;
        }
    };
    consider(pl->origin, QObject::tr("face center"));
    if (auto ss = m_ws.model()->snapshot()) {
        const Vec3 n = pl->normal.normalized();
        for (const auto& b : ss->bodies)
            for (const auto& f : b.mesh.faces)
                // cylinder (GeomAbs_Cylinder) whose axis is along the hole: its centroid lies on the
                // axis, its signature normal (taken mid-face) is perpendicular to it
                if (f.sig.type == 1 && std::abs(f.sig.dir.normalized().dot(n)) < 0.1)
                    consider(f.sig.center, b.locked() ? QObject::tr("mounting hole of %1").arg(QString::fromStdString(b.name))
                                                      : QObject::tr("hole axis"));
        for (const auto& sv : ss->sketches)
            if (std::abs(sv.plane.normal.normalized().dot(n)) > 0.999)
                for (const auto& g : sv.sketch.geos) {
                    if (g.construction && g.kind != GeoKind::Point)
                        continue;
                    if (g.kind == GeoKind::Circle || g.kind == GeoKind::Arc)
                        consider(sv.plane.toWorld(g.c), QObject::tr("sketch center"));
                    else if (g.kind == GeoKind::Point)
                        consider(sv.plane.toWorld(g.p1), QObject::tr("sketch point"));
                    else if (g.kind == GeoKind::Line) {
                        consider(sv.plane.toWorld(g.p1), QObject::tr("sketch point"));
                        consider(sv.plane.toWorld(g.p2), QObject::tr("sketch point"));
                    }
                }
    }
    if (snap)
        return snap;
    if (m_ws.gridSnap()) {
        const double s = m_ws.gridStep();
        local = {std::round(local.x / s) * s, std::round(local.y / s) * s};
        if (what)
            *what = QObject::tr("grid");
    }
    else if (what)
        what->clear();
    return local;
}

bool HoleEditor::mousePress(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return false;
    const SelItem hit = m_ws.viewport()->pickAt(e->position(), SelFilter::Faces | SelFilter::PlanarOnly);
    if (hit.kind == SelKind::Face) {
        if (!hit.planar) {
            m_ws.status(QObject::tr("The hole tool needs a planar face"));
            return true;
        }
        const bool add = e->modifiers() & Qt::ControlModifier;
        if (m_hp.plane.kind != "Face" || m_hp.plane.face.name != hit.name) {
            m_hp.plane = SketchPlaneRef{};
            m_hp.plane.kind = "Face";
            m_hp.plane.face = hit.ref();
            m_hp.centers.clear();
        }
        if (auto c = snapped(e->position(), nullptr)) {
            if (add || m_hp.centers.empty()) {
                m_hp.centers.push_back(*c);
                m_active = int(m_hp.centers.size()) - 1;
            }
            else
                m_hp.centers[size_t(std::clamp(m_active, 0, int(m_hp.centers.size()) - 1))] = *c;
        }
        m_placed = true;
        syncSelection();
        updatePanel();
        pushPreview();
        return true;
    }
    return false;  // empty space: orbit / pan as usual
}

bool HoleEditor::mouseMove(QMouseEvent* e)
{
    m_hoverPx = e->position();
    snapped(e->position(), &m_snapWhat);
    m_ws.viewport()->refreshOverlay();
    return false;
}

void HoleEditor::paintOverlay(QPainter& p)
{
    const auto pl = plane();
    if (!pl)
        return;
    p.setRenderHint(QPainter::Antialiasing);
    for (size_t i = 0; i < m_hp.centers.size(); ++i) {
        QPointF s;
        if (!m_ws.viewport()->toScreen(pl->toWorld(m_hp.centers[i]), &s))
            continue;
        p.setPen(QPen(int(i) == m_active ? Colors::SketchSelected : Colors::Hover, 2));
        p.drawLine(s + QPointF(-8, 0), s + QPointF(8, 0));
        p.drawLine(s + QPointF(0, -8), s + QPointF(0, 8));
        p.drawEllipse(s, 4, 4);
    }
    if (m_hoverPx && !m_snapWhat.isEmpty()) {
        if (auto c = snapped(*m_hoverPx, nullptr)) {
            QPointF s;
            if (m_ws.viewport()->toScreen(pl->toWorld(*c), &s)) {
                p.setPen(QPen(Colors::Hover, 1.5));
                p.setBrush(Qt::NoBrush);
                p.drawRect(QRectF(s - QPointF(5, 5), QSizeF(10, 10)));
                p.drawText(s + QPointF(10, -8), m_snapWhat);
            }
        }
    }
}

FeaturePtr HoleEditor::buildNode() const
{
    auto n = m_existing ? std::make_shared<FeatureNode>(*m_existing) : std::make_shared<FeatureNode>();
    n->id = m_id;
    n->type = FeatureType::FastenerHole;
    n->name = m_name;
    n->params = Json(m_hp);
    return n;
}

void HoleEditor::pushPreview()
{
    DocState d = m_ws.model()->doc();
    if (!ready()) {
        if (m_existing)
            d.marker = d.indexOf(m_id);
        m_ws.model()->preview(d, {});
        return;
    }
    FeaturePtr node = buildNode();
    if (m_existing) {
        d = UpdateFeatureCommand(node).apply(d);
        d.marker = d.indexOf(m_id) + 1;
    }
    else
        d = AddFeatureCommand(node).apply(d);
    m_ws.model()->preview(d, m_id);
}

void HoleEditor::syncSelection()
{
    std::vector<SelItem> sel;
    if (m_hp.plane.kind == "Face")
        if (auto snap = m_ws.model()->snapshot())
            for (size_t b = 0; b < snap->bodies.size(); ++b) {
                const auto& faces = snap->bodies[b].mesh.faces;
                for (size_t f = 0; f < faces.size(); ++f)
                    if (faces[f].name == m_hp.plane.face.name) {
                        SelItem s;
                        s.kind = SelKind::Face;
                        s.owner = snap->bodies[b].id;
                        s.object = int(b);
                        s.index = int(f);
                        s.name = faces[f].name;
                        s.sig = faces[f].sig;
                        s.planar = true;
                        s.plane = faces[f].plane;
                        sel.push_back(s);
                    }
            }
    m_ws.setSelection(std::move(sel));
}

void HoleEditor::onSnapshot()
{
    updatePanel();
}

bool HoleEditor::commit()
{
    if (!ready()) {
        m_ws.status(QObject::tr("Click a planar face to place the hole"));
        return false;
    }
    FeaturePtr node = buildNode();
    if (m_existing)
        m_ws.model()->execute(UpdateFeatureCommand(node));
    else
        m_ws.model()->execute(AddFeatureCommand(node));
    return true;
}

std::vector<MenuAction> HoleEditor::menuActions()
{
    std::vector<MenuAction> a{
        {QObject::tr("OK"), icon(IconId::Finish), [ws = &m_ws] { ws->finishSession(); }},
        {QObject::tr("Cancel"), icon(IconId::Cancel), [ws = &m_ws] { ws->cancelSession(); }},
    };
    if (m_hp.centers.size() > 1)
        a.push_back({QObject::tr("Remove the last hole"), icon(IconId::Cancel), [this] {
                         m_hp.centers.pop_back();
                         m_active = std::min(m_active, int(m_hp.centers.size()) - 1);
                         updatePanel();
                         pushPreview();
                     }});
    return a;
}

QWidget* HoleEditor::createPanel(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* lay = new QVBoxLayout(w);
    auto* title = new QLabel(QString("<b>%1</b>").arg(QString::fromStdString(m_name)), w);
    title->setStyleSheet("font-size: 15px;");
    lay->addWidget(title);
    auto* form = new QFormLayout();

    m_kindBox = new QComboBox(w);
    for (const auto& k : fastenerKinds())
        m_kindBox->addItem(kindLabel(k), QString::fromStdString(k));
    form->addRow(QObject::tr("Type"), m_kindBox);
    m_sizeBox = new QComboBox(w);
    for (const auto& s : fastenerSizes())
        m_sizeBox->addItem(QString::fromStdString(s), QString::fromStdString(s));
    form->addRow(QObject::tr("Size"), m_sizeBox);
    m_fitBox = new QComboBox(w);
    m_fitBox->addItem(QObject::tr("Close (+0.2 mm)"), "close");
    m_fitBox->addItem(QObject::tr("Normal (+0.4 mm)"), "normal");
    form->addRow(QObject::tr("Fit"), m_fitBox);

    auto spin = [&](double lo, double hi, const QString& suffix) {
        auto* b = new ExprSpinBox(w);
        b->setRange(lo, hi);
        b->setDecimals(2);
        b->setSuffix(suffix);
        b->setKeyboardTracking(true);
        return b;
    };
    m_depthBox = spin(0, 1e4, " mm");
    m_depthBox->setSpecialValueText(QObject::tr("Through all"));
    form->addRow(QObject::tr("Depth"), m_depthBox);
    m_nutBox = spin(0, 2, " mm");
    form->addRow(QObject::tr("Nut clearance"), m_nutBox);
    m_angleBox = spin(-360, 360, QString::fromUtf8("°"));
    form->addRow(QObject::tr("Nut angle"), m_angleBox);
    m_xBox = spin(-1e5, 1e5, " mm");
    m_yBox = spin(-1e5, 1e5, " mm");
    form->addRow(QObject::tr("Center X"), m_xBox);
    form->addRow(QObject::tr("Center Y"), m_yBox);
    lay->addLayout(form);

    m_summary = new QLabel(w);
    m_summary->setWordWrap(true);
    m_summary->setStyleSheet("color: #9fd3ff;");
    lay->addWidget(m_summary);
    m_where = new QLabel(w);
    m_where->setWordWrap(true);
    m_where->setStyleSheet("color: #a8b0ba;");
    lay->addWidget(m_where);
    m_message = new QLabel(w);
    m_message->setWordWrap(true);
    lay->addWidget(m_message);
    lay->addStretch(1);

    auto changed = [this] {
        if (m_updating)
            return;
        m_hp.kind = m_kindBox->currentData().toString().toStdString();
        m_hp.size = m_sizeBox->currentData().toString().toStdString();
        m_hp.fit = m_fitBox->currentData().toString().toStdString();
        m_hp.depth = m_depthBox->value();
        m_hp.nutClearance = m_nutBox->value();
        m_hp.angle = m_angleBox->value();
        if (m_active >= 0 && m_active < int(m_hp.centers.size()))
            m_hp.centers[size_t(m_active)] = {m_xBox->value(), m_yBox->value()};
        updatePanel();
        pushPreview();
    };
    for (QComboBox* b : {m_kindBox.data(), m_sizeBox.data(), m_fitBox.data()})
        QObject::connect(b, &QComboBox::currentIndexChanged, w, changed);
    for (ExprSpinBox* b : {m_depthBox.data(), m_nutBox.data(), m_angleBox.data(), m_xBox.data(), m_yBox.data()})
        QObject::connect(b, &QDoubleSpinBox::valueChanged, w, changed);
    updatePanel();
    return w;
}

void HoleEditor::updatePanel()
{
    if (!m_kindBox)
        return;
    m_updating = true;
    m_kindBox->setCurrentIndex(std::max(0, m_kindBox->findData(QString::fromStdString(m_hp.kind))));
    m_sizeBox->setCurrentIndex(std::max(0, m_sizeBox->findData(QString::fromStdString(m_hp.size))));
    m_fitBox->setCurrentIndex(std::max(0, m_fitBox->findData(QString::fromStdString(m_hp.fit))));
    m_fitBox->setEnabled(m_hp.kind != "insert");
    m_depthBox->setValue(m_hp.depth);
    m_nutBox->setValue(m_hp.nutClearance);
    m_angleBox->setValue(m_hp.angle);
    m_nutBox->setEnabled(m_hp.kind == "nut");
    m_angleBox->setEnabled(m_hp.kind == "nut");
    const bool has = m_active >= 0 && m_active < int(m_hp.centers.size());
    m_xBox->setEnabled(has);
    m_yBox->setEnabled(has);
    if (has) {
        m_xBox->setValue(m_hp.centers[size_t(m_active)].x);
        m_yBox->setValue(m_hp.centers[size_t(m_active)].y);
    }
    m_updating = false;

    if (const auto d = fastenerHoleDims(m_hp))
        m_summary->setText(QString::fromStdString(d->summary));
    m_where->setText(!m_placed ? QObject::tr("Click a planar face to place the hole (snaps to the face center, sketch "
                                             "points, hole axes and the grid).")
                               : QObject::tr("%n hole(s). Click to move the hole, Ctrl+click to add another one.", "",
                                             int(m_hp.centers.size())));
    QString msg;
    if (auto snap = m_ws.model()->snapshot())
        for (const auto& r : snap->results)
            if (r.id == m_id && !r.message.empty())
                msg = QString::fromStdString(r.message);
    m_message->setText(msg.isEmpty() ? QString() : QString("<span style='color:#ffb454'>%1</span>").arg(msg.toHtmlEscaped()));
}

}  // namespace silex::ui
