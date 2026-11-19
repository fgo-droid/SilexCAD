#include "SketchEditor.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

#include "Icons.h"
#include "ModelController.h"
#include "Theme.h"
#include "Viewport.h"

namespace silex::ui {

namespace {

QVector3D toQ(const Vec3& v) { return {float(v.x), float(v.y), float(v.z)}; }

double segDistPx(QPointF p, QPointF a, QPointF b)
{
    const QPointF d = b - a;
    const double l2 = d.x() * d.x() + d.y() * d.y();
    double t = l2 > 1e-12 ? ((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / l2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF f = a + d * t;
    return std::hypot(p.x() - f.x(), p.y() - f.y());
}

/// QLineEdit that reports Escape (cancel) and Enter (accept).
class ValueEdit final : public QLineEdit
{
public:
    std::function<void(bool)> done;
    using QLineEdit::QLineEdit;

protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Escape) {
            if (done)
                done(false);
            return;
        }
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            if (done)
                done(true);
            return;
        }
        QLineEdit::keyPressEvent(e);
    }
};

SketchConstraint makeC(ConstraintKind k, GeoRef a, GeoRef b = {})
{
    SketchConstraint c;
    c.id = newUuid();
    c.kind = k;
    c.a = a;
    c.b = b;
    return c;
}

bool isLine(const Sketch& sk, const Uuid& id)
{
    auto g = sk.geoOrReserved(id);
    return g && g->kind == GeoKind::Line;
}

std::vector<PointPos> pointsOf(const SketchGeo& g)
{
    switch (g.kind) {
        case GeoKind::Line: return {PointPos::Start, PointPos::End};
        case GeoKind::Circle: return {PointPos::Center};
        case GeoKind::Arc: return {PointPos::Start, PointPos::End, PointPos::Center};
        case GeoKind::Point: return {PointPos::Start};
    }
    return {};
}

}  // namespace

// ------------------------------------------------------------------ lifecycle

SketchEditor::SketchEditor(Workspace& ws, FeaturePtr existing, SketchPlaneRef planeRef, Placement plane)
    : EditSession(ws), m_existing(std::move(existing)), m_planeRef(std::move(planeRef)), m_plane(plane)
{
    if (m_existing) {
        m_id = m_existing->id;
        m_name = m_existing->name;
        m_sketch = m_existing->as<SketchParams>().sketch;
    }
    else {
        m_id = newUuid();
        m_name = ws.model()->doc().nextName("Sketch");
    }
    solve();

    // Look at the sketch plane (keeping the current target projected on the plane).
    Viewport* vp = ws.viewport();
    const QVector3D n = toQ(m_plane.normal), o = toQ(m_plane.origin);
    QVector3D t = vp->camera().target;
    t -= n * QVector3D::dotProduct(t - o, n);
    vp->animateTo(Camera::orientationFor(m_plane), t);
}

SketchEditor::~SketchEditor()
{
    if (m_edit)
        m_edit->deleteLater();
}

QString SketchEditor::title() const { return QString::fromStdString(m_name); }

bool SketchEditor::commit()
{
    if (m_edit)
        closeValueEditor(true);
    if (!m_existing && m_sketch.geos.empty())
        return true;  // nothing drawn: nothing to add
    SketchParams p;
    p.plane = m_planeRef;
    p.sketch = m_sketch;
    auto node = std::make_shared<FeatureNode>();
    node->id = m_id;
    node->type = FeatureType::Sketch;
    node->name = m_name;
    node->params = Json(p);
    node->suppressed = m_existing ? m_existing->suppressed : false;
    if (m_existing)
        m_ws.model()->execute(UpdateFeatureCommand(node));
    else
        m_ws.model()->execute(AddFeatureCommand(node));
    m_ws.setLastSketch(m_id);
    return true;
}

void SketchEditor::cancel()
{
    if (m_edit)
        closeValueEditor(false);
}

bool SketchEditor::undo()
{
    if (m_undo.empty())
        return true;
    m_redo.push_back(m_sketch);
    m_sketch = m_undo.back();
    m_undo.pop_back();
    m_selPoints.clear();
    m_selCurves.clear();
    m_selConstraints.clear();
    solve();
    changed();
    return true;
}

bool SketchEditor::redo()
{
    if (m_redo.empty())
        return true;
    m_undo.push_back(m_sketch);
    m_sketch = m_redo.back();
    m_redo.pop_back();
    solve();
    changed();
    return true;
}

// ------------------------------------------------------------------ helpers

std::optional<Vec2> SketchEditor::toLocal(QPointF px) const
{
    auto w = m_ws.viewport()->rayPlane(px, m_plane);
    if (!w)
        return std::nullopt;
    return m_plane.toLocal(*w);
}

bool SketchEditor::toScreen(Vec2 p, QPointF* out) const { return m_ws.viewport()->toScreen(m_plane.toWorld(p), out); }

double SketchEditor::modelPerPixel() const
{
    const QSize s = m_ws.viewport()->size();
    const QPointF c(s.width() * 0.5, s.height() * 0.5);
    auto a = toLocal(c), b = toLocal(c + QPointF(10, 0));
    if (!a || !b)
        return 0.1;
    return std::max((*b - *a).length() / 10.0, 1e-6);
}

void SketchEditor::snapshotUndo()
{
    m_undo.push_back(m_sketch);
    if (m_undo.size() > 200)
        m_undo.erase(m_undo.begin());
    m_redo.clear();
}

void SketchEditor::solve(bool diagnose) { m_status = SketchSolver::solve(m_sketch, std::nullopt, diagnose); }

void SketchEditor::changed()
{
    updatePanel();
    m_ws.viewport()->refreshOverlay();
}

void SketchEditor::addGeo(const SketchGeo& g) { m_sketch.geos.push_back(g); }

void SketchEditor::addConstraint(SketchConstraint c)
{
    if (c.id.empty())
        c.id = newUuid();
    m_sketch.constraints.push_back(std::move(c));
}

void SketchEditor::addSnapConstraints(const Snap& s, const GeoRef& p)
{
    for (auto& c : constraintsFromSnap(s, p))
        addConstraint(c);
}

void SketchEditor::resetTool()
{
    m_clicks.clear();
    m_clickSnaps.clear();
    m_chain.reset();
    m_dimFirst.reset();
    m_hint = LineHint::None;
}

void SketchEditor::setTool(Tool t)
{
    if (m_edit)
        closeValueEditor(true);
    m_tool = t;
    resetTool();
    changed();
}

QColor SketchEditor::colorOf(const SketchGeo& g) const
{
    if (m_status.state == SketchDofState::Failed)
        return Colors::SketchOver;
    for (const auto& c : m_sketch.constraints)
        if ((c.a.geo == g.id || c.b.geo == g.id) && (m_status.conflicting.count(c.id) || m_status.redundant.count(c.id)))
            return Colors::SketchOver;
    if (g.construction)
        return Colors::SketchConstruction;
    return m_status.fullyConstrainedGeos.count(g.id) ? Colors::SketchFull : Colors::SketchUnder;
}

/// Solves after an edit; refuses changes that over-constrain the sketch (Fusion behaviour),
/// dropping optional (inferred) constraints first.
static bool solveOrRevert(Sketch& sk, const Sketch& before, SolveResult& status, std::vector<Uuid> optional,
                          QString* why)
{
    status = SketchSolver::solve(sk);
    while ((status.state == SketchDofState::OverConstrained || status.state == SketchDofState::Failed) &&
           !optional.empty()) {
        sk.removeConstraint(optional.back());
        optional.pop_back();
        status = SketchSolver::solve(sk);
    }
    if (status.state == SketchDofState::OverConstrained || status.state == SketchDofState::Failed) {
        if (why)
            *why = status.state == SketchDofState::Failed ? QObject::tr("The sketch cannot be solved with this change")
                                                          : QObject::tr("This would over-constrain the sketch");
        sk = before;
        status = SketchSolver::solve(sk);
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ hit testing

SketchEditor::Hit SketchEditor::hitTest(QPointF px) const
{
    Hit h;
    for (const auto& [r, id] : m_labelRects)
        if (r.adjusted(-3, -3, 3, 3).contains(px)) {
            h.kind = Hit::Constraint;
            h.constraint = id;
            return h;
        }
    double best = 8.0;
    auto tryPoint = [&](const Uuid& id, PointPos pos, Vec2 p) {
        QPointF s;
        if (!toScreen(p, &s))
            return;
        const double d = std::hypot(s.x() - px.x(), s.y() - px.y());
        if (d < best) {
            best = d;
            h.kind = Hit::Point;
            h.ref = {id, pos};
        }
    };
    tryPoint(kOriginId, PointPos::Start, {0, 0});
    for (const SketchGeo& g : m_sketch.geos)
        for (PointPos pos : pointsOf(g))
            tryPoint(g.id, pos, g.point(pos));
    if (h.kind == Hit::Point)
        return h;

    best = 6.0;
    for (const SketchGeo& g : m_sketch.geos) {
        double d = 1e9;
        if (g.kind == GeoKind::Line) {
            QPointF a, b;
            if (toScreen(g.p1, &a) && toScreen(g.p2, &b))
                d = segDistPx(px, a, b);
        }
        else if (g.kind == GeoKind::Circle || g.kind == GeoKind::Arc) {
            const double a0 = g.kind == GeoKind::Arc ? g.a0 : 0, a1 = g.kind == GeoKind::Arc ? g.a1 : 2 * kPi;
            QPointF prev;
            bool havePrev = false;
            for (int k = 0; k <= 64; ++k) {
                const double a = a0 + (a1 - a0) * k / 64;
                QPointF q;
                if (!toScreen(g.c + Vec2{std::cos(a), std::sin(a)} * g.r, &q))
                    continue;
                if (havePrev)
                    d = std::min(d, segDistPx(px, prev, q));
                prev = q;
                havePrev = true;
            }
        }
        if (d < best) {
            best = d;
            h.kind = Hit::Curve;
            h.ref = {g.id, PointPos::None};
        }
    }
    return h;
}

// ------------------------------------------------------------------ tools

void SketchEditor::clickLine(Vec2)
{
    if (m_clicks.empty()) {
        m_clicks = {m_snap.pos};
        m_clickSnaps = {m_snap};
        return;
    }
    const Vec2 a = m_clicks[0];
    const Vec2 b = m_snap.point ? m_snap.pos : applyHint(a, m_snap.pos, m_hint);
    if ((b - a).length() < 1e-6)
        return;
    const Sketch before = m_sketch;
    snapshotUndo();
    SketchGeo g;
    g.id = newUuid();
    g.kind = GeoKind::Line;
    g.p1 = a;
    g.p2 = b;
    addGeo(g);
    addSnapConstraints(m_clickSnaps[0], {g.id, PointPos::Start});
    if (!m_snap.point || m_hint == LineHint::None)
        addSnapConstraints(m_snap, {g.id, PointPos::End});
    std::vector<Uuid> optional;
    if (m_hint != LineHint::None && !m_snap.point) {
        auto c = makeC(m_hint == LineHint::Horizontal ? ConstraintKind::Horizontal : ConstraintKind::Vertical,
                       {g.id, PointPos::None});
        optional.push_back(c.id);
        addConstraint(c);
    }
    if (m_clickSnaps[0].point)
        if (auto arc = inferTangentAtPoint(m_sketch, *m_clickSnaps[0].point, b - a)) {
            auto c = makeC(ConstraintKind::Tangent, {g.id, PointPos::None}, {*arc, PointPos::None});
            optional.push_back(c.id);
            addConstraint(c);
        }
    QString why;
    if (!solveOrRevert(m_sketch, before, m_status, optional, &why)) {
        m_undo.pop_back();
        m_ws.status(why);
        return;
    }
    if (m_snap.point) {  // closed onto an existing point: end of the polyline
        resetTool();
        return;
    }
    const SketchGeo* placed = m_sketch.find(g.id);
    const Vec2 end = placed ? placed->p2 : b;
    m_clicks = {end};
    m_clickSnaps = {Snap{end, GeoRef{g.id, PointPos::End}, std::nullopt}};
    m_chain = GeoRef{g.id, PointPos::End};
}

void SketchEditor::clickRect(Vec2)
{
    if (m_clicks.empty()) {
        m_clicks = {m_snap.pos};
        m_clickSnaps = {m_snap};
        return;
    }
    const Vec2 a = m_clicks[0], c = m_snap.pos;
    if (std::abs(c.x - a.x) < 1e-6 || std::abs(c.y - a.y) < 1e-6)
        return;
    const Sketch before = m_sketch;
    snapshotUndo();
    const Vec2 p[4] = {a, {c.x, a.y}, c, {a.x, c.y}};
    Uuid ids[4];
    for (int i = 0; i < 4; ++i) {
        SketchGeo g;
        g.id = ids[i] = newUuid();
        g.kind = GeoKind::Line;
        g.p1 = p[i];
        g.p2 = p[(i + 1) % 4];
        addGeo(g);
    }
    for (int i = 0; i < 4; ++i)
        addConstraint(makeC(ConstraintKind::Coincident, {ids[i], PointPos::End}, {ids[(i + 1) % 4], PointPos::Start}));
    addConstraint(makeC(ConstraintKind::Horizontal, {ids[0]}));
    addConstraint(makeC(ConstraintKind::Horizontal, {ids[2]}));
    addConstraint(makeC(ConstraintKind::Vertical, {ids[1]}));
    addConstraint(makeC(ConstraintKind::Vertical, {ids[3]}));
    addSnapConstraints(m_clickSnaps[0], {ids[0], PointPos::Start});
    addSnapConstraints(m_snap, {ids[1], PointPos::End});
    QString why;
    if (!solveOrRevert(m_sketch, before, m_status, {}, &why)) {
        m_undo.pop_back();
        m_ws.status(why);
    }
    resetTool();
}

void SketchEditor::clickCircle(Vec2)
{
    if (m_clicks.empty()) {
        m_clicks = {m_snap.pos};
        m_clickSnaps = {m_snap};
        return;
    }
    const double r = (m_snap.pos - m_clicks[0]).length();
    if (r < 1e-6)
        return;
    const Sketch before = m_sketch;
    snapshotUndo();
    SketchGeo g;
    g.id = newUuid();
    g.kind = GeoKind::Circle;
    g.c = m_clicks[0];
    g.r = r;
    addGeo(g);
    addSnapConstraints(m_clickSnaps[0], {g.id, PointPos::Center});
    if (m_snap.point)
        addConstraint(makeC(ConstraintKind::PointOnObject, *m_snap.point, {g.id, PointPos::None}));
    QString why;
    if (!solveOrRevert(m_sketch, before, m_status, {}, &why)) {
        m_undo.pop_back();
        m_ws.status(why);
    }
    resetTool();
}

void SketchEditor::clickArc(Vec2)
{
    if (m_clicks.size() < 2) {
        m_clicks.push_back(m_snap.pos);
        m_clickSnaps.push_back(m_snap);
        return;
    }
    const Vec2 c = m_clicks[0], s = m_clicks[1], e = m_snap.pos;
    const double r = (s - c).length();
    if (r < 1e-6 || (e - c).length() < 1e-6)
        return;
    const Sketch before = m_sketch;
    snapshotUndo();
    SketchGeo g;
    g.id = newUuid();
    g.kind = GeoKind::Arc;
    g.c = c;
    g.r = r;
    g.a0 = std::atan2(s.y - c.y, s.x - c.x);
    g.a1 = std::atan2(e.y - c.y, e.x - c.x);
    while (g.a1 <= g.a0)
        g.a1 += 2 * kPi;
    addGeo(g);
    addSnapConstraints(m_clickSnaps[0], {g.id, PointPos::Center});
    addSnapConstraints(m_clickSnaps[1], {g.id, PointPos::Start});
    addSnapConstraints(m_snap, {g.id, PointPos::End});
    QString why;
    if (!solveOrRevert(m_sketch, before, m_status, {}, &why)) {
        m_undo.pop_back();
        m_ws.status(why);
    }
    resetTool();
}

void SketchEditor::clickDimension(const Hit& h, Vec2)
{
    auto create = [&](SketchConstraint c) {
        const Sketch before = m_sketch;
        c.value = m_sketch.measure(c);
        snapshotUndo();
        addConstraint(c);
        QString why;
        if (!solveOrRevert(m_sketch, before, m_status, {}, &why)) {
            m_undo.pop_back();
            m_ws.status(why);
            return;
        }
        changed();
        openValueEditor(c.id);
    };

    if (!m_dimFirst) {
        if (h.kind == Hit::Curve) {
            auto g = m_sketch.geoOrReserved(h.ref.geo);
            if (!g)
                return;
            if (g->kind == GeoKind::Circle) {
                create(makeC(ConstraintKind::Diameter, {g->id}));
                return;
            }
            if (g->kind == GeoKind::Arc) {
                create(makeC(ConstraintKind::Radius, {g->id}));
                return;
            }
            m_dimFirst = h;  // a line: length, or angle with a second line
            return;
        }
        if (h.kind == Hit::Point)
            m_dimFirst = h;
        return;
    }
    const Hit first = *m_dimFirst;
    m_dimFirst.reset();
    if (first.kind == Hit::Curve) {
        if (h.kind == Hit::Curve && h.ref.geo != first.ref.geo && isLine(m_sketch, h.ref.geo))
            create(makeC(ConstraintKind::Angle, {first.ref.geo}, {h.ref.geo}));
        else
            create(makeC(ConstraintKind::Distance, {first.ref.geo}));
        return;
    }
    if (first.kind == Hit::Point) {
        if (h.kind == Hit::Point && !(h.ref == first.ref))
            create(makeC(ConstraintKind::Distance, first.ref, h.ref));
        else if (h.kind == Hit::Curve && isLine(m_sketch, h.ref.geo))
            create(makeC(ConstraintKind::Distance, first.ref, {h.ref.geo, PointPos::None}));
    }
}

void SketchEditor::clickSelect(const Hit& h, QMouseEvent* e)
{
    const bool additive = e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
    if (!additive) {
        m_selPoints.clear();
        m_selCurves.clear();
        m_selConstraints.clear();
    }
    auto toggle = [](auto& v, const auto& x) {
        auto it = std::find(v.begin(), v.end(), x);
        if (it != v.end())
            v.erase(it);
        else
            v.push_back(x);
    };
    switch (h.kind) {
        case Hit::Point: toggle(m_selPoints, h.ref); break;
        case Hit::Curve: toggle(m_selCurves, h.ref.geo); break;
        case Hit::Constraint: toggle(m_selConstraints, h.constraint); break;
        default: break;
    }
    if ((h.kind == Hit::Point || h.kind == Hit::Curve) && !isReservedGeo(h.ref.geo)) {
        m_pressed = true;
        m_pressPx = e->position();
        m_dragRef = h.ref;
        if (auto lp = toLocal(e->position())) {
            const SketchGeo* g = m_sketch.find(h.ref.geo);
            const Vec2 ref = g ? (g->kind == GeoKind::Line ? g->p1 : g->c) : Vec2{};
            m_grabOffset = *lp - ref;
        }
    }
}

// ------------------------------------------------------------------ constraint commands

void SketchEditor::applyConstraint(ConstraintKind k)
{
    std::vector<SketchConstraint> add;
    const auto& pts = m_selPoints;
    const auto& crv = m_selCurves;
    switch (k) {
        case ConstraintKind::Horizontal:
        case ConstraintKind::Vertical:
            for (const Uuid& id : crv)
                if (isLine(m_sketch, id))
                    add.push_back(makeC(k, {id}));
            if (add.empty() && pts.size() == 2)
                add.push_back(makeC(k, pts[0], pts[1]));
            break;
        case ConstraintKind::Coincident:
            if (pts.size() == 2)
                add.push_back(makeC(k, pts[0], pts[1]));
            else if (pts.size() == 1 && crv.size() == 1)
                add.push_back(makeC(ConstraintKind::PointOnObject, pts[0], {crv[0]}));
            break;
        case ConstraintKind::Parallel:
        case ConstraintKind::Perpendicular:
        case ConstraintKind::Equal:
        case ConstraintKind::Tangent:
            for (size_t i = 1; i < crv.size(); ++i)
                add.push_back(makeC(k, {crv[0]}, {crv[i]}));
            break;
        case ConstraintKind::Fixed:
            for (const GeoRef& p : pts) {
                auto c = makeC(k, p);
                if (auto v = m_sketch.pointOf(p)) {
                    c.value = v->x;
                    c.value2 = v->y;
                }
                add.push_back(c);
            }
            for (const Uuid& id : crv)
                add.push_back(makeC(k, {id}));
            break;
        default: break;
    }
    if (add.empty()) {
        m_ws.status(QObject::tr("Select suitable geometry first (%1)").arg(toString(k)));
        return;
    }
    const Sketch before = m_sketch;
    snapshotUndo();
    for (auto& c : add)
        addConstraint(c);
    QString why;
    if (!solveOrRevert(m_sketch, before, m_status, {}, &why)) {
        m_undo.pop_back();
        m_ws.status(why);
    }
    changed();
}

void SketchEditor::toggleConstruction()
{
    if (m_selCurves.empty())
        return;
    snapshotUndo();
    for (const Uuid& id : m_selCurves)
        if (SketchGeo* g = m_sketch.find(id))
            g->construction = !g->construction;
    solve();
    changed();
}

void SketchEditor::deleteSelection()
{
    if (m_selCurves.empty() && m_selConstraints.empty() && m_selPoints.empty())
        return;
    snapshotUndo();
    for (const Uuid& id : m_selConstraints)
        m_sketch.removeConstraint(id);
    for (const Uuid& id : m_selCurves)
        m_sketch.removeGeo(id);
    for (const GeoRef& p : m_selPoints)
        if (!isReservedGeo(p.geo))
            m_sketch.removeGeo(p.geo);
    m_selCurves.clear();
    m_selConstraints.clear();
    m_selPoints.clear();
    solve();
    changed();
}

// ------------------------------------------------------------------ value editor

void SketchEditor::openValueEditor(const Uuid& id)
{
    closeValueEditor(false);
    auto it = std::find_if(m_sketch.constraints.begin(), m_sketch.constraints.end(),
                           [&](const SketchConstraint& c) { return c.id == id; });
    if (it == m_sketch.constraints.end() || !isDimensional(it->kind))
        return;
    QPointF at;
    if (!labelAnchor(*it, &at))
        return;
    auto* e = new ValueEdit(m_ws.viewport());
    const bool angle = it->kind == ConstraintKind::Angle;
    const double shown = angle ? it->value * 180.0 / kPi : std::abs(it->value);
    e->setText(QLocale::c().toString(shown, 'g', 10));
    e->setAlignment(Qt::AlignCenter);
    e->setStyleSheet("QLineEdit { background: #10151b; color: white; border: 1px solid #2f9bff; border-radius: 3px; "
                     "padding: 2px 4px; font-weight: bold; }");
    e->resize(84, 24);
    e->move(int(at.x() - 42), int(at.y() - 12));
    e->show();
    e->setFocus();
    e->selectAll();
    m_edit = e;
    m_editConstraint = id;
    e->done = [this](bool accept) { closeValueEditor(accept); };
}

void SketchEditor::closeValueEditor(bool apply)
{
    if (!m_edit)
        return;
    QPointer<QLineEdit> e = m_edit;
    m_edit = nullptr;
    if (apply) {
        QString t = e->text().trimmed();
        t.replace(',', '.');
        t.remove("mm");
        t.remove(QChar(0xB0));
        bool ok = false;
        double v = QLocale::c().toDouble(t, &ok);
        auto it = std::find_if(m_sketch.constraints.begin(), m_sketch.constraints.end(),
                               [&](const SketchConstraint& c) { return c.id == m_editConstraint; });
        if (ok && it != m_sketch.constraints.end()) {
            const bool angle = it->kind == ConstraintKind::Angle;
            if (angle)
                v = v * kPi / 180.0;
            else if (v <= 0 && it->kind != ConstraintKind::DistanceX && it->kind != ConstraintKind::DistanceY) {
                m_ws.status(QObject::tr("A dimension must be positive"));
                ok = false;
            }
            else if (it->kind == ConstraintKind::DistanceX || it->kind == ConstraintKind::DistanceY)
                v = std::copysign(v, it->value == 0 ? 1.0 : it->value);
            if (ok) {
                const Sketch before = m_sketch;
                snapshotUndo();
                it->value = v;
                QString why;
                if (!solveOrRevert(m_sketch, before, m_status, {}, &why)) {
                    m_undo.pop_back();
                    m_ws.status(why);
                }
            }
        }
        else if (!ok) {
            m_ws.status(QObject::tr("Invalid value"));
        }
    }
    if (e)
        e->deleteLater();
    m_ws.viewport()->setFocus();
    changed();
}

// ------------------------------------------------------------------ events

bool SketchEditor::mousePress(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return false;
    if (m_edit)
        closeValueEditor(true);
    auto lp = toLocal(e->position());
    if (!lp)
        return true;
    m_snap = snapPoint(m_sketch, *lp, 9.0 * modelPerPixel());
    const Hit h = hitTest(e->position());
    switch (m_tool) {
        case Tool::Line: clickLine(*lp); break;
        case Tool::Rectangle: clickRect(*lp); break;
        case Tool::Circle: clickCircle(*lp); break;
        case Tool::Arc: clickArc(*lp); break;
        case Tool::Dimension: clickDimension(h, *lp); break;
        case Tool::Select: clickSelect(h, e); break;
    }
    changed();
    return true;
}

bool SketchEditor::mouseMove(QMouseEvent* e)
{
    auto lp = toLocal(e->position());
    m_cursorValid = lp.has_value();
    if (!lp)
        return true;

    if (m_pressed) {
        if (!m_dragging && std::hypot(e->position().x() - m_pressPx.x(), e->position().y() - m_pressPx.y()) > 3) {
            m_dragging = true;
            snapshotUndo();
        }
        if (m_dragging) {
            DragTarget d{m_dragRef, *lp, m_grabOffset};
            SketchSolver::solve(m_sketch, d, false);
            m_ws.viewport()->refreshOverlay();
            return true;
        }
    }

    const std::vector<Uuid> exclude;
    m_snap = snapPoint(m_sketch, *lp, 9.0 * modelPerPixel(), exclude);
    m_hint = LineHint::None;
    if (m_tool == Tool::Line && !m_clicks.empty() && !m_snap.point)
        m_hint = inferLineDirection(m_clicks[0], m_snap.pos);
    m_hover = (m_tool == Tool::Select || m_tool == Tool::Dimension) ? hitTest(e->position()) : Hit{};
    m_ws.viewport()->refreshOverlay();
    return true;
}

bool SketchEditor::mouseRelease(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return false;
    if (m_dragging) {
        solve();
        changed();
    }
    m_pressed = false;
    m_dragging = false;
    return true;
}

bool SketchEditor::mouseDoubleClick(QMouseEvent* e)
{
    const Hit h = hitTest(e->position());
    if (h.kind == Hit::Constraint) {
        openValueEditor(h.constraint);
        return true;
    }
    if (m_tool == Tool::Line) {
        resetTool();
        changed();
    }
    return true;
}

bool SketchEditor::keyPress(QKeyEvent* e)
{
    if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier))
        return false;
    switch (e->key()) {
        case Qt::Key_Escape:
            if (m_edit)
                closeValueEditor(false);
            else if (!m_clicks.empty() || m_dimFirst)
                resetTool();
            else if (m_tool != Tool::Select)
                m_tool = Tool::Select;
            else {
                m_selPoints.clear();
                m_selCurves.clear();
                m_selConstraints.clear();
            }
            changed();
            return true;  // never discard a sketch on Escape
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (!m_clicks.empty()) {
                resetTool();
                changed();
                return true;
            }
            return false;  // Workspace -> finish sketch
        case Qt::Key_L: setTool(Tool::Line); return true;
        case Qt::Key_R: setTool(Tool::Rectangle); return true;
        case Qt::Key_C: setTool(Tool::Circle); return true;
        case Qt::Key_A: setTool(Tool::Arc); return true;
        case Qt::Key_D: setTool(Tool::Dimension); return true;
        case Qt::Key_X: toggleConstruction(); return true;
        case Qt::Key_H: applyConstraint(ConstraintKind::Horizontal); return true;
        case Qt::Key_V: applyConstraint(ConstraintKind::Vertical); return true;
        case Qt::Key_I: applyConstraint(ConstraintKind::Coincident); return true;
        case Qt::Key_P: applyConstraint(ConstraintKind::Parallel); return true;
        case Qt::Key_K: applyConstraint(ConstraintKind::Perpendicular); return true;
        case Qt::Key_T: applyConstraint(ConstraintKind::Tangent); return true;
        case Qt::Key_Q: applyConstraint(ConstraintKind::Equal); return true;
        case Qt::Key_F: applyConstraint(ConstraintKind::Fixed); return true;
        case Qt::Key_Delete:
        case Qt::Key_Backspace: deleteSelection(); return true;
        default: return false;
    }
}

std::vector<MenuAction> SketchEditor::menuActions()
{
    return {
        {QObject::tr("Line"), icon(IconId::Line), [this] { setTool(Tool::Line); }},
        {QObject::tr("Rectangle"), icon(IconId::Rectangle), [this] { setTool(Tool::Rectangle); }},
        {QObject::tr("Circle"), icon(IconId::Circle), [this] { setTool(Tool::Circle); }},
        {QObject::tr("Arc"), icon(IconId::Arc), [this] { setTool(Tool::Arc); }},
        {QObject::tr("Dimension"), icon(IconId::Dimension), [this] { setTool(Tool::Dimension); }},
        {QObject::tr("Construction"), icon(IconId::Construction), [this] { toggleConstruction(); }},
        {QObject::tr("Finish sketch"), icon(IconId::Finish), [ws = &m_ws] { ws->finishSession(); }},
        {QObject::tr("Cancel"), icon(IconId::Cancel), [ws = &m_ws] { ws->cancelSession(); }},
    };
}

// ------------------------------------------------------------------ panel

QWidget* SketchEditor::createPanel(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* lay = new QVBoxLayout(w);
    auto* title = new QLabel(QString("<b>%1</b>").arg(QString::fromStdString(m_name)), w);
    title->setStyleSheet("font-size: 15px;");
    lay->addWidget(title);
    m_statusLabel = new QLabel(w);
    lay->addWidget(m_statusLabel);

    auto* create = new QGroupBox(QObject::tr("Create"), w);
    auto* cg = new QGridLayout(create);
    struct T
    {
        Tool tool;
        IconId icon;
        const char* label;
        const char* key;
    };
    const T tools[] = {{Tool::Select, IconId::Fit, "Select", "Esc"},  {Tool::Line, IconId::Line, "Line", "L"},
                       {Tool::Rectangle, IconId::Rectangle, "Rectangle", "R"}, {Tool::Circle, IconId::Circle, "Circle", "C"},
                       {Tool::Arc, IconId::Arc, "Arc", "A"}, {Tool::Dimension, IconId::Dimension, "Dimension", "D"}};
    m_toolButtons.clear();
    int i = 0;
    for (const T& t : tools) {
        auto* b = new QPushButton(icon(t.icon), QObject::tr(t.label), create);
        b->setCheckable(true);
        b->setToolTip(QString("%1 (%2)").arg(QObject::tr(t.label), t.key));
        QObject::connect(b, &QPushButton::clicked, b, [this, tool = t.tool] { setTool(tool); });
        cg->addWidget(b, i / 2, i % 2);
        m_toolButtons.emplace_back(t.tool, b);
        ++i;
    }
    lay->addWidget(create);

    auto* cons = new QGroupBox(QObject::tr("Constrain"), w);
    auto* kg = new QGridLayout(cons);
    struct K
    {
        ConstraintKind k;
        IconId icon;
        const char* label;
        const char* key;
    };
    const K ks[] = {{ConstraintKind::Horizontal, IconId::Horizontal, "Horizontal", "H"},
                    {ConstraintKind::Vertical, IconId::Vertical, "Vertical", "V"},
                    {ConstraintKind::Coincident, IconId::Coincident, "Coincident", "I"},
                    {ConstraintKind::Parallel, IconId::Parallel, "Parallel", "P"},
                    {ConstraintKind::Perpendicular, IconId::Perpendicular, "Perpendicular", "K"},
                    {ConstraintKind::Tangent, IconId::Tangent, "Tangent", "T"},
                    {ConstraintKind::Equal, IconId::Equal, "Equal", "Q"},
                    {ConstraintKind::Fixed, IconId::Fix, "Fix", "F"}};
    i = 0;
    for (const K& k : ks) {
        auto* b = new QPushButton(icon(k.icon), "", cons);
        b->setToolTip(QString("%1 (%2)").arg(QObject::tr(k.label), k.key));
        b->setIconSize(QSize(20, 20));
        QObject::connect(b, &QPushButton::clicked, b, [this, kind = k.k] { applyConstraint(kind); });
        kg->addWidget(b, i / 4, i % 4);
        ++i;
    }
    auto* constr = new QPushButton(icon(IconId::Construction), QObject::tr("Construction"), cons);
    constr->setToolTip(QObject::tr("Toggle construction geometry (X)"));
    QObject::connect(constr, &QPushButton::clicked, constr, [this] { toggleConstruction(); });
    kg->addWidget(constr, 2, 0, 1, 4);
    lay->addWidget(cons);

    auto* help = new QLabel(QObject::tr("<small>Auto-constraints while drawing: snap to points and curves, "
                                        "horizontal / vertical, tangent.<br>Drag points to explore the remaining "
                                        "freedom. Double-click a dimension to edit it.</small>"),
                            w);
    help->setWordWrap(true);
    help->setStyleSheet("color: #9aa3ad;");
    lay->addWidget(help);
    lay->addStretch(1);

    auto* finish = new QPushButton(icon(IconId::Finish), QObject::tr("Finish sketch"), w);
    finish->setDefault(true);
    auto* cancel = new QPushButton(icon(IconId::Cancel), QObject::tr("Cancel"), w);
    QObject::connect(finish, &QPushButton::clicked, finish, [ws = &m_ws] { ws->finishSession(); });
    QObject::connect(cancel, &QPushButton::clicked, cancel, [ws = &m_ws] { ws->cancelSession(); });
    lay->addWidget(finish);
    lay->addWidget(cancel);
    updatePanel();
    return w;
}

void SketchEditor::updatePanel()
{
    for (auto& [tool, b] : m_toolButtons)
        if (b)
            b->setChecked(tool == m_tool);
    if (!m_statusLabel)
        return;
    QString text;
    QColor c;
    switch (m_status.state) {
        case SketchDofState::FullyConstrained:
            text = QObject::tr("Fully constrained");
            c = Colors::SketchFull;
            break;
        case SketchDofState::UnderConstrained:
            text = m_status.dof >= 0 ? QObject::tr("%1 degree(s) of freedom").arg(m_status.dof)
                                     : QObject::tr("Under-constrained");
            c = Colors::SketchUnder;
            break;
        case SketchDofState::OverConstrained:
            text = QObject::tr("Over-constrained");
            c = Colors::SketchOver;
            break;
        case SketchDofState::Failed:
            text = QObject::tr("Solver failed");
            c = Colors::SketchOver;
            break;
    }
    m_statusLabel->setText(QString("<span style='color:%1'>● %2</span>").arg(c.name(), text));
}

// ------------------------------------------------------------------ painting

std::optional<Vec2> SketchEditor::labelAnchor(const SketchConstraint& c, QPointF* screen) const
{
    auto ga = m_sketch.geoOrReserved(c.a.geo);
    if (!ga)
        return std::nullopt;
    auto gb = c.b.valid() ? m_sketch.geoOrReserved(c.b.geo) : std::nullopt;
    const double off = 20.0 * modelPerPixel();
    Vec2 at;
    switch (c.kind) {
        case ConstraintKind::Distance:
        case ConstraintKind::DistanceX:
        case ConstraintKind::DistanceY: {
            Vec2 a, b;
            if (!gb) {
                a = ga->p1;
                b = ga->p2;
            }
            else if (c.b.pos == PointPos::None && gb->kind == GeoKind::Line) {
                a = ga->point(c.a.pos);
                const Vec2 d = (gb->p2 - gb->p1).normalized();
                b = gb->p1 + d * (a - gb->p1).dot(d);
            }
            else {
                a = ga->point(c.a.pos);
                b = gb->point(c.b.pos);
            }
            const Vec2 d = (b - a).normalized();
            at = (a + b) * 0.5 + Vec2{-d.y, d.x} * off;
            break;
        }
        case ConstraintKind::Radius:
        case ConstraintKind::Diameter:
            at = ga->c + Vec2{0.7071, 0.7071} * (ga->r + off);
            break;
        case ConstraintKind::Angle:
            at = gb ? ((ga->p1 + ga->p2) * 0.5 + (gb->p1 + gb->p2) * 0.5) * 0.5 : (ga->p1 + ga->p2) * 0.5;
            break;
        default: {
            if (ga->kind == GeoKind::Line) {
                const Vec2 d = (ga->p2 - ga->p1).normalized();
                at = (ga->p1 + ga->p2) * 0.5 + Vec2{-d.y, d.x} * (off * 0.6);
            }
            else if (ga->kind == GeoKind::Circle || ga->kind == GeoKind::Arc)
                at = ga->c + Vec2{0, ga->r + off * 0.6};
            else
                at = ga->point(c.a.pos) + Vec2{off * 0.5, off * 0.5};
            break;
        }
    }
    at = at + c.labelOffset;
    if (screen && !toScreen(at, screen))
        return std::nullopt;
    return at;
}

void SketchEditor::paintGeometry(QPainter& p)
{
    // sketch axes
    const double L = 4000 * modelPerPixel();
    QPointF a, b;
    p.setPen(QPen(QColor(255, 255, 255, 40), 1, Qt::DashLine));
    if (toScreen({-L, 0}, &a) && toScreen({L, 0}, &b))
        p.drawLine(a, b);
    if (toScreen({0, -L}, &a) && toScreen({0, L}, &b))
        p.drawLine(a, b);

    for (const SketchGeo& g : m_sketch.geos) {
        const bool sel = std::find(m_selCurves.begin(), m_selCurves.end(), g.id) != m_selCurves.end();
        const bool hov = m_hover.kind == Hit::Curve && m_hover.ref.geo == g.id;
        const bool dimFirst = m_dimFirst && m_dimFirst->kind == Hit::Curve && m_dimFirst->ref.geo == g.id;
        QColor c = colorOf(g);
        if (sel || dimFirst)
            c = Colors::Accent;
        else if (hov)
            c = c.lighter(140);
        QPen pen(c, sel || hov ? 2.8 : 1.9, g.construction ? Qt::DashLine : Qt::SolidLine, Qt::RoundCap);
        p.setPen(pen);
        QPainterPath path;
        auto sample = [&](double a0, double a1) {
            const int n = std::max(24, int(std::abs(a1 - a0) / (2 * kPi) * 96));
            for (int k = 0; k <= n; ++k) {
                const double t = a0 + (a1 - a0) * k / n;
                QPointF q;
                if (!toScreen(g.c + Vec2{std::cos(t), std::sin(t)} * g.r, &q))
                    return;
                k == 0 ? path.moveTo(q) : path.lineTo(q);
            }
        };
        switch (g.kind) {
            case GeoKind::Line:
                if (toScreen(g.p1, &a) && toScreen(g.p2, &b)) {
                    path.moveTo(a);
                    path.lineTo(b);
                }
                break;
            case GeoKind::Circle: sample(0, 2 * kPi); break;
            case GeoKind::Arc: sample(g.a0, g.a1); break;
            case GeoKind::Point: break;
        }
        p.drawPath(path);
    }

    // points
    auto drawPoint = [&](const GeoRef& ref, Vec2 pos, QColor c) {
        QPointF s;
        if (!toScreen(pos, &s))
            return;
        const bool sel = std::find(m_selPoints.begin(), m_selPoints.end(), ref) != m_selPoints.end();
        const bool hov = m_hover.kind == Hit::Point && m_hover.ref == ref;
        const bool first = m_dimFirst && m_dimFirst->kind == Hit::Point && m_dimFirst->ref == ref;
        const double r = sel || hov || first ? 4.5 : 3.0;
        p.setPen(Qt::NoPen);
        p.setBrush(sel || first ? Colors::Accent : (hov ? c.lighter(150) : c));
        p.drawRect(QRectF(s.x() - r, s.y() - r, 2 * r, 2 * r));
    };
    drawPoint({kOriginId, PointPos::Start}, {0, 0}, QColor(0xff, 0xd0, 0x40));
    for (const SketchGeo& g : m_sketch.geos)
        for (PointPos pos : pointsOf(g))
            drawPoint({g.id, pos}, g.point(pos), colorOf(g));
    p.setBrush(Qt::NoBrush);
}

void SketchEditor::paintConstraints(QPainter& p)
{
    m_labelRects.clear();
    QFont f = p.font();
    f.setPixelSize(11);
    f.setBold(true);
    p.setFont(f);
    const QFontMetricsF fm(f);

    for (const SketchConstraint& c : m_sketch.constraints) {
        const bool bad = m_status.conflicting.count(c.id) || m_status.redundant.count(c.id);
        const bool sel = std::find(m_selConstraints.begin(), m_selConstraints.end(), c.id) != m_selConstraints.end();
        const bool hov = m_hover.kind == Hit::Constraint && m_hover.constraint == c.id;
        QColor col = bad ? Colors::SketchOver : (isDimensional(c.kind) ? Colors::Dimension : Colors::Constraint);
        if (sel)
            col = Colors::Accent;
        else if (hov)
            col = col.lighter(130);

        if (c.kind == ConstraintKind::Coincident)
            continue;  // shown by the shared point itself
        if (c.kind == ConstraintKind::PointOnObject) {
            QPointF s;
            if (auto pt = m_sketch.pointOf(c.a); pt && toScreen(*pt, &s)) {
                p.setPen(QPen(col, 1.4));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(s, 6, 6);
            }
            continue;
        }

        QPointF at;
        if (!labelAnchor(c, &at))
            continue;
        QString text;
        switch (c.kind) {
            case ConstraintKind::Horizontal: text = "H"; break;
            case ConstraintKind::Vertical: text = "V"; break;
            case ConstraintKind::Parallel: text = "//"; break;
            case ConstraintKind::Perpendicular: text = QString(QChar(0x22A5)); break;
            case ConstraintKind::Tangent: text = "T"; break;
            case ConstraintKind::Equal: text = "="; break;
            case ConstraintKind::Fixed: text = "Fix"; break;
            case ConstraintKind::Distance:
            case ConstraintKind::DistanceX:
            case ConstraintKind::DistanceY: text = QLocale::c().toString(std::abs(c.value), 'f', 2); break;
            case ConstraintKind::Radius: text = "R" + QLocale::c().toString(c.value, 'f', 2); break;
            case ConstraintKind::Diameter: text = QString(QChar(0x2300)) + QLocale::c().toString(c.value, 'f', 2); break;
            case ConstraintKind::Angle:
                text = QLocale::c().toString(std::abs(c.value) * 180.0 / kPi, 'f', 1) + QChar(0xB0);
                break;
            default: break;
        }

        // dimension lines
        if (isDimensional(c.kind)) {
            auto ga = m_sketch.geoOrReserved(c.a.geo);
            auto gb = c.b.valid() ? m_sketch.geoOrReserved(c.b.geo) : std::nullopt;
            p.setPen(QPen(col, 1.0));
            if (ga && (c.kind == ConstraintKind::Radius || c.kind == ConstraintKind::Diameter)) {
                QPointF s0, s1;
                if (toScreen(ga->c, &s0) && toScreen(ga->c + Vec2{0.7071, 0.7071} * ga->r, &s1)) {
                    p.drawLine(s0, s1);
                    p.drawLine(s1, at);
                }
            }
            else if (ga && (c.kind == ConstraintKind::Distance || c.kind == ConstraintKind::DistanceX ||
                            c.kind == ConstraintKind::DistanceY)) {
                Vec2 a = ga->p1, b = ga->p2;
                if (gb) {
                    a = ga->point(c.a.pos);
                    if (c.b.pos == PointPos::None && gb->kind == GeoKind::Line) {
                        const Vec2 d = (gb->p2 - gb->p1).normalized();
                        b = gb->p1 + d * (a - gb->p1).dot(d);
                    }
                    else
                        b = gb->point(c.b.pos);
                }
                const Vec2 d = (b - a).normalized();
                const Vec2 n{-d.y, d.x};
                const double off = 20.0 * modelPerPixel();
                QPointF sa, sb, oa, ob;
                if (toScreen(a, &sa) && toScreen(b, &sb) && toScreen(a + n * off, &oa) && toScreen(b + n * off, &ob)) {
                    p.drawLine(sa, oa);
                    p.drawLine(sb, ob);
                    p.drawLine(oa, ob);
                }
            }
        }

        const QSizeF ts = fm.size(0, text) + QSizeF(8, 4);
        const QRectF r(at.x() - ts.width() / 2, at.y() - ts.height() / 2, ts.width(), ts.height());
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(20, 24, 30, isDimensional(c.kind) ? 220 : 170));
        p.drawRoundedRect(r, 3, 3);
        p.setPen(col);
        p.drawText(r, Qt::AlignCenter, text);
        m_labelRects.emplace_back(r, c.id);
    }
    p.setBrush(Qt::NoBrush);
}

void SketchEditor::paintToolPreview(QPainter& p)
{
    if (!m_cursorValid || m_tool == Tool::Select)
        return;
    const Vec2 cur = m_snap.pos;
    QPen pen(Colors::Hover, 1.6, Qt::SolidLine, Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    QPointF a, b;
    auto circle = [&](Vec2 c, double r, double a0, double a1) {
        QPainterPath path;
        for (int k = 0; k <= 72; ++k) {
            const double t = a0 + (a1 - a0) * k / 72;
            QPointF q;
            if (!toScreen(c + Vec2{std::cos(t), std::sin(t)} * r, &q))
                return;
            k == 0 ? path.moveTo(q) : path.lineTo(q);
        }
        p.drawPath(path);
    };

    if (!m_clicks.empty()) {
        const Vec2 s = m_clicks[0];
        switch (m_tool) {
            case Tool::Line: {
                const Vec2 e = m_snap.point ? cur : applyHint(s, cur, m_hint);
                if (toScreen(s, &a) && toScreen(e, &b)) {
                    p.drawLine(a, b);
                    p.setPen(Colors::Dimension);
                    p.drawText(b + QPointF(14, -10), QLocale::c().toString((e - s).length(), 'f', 2));
                    if (m_hint != LineHint::None) {
                        p.setPen(Colors::Constraint);
                        p.drawText((a + b) / 2 + QPointF(8, -8), m_hint == LineHint::Horizontal ? "H" : "V");
                    }
                }
                break;
            }
            case Tool::Rectangle: {
                const Vec2 q[4] = {s, {cur.x, s.y}, cur, {s.x, cur.y}};
                for (int i = 0; i < 4; ++i)
                    if (toScreen(q[i], &a) && toScreen(q[(i + 1) % 4], &b))
                        p.drawLine(a, b);
                if (toScreen(cur, &b)) {
                    p.setPen(Colors::Dimension);
                    p.drawText(b + QPointF(14, -10), QString("%1 x %2")
                                                         .arg(QLocale::c().toString(std::abs(cur.x - s.x), 'f', 2))
                                                         .arg(QLocale::c().toString(std::abs(cur.y - s.y), 'f', 2)));
                }
                break;
            }
            case Tool::Circle: {
                const double r = (cur - s).length();
                circle(s, r, 0, 2 * kPi);
                if (toScreen(cur, &b)) {
                    p.setPen(Colors::Dimension);
                    p.drawText(b + QPointF(14, -10), QString(QChar(0x2300)) + QLocale::c().toString(2 * r, 'f', 2));
                }
                break;
            }
            case Tool::Arc: {
                if (m_clicks.size() == 1) {
                    if (toScreen(s, &a) && toScreen(cur, &b))
                        p.drawLine(a, b);
                }
                else {
                    const Vec2 st = m_clicks[1];
                    const double r = (st - s).length();
                    double a0 = std::atan2(st.y - s.y, st.x - s.x), a1 = std::atan2(cur.y - s.y, cur.x - s.x);
                    while (a1 <= a0)
                        a1 += 2 * kPi;
                    circle(s, r, a0, a1);
                }
                break;
            }
            default: break;
        }
    }

    // snap indicator
    if (toScreen(cur, &a)) {
        if (m_snap.point) {
            p.setPen(QPen(QColor(0xff, 0xd0, 0x40), 1.6));
            p.drawRect(QRectF(a.x() - 6, a.y() - 6, 12, 12));
        }
        else if (m_snap.curve) {
            p.setPen(QPen(QColor(0xff, 0xd0, 0x40), 1.6));
            p.drawLine(a + QPointF(-5, -5), a + QPointF(5, 5));
            p.drawLine(a + QPointF(-5, 5), a + QPointF(5, -5));
        }
        else {
            p.setPen(QPen(QColor(255, 255, 255, 160), 1));
            p.drawLine(a + QPointF(-7, 0), a + QPointF(7, 0));
            p.drawLine(a + QPointF(0, -7), a + QPointF(0, 7));
        }
    }
}

void SketchEditor::paintStatus(QPainter& p)
{
    QString state;
    QColor c;
    switch (m_status.state) {
        case SketchDofState::FullyConstrained:
            state = QObject::tr("Fully constrained");
            c = Colors::SketchFull;
            break;
        case SketchDofState::UnderConstrained:
            state = m_status.dof >= 0 ? QObject::tr("%1 DOF").arg(m_status.dof) : QObject::tr("Under-constrained");
            c = Colors::SketchUnder;
            break;
        case SketchDofState::OverConstrained:
            state = QObject::tr("Over-constrained");
            c = Colors::SketchOver;
            break;
        case SketchDofState::Failed:
            state = QObject::tr("Solver failed");
            c = Colors::SketchOver;
            break;
    }
    static const char* toolNames[] = {"Select", "Line", "Rectangle", "Circle", "Arc", "Dimension"};
    QString hint;
    switch (m_tool) {
        case Tool::Line: hint = m_clicks.empty() ? QObject::tr("Click the start point") : QObject::tr("Click the next point — Esc to stop"); break;
        case Tool::Rectangle: hint = m_clicks.empty() ? QObject::tr("Click the first corner") : QObject::tr("Click the opposite corner"); break;
        case Tool::Circle: hint = m_clicks.empty() ? QObject::tr("Click the center") : QObject::tr("Click to set the radius"); break;
        case Tool::Arc:
            hint = m_clicks.empty() ? QObject::tr("Click the center")
                   : m_clicks.size() == 1 ? QObject::tr("Click the start point")
                                          : QObject::tr("Click the end point (counter-clockwise)");
            break;
        case Tool::Dimension:
            hint = m_dimFirst ? QObject::tr("Click a second entity, or empty space for a length")
                              : QObject::tr("Click a line, circle, arc or point");
            break;
        case Tool::Select: hint = QObject::tr("Drag geometry · double-click a dimension to edit · Enter to finish"); break;
    }
    QFont f = p.font();
    f.setPixelSize(12);
    f.setBold(true);
    p.setFont(f);
    const QString head = QString("%1  ·  %2").arg(QString::fromStdString(m_name), QObject::tr(toolNames[int(m_tool)]));
    QRectF box(12, 12, 360, 52);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(20, 24, 30, 200));
    p.drawRoundedRect(box, 6, 6);
    p.setPen(QColor(230, 233, 237));
    p.drawText(box.adjusted(10, 6, -10, -26), Qt::AlignLeft | Qt::AlignVCenter, head);
    p.setPen(c);
    p.drawText(box.adjusted(10, 6, -10, -26), Qt::AlignRight | Qt::AlignVCenter, QString::fromUtf8("● ") + state);
    f.setBold(false);
    f.setPixelSize(11);
    p.setFont(f);
    p.setPen(QColor(170, 178, 188));
    p.drawText(box.adjusted(10, 28, -10, -4), Qt::AlignLeft | Qt::AlignVCenter, hint);
    p.setBrush(Qt::NoBrush);
}

void SketchEditor::paintOverlay(QPainter& p)
{
    paintGeometry(p);
    paintConstraints(p);
    paintToolPreview(p);
    paintStatus(p);
    if (m_edit) {
        // keep the inline editor glued to its dimension while the camera moves
        auto it = std::find_if(m_sketch.constraints.begin(), m_sketch.constraints.end(),
                               [&](const SketchConstraint& c) { return c.id == m_editConstraint; });
        QPointF at;
        if (it != m_sketch.constraints.end() && labelAnchor(*it, &at))
            m_edit->move(int(at.x() - m_edit->width() / 2), int(at.y() - m_edit->height() / 2));
    }
}

}  // namespace silex::ui
