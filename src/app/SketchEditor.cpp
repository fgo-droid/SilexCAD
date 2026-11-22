#include "SketchEditor.h"

#include "ExprSpinBox.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QCheckBox>
#include <QComboBox>
#include <QMenu>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

#include <QBuffer>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QMessageBox>
#include <QSettings>
#include <QSlider>

#include "Icons.h"
#include "sketch/SketchCoach.h"
#include "sketch/VectorImporter.h"
#include "ModelController.h"
#include "Theme.h"
#include "Viewport.h"

namespace silex::ui {

namespace st = sketchtools;

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
        m_canvas = m_existing->as<SketchParams>().canvas;
        if (auto snap = ws.model()->snapshot())
            if (const SketchView* sv = snap->sketch(m_id); sv && !sv->sketch.geos.empty() &&
                std::any_of(sv->sketch.geos.begin(), sv->sketch.geos.end(), [](const SketchGeo& g) { return g.external(); }))
                m_sketch = sv->sketch;
    }
    else {
        m_id = newUuid();
        m_name = ws.model()->doc().nextName("Sketch");
    }
    solve();
    lookAt();
}

void SketchEditor::lookAt()
{
    // Look at the sketch plane (keeping the current target projected on the plane).
    Viewport* vp = m_ws.viewport();
    const QVector3D n = toQ(m_plane.normal), o = toQ(m_plane.origin);
    QVector3D t = vp->camera().target;
    t -= n * QVector3D::dotProduct(t - o, n);
    vp->animateTo(Camera::orientationFor(m_plane), t);
}

SketchEditor::~SketchEditor()
{
    if (m_edit)
        m_edit->deleteLater();
    if (m_pulseTimer)
        m_pulseTimer->deleteLater();
}

QString SketchEditor::title() const { return QString::fromStdString(m_name); }

bool SketchEditor::commit()
{
    if (m_edit)
        closeValueEditor(true);
    if (!m_existing && m_sketch.geos.empty() && !m_canvas)
        return true;  // nothing drawn: nothing to add
    SketchParams p;
    p.plane = m_planeRef;
    p.sketch = m_sketch;
    p.canvas = m_canvas;
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
    m_filletFirst.reset();
    m_piece.reset();
    m_typedDims.clear();
    m_hint = LineHint::None;
    m_fieldStage = -1;
    refreshFields();
}

void SketchEditor::setTool(Tool t)
{
    if (m_edit)
        closeValueEditor(true);
    m_tool = t;
    m_pending = Pending::None;
    m_previewGeos.clear();
    resetTool();
    if (t == Tool::Trim)
        m_ws.status(QObject::tr("Trim: hover a curve, the red dotted portion is removed on click"));
    else if (t == Tool::Fillet)
        m_ws.status(QObject::tr("Sketch fillet: click the corner of two lines (or two lines)"));
    if (t != Tool::Project)
        m_ws.setHover({});
    changed();
}

QColor SketchEditor::colorOf(const SketchGeo& g) const
{
    if (m_status.state == SketchDofState::Failed)
        return Colors::SketchOver;
    for (const auto& c : m_sketch.constraints)
        if ((c.a.geo == g.id || c.b.geo == g.id) && (m_status.conflicting.count(c.id) || m_status.redundant.count(c.id)))
            return Colors::SketchOver;
    if (g.external())
        return Colors::SketchProjected;
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

// ------------------------------------------------------------------ Sketcher Pro helpers

Snap SketchEditor::snapAt(Vec2 lp, Qt::KeyboardModifiers mods) const
{
    Snap s = snapPoint(m_sketch, lp, 9.0 * modelPerPixel());
    if (!m_ws.gridSnap() || (mods & Qt::AltModifier) || s.point)
        return s;  // Alt held: magnetic grid temporarily off
    const double step = m_ws.gridStep();
    const Vec2 g = snapToGrid(lp, step);
    if (!s.curve)
        s.pos = g;
    else if (*s.curve == kXAxisId)
        s.pos = {g.x, 0};
    else if (*s.curve == kYAxisId)
        s.pos = {0, g.y};
    return s;
}

std::set<Uuid> SketchEditor::constraintGeos(const Uuid& id) const
{
    std::set<Uuid> out;
    for (const auto& c : m_sketch.constraints)
        if (c.id == id)
            for (const GeoRef* r : {&c.a, &c.b, &c.c})
                if (r->valid())
                    out.insert(r->geo);
    return out;
}

bool SketchEditor::applyTool(const st::ToolResult& r, const Sketch& before, const QString& done)
{
    if (!r) {
        m_sketch = before;
        m_ws.status(QString::fromStdString(r.error));
        return false;
    }
    QString why;
    if (!solveOrRevert(m_sketch, before, m_status, {}, &why)) {
        m_ws.status(why);
        m_ws.model()->noteUserError(why.toStdString());
        return false;
    }
    m_undo.push_back(before);
    if (m_undo.size() > 200)
        m_undo.erase(m_undo.begin());
    m_redo.clear();
    QString msg = done;
    if (r.removedConstraints > 0)
        msg += QObject::tr(" — %1 constraint(s) removed").arg(r.removedConstraints);
    m_ws.status(msg);
    return true;
}

SketchEditor::Field* SketchEditor::field(const QString& key)
{
    for (Field& f : m_fields)
        if (f.key == key)
            return &f;
    return nullptr;
}

std::optional<double> SketchEditor::fieldValue(const QString& key) const
{
    for (const Field& f : m_fields) {
        if (f.key != key || f.text.trimmed().isEmpty())
            continue;
        QString t = f.text.trimmed();
        if (!t.contains('('))
            t.replace(',', '.');
        try {
            const auto values = parameterValues(m_ws.model()->doc().parameters);
            double v = evaluateExpression(t.toStdString(), lookupIn(values));
            if (f.angle)
                v *= kPi / 180.0;
            return v;
        }
        catch (const ExpressionError&) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

void SketchEditor::refreshFields()
{
    const int stage = m_pending != Pending::None ? 1000 + int(m_pending) : int(m_tool) * 10 + int(m_clicks.size());
    if (stage == m_fieldStage)
        return;
    m_fieldStage = stage;
    m_fields.clear();
    m_activeField = 0;
    auto add = [&](const char* key, const QString& label, bool angle = false, bool integer = false) {
        Field f;
        f.key = key;
        f.label = label;
        f.angle = angle;
        f.integer = integer;
        m_fields.push_back(f);
    };
    switch (m_pending) {
        case Pending::Offset: add("distance", QObject::tr("Offset")); break;
        case Pending::LinearPattern:
            add("count", QObject::tr("Count"), false, true);
            add("spacing", QObject::tr("Spacing"));
            add("angle", QObject::tr("Angle"), true);
            break;
        case Pending::CircularPattern:
            add("count", QObject::tr("Count"), false, true);
            add("total", QObject::tr("Total angle"), true);
            break;
        case Pending::None:
            if (m_clicks.empty())
                break;
            switch (m_tool) {
                case Tool::Line:
                    add("length", QObject::tr("Length"));
                    add("angle", QObject::tr("Angle"), true);
                    break;
                case Tool::Rectangle:
                case Tool::CenterRectangle:
                    add("width", QObject::tr("Width"));
                    add("height", QObject::tr("Height"));
                    break;
                case Tool::Circle: add("diameter", QObject::tr("Diameter")); break;
                case Tool::Polygon:
                    add("sides", QObject::tr("Sides"), false, true);
                    add("radius", m_polyInscribed ? QObject::tr("Radius") : QObject::tr("Apothem"));
                    break;
                case Tool::Slot:
                    if (m_clicks.size() == 1)
                        add("length", m_slotOverall ? QObject::tr("Length") : QObject::tr("Center distance"));
                    else
                        add("width", QObject::tr("Width"));
                    break;
                default: break;
            }
            break;
    }
    updateLiveFields();
}

Vec2 SketchEditor::constrainedCursor() const
{
    Vec2 cur = m_snap.pos;
    if (m_clicks.empty() || m_pending != Pending::None)
        return cur;
    const Vec2 s = m_clicks[0];
    auto sgn = [](double v) { return v < 0 ? -1.0 : 1.0; };
    switch (m_tool) {
        case Tool::Line: {
            if (m_hint != LineHint::None && !m_snap.point)
                cur = applyHint(s, cur, m_hint);
            const auto L = fieldValue("length"), A = fieldValue("angle");
            if (!L && !A)
                return cur;
            Vec2 d = cur - s;
            if (d.length() < 1e-12)
                d = {1, 0};
            Vec2 u = d.normalized();
            if (A)
                u = {std::cos(*A), std::sin(*A)};
            const double len = L ? std::abs(*L) : std::max(d.dot(u), 1e-6);
            return s + u * len;
        }
        case Tool::Rectangle: {
            Vec2 d = cur - s;
            if (auto w = fieldValue("width"))
                d.x = sgn(d.x) * std::abs(*w);
            if (auto h = fieldValue("height"))
                d.y = sgn(d.y) * std::abs(*h);
            return s + d;
        }
        case Tool::CenterRectangle: {
            Vec2 d = cur - s;
            if (auto w = fieldValue("width"))
                d.x = sgn(d.x) * std::abs(*w) / 2;
            if (auto h = fieldValue("height"))
                d.y = sgn(d.y) * std::abs(*h) / 2;
            return s + d;
        }
        case Tool::Circle:
        case Tool::Polygon: {
            const auto v = m_tool == Tool::Circle ? fieldValue("diameter") : fieldValue("radius");
            if (!v)
                return cur;
            Vec2 d = cur - s;
            const Vec2 u = d.length() < 1e-12 ? Vec2{1, 0} : d.normalized();
            return s + u * (m_tool == Tool::Circle ? std::abs(*v) / 2 : std::abs(*v));
        }
        case Tool::Slot: {
            if (m_clicks.size() == 1) {
                const auto L = fieldValue("length");
                if (!L)
                    return cur;
                Vec2 d = cur - s;
                const Vec2 u = d.length() < 1e-12 ? Vec2{1, 0} : d.normalized();
                return s + u * (m_slotOverall ? std::abs(*L) / 2 : std::abs(*L));
            }
            const auto W = fieldValue("width");
            if (!W)
                return cur;
            const Vec2 a = m_slotOverall ? s * 2.0 - m_clicks[1] : s, b = m_clicks[1];
            const Vec2 u = (b - a).normalized(), n{-u.y, u.x};
            const double side = (cur - a).dot(n) < 0 ? -1.0 : 1.0;
            return a + u * (cur - a).dot(u) + n * (side * std::abs(*W) / 2);
        }
        default: return cur;
    }
}

void SketchEditor::updateLiveFields()
{
    if (m_fields.empty())
        return;
    const Vec2 cur = constrainedCursor();
    const Vec2 s = m_clicks.empty() ? Vec2{} : m_clicks[0];
    for (Field& f : m_fields) {
        const QString& k = f.key;
        if (m_pending == Pending::Offset)
            f.live = 2.0;
        else if (m_pending == Pending::LinearPattern)
            f.live = k == "count" ? 3 : k == "spacing" ? 10 : 0;
        else if (m_pending == Pending::CircularPattern)
            f.live = k == "count" ? 6 : 360;
        else if (k == "length" && m_tool == Tool::Line)
            f.live = (cur - s).length();
        else if (k == "angle" && m_tool == Tool::Line)
            f.live = std::atan2(cur.y - s.y, cur.x - s.x) * 180.0 / kPi;
        else if (k == "width" && m_tool == Tool::Rectangle)
            f.live = std::abs(cur.x - s.x);
        else if (k == "height" && m_tool == Tool::Rectangle)
            f.live = std::abs(cur.y - s.y);
        else if (k == "width" && m_tool == Tool::CenterRectangle)
            f.live = 2 * std::abs(cur.x - s.x);
        else if (k == "height" && m_tool == Tool::CenterRectangle)
            f.live = 2 * std::abs(cur.y - s.y);
        else if (k == "diameter")
            f.live = 2 * (cur - s).length();
        else if (k == "sides")
            f.live = m_polySides;
        else if (k == "radius")
            f.live = (cur - s).length();
        else if (k == "length" && m_tool == Tool::Slot)
            f.live = (cur - s).length() * (m_slotOverall ? 2 : 1);
        else if (k == "width" && m_tool == Tool::Slot && m_clicks.size() >= 2) {
            const Vec2 a = m_slotOverall ? s * 2.0 - m_clicks[1] : s, b = m_clicks[1];
            const Vec2 u = (b - a).normalized();
            f.live = 2 * std::abs(u.cross(cur - a));
        }
    }
}

bool SketchEditor::fieldKey(QKeyEvent* e)
{
    if (m_fields.empty())
        return false;
    Field& f = m_fields[size_t(std::clamp(m_activeField, 0, int(m_fields.size()) - 1))];
    const int k = e->key();
    if (k == Qt::Key_Tab || k == Qt::Key_Backtab) {
        const int n = int(m_fields.size());
        m_activeField = (m_activeField + (k == Qt::Key_Backtab ? n - 1 : 1)) % n;
        changed();
        return true;
    }
    if (k == Qt::Key_Return || k == Qt::Key_Enter) {
        const bool typed = std::any_of(m_fields.begin(), m_fields.end(), [](const Field& x) { return !x.text.isEmpty(); });
        if (!typed && m_pending == Pending::None)
            return false;  // plain Enter: end of the polyline / finish sketch
        commitFields();
        return true;
    }
    if (k == Qt::Key_Backspace) {
        if (f.text.isEmpty())
            return false;
        f.text.chop(1);
        updatePendingPreview();
        changed();
        return true;
    }
    if (k == Qt::Key_Escape) {
        const bool typed = std::any_of(m_fields.begin(), m_fields.end(), [](const Field& x) { return !x.text.isEmpty(); });
        if (!typed)
            return false;
        for (Field& x : m_fields)
            x.text.clear();
        updatePendingPreview();
        changed();
        return true;
    }
    const QString t = e->text();
    if (t.size() != 1)
        return false;
    const QChar c = t[0];
    // digits always type; letters only once typing started (expressions of global parameters)
    const bool numeric = c.isDigit() || c == '.' || c == ',' || (c == '-' && f.text.isEmpty());
    const bool expr = !f.text.isEmpty() && (c.isLetterOrNumber() || QString("+-*/()_ ").contains(c));
    if (!numeric && !expr)
        return false;
    f.text += c;
    updatePendingPreview();
    changed();
    return true;
}

void SketchEditor::updatePendingPreview()
{
    m_previewGeos.clear();
    if (m_pending == Pending::None)
        return;
    Sketch tmp = m_sketch;
    st::ToolResult r;
    if (m_pending == Pending::Offset) {
        r = st::offsetCurves(tmp, m_pendingGeos, fieldValue("distance").value_or(2.0));
    }
    else if (m_pending == Pending::LinearPattern) {
        const double a = fieldValue("angle").value_or(0.0);
        r = st::linearPattern(tmp, m_pendingGeos, {std::cos(a), std::sin(a)}, int(std::lround(fieldValue("count").value_or(3))),
                              fieldValue("spacing").value_or(10.0));
    }
    else {
        r = st::circularPattern(tmp, m_pendingGeos, m_pendingCenter, int(std::lround(fieldValue("count").value_or(6))),
                                fieldValue("total").value_or(2 * kPi));
    }
    if (r)
        for (const Uuid& id : r.geos)
            if (const SketchGeo* g = tmp.find(id); g && !g->construction)
                m_previewGeos.push_back(*g);
}

void SketchEditor::commitFields()
{
    if (m_pending != Pending::None) {
        const Sketch before = m_sketch;
        st::ToolResult r;
        QString done;
        if (m_pending == Pending::Offset) {
            r = st::offsetCurves(m_sketch, m_pendingGeos, fieldValue("distance").value_or(2.0));
            done = QObject::tr("Offset created — double-click its dimension to change the distance");
        }
        else if (m_pending == Pending::LinearPattern) {
            const double a = fieldValue("angle").value_or(0.0);
            r = st::linearPattern(m_sketch, m_pendingGeos, {std::cos(a), std::sin(a)},
                                  int(std::lround(fieldValue("count").value_or(3))), fieldValue("spacing").value_or(10.0));
            done = QObject::tr("Linear pattern created (pitch held by construction lines)");
        }
        else {
            r = st::circularPattern(m_sketch, m_pendingGeos, m_pendingCenter, int(std::lround(fieldValue("count").value_or(6))),
                                    fieldValue("total").value_or(2 * kPi));
            done = QObject::tr("Circular pattern created");
        }
        m_pending = Pending::None;
        m_previewGeos.clear();
        const bool ok = applyTool(r, before, done);
        resetTool();
        changed();
        if (ok && !r.driving.empty())
            openValueEditor(r.driving);
        return;
    }
    if (m_clicks.empty())
        return;
    if (auto n = fieldValue("sides"))
        m_polySides = std::clamp(int(std::lround(*n)), 3, 64);
    const Vec2 p = constrainedCursor();
    m_snap = Snap{p, std::nullopt, std::nullopt};
    m_hint = LineHint::None;
    dispatchClick(Hit{}, p, nullptr);
    changed();
}

void SketchEditor::dispatchClick(const Hit& h, Vec2 lp, QMouseEvent* e)
{
    // values typed for this click (they drive the geometry and become dimensions)
    for (const Field& f : m_fields)
        if (auto v = fieldValue(f.key); v && f.key != "sides")
            m_typedDims[f.key] = *v;
    if (auto n = fieldValue("sides"))
        m_polySides = std::clamp(int(std::lround(*n)), 3, 64);
    if (!m_clicks.empty() && m_tool != Tool::Select && m_tool != Tool::Dimension)
        m_snap.pos = constrainedCursor();
    const auto typed = m_typedDims;
    if (!typed.empty()) {
        m_hint = LineHint::None;  // typed values win over the inferred direction and the snap
        if (m_snap.point && (*m_sketch.pointOf(*m_snap.point) - m_snap.pos).length() > 1e-9)
            m_snap.point.reset();
        m_snap.curve.reset();
    }
    m_lastNamed.clear();
    switch (m_tool) {
        case Tool::Line: clickLine(lp); break;
        case Tool::Rectangle: clickRect(lp); break;
        case Tool::CenterRectangle: clickCenterRect(); break;
        case Tool::Polygon: clickPolygon(); break;
        case Tool::Circle: clickCircle(lp); break;
        case Tool::Arc: clickArc(lp); break;
        case Tool::Slot: clickSlot(); break;
        case Tool::Dimension: clickDimension(h, lp); break;
        case Tool::Select:
            if (e)
                clickSelect(h, e);
            break;
        case Tool::Project:
            if (e)
                clickProject(e->position());
            break;
        case Tool::Trim:
        case Tool::Extend: clickTrimExtend(h, lp); break;
        case Tool::Fillet: clickFillet(h); break;
    }
    if (!m_lastNamed.empty() && !typed.empty()) {
        m_typedDims = typed;
        addTypedDimensions();
    }
    if (m_lastNamed.empty() && m_tool == Tool::Line)
        m_typedDims.clear();  // next segment of a polyline: fresh values
    if (!m_lastNamed.empty())
        m_fieldStage = -1;  // a shape was created: fresh inputs for the next one
    refreshFields();
}

void SketchEditor::addTypedDimensions()
{
    std::vector<SketchConstraint> dims;
    auto named = [&](const char* k) -> std::optional<Uuid> {
        auto it = m_lastNamed.find(k);
        if (it == m_lastNamed.end())
            return std::nullopt;
        return it->second;
    };
    auto typed = [&](const char* k) -> std::optional<double> {
        auto it = m_typedDims.find(k);
        if (it == m_typedDims.end())
            return std::nullopt;
        return it->second;
    };
    auto dim = [&](ConstraintKind k, const std::optional<Uuid>& g, double v) {
        if (!g)
            return;
        auto c = makeC(k, {*g});
        c.value = v;
        dims.push_back(c);
    };
    if (auto v = typed("length"); v && m_tool == Tool::Line)
        dim(ConstraintKind::Distance, named("line"), std::abs(*v));
    if (auto v = typed("angle"); v && m_tool == Tool::Line) {
        if (auto g = named("line")) {
            auto c = makeC(ConstraintKind::Angle, {*g});
            const SketchGeo* l = m_sketch.find(*g);
            c.value = l ? std::atan2(l->p2.y - l->p1.y, l->p2.x - l->p1.x) : *v;
            dims.push_back(c);
        }
    }
    if (auto v = typed("width"); v && (m_tool == Tool::Rectangle || m_tool == Tool::CenterRectangle))
        dim(ConstraintKind::Distance, named("width"), std::abs(*v));
    if (auto v = typed("height"); v && (m_tool == Tool::Rectangle || m_tool == Tool::CenterRectangle))
        dim(ConstraintKind::Distance, named("height"), std::abs(*v));
    if (auto v = typed("diameter"))
        dim(ConstraintKind::Diameter, named("circle"), std::abs(*v));
    if (auto v = typed("radius"))
        dim(ConstraintKind::Diameter, named("sizecircle"), 2 * std::abs(*v));
    if (auto v = typed("length"); v && m_tool == Tool::Slot)
        dim(ConstraintKind::Distance, named("axis"), std::abs(*v));
    if (auto v = typed("width"); v && m_tool == Tool::Slot)
        dim(ConstraintKind::Radius, named("arc1"), std::abs(*v) / 2);
    m_typedDims.clear();
    if (dims.empty())
        return;
    // the shape was drawn at these values: the dimensions hold; drop them if they over-constrain
    const Sketch before = m_sketch;
    std::vector<Uuid> optional;
    for (auto& c : dims) {
        if (c.kind != ConstraintKind::Angle)
            c.value = m_sketch.measure(c);
        optional.push_back(c.id);
        addConstraint(c);
    }
    QString why;
    solveOrRevert(m_sketch, before, m_status, optional, &why);
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
    if (h.kind == Hit::Curve)
        return h;
    // v0.3: the sketch X / Y axes can be picked (dimensions, H/V, coincident, parallel...)
    const double L = 4000 * modelPerPixel();
    for (const auto& [id, dir] : {std::pair{kXAxisId, Vec2{1, 0}}, std::pair{kYAxisId, Vec2{0, 1}}}) {
        QPointF a, b;
        if (!toScreen(dir * -L, &a) || !toScreen(dir * L, &b))
            continue;
        const double d = segDistPx(px, a, b);
        if (d < best) {
            best = d;
            h.kind = Hit::Curve;
            h.ref = {id, PointPos::None};
        }
    }
    return h;
}

// ------------------------------------------------------------------ tools

void SketchEditor::clickProject(QPointF px)
{
    const SelItem it = m_ws.viewport()->pickAt(px, SelFilter::Edges);
    auto snap = m_ws.model()->snapshot();
    if (it.kind != SelKind::Edge || !snap || it.object < 0 || it.object >= int(snap->bodies.size())) {
        m_ws.status(QObject::tr("Click an edge of the model to project it"));
        return;
    }
    for (const SketchGeo& g : m_sketch.geos)
        if (g.external() && g.ext.name == it.name) {
            m_ws.status(QObject::tr("This edge is already projected"));
            return;
        }
    const RenderMesh& mesh = snap->bodies[size_t(it.object)].mesh;
    if (it.index < 0 || it.index >= int(mesh.edges.size()))
        return;
    const MeshEdge& me = mesh.edges[size_t(it.index)];
    std::vector<Vec2> pts;  // polyline of the edge, in sketch coordinates
    for (uint32_t k = me.first; k < me.first + me.count; ++k) {
        const Vec2 q = m_plane.toLocal(Vec3{mesh.edgePoints[3 * k], mesh.edgePoints[3 * k + 1], mesh.edgePoints[3 * k + 2]});
        if (pts.empty() || (pts.back() - q).length() > 1e-9)
            pts.push_back(q);
    }
    if (pts.size() < 2) {
        m_ws.status(QObject::tr("This edge projects to a point on the sketch plane"));
        return;
    }
    SketchGeo g;
    g.id = newUuid();
    g.construction = true;
    g.ext = it.ref();
    g.kind = GeoKind::Line;
    g.p1 = pts.front();
    g.p2 = pts.back();
    if (me.sig.type == 1 && pts.size() >= 3) {
        // circle through 3 points of the projected polyline (the kernel refines it exactly)
        const bool closed = (pts.front() - pts.back()).length() < 1e-6;
        const Vec2 a = pts.front(), b = pts[pts.size() / 3], c = pts[2 * pts.size() / 3];
        const double d = 2 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
        if (std::abs(d) > 1e-12) {
            const double a2 = a.dot(a), b2 = b.dot(b), c2 = c.dot(c);
            g.c = {(a2 * (b.y - c.y) + b2 * (c.y - a.y) + c2 * (a.y - b.y)) / d,
                   (a2 * (c.x - b.x) + b2 * (a.x - c.x) + c2 * (b.x - a.x)) / d};
            g.r = (a - g.c).length();
            if (closed) {
                g.kind = GeoKind::Circle;
            }
            else {
                const Vec2 e = pts.back(), m = pts[pts.size() / 2];
                auto ang = [&](Vec2 q) { return std::atan2(q.y - g.c.y, q.x - g.c.x); };
                auto ccw = [](double from, double to) {  // CCW sweep from -> to in [0, 2pi)
                    double s = to - from;
                    while (s < 0) s += 2 * kPi;
                    while (s >= 2 * kPi) s -= 2 * kPi;
                    return s;
                };
                double s0 = ang(a), s1 = ang(e);
                if (ccw(s0, ang(m)) > ccw(s0, s1))
                    std::swap(s0, s1);  // the edge runs clockwise in the sketch frame
                g.kind = GeoKind::Arc;
                g.a0 = s0;
                g.a1 = s0 + ccw(s0, s1);
            }
        }
    }
    snapshotUndo();
    m_sketch.geos.push_back(g);
    solve();
    m_ws.status(QObject::tr("Edge projected (purple): it follows the model — dimension or constrain against it"));
}

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
        m_ws.model()->noteUserError(why.toStdString());
        return;
    }
    m_lastNamed["line"] = g.id;
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
        m_ws.model()->noteUserError(why.toStdString());
    }
    else
        m_lastNamed = {{"width", ids[0]}, {"height", ids[1]}};
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
        m_ws.model()->noteUserError(why.toStdString());
    }
    else
        m_lastNamed = {{"circle", g.id}};
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
        m_ws.model()->noteUserError(why.toStdString());
    }
    resetTool();
}

void SketchEditor::clickCenterRect()
{
    if (m_clicks.empty()) {
        m_clicks = {m_snap.pos};
        m_clickSnaps = {m_snap};
        return;
    }
    const Sketch before = m_sketch;
    auto r = st::addCenterRectangle(m_sketch, m_clicks[0], m_snap.pos);
    if (r) {
        addSnapConstraints(m_clickSnaps[0], {r.named["center"], PointPos::Start});
        addSnapConstraints(m_snap, {r.named["right"], PointPos::End});
    }
    if (applyTool(r, before, QObject::tr("Center rectangle: symmetric about its center")))
        m_lastNamed = {{"width", r.named["bottom"]}, {"height", r.named["left"]}};
    resetTool();
}

void SketchEditor::clickPolygon()
{
    if (m_clicks.empty()) {
        m_clicks = {m_snap.pos};
        m_clickSnaps = {m_snap};
        return;
    }
    const Sketch before = m_sketch;
    auto r = st::addPolygon(m_sketch, m_clicks[0], m_snap.pos, m_polySides, m_polyInscribed);
    if (r)
        addSnapConstraints(m_clickSnaps[0], {r.named["circle"], PointPos::Center});
    if (applyTool(r, before, QObject::tr("Regular polygon: %1 sides").arg(m_polySides)))
        m_lastNamed = {{"sizecircle", r.named[m_polyInscribed ? "circle" : "incircle"]}};
    resetTool();
}

void SketchEditor::clickSlot()
{
    if (m_clicks.size() < 2) {
        if (m_clicks.size() == 1 && (m_snap.pos - m_clicks[0]).length() < 1e-6)
            return;
        m_clicks.push_back(m_snap.pos);
        m_clickSnaps.push_back(m_snap);
        return;
    }
    // overall mode: clicks = center, end of the axis; else: center 1, center 2
    const Vec2 c2 = m_clicks[1];
    const Vec2 c1 = m_slotOverall ? m_clicks[0] * 2.0 - c2 : m_clicks[0];
    const Vec2 u = (c2 - c1).normalized();
    const double radius = std::abs(u.cross(m_snap.pos - c1));
    const Sketch before = m_sketch;
    auto r = st::addSlot(m_sketch, c1, c2, radius, m_slotOverall);
    if (r) {
        if (m_slotOverall)
            addSnapConstraints(m_clickSnaps[0], {r.named["center"], PointPos::Start});
        else {
            addSnapConstraints(m_clickSnaps[0], {r.named["axis"], PointPos::Start});
            addSnapConstraints(m_clickSnaps[1], {r.named["axis"], PointPos::End});
        }
    }
    if (applyTool(r, before, QObject::tr("Slot: tangent sides and equal ends")))
        m_lastNamed = {{"axis", r.named["axis"]}, {"arc1", r.named["arc1"]}};
    resetTool();
}

void SketchEditor::clickTrimExtend(const Hit& h, Vec2 p)
{
    if (h.kind != Hit::Curve || isReservedGeo(h.ref.geo)) {
        m_ws.status(m_tool == Tool::Trim ? QObject::tr("Click the portion of a curve to remove")
                                         : QObject::tr("Click near the end of a line or arc to extend"));
        return;
    }
    const Sketch before = m_sketch;
    if (m_tool == Tool::Trim)
        applyTool(st::trim(m_sketch, h.ref.geo, p), before, QObject::tr("Trimmed"));
    else
        applyTool(st::extend(m_sketch, h.ref.geo, p), before, QObject::tr("Extended"));
    m_piece.reset();
}

void SketchEditor::clickFillet(const Hit& h)
{
    const Sketch before = m_sketch;
    st::ToolResult r;
    if (h.kind == Hit::Point && !isReservedGeo(h.ref.geo)) {
        r = st::filletAtPoint(m_sketch, h.ref, m_filletRadius);
    }
    else if (h.kind == Hit::Curve && isLine(m_sketch, h.ref.geo) && !isReservedGeo(h.ref.geo)) {
        if (!m_filletFirst || *m_filletFirst == h.ref.geo) {
            m_filletFirst = h.ref.geo;
            m_ws.status(QObject::tr("Click the second line"));
            return;
        }
        r = st::filletLines(m_sketch, *m_filletFirst, h.ref.geo, m_filletRadius);
        m_filletFirst.reset();
    }
    else {
        m_ws.status(QObject::tr("Click the corner shared by two lines"));
        return;
    }
    if (applyTool(r, before, QObject::tr("Sketch fillet — type the radius")) && !r.driving.empty()) {
        changed();
        openValueEditor(r.driving);
    }
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
            m_ws.model()->noteUserError(why.toStdString());
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
        else if (h.kind == Hit::Point && isLine(m_sketch, first.ref.geo))
            create(makeC(ConstraintKind::Distance, h.ref, {first.ref.geo, PointPos::None}));
        else if (!isReservedGeo(first.ref.geo))
            create(makeC(ConstraintKind::Distance, {first.ref.geo}));
        else
            m_ws.status(QObject::tr("An axis has no length: click a point after it"));
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
    if (h.kind == Hit::None) {  // click in the void: start a selection box
        m_boxing = true;
        m_boxFrom = m_boxTo = e->position();
    }
    const SketchGeo* hitGeo = m_sketch.find(h.ref.geo);
    if ((h.kind == Hit::Point || h.kind == Hit::Curve) && !isReservedGeo(h.ref.geo) && !(hitGeo && hitGeo->external())) {
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

/// Window (drag to the right): entities fully inside. Crossing (drag to the left): touched or inside.
void SketchEditor::finishBoxSelect(bool additive)
{
    const QRectF box = QRectF(m_boxFrom, m_boxTo).normalized();
    const bool crossing = m_boxTo.x() < m_boxFrom.x();
    if (!additive) {
        m_selPoints.clear();
        m_selCurves.clear();
        m_selConstraints.clear();
    }
    auto segHits = [&](QPointF a, QPointF b) {
        if (box.contains(a) || box.contains(b))
            return true;
        const QPointF c[4] = {box.topLeft(), box.topRight(), box.bottomRight(), box.bottomLeft()};
        for (int k = 0; k < 4; ++k)
            if (QLineF(a, b).intersects(QLineF(c[k], c[(k + 1) % 4]), nullptr) == QLineF::BoundedIntersection)
                return true;
        return false;
    };
    for (const SketchGeo& g : m_sketch.geos) {
        if (isReservedGeo(g.id) || g.external())
            continue;
        std::vector<QPointF> pts;
        bool ok = true;
        auto add = [&](Vec2 v) {
            QPointF q;
            if (toScreen(v, &q))
                pts.push_back(q);
            else
                ok = false;
        };
        switch (g.kind) {
            case GeoKind::Point: add(g.p1); break;
            case GeoKind::Line:
                add(g.p1);
                add(g.p2);
                break;
            case GeoKind::Circle:
            case GeoKind::Arc: {
                const double a0 = g.kind == GeoKind::Circle ? 0 : g.a0, a1 = g.kind == GeoKind::Circle ? 2 * kPi : g.a1;
                for (int k = 0; k <= 64; ++k) {
                    const double t = a0 + (a1 - a0) * k / 64;
                    add(g.c + Vec2{std::cos(t), std::sin(t)} * g.r);
                }
                break;
            }
        }
        if (!ok || pts.empty())
            continue;
        bool inside = true, touched = false;
        for (size_t k = 0; k < pts.size(); ++k) {
            inside = inside && box.contains(pts[k]);
            if (k + 1 < pts.size() && segHits(pts[k], pts[k + 1]))
                touched = true;
        }
        touched = touched || box.contains(pts[0]);
        if (!(crossing ? touched : inside))
            continue;
        if (g.kind == GeoKind::Point) {
            const GeoRef r{g.id, PointPos::Start};
            if (std::find(m_selPoints.begin(), m_selPoints.end(), r) == m_selPoints.end())
                m_selPoints.push_back(r);
        }
        else if (std::find(m_selCurves.begin(), m_selCurves.end(), g.id) == m_selCurves.end())
            m_selCurves.push_back(g.id);
    }
    m_ws.status(crossing ? QObject::tr("Crossing selection: %1 item(s) touched by the box")
                               .arg(m_selCurves.size() + m_selPoints.size())
                         : QObject::tr("Window selection: %1 item(s) fully inside the box")
                               .arg(m_selCurves.size() + m_selPoints.size()));
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
        // ---------------------------------------------------------- Sketcher Pro
        case ConstraintKind::Midpoint:
            if (pts.size() == 1 && crv.size() == 1)
                add.push_back(makeC(k, pts[0], {crv[0]}));
            break;
        case ConstraintKind::Symmetric: {
            // 2 points + axis, or 2 curves + axis (the axis is the last selected line)
            if (crv.empty())
                break;
            const GeoRef axis{crv.back(), PointPos::None};
            if (pts.size() == 2 && crv.size() == 1)
                add.push_back(st::makeSymmetric(m_sketch, pts[0], pts[1], axis));
            else if (pts.empty() && crv.size() == 3)
                add.push_back(st::makeSymmetric(m_sketch, {crv[0]}, {crv[1]}, axis));
            break;
        }
        case ConstraintKind::Concentric:
        case ConstraintKind::Collinear:
            for (size_t i = 1; i < crv.size(); ++i)
                add.push_back(makeC(k, {crv[0]}, {crv[i]}));
            break;
        default: break;
    }
    add.erase(std::remove_if(add.begin(), add.end(),
                             [&](const SketchConstraint& c) { return !st::checkConstraint(m_sketch, c).empty(); }),
              add.end());
    if (add.empty()) {
        static const std::map<ConstraintKind, const char*> hints{
            {ConstraintKind::Midpoint, "Midpoint: select a point and a line or an arc"},
            {ConstraintKind::Symmetric, "Symmetry: select 2 points (or 2 lines / circles / arcs), then the symmetry line"},
            {ConstraintKind::Concentric, "Concentric: select 2 circles or arcs"},
            {ConstraintKind::Collinear, "Collinear: select 2 lines"},
        };
        auto it = hints.find(k);
        m_ws.status(it != hints.end() ? QObject::tr(it->second)
                                      : QObject::tr("Select suitable geometry first (%1)").arg(toString(k)));
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
        m_ws.model()->noteUserError(why.toStdString());
    }
    changed();
}

void SketchEditor::runOffset()
{
    std::vector<Uuid> geos;
    for (const Uuid& id : m_selCurves)
        if (!isReservedGeo(id))
            geos.push_back(id);
    if (geos.empty()) {
        m_ws.status(QObject::tr("Offset: select a contour (lines, arcs, circles) first"));
        return;
    }
    setTool(Tool::Select);
    m_pending = Pending::Offset;
    m_pendingGeos = geos;
    refreshFields();
    updatePendingPreview();
    m_ws.status(QObject::tr("Offset: type the distance (negative = other side) and press Enter"));
    changed();
}

void SketchEditor::runMirror()
{
    if (m_selCurves.size() < 2 || !isLine(m_sketch, m_selCurves.back())) {
        m_ws.status(QObject::tr("Sketch mirror: select the entities, then the mirror line (or an axis) last"));
        return;
    }
    const GeoRef axis{m_selCurves.back(), PointPos::None};
    std::vector<Uuid> geos(m_selCurves.begin(), m_selCurves.end() - 1);
    const Sketch before = m_sketch;
    if (applyTool(st::mirrorCurves(m_sketch, geos, axis), before, QObject::tr("Mirrored (symmetry constraints)"))) {
        m_selCurves.clear();
        m_selPoints.clear();
    }
    changed();
}

void SketchEditor::runPattern()
{
    std::vector<Uuid> geos;
    for (const Uuid& id : m_selCurves)
        if (!isReservedGeo(id))
            geos.push_back(id);
    if (geos.empty()) {
        m_ws.status(QObject::tr("Sketch pattern: select the entities (and a center point for a circular pattern)"));
        return;
    }
    setTool(Tool::Select);
    m_pendingGeos = geos;
    if (m_selPoints.size() == 1) {
        m_pending = Pending::CircularPattern;
        m_pendingCenter = m_selPoints[0];
        m_ws.status(QObject::tr("Circular pattern: count, Tab, total angle — Enter"));
    }
    else {
        m_pending = Pending::LinearPattern;
        m_ws.status(QObject::tr("Linear pattern: count, Tab, spacing, Tab, angle — Enter "
                                "(select a center point too for a circular pattern)"));
    }
    refreshFields();
    updatePendingPreview();
    changed();
}

void SketchEditor::showUnderConstrained()
{
    solve();
    int n = 0;
    for (const SketchGeo& g : m_sketch.geos)
        if (!g.construction && !g.external() && !m_status.fullyConstrainedGeos.count(g.id))
            ++n;
    if (n == 0) {
        m_ws.status(QObject::tr("Every entity is fully constrained"));
        return;
    }
    m_ws.status(QObject::tr("%1 entity(ies) still have degrees of freedom (pulsing in blue)").arg(n));
    if (!m_pulseTimer) {
        m_pulseTimer = new QTimer(m_ws.viewport());
        m_pulseTimer->setInterval(30);
        QObject::connect(m_pulseTimer, &QTimer::timeout, m_pulseTimer, [this] {
            if (m_pulseClock.elapsed() > 1800) {
                m_pulsing = false;
                m_pulseTimer->stop();
            }
            m_ws.viewport()->refreshOverlay();
        });
    }
    m_pulsing = true;
    m_pulseClock.start();
    m_pulseTimer->start();
}

void SketchEditor::toggleConstruction()
{
    if (m_selCurves.empty())
        return;
    snapshotUndo();
    for (const Uuid& id : m_selCurves)
        if (SketchGeo* g = m_sketch.find(id); g && !g->external())
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
        if (!isReservedGeo(id))
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
    e->setText(it->expr.empty() ? QLocale::c().toString(shown, 'g', 10) : QString::fromStdString(it->expr));
    e->setToolTip(QObject::tr("Value or expression of global parameters (Ctrl+P)"));
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
        if (!t.contains('('))
            t.replace(',', '.');
        t.remove(QChar(0xB0));
        bool ok = false;
        double v = 0;
        std::string expr;  // v0.3: dimension driven by global parameters
        try {
            const auto values = parameterValues(m_ws.model()->doc().parameters);
            v = evaluateExpression(t.toStdString(), lookupIn(values));
            ok = true;
            if (!isPlainNumber(t.toStdString()))
                expr = t.toStdString();
        }
        catch (const ExpressionError& ex) {
            m_ws.status(QObject::tr("Invalid value: %1").arg(QString::fromUtf8(ex.what())));
        }
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
                if (it->kind == ConstraintKind::Radius)
                    m_filletRadius = v;  // next sketch fillet starts from the last radius typed
                const Sketch before = m_sketch;
                snapshotUndo();
                it->value = v;
                it->expr = expr;
                QString why;
                if (!solveOrRevert(m_sketch, before, m_status, {}, &why)) {
                    m_undo.pop_back();
                    m_ws.status(why);
                    m_ws.model()->noteUserError(why.toStdString());
                }
            }
        }
        else if (!ok && t.isEmpty()) {
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
    if (m_calibStage > 0)
        return calibrationClick(*lp);
    if (m_pending != Pending::None) {  // a click applies the pending command (offset / pattern)
        commitFields();
        return true;
    }
    m_snap = snapAt(*lp, e->modifiers());
    if (m_tool == Tool::Line && !m_clicks.empty() && !m_snap.point)
        m_hint = inferLineDirection(m_clicks[0], m_snap.pos);
    const Hit h = hitTest(e->position());
    dispatchClick(h, *lp, e);
    changed();
    return true;
}

bool SketchEditor::mouseMove(QMouseEvent* e)
{
    auto lp = toLocal(e->position());
    m_cursorValid = lp.has_value();
    m_cursorPx = e->position();
    if (!lp)
        return true;

    if (m_boxing) {
        m_boxTo = e->position();
        m_ws.viewport()->refreshOverlay();
        return true;
    }
    if (m_pressed) {
        if (!m_dragging && std::hypot(e->position().x() - m_pressPx.x(), e->position().y() - m_pressPx.y()) > 3) {
            m_dragging = true;
            snapshotUndo();
        }
        if (m_dragging) {
            Vec2 to = *lp;
            if (m_ws.gridSnap() && !(e->modifiers() & Qt::AltModifier) && m_dragRef.pos != PointPos::None)
                to = snapToGrid(to, m_ws.gridStep());
            DragTarget d{m_dragRef, to, m_grabOffset};
            SketchSolver::solve(m_sketch, d, false);
            m_ws.viewport()->refreshOverlay();
            return true;
        }
    }

    m_snap = snapAt(*lp, e->modifiers());
    m_hint = LineHint::None;
    if (m_tool == Tool::Line && !m_clicks.empty() && !m_snap.point)
        m_hint = inferLineDirection(m_clicks[0], m_snap.pos);
    const bool hovering = m_tool == Tool::Select || m_tool == Tool::Dimension || m_tool == Tool::Trim ||
                          m_tool == Tool::Extend || m_tool == Tool::Fillet;
    m_hover = hovering ? hitTest(e->position()) : Hit{};
    m_piece.reset();
    if ((m_tool == Tool::Trim || m_tool == Tool::Extend) && m_hover.kind == Hit::Curve && !isReservedGeo(m_hover.ref.geo))
        m_piece = m_tool == Tool::Trim ? st::trimPreview(m_sketch, m_hover.ref.geo, *lp)
                                       : st::extendPreview(m_sketch, m_hover.ref.geo, *lp);
    if (m_tool == Tool::Project)
        m_ws.setHover(m_ws.viewport()->pickAt(e->position(), SelFilter::Edges));
    updateLiveFields();
    m_ws.viewport()->refreshOverlay();
    return true;
}

bool SketchEditor::mouseRelease(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return false;
    if (m_boxing) {
        m_boxing = false;
        m_boxTo = e->position();
        if (std::abs(m_boxTo.x() - m_boxFrom.x()) > 4 || std::abs(m_boxTo.y() - m_boxFrom.y()) > 4)
            finishBoxSelect(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier));
        changed();
    }
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
    if (e->key() == Qt::Key_I && (e->modifiers() & Qt::ControlModifier) && (e->modifiers() & Qt::ShiftModifier)) {
        importVector();  // v0.7 lot B (Ctrl+Shift+I)
        return true;
    }
    if (e->key() == Qt::Key_Escape && m_calibStage > 0) {
        m_calibStage = 0;
        m_ws.status(QObject::tr("Calibration cancelled"));
        m_ws.viewport()->refreshOverlay();
        return true;
    }
    if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier))
        return false;
    if (fieldKey(e))
        return true;
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    switch (e->key()) {
        case Qt::Key_Escape:
            if (m_edit)
                closeValueEditor(false);
            else if (m_pending != Pending::None) {
                m_pending = Pending::None;
                m_previewGeos.clear();
                resetTool();
            }
            else if (!m_clicks.empty() || m_dimFirst || m_filletFirst)
                resetTool();
            else if (m_tool != Tool::Select)
                setTool(Tool::Select);
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
        case Qt::Key_R: setTool(shift ? Tool::CenterRectangle : Tool::Rectangle); return true;
        case Qt::Key_C: setTool(Tool::Circle); return true;
        case Qt::Key_A:
            if (e->modifiers() & Qt::ControlModifier)
                selectAllCurves();  // Ctrl+A
            else
                setTool(Tool::Arc);
            return true;
        case Qt::Key_U: setTool(Tool::Slot); return true;
        case Qt::Key_D: setTool(Tool::Dimension); return true;
        case Qt::Key_E: setTool(Tool::Project); return true;
        case Qt::Key_X: toggleConstruction(); return true;
        case Qt::Key_N:  // Normal To / Look At: back flat in front of the sketch plane
            lookAt();
            m_ws.status(QObject::tr("Look At (N): view normal to the sketch plane"));
            return true;
        case Qt::Key_H: applyConstraint(ConstraintKind::Horizontal); return true;
        case Qt::Key_V: applyConstraint(ConstraintKind::Vertical); return true;
        case Qt::Key_I: applyConstraint(ConstraintKind::Coincident); return true;
        case Qt::Key_P: applyConstraint(ConstraintKind::Parallel); return true;
        case Qt::Key_K: applyConstraint(ConstraintKind::Perpendicular); return true;
        case Qt::Key_T:  // Sketcher Pro: T = trim, Shift+T = tangent
            if (shift)
                applyConstraint(ConstraintKind::Tangent);
            else
                setTool(Tool::Trim);
            return true;
        case Qt::Key_Q: applyConstraint(ConstraintKind::Equal); return true;
        case Qt::Key_F:
            if (shift)
                setTool(Tool::Fillet);
            else
                applyConstraint(ConstraintKind::Fixed);
            return true;
        case Qt::Key_M: applyConstraint(ConstraintKind::Midpoint); return true;
        case Qt::Key_S: applyConstraint(ConstraintKind::Symmetric); return true;
        case Qt::Key_O:
            if (shift)
                runOffset();
            else
                applyConstraint(ConstraintKind::Concentric);
            return true;
        case Qt::Key_Delete:
        case Qt::Key_Backspace: deleteSelection(); return true;
        default: return false;
    }
}

void SketchEditor::selectAllCurves()
{
    m_selCurves.clear();
    m_selPoints.clear();
    for (const SketchGeo& g : m_sketch.geos)
        if (!isReservedGeo(g.id) && !g.external() && g.kind != GeoKind::Point)
            m_selCurves.push_back(g.id);
    changed();
}

std::vector<ConstraintKind> SketchEditor::compatibleConstraints() const
{
    int lines = 0, rounds = 0;
    for (const Uuid& id : m_selCurves)
        if (const SketchGeo* g = m_sketch.find(id))
            (g->kind == GeoKind::Line ? lines : rounds) += 1;
    const int pts = int(m_selPoints.size()), curves = lines + rounds;
    std::vector<ConstraintKind> k;
    if (pts == 0 && curves == 0)
        return k;
    if ((lines >= 1 && pts == 0 && rounds == 0) || (pts == 2 && curves == 0)) {
        k.push_back(ConstraintKind::Horizontal);
        k.push_back(ConstraintKind::Vertical);
    }
    if ((pts >= 2 && curves == 0) || (pts == 1 && curves == 1))
        k.push_back(ConstraintKind::Coincident);
    if (pts == 1 && curves == 1)
        k.push_back(ConstraintKind::Midpoint);
    if ((pts == 2 && lines == 1 && rounds == 0) || (pts == 0 && curves == 3 && lines >= 1))
        k.push_back(ConstraintKind::Symmetric);
    if (pts == 0 && lines == 2 && rounds == 0) {
        k.push_back(ConstraintKind::Parallel);
        k.push_back(ConstraintKind::Perpendicular);
        k.push_back(ConstraintKind::Collinear);
    }
    if (pts == 0 && curves == 2 && rounds >= 1)
        k.push_back(ConstraintKind::Tangent);
    if (pts == 0 && curves >= 2 && (lines == 0 || rounds == 0))
        k.push_back(ConstraintKind::Equal);
    if ((pts == 0 && rounds >= 2 && lines == 0) || (pts == 1 && rounds == 1 && lines == 0))
        k.push_back(ConstraintKind::Concentric);
    k.push_back(ConstraintKind::Fixed);
    return k;
}

IconId SketchEditor::constraintIcon(ConstraintKind k)
{
    switch (k) {
        case ConstraintKind::Horizontal: return IconId::Horizontal;
        case ConstraintKind::Vertical: return IconId::Vertical;
        case ConstraintKind::Coincident: return IconId::Coincident;
        case ConstraintKind::Parallel: return IconId::Parallel;
        case ConstraintKind::Perpendicular: return IconId::Perpendicular;
        case ConstraintKind::Tangent: return IconId::Tangent;
        case ConstraintKind::Equal: return IconId::Equal;
        case ConstraintKind::Midpoint: return IconId::Midpoint;
        case ConstraintKind::Symmetric: return IconId::Symmetry;
        case ConstraintKind::Concentric: return IconId::Concentric;
        case ConstraintKind::Collinear: return IconId::Collinear;
        default: return IconId::Fix;
    }
}

QString SketchEditor::constraintLabel(ConstraintKind k)
{
    switch (k) {
        case ConstraintKind::Horizontal: return QObject::tr("Horizontal (H)");
        case ConstraintKind::Vertical: return QObject::tr("Vertical (V)");
        case ConstraintKind::Coincident: return QObject::tr("Coincident / Co\u00efncidence (I)");
        case ConstraintKind::Parallel: return QObject::tr("Parallel / Parall\u00e8le (P)");
        case ConstraintKind::Perpendicular: return QObject::tr("Perpendicular / Perpendiculaire (K)");
        case ConstraintKind::Tangent: return QObject::tr("Tangent / Tangence (Shift+T)");
        case ConstraintKind::Equal: return QObject::tr("Equal / \u00c9gal (Q)");
        case ConstraintKind::Fixed: return QObject::tr("Fix / Fixe (F)");
        case ConstraintKind::Midpoint: return QObject::tr("Midpoint / Milieu (M)");
        case ConstraintKind::Symmetric: return QObject::tr("Symmetry / Sym\u00e9trie (S)");
        case ConstraintKind::Concentric: return QObject::tr("Concentric / Concentrique (O)");
        case ConstraintKind::Collinear: return QObject::tr("Collinear / Colin\u00e9aire");
        default: return QString();
    }
}

std::vector<MenuAction> SketchEditor::menuActions()
{
    if (const auto kinds = compatibleConstraints(); !kinds.empty()) {
        std::vector<MenuAction> a;
        for (ConstraintKind k : kinds)
            a.push_back({constraintLabel(k), icon(constraintIcon(k)), [this, k] { applyConstraint(k); }});
        a.push_back({QObject::tr("Look At (N)"), icon(IconId::Fit), [this] { lookAt(); }});
        a.push_back({QObject::tr("Delete (Del)"), icon(IconId::Cancel), [this] { deleteSelection(); }});
        a.push_back({QObject::tr("Finish sketch"), icon(IconId::Finish), [ws = &m_ws] { ws->finishSession(); }});
        return a;
    }
    return {
        {QObject::tr("Line"), icon(IconId::Line), [this] { setTool(Tool::Line); }},
        {QObject::tr("Rectangle"), icon(IconId::Rectangle), [this] { setTool(Tool::Rectangle); }},
        {QObject::tr("Circle"), icon(IconId::Circle), [this] { setTool(Tool::Circle); }},
        {QObject::tr("Arc"), icon(IconId::Arc), [this] { setTool(Tool::Arc); }},
        {QObject::tr("Slot"), icon(IconId::Slot), [this] { setTool(Tool::Slot); }},
        {QObject::tr("Trim"), icon(IconId::Trim), [this] { setTool(Tool::Trim); }},
        {QObject::tr("Offset"), icon(IconId::Offset), [this] { runOffset(); }},
        {QObject::tr("Dimension"), icon(IconId::Dimension), [this] { setTool(Tool::Dimension); }},
        {QObject::tr("Look At (N)"), icon(IconId::Fit), [this] { lookAt(); }},
        {QObject::tr("Construction"), icon(IconId::Construction), [this] { toggleConstruction(); }},
        {QObject::tr("Import SVG/DXF (Ctrl+Shift+I)"), icon(IconId::ImportVector), [this] { importVector(); }},
        m_canvas ? MenuAction{QObject::tr("Calibrate image (2 points)"), icon(IconId::Calibrate), [this] { startCalibration(); }}
                 : MenuAction{QObject::tr("Insert Image (Canvas)"), icon(IconId::Canvas), [this] { insertCanvas(); }},
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
    {
        // v0.4 lot 3: "why is it still blue?" — plain-language coach
        auto* row = new QHBoxLayout();
        row->addWidget(m_statusLabel, 1);
        auto* why = new QPushButton(icon(IconId::Hint), m_ws.model()->pedagogyFrench() ? QObject::tr("Expliquer") : QObject::tr("Explain"), w);
        why->setToolTip(QObject::tr("What is still free in this sketch, and which tool fixes it"));
        QObject::connect(why, &QPushButton::clicked, why, [this] { explain(); });
        row->addWidget(why);
        auto* look = new QPushButton(icon(IconId::Fit), QString(), w);  // icon only: the panel stays narrow
        look->setToolTip(QObject::tr("Look At / Vue normale (N): back flat in front of the sketch plane"));
        QObject::connect(look, &QPushButton::clicked, look, [this] { lookAt(); });
        row->addWidget(look);
        lay->addLayout(row);
        m_coachLabel = new QLabel(w);
        m_coachLabel->setWordWrap(true);
        m_coachLabel->setStyleSheet("background: #1d2a1f; color: #cfe8d2; border-radius: 4px; padding: 6px;");
        m_coachLabel->hide();
        lay->addWidget(m_coachLabel);
    }

    // v0.3: offset of the sketch plane along its normal (loft sections, stacked profiles...)
    {
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(QObject::tr("Plane offset"), w));
        auto* off = new ExprSpinBox(w);
        off->setRange(-1e5, 1e5);
        off->setDecimals(2);
        off->setSuffix(" mm");
        off->setValueAndExpression(m_planeRef.offset, {});
        off->setToolTip(QObject::tr("Shifts the sketch plane along its normal"));
        QObject::connect(off, &QDoubleSpinBox::valueChanged, off, [this](double v) {
            const double delta = v - m_planeRef.offset;
            if (std::abs(delta) < 1e-12)
                return;
            m_planeRef.offset = v;
            m_plane = offsetPlacement(m_plane, delta);
            m_ws.viewport()->update();
            m_ws.viewport()->refreshOverlay();
        });
        row->addWidget(off, 1);
        lay->addLayout(row);
    }

    auto* create = new QGroupBox(QObject::tr("Create"), w);
    auto* cg = new QGridLayout(create);
    m_toolButtons.clear();
    auto toolButton = [&](QWidget* parent, IconId ic, const QString& label, const QString& key, Tool tool) {
        auto* b = new QToolButton(parent);
        b->setIcon(icon(ic));
        b->setText(label);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        b->setCheckable(true);
        b->setToolTip(key.isEmpty() ? label : QString("%1 (%2)").arg(label, key));
        b->setProperty("silexTool", int(tool));  // drop-down buttons change it
        QObject::connect(b, &QToolButton::clicked, b, [this, b] { setTool(Tool(b->property("silexTool").toInt())); });
        m_toolButtons.emplace_back(tool, b);
        return b;
    };
    /// main action + drop-down menu of variants (the button shows the last variant used)
    auto dropButton = [&](QWidget* parent, std::vector<std::tuple<Tool, IconId, QString, QString>> variants) {
        auto* b = toolButton(parent, std::get<1>(variants[0]), std::get<2>(variants[0]), std::get<3>(variants[0]),
                             std::get<0>(variants[0]));
        b->setPopupMode(QToolButton::MenuButtonPopup);
        auto* menu = new QMenu(b);
        for (const auto& [tool, ic, label, key] : variants) {
            QAction* act = menu->addAction(icon(ic), key.isEmpty() ? label : QString("%1\t%2").arg(label, key));
            QObject::connect(act, &QAction::triggered, b, [this, b, tool = tool, ic = ic, label = label] {
                b->setIcon(icon(ic));
                b->setText(label);
                b->setProperty("silexTool", int(tool));
                setTool(tool);
            });
            if (tool != std::get<0>(variants[0]))
                m_toolButtons.emplace_back(tool, b);
        }
        b->setMenu(menu);
        return b;
    };
    cg->addWidget(toolButton(create, IconId::Fit, QObject::tr("Select"), "Esc", Tool::Select), 0, 0);
    cg->addWidget(toolButton(create, IconId::Line, QObject::tr("Line"), "L", Tool::Line), 0, 1);
    cg->addWidget(dropButton(create, {{Tool::Rectangle, IconId::Rectangle, QObject::tr("Rectangle"), "R"},
                                      {Tool::CenterRectangle, IconId::CenterRectangle, QObject::tr("Center rectangle"), "Shift+R"},
                                      {Tool::Polygon, IconId::Polygon, QObject::tr("Polygon"), ""}}),
                  1, 0);
    cg->addWidget(toolButton(create, IconId::Circle, QObject::tr("Circle"), "C", Tool::Circle), 1, 1);
    cg->addWidget(toolButton(create, IconId::Arc, QObject::tr("Arc"), "A", Tool::Arc), 2, 0);
    cg->addWidget(toolButton(create, IconId::Slot, QObject::tr("Slot"), "U", Tool::Slot), 2, 1);
    cg->addWidget(toolButton(create, IconId::Dimension, QObject::tr("Dimension"), "D", Tool::Dimension), 3, 0);
    cg->addWidget(toolButton(create, IconId::Project, QObject::tr("Project"), "E", Tool::Project), 3, 1);
    {
        // polygon & slot options
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(QObject::tr("Polygon"), create));
        auto* sides = new QSpinBox(create);
        sides->setRange(3, 12);
        sides->setValue(m_polySides);
        sides->setSuffix(QObject::tr(" sides"));
        QObject::connect(sides, &QSpinBox::valueChanged, sides, [this](int n) { m_polySides = n; });
        row->addWidget(sides);
        auto* mode = new QComboBox(create);
        mode->addItems({QObject::tr("Inscribed"), QObject::tr("Circumscribed")});
        mode->setCurrentIndex(m_polyInscribed ? 0 : 1);
        mode->setToolTip(QObject::tr("Inscribed: click a vertex. Circumscribed: click the middle of a side "
                                     "(construction circle tangent to the sides)"));
        QObject::connect(mode, &QComboBox::currentIndexChanged, mode, [this](int i) {
            m_polyInscribed = i == 0;
            m_fieldStage = -1;
            refreshFields();
        });
        row->addWidget(mode, 1);
        cg->addLayout(row, 4, 0, 1, 2);
        auto* row2 = new QHBoxLayout();
        row2->addWidget(new QLabel(QObject::tr("Slot"), create));
        auto* slot = new QComboBox(create);
        slot->addItems({QObject::tr("Center to center"), QObject::tr("Overall center")});
        slot->setCurrentIndex(m_slotOverall ? 1 : 0);
        QObject::connect(slot, &QComboBox::currentIndexChanged, slot, [this](int i) {
            m_slotOverall = i == 1;
            if (m_tool == Tool::Slot)
                setTool(Tool::Slot);
        });
        row2->addWidget(slot, 1);
        cg->addLayout(row2, 5, 0, 1, 2);
    }
    lay->addWidget(create);

    auto* modify = new QGroupBox(QObject::tr("Modify"), w);
    auto* mg = new QGridLayout(modify);
    mg->addWidget(dropButton(modify, {{Tool::Trim, IconId::Trim, QObject::tr("Trim"), "T"},
                                      {Tool::Extend, IconId::Extend, QObject::tr("Extend"), ""}}),
                  0, 0);
    mg->addWidget(toolButton(modify, IconId::SketchFillet, QObject::tr("Fillet"), "Shift+F", Tool::Fillet), 0, 1);
    auto command = [&](IconId ic, const QString& label, const QString& tip, std::function<void()> fn) {
        auto* b = new QToolButton(modify);
        b->setIcon(icon(ic));
        b->setText(label);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        b->setToolTip(tip);
        QObject::connect(b, &QToolButton::clicked, b, fn);
        return b;
    };
    mg->addWidget(command(IconId::Offset, QObject::tr("Offset"), QObject::tr("Offset the selected contour (Shift+O)"),
                          [this] { runOffset(); }),
                  1, 0);
    mg->addWidget(command(IconId::SketchMirror, QObject::tr("Mirror"),
                          QObject::tr("Mirror the selection about the line selected last"), [this] { runMirror(); }),
                  1, 1);
    mg->addWidget(command(IconId::SketchPattern, QObject::tr("Pattern"),
                          QObject::tr("Repeat the selection (linear; circular when a center point is selected)"),
                          [this] { runPattern(); }),
                  2, 0);
    lay->addWidget(modify);

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
                    {ConstraintKind::Tangent, IconId::Tangent, "Tangent", "Shift+T"},
                    {ConstraintKind::Equal, IconId::Equal, "Equal", "Q"},
                    {ConstraintKind::Fixed, IconId::Fix, "Fix", "F"},
                    {ConstraintKind::Midpoint, IconId::Midpoint, "Midpoint (point + line / arc)", "M"},
                    {ConstraintKind::Symmetric, IconId::Symmetry, "Symmetry (2 entities, then the axis)", "S"},
                    {ConstraintKind::Concentric, IconId::Concentric, "Concentric", "O"},
                    {ConstraintKind::Collinear, IconId::Collinear, "Collinear", ""}};
    int i = 0;
    for (const K& k : ks) {
        auto* b = new QPushButton(icon(k.icon), "", cons);
        b->setToolTip(constraintLabel(k.k) + "<br><small>" + QObject::tr(k.label) + "</small>");
        b->setIconSize(QSize(20, 20));
        QObject::connect(b, &QPushButton::clicked, b, [this, kind = k.k] { applyConstraint(kind); });
        kg->addWidget(b, i / 4, i % 4);
        ++i;
    }
    auto* constr = new QPushButton(icon(IconId::Construction), QObject::tr("Construction"), cons);
    constr->setToolTip(QObject::tr("Toggle construction geometry (X)"));
    QObject::connect(constr, &QPushButton::clicked, constr, [this] { toggleConstruction(); });
    kg->addWidget(constr, 3, 0, 1, 4);
    auto* under = new QPushButton(icon(IconId::UnderConstrained), QObject::tr("Show under-constrained"), cons);
    under->setToolTip(QObject::tr("Pulses the entities that still have degrees of freedom"));
    QObject::connect(under, &QPushButton::clicked, under, [this] { showUnderConstrained(); });
    kg->addWidget(under, 4, 0, 1, 4);
    lay->addWidget(cons);

    auto* help = new QLabel(QObject::tr("<small>While drawing, type a value then <b>Tab</b> / <b>Enter</b>: the "
                                        "dimension is created. Auto-constraints: snap to points and curves, "
                                        "horizontal / vertical, tangent. Hover a constraint badge to see what it "
                                        "links, click it + Del to remove it. Magnetic grid: status bar (hold Alt to "
                                        "bypass).</small>"),
                            w);
    help->setWordWrap(true);
    help->setStyleSheet("color: #9aa3ad;");
    lay->addWidget(help);
    // v0.7: vector import and calibrated reference image
    {
        auto* box = new QGroupBox(QObject::tr("Import / reference image"), w);
        auto* g = new QGridLayout(box);
        auto button = [&](IconId ic, const QString& text, const QString& tip, std::function<void()> fn) {
            auto* b = new QToolButton(box);
            b->setIcon(icon(ic));
            b->setText(text);
            b->setToolTip(tip);
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            QObject::connect(b, &QToolButton::clicked, b, fn);
            return b;
        };
        g->addWidget(button(IconId::ImportVector, QObject::tr("Import SVG/DXF"),
                            QObject::tr("Logo, profile or laser-cut drawing as sketch curves (Ctrl+Shift+I)"),
                            [this] { importVector(); }), 0, 0);
        g->addWidget(button(IconId::Canvas, QObject::tr("Insert Image"),
                            QObject::tr("Photo or scan under the sketch (Canvas), to redraw a part"),
                            [this] { insertCanvas(); }), 0, 1);
        g->addWidget(button(IconId::Calibrate, QObject::tr("Calibrate"),
                            QObject::tr("Click two points of known distance on the image, then type the distance"),
                            [this] { startCalibration(); }), 1, 0);
        g->addWidget(button(IconId::Cancel, QObject::tr("Remove image"), QObject::tr("Removes the reference image"),
                            [this] { removeCanvas(); }), 1, 1);
        auto* op = new QSlider(Qt::Horizontal, box);
        op->setRange(5, 100);
        op->setValue(m_canvas ? int(std::lround(m_canvas->opacity * 100)) : 50);
        op->setToolTip(QObject::tr("Opacity of the reference image"));
        QObject::connect(op, &QSlider::valueChanged, op, [this](int v) {
            if (m_canvas) {
                m_canvas->opacity = v / 100.0;
                m_ws.viewport()->refreshOverlay();
            }
        });
        g->addWidget(new QLabel(QObject::tr("Opacity"), box), 2, 0);
        g->addWidget(op, 2, 1);
        auto* flipH = new QCheckBox(QObject::tr("Flip H"), box);
        auto* flipV = new QCheckBox(QObject::tr("Flip V"), box);
        flipH->setChecked(m_canvas && m_canvas->flipH);
        flipV->setChecked(m_canvas && m_canvas->flipV);
        QObject::connect(flipH, &QCheckBox::toggled, flipH, [this](bool on) {
            if (m_canvas) {
                m_canvas->flipH = on;
                m_ws.viewport()->refreshOverlay();
            }
        });
        QObject::connect(flipV, &QCheckBox::toggled, flipV, [this](bool on) {
            if (m_canvas) {
                m_canvas->flipV = on;
                m_ws.viewport()->refreshOverlay();
            }
        });
        g->addWidget(flipH, 3, 0);
        g->addWidget(flipV, 3, 1);
        lay->addWidget(box);
    }

    lay->addStretch(1);
    auto* keys = new QLabel(QObject::tr("<small><b>Sketch shortcuts</b><br>"
                                        "L line \u00b7 R rectangle \u00b7 Shift+R center rect. \u00b7 C circle \u00b7 A arc \u00b7 U slot<br>"
                                        "D dimension \u00b7 T trim \u00b7 Shift+F fillet \u00b7 Shift+O offset \u00b7 X construction<br>"
                                        "H/V \u00b7 I coincident \u00b7 P parallel \u00b7 K perpendicular \u00b7 Shift+T tangent<br>"
                                        "Q equal \u00b7 F fix \u00b7 M midpoint \u00b7 S symmetry \u00b7 O concentric<br>"
                                        "N look at \u00b7 drag in the void: box (\u2192 window, \u2190 crossing)<br>"
                                        "Right-click a selection: compatible constraints \u00b7 Enter: finish</small>"),
                            w);
    keys->setWordWrap(true);
    keys->setStyleSheet("color: #9aa3ad;");
    lay->addWidget(keys);

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

void SketchEditor::explain()
{
    const auto items = explainSketch(m_sketch, m_ws.model()->pedagogyFrench());
    QStringList lines;
    for (const auto& it : items)
        lines << QString::fromStdString(it.text);
    if (m_coachLabel) {
        m_coachLabel->setText(QString::fromUtf8("\xF0\x9F\x92\xA1 ") + lines.join("\n\n"));
        m_coachLabel->show();
    }
    m_ws.status(lines.isEmpty() ? QString() : lines.front(), 8000);
    if (std::any_of(items.begin(), items.end(), [](const CoachItem& i) { return i.dof > 0; }))
        showUnderConstrained();  // the blue entities pulse
}

void SketchEditor::updatePanel()
{
    for (auto& [tool, b] : m_toolButtons)
        if (b)
            b->setChecked(false);
    for (auto& [tool, b] : m_toolButtons)
        if (b && tool == m_tool)
            b->setChecked(true);
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
    m_statusLabel->setToolTip(m_ws.model()->pedagogyFrench() ? QObject::tr("Clique « Expliquer » pour savoir ce qui manque")
                                                             : QObject::tr("Click Explain to know what is missing"));
    if (m_coachLabel && m_coachLabel->isVisible()) {
        QStringList lines;
        for (const auto& it : explainSketch(m_sketch, m_ws.model()->pedagogyFrench()))
            lines << QString::fromStdString(it.text);
        m_coachLabel->setText(QString::fromUtf8("\xF0\x9F\x92\xA1 ") + lines.join("\n\n"));
    }
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
        case ConstraintKind::Symmetric: {
            auto mid = [](const SketchGeo& g, PointPos pos) {
                if (pos != PointPos::None || g.kind == GeoKind::Point)
                    return g.point(pos == PointPos::None ? PointPos::Start : pos);
                return g.kind == GeoKind::Line ? (g.p1 + g.p2) * 0.5 : g.c;
            };
            at = gb ? (mid(*ga, c.a.pos) + mid(*gb, c.b.pos)) * 0.5 + Vec2{0, off * 0.6} : mid(*ga, c.a.pos);
            break;
        }
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
    auto axisPen = [&](const Uuid& id, QColor c) {
        const bool sel = std::find(m_selCurves.begin(), m_selCurves.end(), id) != m_selCurves.end() ||
                         (m_dimFirst && m_dimFirst->kind == Hit::Curve && m_dimFirst->ref.geo == id);
        const bool hov = m_hover.kind == Hit::Curve && m_hover.ref.geo == id;
        if (sel)
            return QPen(Colors::SketchSelected, 3.0, Qt::DashLine);
        c.setAlpha(hov ? 230 : 70);
        return QPen(c, hov ? 2.2 : 1, Qt::DashLine);
    };
    p.setPen(axisPen(kXAxisId, QColor(0xe0, 0x60, 0x60)));
    if (toScreen({-L, 0}, &a) && toScreen({L, 0}, &b))
        p.drawLine(a, b);
    p.setPen(axisPen(kYAxisId, QColor(0x70, 0xc8, 0x70)));
    if (toScreen({0, -L}, &a) && toScreen({0, L}, &b))
        p.drawLine(a, b);

    // entities linked by the hovered / selected constraint badge
    std::set<Uuid> linked;
    if (m_hover.kind == Hit::Constraint)
        linked = constraintGeos(m_hover.constraint);
    for (const Uuid& c : m_selConstraints)
        for (const Uuid& g : constraintGeos(c))
            linked.insert(g);
    if (linked.count(kXAxisId) || linked.count(kYAxisId)) {
        p.setPen(QPen(Colors::Hover, 2.4, Qt::DashLine));
        if (linked.count(kXAxisId) && toScreen({-L, 0}, &a) && toScreen({L, 0}, &b))
            p.drawLine(a, b);
        if (linked.count(kYAxisId) && toScreen({0, -L}, &a) && toScreen({0, L}, &b))
            p.drawLine(a, b);
    }
    // "show under-constrained": free entities pulse in blue
    const double pulse = m_pulsing ? 0.5 + 0.5 * std::sin(m_pulseClock.elapsed() / 1000.0 * 2 * kPi * 2.5) : 0.0;

    for (const SketchGeo& g : m_sketch.geos) {
        const bool sel = std::find(m_selCurves.begin(), m_selCurves.end(), g.id) != m_selCurves.end();
        const bool hov = (m_hover.kind == Hit::Curve && m_hover.ref.geo == g.id) || linked.count(g.id) > 0;
        const bool dimFirst = m_dimFirst && m_dimFirst->kind == Hit::Curve && m_dimFirst->ref.geo == g.id;
        QColor c = colorOf(g);
        if (sel || dimFirst)
            c = Colors::SketchSelected;
        else if (hov)
            c = Colors::Hover;
        QPen pen(c, sel || dimFirst ? 3.0 : (hov ? 2.6 : 1.5), g.construction ? Qt::DashLine : Qt::SolidLine, Qt::RoundCap);
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
        if (m_pulsing && !g.construction && !g.external() && !m_status.fullyConstrainedGeos.count(g.id)) {
            QColor glow = Colors::SketchUnder;
            glow.setAlphaF(0.25 + 0.55 * pulse);
            p.setPen(QPen(glow, 5 + 5 * pulse, Qt::SolidLine, Qt::RoundCap));
            p.drawPath(path);
            p.setPen(pen);
        }
        if (linked.count(g.id) && g.kind == GeoKind::Point) {
            QPointF q;
            if (toScreen(g.p1, &q)) {
                p.setPen(QPen(Colors::Hover, 2));
                p.drawEllipse(q, 7, 7);
                p.setPen(pen);
            }
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
        double r = sel || first ? 6.0 : (hov ? 4.5 : 3.0);
        if (m_pulsing && !isReservedGeo(ref.geo) && !m_status.fullyConstrainedGeos.count(ref.geo)) {
            const SketchGeo* g = m_sketch.find(ref.geo);
            if (g && !g->construction && !g->external()) {
                QColor glow = Colors::SketchUnder;
                glow.setAlphaF(0.3 + 0.5 * pulse);
                p.setPen(Qt::NoPen);
                p.setBrush(glow);
                p.drawEllipse(s, 5 + 5 * pulse, 5 + 5 * pulse);
                r = 4.0;
            }
        }
        p.setPen(Qt::NoPen);
        p.setBrush(sel || first ? Colors::SketchSelected : (hov ? Colors::Hover : c));
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

    std::map<std::pair<int, int>, double> stack;  // badges sharing an anchor are laid side by side
    for (const SketchConstraint& c : m_sketch.constraints) {
        const bool bad = m_status.conflicting.count(c.id) || m_status.redundant.count(c.id);
        const bool sel = std::find(m_selConstraints.begin(), m_selConstraints.end(), c.id) != m_selConstraints.end();
        const bool hov = m_hover.kind == Hit::Constraint && m_hover.constraint == c.id;
        QColor col = bad ? Colors::SketchOver : (isDimensional(c.kind) ? Colors::Dimension : Colors::Constraint);
        if (sel)
            col = Colors::SketchSelected;
        else if (hov)
            col = col.lighter(130);

        if (c.kind == ConstraintKind::Coincident)
            continue;  // shown by the shared point itself
        if (!c.group.empty() && !bad)
            continue;  // v0.7: internal dimension of a rigid block (the block shows one badge)
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
            case ConstraintKind::Midpoint: text = "Mid"; break;
            case ConstraintKind::Symmetric: text = "Sym"; break;
            case ConstraintKind::Concentric: text = QString(QChar(0x25CE)); break;
            case ConstraintKind::Collinear: text = "Col"; break;
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
        if (!isDimensional(c.kind)) {
            double& used = stack[{int(std::lround(at.x() / 6)), int(std::lround(at.y() / 6))}];
            at.rx() += used;
            used += ts.width() + 3;
        }
        const QRectF r(at.x() - ts.width() / 2, at.y() - ts.height() / 2, ts.width(), ts.height());
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(20, 24, 30, isDimensional(c.kind) ? 220 : 170));
        if (hov && !isDimensional(c.kind)) {
            p.setPen(QPen(Colors::Hover, 1.2));
        }
        p.drawRoundedRect(r, 3, 3);
        p.setPen(col);
        p.drawText(r, Qt::AlignCenter, text);
        m_labelRects.emplace_back(r, c.id);
    }
    p.setBrush(Qt::NoBrush);
}

void SketchEditor::paintPiece(QPainter& p, const st::CurvePiece& piece, const QPen& pen)
{
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    QPainterPath path;
    if (piece.kind == GeoKind::Line) {
        QPointF a, b;
        if (toScreen(piece.p1, &a) && toScreen(piece.p2, &b)) {
            path.moveTo(a);
            path.lineTo(b);
        }
    }
    else {
        const double a0 = piece.kind == GeoKind::Circle ? 0 : piece.a0, a1 = piece.kind == GeoKind::Circle ? 2 * kPi : piece.a1;
        const int n = std::max(16, int(std::abs(a1 - a0) / (2 * kPi) * 96));
        for (int k = 0; k <= n; ++k) {
            const double t = a0 + (a1 - a0) * k / n;
            QPointF q;
            if (!toScreen(piece.c + Vec2{std::cos(t), std::sin(t)} * piece.r, &q))
                return;
            k == 0 ? path.moveTo(q) : path.lineTo(q);
        }
    }
    p.drawPath(path);
}

void SketchEditor::paintToolPreview(QPainter& p)
{
    // pending offset / pattern: dashed preview of the result
    for (const SketchGeo& g : m_previewGeos) {
        st::CurvePiece piece;
        piece.kind = g.kind;
        piece.p1 = g.p1;
        piece.p2 = g.p2;
        piece.c = g.c;
        piece.r = g.r;
        piece.a0 = g.a0;
        piece.a1 = g.a1;
        if (g.kind != GeoKind::Point)
            paintPiece(p, piece, QPen(Colors::Hover, 1.6, Qt::DashLine));
    }
    // trim: portion removed (red dotted); extend: resulting curve (green dashed)
    if (m_piece)
        paintPiece(p, *m_piece,
                   m_tool == Tool::Trim ? QPen(Colors::SketchOver, 3.2, Qt::DotLine, Qt::RoundCap)
                                        : QPen(QColor(0x5c, 0xd6, 0x8a), 2.2, Qt::DashLine));
    if (m_tool == Tool::Fillet && m_filletFirst)
        if (const SketchGeo* g = m_sketch.find(*m_filletFirst)) {
            st::CurvePiece piece;
            piece.p1 = g->p1;
            piece.p2 = g->p2;
            paintPiece(p, piece, QPen(Colors::Accent, 2.8));
        }

    if (!m_cursorValid || m_tool == Tool::Select || m_tool == Tool::Trim || m_tool == Tool::Extend ||
        m_tool == Tool::Fillet || m_pending != Pending::None)
        return;
    const Vec2 cur = constrainedCursor();
    QPen pen(Colors::Hover, 1.6, Qt::SolidLine, Qt::RoundCap);
    QPen construction(Colors::SketchConstruction, 1.1, Qt::DashLine);
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
    auto seg = [&](Vec2 u, Vec2 v) {
        if (toScreen(u, &a) && toScreen(v, &b))
            p.drawLine(a, b);
    };
    auto label = [&](Vec2 at, const QString& t) {
        if (m_fields.empty() && toScreen(at, &b)) {
            p.setPen(Colors::Dimension);
            p.drawText(b + QPointF(14, -10), t);
        }
    };
    auto num = [](double v) { return QLocale::c().toString(v, 'f', 2); };

    if (!m_clicks.empty()) {
        const Vec2 s = m_clicks[0];
        switch (m_tool) {
            case Tool::Line: {
                const Vec2 e = cur;
                if (toScreen(s, &a) && toScreen(e, &b)) {
                    p.drawLine(a, b);
                    label(e, num((e - s).length()));
                    if (m_hint != LineHint::None && !fieldValue("angle")) {
                        p.setPen(Colors::Constraint);
                        p.drawText((a + b) / 2 + QPointF(8, -8), m_hint == LineHint::Horizontal ? "H" : "V");
                    }
                }
                break;
            }
            case Tool::Rectangle: {
                const Vec2 q[4] = {s, {cur.x, s.y}, cur, {s.x, cur.y}};
                for (int i = 0; i < 4; ++i)
                    seg(q[i], q[(i + 1) % 4]);
                label(cur, QString("%1 x %2").arg(num(std::abs(cur.x - s.x)), num(std::abs(cur.y - s.y))));
                break;
            }
            case Tool::CenterRectangle: {
                const Vec2 d{std::abs(cur.x - s.x), std::abs(cur.y - s.y)};
                const Vec2 q[4] = {s + Vec2{-d.x, -d.y}, s + Vec2{d.x, -d.y}, s + d, s + Vec2{-d.x, d.y}};
                for (int i = 0; i < 4; ++i)
                    seg(q[i], q[(i + 1) % 4]);
                p.setPen(construction);
                seg(q[0], q[2]);
                seg(q[1], q[3]);
                p.setPen(pen);
                label(cur, QString("%1 x %2").arg(num(2 * d.x), num(2 * d.y)));
                break;
            }
            case Tool::Polygon: {
                const int n = std::clamp(int(std::lround(fieldValue("sides").value_or(m_polySides))), 3, 64);
                const double r0 = (cur - s).length();
                double a0 = std::atan2(cur.y - s.y, cur.x - s.x), R = r0;
                if (!m_polyInscribed) {
                    R = r0 / std::cos(kPi / n);
                    a0 -= kPi / n;
                }
                for (int k = 0; k < n; ++k)
                    seg(s + Vec2{std::cos(a0 + 2 * kPi * k / n), std::sin(a0 + 2 * kPi * k / n)} * R,
                        s + Vec2{std::cos(a0 + 2 * kPi * (k + 1) / n), std::sin(a0 + 2 * kPi * (k + 1) / n)} * R);
                p.setPen(construction);
                circle(s, r0, 0, 2 * kPi);
                break;
            }
            case Tool::Circle: {
                const double r = (cur - s).length();
                circle(s, r, 0, 2 * kPi);
                label(cur, QString(QChar(0x2300)) + num(2 * r));
                break;
            }
            case Tool::Arc: {
                if (m_clicks.size() == 1)
                    seg(s, cur);
                else {
                    const Vec2 stp = m_clicks[1];
                    const double r = (stp - s).length();
                    double a0 = std::atan2(stp.y - s.y, stp.x - s.x), a1 = std::atan2(cur.y - s.y, cur.x - s.x);
                    while (a1 <= a0)
                        a1 += 2 * kPi;
                    circle(s, r, a0, a1);
                }
                break;
            }
            case Tool::Slot: {
                Vec2 c1 = s, c2 = cur, w = cur;
                if (m_clicks.size() >= 2) {
                    c2 = m_clicks[1];
                    w = cur;
                }
                if (m_slotOverall)
                    c1 = s * 2.0 - c2;
                p.setPen(construction);
                seg(c1, c2);
                p.setPen(pen);
                if (m_clicks.size() >= 2 && (c2 - c1).length() > 1e-9) {
                    const Vec2 u = (c2 - c1).normalized(), n{-u.y, u.x};
                    const double r = std::abs(u.cross(w - c1));
                    const double phi = std::atan2(u.y, u.x);
                    seg(c1 - n * r, c2 - n * r);
                    seg(c2 + n * r, c1 + n * r);
                    circle(c1, r, phi + kPi / 2, phi + 3 * kPi / 2);
                    circle(c2, r, phi - kPi / 2, phi + kPi / 2);
                }
                break;
            }
            default: break;
        }
    }

    // snap indicator
    if (toScreen(m_snap.pos, &a)) {
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

void SketchEditor::paintGrid(QPainter& p)
{
    if (!m_ws.gridSnap() || !m_cursorValid)
        return;
    const double mpp = modelPerPixel();
    double step = m_ws.gridStep();
    while (step / mpp < 9)
        step *= 5;  // keep the dots readable when zoomed out
    auto c = toLocal(m_cursorPx);
    if (!c)
        return;
    const double R = 140 * mpp;  // dots around the cursor only: light and fast
    const int i0 = int(std::floor((c->x - R) / step)), i1 = int(std::ceil((c->x + R) / step));
    const int j0 = int(std::floor((c->y - R) / step)), j1 = int(std::ceil((c->y + R) / step));
    if ((i1 - i0) * (j1 - j0) > 6000)
        return;
    p.setPen(Qt::NoPen);
    for (int i = i0; i <= i1; ++i)
        for (int j = j0; j <= j1; ++j) {
            const Vec2 g{i * step, j * step};
            const double d = (g - *c).length() / R;
            if (d > 1)
                continue;
            QPointF q;
            if (!toScreen(g, &q))
                continue;
            p.setBrush(QColor(255, 255, 255, int(110 * (1 - d))));
            p.drawEllipse(q, 1.3, 1.3);
        }
    p.setBrush(Qt::NoBrush);
}

void SketchEditor::paintFields(QPainter& p)
{
    if (m_fields.empty() || !m_cursorValid)
        return;
    QFont f = p.font();
    f.setPixelSize(11);
    f.setBold(true);
    p.setFont(f);
    const QFontMetricsF fm(f);
    QPointF at = m_cursorPx + QPointF(22, 18);
    for (size_t i = 0; i < m_fields.size(); ++i) {
        const Field& fl = m_fields[i];
        const bool active = int(i) == m_activeField;
        const QString value = !fl.text.isEmpty() ? fl.text
                              : fl.integer       ? QString::number(int(std::lround(fl.live)))
                                                 : QLocale::c().toString(fl.live, 'f', fl.angle ? 1 : 2);
        const QString unit = fl.integer ? QString() : fl.angle ? QString(QChar(0xB0)) : QString(" mm");
        const QString txt = QString("%1  %2%3").arg(fl.label, value, unit);
        const QSizeF ts = fm.size(0, txt) + QSizeF(14, 6);
        const QRectF r(at, ts);
        p.setPen(QPen(active ? QColor(0x2f, 0x9b, 0xff) : QColor(80, 88, 98), active ? 1.4 : 1));
        p.setBrush(QColor(16, 21, 27, 225));
        p.drawRoundedRect(r, 3, 3);
        p.setPen(fl.text.isEmpty() ? QColor(170, 178, 188) : QColor(255, 255, 255));
        p.drawText(r, Qt::AlignCenter, txt);
        at.ry() += ts.height() + 3;
    }
    f.setBold(false);
    f.setPixelSize(10);
    p.setFont(f);
    p.setPen(QColor(150, 158, 168));
    p.drawText(at + QPointF(2, 10), QObject::tr("type a value · Tab next · Enter apply"));
    p.setBrush(Qt::NoBrush);
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
    static const char* toolNames[] = {"Select", "Line",      "Rectangle", "Center rectangle", "Polygon",
                                      "Circle", "Arc",       "Slot",      "Dimension",        "Project",
                                      "Trim",   "Extend",    "Fillet"};
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
                              : QObject::tr("Click a line, circle, arc, point or an axis");
            break;
        case Tool::Project: hint = QObject::tr("Click a model edge: projected in purple, linked to the model"); break;
        case Tool::CenterRectangle:
            hint = m_clicks.empty() ? QObject::tr("Click the center") : QObject::tr("Click a corner — or type width, Tab, height");
            break;
        case Tool::Polygon:
            hint = m_clicks.empty() ? QObject::tr("Click the center")
                   : m_polyInscribed ? QObject::tr("Click a vertex — or type sides, Tab, radius")
                                     : QObject::tr("Click the middle of a side — or type sides, Tab, apothem");
            break;
        case Tool::Slot:
            hint = m_clicks.empty() ? (m_slotOverall ? QObject::tr("Click the center of the slot") : QObject::tr("Click the first center"))
                   : m_clicks.size() == 1 ? (m_slotOverall ? QObject::tr("Click the end of the axis") : QObject::tr("Click the second center"))
                                          : QObject::tr("Click to set the width");
            break;
        case Tool::Trim: hint = QObject::tr("Click the portion to remove (red dotted)"); break;
        case Tool::Extend: hint = QObject::tr("Click near the end to extend to the next curve"); break;
        case Tool::Fillet:
            hint = m_filletFirst ? QObject::tr("Click the second line") : QObject::tr("Click a corner, or two lines");
            break;
        case Tool::Select: hint = QObject::tr("Drag geometry · double-click a dimension to edit · Enter to finish"); break;
    }
    QFont f = p.font();
    f.setPixelSize(12);
    f.setBold(true);
    p.setFont(f);
    const QString head = QString("%1  ·  %2").arg(QString::fromStdString(m_name), QObject::tr(toolNames[int(m_tool)]));
    QRectF box(12, 12, 400, 52);
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
    paintCanvas(p);  // v0.7: the reference image lies under the grid and the sketch
    paintGrid(p);
    paintGeometry(p);
    paintConstraints(p);
    paintToolPreview(p);
    paintFields(p);
    paintStatus(p);
    if (m_boxing)
        paintSelectionBox(p, m_boxFrom, m_boxTo);
    if (m_edit) {
        // keep the inline editor glued to its dimension while the camera moves
        auto it = std::find_if(m_sketch.constraints.begin(), m_sketch.constraints.end(),
                               [&](const SketchConstraint& c) { return c.id == m_editConstraint; });
        QPointF at;
        if (it != m_sketch.constraints.end() && labelAnchor(*it, &at))
            m_edit->move(int(at.x() - m_edit->width() / 2), int(at.y() - m_edit->height() / 2));
    }
}

// ------------------------------------------------------------------ v0.7: reference image (Canvas)

void SketchEditor::paintCanvas(QPainter& p)
{
    if (!m_canvas || !m_canvas->visible || m_canvas->pxWidth <= 0 || m_canvas->pxHeight <= 0)
        return;
    const size_t key = std::hash<std::string>{}(m_canvas->imageData);
    if (key != m_canvasKey) {
        m_canvasKey = key;
        m_canvasImage = QImage::fromData(QByteArray::fromBase64(QByteArray::fromStdString(m_canvas->imageData)));
        if (m_canvasImage.isNull() && !m_canvas->imagePath.empty())
            m_canvasImage = QImage(QString::fromStdString(m_canvas->imagePath));  // not embedded (very large file)
    }
    if (m_canvasImage.isNull())
        return;
    // the image rectangle is projected exactly (perspective) onto the sketch plane: four pixel corners ->
    // sketch -> screen, then a projective transform maps the image onto that quad
    const double W = m_canvas->pxWidth, H = m_canvas->pxHeight;
    QPolygonF src, dst;
    for (const Vec2 px : {Vec2{0, 0}, Vec2{W, 0}, Vec2{W, H}, Vec2{0, H}}) {
        QPointF s;
        if (!toScreen(canvasPixelToSketch(*m_canvas, px), &s))
            return;
        src << QPointF(px.x * m_canvasImage.width() / W, px.y * m_canvasImage.height() / H);
        dst << s;
    }
    QTransform t;
    if (!QTransform::quadToQuad(src, dst, t))
        return;
    p.save();
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setOpacity(std::clamp(m_canvas->opacity, 0.05, 1.0));
    p.setTransform(t, false);
    p.drawImage(QPointF(0, 0), m_canvasImage);
    p.restore();
    if (m_calibStage == 2) {
        QPointF a;
        if (toScreen(m_calibA, &a)) {
            p.setPen(QPen(QColor(0xff, 0x98, 0x00), 2));
            p.drawEllipse(a, 5, 5);
            if (m_cursorValid)
                p.drawLine(a, m_cursorPx);
        }
    }
}

void SketchEditor::insertCanvas()
{
    const QString file = QFileDialog::getOpenFileName(m_ws.viewport(), QObject::tr("Insert Image (Canvas)"),
                                                      QSettings().value("canvas/dir").toString(),
                                                      QObject::tr("Images (*.png *.jpg *.jpeg *.bmp *.webp)"));
    if (file.isEmpty())
        return;
    QSettings().setValue("canvas/dir", QFileInfo(file).absolutePath());
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        m_ws.status(QObject::tr("Cannot read %1").arg(file));
        return;
    }
    const QByteArray bytes = f.readAll();
    const QImage img = QImage::fromData(bytes);
    if (img.isNull()) {
        m_ws.status(QObject::tr("%1 is not a readable image").arg(QFileInfo(file).fileName()));
        return;
    }
    // about 60 % of the visible width, centered in the view (calibrate it next)
    double width = 100;
    Vec2 center{0, 0};
    const QSize vs = m_ws.viewport()->size();
    if (auto a = toLocal(QPointF(vs.width() * 0.2, vs.height() * 0.5)), b = toLocal(QPointF(vs.width() * 0.8, vs.height() * 0.5)),
        c = toLocal(QPointF(vs.width() * 0.5, vs.height() * 0.5));
        a && b && c) {
        width = std::max(1.0, (*b - *a).length());
        center = *c;
    }
    SketchCanvas cv = makeCanvas(img.width(), img.height(), width, center);
    cv.imagePath = QDir::toNativeSeparators(file).toStdString();
    cv.mime = QImageReader::imageFormat(file).isEmpty() ? "image/png" : "image/" + QImageReader::imageFormat(file).toStdString();
    constexpr qint64 kEmbedLimit = 12 * 1024 * 1024;
    if (bytes.size() <= kEmbedLimit)
        cv.imageData = bytes.toBase64().toStdString();
    else
        m_ws.status(QObject::tr("Large image: kept as a link to %1 (not embedded in the design)").arg(file), 8000);
    if (m_canvas)
        cv.opacity = m_canvas->opacity;
    m_canvas = cv;
    m_canvasKey = 0;
    m_ws.status(QObject::tr("Image inserted. Right-click > Calibrate image: click two points of known distance."), 10000);
    m_ws.viewport()->refreshOverlay();
}

void SketchEditor::startCalibration()
{
    if (!m_canvas) {
        m_ws.status(QObject::tr("Insert an image first"));
        return;
    }
    setTool(Tool::Select);
    m_calibStage = 1;
    m_ws.status(QObject::tr("Calibration: click the FIRST point of a known distance on the image (Esc cancels)"), 0);
}

bool SketchEditor::calibrationClick(Vec2 lp)
{
    if (m_calibStage == 1) {
        m_calibA = lp;
        m_calibStage = 2;
        m_ws.status(QObject::tr("Calibration: click the SECOND point"), 0);
        m_ws.viewport()->refreshOverlay();
        return true;
    }
    m_calibStage = 0;
    bool ok = false;
    const double mm = QInputDialog::getDouble(m_ws.viewport(), QObject::tr("Calibrate image"),
                                              QObject::tr("Real distance between the two points (mm):"),
                                              (lp - m_calibA).length(), 0.001, 1e6, 3, &ok);
    if (!ok) {
        m_ws.status(QObject::tr("Calibration cancelled"));
        m_ws.viewport()->refreshOverlay();
        return true;
    }
    std::string err;
    if (!calibrateCanvas(*m_canvas, canvasSketchToPixel(*m_canvas, m_calibA), canvasSketchToPixel(*m_canvas, lp), mm, &err))
        m_ws.status(QString::fromStdString(err));
    else
        m_ws.status(QObject::tr("Image calibrated: %1 x %2 mm (scale 1:1)")
                        .arg(m_canvas->widthMm, 0, 'f', 1)
                        .arg(m_canvas->heightMm, 0, 'f', 1),
                    8000);
    m_ws.viewport()->refreshOverlay();
    return true;
}

void SketchEditor::removeCanvas()
{
    if (!m_canvas)
        return;
    m_canvas.reset();
    m_calibStage = 0;
    m_canvasImage = QImage();
    m_canvasKey = 0;
    m_ws.viewport()->refreshOverlay();
}

void SketchEditor::importVector()
{
    const QString file = QFileDialog::getOpenFileName(m_ws.viewport(), QObject::tr("Import SVG / DXF"),
                                                      QSettings().value("vector/dir").toString(),
                                                      QObject::tr("Vector drawings (*.svg *.dxf)"));
    if (file.isEmpty())
        return;
    QSettings().setValue("vector/dir", QFileInfo(file).absolutePath());

    // options: size, position, rotation, rigid block
    QDialog dlg(m_ws.viewport());
    dlg.setWindowTitle(QObject::tr("Import %1").arg(QFileInfo(file).fileName()));
    auto* form = new QFormLayout(&dlg);
    auto* mode = new QComboBox(&dlg);
    mode->addItem(QObject::tr("Scale factor"));
    mode->addItem(QObject::tr("Target width (mm)"));
    auto* scale = new QDoubleSpinBox(&dlg);
    scale->setRange(1e-4, 1e5);
    scale->setDecimals(4);
    scale->setValue(1.0);
    auto* width = new QDoubleSpinBox(&dlg);
    width->setRange(0.01, 1e5);
    width->setDecimals(2);
    width->setValue(50);
    width->setSuffix(" mm");
    width->setEnabled(false);
    QObject::connect(mode, &QComboBox::currentIndexChanged, &dlg, [scale, width](int i) {
        scale->setEnabled(i == 0);
        width->setEnabled(i == 1);
    });
    auto* ox = new QDoubleSpinBox(&dlg);
    auto* oy = new QDoubleSpinBox(&dlg);
    for (auto* b : {ox, oy}) {
        b->setRange(-1e5, 1e5);
        b->setDecimals(2);
        b->setSuffix(" mm");
    }
    auto* angle = new QDoubleSpinBox(&dlg);
    angle->setRange(-360, 360);
    angle->setSuffix(QString::fromUtf8("°"));
    auto* rigid = new QCheckBox(QObject::tr("Group as rigid block"), &dlg);
    rigid->setToolTip(QObject::tr("Keeps the proportions of the drawing: move or dimension it as a whole without "
                                  "deforming it (delete its center point to explode the block)"));
    rigid->setChecked(true);
    auto* construction = new QCheckBox(QObject::tr("As construction geometry"), &dlg);
    form->addRow(QObject::tr("Size"), mode);
    form->addRow(QObject::tr("Scale"), scale);
    form->addRow(QObject::tr("Width"), width);
    form->addRow(QObject::tr("Center X"), ox);
    form->addRow(QObject::tr("Center Y"), oy);
    form->addRow(QObject::tr("Rotation"), angle);
    form->addRow(rigid);
    form->addRow(construction);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(bb);
    if (dlg.exec() != QDialog::Accepted)
        return;

    vectorimport::ImportOptions o;
    o.scale = scale->value();
    o.targetWidth = mode->currentIndex() == 1 ? width->value() : 0;
    o.offset = {ox->value(), oy->value()};
    o.angleDeg = angle->value();
    o.rigid = rigid->isChecked();
    o.construction = construction->isChecked();
    const Sketch before = m_sketch;
    snapshotUndo();
    const auto r = vectorimport::importFile(m_sketch, std::filesystem::path(file.toStdWString()), o);
    if (!r) {
        m_sketch = before;
        m_undo.pop_back();
        QMessageBox::warning(m_ws.viewport(), dlg.windowTitle(), QString::fromStdString(r.error));
        return;
    }
    if (o.rigid)
        for (auto& c : m_sketch.constraints)
            if (std::find(r.constraints.begin(), r.constraints.end(), c.id) != r.constraints.end() &&
                c.kind != ConstraintKind::Coincident)
                c.group = "block:" + r.anchor;
    solve();
    changed();
    m_ws.status(QObject::tr("Imported %1 line(s), %2 arc(s), %3 circle(s); %4 end(s) joined%5")
                    .arg(r.lines)
                    .arg(r.arcs)
                    .arg(r.circles)
                    .arg(r.merged)
                    .arg(r.warnings.empty() ? QString() : " - " + QString::fromStdString(r.warnings.front())),
                10000);
}
}  // namespace silex::ui
