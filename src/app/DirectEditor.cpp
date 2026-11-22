#include "DirectEditor.h"

#include <QCheckBox>
#include <QSpinBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>
#include <cmath>

#include "ExprSpinBox.h"
#include "Icons.h"
#include "ModelController.h"
#include "Theme.h"
#include "Viewport.h"
#include "model/Bindings.h"

namespace silex::ui {

namespace {

QVector3D toQ(const Vec3& v) { return {float(v.x), float(v.y), float(v.z)}; }

const Vec3 kAxes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
const QColor kAxisColors[3] = {QColor(0xe0, 0x60, 0x60), QColor(0x70, 0xc8, 0x70), QColor(0x4a, 0xa3, 0xff)};
const char* kFieldPtr[6] = {"/dx", "/dy", "/dz", "/rx", "/ry", "/rz"};

double segDist(QPointF p, QPointF a, QPointF b)
{
    const QPointF d = b - a;
    const double l2 = d.x() * d.x() + d.y() * d.y();
    double t = l2 > 1e-12 ? ((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / l2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF f = a + d * t;
    return std::hypot(p.x() - f.x(), p.y() - f.y());
}

double axisParam(const Ray& r, const Vec3& o, const Vec3& d)
{
    const QVector3D O = toQ(o), D = toQ(d).normalized();
    const QVector3D w0 = O - r.origin;
    const float b = QVector3D::dotProduct(D, r.dir);
    const float dd = QVector3D::dotProduct(D, w0);
    const float e = QVector3D::dotProduct(r.dir, w0);
    const float denom = 1.f - b * b;
    return std::abs(denom) < 1e-6f ? 0 : (b * e - dd) / denom;
}

double* fieldOf(TransformBodyParams& p, int i)
{
    double* f[6] = {&p.dx, &p.dy, &p.dz, &p.rx, &p.ry, &p.rz};
    return f[i];
}

}  // namespace

bool DirectEditor::handles(const std::string& t)
{
    return t == FeatureType::RemoveBody || t == FeatureType::DefeatureFace || t == FeatureType::SplitBody ||
           t == FeatureType::TransformBody || t == FeatureType::ConstructionPlane || t == FeatureType::Rib ||
           t == FeatureType::Thread;
}

DirectEditor::DirectEditor(Workspace& ws, std::string type, FeaturePtr existing)
    : EditSession(ws), m_type(std::move(type)), m_existing(std::move(existing))
{
    if (m_existing) {
        m_id = m_existing->id;
        m_name = m_existing->name;
        const Json& pj = m_existing->params;
        if (m_type == FeatureType::RemoveBody)
            m_bodies = m_existing->as<RemoveBodyParams>().bodies;
        else if (m_type == FeatureType::DefeatureFace)
            m_faces = m_existing->as<DefeatureParams>().faces;
        else if (isPlane()) {
            m_cp = m_existing->as<ConstructionPlaneParams>();
            for (const char* ptr : {"/base/offset", "/angle"})
                if (auto e = fieldExpression(pj, ptr); !e.empty())
                    m_exprs[ptr] = e;
        }
        else if (isThread())
            m_thread = m_existing->as<ThreadParams>();
        else if (isRib()) {
            m_rib = m_existing->as<RibParams>();
            if (auto e = fieldExpression(pj, "/thickness"); !e.empty())
                m_exprs["/thickness"] = e;
        }
        else if (m_type == FeatureType::SplitBody) {
            const auto p = m_existing->as<SplitBodyParams>();
            m_bodies = {p.body};
            m_plane = p.plane;
            m_split = p;
            if (auto e = fieldExpression(pj, "/plane/offset"); !e.empty())
                m_exprs["/plane/offset"] = e;
        }
        else {
            m_tp = m_existing->as<TransformBodyParams>();
            m_bodies = m_tp.bodies;
            for (const char* ptr : kFieldPtr)
                if (auto e = fieldExpression(pj, ptr); !e.empty())
                    m_exprs[ptr] = e;
            if (!m_tp.mate.is_null() && !m_tp.mate.empty()) {
                m_mateMode = m_tp.mate.value("mode", std::string("face"));
                m_mateMoving = m_tp.mate.value("moving", TopoRef{});
                m_mateFixed = m_tp.mate.value("fixed", TopoRef{});
                m_mateOffset = m_tp.mate.value("offset", 0.0);
                m_mateFlip = m_tp.mate.value("flip", false);
                m_mateOffsetExpr = fieldExpression(pj, "/mate/offset");
            }
        }
    }
    else {
        m_id = newUuid();
        const std::string base = m_type == FeatureType::DefeatureFace   ? "DeleteFace"
                                 : m_type == FeatureType::TransformBody ? "Move"
                                 : m_type == FeatureType::SplitBody     ? "Split"
                                 : isPlane()                            ? "Plane"
                                 : isRib()                              ? "Rib"
                                 : isThread()                           ? "Thread"
                                                                        : "RemoveBody";
        m_name = ws.model()->doc().nextName(base);
        m_plane.kind = "XY";
        for (const Uuid& b : ws.selectedBodies())
            if (m_type != FeatureType::DefeatureFace)
                m_bodies.push_back(b);
        if (m_type == FeatureType::SplitBody && m_bodies.size() > 1)
            m_bodies.resize(1);
        if (m_type == FeatureType::DefeatureFace)
            for (const SelItem& s : ws.selection())
                if (s.kind == SelKind::Face)
                    m_faces.push_back(s.ref());
        if (isPlane()) {
            m_bodies.clear();
            m_cp.base.kind = "XY";
            m_cp.base.offset = 10;
            for (const SelItem& s : ws.selection())  // a selected planar face is the base
                if (s.kind == SelKind::Face && s.planar) {
                    m_cp.base.kind = "Face";
                    m_cp.base.face = s.ref();
                    break;
                }
        }
        if (isThread()) {
            m_bodies.clear();
            for (const SelItem& s : ws.selection())
                if (s.kind == SelKind::Face && !s.planar) {
                    m_thread.face = s.ref();
                    break;
                }
        }
        if (isRib()) {
            m_rib.sketch = ws.lastSketch();
            m_rib.body = m_bodies.empty() ? Uuid{} : m_bodies.front();
            m_bodies.clear();
        }
        if (m_type == FeatureType::SplitBody && !m_bodies.empty())  // default: cut at mid-height
            if (auto snap = ws.model()->snapshot())
                for (const auto& b : snap->bodies)
                    if (b.id == m_bodies.front() && b.mesh.bbox.valid())
                        m_plane.offset = std::round((b.mesh.bbox.min.z + b.mesh.bbox.max.z) * 0.5 * 100) / 100;
    }
    syncSelection();
    pushPreview();
}

DirectEditor::~DirectEditor() = default;

void DirectEditor::startMate(const std::string& mode)
{
    m_mateMode = mode;
    m_mateMoving = {};
    m_mateFixed = {};
    m_bodies.clear();
    m_name = m_ws.model()->doc().nextName("Align");
    updatePanel();
    pushPreview();
}

void DirectEditor::setPlaneMode(const std::string& mode)
{
    if (!isPlane())
        return;
    m_cp.mode = mode;
    m_pick = Pick::None;
    updatePanel();
    syncSelection();
    pushPreview();
}

QString DirectEditor::bodyName(const Uuid& id) const
{
    if (auto snap = m_ws.model()->snapshot())
        for (const auto& b : snap->bodies)
            if (b.id == id)
                return QString::fromStdString(b.name);
    return QObject::tr("(missing body)");
}

uint32_t DirectEditor::pickFilter() const
{
    if (isPlane())
        return m_pick == Pick::AxisEdge ? SelFilter::Edges : (SelFilter::Faces | SelFilter::PlanarOnly);
    if (isThread())
        return SelFilter::Faces;
    if (m_type == FeatureType::SplitBody && m_pickSplitFace)
        return SelFilter::Faces | SelFilter::PlanarOnly;
    if (isTransform() && mateActive())
        return m_mateMode == "axis" ? (SelFilter::Faces | SelFilter::Edges) : (SelFilter::Faces | SelFilter::PlanarOnly);
    return SelFilter::Faces;
}

bool DirectEditor::ready() const
{
    if (isPlane())
        return m_cp.mode != "mid" || (!m_cp.face1.name.empty() && !m_cp.face2.name.empty());
    if (isRib())
        return !m_rib.sketch.empty();
    if (isThread())
        return !m_thread.face.name.empty();
    if (m_type == FeatureType::DefeatureFace)
        return !m_faces.empty();
    if (isTransform() && mateActive())
        return !m_mateMoving.name.empty() && !m_mateFixed.name.empty() && !m_bodies.empty();
    return !m_bodies.empty();
}

FeaturePtr DirectEditor::buildNode() const
{
    auto n = m_existing ? std::make_shared<FeatureNode>(*m_existing) : std::make_shared<FeatureNode>();
    n->id = m_id;
    n->type = m_type;
    n->name = m_name;
    if (m_type == FeatureType::RemoveBody) {
        RemoveBodyParams p;
        p.bodies = m_bodies;
        n->params = Json(p);
    }
    else if (m_type == FeatureType::DefeatureFace) {
        DefeatureParams p;
        p.faces = m_faces;
        n->params = Json(p);
    }
    else if (isPlane())
        n->params = Json(m_cp);
    else if (isRib())
        n->params = Json(m_rib);
    else if (isThread())
        n->params = Json(m_thread);
    else if (m_type == FeatureType::SplitBody) {
        SplitBodyParams p = m_split;
        p.body = m_bodies.empty() ? Uuid{} : m_bodies.front();
        p.plane = m_plane;
        n->params = Json(p);
    }
    else {
        TransformBodyParams p = m_tp;
        p.bodies = m_bodies;
        p.mate = Json();
        if (mateActive())
            p.mate = Json{{"mode", m_mateMode},       {"moving", m_mateMoving}, {"fixed", m_mateFixed},
                          {"offset", m_mateOffset},   {"flip", m_mateFlip}};
        n->params = Json(p);
        if (mateActive())
            setFieldExpression(n->params, "/mate/offset", m_mateOffsetExpr);
    }
    for (const auto& [ptr, e] : m_exprs)
        setFieldExpression(n->params, ptr, e);
    return n;
}

void DirectEditor::pushPreview()
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

void DirectEditor::syncSelection()
{
    std::vector<SelItem> sel;
    auto snap = m_ws.model()->snapshot();
    if (snap) {
        std::vector<TopoRef> faces = m_faces;
        if (!m_mateMoving.name.empty())
            faces.push_back(m_mateMoving);
        if (!m_mateFixed.name.empty())
            faces.push_back(m_mateFixed);
        if (m_plane.kind == "Face")
            faces.push_back(m_plane.face);
        if (isThread() && !m_thread.face.name.empty())
            faces.push_back(m_thread.face);
        if (isPlane()) {
            if (m_cp.base.kind == "Face")
                faces.push_back(m_cp.base.face);
            if (!m_cp.face1.name.empty())
                faces.push_back(m_cp.face1);
            if (!m_cp.face2.name.empty())
                faces.push_back(m_cp.face2);
        }
        for (size_t b = 0; b < snap->bodies.size(); ++b) {
            const auto& bv = snap->bodies[b];
            const bool whole = !mateActive() && m_type != FeatureType::DefeatureFace &&
                               std::find(m_bodies.begin(), m_bodies.end(), bv.id) != m_bodies.end();
            for (size_t f = 0; f < bv.mesh.faces.size(); ++f) {
                const auto& mf = bv.mesh.faces[f];
                const bool pick = std::any_of(faces.begin(), faces.end(), [&](const TopoRef& r) { return r.name == mf.name; });
                if (!whole && !pick)
                    continue;
                SelItem s;
                s.kind = SelKind::Face;
                s.owner = bv.id;
                s.object = int(b);
                s.index = int(f);
                s.name = mf.name;
                s.sig = mf.sig;
                sel.push_back(s);
            }
        }
    }
    m_ws.setSelection(std::move(sel));
}

void DirectEditor::onSnapshot()
{
    syncSelection();
    updatePanel();
}

void DirectEditor::onPicked(const SelItem& item, bool)
{
    if (item.kind != SelKind::Face && item.kind != SelKind::Edge)
        return;
    if (isPlane()) {
        if (m_pick == Pick::AxisEdge && item.kind == SelKind::Edge) {
            m_cp.axis.kind = "Edge";
            m_cp.axis.ref = item.ref();
            m_pick = Pick::None;
        }
        else if (item.kind == SelKind::Face && item.planar) {
            if (m_cp.mode == "mid") {
                if (m_cp.face1.name.empty() || !m_cp.face2.name.empty()) {
                    m_cp.face1 = item.ref();
                    m_cp.face2 = {};
                }
                else
                    m_cp.face2 = item.ref();
            }
            else {
                m_cp.base.kind = "Face";
                m_cp.base.face = item.ref();
            }
        }
        else
            return;
        syncSelection();
        pushPreview();
        updatePanel();
        return;
    }
    if (isThread()) {
        if (item.kind != SelKind::Face || item.planar) {
            m_ws.status(QObject::tr("Click the cylindrical face of a shaft or of a hole"));
            return;
        }
        m_thread.face = item.ref();
        syncSelection();
        pushPreview();
        updatePanel();
        return;
    }
    if (isRib()) {
        if (item.kind == SelKind::Face)
            m_rib.body = item.owner;
        pushPreview();
        updatePanel();
        return;
    }
    if (m_type == FeatureType::DefeatureFace) {
        if (item.kind != SelKind::Face)
            return;
        auto it = std::find_if(m_faces.begin(), m_faces.end(), [&](const TopoRef& r) { return r.name == item.name; });
        if (it != m_faces.end())
            m_faces.erase(it);
        else
            m_faces.push_back(item.ref());
    }
    else if (m_type == FeatureType::SplitBody && m_pickSplitFace) {
        if (!item.planar)
            return;
        m_plane.kind = "Face";
        m_plane.face = item.ref();
        m_plane.offset = 0;
        m_pickSplitFace = false;
        if (m_bodies.empty())
            m_bodies = {item.owner};
    }
    else if (isTransform() && mateActive()) {
        if (m_mateMoving.name.empty() || (!m_mateFixed.name.empty() && item.owner == m_bodies.front())) {
            m_mateMoving = item.ref();
            m_bodies = {item.owner};
            m_mateFixed = {};
            m_ws.status(QObject::tr("Now click the face / axis of the fixed body"));
        }
        else {
            if (item.owner == m_bodies.front()) {
                m_ws.status(QObject::tr("Click a face of another body (the fixed one)"));
                return;
            }
            m_mateFixed = item.ref();
        }
    }
    else {
        auto it = std::find(m_bodies.begin(), m_bodies.end(), item.owner);
        if (m_type == FeatureType::SplitBody)
            m_bodies = {item.owner};
        else if (it != m_bodies.end())
            m_bodies.erase(it);
        else
            m_bodies.push_back(item.owner);
    }
    syncSelection();
    pushPreview();
    updatePanel();
}

bool DirectEditor::commit()
{
    if (!ready()) {
        m_ws.status(m_type == FeatureType::DefeatureFace ? QObject::tr("Click the faces to delete")
                    : mateActive()                       ? QObject::tr("Click a face of the moved body, then of the fixed body")
                                                         : QObject::tr("Click the body (a face of it)"));
        return false;
    }
    FeaturePtr node = buildNode();
    if (m_existing)
        m_ws.model()->execute(UpdateFeatureCommand(node));
    else
        m_ws.model()->execute(AddFeatureCommand(node));
    return true;
}

std::vector<MenuAction> DirectEditor::menuActions()
{
    std::vector<MenuAction> a{
        {QObject::tr("OK"), icon(IconId::Finish), [ws = &m_ws] { ws->finishSession(); }},
        {QObject::tr("Cancel"), icon(IconId::Cancel), [ws = &m_ws] { ws->cancelSession(); }},
    };
    if (isTransform()) {
        a.push_back({QObject::tr("Create copy"), icon(IconId::Body), [this] {
                         m_tp.copy = !m_tp.copy;
                         updatePanel();
                         pushPreview();
                     }});
        if (mateActive())
            a.push_back({QObject::tr("Flip"), icon(IconId::Mirror), [this] {
                             m_mateFlip = !m_mateFlip;
                             pushPreview();
                         }});
    }
    return a;
}

// ------------------------------------------------------------------ gizmo (Move / Copy)

bool DirectEditor::gizmoCenter(Vec3* c, double* worldLen) const
{
    if (!isTransform() || mateActive() || m_bodies.empty())
        return false;
    auto snap = m_ws.model()->snapshot();
    if (!snap)
        return false;
    BBox box;
    for (const auto& b : snap->bodies)
        if (std::find(m_bodies.begin(), m_bodies.end(), b.id) != m_bodies.end() && b.mesh.bbox.valid()) {
            box.add(b.mesh.bbox.min);
            box.add(b.mesh.bbox.max);
        }
    if (!box.valid())
        return false;
    *c = (box.min + box.max) * 0.5 + Vec3{m_tp.dx, m_tp.dy, m_tp.dz};
    Viewport* vp = m_ws.viewport();
    *worldLen = vp->camera().pixelSize(vp->size()) * 80;  // constant on screen
    return true;
}

std::vector<QPointF> DirectEditor::arcPoints(int axis, const Vec3& c, double r) const
{
    const Vec3 u = kAxes[(axis + 1) % 3], v = kAxes[(axis + 2) % 3];
    std::vector<QPointF> pts;
    for (int k = 0; k <= 48; ++k) {
        const double t = 2 * M_PI * k / 48;
        QPointF q;
        if (m_ws.viewport()->toScreen(c + u * (r * std::cos(t)) + v * (r * std::sin(t)), &q))
            pts.push_back(q);
    }
    return pts;
}

int DirectEditor::hitGizmo(QPointF px) const
{
    Vec3 c;
    double L;
    if (!gizmoCenter(&c, &L))
        return -1;
    QPointF s0, s1;
    for (int a = 0; a < 3; ++a)
        if (m_ws.viewport()->toScreen(c, &s0) && m_ws.viewport()->toScreen(c + kAxes[a] * L, &s1) &&
            std::hypot(px.x() - s1.x(), px.y() - s1.y()) < 11)
            return a;
    for (int a = 0; a < 3; ++a) {
        const auto pts = arcPoints(a, c, L * 0.62);
        for (size_t k = 1; k < pts.size(); ++k)
            if (segDist(px, pts[k - 1], pts[k]) < 5)
                return 3 + a;
    }
    return -1;
}

bool DirectEditor::mousePress(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return false;
    const int h = hitGizmo(e->position());
    if (h < 0)
        return false;
    m_drag = h;
    m_dragPx0 = e->position();
    m_dragStart = *fieldOf(m_tp, h);
    Vec3 c;
    double L;
    gizmoCenter(&c, &L);
    if (h < 3)
        m_dragT0 = axisParam(m_ws.viewport()->camera().ray(e->position(), m_ws.viewport()->size()), c, kAxes[h]);
    return true;
}

bool DirectEditor::mouseMove(QMouseEvent* e)
{
    if (m_drag >= 0) {
        Vec3 c;
        double L;
        if (!gizmoCenter(&c, &L))
            return true;
        double v;
        if (m_drag < 3) {
            const Vec3 c0 = c - kAxes[m_drag] * (*fieldOf(m_tp, m_drag) - m_dragStart);
            const double t = axisParam(m_ws.viewport()->camera().ray(e->position(), m_ws.viewport()->size()), c0, kAxes[m_drag]);
            v = m_dragStart + (t - m_dragT0);
            if (e->modifiers() & Qt::ControlModifier)
                v = std::round(v);
        }
        else {
            QPointF sc;
            m_ws.viewport()->toScreen(c, &sc);
            const double a0 = std::atan2(m_dragPx0.y() - sc.y(), m_dragPx0.x() - sc.x());
            const double a1 = std::atan2(e->position().y() - sc.y(), e->position().x() - sc.x());
            // screen y is down; turn the right way depending on whether the axis faces the camera
            const QVector3D fwd = m_ws.viewport()->camera().forward();
            const double sign = QVector3D::dotProduct(fwd, toQ(kAxes[m_drag - 3])) > 0 ? 1.0 : -1.0;
            v = m_dragStart + sign * (a1 - a0) * 180.0 / M_PI;
            if (e->modifiers() & Qt::ControlModifier)
                v = std::round(v / 15) * 15;
        }
        *fieldOf(m_tp, m_drag) = v;
        m_exprs.erase(kFieldPtr[m_drag]);
        updatePanel();
        pushPreview();
        m_ws.viewport()->refreshOverlay();
        return true;
    }
    const int h = hitGizmo(e->position());
    if (h != m_hover) {
        m_hover = h;
        m_ws.viewport()->refreshOverlay();
    }
    return h >= 0;
}

bool DirectEditor::mouseRelease(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && m_drag >= 0) {
        m_drag = -1;
        return true;
    }
    return false;
}

void DirectEditor::paintOverlay(QPainter& p)
{
    Vec3 c;
    double L;
    if (gizmoCenter(&c, &L)) {
        QPointF s0, s1;
        for (int a = 0; a < 3; ++a) {  // rotation arcs
            const auto pts = arcPoints(a, c, L * 0.62);
            QColor col = kAxisColors[a];
            col.setAlpha(m_hover == 3 + a || m_drag == 3 + a ? 255 : 150);
            p.setPen(QPen(col, m_hover == 3 + a || m_drag == 3 + a ? 3.2 : 2.0));
            p.drawPolyline(pts.data(), int(pts.size()));
        }
        for (int a = 0; a < 3; ++a)  // translation arrows
            if (m_ws.viewport()->toScreen(c, &s0) && m_ws.viewport()->toScreen(c + kAxes[a] * L, &s1)) {
                const bool hot = m_hover == a || m_drag == a;
                const QColor col = hot ? QColor(0xff, 0xd0, 0x40) : kAxisColors[a];
                p.setPen(QPen(col, 2.6, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(s0, s1);
                QPointF d = s1 - s0;
                const double len = std::hypot(d.x(), d.y());
                if (len > 1) {
                    d /= len;
                    const QPointF n(-d.y(), d.x());
                    p.setBrush(col);
                    p.drawPolygon(QPolygonF{s1 + d * 12, s1 + n * 6, s1 - n * 6});
                    p.setBrush(Qt::NoBrush);
                }
            }
    }
    // header
    QString hint;
    if (m_type == FeatureType::RemoveBody)
        hint = QObject::tr("%n body(ies) removed from this step on — click a body to toggle", nullptr, int(m_bodies.size()));
    else if (m_type == FeatureType::DefeatureFace)
        hint = QObject::tr("%n face(s) deleted, the neighbours are extended to heal the solid", nullptr, int(m_faces.size()));
    else if (isPlane())
        hint = m_cp.mode == "mid"     ? QObject::tr("Click two parallel planar faces")
               : m_pick == Pick::AxisEdge ? QObject::tr("Click a straight edge as rotation axis")
                                          : QObject::tr("Choose the base plane (or click a planar face)");
    else if (isRib())
        hint = QObject::tr("Open line of a sketch -> rib up to the walls (click a face to choose the body)");
    else if (isThread())
        hint = m_thread.face.name.empty() ? QObject::tr("Click the cylindrical face of a shaft (screw) or a hole (nut)")
                                          : QObject::tr("ISO thread — the FDM clearance lets printed parts screw together");
    else if (m_type == FeatureType::SplitBody)
        hint = m_pickSplitFace ? QObject::tr("Click the planar face used as splitting tool")
                               : QObject::tr("Click the body to split, choose the plane");
    else if (mateActive())
        hint = m_mateMoving.name.empty() ? QObject::tr("Click a face of the body to MOVE")
               : m_mateFixed.name.empty() ? QObject::tr("Click the face / axis of the FIXED body")
                                          : QObject::tr("Aligned — set the gap, Flip if needed");
    else
        hint = QObject::tr("Drag the arrows / arcs (Ctrl: 1 mm / 15°) or type values");
    QFont f = p.font();
    f.setPixelSize(12);
    f.setBold(true);
    p.setFont(f);
    QRectF box(12, 12, 380, 52);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(20, 24, 30, 200));
    p.drawRoundedRect(box, 6, 6);
    p.setPen(QColor(230, 233, 237));
    p.drawText(box.adjusted(10, 6, -10, -26), Qt::AlignLeft | Qt::AlignVCenter, QString::fromStdString(m_name));
    f.setBold(false);
    f.setPixelSize(11);
    p.setFont(f);
    p.setPen(QColor(170, 178, 188));
    p.drawText(box.adjusted(10, 28, -10, -4), Qt::AlignLeft | Qt::AlignVCenter, hint);
    p.setBrush(Qt::NoBrush);
}

// ------------------------------------------------------------------ panel

QWidget* DirectEditor::createPanel(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* lay = new QVBoxLayout(w);
    auto* title = new QLabel(QString("<b>%1</b>").arg(QString::fromStdString(m_name)), w);
    title->setStyleSheet("font-size: 15px;");
    lay->addWidget(title);
    auto* form = new QFormLayout();
    m_selLabel = new QLabel(w);
    m_selLabel->setWordWrap(true);
    form->addRow(m_type == FeatureType::DefeatureFace ? QObject::tr("Faces")
                 : isPlane()                          ? QObject::tr("Faces")
                 : isRib()                            ? QObject::tr("Body")
                 : isThread()                         ? QObject::tr("Face")
                                                      : QObject::tr("Bodies"),
                 m_selLabel);

    auto spin = [&](double value, const std::string& expr, double lo, double hi, const QString& suffix) {
        auto* b = new ExprSpinBox(w);
        b->setRange(lo, hi);
        b->setDecimals(3);
        b->setSuffix(suffix);
        b->setKeyboardTracking(true);
        b->setValueAndExpression(value, expr);
        return b;
    };

    if (isPlane()) {
        m_modeBox = new QComboBox(w);
        m_modeBox->addItems({QObject::tr("Offset plane"), QObject::tr("Angled plane"), QObject::tr("Midplane")});
        m_modeBox->setCurrentIndex(m_cp.mode == "angle" ? 1 : m_cp.mode == "mid" ? 2 : 0);
        form->addRow(QObject::tr("Type"), m_modeBox);
        QObject::connect(m_modeBox, &QComboBox::currentIndexChanged, m_modeBox, [this](int i) {
            if (m_updating)
                return;
            m_cp.mode = i == 1 ? "angle" : i == 2 ? "mid" : "offset";
            m_pick = Pick::None;
            updatePanel();
            syncSelection();
            pushPreview();
        });
        m_baseBox = new QComboBox(w);
        fillPlaneCombo(m_baseBox, m_cp.base);
        form->addRow(QObject::tr("Base plane"), m_baseBox);
        QObject::connect(m_baseBox, &QComboBox::currentIndexChanged, m_baseBox, [this](int) {
            if (m_updating)
                return;
            applyPlaneCombo(m_baseBox, m_cp.base);
            syncSelection();
            pushPreview();
        });
        auto* off = spin(m_cp.base.offset, m_exprs.count("/base/offset") ? m_exprs["/base/offset"] : "", -1e5, 1e5, " mm");
        form->addRow(QObject::tr("Distance"), off);
        QObject::connect(off, &QDoubleSpinBox::valueChanged, off, [this](double v) {
            if (m_updating)
                return;
            m_cp.base.offset = v;
            pushPreview();
        });
        QObject::connect(off, &ExprSpinBox::exprChanged, off, [this](const QString& e) { m_exprs["/base/offset"] = e.toStdString(); });
        m_axisBox = new QComboBox(w);
        m_axisBox->addItems({QObject::tr("X axis"), QObject::tr("Y axis"), QObject::tr("Z axis"), QObject::tr("Straight edge...")});
        m_axisBox->setCurrentIndex(m_cp.axis.kind == "Y" ? 1 : m_cp.axis.kind == "Z" ? 2 : m_cp.axis.kind == "Edge" ? 3 : 0);
        form->addRow(QObject::tr("Rotation axis"), m_axisBox);
        QObject::connect(m_axisBox, &QComboBox::currentIndexChanged, m_axisBox, [this](int i) {
            if (m_updating)
                return;
            if (i == 3) {
                m_pick = Pick::AxisEdge;
                m_ws.status(QObject::tr("Click a straight edge"));
                return;
            }
            m_pick = Pick::None;
            m_cp.axis = GeomRef{i == 1 ? "Y" : i == 2 ? "Z" : "X", {}};
            pushPreview();
        });
        auto* ang = spin(m_cp.angle, m_exprs.count("/angle") ? m_exprs["/angle"] : "", -360, 360, QString(QChar(0xB0)));
        form->addRow(QObject::tr("Angle"), ang);
        QObject::connect(ang, &QDoubleSpinBox::valueChanged, ang, [this](double v) {
            if (m_updating)
                return;
            m_cp.angle = v;
            pushPreview();
        });
        QObject::connect(ang, &ExprSpinBox::exprChanged, ang, [this](const QString& e) { m_exprs["/angle"] = e.toStdString(); });
    }
    if (isRib()) {
        m_sketchBox = new QComboBox(w);
        m_lineBox = new QComboBox(w);
        form->addRow(QObject::tr("Sketch"), m_sketchBox);
        form->addRow(QObject::tr("Line"), m_lineBox);
        fillRibCombos();
        QObject::connect(m_sketchBox, &QComboBox::currentIndexChanged, m_sketchBox, [this](int i) {
            if (m_updating || i < 0)
                return;
            m_rib.sketch = m_sketchBox->itemData(i).toString().toStdString();
            m_rib.curve.clear();
            fillRibCombos();
            pushPreview();
        });
        QObject::connect(m_lineBox, &QComboBox::currentIndexChanged, m_lineBox, [this](int i) {
            if (m_updating || i < 0)
                return;
            m_rib.curve = m_lineBox->itemData(i).toString().toStdString();
            pushPreview();
        });
        auto* th = spin(m_rib.thickness, m_exprs.count("/thickness") ? m_exprs["/thickness"] : "", 0.01, 1e3, " mm");
        form->addRow(QObject::tr("Thickness"), th);
        QObject::connect(th, &QDoubleSpinBox::valueChanged, th, [this](double v) {
            if (m_updating)
                return;
            m_rib.thickness = v;
            pushPreview();
        });
        QObject::connect(th, &ExprSpinBox::exprChanged, th, [this](const QString& e) { m_exprs["/thickness"] = e.toStdString(); });
    }
    if (isThread()) {
        auto* size = new QComboBox(w);
        for (const auto& sz : metricThreadSizes()) {
            const auto t = metricThread(sz);
            size->addItem(QString("%1 (%2 / %3)").arg(QString::fromStdString(sz)).arg(t->pitch).arg(t->finePitch),
                          QString::fromStdString(sz));
        }
        size->setCurrentIndex(std::max(0, size->findData(QString::fromStdString(m_thread.size))));
        form->addRow(QObject::tr("Size (coarse / fine pitch)"), size);
        QObject::connect(size, &QComboBox::currentIndexChanged, size, [this, size](int) {
            m_thread.size = size->currentData().toString().toStdString();
            pushPreview();
        });
        auto* fine = new QCheckBox(QObject::tr("Fine pitch"), w);
        fine->setChecked(m_thread.fine);
        QObject::connect(fine, &QCheckBox::toggled, fine, [this](bool on) {
            m_thread.fine = on;
            pushPreview();
        });
        form->addRow("", fine);
        auto* clr = spin(m_thread.clearance, "", 0, 1, " mm");
        clr->setToolTip(QObject::tr("Radial FDM clearance on the flank (0.15 mm: a printed screw and nut fit without forcing)"));
        form->addRow(QObject::tr("FDM clearance"), clr);
        QObject::connect(clr, &QDoubleSpinBox::valueChanged, clr, [this](double v) {
            m_thread.clearance = v;
            pushPreview();
        });
        auto* len = spin(m_thread.length, "", 0, 1e4, " mm");
        len->setToolTip(QObject::tr("0 = the whole face, else measured from the end of the face"));
        form->addRow(QObject::tr("Length"), len);
        QObject::connect(len, &QDoubleSpinBox::valueChanged, len, [this](double v) {
            m_thread.length = v;
            pushPreview();
        });
        auto* mode = new QComboBox(w);
        mode->addItems({QObject::tr("Modeled 3D (printing)"), QObject::tr("Cosmetic (drawing, light)")});
        mode->setCurrentIndex(m_thread.modeled ? 0 : 1);
        form->addRow(QObject::tr("Representation"), mode);
        QObject::connect(mode, &QComboBox::currentIndexChanged, mode, [this](int i) {
            m_thread.modeled = i == 0;
            pushPreview();
        });
    }
    if (m_type == FeatureType::SplitBody) {
        m_planeBox = new QComboBox(w);
        fillPlaneCombo(m_planeBox, m_plane);
        form->addRow(QObject::tr("Splitting plane"), m_planeBox);
        QObject::connect(m_planeBox, &QComboBox::currentIndexChanged, m_planeBox, [this](int i) {
            if (m_updating)
                return;
            if (i == 3) {
                m_pickSplitFace = true;
                m_ws.status(QObject::tr("Click a planar face"));
                return;
            }
            m_pickSplitFace = false;
            applyPlaneCombo(m_planeBox, m_plane);
            syncSelection();
            pushPreview();
        });
        auto* off = spin(m_plane.offset, m_exprs.count("/plane/offset") ? m_exprs["/plane/offset"] : "", -1e5, 1e5, " mm");
        form->addRow(QObject::tr("Offset"), off);
        QObject::connect(off, &QDoubleSpinBox::valueChanged, off, [this](double v) {
            if (m_updating)
                return;
            m_plane.offset = v;
            pushPreview();
        });
        QObject::connect(off, &ExprSpinBox::exprChanged, off,
                         [this](const QString& e) { m_exprs["/plane/offset"] = e.toStdString(); });

        // v0.5 lot 4: alignment pins on part 1, matching holes (with FDM clearance) in part 2
        auto* pegs = new QCheckBox(QObject::tr("Add alignment pins / pegs"), w);
        pegs->setChecked(m_split.pegs);
        pegs->setToolTip(QObject::tr("Pins on the lower part, holes with the FDM clearance in the upper part: the two "
                                     "halves of a large print glue back without offset"));
        form->addRow(pegs);
        auto* count = new QSpinBox(w);
        count->setRange(1, 8);
        count->setValue(m_split.pegCount);
        auto* dia = spin(m_split.pegDiameter, "", 1, 30, " mm");
        auto* len = spin(m_split.pegLength, "", 1, 100, " mm");
        auto* clr = spin(m_split.pegClearance, "", 0, 1, " mm");
        form->addRow(QObject::tr("Pins"), count);
        form->addRow(QObject::tr("Pin diameter"), dia);
        form->addRow(QObject::tr("Pin length"), len);
        form->addRow(QObject::tr("Radial clearance"), clr);
        auto enable = [=](bool on) {
            for (QWidget* x : std::initializer_list<QWidget*>{count, dia, len, clr})
                x->setEnabled(on);
        };
        enable(m_split.pegs);
        QObject::connect(pegs, &QCheckBox::toggled, pegs, [this, enable](bool on) {
            m_split.pegs = on;
            enable(on);
            pushPreview();
        });
        QObject::connect(count, &QSpinBox::valueChanged, count, [this](int v) {
            m_split.pegCount = v;
            pushPreview();
        });
        QObject::connect(dia, &QDoubleSpinBox::valueChanged, dia, [this](double v) {
            m_split.pegDiameter = v;
            pushPreview();
        });
        QObject::connect(len, &QDoubleSpinBox::valueChanged, len, [this](double v) {
            m_split.pegLength = v;
            pushPreview();
        });
        QObject::connect(clr, &QDoubleSpinBox::valueChanged, clr, [this](double v) {
            m_split.pegClearance = v;
            pushPreview();
        });
    }
    if (isTransform()) {
        m_mateBox = new QComboBox(w);
        m_mateBox->addItems({QObject::tr("Free move (gizmo)"), QObject::tr("Align face to face"), QObject::tr("Align coaxial")});
        form->addRow(QObject::tr("Mode"), m_mateBox);
        QObject::connect(m_mateBox, &QComboBox::currentIndexChanged, m_mateBox, [this](int i) {
            if (m_updating)
                return;
            m_mateMode = i == 1 ? "face" : i == 2 ? "axis" : "";
            m_mateMoving = m_mateFixed = {};
            updatePanel();
            syncSelection();
            pushPreview();
            m_ws.viewport()->refreshOverlay();
        });
        const QString labels[6] = {"ΔX", "ΔY", "ΔZ", "Rx", "Ry", "Rz"};
        m_fields.clear();
        for (int i = 0; i < 6; ++i) {
            auto* b = spin(*fieldOf(m_tp, i), m_exprs.count(kFieldPtr[i]) ? m_exprs[kFieldPtr[i]] : "", -1e5, 1e5,
                           i < 3 ? QString(" mm") : QString(QChar(0xB0)));
            form->addRow(labels[i], b);
            QObject::connect(b, &QDoubleSpinBox::valueChanged, b, [this, i](double v) {
                if (m_updating)
                    return;
                *fieldOf(m_tp, i) = v;
                pushPreview();
                m_ws.viewport()->refreshOverlay();
            });
            QObject::connect(b, &ExprSpinBox::exprChanged, b, [this, i](const QString& e) {
                if (!m_updating)
                    m_exprs[kFieldPtr[i]] = e.toStdString();
            });
            m_fields.push_back(b);
        }
        m_offsetBox = spin(m_mateOffset, m_mateOffsetExpr, -1e4, 1e4, " mm");
        m_offsetBox->setToolTip(QObject::tr("Gap between the faces (e.g. jeu_montage) or shift along the axis"));
        form->addRow(QObject::tr("Gap / offset"), m_offsetBox);
        QObject::connect(m_offsetBox, &QDoubleSpinBox::valueChanged, m_offsetBox, [this](double v) {
            if (m_updating)
                return;
            m_mateOffset = v;
            pushPreview();
        });
        QObject::connect(m_offsetBox, &ExprSpinBox::exprChanged, m_offsetBox, [this](const QString& e) {
            if (!m_updating)
                m_mateOffsetExpr = e.toStdString();
        });
        auto* flip = new QPushButton(QObject::tr("Flip"), w);
        QObject::connect(flip, &QPushButton::clicked, flip, [this] {
            m_mateFlip = !m_mateFlip;
            pushPreview();
        });
        form->addRow("", flip);
        m_copyBox = new QCheckBox(QObject::tr("Create copy"), w);
        m_copyBox->setChecked(m_tp.copy);
        QObject::connect(m_copyBox, &QCheckBox::toggled, m_copyBox, [this](bool on) {
            if (m_updating)
                return;
            m_tp.copy = on;
            pushPreview();
        });
        form->addRow("", m_copyBox);
    }
    lay->addLayout(form);
    auto* clear = new QPushButton(QObject::tr("Clear selection"), w);
    QObject::connect(clear, &QPushButton::clicked, clear, [this] {
        m_bodies.clear();
        m_faces.clear();
        m_mateMoving = m_mateFixed = {};
        syncSelection();
        pushPreview();
        updatePanel();
    });
    lay->addWidget(clear);
    m_message = new QLabel(w);
    m_message->setWordWrap(true);
    lay->addWidget(m_message);
    lay->addStretch(1);
    auto* ok = new QPushButton(icon(IconId::Finish), QObject::tr("OK"), w);
    ok->setDefault(true);
    auto* cancel = new QPushButton(icon(IconId::Cancel), QObject::tr("Cancel"), w);
    QObject::connect(ok, &QPushButton::clicked, ok, [ws = &m_ws] { ws->finishSession(); });
    QObject::connect(cancel, &QPushButton::clicked, cancel, [ws = &m_ws] { ws->cancelSession(); });
    lay->addWidget(ok);
    lay->addWidget(cancel);
    updatePanel();
    return w;
}

void DirectEditor::fillPlaneCombo(QComboBox* box, const SketchPlaneRef& cur) const
{
    const bool was = box->signalsBlocked();
    box->blockSignals(true);
    box->clear();
    box->addItem(QObject::tr("XY (horizontal)"), "XY");
    box->addItem(QObject::tr("XZ (front)"), "XZ");
    box->addItem(QObject::tr("YZ (side)"), "YZ");
    box->addItem(cur.kind == "Face" ? QObject::tr("Selected face") : QObject::tr("Planar face..."), "Face");
    if (auto snap = m_ws.model()->snapshot())
        for (const auto& pv : snap->planes)
            if (pv.id != m_id)
                box->addItem(icon(IconId::Plane), QString::fromStdString(pv.name), QString::fromStdString("plane:" + pv.id));
    int idx = box->findData(QString::fromStdString(cur.kind == "Plane" ? "plane:" + cur.planeId : cur.kind));
    box->setCurrentIndex(std::max(0, idx));
    box->blockSignals(was);
}

void DirectEditor::applyPlaneCombo(QComboBox* box, SketchPlaneRef& ref)
{
    const QString d = box->currentData().toString();
    if (d == "Face") {
        m_ws.status(QObject::tr("Click a planar face"));
        return;
    }
    ref.face = {};
    if (d.startsWith("plane:")) {
        ref.kind = "Plane";
        ref.planeId = d.mid(6).toStdString();
    }
    else {
        ref.kind = d.toStdString();
        ref.planeId.clear();
    }
}

void DirectEditor::fillRibCombos()
{
    if (!m_sketchBox || !m_lineBox)
        return;
    m_updating = true;
    m_sketchBox->clear();
    for (const auto& f : m_ws.model()->doc().features)
        if (f->type == FeatureType::Sketch)
            m_sketchBox->addItem(QString::fromStdString(f->name), QString::fromStdString(f->id));
    int idx = m_sketchBox->findData(QString::fromStdString(m_rib.sketch));
    if (idx < 0 && m_sketchBox->count() > 0) {
        idx = m_sketchBox->count() - 1;
        m_rib.sketch = m_sketchBox->itemData(idx).toString().toStdString();
    }
    m_sketchBox->setCurrentIndex(idx);
    m_lineBox->clear();
    m_lineBox->addItem(QObject::tr("First line"), QString());
    if (FeaturePtr f = m_ws.model()->doc().find(m_rib.sketch)) {
        int n = 1;
        for (const SketchGeo& g : f->as<SketchParams>().sketch.geos)
            if (g.kind == GeoKind::Line && !g.construction)
                m_lineBox->addItem(QObject::tr("Line %1").arg(n++), QString::fromStdString(g.id));
    }
    m_lineBox->setCurrentIndex(std::max(0, m_lineBox->findData(QString::fromStdString(m_rib.curve))));
    m_updating = false;
}

void DirectEditor::updatePanel()
{
    m_updating = true;
    if (m_selLabel) {
        QStringList names;
        if (m_type == FeatureType::DefeatureFace)
            m_selLabel->setText(QObject::tr("%n selected", nullptr, int(m_faces.size())));
        else if (isPlane())
            m_selLabel->setText(m_cp.mode == "mid" ? QObject::tr("%1 / 2 faces")
                                                         .arg((m_cp.face1.name.empty() ? 0 : 1) + (m_cp.face2.name.empty() ? 0 : 1))
                                : m_cp.base.kind == "Face" ? QObject::tr("base: selected face")
                                                           : QObject::tr("—"));
        else if (isRib())
            m_selLabel->setText(m_rib.body.empty() ? QObject::tr("last body") : bodyName(m_rib.body));
        else if (isThread())
            m_selLabel->setText(m_thread.face.name.empty() ? QObject::tr("click a cylindrical face") : QObject::tr("1 face"));
        else {
            for (const Uuid& b : m_bodies)
                names << bodyName(b);
            m_selLabel->setText(names.isEmpty() ? QObject::tr("click a body") : names.join(", "));
        }
    }
    if (m_planeBox)
        fillPlaneCombo(m_planeBox, m_plane);
    if (m_baseBox)
        fillPlaneCombo(m_baseBox, m_cp.base);
    if (m_modeBox)
        m_modeBox->setCurrentIndex(m_cp.mode == "angle" ? 1 : m_cp.mode == "mid" ? 2 : 0);
    if (m_axisBox) {
        m_axisBox->setEnabled(m_cp.mode == "angle");
        m_axisBox->setCurrentIndex(m_cp.axis.kind == "Y" ? 1 : m_cp.axis.kind == "Z" ? 2 : m_cp.axis.kind == "Edge" ? 3 : 0);
    }
    if (m_baseBox)
        m_baseBox->setEnabled(m_cp.mode != "mid");

    if (m_mateBox)
        m_mateBox->setCurrentIndex(m_mateMode == "face" ? 1 : m_mateMode == "axis" ? 2 : 0);
    for (int i = 0; i < int(m_fields.size()); ++i)
        if (m_fields[size_t(i)]) {
            m_fields[size_t(i)]->setEnabled(!mateActive());
            if (std::abs(m_fields[size_t(i)]->value() - *fieldOf(m_tp, i)) > 1e-9)
                m_fields[size_t(i)]->setValueAndExpression(*fieldOf(m_tp, i), m_exprs.count(kFieldPtr[i]) ? m_exprs[kFieldPtr[i]] : "");
        }
    if (m_offsetBox)
        m_offsetBox->setEnabled(mateActive());
    if (m_message) {
        QString msg;
        if (auto snap = m_ws.model()->snapshot())
            if (const FeatureResult* r = snap->result(m_id); r && r->status == FeatureStatus::Error)
                msg = QString("<span style='color:%1'>%2</span>").arg(Colors::Error.name(), QString::fromStdString(r->message).toHtmlEscaped());
        if (const QString why = m_ws.explainStep(m_id); !why.isEmpty())
            msg += QString("<br><span style='color:#cfe8d2'>&#128161; %1</span>").arg(why.toHtmlEscaped());
        m_message->setText(msg);
    }
    m_updating = false;
}

}  // namespace silex::ui
