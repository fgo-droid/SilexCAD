#include "FeatureEditor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>

#include "ExprSpinBox.h"
#include "model/Bindings.h"
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>

#include "Icons.h"
#include "ModelController.h"
#include "Theme.h"
#include "Viewport.h"

namespace silex::ui {

namespace {
QVector3D toQ(const Vec3& v) { return {float(v.x), float(v.y), float(v.z)}; }

int opIndex(BooleanOp op) { return static_cast<int>(op); }
BooleanOp opFromIndex(int i) { return static_cast<BooleanOp>(std::clamp(i, 0, 3)); }
}  // namespace

FeatureEditor::FeatureEditor(Workspace& ws, std::string type, FeaturePtr existing)
    : EditSession(ws), m_type(std::move(type)), m_existing(std::move(existing))
{
    auto snap = ws.model()->snapshot();
    if (m_existing) {
        m_id = m_existing->id;
        m_name = m_existing->name;
        m_valueExpr = fieldExpression(m_existing->params, valuePointer());
        if (m_type == FeatureType::Extrude) {
            const auto p = m_existing->as<ExtrudeParams>();
            for (const auto& k : p.profile.regions)
                m_profiles.emplace_back(p.profile.sketch, k);
            if (p.profile.regions.empty() && snap)
                for (const auto& pv : snap->profiles)
                    if (pv.sketchId == p.profile.sketch)
                        m_profiles.emplace_back(pv.sketchId, pv.regionKey);
            m_value = p.reversed ? -p.distance : p.distance;
            m_symmetric = p.symmetric;
            m_op = p.op;
        }
        else if (m_type == FeatureType::Revolve) {
            const auto p = m_existing->as<RevolveParams>();
            for (const auto& k : p.profile.regions)
                m_profiles.emplace_back(p.profile.sketch, k);
            if (p.profile.regions.empty() && snap)
                for (const auto& pv : snap->profiles)
                    if (pv.sketchId == p.profile.sketch)
                        m_profiles.emplace_back(pv.sketchId, pv.regionKey);
            m_value = p.angle;
            m_axis = p.axis;
            m_op = p.op;
        }
        else if (m_type == FeatureType::Shell) {
            const auto p = m_existing->as<ShellParams>();
            m_shellFaces = p.removedFaces;
            m_value = p.thickness;
            m_outward = p.outward;
        }
        else if (m_type == FeatureType::OffsetFace) {
            const auto p = m_existing->as<OffsetFaceParams>();
            m_face = p.face;
            m_faceBody = p.body;
            m_value = p.distance;
        }
        else if (m_type == FeatureType::Box || m_type == FeatureType::Cylinder) {
            m_prim = m_existing->as<PrimitiveParams>();
            m_value = m_prim.height;
            m_op = m_prim.op;
            for (const char* ptr : {"/centerX", "/centerY", "/length", "/width", "/diameter"})
                if (auto e = fieldExpression(m_existing->params, ptr); !e.empty())
                    m_exprs[ptr] = e;
        }
        else if (m_type == FeatureType::Combine) {
            const auto p = m_existing->as<CombineParams>();
            m_target = p.target;
            m_tools = p.tools;
            m_op = p.op;
            m_keepTools = p.keepTools;
        }
        else {
            const auto p = m_existing->as<EdgeBlendParams>();
            m_edges = p.edges;
            m_value = p.value;
        }
    }
    else {
        m_id = newUuid();
        m_name = ws.model()->doc().nextName(m_type == FeatureType::OffsetFace ? std::string("PressPull") : m_type);
        const bool hasBodies = snap && !snap->bodies.empty();
        if (m_type == FeatureType::Extrude) {
            m_value = 10.0;
            m_op = hasBodies ? BooleanOp::Join : BooleanOp::NewBody;
        }
        else if (m_type == FeatureType::Revolve) {
            m_value = 360.0;
            m_op = hasBodies ? BooleanOp::Join : BooleanOp::NewBody;
        }
        else if (m_type == FeatureType::Fillet)
            m_value = 2.0;
        else if (m_type == FeatureType::Combine)
            m_op = BooleanOp::Join;
        else if (m_type == FeatureType::Shell)
            m_value = 2.0;
        else if (m_type == FeatureType::OffsetFace)
            m_value = 5.0;
        else if (isPrimitive()) {
            m_value = 10.0;
            m_op = BooleanOp::NewBody;
            if (m_type == FeatureType::Cylinder)
                m_prim.diameter = 20;
        }
        else
            m_value = 1.0;
        for (const SelItem& s : ws.selection()) {
            if (isOffsetFace() && s.kind == SelKind::Face && s.planar && m_face.name.empty()) {
                m_face = s.ref();
                m_faceBody = s.owner;
            }
            if (isPrimitive() && s.kind == SelKind::Face && s.planar && m_prim.plane.kind != "Face") {
                m_prim.plane.kind = "Face";  // a primitive on the selected face (boss, pocket)
                m_prim.plane.face = s.ref();
                m_op = BooleanOp::Join;
            }
            if (isShell() && s.kind == SelKind::Face)
                m_shellFaces.push_back(s.ref());
            if (isProfileBased() && s.kind == SelKind::Profile)
                m_profiles.emplace_back(s.owner, s.name);
            if (isBlend() && s.kind == SelKind::Edge)
                m_edges.push_back(s.ref());
            if (isCombine() && (s.kind == SelKind::Face || s.kind == SelKind::Edge || s.kind == SelKind::Body)) {
                // first picked body = target, the others = tools
                if (m_target.empty())
                    m_target = s.owner;
                else if (s.owner != m_target && std::find(m_tools.begin(), m_tools.end(), s.owner) == m_tools.end())
                    m_tools.push_back(s.owner);
            }
        }
        // keep a single sketch per profile feature
        if (!m_profiles.empty()) {
            const Uuid sk = m_profiles.front().first;
            std::erase_if(m_profiles, [&](const auto& p) { return p.first != sk; });
        }
        m_autoPickPending = isProfileBased() && m_profiles.empty();
    }
    syncSelection();
    pushPreview();
}

FeatureEditor::~FeatureEditor() = default;

QString FeatureEditor::title() const { return QString::fromStdString(m_name); }

bool FeatureEditor::isProfileBased() const
{
    return m_type == FeatureType::Extrude || m_type == FeatureType::Revolve;
}

bool FeatureEditor::isBlend() const { return m_type == FeatureType::Fillet || m_type == FeatureType::Chamfer; }

bool FeatureEditor::isCombine() const { return m_type == FeatureType::Combine; }

bool FeatureEditor::isShell() const { return m_type == FeatureType::Shell; }

bool FeatureEditor::isOffsetFace() const { return m_type == FeatureType::OffsetFace; }

bool FeatureEditor::isPrimitive() const { return m_type == FeatureType::Box || m_type == FeatureType::Cylinder; }

Placement FeatureEditor::primitivePlane() const
{
    if (m_prim.plane.kind != "Face")
        return offsetPlacement(originPlacement(m_prim.plane.kind), m_prim.plane.offset);
    if (auto snap = m_ws.model()->snapshot())
        for (const auto& b : snap->bodies)
            for (const auto& f : b.mesh.faces)
                if (f.name == m_prim.plane.face.name && f.planar) {
                    // same frame as the kernel: face centroid, X = world X projected on the face
                    Placement pl = f.plane;
                    pl.origin = f.sig.center;
                    const Vec3 n = pl.normal.normalized();
                    Vec3 x = Vec3{1, 0, 0} - n * n.x;
                    if (x.length() < 1e-6)
                        x = Vec3{0, 1, 0} - n * n.y;
                    pl.xDir = x.normalized();
                    return offsetPlacement(pl, m_prim.plane.offset);
                }
    return originPlacement("XY");
}

std::vector<TopoRef> FeatureEditor::highlightedFaces() const
{
    if (isShell())
        return m_shellFaces;
    if (isOffsetFace() && !m_face.name.empty())
        return {m_face};
    if (isPrimitive() && m_prim.plane.kind == "Face")
        return {m_prim.plane.face};
    return {};
}

QString FeatureEditor::bodyName(const Uuid& id) const
{
    if (auto snap = m_ws.model()->snapshot())
        for (const auto& b : snap->bodies)
            if (b.id == id)
                return QString::fromStdString(b.name);
    return QObject::tr("(missing body)");
}

uint32_t FeatureEditor::pickFilter() const
{
    if (isProfileBased())
        return SelFilter::Profiles;
    if (isOffsetFace())
        return SelFilter::Faces | SelFilter::PlanarOnly | (m_pressPull && m_face.name.empty() ? SelFilter::Profiles : 0u);
    if (isPrimitive())
        return SelFilter::Faces | SelFilter::PlanarOnly;
    return (isCombine() || isShell()) ? SelFilter::Faces : SelFilter::Edges;
}

std::set<Uuid> FeatureEditor::sketchesInUse() const
{
    std::set<Uuid> s;
    for (const auto& p : m_profiles)
        s.insert(p.first);
    if (m_autoPickPending && !m_ws.lastSketch().empty())
        s.insert(m_ws.lastSketch());
    return s;
}

// ------------------------------------------------------------------ document

std::string FeatureEditor::valuePointer() const
{
    if (m_type == FeatureType::Extrude) return "/distance";
    if (m_type == FeatureType::Revolve) return "/angle";
    if (isShell()) return "/thickness";
    if (isOffsetFace()) return "/distance";
    if (isPrimitive()) return "/height";
    if (isCombine()) return {};
    return "/value";
}

FeaturePtr FeatureEditor::buildNode() const
{
    auto n = m_existing ? std::make_shared<FeatureNode>(*m_existing) : std::make_shared<FeatureNode>();
    n->id = m_id;
    n->type = m_type;
    n->name = m_name;
    if (m_type == FeatureType::Extrude) {
        ExtrudeParams p;
        if (!m_profiles.empty())
            p.profile.sketch = m_profiles.front().first;
        for (const auto& pr : m_profiles)
            p.profile.regions.push_back(pr.second);
        p.distance = std::abs(m_value);
        p.reversed = m_value < 0;
        p.symmetric = m_symmetric;
        p.op = m_op;
        n->params = Json(p);
    }
    else if (m_type == FeatureType::Revolve) {
        RevolveParams p;
        if (!m_profiles.empty())
            p.profile.sketch = m_profiles.front().first;
        for (const auto& pr : m_profiles)
            p.profile.regions.push_back(pr.second);
        p.axis = m_axis;
        p.angle = m_value;
        p.op = m_op;
        n->params = Json(p);
    }
    else if (isShell()) {
        ShellParams p;
        p.removedFaces = m_shellFaces;
        p.thickness = m_value;
        p.outward = m_outward;
        n->params = Json(p);
    }
    else if (isOffsetFace()) {
        OffsetFaceParams p;
        p.face = m_face;
        p.body = m_faceBody;
        p.distance = m_value;
        n->params = Json(p);
    }
    else if (isPrimitive()) {
        PrimitiveParams p = m_prim;
        p.height = m_value;
        p.op = m_op;
        n->params = Json(p);
        for (const auto& [ptr, e] : m_exprs)
            setFieldExpression(n->params, ptr, e);
    }
    else if (isCombine()) {
        CombineParams p;
        p.target = m_target;
        p.tools = m_tools;
        p.op = m_op;
        p.keepTools = m_keepTools;
        n->params = Json(p);
    }
    else {
        EdgeBlendParams p;
        p.edges = m_edges;
        p.value = m_value;
        n->params = Json(p);
    }
    if (!isCombine())
        setFieldExpression(n->params, valuePointer(), m_valueExpr);
    return n;
}

DocState FeatureEditor::previewDoc() const
{
    DocState d = m_ws.model()->doc();
    FeaturePtr node = buildNode();
    if (m_existing) {
        d = UpdateFeatureCommand(node).apply(d);
        d.marker = d.indexOf(m_id) + 1;  // later features are rolled back while editing
    }
    else {
        d = AddFeatureCommand(node).apply(d);
    }
    return d;
}

void FeatureEditor::pushPreview()
{
    const bool ready = isProfileBased() ? !m_profiles.empty()
                       : isCombine()    ? (!m_target.empty() && !m_tools.empty())
                       : isShell()      ? !m_shellFaces.empty()
                       : isOffsetFace() ? !m_face.name.empty()
                       : isPrimitive()  ? true
                                        : !m_edges.empty();
    if (!ready) {
        DocState d = m_ws.model()->doc();
        if (m_existing)
            d.marker = d.indexOf(m_id);
        m_ws.model()->preview(d, {});
        return;
    }
    m_ws.model()->preview(previewDoc(), m_id);
}

void FeatureEditor::syncSelection()
{
    std::vector<SelItem> sel;
    for (const auto& [sk, key] : m_profiles)
        if (SelItem s = m_ws.findProfile(sk, key); s.valid())
            sel.push_back(s);
    for (const auto& e : m_edges)
        if (SelItem s = m_ws.findEdge(e); s.valid())
            sel.push_back(s);
    if (const auto faces = highlightedFaces(); !faces.empty())
        if (auto snap = m_ws.model()->snapshot())
            for (const TopoRef& r : faces)
                for (size_t b = 0; b < snap->bodies.size(); ++b) {
                    const auto& faces = snap->bodies[b].mesh.faces;
                    for (size_t f = 0; f < faces.size(); ++f)
                        if (faces[f].name == r.name) {
                            SelItem s;
                            s.kind = SelKind::Face;
                            s.owner = snap->bodies[b].id;
                            s.object = int(b);
                            s.index = int(f);
                            s.name = faces[f].name;
                            s.sig = faces[f].sig;
                            sel.push_back(s);
                        }
                }
    if (isCombine())
        if (auto snap = m_ws.model()->snapshot()) {
            // highlight every face of the target and tool bodies
            std::vector<Uuid> bodies = m_tools;
            if (!m_target.empty())
                bodies.push_back(m_target);
            for (size_t b = 0; b < snap->bodies.size(); ++b) {
                const BodyView& bv = snap->bodies[b];
                if (std::find(bodies.begin(), bodies.end(), bv.id) == bodies.end())
                    continue;
                for (size_t f = 0; f < bv.mesh.faces.size(); ++f) {
                    SelItem s;
                    s.kind = SelKind::Face;
                    s.owner = bv.id;
                    s.object = int(b);
                    s.index = int(f);
                    s.name = bv.mesh.faces[f].name;
                    s.sig = bv.mesh.faces[f].sig;
                    sel.push_back(s);
                }
            }
        }
    m_ws.setSelection(std::move(sel));
}

void FeatureEditor::onSnapshot()
{
    auto snap = m_ws.model()->snapshot();
    if (!snap)
        return;
    if (m_autoPickPending) {
        std::vector<const ProfileView*> candidates;
        for (const auto& pv : snap->profiles)
            if (m_ws.lastSketch().empty() || pv.sketchId == m_ws.lastSketch())
                candidates.push_back(&pv);
        if (!candidates.empty()) {
            m_autoPickPending = false;
            if (candidates.size() == 1) {
                m_profiles.emplace_back(candidates.front()->sketchId, candidates.front()->regionKey);
                pushPreview();
                fillAxisCombo();
            }
        }
    }
    syncSelection();
    updatePanel();
}

void FeatureEditor::onPicked(const SelItem& item, bool)
{
    if (item.kind == SelKind::Profile && isProfileBased()) {
        auto it = std::find_if(m_profiles.begin(), m_profiles.end(),
                               [&](const auto& p) { return p.first == item.owner && p.second == item.name; });
        if (it != m_profiles.end())
            m_profiles.erase(it);
        else {
            if (!m_profiles.empty() && m_profiles.front().first != item.owner)
                m_profiles.clear();  // one sketch per feature
            m_profiles.emplace_back(item.owner, item.name);
        }
        m_autoPickPending = false;
        fillAxisCombo();
    }
    else if (item.kind == SelKind::Edge && isBlend()) {
        auto it = std::find_if(m_edges.begin(), m_edges.end(), [&](const TopoRef& r) { return r.name == item.name; });
        if (it != m_edges.end())
            m_edges.erase(it);
        else
            m_edges.push_back(item.ref());
    }
    else if (item.kind == SelKind::Profile && isOffsetFace() && m_pressPull) {
        // Press / Pull on a closed sketch profile = a regular extrusion
        QTimer::singleShot(0, m_ws.viewport(), [ws = &m_ws, item] {
            ws->cancelSession();
            ws->setSelection({item});
            ws->startFeature(FeatureType::Extrude);
        });
        return;
    }
    else if (item.kind == SelKind::Face && isOffsetFace()) {
        if (!item.planar) {
            m_ws.status(QObject::tr("Press / Pull works on planar faces"));
            return;
        }
        m_face = item.ref();
        m_faceBody = item.owner;
    }
    else if (item.kind == SelKind::Face && isPrimitive()) {
        if (!item.planar)
            return;
        m_prim.plane.kind = "Face";
        m_prim.plane.face = item.ref();
        m_prim.plane.offset = 0;
        if (m_op == BooleanOp::NewBody)
            m_op = BooleanOp::Join;
        if (m_planeBox) {
            m_updatingPanel = true;
            m_planeBox->setItemText(3, QObject::tr("Selected face"));
            m_planeBox->setCurrentIndex(3);
            m_updatingPanel = false;
        }
    }
    else if (item.kind == SelKind::Face && isShell()) {
        auto it = std::find_if(m_shellFaces.begin(), m_shellFaces.end(),
                               [&](const TopoRef& r) { return r.name == item.name; });
        if (it != m_shellFaces.end())
            m_shellFaces.erase(it);
        else
            m_shellFaces.push_back(item.ref());
    }
    else if ((item.kind == SelKind::Face || item.kind == SelKind::Edge) && isCombine()) {
        const Uuid body = item.owner;
        if (body == m_target)
            m_target.clear();  // click the target again to release it
        else if (auto it = std::find(m_tools.begin(), m_tools.end(), body); it != m_tools.end())
            m_tools.erase(it);
        else if (m_target.empty())
            m_target = body;
        else
            m_tools.push_back(body);
    }
    else {
        return;
    }
    syncSelection();
    pushPreview();
    updatePanel();
}

bool FeatureEditor::commit()
{
    if (isProfileBased() && m_profiles.empty()) {
        m_ws.status(QObject::tr("Select at least one profile"));
        return false;
    }
    if (isBlend() && m_edges.empty()) {
        m_ws.status(QObject::tr("Select at least one edge"));
        return false;
    }
    if (isShell() && m_shellFaces.empty()) {
        m_ws.status(QObject::tr("Select at least one face to remove (the opening)"));
        return false;
    }
    if (isOffsetFace() && m_face.name.empty()) {
        m_ws.status(m_pressPull ? QObject::tr("Click a planar face to push / pull, or a closed profile to extrude")
                                : QObject::tr("Click a planar face to push / pull"));
        return false;
    }
    if (isCombine() && (m_target.empty() || m_tools.empty())) {
        m_ws.status(QObject::tr("Click a target body, then one or more tool bodies"));
        return false;
    }
    FeaturePtr node = buildNode();
    if (m_existing)
        m_ws.model()->execute(UpdateFeatureCommand(node));
    else
        m_ws.model()->execute(AddFeatureCommand(node));
    return true;
}

void FeatureEditor::cancel() {}

std::vector<MenuAction> FeatureEditor::menuActions()
{
    std::vector<MenuAction> a{
        {QObject::tr("OK"), icon(IconId::Finish), [ws = &m_ws] { ws->finishSession(); }},
        {QObject::tr("Cancel"), icon(IconId::Cancel), [ws = &m_ws] { ws->cancelSession(); }},
    };
    if (m_type == FeatureType::Extrude) {
        a.push_back({QObject::tr("Flip"), icon(IconId::Extrude), [this] {
                         m_value = -m_value;
                         updatePanel();
                         pushPreview();
                     }});
        a.push_back({QObject::tr("Symmetric"), icon(IconId::Extrude), [this] {
                         m_symmetric = !m_symmetric;
                         updatePanel();
                         pushPreview();
                     }});
    }
    if (isOffsetFace() || isPrimitive()) {
        a.push_back({QObject::tr("Flip"), icon(IconId::PressPull), [this] {
                         m_value = -m_value;
                         updatePanel();
                         pushPreview();
                     }});
    }
    if (isProfileBased() || isPrimitive()) {
        a.push_back({QObject::tr("New body"), icon(IconId::Body), [this] {
                         m_op = BooleanOp::NewBody;
                         updatePanel();
                         pushPreview();
                     }});
        a.push_back({QObject::tr("Join"), icon(IconId::Body), [this] {
                         m_op = BooleanOp::Join;
                         updatePanel();
                         pushPreview();
                     }});
        a.push_back({QObject::tr("Cut"), icon(IconId::Body), [this] {
                         m_op = BooleanOp::Cut;
                         updatePanel();
                         pushPreview();
                     }});
    }
    return a;
}

// ------------------------------------------------------------------ gizmo (extrude distance)

bool FeatureEditor::gizmo(QPointF* base, QPointF* tip, Vec3* origin, Vec3* dir) const
{
    auto snap = m_ws.model()->snapshot();
    if (!snap)
        return false;
    // direct modelling: arrow on the face (Press / Pull) or on the base of the primitive
    if (isOffsetFace() || isPrimitive()) {
        Vec3 c, d;
        bool found = false;
        if (isOffsetFace()) {
            for (const auto& b : snap->bodies)
                for (const auto& f : b.mesh.faces)
                    if (!found && f.name == m_face.name && f.planar) {
                        c = f.sig.center;
                        d = f.plane.normal.normalized();
                        found = true;
                    }
        }
        else {
            const Placement pl = primitivePlane();
            c = pl.toWorld({m_prim.centerX, m_prim.centerY});
            d = pl.normal.normalized();
            found = true;
        }
        if (!found)
            return false;
        if (origin)
            *origin = c;
        if (dir)
            *dir = d;
        Viewport* vp = m_ws.viewport();
        return vp->toScreen(c, base) && vp->toScreen(c + d * m_value, tip);
    }
    if (m_type != FeatureType::Extrude || m_profiles.empty())
        return false;
    const SketchView* sv = snap->sketch(m_profiles.front().first);
    if (!sv)
        return false;
    Vec3 c{};
    int n = 0;
    for (const auto& pv : snap->profiles)
        for (const auto& [sk, key] : m_profiles)
            if (pv.sketchId == sk && pv.regionKey == key) {
                c = c + pv.centroid;
                ++n;
            }
    if (!n)
        return false;
    c = c * (1.0 / n);
    const Vec3 d = sv->plane.normal.normalized();
    const double reach = m_symmetric ? m_value * 0.5 : m_value;
    if (origin)
        *origin = c;
    if (dir)
        *dir = d;
    Viewport* vp = m_ws.viewport();
    return vp->toScreen(c, base) && vp->toScreen(c + d * reach, tip);
}

static double axisParam(const Ray& r, const Vec3& o, const Vec3& d)
{
    const QVector3D O = toQ(o), D = toQ(d).normalized();
    const QVector3D w0 = O - r.origin;
    const float b = QVector3D::dotProduct(D, r.dir);
    const float dd = QVector3D::dotProduct(D, w0);
    const float e = QVector3D::dotProduct(r.dir, w0);
    const float denom = 1.f - b * b;
    if (std::abs(denom) < 1e-6f)
        return 0;
    return (b * e - dd) / denom;
}

bool FeatureEditor::mousePress(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return false;
    QPointF base, tip;
    Vec3 o, d;
    if (gizmo(&base, &tip, &o, &d) && std::hypot(e->position().x() - tip.x(), e->position().y() - tip.y()) < 12) {
        m_dragging = true;
        m_dragStartValue = m_value;
        m_dragStartT = axisParam(m_ws.viewport()->camera().ray(e->position(), m_ws.viewport()->size()), o, d);
        return true;
    }
    return false;
}

bool FeatureEditor::mouseMove(QMouseEvent* e)
{
    QPointF base, tip;
    Vec3 o, d;
    const bool have = gizmo(&base, &tip, &o, &d);
    if (m_dragging && have) {
        const double t = axisParam(m_ws.viewport()->camera().ray(e->position(), m_ws.viewport()->size()), o, d);
        double v = m_dragStartValue + (t - m_dragStartT) * (m_symmetric ? 2.0 : 1.0);
        if (e->modifiers() & Qt::ControlModifier)
            v = std::round(v);  // snap to whole millimetres
        if (std::abs(v) < 1e-3)
            v = v < 0 ? -1e-3 : 1e-3;
        m_value = v;
        updatePanel();
        pushPreview();
        m_ws.viewport()->refreshOverlay();
        return true;
    }
    const bool hover = have && std::hypot(e->position().x() - tip.x(), e->position().y() - tip.y()) < 12;
    if (hover != m_gizmoHover) {
        m_gizmoHover = hover;
        m_ws.viewport()->refreshOverlay();
    }
    return hover;
}

bool FeatureEditor::mouseRelease(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        return true;
    }
    return false;
}

bool FeatureEditor::keyPress(QKeyEvent*) { return false; }

void FeatureEditor::paintOverlay(QPainter& p)
{
    QPointF base, tip;
    if (gizmo(&base, &tip, nullptr, nullptr)) {
        const QColor c = m_dragging || m_gizmoHover ? QColor(0xff, 0xd0, 0x40) : QColor(0xff, 0xa0, 0x30);
        p.setPen(QPen(c, 2.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(base, tip);
        QPointF dir = tip - base;
        const double len = std::hypot(dir.x(), dir.y());
        if (len > 1) {
            dir /= len;
            const QPointF nrm(-dir.y(), dir.x());
            QPolygonF head{tip + dir * 12, tip + nrm * 6, tip - nrm * 6};
            p.setBrush(c);
            p.drawPolygon(head);
        }
        p.setBrush(m_dragging || m_gizmoHover ? c : QColor(30, 34, 40));
        p.drawEllipse(tip, 6, 6);
        QFont f = p.font();
        f.setPixelSize(12);
        f.setBold(true);
        p.setFont(f);
        const QString t = QString("%1 mm").arg(QLocale::c().toString(m_value, 'f', 2));
        const QRectF r(tip + QPointF(14, -11), QSizeF(QFontMetricsF(f).horizontalAdvance(t) + 12, 22));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(20, 24, 30, 210));
        p.drawRoundedRect(r, 4, 4);
        p.setPen(Qt::white);
        p.drawText(r, Qt::AlignCenter, t);
        p.setBrush(Qt::NoBrush);
    }

    // header
    QString hint;
    if (isProfileBased())
        hint = m_profiles.empty() ? QObject::tr("Select one or more profiles")
                                  : QObject::tr("%n profile(s) — drag the arrow or type a value", nullptr, int(m_profiles.size()));
    else if (isOffsetFace())
        hint = m_face.name.empty()
                   ? (m_pressPull ? QObject::tr("Click a planar face (push / pull) or a closed profile (extrude)")
                                  : QObject::tr("Click the planar face to push / pull"))
                   : (m_value >= 0 ? QObject::tr("Pull: adds material — drag the arrow or type a distance")
                                   : QObject::tr("Push: removes material — drag the arrow or type a distance"));
    else if (isPrimitive())
        hint = QObject::tr("Drag the arrow for the height · click a planar face to build on it");
    else if (isShell())
        hint = m_shellFaces.empty() ? QObject::tr("Click the face(s) to open")
                                    : QObject::tr("%n face(s) removed — set the wall thickness", nullptr,
                                                  int(m_shellFaces.size()));
    else if (isCombine())
        hint = m_target.empty() ? QObject::tr("Click the target body")
               : m_tools.empty() ? QObject::tr("Click the tool body (or bodies)")
                                 : QObject::tr("%1 ← %n tool body(ies)", nullptr, int(m_tools.size())).arg(bodyName(m_target));
    else
        hint = m_edges.empty() ? QObject::tr("Select edges to %1").arg(m_type == FeatureType::Fillet ? "fillet" : "chamfer")
                               : QObject::tr("%n edge(s) selected", nullptr, int(m_edges.size()));
    QFont f = p.font();
    f.setPixelSize(12);
    f.setBold(true);
    p.setFont(f);
    QRectF box(12, 12, 340, 52);
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

QWidget* FeatureEditor::createPanel(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* lay = new QVBoxLayout(w);
    auto* title = new QLabel(QString("<b>%1</b>").arg(QString::fromStdString(m_name)), w);
    title->setStyleSheet("font-size: 15px;");
    lay->addWidget(title);

    auto* form = new QFormLayout();
    auto* selRow = new QHBoxLayout();
    m_selLabel = new QLabel(w);
    auto* clear = new QPushButton(QObject::tr("Clear"), w);
    QObject::connect(clear, &QPushButton::clicked, clear, [this] {
        m_profiles.clear();
        m_edges.clear();
        m_target.clear();
        m_tools.clear();
        m_shellFaces.clear();
        m_face = {};
        m_faceBody.clear();
        syncSelection();
        pushPreview();
        updatePanel();
    });
    selRow->addWidget(m_selLabel, 1);
    selRow->addWidget(clear);
    if (isPrimitive()) {
        m_selLabel->hide();
        clear->hide();
    }
    else
        form->addRow(isProfileBased() ? QObject::tr("Profiles")
                     : isCombine()    ? QObject::tr("Bodies")
                     : isShell()      ? QObject::tr("Removed Faces")
                     : isOffsetFace() ? QObject::tr("Face")
                                      : QObject::tr("Edges"),
                     selRow);

    m_valueBox = new ExprSpinBox(w);
    m_valueBox->setDecimals(3);
    QObject::connect(m_valueBox, &ExprSpinBox::exprChanged, m_valueBox, [this](const QString& e) {
        if (!m_updatingPanel)
            m_valueExpr = e.toStdString();
    });
    m_valueBox->setKeyboardTracking(true);
    if (isShell()) {
        m_valueBox->setRange(0.05, 1000);
        m_valueBox->setDecimals(2);
        m_valueBox->setSuffix(" mm");
        form->addRow(QObject::tr("Thickness"), m_valueBox);
        m_dirBox = new QComboBox(w);
        m_dirBox->addItems({QObject::tr("Inside"), QObject::tr("Outside")});
        form->addRow(QObject::tr("Direction"), m_dirBox);
        QObject::connect(m_dirBox, &QComboBox::currentIndexChanged, m_dirBox, [this](int i) {
            if (m_updatingPanel)
                return;
            m_outward = i == 1;
            pushPreview();
        });
    }
    else if (isCombine()) {
        m_valueBox->hide();
        m_opBox = new QComboBox(w);
        m_opBox->addItems({QObject::tr("Join (union)"), QObject::tr("Cut (subtract)"), QObject::tr("Intersect")});
        if (essentialUi())  // v0.5: advanced option hidden in the Essential profile
            if (auto* v = qobject_cast<QListView*>(m_opBox->view()))
                v->setRowHidden(2, true);
        form->addRow(QObject::tr("Operation"), m_opBox);
        QObject::connect(m_opBox, &QComboBox::currentIndexChanged, m_opBox, [this](int i) {
            if (m_updatingPanel)
                return;
            m_op = opFromIndex(i + 1);
            pushPreview();
        });
        auto* keep = new QCheckBox(QObject::tr("Keep tool bodies"), w);
        keep->setChecked(m_keepTools);
        QObject::connect(keep, &QCheckBox::toggled, keep, [this](bool on) {
            m_keepTools = on;
            pushPreview();
        });
        form->addRow("", keep);
        auto* help = new QLabel(QObject::tr("<small>Click the target body first, then the tool bodies. "
                                            "Click a body again to deselect it.</small>"),
                                w);
        help->setWordWrap(true);
        help->setStyleSheet("color: #9aa3ad;");
        form->addRow(help);
    }
    else if (m_type == FeatureType::Extrude) {
        m_valueBox->setRange(-1e5, 1e5);
        m_valueBox->setSuffix(" mm");
        form->addRow(QObject::tr("Distance"), m_valueBox);
        m_dirBox = new QComboBox(w);
        m_dirBox->addItems({QObject::tr("One side"), QObject::tr("Symmetric")});
        form->addRow(QObject::tr("Direction"), m_dirBox);
        QObject::connect(m_dirBox, &QComboBox::currentIndexChanged, m_dirBox, [this](int i) {
            if (m_updatingPanel)
                return;
            m_symmetric = i == 1;
            pushPreview();
            m_ws.viewport()->refreshOverlay();
        });
        auto* flip = new QPushButton(QObject::tr("Flip direction"), w);
        QObject::connect(flip, &QPushButton::clicked, flip, [this] {
            m_value = -m_value;
            if (!m_valueExpr.empty())
                m_valueExpr = "-(" + m_valueExpr + ")";
            updatePanel();
            pushPreview();
        });
        form->addRow("", flip);
    }
    else if (isOffsetFace()) {
        m_valueBox->setRange(-1e5, 1e5);
        m_valueBox->setSuffix(" mm");
        form->addRow(QObject::tr("Distance"), m_valueBox);
        m_opInfo = new QLabel(w);
        form->addRow(QObject::tr("Operation"), m_opInfo);
        auto* flip = new QPushButton(QObject::tr("Push / Pull (flip)"), w);
        QObject::connect(flip, &QPushButton::clicked, flip, [this] {
            m_value = -m_value;
            if (!m_valueExpr.empty())
                m_valueExpr = "-(" + m_valueExpr + ")";
            updatePanel();
            pushPreview();
        });
        form->addRow("", flip);
    }
    else if (isPrimitive()) {
        m_planeBox = new QComboBox(w);
        m_planeBox->addItems({QObject::tr("XY (top)"), QObject::tr("XZ (front)"), QObject::tr("YZ (right)"),
                              m_prim.plane.kind == "Face" ? QObject::tr("Selected face") : QObject::tr("Click a face...")});
        m_planeBox->setCurrentIndex(m_prim.plane.kind == "XZ" ? 1 : m_prim.plane.kind == "YZ" ? 2 : m_prim.plane.kind == "Face" ? 3 : 0);
        form->addRow(QObject::tr("Base plane"), m_planeBox);
        QObject::connect(m_planeBox, &QComboBox::currentIndexChanged, m_planeBox, [this](int i) {
            if (m_updatingPanel)
                return;
            if (i == 3) {
                if (m_prim.plane.kind != "Face")
                    m_ws.status(QObject::tr("Click a planar face in the view"));
                return;
            }
            m_prim.plane.kind = i == 1 ? "XZ" : i == 2 ? "YZ" : "XY";
            m_prim.plane.face = {};
            pushPreview();
            m_ws.viewport()->refreshOverlay();
        });
        // secondary fields: expressions of global parameters accepted (bound per JSON pointer)
        auto field = [&](const QString& label, double* v, const char* ptr, double lo) {
            auto* box = new ExprSpinBox(w);
            box->setRange(lo, 1e5);
            box->setDecimals(3);
            box->setSuffix(" mm");
            box->setKeyboardTracking(true);
            box->setValueAndExpression(*v, m_exprs.count(ptr) ? m_exprs[ptr] : std::string{});
            QObject::connect(box, &QDoubleSpinBox::valueChanged, box, [this, v](double x) {
                if (m_updatingPanel)
                    return;
                *v = x;
                pushPreview();
                m_ws.viewport()->refreshOverlay();
            });
            QObject::connect(box, &ExprSpinBox::exprChanged, box, [this, ptr = std::string(ptr)](const QString& e) {
                if (!m_updatingPanel)
                    m_exprs[ptr] = e.toStdString();
            });
            form->addRow(label, box);
        };
        field(QObject::tr("Center X"), &m_prim.centerX, "/centerX", -1e5);
        field(QObject::tr("Center Y"), &m_prim.centerY, "/centerY", -1e5);
        if (m_type == FeatureType::Box) {
            field(QObject::tr("Length (X)"), &m_prim.length, "/length", 0.001);
            field(QObject::tr("Width (Y)"), &m_prim.width, "/width", 0.001);
        }
        else
            field(QObject::tr("Diameter (%1)").arg(QChar(0x2300)), &m_prim.diameter, "/diameter", 0.001);
        m_valueBox->setRange(-1e5, 1e5);
        m_valueBox->setSuffix(" mm");
        form->addRow(QObject::tr("Height (Z)"), m_valueBox);
        m_opBox = new QComboBox(w);
        m_opBox->addItems({QObject::tr("New body"), QObject::tr("Join"), QObject::tr("Cut")});
        form->addRow(QObject::tr("Operation"), m_opBox);
        QObject::connect(m_opBox, &QComboBox::currentIndexChanged, m_opBox, [this](int i) {
            if (m_updatingPanel)
                return;
            m_op = opFromIndex(i);
            pushPreview();
        });
    }
    else if (m_type == FeatureType::Revolve) {
        m_axisBox = new QComboBox(w);
        form->addRow(QObject::tr("Axis"), m_axisBox);
        QObject::connect(m_axisBox, &QComboBox::currentIndexChanged, m_axisBox, [this](int i) {
            if (m_updatingPanel || i < 0)
                return;
            m_axis = m_axisBox->itemData(i).toString().toStdString();
            pushPreview();
        });
        m_valueBox->setRange(-360, 360);
        m_valueBox->setSuffix(QString(QChar(0xB0)));
        form->addRow(QObject::tr("Angle"), m_valueBox);
    }
    else {
        m_valueBox->setRange(0.001, 1e4);
        m_valueBox->setSuffix(" mm");
        form->addRow(m_type == FeatureType::Fillet ? QObject::tr("Radius") : QObject::tr("Distance"), m_valueBox);
    }
    QObject::connect(m_valueBox, &QDoubleSpinBox::valueChanged, m_valueBox, [this](double v) {
        if (m_updatingPanel)
            return;
        if ((m_type == FeatureType::Extrude || isOffsetFace() || isPrimitive()) && std::abs(v) < 1e-6)
            return;
        m_value = v;
        pushPreview();
        m_ws.viewport()->refreshOverlay();
    });

    if (isProfileBased()) {
        m_opBox = new QComboBox(w);
        m_opBox->addItems({QObject::tr("New body"), QObject::tr("Join"), QObject::tr("Cut"), QObject::tr("Intersect")});
        if (essentialUi())  // v0.5: advanced option hidden in the Essential profile
            if (auto* v = qobject_cast<QListView*>(m_opBox->view()))
                v->setRowHidden(3, true);
        form->addRow(QObject::tr("Operation"), m_opBox);
        QObject::connect(m_opBox, &QComboBox::currentIndexChanged, m_opBox, [this](int i) {
            if (m_updatingPanel)
                return;
            m_op = opFromIndex(i);
            pushPreview();
        });
    }
    lay->addLayout(form);

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

    fillAxisCombo();
    updatePanel();
    if (!isCombine()) {
        m_valueBox->setFocus();
        m_valueBox->selectAll();
    }
    return w;
}

void FeatureEditor::fillAxisCombo()
{
    if (!m_axisBox)
        return;
    m_updatingPanel = true;
    m_axisBox->clear();
    m_axisBox->addItem(QObject::tr("Sketch X axis"), "X");
    m_axisBox->addItem(QObject::tr("Sketch Y axis"), "Y");
    if (!m_profiles.empty())
        if (FeaturePtr f = m_ws.model()->doc().find(m_profiles.front().first)) {
            const Sketch sk = f->as<SketchParams>().sketch;
            int n = 1;
            for (const SketchGeo& g : sk.geos)
                if (g.kind == GeoKind::Line) {
                    const QString label = g.construction ? QObject::tr("Construction line %1").arg(n)
                                                         : QObject::tr("Line %1").arg(n);
                    m_axisBox->addItem(label, QString::fromStdString(g.id));
                    ++n;
                }
        }
    const int idx = m_axisBox->findData(QString::fromStdString(m_axis));
    m_axisBox->setCurrentIndex(idx >= 0 ? idx : 1);
    m_updatingPanel = false;
}

void FeatureEditor::updatePanel()
{
    m_updatingPanel = true;
    if (m_selLabel) {
        if (isCombine())
            m_selLabel->setText(m_target.empty() ? QObject::tr("target: —")
                                                 : QObject::tr("target: %1, %n tool(s)", nullptr, int(m_tools.size()))
                                                       .arg(bodyName(m_target)));
        else
            m_selLabel->setText(isProfileBased() ? QObject::tr("%n selected", nullptr, int(m_profiles.size()))
                                : isShell()      ? QObject::tr("%n selected", nullptr, int(m_shellFaces.size()))
                                : isOffsetFace() ? QObject::tr("%n selected", nullptr, m_face.name.empty() ? 0 : 1)
                                                 : QObject::tr("%n selected", nullptr, int(m_edges.size())));
    }
    if (m_valueBox && (std::abs(m_valueBox->value() - m_value) > 1e-9 || m_valueBox->expression() != m_valueExpr))
        m_valueBox->setValueAndExpression(m_value, m_valueExpr);
    if (m_dirBox)
        m_dirBox->setCurrentIndex(isShell() ? (m_outward ? 1 : 0) : (m_symmetric ? 1 : 0));
    if (m_opBox)
        m_opBox->setCurrentIndex(isCombine() ? std::max(0, opIndex(m_op) - 1) : opIndex(m_op));
    if (m_opInfo)
        m_opInfo->setText(m_value >= 0 ? QString("<span style='color:%1'>%2</span>")
                                             .arg(QColor(0x4a, 0xa3, 0xff).name(), QObject::tr("Pull: join (adds material)"))
                                       : QString("<span style='color:%1'>%2</span>")
                                             .arg(Colors::Error.name(), QObject::tr("Push: cut (removes material)")));
    if (m_message) {
        QString msg;
        QColor c = Colors::Warning;
        if (auto snap = m_ws.model()->snapshot())
            if (const FeatureResult* r = snap->result(m_id)) {
                if (r->status == FeatureStatus::Error) {
                    msg = QString::fromStdString(r->message);
                    c = Colors::Error;
                }
                else if (r->status == FeatureStatus::Warning)
                    msg = QString::fromStdString(r->message);
            }
        QString html = msg.isEmpty() ? QString() : QString("<span style='color:%1'>%2</span>").arg(c.name(), msg.toHtmlEscaped());
        if (const QString why = m_ws.explainStep(m_id); !why.isEmpty())
            html += QString("<br><span style='color:#cfe8d2'>&#128161; %1</span>").arg(why.toHtmlEscaped());
        m_message->setText(html);
    }
    m_updatingPanel = false;
}

}  // namespace silex::ui
