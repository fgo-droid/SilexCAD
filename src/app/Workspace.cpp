#include "Workspace.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <algorithm>

#include "AnalysisTools.h"
#include "FeatureEditor.h"
#include "SweepLoftEditor.h"
#include "TransformEditor.h"
#include "Icons.h"
#include "ModelController.h"
#include "SketchEditor.h"
#include "DirectEditor.h"
#include "HoleEditor.h"
#include "TextEditor.h"
#include "EnclosureWizard.h"
#include "ComponentLibrary.h"
#include "PrintTools.h"
#include "model/Pedagogy.h"
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
        // v0.5: an edit of an existing step or of the parameters (not an addition) shows the previous
        // geometry as a ghost; m_model->snapshot() is still the state before the change here
        const std::string label = m_model->history().undoLabel();
        if (m_ghostOn && !m_session && !m_ghostHold && (label.rfind("Edit ", 0) == 0 || label == "Edit parameters"))
            if (auto snap = m_model->snapshot(); snap && !snap->bodies.empty()) {
                m_ghost = snap;
                m_ghostRev = snap->revision;
                ++m_ghostGen;  // the fade starts when the new geometry arrives
            }
    });
}

void Workspace::setGhostEnabled(bool on)
{
    m_ghostOn = on;
    if (!on) {
        m_ghost.reset();
        m_ghostHold = false;
        m_vp->refreshOverlay();
    }
}

void Workspace::holdGhost(bool on)
{
    if (!m_ghostOn)
        return;
    if (on) {
        if (!m_ghostHold)
            if (auto snap = m_model->snapshot(); snap && !snap->bodies.empty()) {
                m_ghost = snap;
                m_ghostRev = snap->revision;
            }
        m_ghostHold = true;
        ++m_ghostGen;
    }
    else if (m_ghostHold) {
        m_ghostHold = false;
        armGhostFade();
    }
}

void Workspace::armGhostFade()
{
    if (!m_ghost)
        return;
    const int gen = ++m_ghostGen;
    QTimer::singleShot(3000, this, [this, gen] {
        if (gen != m_ghostGen || m_ghostHold)
            return;
        m_ghost.reset();
        m_vp->refreshOverlay();
    });
}

void Workspace::paintGhost(QPainter& p)
{
    if (!m_ghost)
        return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(0xd8, 0xdc, 0xe2, 150), 1.2));
    for (const auto& b : m_ghost->bodies) {
        const auto& e = b.mesh.edgePoints;
        for (size_t i = 0; i + 5 < e.size(); i += 6) {
            QPointF a, c;
            if (m_vp->toScreen(Vec3{e[i], e[i + 1], e[i + 2]}, &a) && m_vp->toScreen(Vec3{e[i + 3], e[i + 4], e[i + 5]}, &c))
                p.drawLine(a, c);
        }
    }
    QFont f = p.font();
    f.setPixelSize(12);
    p.setFont(f);
    p.setPen(QColor(0xd8, 0xdc, 0xe2, 200));
    p.drawText(QPointF(14, 22), m_model->pedagogyFrench() ? QString::fromUtf8("Fantôme : état avant la modification")
                                                          : tr("Ghost: state before the change"));
    p.restore();
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

static bool isSweepLoftType(const std::string& t) { return t == FeatureType::Sweep || t == FeatureType::Loft; }

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

void Workspace::setBed(const Bed& bed)
{
    m_bed = bed;
    updateBed();
}

QStringList Workspace::bodiesOutsideBed() const { return m_outside; }

void Workspace::updateBed()
{
    m_outside.clear();
    if (m_bed.on)
        if (auto snap = m_model->snapshot())
            for (const auto& b : snap->bodies) {
                if (b.locked() || bodyHidden(b.id) || !b.mesh.bbox.valid())
                    continue;
                const BBox& bb = b.mesh.bbox;
                const double e = 0.05;
                if (bb.min.x < -m_bed.size.x / 2 - e || bb.max.x > m_bed.size.x / 2 + e || bb.min.y < -m_bed.size.y / 2 - e ||
                    bb.max.y > m_bed.size.y / 2 + e || bb.min.z < -e || bb.max.z > m_bed.size.z + e)
                    m_outside << QString::fromStdString(b.name);
            }
    Viewport::BedSpec spec;
    spec.on = m_bed.on;
    spec.size = QVector3D(float(m_bed.size.x), float(m_bed.size.y), float(m_bed.size.z));
    spec.exceeded = !m_outside.isEmpty();
    m_vp->setBed(spec);
    m_vp->refreshOverlay();
}

void Workspace::startLayFlat()
{
    for (const SelItem& s : m_hl.selected)
        if (s.kind == SelKind::Face && s.planar) {
            layFlat(s);
            return;
        }
    if (m_session) {
        if (!m_session->commit())
            return;
        endSession();
    }
    m_layFlatPick = true;
    status(tr("Lay flat on bed: click the planar face that must rest on the build plate (Esc cancels)"), 8000);
    m_vp->refreshOverlay();
}

void Workspace::layFlat(const SelItem& face)
{
    m_layFlatPick = false;
    if (face.kind != SelKind::Face || !face.planar) {
        status(tr("Lay flat needs a planar face"));
        return;
    }
    TransformBodyParams tp;
    tp.bodies = {face.owner};
    tp.mate = Json{{"mode", "bed"}, {"moving", face.ref()}, {"center", true}};
    FeaturePtr f = makeFeature(FeatureType::TransformBody, m_model->doc().nextName("LayFlat"), Json(tp));
    m_model->execute(AddFeatureCommand(f));
    if (!m_bed.on) {
        m_bed.on = true;  // show the plate: the result is easier to read
        emit bedChanged();
    }
    clearSelection();
    updateBed();
    status(tr("Laid flat on the bed (Z = 0), centered on the plate"));
}

void Workspace::startEnclosureWizard()
{
    if (m_session) {
        if (!m_session->commit())
            return;
        endSession();
    }
    auto spec = askEnclosureSpec(m_vp->window(), m_model->pedagogyFrench());
    if (!spec)
        return;
    std::string err;
    auto res = buildEnclosure(m_model->doc(), *spec, &err);
    if (!res) {
        status(tr("Enclosure wizard: %1").arg(QString::fromStdString(err)), 10000);
        return;
    }
    m_model->execute(*res->command);
    m_model->noteEvent("enclosure_created");
    m_vp->fitAll();
    status(tr("Enclosure created: %1 commented steps in the timeline, sizes in Parameters (Ctrl+P)").arg(res->featureCount), 10000);
}

void Workspace::onSnapshot()
{
    auto snap = m_model->snapshot();
    updateProfileVisibility();
    if (m_bed.on)
        updateBed();
    // v0.5: the ghost fades out 3 s after the edited geometry arrived
    if (m_ghost && !m_ghostHold && snap && snap->revision != m_ghostRev && !m_model->inPreview())
        armGhostFade();
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
        if (f->type == FeatureType::Sweep) {
            consumed.insert(f->params.value("profile", Json::object()).value("sketch", std::string{}));
            consumed.insert(f->params.value("path", Json::object()).value("sketch", std::string{}));
        }
        if (f->type == FeatureType::Loft)
            for (const auto& sec : f->params.value("sections", Json::array()))
                consumed.insert(sec.value("sketch", std::string{}));
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
    if (!m_session->editedFeature().empty())
        holdGhost(true);  // editing an existing step: the state before stays visible as a ghost
    emit sessionChanged();
    updateProfileVisibility();
    touchHighlight();
}

void Workspace::endSession()
{
    m_session.reset();
    m_model->endPreview();
    holdGhost(false);
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
    if (item.kind == SelKind::OriginPlane && item.name.rfind("plane:", 0) == 0) {
        ref.kind = "Plane";  // v0.4 construction plane
        ref.planeId = item.owner;
    }
    else if (item.kind == SelKind::OriginPlane)
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
    if (type == FeatureType::FastenerHole)
        beginSession(std::make_unique<HoleEditor>(*this, nullptr));
    else if (type == FeatureType::EmbossText)
        beginSession(std::make_unique<TextEditor>(*this, nullptr));
    else if (DirectEditor::handles(type))
        beginSession(std::make_unique<DirectEditor>(*this, type, nullptr));
    else if (isTransformType(type))
        beginSession(std::make_unique<TransformEditor>(*this, type, nullptr));
    else if (isSweepLoftType(type))
        beginSession(std::make_unique<SweepLoftEditor>(*this, type, nullptr));
    else
        beginSession(std::make_unique<FeatureEditor>(*this, type, nullptr));
}

void Workspace::startPressPull()
{
    if (m_session && m_session->isSketch()) {  // finishing a sketch: Q extrudes its profile
        startFeature(FeatureType::Extrude);
        return;
    }
    bool profile = false, face = false;
    for (const SelItem& s : m_hl.selected) {
        profile |= s.kind == SelKind::Profile;
        face |= s.kind == SelKind::Face && s.planar;
    }
    if (profile) {
        startFeature(FeatureType::Extrude);
        return;
    }
    startFeature(FeatureType::OffsetFace);
    if (auto* fe = dynamic_cast<FeatureEditor*>(m_session.get()))
        fe->setPressPullMode(true);
    if (!face)
        status(tr("Press / Pull: click a planar face of a solid, or a closed sketch profile"), 0);
}

QString Workspace::explainStep(const Uuid& id) const
{
    auto snap = m_model->snapshot();
    const FeaturePtr f = m_model->doc().find(id);
    if (!snap || !f)
        return {};
    const FeatureResult* r = snap->result(id);
    if (!r || (r->status != FeatureStatus::Error && r->status != FeatureStatus::Warning))
        return {};
    ErrorContext ctx;
    for (const auto& b : snap->bodies)
        if (b.mesh.bbox.valid()) {
            const Vec3 e = b.mesh.bbox.max - b.mesh.bbox.min;
            const double lo = std::min({e.x, e.y, e.z}), hi = std::max({e.x, e.y, e.z});
            ctx.smallestBodyExtent = ctx.smallestBodyExtent > 0 ? std::min(ctx.smallestBodyExtent, lo) : lo;
            ctx.largestBodyExtent = std::max(ctx.largestBodyExtent, hi);
        }
    return QString::fromStdString(explainFeatureError(f->type, r->message, f->params, ctx, m_model->pedagogyFrench()));
}

void Workspace::startPrintability()
{
    std::vector<SelItem> keep = m_hl.selected;
    if (m_session) {
        if (!m_session->commit())
            return;
        endSession();
    }
    m_hl.selected = keep;
    m_pickingPlane = false;
    m_hl.showOriginPlanes = false;
    beginSession(std::make_unique<PrintabilityTool>(*this));
}

void Workspace::startPlane(const std::string& mode)
{
    startFeature(FeatureType::ConstructionPlane);
    if (auto* de = dynamic_cast<DirectEditor*>(m_session.get()))
        de->setPlaneMode(mode);
}

void Workspace::startDeleteFace()
{
    const bool faces = std::any_of(m_hl.selected.begin(), m_hl.selected.end(),
                                   [](const SelItem& s) { return s.kind == SelKind::Face; });
    if (faces && !m_session)
        deleteSelection();
    else
        startFeature(FeatureType::DefeatureFace);
}

void Workspace::startAlign(const std::string& mode)
{
    startFeature(FeatureType::TransformBody);
    if (auto* de = dynamic_cast<DirectEditor*>(m_session.get()))
        de->startMate(mode);
    status(tr("Align: click a face of the body to move, then a face of the fixed body"), 0);
}

// ------------------------------------------------------------------ v0.4 lot 0: bodies & faces

void Workspace::deleteSelection()
{
    std::vector<Uuid> bodies;
    std::vector<TopoRef> faces;
    Uuid faceBody;
    for (const SelItem& s : m_hl.selected) {
        if (s.kind == SelKind::Body && std::find(bodies.begin(), bodies.end(), s.owner) == bodies.end())
            bodies.push_back(s.owner);
        if (s.kind == SelKind::Face && (faceBody.empty() || s.owner == faceBody)) {
            faceBody = s.owner;
            faces.push_back(s.ref());
        }
    }
    if (!bodies.empty()) {
        RemoveBodyParams p;
        p.bodies = bodies;
        const std::string name = m_model->doc().nextName("RemoveBody");
        m_model->execute(AddFeatureCommand(makeFeature(FeatureType::RemoveBody, name, Json(p))));
        clearSelection();
        status(tr("%n body(ies) removed by the step %1 — Ctrl+Z to undo", nullptr, int(bodies.size()))
                   .arg(QString::fromStdString(name)));
        return;
    }
    if (!faces.empty()) {
        DefeatureParams p;
        p.faces = faces;
        p.body = faceBody;
        const std::string name = m_model->doc().nextName("DeleteFace");
        m_model->execute(AddFeatureCommand(makeFeature(FeatureType::DefeatureFace, name, Json(p))));
        clearSelection();
        status(tr("%n face(s) deleted and healed (%1) — Ctrl+Z to undo", nullptr, int(faces.size()))
                   .arg(QString::fromStdString(name)));
        return;
    }
    status(tr("Select a body (double-click) or faces to delete"));
}

void Workspace::visibilityUpdated()
{
    // hidden bodies cannot stay selected
    std::erase_if(m_hl.selected, [&](const SelItem& s) { return !s.owner.empty() && m_hl.hiddenBodies.count(s.owner); });
    ++m_hl.version;
    m_vp->highlightChanged();
    emit selectionChanged();
    emit visibilityChanged();
}

void Workspace::setBodyHidden(const Uuid& id, bool hidden)
{
    if (hidden)
        m_hl.hiddenBodies.insert(id);
    else
        m_hl.hiddenBodies.erase(id);
    visibilityUpdated();
}

void Workspace::toggleVisibility()
{
    const auto ids = selectedBodies();
    if (ids.empty()) {
        if (m_hl.hiddenBodies.empty()) {
            status(tr("Select bodies to hide (double-click a body), V again to show them"));
            return;
        }
        m_hl.hiddenBodies.clear();
        m_isolated = false;
        status(tr("Every body is visible again"));
    }
    else {
        for (const Uuid& id : ids)
            if (!m_hl.hiddenBodies.erase(id))
                m_hl.hiddenBodies.insert(id);
        status(tr("%n body(ies) hidden — V with nothing selected shows everything", nullptr, int(ids.size())));
    }
    visibilityUpdated();
}

void Workspace::isolateSelection()
{
    const auto ids = selectedBodies();
    auto snap = m_model->snapshot();
    if (ids.empty() || !snap)
        return;
    if (!m_isolated)
        m_preIsolateHidden = m_hl.hiddenBodies;
    m_hl.hiddenBodies.clear();
    for (const auto& b : snap->bodies)
        if (std::find(ids.begin(), ids.end(), b.id) == ids.end())
            m_hl.hiddenBodies.insert(b.id);
    m_isolated = true;
    status(tr("Isolated — right-click > Unisolate to show the other bodies"));
    visibilityUpdated();
}

void Workspace::unisolate()
{
    if (!m_isolated)
        return;
    m_hl.hiddenBodies = m_preIsolateHidden;
    m_isolated = false;
    visibilityUpdated();
}

void Workspace::startMeasure()
{
    std::vector<SelItem> keep = m_hl.selected;
    if (m_session) {
        if (!m_session->commit())
            return;
        endSession();
    }
    m_hl.selected = keep;
    m_pickingPlane = false;
    m_hl.showOriginPlanes = false;
    beginSession(std::make_unique<MeasureTool>(*this));
}

void Workspace::startInterference()
{
    std::vector<SelItem> keep = m_hl.selected;
    if (m_session) {
        if (!m_session->commit())
            return;
        endSession();
    }
    m_hl.selected = keep;
    m_pickingPlane = false;
    m_hl.showOriginPlanes = false;
    beginSession(std::make_unique<InterferenceTool>(*this));
}

Placement Workspace::sectionBasePlacement(const SectionState& s) const
{
    if (s.plane == "Face")
        return s.face.name.empty() ? originPlacement("XY") : s.facePlane;
    return originPlacement(s.plane);
}

void Workspace::setSection(const SectionState& s)
{
    m_section = s;
    const Placement base = sectionBasePlacement(s);
    Vec3 n = base.normal.normalized();
    const Vec3 q = base.origin + n * s.offset;
    if (s.flip)
        n = -n;
    m_hl.clipOn = s.on;
    m_hl.clipNormal = n;
    m_hl.clipD = n.dot(q);
    touchHighlight();
}

void Workspace::setSectionFace(const SelItem& face)
{
    if (face.kind != SelKind::Face || !face.planar)
        return;
    m_section.face = face.ref();
    // origin of the frame = face centroid (on its plane)
    m_section.facePlane = face.plane;
    m_section.facePlane.origin = face.sig.center;
}

void Workspace::setGhostBodies(bool on)
{
    if (m_hl.ghostBodies == on)
        return;
    m_hl.ghostBodies = on;
    touchHighlight();
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
        status(f->as<ImportParams>().locked ? tr("Library component: move it with Move/Copy or Align (it cannot be modified)")
                                            : tr("Imported geometry has no editable parameters"));
        return;
    }
    if (f->type == FeatureType::TransformBody && f->params.contains("mate") && f->params["mate"].is_object() &&
        f->params["mate"].value("mode", std::string{}) == "bed") {
        status(tr("Lay flat step: to rest the body on another face, delete it and lay flat again"));
        return;
    }
    m_hl.selected.clear();
    if (f->type == FeatureType::FastenerHole)
        beginSession(std::make_unique<HoleEditor>(*this, f));
    else if (f->type == FeatureType::EmbossText)
        beginSession(std::make_unique<TextEditor>(*this, f));
    else if (DirectEditor::handles(f->type))
        beginSession(std::make_unique<DirectEditor>(*this, f->type, f));
    else if (isTransformType(f->type))
        beginSession(std::make_unique<TransformEditor>(*this, f->type, f));
    else if (isSweepLoftType(f->type))
        beginSession(std::make_unique<SweepLoftEditor>(*this, f->type, f));
    else
        beginSession(std::make_unique<FeatureEditor>(*this, f->type, f));
}

bool Workspace::insertComponent(const std::string& id)
{
    if (m_session) {
        if (!m_session->commit())
            return false;
        endSession();
    }
    std::string err;
    auto f = ComponentLibrary::makeFeature(m_model->doc(), id, m_model->pedagogyFrench(), &err);
    if (!f) {
        status(QString::fromStdString(err), 8000);
        return false;
    }
    m_model->execute(AddFeatureCommand(*f));
    m_model->noteEvent("component_inserted");
    status(tr("%1 inserted (locked reference): place it with Move/Copy or Align, project its edges in a sketch")
               .arg(QString::fromStdString((*f)->name)),
           8000);
    return true;
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
    const auto bodyActions = [this] {
        std::vector<MenuAction> a{
            {tr("Move / Copy"), icon(IconId::MoveBody), [this] { startFeature(FeatureType::TransformBody); }},
            {tr("Align"), icon(IconId::Align), [this] { startAlign("face"); }},
            {tr("Split body"), icon(IconId::SplitBody), [this] { startFeature(FeatureType::SplitBody); }},
            {tr("Hide (V)"), icon(IconId::EyeOff), [this] { toggleVisibility(); }},
            {tr("Isolate"), icon(IconId::Eye), [this] { isolateSelection(); }},
            {tr("Delete body (Del)"), icon(IconId::Cancel), [this] { deleteSelection(); }},
        };
        if (m_isolated)
            a.push_back({tr("Unisolate"), icon(IconId::Eye), [this] { unisolate(); }});
        return a;
    };
    const bool wholeBody = std::any_of(m_hl.selected.begin(), m_hl.selected.end(),
                                       [](const SelItem& s) { return s.kind == SelKind::Body; });
    if (const auto bodies = selectedBodies(); bodies.size() == 1 && wholeBody) {
        auto a = bodyActions();
        a.push_back({tr("Measure"), icon(IconId::Measure), [this] { startMeasure(); }});
        a.push_back({tr("Clear selection"), icon(IconId::Cancel), [this] { clearSelection(); }});
        return a;
    }
    if (const auto faceSel = std::count_if(m_hl.selected.begin(), m_hl.selected.end(),
                                           [](const SelItem& s) { return s.kind == SelKind::Face; });
        faceSel > 0 && !wholeBody) {
        return {
            {tr("Press / Pull"), icon(IconId::PressPull), [this] { startPressPull(); }},
            {tr("Hole / Insert (Shift+H)"), icon(IconId::Hole), [this] { startFeature(FeatureType::FastenerHole); }},
            {tr("Text (Emboss / Engrave) (Shift+T)"), icon(IconId::Text), [this] { startFeature(FeatureType::EmbossText); }},
            {tr("Lay flat on bed"), icon(IconId::LayFlat), [this] { startLayFlat(); }},
            {tr("Delete face (Del)"), icon(IconId::DeleteFace), [this] { deleteSelection(); }},
            {tr("Sketch on face"), icon(IconId::Sketch), [this] { startSketch(); }},
            {tr("Shell"), icon(IconId::Shell), [this] { startFeature(FeatureType::Shell); }},
            {tr("Measure"), icon(IconId::Measure), [this] { startMeasure(); }},
            {tr("Clear selection"), icon(IconId::Cancel), [this] { clearSelection(); }},
        };
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
            {tr("Isolate"), icon(IconId::Eye), [this] { isolateSelection(); }},
            {tr("Hide (V)"), icon(IconId::EyeOff), [this] { toggleVisibility(); }},
            {tr("Check interference"), icon(IconId::Interference), [this] { startInterference(); }},
            {tr("Measure"), icon(IconId::Measure), [this] { startMeasure(); }},
            {tr("Clear selection"), icon(IconId::Cancel), [this] { clearSelection(); }},
            {tr("Undo"), icon(IconId::Undo), [this] { undo(); }, m_model->history().canUndo()},
            {tr("Fit all"), icon(IconId::Fit), [this] { m_vp->fitAll(); }},
        };
    }
    return {
        {tr("Sketch"), icon(IconId::Sketch), [this] { startSketch(); }},
        {tr("Extrude"), icon(IconId::Extrude), [this] { startFeature(FeatureType::Extrude); }},
        {tr("Press / Pull"), icon(IconId::PressPull), [this] { startPressPull(); }},
        {tr("Box"), icon(IconId::Box), [this] { startFeature(FeatureType::Box); }},
        {tr("Cylinder"), icon(IconId::Cylinder), [this] { startFeature(FeatureType::Cylinder); }},
        {tr("Revolve"), icon(IconId::Revolve), [this] { startFeature(FeatureType::Revolve); }},
        {tr("Fillet"), icon(IconId::Fillet), [this] { startFeature(FeatureType::Fillet); }},
        {tr("Chamfer"), icon(IconId::Chamfer), [this] { startFeature(FeatureType::Chamfer); }},
        {tr("Shell"), icon(IconId::Shell), [this] { startFeature(FeatureType::Shell); }},
        {tr("Combine"), icon(IconId::Combine), [this] { startFeature(FeatureType::Combine); }},
        {tr("Mirror"), icon(IconId::Mirror), [this] { startFeature(FeatureType::Mirror); }},
        {tr("Linear Pattern"), icon(IconId::LinearPattern), [this] { startFeature(FeatureType::LinearPattern); }},
        {tr("Circular Pattern"), icon(IconId::CircularPattern), [this] { startFeature(FeatureType::CircularPattern); }},
        {tr("Sweep"), icon(IconId::Sweep), [this] { startFeature(FeatureType::Sweep); }},
        {tr("Loft"), icon(IconId::Loft), [this] { startFeature(FeatureType::Loft); }},
        {tr("Measure"), icon(IconId::Measure), [this] { startMeasure(); }},
        {tr("Interference"), icon(IconId::Interference), [this] { startInterference(); }},
        {tr("Section view"), icon(IconId::Section), [this] { emit sectionToggleRequested(); }},
        {tr("Split body"), icon(IconId::SplitBody), [this] { startFeature(FeatureType::SplitBody); }},
        {tr("Move / Copy"), icon(IconId::MoveBody), [this] { startFeature(FeatureType::TransformBody); }},
        {tr("Construction plane"), icon(IconId::Plane), [this] { startFeature(FeatureType::ConstructionPlane); }},
        {tr("Thread"), icon(IconId::Thread), [this] { startFeature(FeatureType::Thread); }},
        {tr("Printability"), icon(IconId::Printability), [this] { startPrintability(); }},
        {tr("Rib"), icon(IconId::Rib), [this] { startFeature(FeatureType::Rib); }},
        {tr("Undo"), icon(IconId::Undo), [this] { undo(); }, m_model->history().canUndo()},
        {tr("Fit all"), icon(IconId::Fit), [this] { m_vp->fitAll(); }},
        {m_isolated ? tr("Unisolate") : tr("Show all bodies"), icon(IconId::Eye), [this] {
             if (m_isolated)
                 unisolate();
             else {
                 m_hl.hiddenBodies.clear();
                 visibilityUpdated();
             }
         }, m_isolated || !m_hl.hiddenBodies.empty()},
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
    if (m_layFlatPick) {
        const SelItem f = m_vp->pickAt(e->position(), SelFilter::Faces | SelFilter::PlanarOnly);
        if (f.valid())
            layFlat(f);
        return;
    }
    const SelItem hit = m_vp->pickAt(e->position(), currentFilter());
    const bool additive = e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);

    if (m_pickingPlane) {
        if (hit.kind != SelKind::Face)
            if (SelItem cp = constructionPlaneAt(e->position()); cp.valid()) {
                startSketchOn(cp);
                return;
            }
        if (hit.valid() && hit.planar)
            startSketchOn(hit);
        return;
    }
    if (m_session) {
        m_session->onPicked(hit, additive);
        return;
    }
    if (!hit.valid()) {  // empty space: start a selection box (selection is applied on release)
        m_boxing = true;
        m_boxFrom = m_boxTo = e->position();
        if (!additive)
            setSelection({});
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
    if (m_boxing) {
        m_boxTo = e->position();
        m_vp->refreshOverlay();
        return;
    }
    if (e->buttons() != Qt::NoButton)
        return;
    setHover(m_vp->pickAt(e->position(), currentFilter()));
}

void Workspace::viewportMouseRelease(QMouseEvent* e)
{
    if (m_session && !m_pickingPlane)
        m_session->mouseRelease(e);
    if (m_boxing && e->button() == Qt::LeftButton) {
        m_boxing = false;
        m_boxTo = e->position();
        if (std::abs(m_boxTo.x() - m_boxFrom.x()) > 4 || std::abs(m_boxTo.y() - m_boxFrom.y()) > 4)
            finishBoxSelect(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier));
        m_vp->refreshOverlay();
    }
}

/// Bodies whose projection is fully inside the box (window, dragged to the right) or touched by it
/// (crossing, dragged to the left).
void Workspace::finishBoxSelect(bool additive)
{
    auto snap = m_model->snapshot();
    if (!snap)
        return;
    const QRectF box = QRectF(m_boxFrom, m_boxTo).normalized();
    const bool crossing = m_boxTo.x() < m_boxFrom.x();
    const QPointF corners[4] = {box.topLeft(), box.topRight(), box.bottomRight(), box.bottomLeft()};
    auto segHits = [&](QPointF a, QPointF b) {
        for (int k = 0; k < 4; ++k)
            if (QLineF(a, b).intersects(QLineF(corners[k], corners[(k + 1) % 4]), nullptr) == QLineF::BoundedIntersection)
                return true;
        return false;
    };
    auto inTri = [](QPointF p, QPointF a, QPointF b, QPointF c) {
        auto cr = [](QPointF o, QPointF u, QPointF v) {
            return (u.x() - o.x()) * (v.y() - o.y()) - (u.y() - o.y()) * (v.x() - o.x());
        };
        const double d1 = cr(a, b, p), d2 = cr(b, c, p), d3 = cr(c, a, p);
        return !((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0));
    };
    std::vector<SelItem> sel = additive ? m_hl.selected : std::vector<SelItem>{};
    int added = 0;
    for (const auto& b : snap->bodies) {
        if (bodyHidden(b.id))
            continue;
        const auto& pos = b.mesh.positions;
        std::vector<QPointF> scr(pos.size() / 3);
        std::vector<char> ok(scr.size());
        bool inside = !scr.empty(), touched = false;
        for (size_t v = 0; v < scr.size(); ++v) {
            ok[v] = m_vp->toScreen({pos[3 * v], pos[3 * v + 1], pos[3 * v + 2]}, &scr[v]);
            const bool in = ok[v] && box.contains(scr[v]);
            inside = inside && in;
            touched = touched || in;
        }
        if (crossing && !touched) {
            const auto& idx = b.mesh.indices;
            for (size_t t = 0; t + 2 < idx.size() && !touched; t += 3) {
                const uint32_t i0 = idx[t], i1 = idx[t + 1], i2 = idx[t + 2];
                if (!ok[i0] || !ok[i1] || !ok[i2])
                    continue;
                touched = inTri(box.center(), scr[i0], scr[i1], scr[i2]) || segHits(scr[i0], scr[i1]) ||
                          segHits(scr[i1], scr[i2]) || segHits(scr[i2], scr[i0]);
            }
        }
        if (!(crossing ? touched : inside))
            continue;
        SelItem it = bodyItem(b.id);
        if (it.valid() && std::none_of(sel.begin(), sel.end(), [&](const SelItem& s) { return s.sameAs(it); })) {
            sel.push_back(it);
            ++added;
        }
    }
    setSelection(std::move(sel));
    status(crossing ? tr("Crossing selection: %1 body(ies) touched by the box").arg(added)
                    : tr("Window selection: %1 body(ies) fully inside the box").arg(added));
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
            if (m_layFlatPick) {
                m_layFlatPick = false;
                m_vp->refreshOverlay();
                return true;
            }
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
        case Qt::Key_H:
            startFeature((e->modifiers() & Qt::ShiftModifier) ? FeatureType::FastenerHole : FeatureType::Shell);
            return true;
        case Qt::Key_T:
            if (!(e->modifiers() & Qt::ShiftModifier))
                return false;
            startFeature(FeatureType::EmbossText);  // v0.7 (in a sketch, Shift+T stays Tangent)
            return true;
        case Qt::Key_I:
            if (e->modifiers() & Qt::ShiftModifier)
                startInterference();
            else
                startMeasure();
            return true;
        case Qt::Key_L:
            if (!(e->modifiers() & Qt::ShiftModifier))
                return false;
            startFeature(FeatureType::Loft);
            return true;
        case Qt::Key_M: startFeature(FeatureType::Mirror); return true;
        case Qt::Key_B:
            startFeature((e->modifiers() & Qt::ShiftModifier) ? FeatureType::Box : FeatureType::Combine);
            return true;
        case Qt::Key_Y:
            if (!(e->modifiers() & Qt::ShiftModifier))
                return false;
            startFeature(FeatureType::Cylinder);
            return true;
        case Qt::Key_Q: startPressPull(); return true;
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            if (m_session || m_pickingPlane)
                return false;
            deleteSelection();
            return true;
        case Qt::Key_V:
            if (m_session || m_pickingPlane)
                return false;
            toggleVisibility();
            return true;
        case Qt::Key_W:
            if (e->modifiers() & Qt::ShiftModifier) {
                startFeature(FeatureType::Sweep);
                return true;
            }
            m_vp->fitAll();
            return true;
        case Qt::Key_Home: m_vp->fitAll(); return true;
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
        if (f->type == FeatureType::Sweep) {
            consumed.insert(f->params.value("profile", Json::object()).value("sketch", std::string{}));
            consumed.insert(f->params.value("path", Json::object()).value("sketch", std::string{}));
        }
        if (f->type == FeatureType::Loft)
            for (const auto& sec : f->params.value("sections", Json::array()))
                consumed.insert(sec.value("sketch", std::string{}));
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
        const Sketch sk = sv.sketch.geos.empty() ? f->as<SketchParams>().sketch : sv.sketch;
        auto toPx = [&](Vec2 q, QPointF* out) { return m_vp->toScreen(sv.plane.toWorld(q), out); };
        for (const SketchGeo& g : sk.geos) {
            QPen pen(g.external() ? Colors::SketchProjected
                     : g.construction ? Colors::SketchConstruction
                                      : Colors::SketchInactive,
                     1.3);
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

SelItem Workspace::constructionPlaneAt(QPointF px) const
{
    SelItem none;
    auto snap = m_model->snapshot();
    if (!snap)
        return none;
    for (const auto& pv : snap->planes) {
        const Vec3 x = pv.plane.xDir.normalized() * pv.size, y = pv.plane.yDir().normalized() * pv.size;
        QPolygonF poly;
        for (const Vec3& c : {pv.plane.origin - x - y, pv.plane.origin + x - y, pv.plane.origin + x + y, pv.plane.origin - x + y}) {
            QPointF q;
            if (!m_vp->toScreen(c, &q))
                return none;
            poly << q;
        }
        if (poly.containsPoint(px, Qt::OddEvenFill)) {
            SelItem s;
            s.kind = SelKind::OriginPlane;
            s.name = "plane:" + pv.id;
            s.owner = pv.id;
            s.planar = true;
            s.plane = pv.plane;
            return s;
        }
    }
    return none;
}

void Workspace::paintPlanes(QPainter& p)
{
    auto snap = m_model->snapshot();
    if (!snap)
        return;
    QFont f = p.font();
    f.setPixelSize(11);
    p.setFont(f);
    for (const auto& pv : snap->planes) {
        const Vec3 x = pv.plane.xDir.normalized() * pv.size, y = pv.plane.yDir().normalized() * pv.size;
        QPolygonF poly;
        bool ok = true;
        for (const Vec3& c : {pv.plane.origin - x - y, pv.plane.origin + x - y, pv.plane.origin + x + y, pv.plane.origin - x + y}) {
            QPointF q;
            ok = ok && m_vp->toScreen(c, &q);
            poly << q;
        }
        if (!ok)
            continue;
        p.setPen(QPen(QColor(0xf0, 0xa0, 0x40, 200), 1.2));
        p.setBrush(QColor(0xf0, 0xa0, 0x40, m_pickingPlane ? 70 : 34));
        p.drawPolygon(poly);
        p.setPen(QColor(0xf0, 0xa0, 0x40));
        p.drawText(poly[3] + QPointF(4, 14), QString::fromStdString(pv.name));
    }
    // v0.4 lot 2: cosmetic threads = dashed rings along the threaded length + label
    for (const auto& tv : snap->threads) {
        const Vec3 ax = tv.b - tv.a;
        const Vec3 d = ax.normalized();
        Vec3 u = (std::abs(d.z) < 0.9 ? Vec3{0, 0, 1} : Vec3{1, 0, 0}).cross(d).normalized();
        const Vec3 v = d.cross(u);
        p.setPen(QPen(QColor(0x9a, 0xd0, 0xff), 1.2, Qt::DashLine));
        const int rings = std::clamp(int(ax.length() / 2), 2, 24);
        for (int k = 0; k <= rings; ++k) {
            const Vec3 c = tv.a + ax * (double(k) / rings);
            QPolygonF ring;
            bool ok = true;
            for (int i = 0; i <= 32; ++i) {
                const double t = 2 * 3.14159265358979 * i / 32;
                QPointF q;
                ok = ok && m_vp->toScreen(c + (u * std::cos(t) + v * std::sin(t)) * tv.radius, &q);
                ring << q;
            }
            if (ok)
                p.drawPolyline(ring);
        }
        QPointF lp;
        if (m_vp->toScreen(tv.b + u * tv.radius, &lp)) {
            p.setPen(QColor(0x9a, 0xd0, 0xff));
            p.drawText(lp + QPointF(6, -4), QString::fromStdString(tv.label));
        }
    }
    p.setBrush(Qt::NoBrush);
}

void Workspace::paintOverlay(QPainter& p)
{
    paintGhost(p);
    paintPlanes(p);
    paintSketches(p);
    if (m_session)
        m_session->paintOverlay(p);
    if (m_boxing)
        paintSelectionBox(p, m_boxFrom, m_boxTo);

    if (m_bed.on && !m_outside.isEmpty()) {
        QFont f = p.font();
        f.setPixelSize(13);
        f.setBold(true);
        p.setFont(f);
        const QString t = QString::fromUtf8("\u26A0 ") +
                          tr("Outside the %1 build volume (%2 × %3 × %4 mm): %5")
                              .arg(m_bed.name)
                              .arg(m_bed.size.x)
                              .arg(m_bed.size.y)
                              .arg(m_bed.size.z)
                              .arg(m_outside.join(", "));
        const QRectF r(0, m_vp->height() - 34, m_vp->width(), 24);
        p.setPen(QColor(0xff, 0x7a, 0x70));
        p.drawText(r, Qt::AlignHCenter | Qt::AlignVCenter, t);
    }
    if (m_layFlatPick) {
        QFont f = p.font();
        f.setPixelSize(14);
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(255, 255, 255, 230));
        p.drawText(QRectF(0, 12, m_vp->width(), 28), Qt::AlignHCenter | Qt::AlignVCenter,
                   tr("Click the face that must rest on the build plate"));
    }
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
