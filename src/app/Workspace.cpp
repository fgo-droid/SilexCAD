#include "Workspace.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>

#include "FeatureEditor.h"
#include "TransformEditor.h"
#include "Icons.h"
#include "ModelController.h"
#include "SketchEditor.h"
#include "Theme.h"
#include "Viewport.h"

namespace silex::ui {

Workspace::Workspace(ModelController* model, Viewport* vp, QObject* parent)
    : QObject(parent), m_model(model), m_vp(vp)
{
    vp->setClient(this);
    connect(model, &ModelController::snapshotChanged, this, &Workspace::onSnapshot);
    connect(model, &ModelController::documentChanged, this, [this] {
        updateProfileVisibility();
        touchHighlight();
    });
}

Workspace::~Workspace() { m_vp->setClient(nullptr); }

// ------------------------------------------------------------------ snapshot & visibility

SelItem Workspace::findProfile(const Uuid& sketch, const std::string& key) const
{
    SelItem s;
    auto snap = m_model->snapshot();
    if (!snap)
        return s;
    for (size_t i = 0; i < snap->profiles.size(); ++i) {
        const ProfileView& pv = snap->profiles[i];
        if (pv.sketchId != sketch || pv.regionKey != key)
            continue;
        s.kind = SelKind::Profile;
        s.owner = sketch;
        s.name = key;
        s.object = int(i);
        s.index = 0;
        if (!pv.mesh.faces.empty()) {
            s.sig = pv.mesh.faces.front().sig;
            s.planar = true;
            s.plane = pv.mesh.faces.front().plane;
        }
        return s;
    }
    return s;
}

SelItem Workspace::findEdge(const TopoRef& ref) const
{
    SelItem s;
    auto snap = m_model->snapshot();
    if (!snap)
        return s;
    const double size = snap->bbox.diagonal();
    double best = 1e100;
    for (size_t b = 0; b < snap->bodies.size(); ++b) {
        const auto& edges = snap->bodies[b].mesh.edges;
        for (size_t i = 0; i < edges.size(); ++i) {
            if (edges[i].name != ref.name)
                continue;
            const double d = ref.sig.distance(edges[i].sig, size);
            if (d < best) {
                best = d;
                s.kind = SelKind::Edge;
                s.owner = snap->bodies[b].id;
                s.object = int(b);
                s.index = int(i);
                s.name = edges[i].name;
                s.sig = edges[i].sig;
            }
        }
    }
    return s;
}

static bool isTransformType(const std::string& t)
{
    return t == FeatureType::Mirror || t == FeatureType::LinearPattern || t == FeatureType::CircularPattern;
}

SelItem Workspace::bodyItem(const Uuid& bodyId) const
{
    SelItem s;
    auto snap = m_model->snapshot();
    if (!snap)
        return s;
    for (size_t b = 0; b < snap->bodies.size(); ++b)
        if (snap->bodies[b].id == bodyId) {
            s.kind = SelKind::Body;
            s.owner = bodyId;
            s.object = int(b);
            s.index = -1;
            s.name = snap->bodies[b].name;
        }
    return s;
}

std::vector<Uuid> Workspace::selectedBodies() const
{
    std::vector<Uuid> ids;
    for (const SelItem& s : m_hl.selected)
        if ((s.kind == SelKind::Body || s.kind == SelKind::Face || s.kind == SelKind::Edge) &&
            std::find(ids.begin(), ids.end(), s.owner) == ids.end())
            ids.push_back(s.owner);
    return ids;
}

void Workspace::toggleBody(const Uuid& bodyId)
{
    std::vector<SelItem> sel = m_hl.selected;
    auto it = std::find_if(sel.begin(), sel.end(),
                           [&](const SelItem& s) { return s.kind == SelKind::Body && s.owner == bodyId; });
    if (it != sel.end()) {
        sel.erase(it);
    }
    else {
        // a whole body supersedes its individual faces / edges
        std::erase_if(sel, [&](const SelItem& s) { return s.owner == bodyId; });
        if (SelItem b = bodyItem(bodyId); b.valid())
            sel.push_back(b);
    }
    setSelection(std::move(sel));
}

void Workspace::combineSelected(BooleanOp op)
{
    const std::vector<Uuid> bodies = selectedBodies();
    if (bodies.size() < 2) {
        status(tr("Select at least two bodies (double-click a body, Ctrl+double-click to add)"));
        return;
    }
    if (m_session) {
        if (!m_session->commit())
            return;
        endSession();
    }
    CombineParams p;
    p.target = bodies.front();
    p.tools.assign(bodies.begin() + 1, bodies.end());
    p.op = op;
    const std::string name = m_model->doc().nextName(op == BooleanOp::Join  ? "Merge"
                                                     : op == BooleanOp::Cut ? "Subtract"
                                                                            : "Intersect");
    m_model->execute(AddFeatureCommand(makeFeature(FeatureType::Combine, name, Json(p))));
    clearSelection();
    status(tr("%1 created — double-click it in the timeline to edit").arg(QString::fromStdString(name)));
}

namespace {
SelItem findFace(const ModelSnapshot& snap, const SelItem& old)
{
    SelItem s;
    double best = 1e100;
    for (size_t b = 0; b < snap.bodies.size(); ++b) {
        if (snap.bodies[b].id != old.owner)
            continue;
        const auto& faces = snap.bodies[b].mesh.faces;
        for (size_t i = 0; i < faces.size(); ++i) {
            if (faces[i].name != old.name)
                continue;
            const double d = old.sig.distance(faces[i].sig, snap.bbox.diagonal());
            if (d < best) {
                best = d;
                s = old;
                s.object = int(b);
                s.index = int(i);
                s.sig = faces[i].sig;
                s.planar = faces[i].planar;
                s.plane = faces[i].plane;
            }
        }
    }
    return s;
}
}  // namespace

void Workspace::onSnapshot()
{
    auto snap = m_model->snapshot();
    updateProfileVisibility();
    std::vector<SelItem> sel;
    if (snap) {
        for (const SelItem& s : m_hl.selected) {
            SelItem r;
            switch (s.kind) {
                case SelKind::Face: r = findFace(*snap, s); break;
                case SelKind::Edge: r = findEdge(s.ref()); break;
                case SelKind::Profile: r = findProfile(s.owner, s.name); break;
                case SelKind::OriginPlane: r = s; break;
                case SelKind::Body: r = bodyItem(s.owner); break;
                default: break;
            }
            if (r.valid())
                sel.push_back(r);
        }
    }
    m_hl.selected = std::move(sel);
    m_hl.hover = {};
    m_vp->setSnapshot(snap);
    if (m_session)
        m_session->onSnapshot();
    touchHighlight();
    emit selectionChanged();
}

void Workspace::updateProfileVisibility()
{
    auto snap = m_model->snapshot();
    const DocState& d = m_model->displayedDoc();
    std::set<Uuid> consumed;
    for (int i = 0; i < d.marker && i < int(d.features.size()); ++i) {
        const auto& f = d.features[size_t(i)];
        if (f->suppressed)
            continue;
        if (f->type == FeatureType::Extrude || f->type == FeatureType::Revolve)
            consumed.insert(f->params.value("profile", Json::object()).value("sketch", std::string{}));
    }
    const std::set<Uuid> inUse = m_session ? m_session->sketchesInUse() : std::set<Uuid>{};
    const Uuid hidden = m_session ? m_session->hiddenSketch() : Uuid{};
    std::vector<bool> vis;
    if (snap)
        for (const auto& pv : snap->profiles)
            vis.push_back((!consumed.count(pv.sketchId) || inUse.count(pv.sketchId)) && pv.sketchId != hidden);
    if (vis != m_hl.profileVisible) {
        m_hl.profileVisible = std::move(vis);
        touchHighlight();
    }
}

void Workspace::touchHighlight()
{
    ++m_hl.version;
    m_vp->highlightChanged();
}

void Workspace::setSelection(std::vector<SelItem> items)
{
    m_hl.selected = std::move(items);
    touchHighlight();
    emit selectionChanged();
}

void Workspace::setHover(const SelItem& s)
{
    if (s.sameAs(m_hl.hover))
        return;
    m_hl.hover = s;
    touchHighlight();
}

uint32_t Workspace::currentFilter() const
{
    if (m_pickingPlane)
        return SelFilter::Faces | SelFilter::PlanarOnly | SelFilter::OriginPlanes;
    if (m_session)
        return m_session->pickFilter();
    return SelFilter::Model | SelFilter::Profiles;
}

// ------------------------------------------------------------------ sessions

void Workspace::beginSession(std::unique_ptr<EditSession> s)
{
    m_pickingPlane = false;
    m_hl.showOriginPlanes = false;
    m_session = std::move(s);
    emit sessionChanged();
    updateProfileVisibility();
    touchHighlight();
}

void Workspace::endSession()
{
    m_session.reset();
    m_model->endPreview();
    m_hl.dimBodies = false;
    m_hl.showPreview = true;
    emit sessionChanged();
    m_hl.selected.clear();
    m_hl.hover = {};
    updateProfileVisibility();
    touchHighlight();
    emit selectionChanged();
}

void Workspace::finishSession()
{
    if (m_session && m_session->commit())
        endSession();
}

void Workspace::cancelSession()
{
    if (m_pickingPlane) {
        m_pickingPlane = false;
        m_hl.showOriginPlanes = false;
        touchHighlight();
        return;
    }
    if (!m_session)
        return;
    m_session->cancel();
    endSession();
}

void Workspace::startSketch()
{
    std::vector<SelItem> keep = m_hl.selected;
    if (m_session) {
        if (!m_session->commit())
            return;
        endSession();
    }
    for (const SelItem& s : keep)
        if (s.planar && (s.kind == SelKind::Face || s.kind == SelKind::OriginPlane)) {
            startSketchOn(s);
            return;
        }
    m_pickingPlane = true;
    m_hl.showOriginPlanes = true;
    touchHighlight();
    status(tr("Select a plane or a planar face to sketch on"), 0);
}

void Workspace::startSketchOn(const SelItem& item)
{
    SketchPlaneRef ref;
    if (item.kind == SelKind::OriginPlane)
        ref.kind = item.name;
    else {
        ref.kind = "Face";
        ref.face = item.ref();
    }
    m_hl.selected.clear();
    beginSession(std::make_unique<SketchEditor>(*this, nullptr, ref, item.plane));
    status(tr("Sketching — L line, R rectangle, C circle, A arc, D dimension, Enter to finish"));
}

void Workspace::startFeature(const std::string& type)
{
    std::vector<SelItem> keep = m_hl.selected;
    if (m_session) {
        // Zero context break: finishing the sketch and starting the operation is one gesture;
        // the FeatureEditor then picks the sketch profile automatically when unambiguous.
        if (!m_session->commit())
            return;
        endSession();
    }
    m_hl.selected = keep;
    m_pickingPlane = false;
    m_hl.showOriginPlanes = false;
    if (isTransformType(type))
        beginSession(std::make_unique<TransformEditor>(*this, type, nullptr));
    else
        beginSession(std::make_unique<FeatureEditor>(*this, type, nullptr));
}

void Workspace::editFeature(const Uuid& id)
{
    FeaturePtr f = m_model->doc().find(id);
    if (!f)
        return;
    if (m_session) {
        if (m_session->editedFeature() == id)
            return;
        if (!m_session->commit())
            return;
        endSession();
    }
    if (f->type == FeatureType::Sketch) {
        const auto p = f->as<SketchParams>();
        Placement pl = originPlacement(p.plane.kind);
        if (auto snap = m_model->snapshot())
            if (const SketchView* sv = snap->sketch(id))
                pl = sv->plane;
        m_hl.selected.clear();
        beginSession(std::make_unique<SketchEditor>(*this, f, p.plane, pl));
        return;
    }
    if (f->type == FeatureType::ImportStep) {
        status(tr("Imported geometry has no editable parameters"));
        return;
    }
    m_hl.selected.clear();
    if (isTransformType(f->type))
        beginSession(std::make_unique<TransformEditor>(*this, f->type, f));
    else
        beginSession(std::make_unique<FeatureEditor>(*this, f->type, f));
}

bool Workspace::undo()
{
    if (m_session)
        return m_session->undo();
    m_model->undo();
    return true;
}

bool Workspace::redo()
{
    if (m_session)
        return m_session->redo();
    m_model->redo();
    return true;
}

// ------------------------------------------------------------------ marking menu

std::vector<MenuAction> Workspace::contextActions()
{
    if (m_pickingPlane)
        return {{tr("Cancel"), icon(IconId::Cancel), [this] { cancelSession(); }}};
    if (m_session) {
        auto a = m_session->menuActions();
        if (a.empty()) {
            a.push_back({tr("OK"), icon(IconId::Finish), [this] { finishSession(); }});
            a.push_back({tr("Cancel"), icon(IconId::Cancel), [this] { cancelSession(); }});
        }
        return a;
    }
    if (const auto bodies = selectedBodies(); bodies.size() >= 2) {
        // Contextual: several bodies selected => direct booleans (first selected = target).
        const QString target = QString::fromStdString(bodyItem(bodies.front()).name);
        return {
            {tr("Merge %n bodies", nullptr, int(bodies.size())), icon(IconId::Combine),
             [this] { combineSelected(BooleanOp::Join); }},
            {tr("Subtract from %1").arg(target), icon(IconId::Combine), [this] { combineSelected(BooleanOp::Cut); }},
            {tr("Intersect"), icon(IconId::Combine), [this] { combineSelected(BooleanOp::Intersect); }},
            {tr("Combine..."), icon(IconId::Combine), [this] { startFeature(FeatureType::Combine); }},
            {tr("Clear selection"), icon(IconId::Cancel), [this] { clearSelection(); }},
            {tr("Undo"), icon(IconId::Undo), [this] { undo(); }, m_model->history().canUndo()},
            {tr("Fit all"), icon(IconId::Fit), [this] { m_vp->fitAll(); }},
        };
    }
    return {
        {tr("Sketch"), icon(IconId::Sketch), [this] { startSketch(); }},
        {tr("Extrude"), icon(IconId::Extrude), [this] { startFeature(FeatureType::Extrude); }},
        {tr("Revolve"), icon(IconId::Revolve), [this] { startFeature(FeatureType::Revolve); }},
        {tr("Fillet"), icon(IconId::Fillet), [this] { startFeature(FeatureType::Fillet); }},
        {tr("Chamfer"), icon(IconId::Chamfer), [this] { startFeature(FeatureType::Chamfer); }},
        {tr("Shell"), icon(IconId::Shell), [this] { startFeature(FeatureType::Shell); }},
        {tr("Combine"), icon(IconId::Combine), [this] { startFeature(FeatureType::Combine); }},
        {tr("Mirror"), icon(IconId::Mirror), [this] { startFeature(FeatureType::Mirror); }},
        {tr("Linear Pattern"), icon(IconId::LinearPattern), [this] { startFeature(FeatureType::LinearPattern); }},
        {tr("Circular Pattern"), icon(IconId::CircularPattern), [this] { startFeature(FeatureType::CircularPattern); }},
        {tr("Undo"), icon(IconId::Undo), [this] { undo(); }, m_model->history().canUndo()},
        {tr("Fit all"), icon(IconId::Fit), [this] { m_vp->fitAll(); }},
    };
}

// ------------------------------------------------------------------ viewport events

void Workspace::viewportMousePress(QMouseEvent* e)
{
    if (e->button() == Qt::RightButton) {
        m_rightPressPos = e->position().toPoint();
        emit markingMenuRequested(m_rightPressPos);
        return;
    }
    if (m_session && !m_pickingPlane && m_session->mousePress(e))
        return;
    if (e->button() != Qt::LeftButton)
        return;
    const SelItem hit = m_vp->pickAt(e->position(), currentFilter());
    const bool additive = e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);

    if (m_pickingPlane) {
        if (hit.valid() && hit.planar)
            startSketchOn(hit);
        return;
    }
    if (m_session) {
        m_session->onPicked(hit, additive);
        return;
    }
    std::vector<SelItem> sel = additive ? m_hl.selected : std::vector<SelItem>{};
    if (hit.valid()) {
        auto it = std::find_if(sel.begin(), sel.end(), [&](const SelItem& s) { return s.sameAs(hit); });
        if (it != sel.end())
            sel.erase(it);
        else
            sel.push_back(hit);
    }
    setSelection(std::move(sel));
}

void Workspace::viewportMouseMove(QMouseEvent* e)
{
    if (m_session && !m_pickingPlane && m_session->mouseMove(e))
        return;
    if (e->buttons() != Qt::NoButton)
        return;
    setHover(m_vp->pickAt(e->position(), currentFilter()));
}

void Workspace::viewportMouseRelease(QMouseEvent* e)
{
    if (m_session && !m_pickingPlane)
        m_session->mouseRelease(e);
}

void Workspace::viewportMouseDoubleClick(QMouseEvent* e)
{
    if (m_session && !m_pickingPlane && m_session->mouseDoubleClick(e))
        return;
    if (m_session || m_pickingPlane || e->button() != Qt::LeftButton)
        return;
    const SelItem hit = m_vp->pickAt(e->position(), currentFilter());
    if (hit.kind == SelKind::Profile) {
        setSelection({hit});
        startFeature(FeatureType::Extrude);
        return;
    }
    // Double-click a face/edge: select the whole body (Ctrl/Shift: add to the selection).
    if (hit.kind == SelKind::Face || hit.kind == SelKind::Edge) {
        const bool additive = e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
        if (additive)
            toggleBody(hit.owner);
        else if (SelItem b = bodyItem(hit.owner); b.valid())
            setSelection({b});
        const auto n = selectedBodies().size();
        if (n >= 2)
            status(tr("%1 bodies selected — right-click to merge / subtract / intersect").arg(n));
    }
}

bool Workspace::viewportKeyPress(QKeyEvent* e)
{
    if (m_session && !m_pickingPlane && m_session->keyPress(e))
        return true;
    if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier))
        return false;
    switch (e->key()) {
        case Qt::Key_Escape:
            if (m_pickingPlane || m_session)
                cancelSession();
            else
                clearSelection();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (m_session)
                finishSession();
            return true;
        case Qt::Key_S: startSketch(); return true;
        case Qt::Key_E: startFeature(FeatureType::Extrude); return true;
        case Qt::Key_R: startFeature(FeatureType::Revolve); return true;
        case Qt::Key_F: startFeature(FeatureType::Fillet); return true;
        case Qt::Key_C:
            startFeature((e->modifiers() & Qt::ShiftModifier) ? FeatureType::CircularPattern : FeatureType::Chamfer);
            return true;
        case Qt::Key_P:
            if (!(e->modifiers() & Qt::ShiftModifier))
                return false;
            startFeature(FeatureType::LinearPattern);
            return true;
        case Qt::Key_H: startFeature(FeatureType::Shell); return true;
        case Qt::Key_M: startFeature(FeatureType::Mirror); return true;
        case Qt::Key_B: startFeature(FeatureType::Combine); return true;
        case Qt::Key_Home:
        case Qt::Key_W: m_vp->fitAll(); return true;
        case Qt::Key_0: m_vp->setStandardView(0); return true;
        case Qt::Key_1: m_vp->setStandardView(1); return true;
        case Qt::Key_3: m_vp->setStandardView(3); return true;
        case Qt::Key_7: m_vp->setStandardView(7); return true;
        default: return false;
    }
}

// ------------------------------------------------------------------ overlay

void Workspace::paintSketches(QPainter& p)
{
    auto snap = m_model->snapshot();
    if (!snap)
        return;
    const DocState& d = m_model->displayedDoc();
    std::set<Uuid> visible;
    for (size_t i = 0; i < snap->profiles.size() && i < m_hl.profileVisible.size(); ++i)
        if (m_hl.profileVisible[i])
            visible.insert(snap->profiles[i].sketchId);
    const Uuid hidden = m_session ? m_session->hiddenSketch() : Uuid{};
    // sketches with only open curves have no profile: show them while not consumed
    std::set<Uuid> consumed;
    for (int i = 0; i < d.marker && i < int(d.features.size()); ++i) {
        const auto& f = d.features[size_t(i)];
        if (f->type == FeatureType::Extrude || f->type == FeatureType::Revolve)
            consumed.insert(f->params.value("profile", Json::object()).value("sketch", std::string{}));
    }
    const std::set<Uuid> inUse = m_session ? m_session->sketchesInUse() : std::set<Uuid>{};

    for (const SketchView& sv : snap->sketches) {
        if (sv.id == hidden)
            continue;
        if (!visible.count(sv.id) && consumed.count(sv.id) && !inUse.count(sv.id))
            continue;
        FeaturePtr f = d.find(sv.id);
        if (!f || !d.isActive(sv.id))
            continue;
        const Sketch sk = f->as<SketchParams>().sketch;
        auto toPx = [&](Vec2 q, QPointF* out) { return m_vp->toScreen(sv.plane.toWorld(q), out); };
        for (const SketchGeo& g : sk.geos) {
            QPen pen(g.construction ? Colors::SketchConstruction : Colors::SketchInactive, 1.3);
            if (g.construction)
                pen.setStyle(Qt::DashLine);
            p.setPen(pen);
            QPainterPath path;
            auto sample = [&](double a0, double a1, Vec2 c, double r) {
                const int n = 72;
                for (int k = 0; k <= n; ++k) {
                    const double a = a0 + (a1 - a0) * k / n;
                    QPointF q;
                    if (!toPx(c + Vec2{std::cos(a), std::sin(a)} * r, &q))
                        return;
                    k == 0 ? path.moveTo(q) : path.lineTo(q);
                }
            };
            switch (g.kind) {
                case GeoKind::Line: {
                    QPointF a, b;
                    if (toPx(g.p1, &a) && toPx(g.p2, &b)) {
                        path.moveTo(a);
                        path.lineTo(b);
                    }
                    break;
                }
                case GeoKind::Circle: sample(0, 2 * kPi, g.c, g.r); break;
                case GeoKind::Arc: sample(g.a0, g.a1, g.c, g.r); break;
                case GeoKind::Point: {
                    QPointF a;
                    if (toPx(g.p1, &a))
                        p.drawEllipse(a, 2.5, 2.5);
                    break;
                }
            }
            p.drawPath(path);
        }
    }
}

void Workspace::paintOverlay(QPainter& p)
{
    paintSketches(p);
    if (m_session)
        m_session->paintOverlay(p);

    if (m_pickingPlane) {
        const QString t = tr("Select a plane or a planar face");
        QFont f = p.font();
        f.setPixelSize(14);
        f.setBold(true);
        p.setFont(f);
        const QRectF r(0, 12, m_vp->width(), 28);
        p.setPen(QColor(255, 255, 255, 230));
        p.drawText(r, Qt::AlignHCenter | Qt::AlignVCenter, t);
    }

    // Hovered entity, with its persistent name (useful to understand the TNP layer).
    if (m_hl.hover.valid()) {
        QString kind;
        switch (m_hl.hover.kind) {
            case SelKind::Face: kind = tr("Face"); break;
            case SelKind::Edge: kind = tr("Edge"); break;
            case SelKind::Profile: kind = tr("Profile"); break;
            case SelKind::OriginPlane: kind = tr("Plane"); break;
            case SelKind::Body: kind = tr("Body"); break;
            default: break;
        }
        QString name = QString::fromStdString(m_hl.hover.name);
        if (name.size() > 90)
            name = name.left(87) + "...";
        QFont f = p.font();
        f.setPixelSize(11);
        f.setBold(false);
        p.setFont(f);
        p.setPen(QColor(200, 206, 214, 200));
        p.drawText(QRectF(0, m_vp->height() - 22, m_vp->width() - 10, 18), Qt::AlignRight | Qt::AlignVCenter,
                   kind + "  " + name);
    }
}

}  // namespace silex::ui
