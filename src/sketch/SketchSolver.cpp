#include "sketch/SketchSolver.h"

#include <GCS.h>

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace silex {
namespace {

struct Slot
{
    GeoKind kind = GeoKind::Point;
    GCS::Point pt;
    GCS::Line line;
    GCS::Circle circle;
    GCS::Arc arc;
    std::vector<double*> params;
};

class SystemBuilder
{
public:
    explicit SystemBuilder(const Sketch& sk) : m_sketch(sk)
    {
        // Implicit, fixed origin and axes (parameters NOT declared as unknowns).
        Slot o;
        o.kind = GeoKind::Point;
        o.pt = GCS::Point(constant(0), constant(0));
        m_slots[kOriginId] = o;
        Slot x;
        x.kind = GeoKind::Line;
        x.line.p1 = GCS::Point(constant(0), constant(0));
        x.line.p2 = GCS::Point(constant(1), constant(0));
        m_slots[kXAxisId] = x;
        Slot y;
        y.kind = GeoKind::Line;
        y.line.p1 = GCS::Point(constant(0), constant(0));
        y.line.p2 = GCS::Point(constant(0), constant(1));
        m_slots[kYAxisId] = y;

        for (const SketchGeo& g : sk.geos)
            addGeo(g);
        for (const SketchConstraint& c : sk.constraints)
            if (c.kind == ConstraintKind::Coincident && c.a.pos != PointPos::None && c.b.pos != PointPos::None) {
                m_coincident.emplace(c.a, c.b);
                m_coincident.emplace(c.b, c.a);
            }
    }

    /// End points of a and b joined by a Coincident constraint (tangency at a joint).
    std::optional<std::pair<GeoRef, GeoRef>> sharedEnd(const Uuid& a, const Uuid& b) const
    {
        for (PointPos pa : {PointPos::Start, PointPos::End})
            for (PointPos pb : {PointPos::Start, PointPos::End})
                if (m_coincident.count({GeoRef{a, pa}, GeoRef{b, pb}}))
                    return std::pair{GeoRef{a, pa}, GeoRef{b, pb}};
        return std::nullopt;
    }

    static GCS::Curve* curveOf(Slot* s)
    {
        switch (s->kind) {
            case GeoKind::Line: return &s->line;
            case GeoKind::Circle: return &s->circle;
            case GeoKind::Arc: return &s->arc;
            default: return nullptr;
        }
    }

    double* constant(double v)
    {
        m_store.push_back(v);
        return &m_store.back();
    }
    double* unknown(double v, const Uuid& owner)
    {
        double* p = constant(v);
        if (m_fixed)
            return p;
        m_unknowns.push_back(p);
        m_owner[p] = owner;
        return p;
    }

    void addGeo(const SketchGeo& g)
    {
        // projected model edges are fixed: their parameters are constants, not unknowns
        m_fixed = g.external();
        if (m_fixed)
            m_external.insert(g.id);
        Slot s;
        s.kind = g.kind;
        switch (g.kind) {
            case GeoKind::Point: s.pt = GCS::Point(unknown(g.p1.x, g.id), unknown(g.p1.y, g.id)); break;
            case GeoKind::Line:
                s.line.p1 = GCS::Point(unknown(g.p1.x, g.id), unknown(g.p1.y, g.id));
                s.line.p2 = GCS::Point(unknown(g.p2.x, g.id), unknown(g.p2.y, g.id));
                break;
            case GeoKind::Circle:
                s.circle.center = GCS::Point(unknown(g.c.x, g.id), unknown(g.c.y, g.id));
                s.circle.rad = unknown(g.r, g.id);
                break;
            case GeoKind::Arc: {
                const Vec2 ps = g.point(PointPos::Start), pe = g.point(PointPos::End);
                s.arc.center = GCS::Point(unknown(g.c.x, g.id), unknown(g.c.y, g.id));
                s.arc.rad = unknown(g.r, g.id);
                s.arc.startAngle = unknown(g.a0, g.id);
                s.arc.endAngle = unknown(g.a1, g.id);
                s.arc.start = GCS::Point(unknown(ps.x, g.id), unknown(ps.y, g.id));
                s.arc.end = GCS::Point(unknown(pe.x, g.id), unknown(pe.y, g.id));
                break;
            }
        }
        m_slots[g.id] = s;
        m_order.push_back(g.id);
    }

    Slot* slot(const Uuid& id)
    {
        auto it = m_slots.find(id);
        return it == m_slots.end() ? nullptr : &it->second;
    }

    GCS::Point* point(const GeoRef& r)
    {
        Slot* s = slot(r.geo);
        if (!s)
            return nullptr;
        switch (s->kind) {
            case GeoKind::Point: return &s->pt;
            case GeoKind::Line: return r.pos == PointPos::End ? &s->line.p2 : &s->line.p1;
            case GeoKind::Circle: return &s->circle.center;
            case GeoKind::Arc:
                if (r.pos == PointPos::Start) return &s->arc.start;
                if (r.pos == PointPos::End) return &s->arc.end;
                return &s->arc.center;
        }
        return nullptr;
    }

    void addArcRules(GCS::System& sys)
    {
        for (auto& [id, s] : m_slots)
            if (s.kind == GeoKind::Arc && !m_external.count(id))
                sys.addConstraintArcRules(s.arc, 0);  // tag 0: internal, high priority
    }

    /// Returns false if the constraint is not applicable to the referenced geometry.
    bool addConstraint(GCS::System& sys, const SketchConstraint& c, int tag)
    {
        Slot* a = slot(c.a.geo);
        Slot* b = c.b.valid() ? slot(c.b.geo) : nullptr;
        if (!a)
            return false;
        const bool aPoint = c.a.pos != PointPos::None || a->kind == GeoKind::Point;
        const bool bPoint = b && (c.b.pos != PointPos::None || b->kind == GeoKind::Point);

        switch (c.kind) {
            case ConstraintKind::Coincident: {
                GCS::Point *pa = point(c.a), *pb = b ? point(c.b) : nullptr;
                if (!pa || !pb)
                    return false;
                sys.addConstraintP2PCoincident(*pa, *pb, tag);
                return true;
            }
            case ConstraintKind::Horizontal:
            case ConstraintKind::Vertical: {
                const bool h = c.kind == ConstraintKind::Horizontal;
                if (b && aPoint && bPoint) {
                    GCS::Point *pa = point(c.a), *pb = point(c.b);
                    h ? sys.addConstraintHorizontal(*pa, *pb, tag) : sys.addConstraintVertical(*pa, *pb, tag);
                    return true;
                }
                if (a->kind != GeoKind::Line)
                    return false;
                h ? sys.addConstraintHorizontal(a->line, tag) : sys.addConstraintVertical(a->line, tag);
                return true;
            }
            case ConstraintKind::Parallel:
            case ConstraintKind::Perpendicular:
                if (!b || a->kind != GeoKind::Line || b->kind != GeoKind::Line)
                    return false;
                if (c.kind == ConstraintKind::Parallel)
                    sys.addConstraintParallel(a->line, b->line, tag);
                else
                    sys.addConstraintPerpendicular(a->line, b->line, tag);
                return true;
            case ConstraintKind::Tangent: {
                if (!b)
                    return false;
                // Tangency at a joint (end points made coincident): "angle via point" as in FreeCAD.
                // The distance-based tangency is singular there (redundant with the coincidence).
                if (auto sh = sharedEnd(c.a.geo, c.b.geo)) {
                    GCS::Curve *ca = curveOf(a), *cb = curveOf(b);
                    GCS::Point *p1 = point(sh->first), *p2 = point(sh->second);
                    if (ca && cb && p1 && p2) {
                        const double ang = sys.calculateAngleViaPoint(*ca, *cb, *p1, *p2);
                        const double want = std::cos(ang) >= 0 ? 0.0 : (ang > 0 ? kPi : -kPi);
                        sys.addConstraintAngleViaPoint(*ca, *cb, *p1, constant(want), tag);
                        return true;
                    }
                }
                Slot *x = a, *y = b;
                if (y->kind == GeoKind::Line)
                    std::swap(x, y);
                if (x->kind == GeoKind::Line) {
                    if (y->kind == GeoKind::Circle) sys.addConstraintTangent(x->line, y->circle, tag);
                    else if (y->kind == GeoKind::Arc) sys.addConstraintTangent(x->line, y->arc, tag);
                    else return false;
                    return true;
                }
                if (x->kind == GeoKind::Arc && y->kind == GeoKind::Circle)
                    std::swap(x, y);
                if (x->kind == GeoKind::Circle && y->kind == GeoKind::Circle)
                    sys.addConstraintTangent(x->circle, y->circle, tag);
                else if (x->kind == GeoKind::Circle && y->kind == GeoKind::Arc)
                    sys.addConstraintTangent(x->circle, y->arc, tag);
                else if (x->kind == GeoKind::Arc && y->kind == GeoKind::Arc)
                    sys.addConstraintTangent(x->arc, y->arc, tag);
                else
                    return false;
                return true;
            }
            case ConstraintKind::Equal: {
                if (!b)
                    return false;
                if (a->kind == GeoKind::Line && b->kind == GeoKind::Line) {
                    sys.addConstraintEqualLength(a->line, b->line, tag);
                    return true;
                }
                Slot *x = a, *y = b;
                if (x->kind == GeoKind::Arc && y->kind == GeoKind::Circle)
                    std::swap(x, y);
                if (x->kind == GeoKind::Circle && y->kind == GeoKind::Circle)
                    sys.addConstraintEqualRadius(x->circle, y->circle, tag);
                else if (x->kind == GeoKind::Circle && y->kind == GeoKind::Arc)
                    sys.addConstraintEqualRadius(x->circle, y->arc, tag);
                else if (x->kind == GeoKind::Arc && y->kind == GeoKind::Arc)
                    sys.addConstraintEqualRadius(x->arc, y->arc, tag);
                else
                    return false;
                return true;
            }
            case ConstraintKind::Fixed: {
                if (aPoint) {
                    GCS::Point* p = point(c.a);
                    sys.addConstraintCoordinateX(*p, constant(c.value), tag);
                    sys.addConstraintCoordinateY(*p, constant(c.value2), tag);
                    return true;
                }
                // Whole curve: lock every parameter at its current value.
                for (double* p : paramsOf(*a))
                    sys.addConstraintEqual(p, constant(*p), tag);
                return true;
            }
            case ConstraintKind::PointOnObject: {
                if (!b || !aPoint)
                    return false;
                GCS::Point* p = point(c.a);
                if (b->kind == GeoKind::Line) sys.addConstraintPointOnLine(*p, b->line, tag);
                else if (b->kind == GeoKind::Circle) sys.addConstraintPointOnCircle(*p, b->circle, tag);
                else if (b->kind == GeoKind::Arc) sys.addConstraintPointOnArc(*p, b->arc, tag);
                else return false;
                return true;
            }
            case ConstraintKind::Distance: {
                double* v = constant(c.value);
                if (!b) {
                    if (a->kind != GeoKind::Line)
                        return false;
                    sys.addConstraintP2PDistance(a->line.p1, a->line.p2, v, tag);
                    return true;
                }
                if (aPoint && bPoint) {
                    sys.addConstraintP2PDistance(*point(c.a), *point(c.b), v, tag);
                    return true;
                }
                if (aPoint && b->kind == GeoKind::Line) {
                    sys.addConstraintP2LDistance(*point(c.a), b->line, v, tag);
                    return true;
                }
                return false;
            }
            case ConstraintKind::DistanceX:
            case ConstraintKind::DistanceY: {
                const bool isX = c.kind == ConstraintKind::DistanceX;
                GCS::Point *pa = nullptr, *pb = nullptr;
                if (!b && a->kind == GeoKind::Line) {
                    pa = &a->line.p1;
                    pb = &a->line.p2;
                }
                else if (b) {
                    pa = point(c.a);
                    pb = point(c.b);
                }
                if (!pa || !pb)
                    return false;
                sys.addConstraintDifference(isX ? pa->x : pa->y, isX ? pb->x : pb->y, constant(c.value), tag);
                return true;
            }
            case ConstraintKind::Radius:
            case ConstraintKind::Diameter: {
                double* v = constant(c.value);
                const bool diam = c.kind == ConstraintKind::Diameter;
                if (a->kind == GeoKind::Circle)
                    diam ? sys.addConstraintCircleDiameter(a->circle, v, tag)
                         : sys.addConstraintCircleRadius(a->circle, v, tag);
                else if (a->kind == GeoKind::Arc)
                    diam ? sys.addConstraintArcDiameter(a->arc, v, tag) : sys.addConstraintArcRadius(a->arc, v, tag);
                else
                    return false;
                return true;
            }
            case ConstraintKind::Angle: {
                double* v = constant(c.value);
                if (a->kind != GeoKind::Line)
                    return false;
                if (!b) {
                    sys.addConstraintL2LAngle(slot(kXAxisId)->line, a->line, v, tag);
                    return true;
                }
                if (b->kind != GeoKind::Line)
                    return false;
                sys.addConstraintL2LAngle(a->line, b->line, v, tag);
                return true;
            }
            // ---------------------------------------------------------- Sketcher Pro
            case ConstraintKind::Midpoint: {
                if (!b)
                    return false;
                GeoRef pr = c.a, cr = c.b;  // point, curve (accepted in any order)
                Slot* curve = b;
                if (!aPoint && bPoint) {
                    std::swap(pr, cr);
                    curve = a;
                }
                else if (!aPoint || bPoint)
                    return false;
                GCS::Point* p = point(pr);
                if (!p)
                    return false;
                if (curve->kind == GeoKind::Line) {
                    sys.addConstraintP2PSymmetric(curve->line.p1, curve->line.p2, *p, tag);
                    return true;
                }
                if (curve->kind == GeoKind::Arc) {  // curvilinear middle: on the arc and on the chord bisector
                    sys.addConstraintPointOnArc(*p, curve->arc, tag);
                    sys.addConstraintPointOnPerpBisector(*p, curve->arc.start, curve->arc.end, tag);
                    return true;
                }
                return false;
            }
            case ConstraintKind::Symmetric: {
                Slot* ax = c.c.valid() ? slot(c.c.geo) : nullptr;
                if (!b || !ax)
                    return false;
                const bool axPoint = c.c.pos != PointPos::None || ax->kind == GeoKind::Point;
                if (!axPoint && ax->kind != GeoKind::Line)
                    return false;
                auto sym = [&](GCS::Point& p1, GCS::Point& p2) {
                    if (axPoint)
                        sys.addConstraintP2PSymmetric(p1, p2, *point(c.c), tag);
                    else
                        sys.addConstraintP2PSymmetric(p1, p2, ax->line, tag);
                };
                if (aPoint && bPoint) {
                    sym(*point(c.a), *point(c.b));
                    return true;
                }
                if (aPoint || bPoint || a->kind != b->kind)
                    return false;
                switch (a->kind) {
                    case GeoKind::Line:
                        if (c.value != 0) {
                            sym(a->line.p1, b->line.p2);
                            sym(a->line.p2, b->line.p1);
                        }
                        else {
                            sym(a->line.p1, b->line.p1);
                            sym(a->line.p2, b->line.p2);
                        }
                        return true;
                    case GeoKind::Circle:
                        sym(a->circle.center, b->circle.center);
                        sys.addConstraintEqualRadius(a->circle, b->circle, tag);
                        return true;
                    case GeoKind::Arc:  // a mirrored arc runs the other way round
                        sym(a->arc.start, b->arc.end);
                        sym(a->arc.end, b->arc.start);
                        sys.addConstraintEqualRadius(a->arc, b->arc, tag);
                        return true;
                    default: return false;
                }
            }
            case ConstraintKind::Concentric: {
                if (!b)
                    return false;
                auto center = [](Slot* s) -> GCS::Point* {
                    if (s->kind == GeoKind::Circle) return &s->circle.center;
                    if (s->kind == GeoKind::Arc) return &s->arc.center;
                    return nullptr;
                };
                GCS::Point *ca = center(a), *cb = center(b);
                if (!ca || !cb)
                    return false;
                sys.addConstraintP2PCoincident(*ca, *cb, tag);
                return true;
            }
            case ConstraintKind::Collinear:
                if (!b || a->kind != GeoKind::Line || b->kind != GeoKind::Line)
                    return false;
                sys.addConstraintPointOnLine(b->line.p1, a->line, tag);
                sys.addConstraintPointOnLine(b->line.p2, a->line, tag);
                return true;
        }
        return false;
    }

    void addDrag(GCS::System& sys, const DragTarget& d)
    {
        const int tag = GCS::DefaultTemporaryConstraint;
        if (isExternal(d.point.geo))
            return;  // projected edges cannot be dragged
        Slot* s = slot(d.point.geo);
        if (!s)
            return;
        if (d.point.pos != PointPos::None || s->kind == GeoKind::Point) {
            GCS::Point* p = point(d.point);
            sys.addConstraintCoordinateX(*p, constant(d.to.x), tag);
            sys.addConstraintCoordinateY(*p, constant(d.to.y), tag);
            return;
        }
        // Whole-curve drag: translate it rigidly towards the cursor.
        const SketchGeo* g = m_sketch.find(d.point.geo);
        if (!g)
            return;
        const Vec2 ref = g->kind == GeoKind::Line ? g->p1 : g->c;
        const Vec2 delta = (d.to - d.grabOffset) - ref;
        auto pin = [&](GCS::Point& p) {
            sys.addConstraintCoordinateX(p, constant(*p.x + delta.x), tag);
            sys.addConstraintCoordinateY(p, constant(*p.y + delta.y), tag);
        };
        if (s->kind == GeoKind::Line) {
            pin(s->line.p1);
            pin(s->line.p2);
        }
        else if (s->kind == GeoKind::Circle) {
            pin(s->circle.center);
        }
        else if (s->kind == GeoKind::Arc) {
            pin(s->arc.center);
        }
    }

    std::vector<double*> paramsOf(const Slot& s)
    {
        switch (s.kind) {
            case GeoKind::Point: return {s.pt.x, s.pt.y};
            case GeoKind::Line: return {s.line.p1.x, s.line.p1.y, s.line.p2.x, s.line.p2.y};
            case GeoKind::Circle: return {s.circle.center.x, s.circle.center.y, s.circle.rad};
            case GeoKind::Arc:
                return {s.arc.center.x, s.arc.center.y, s.arc.rad, s.arc.startAngle, s.arc.endAngle};
        }
        return {};
    }

    void writeBack(Sketch& sk)
    {
        for (SketchGeo& g : sk.geos) {
            Slot* s = slot(g.id);
            if (!s)
                continue;
            switch (g.kind) {
                case GeoKind::Point: g.p1 = {*s->pt.x, *s->pt.y}; break;
                case GeoKind::Line:
                    g.p1 = {*s->line.p1.x, *s->line.p1.y};
                    g.p2 = {*s->line.p2.x, *s->line.p2.y};
                    break;
                case GeoKind::Circle:
                    g.c = {*s->circle.center.x, *s->circle.center.y};
                    g.r = std::abs(*s->circle.rad);
                    break;
                case GeoKind::Arc: {
                    g.c = {*s->arc.center.x, *s->arc.center.y};
                    g.r = std::abs(*s->arc.rad);
                    double a0 = *s->arc.startAngle, a1 = *s->arc.endAngle;
                    while (a1 <= a0) a1 += 2 * kPi;
                    while (a1 - a0 > 2 * kPi) a1 -= 2 * kPi;
                    // keep angles in a canonical range for readability of the file
                    const double shift = std::floor(a0 / (2 * kPi)) * 2 * kPi;
                    g.a0 = a0 - shift;
                    g.a1 = a1 - shift;
                    break;
                }
            }
        }
    }

    GCS::VEC_pD& unknowns() { return m_unknowns; }
    const std::unordered_map<double*, Uuid>& owners() const { return m_owner; }
    const std::vector<Uuid>& order() const { return m_order; }

private:
    const Sketch& m_sketch;
    std::deque<double> m_store;  // stable addresses
    GCS::VEC_pD m_unknowns;
    std::unordered_map<double*, Uuid> m_owner;
    std::map<Uuid, Slot> m_slots;
    std::vector<Uuid> m_order;
    bool m_fixed = false;
    std::set<Uuid> m_external;
    struct RefLess
    {
        bool operator()(const std::pair<GeoRef, GeoRef>& x, const std::pair<GeoRef, GeoRef>& y) const
        {
            auto key = [](const std::pair<GeoRef, GeoRef>& p) {
                return std::tie(p.first.geo, p.first.pos, p.second.geo, p.second.pos);
            };
            return key(x) < key(y);
        }
    };
    std::set<std::pair<GeoRef, GeoRef>, RefLess> m_coincident;

public:
    bool isExternal(const Uuid& id) const { return m_external.count(id) > 0; }
};

}  // namespace

SolveResult SketchSolver::solve(Sketch& sketch, const std::optional<DragTarget>& drag, bool diagnose)
{
    SolveResult res;
    try {
        SystemBuilder b(sketch);
        GCS::System sys;
        b.addArcRules(sys);
        for (size_t i = 0; i < sketch.constraints.size(); ++i)
            b.addConstraint(sys, sketch.constraints[i], static_cast<int>(i) + 1);
        if (drag)
            b.addDrag(sys, *drag);

        sys.declareUnknowns(b.unknowns());
        sys.initSolution(GCS::DogLeg);  // also runs the QR-based diagnosis (tags >= 0)

        int status = sys.solve(true, GCS::DogLeg);
        if (status != GCS::Success) {
            status = sys.solve(true, GCS::LevenbergMarquardt);
            if (status != GCS::Success)
                status = sys.solve(true, GCS::BFGS);
        }
        res.converged = status == GCS::Success;
        if (res.converged || (drag && status == GCS::Converged)) {
            sys.applySolution();
            b.writeBack(sketch);
            res.converged = true;
        }
        else {
            sys.undoSolution();
            res.message = "The sketch solver did not converge";
        }

        if (diagnose && !drag) {
            res.dof = sys.dofsNumber();
            GCS::VEC_I conflicting, redundant;
            sys.getConflicting(conflicting);
            sys.getRedundant(redundant);
            auto toIds = [&](const GCS::VEC_I& tags, std::set<Uuid>& out) {
                for (int t : tags)
                    if (t >= 1 && t <= static_cast<int>(sketch.constraints.size()))
                        out.insert(sketch.constraints[static_cast<size_t>(t - 1)].id);
            };
            toIds(conflicting, res.conflicting);
            toIds(redundant, res.redundant);

            GCS::VEC_pD dependent;
            sys.getDependentParams(dependent);
            std::set<Uuid> free;
            for (double* p : dependent) {
                auto it = b.owners().find(p);
                if (it != b.owners().end())
                    free.insert(it->second);
            }
            // An empty diagnosis matrix means "no constraint at all": everything is free.
            if (!sys.isEmptyDiagnoseMatrix() || b.unknowns().empty())
                for (const Uuid& id : b.order())
                    if (!free.count(id))
                        res.fullyConstrainedGeos.insert(id);
            for (const Uuid& id : b.order())
                if (b.isExternal(id))
                    res.fullyConstrainedGeos.insert(id);
            if (sys.isEmptyDiagnoseMatrix())
                res.dof = static_cast<int>(b.unknowns().size()) -
                          4 * static_cast<int>(std::count_if(sketch.geos.begin(), sketch.geos.end(), [](const SketchGeo& g) {
                              return g.kind == GeoKind::Arc && !g.external();
                          }));
        }

        if (!res.converged)
            res.state = SketchDofState::Failed;
        else if (!res.conflicting.empty() || !res.redundant.empty())
            res.state = SketchDofState::OverConstrained;
        else if (res.dof == 0)
            res.state = SketchDofState::FullyConstrained;
        else
            res.state = SketchDofState::UnderConstrained;
    }
    catch (const std::exception& e) {
        res.converged = false;
        res.state = SketchDofState::Failed;
        res.message = e.what();
    }
    return res;
}

}  // namespace silex
