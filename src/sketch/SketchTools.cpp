#include "sketch/SketchTools.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>

#include "sketch/SketchSolver.h"

namespace silex::sketchtools {

namespace {

constexpr double kTol = 1e-6;  // mm

double norm2pi(double a)
{
    a = std::fmod(a, 2 * kPi);
    if (a < 0)
        a += 2 * kPi;
    return a;
}

Vec2 polar(double a) { return {std::cos(a), std::sin(a)}; }
double angleOf(Vec2 v) { return std::atan2(v.y, v.x); }

SketchGeo mkLine(Vec2 a, Vec2 b, bool construction)
{
    SketchGeo g;
    g.id = newUuid();
    g.kind = GeoKind::Line;
    g.p1 = a;
    g.p2 = b;
    g.construction = construction;
    return g;
}

SketchGeo mkArc(Vec2 c, double r, double a0, double a1, bool construction)
{
    SketchGeo g;
    g.id = newUuid();
    g.kind = GeoKind::Arc;
    g.c = c;
    g.r = r;
    g.a0 = a0;
    g.a1 = a1;
    while (g.a1 <= g.a0)
        g.a1 += 2 * kPi;
    g.construction = construction;
    return g;
}

SketchGeo mkCircle(Vec2 c, double r, bool construction)
{
    SketchGeo g;
    g.id = newUuid();
    g.kind = GeoKind::Circle;
    g.c = c;
    g.r = r;
    g.construction = construction;
    return g;
}

SketchGeo mkPoint(Vec2 p)
{
    SketchGeo g;
    g.id = newUuid();
    g.kind = GeoKind::Point;
    g.p1 = p;
    g.construction = true;
    return g;
}

SketchConstraint mkC(ConstraintKind k, GeoRef a, GeoRef b = {}, double v = 0)
{
    SketchConstraint c;
    c.id = newUuid();
    c.kind = k;
    c.a = a;
    c.b = b;
    c.value = v;
    return c;
}

struct Builder
{
    Sketch& sk;
    ToolResult& res;
    Uuid geo(const SketchGeo& g)
    {
        sk.geos.push_back(g);
        res.geos.push_back(g.id);
        return g.id;
    }
    Uuid con(SketchConstraint c)
    {
        res.constraints.push_back(c.id);
        sk.constraints.push_back(c);
        return res.constraints.back();
    }
    Uuid con(ConstraintKind k, GeoRef a, GeoRef b = {}, double v = 0) { return con(mkC(k, a, b, v)); }
};

ToolResult fail(std::string why)
{
    ToolResult r;
    r.ok = false;
    r.error = std::move(why);
    return r;
}

// ---------------------------------------------------------------- intersections

/// Intersection points of the *infinite* supports (line / full circle) of two curves.
std::vector<Vec2> supportIntersections(const SketchGeo& a, const SketchGeo& b)
{
    std::vector<Vec2> out;
    const bool la = a.kind == GeoKind::Line, lb = b.kind == GeoKind::Line;
    if (la && lb) {
        const Vec2 d1 = a.p2 - a.p1, d2 = b.p2 - b.p1;
        const double den = d1.cross(d2);
        if (std::abs(den) < 1e-12 * std::max(1.0, d1.length() * d2.length()))
            return out;  // parallel (collinear overlaps are not intersections)
        const double t = (b.p1 - a.p1).cross(d2) / den;
        out.push_back(a.p1 + d1 * t);
        return out;
    }
    if (la || lb) {
        const SketchGeo& l = la ? a : b;
        const SketchGeo& c = la ? b : a;
        const Vec2 d = (l.p2 - l.p1).normalized();
        const Vec2 foot = l.p1 + d * (c.c - l.p1).dot(d);
        const double h = (c.c - foot).length();
        if (h > c.r + kTol)
            return out;
        const double s = std::sqrt(std::max(0.0, c.r * c.r - h * h));
        out.push_back(foot - d * s);
        if (s > kTol)
            out.push_back(foot + d * s);
        return out;
    }
    // circle / circle
    const Vec2 dc = b.c - a.c;
    const double D = dc.length();
    if (D < 1e-12 || D > a.r + b.r + kTol || D < std::abs(a.r - b.r) - kTol)
        return out;
    const double x = (D * D + a.r * a.r - b.r * b.r) / (2 * D);
    const double h = std::sqrt(std::max(0.0, a.r * a.r - x * x));
    const Vec2 u = dc * (1.0 / D), n{-u.y, u.x};
    out.push_back(a.c + u * x + n * h);
    if (h > kTol)
        out.push_back(a.c + u * x - n * h);
    return out;
}

/// Parameter of p on the curve: line t in [0, 1] (unbounded), arc angle offset from a0 in [0, 2pi),
/// circle angle in [0, 2pi).
double paramOf(const SketchGeo& g, Vec2 p)
{
    switch (g.kind) {
        case GeoKind::Line: {
            const Vec2 d = g.p2 - g.p1;
            const double l2 = d.dot(d);
            return l2 > 1e-24 ? (p - g.p1).dot(d) / l2 : 0.0;
        }
        case GeoKind::Arc: return norm2pi(angleOf(p - g.c) - g.a0);
        case GeoKind::Circle: return norm2pi(angleOf(p - g.c));
        default: return 0;
    }
}

Vec2 pointAt(const SketchGeo& g, double t)
{
    switch (g.kind) {
        case GeoKind::Line: return g.p1 + (g.p2 - g.p1) * t;
        case GeoKind::Arc: return g.c + polar(g.a0 + t) * g.r;
        case GeoKind::Circle: return g.c + polar(t) * g.r;
        default: return g.p1;
    }
}

/// Is p (known to be on the support) inside the bounded curve, within tolerance?
bool onBounded(const SketchGeo& g, Vec2 p)
{
    switch (g.kind) {
        case GeoKind::Line: {
            const double L = (g.p2 - g.p1).length();
            const double t = paramOf(g, p);
            return L > 0 && t >= -kTol / L && t <= 1 + kTol / L;
        }
        case GeoKind::Arc: {
            const double span = g.a1 - g.a0;
            const double u = paramOf(g, p);
            const double tolA = kTol / std::max(g.r, 1e-9);
            return u <= span + tolA || u >= 2 * kPi - tolA;
        }
        case GeoKind::Circle: return true;
        default: return false;
    }
}

struct Isect
{
    double t;  ///< parameter on the target curve
    Vec2 p;
    Uuid other;
};

/// Intersections of the target support with every other (bounded) curve of the sketch.
std::vector<Isect> isectsOnSupport(const Sketch& sk, const SketchGeo& target)
{
    std::vector<Isect> out;
    for (const SketchGeo& o : sk.geos) {
        if (o.id == target.id || o.kind == GeoKind::Point)
            continue;
        for (Vec2 p : supportIntersections(target, o))
            if (onBounded(o, p))
                out.push_back({paramOf(target, p), p, o.id});
    }
    return out;
}

// ---------------------------------------------------------------- constraint bookkeeping

/// Removes every constraint attached to the given point; returns how many.
int dropPointConstraints(Sketch& sk, const GeoRef& p)
{
    const size_t n = sk.constraints.size();
    sk.constraints.erase(std::remove_if(sk.constraints.begin(), sk.constraints.end(),
                                        [&](const SketchConstraint& c) { return c.referencesPoint(p); }),
                         sk.constraints.end());
    return int(n - sk.constraints.size());
}

/// Removes constraints that depend on the extent of the curve (length, middle, symmetric ends).
int dropShapeConstraints(Sketch& sk, const Uuid& id, bool lengthChanges)
{
    const SketchGeo* g = sk.find(id);
    const bool line = g && g->kind == GeoKind::Line;
    const size_t n = sk.constraints.size();
    sk.constraints.erase(
        std::remove_if(sk.constraints.begin(), sk.constraints.end(),
                       [&](const SketchConstraint& c) {
                           if (!c.references(id))
                               return false;
                           switch (c.kind) {
                               case ConstraintKind::Midpoint:  // the middle of this curve moves
                                   return (c.a.geo == id && c.a.pos == PointPos::None) ||
                                          (c.b.geo == id && c.b.pos == PointPos::None);
                               case ConstraintKind::Symmetric:
                                   return (c.a.geo == id && c.a.pos == PointPos::None) ||
                                          (c.b.geo == id && c.b.pos == PointPos::None);
                               case ConstraintKind::Distance:
                               case ConstraintKind::DistanceX:
                               case ConstraintKind::DistanceY:
                                   return lengthChanges && line && c.a.geo == id && c.a.pos == PointPos::None && !c.b.valid();
                               case ConstraintKind::Equal: return lengthChanges && line;
                               default: return false;
                           }
                       }),
        sk.constraints.end());
    return int(n - sk.constraints.size());
}

/// Curves linked to `id` through Collinear / Concentric constraints (pieces of the same curve).
std::set<Uuid> family(const Sketch& sk, const Uuid& id)
{
    std::set<Uuid> fam{id};
    bool grew = true;
    while (grew) {
        grew = false;
        for (const auto& c : sk.constraints)
            if (c.kind == ConstraintKind::Collinear || c.kind == ConstraintKind::Concentric) {
                const bool ia = fam.count(c.a.geo) > 0, ib = fam.count(c.b.geo) > 0;
                if (ia != ib) {
                    fam.insert(ia ? c.b.geo : c.a.geo);
                    grew = true;
                }
            }
    }
    return fam;
}

std::vector<PointPos> endsOf(const SketchGeo& g)
{
    switch (g.kind) {
        case GeoKind::Line:
        case GeoKind::Arc: return {PointPos::Start, PointPos::End};
        case GeoKind::Point: return {PointPos::Start};
        default: return {};
    }
}

/// Constraint attaching the new end `pt` (at p) of a cut curve to the cutting curve `other`.
std::optional<SketchConstraint> attach(Sketch& sk, const GeoRef& pt, Vec2 p, const Uuid& other)
{
    if (other.empty())
        return std::nullopt;
    const SketchGeo* o = sk.find(other);
    if (!o)
        return std::nullopt;
    for (PointPos pos : endsOf(*o))
        if ((o->point(pos) - p).length() < 1e-5) {
            // the end of `other` lies here: it becomes a true coincidence (drop the now implied
            // point-on-object constraints onto the pieces of the cut curve)
            const GeoRef oe{other, pos};
            const auto fam = family(sk, pt.geo);
            sk.constraints.erase(std::remove_if(sk.constraints.begin(), sk.constraints.end(),
                                                [&](const SketchConstraint& c) {
                                                    return c.kind == ConstraintKind::PointOnObject && c.a == oe &&
                                                           fam.count(c.b.geo);
                                                }),
                                 sk.constraints.end());
            return mkC(ConstraintKind::Coincident, pt, oe);
        }
    return mkC(ConstraintKind::PointOnObject, pt, {other, PointPos::None});
}

void retargetPoint(Sketch& sk, const GeoRef& from, const GeoRef& to)
{
    for (auto& c : sk.constraints)
        for (GeoRef* r : {&c.a, &c.b, &c.c})
            if (*r == from)
                *r = to;
}

}  // namespace

// ================================================================== helpers (public)

Vec2 reflect(Vec2 p, Vec2 a, Vec2 dir)
{
    const Vec2 u = dir.normalized();
    const Vec2 v = p - a;
    return a + u * (2 * u.dot(v)) - v;
}

std::vector<Uuid> addIfIndependent(Sketch& sk, const std::vector<SketchConstraint>& candidates)
{
    std::vector<Uuid> added;
    if (candidates.empty())
        return added;
    auto badness = [](const SolveResult& r) {
        return r.converged ? r.conflicting.size() + r.redundant.size() : std::numeric_limits<size_t>::max();
    };
    Sketch probe = sk;
    size_t base = badness(SketchSolver::solve(probe));
    for (const auto& c : candidates) {
        if (!checkConstraint(sk, c).empty())
            continue;
        Sketch trial = sk;
        trial.constraints.push_back(c);
        const size_t bad = badness(SketchSolver::solve(trial));
        if (bad > base || bad == std::numeric_limits<size_t>::max())
            continue;  // redundant or conflicting: already implied (or impossible)
        base = bad;
        sk.constraints.push_back(c);
        added.push_back(c.id);
    }
    return added;
}

std::string checkConstraint(const Sketch& sk, const SketchConstraint& c)
{
    auto ga = sk.geoOrReserved(c.a.geo);
    auto gb = c.b.valid() ? sk.geoOrReserved(c.b.geo) : std::nullopt;
    if (!ga || (c.b.valid() && !gb))
        return "references unknown geometry";
    const bool aPt = c.a.pos != PointPos::None || ga->kind == GeoKind::Point;
    const bool bPt = gb && (c.b.pos != PointPos::None || gb->kind == GeoKind::Point);
    auto isCirc = [](const std::optional<SketchGeo>& g) {
        return g && (g->kind == GeoKind::Circle || g->kind == GeoKind::Arc);
    };
    switch (c.kind) {
        case ConstraintKind::Midpoint: {
            if (!gb || aPt == bPt)
                return "midpoint needs one point and one line or arc";
            const auto& crv = aPt ? gb : ga;
            if (crv->kind != GeoKind::Line && crv->kind != GeoKind::Arc)
                return "midpoint needs a line or an arc";
            return {};
        }
        case ConstraintKind::Symmetric: {
            auto ax = c.c.valid() ? sk.geoOrReserved(c.c.geo) : std::nullopt;
            if (!gb || !ax)
                return "symmetry needs two entities and a symmetry line";
            const bool axPt = c.c.pos != PointPos::None || ax->kind == GeoKind::Point;
            if (!axPt && ax->kind != GeoKind::Line)
                return "the symmetry axis must be a line (or the X / Y axis)";
            if (c.a.geo == c.c.geo || c.b.geo == c.c.geo)
                return "the symmetry axis cannot be one of the symmetric entities";
            if (aPt && bPt)
                return {};
            if (aPt != bPt || ga->kind != gb->kind || ga->kind == GeoKind::Point)
                return "symmetry needs two points, two lines, two circles or two arcs";
            return {};
        }
        case ConstraintKind::Concentric:
            if (!isCirc(ga) || !isCirc(gb) || c.a.geo == c.b.geo)
                return "concentric needs two circles / arcs";
            return {};
        case ConstraintKind::Collinear:
            if (!gb || ga->kind != GeoKind::Line || gb->kind != GeoKind::Line || aPt || bPt || c.a.geo == c.b.geo)
                return "collinear needs two lines";
            return {};
        default: return {};
    }
}

SketchConstraint makeSymmetric(const Sketch& sk, const GeoRef& a, const GeoRef& b, const GeoRef& axis)
{
    SketchConstraint c = mkC(ConstraintKind::Symmetric, a, b);
    c.c = axis;
    auto ga = sk.geoOrReserved(a.geo), gb = sk.geoOrReserved(b.geo), ax = sk.geoOrReserved(axis.geo);
    if (ga && gb && ax && ga->kind == GeoKind::Line && gb->kind == GeoKind::Line && a.pos == PointPos::None &&
        b.pos == PointPos::None && ax->kind == GeoKind::Line && axis.pos == PointPos::None) {
        const Vec2 m = reflect(ga->p1, ax->p1, ax->p2 - ax->p1);
        if ((m - gb->p2).length() < (m - gb->p1).length())
            c.value = 1;  // a.p1 <-> b.p2
    }
    return c;
}

// ================================================================== smart primitives

ToolResult addCenterRectangle(Sketch& sk, Vec2 center, Vec2 corner, bool construction)
{
    const double hx = std::abs(corner.x - center.x), hy = std::abs(corner.y - center.y);
    if (hx < kTol || hy < kTol)
        return fail("The rectangle has no area");
    ToolResult res;
    Builder b{sk, res};
    const Vec2 q[4] = {center + Vec2{-hx, -hy}, center + Vec2{hx, -hy}, center + Vec2{hx, hy}, center + Vec2{-hx, hy}};
    Uuid ids[4];
    for (int i = 0; i < 4; ++i)
        ids[i] = b.geo(mkLine(q[i], q[(i + 1) % 4], construction));
    for (int i = 0; i < 4; ++i)
        b.con(ConstraintKind::Coincident, {ids[i], PointPos::End}, {ids[(i + 1) % 4], PointPos::Start});
    b.con(ConstraintKind::Horizontal, {ids[0]});
    b.con(ConstraintKind::Horizontal, {ids[2]});
    b.con(ConstraintKind::Vertical, {ids[1]});
    b.con(ConstraintKind::Vertical, {ids[3]});
    const Uuid d1 = b.geo(mkLine(q[0], q[2], true));
    const Uuid d2 = b.geo(mkLine(q[1], q[3], true));
    b.con(ConstraintKind::Coincident, {d1, PointPos::Start}, {ids[0], PointPos::Start});
    b.con(ConstraintKind::Coincident, {d1, PointPos::End}, {ids[2], PointPos::Start});
    b.con(ConstraintKind::Coincident, {d2, PointPos::Start}, {ids[1], PointPos::Start});
    b.con(ConstraintKind::Coincident, {d2, PointPos::End}, {ids[3], PointPos::Start});
    const Uuid cp = b.geo(mkPoint(center));
    b.con(ConstraintKind::Midpoint, {cp, PointPos::Start}, {d1, PointPos::None});
    res.named = {{"bottom", ids[0]}, {"right", ids[1]},  {"top", ids[2]},   {"left", ids[3]},
                 {"diagonal1", d1},  {"diagonal2", d2}, {"center", cp}};
    res.ok = true;
    return res;
}

ToolResult addSlot(Sketch& sk, Vec2 c1, Vec2 c2, double radius, bool overallCenter, bool construction)
{
    const Vec2 axisV = c2 - c1;
    if (axisV.length() < kTol)
        return fail("The two slot centers are the same point");
    if (radius < kTol)
        return fail("The slot width must be positive");
    ToolResult res;
    Builder b{sk, res};
    const double phi = angleOf(axisV);
    const Vec2 n = polar(phi + kPi / 2);
    const Uuid axis = b.geo(mkLine(c1, c2, true));
    const Uuid arc1 = b.geo(mkArc(c1, radius, phi + kPi / 2, phi + 3 * kPi / 2, construction));
    const Uuid arc2 = b.geo(mkArc(c2, radius, phi - kPi / 2, phi + kPi / 2, construction));
    const Uuid s1 = b.geo(mkLine(c1 - n * radius, c2 - n * radius, construction));
    const Uuid s2 = b.geo(mkLine(c2 + n * radius, c1 + n * radius, construction));
    b.con(ConstraintKind::Coincident, {arc1, PointPos::Center}, {axis, PointPos::Start});
    b.con(ConstraintKind::Coincident, {arc2, PointPos::Center}, {axis, PointPos::End});
    b.con(ConstraintKind::Coincident, {arc1, PointPos::End}, {s1, PointPos::Start});
    b.con(ConstraintKind::Coincident, {s1, PointPos::End}, {arc2, PointPos::Start});
    b.con(ConstraintKind::Coincident, {arc2, PointPos::End}, {s2, PointPos::Start});
    b.con(ConstraintKind::Coincident, {s2, PointPos::End}, {arc1, PointPos::Start});
    b.con(ConstraintKind::Tangent, {s1}, {arc1});
    b.con(ConstraintKind::Tangent, {s1}, {arc2});
    b.con(ConstraintKind::Tangent, {s2}, {arc1});
    b.con(ConstraintKind::Tangent, {s2}, {arc2});
    b.con(ConstraintKind::Equal, {arc1}, {arc2});
    res.named = {{"axis", axis}, {"arc1", arc1}, {"arc2", arc2}, {"side1", s1}, {"side2", s2}};
    if (overallCenter) {
        const Uuid cp = b.geo(mkPoint((c1 + c2) * 0.5));
        b.con(ConstraintKind::Midpoint, {cp, PointPos::Start}, {axis, PointPos::None});
        res.named["center"] = cp;
    }
    res.ok = true;
    return res;
}

ToolResult addPolygon(Sketch& sk, Vec2 center, Vec2 vertex, int sides, bool inscribed, bool construction)
{
    if (sides < 3 || sides > 64)
        return fail("A polygon has 3 to 64 sides");
    const double R0 = (vertex - center).length();
    if (R0 < kTol)
        return fail("The polygon has no size");
    ToolResult res;
    Builder b{sk, res};
    const double step = 2 * kPi / sides;
    double a0 = angleOf(vertex - center);
    double R = R0;
    if (!inscribed) {  // `vertex` is the middle of a side: the apothem is R0
        R = R0 / std::cos(kPi / sides);
        a0 -= kPi / sides;
    }
    const Uuid circ = b.geo(mkCircle(center, R, true));
    std::vector<Uuid> L;
    for (int k = 0; k < sides; ++k)
        L.push_back(b.geo(mkLine(center + polar(a0 + k * step) * R, center + polar(a0 + (k + 1) * step) * R, construction)));
    for (int k = 0; k < sides; ++k) {
        b.con(ConstraintKind::Coincident, {L[size_t(k)], PointPos::End}, {L[size_t((k + 1) % sides)], PointPos::Start});
        b.con(ConstraintKind::PointOnObject, {L[size_t(k)], PointPos::Start}, {circ});
        if (k > 0)
            b.con(ConstraintKind::Equal, {L[0]}, {L[size_t(k)]});
    }
    res.named["circle"] = circ;
    if (!inscribed) {
        const Uuid in = b.geo(mkCircle(center, R0, true));
        b.con(ConstraintKind::Concentric, {in}, {circ});
        b.con(ConstraintKind::Tangent, {L[0]}, {in});
        res.named["incircle"] = in;
    }
    for (int k = 0; k < sides; ++k)
        res.named["side" + std::to_string(k)] = L[size_t(k)];
    res.ok = true;
    return res;
}

// ================================================================== trim

namespace {

struct TrimPlan
{
    enum Kind { DeleteAll, CutStart, CutEnd, Split, CircleToArc } kind = DeleteAll;
    double lo = 0, hi = 0;  ///< removed parameter interval
    Vec2 pLo, pHi;
    Uuid otherLo, otherHi;
};

std::optional<TrimPlan> planTrim(const Sketch& sk, const SketchGeo& g, Vec2 pick)
{
    if (g.kind == GeoKind::Point)
        return std::nullopt;
    const auto xs = isectsOnSupport(sk, g);
    TrimPlan plan;
    if (g.kind == GeoKind::Circle) {
        std::vector<Isect> v;
        for (const auto& x : xs) {
            bool dup = false;
            for (const auto& y : v)
                if ((y.p - x.p).length() < 1e-5)
                    dup = true;
            if (!dup)
                v.push_back(x);
        }
        if (v.size() < 2)
            return plan;  // delete the whole circle
        std::sort(v.begin(), v.end(), [](const Isect& a, const Isect& b) { return a.t < b.t; });
        const double tp = paramOf(g, pick);
        size_t i = 0;  // removed interval: (v[i-1], v[i]) cyclic, containing tp
        while (i < v.size() && v[i].t < tp)
            ++i;
        const Isect& lo = v[(i + v.size() - 1) % v.size()];
        const Isect& hi = v[i % v.size()];
        plan.kind = TrimPlan::CircleToArc;
        plan.lo = lo.t;
        plan.hi = hi.t;
        plan.pLo = lo.p;
        plan.pHi = hi.p;
        plan.otherLo = lo.other;
        plan.otherHi = hi.other;
        return plan;
    }
    const double span = g.kind == GeoKind::Line ? 1.0 : g.a1 - g.a0;
    const double scale = g.kind == GeoKind::Line ? (g.p2 - g.p1).length() : g.r;
    const double eps = 1e-5 / std::max(scale, 1e-9);
    double tp = paramOf(g, pick);
    if (g.kind == GeoKind::Arc && tp > span)  // picked off the arc: nearest end
        tp = (tp - span < 2 * kPi - tp) ? span : 0.0;
    std::optional<Isect> lo, hi;
    for (const auto& x : xs) {
        double t = x.t;
        if (g.kind == GeoKind::Arc && t > span + eps)
            continue;
        if (t <= eps || t >= span - eps)
            continue;  // touching at an end: not a cut
        if (t < tp && (!lo || t > lo->t))
            lo = Isect{t, x.p, x.other};
        if (t > tp && (!hi || t < hi->t))
            hi = Isect{t, x.p, x.other};
    }
    plan.lo = lo ? lo->t : 0.0;
    plan.hi = hi ? hi->t : span;
    if (lo) {
        plan.pLo = lo->p;
        plan.otherLo = lo->other;
    }
    if (hi) {
        plan.pHi = hi->p;
        plan.otherHi = hi->other;
    }
    plan.kind = !lo && !hi ? TrimPlan::DeleteAll : !lo ? TrimPlan::CutStart : !hi ? TrimPlan::CutEnd : TrimPlan::Split;
    return plan;
}

}  // namespace

std::optional<CurvePiece> trimPreview(const Sketch& sk, const Uuid& id, Vec2 pick)
{
    const SketchGeo* g = sk.find(id);
    if (!g || g->external())
        return std::nullopt;
    auto plan = planTrim(sk, *g, pick);
    if (!plan)
        return std::nullopt;
    CurvePiece piece;
    piece.kind = g->kind;
    piece.c = g->c;
    piece.r = g->r;
    switch (g->kind) {
        case GeoKind::Line:
            piece.p1 = pointAt(*g, plan->lo);
            piece.p2 = pointAt(*g, plan->hi);
            break;
        case GeoKind::Arc:
            piece.a0 = g->a0 + plan->lo;
            piece.a1 = g->a0 + plan->hi;
            break;
        case GeoKind::Circle:
            if (plan->kind == TrimPlan::DeleteAll) {
                piece.a0 = 0;
                piece.a1 = 2 * kPi;
            }
            else {
                piece.kind = GeoKind::Arc;
                piece.a0 = plan->lo;
                piece.a1 = plan->hi;
                while (piece.a1 <= piece.a0)
                    piece.a1 += 2 * kPi;
            }
            break;
        default: return std::nullopt;
    }
    return piece;
}

ToolResult trim(Sketch& sk, const Uuid& geoId, Vec2 pick)
{
    const Uuid id = geoId;
    SketchGeo* gp = sk.find(id);
    if (!gp)
        return fail("Unknown geometry");
    if (gp->external())
        return fail("A projected edge cannot be trimmed");
    auto plan = planTrim(sk, *gp, pick);
    if (!plan)
        return fail("Only lines, arcs and circles can be trimmed");
    ToolResult res;
    res.ok = true;
    if (plan->kind == TrimPlan::DeleteAll) {
        const size_t n = sk.constraints.size();
        sk.removeGeo(id);
        res.removedConstraints = int(n - sk.constraints.size());
        return res;
    }

    std::vector<SketchConstraint> links;
    auto attachTo = [&](const GeoRef& pt, Vec2 p, const Uuid& other) {
        if (auto c = attach(sk, pt, p, other))
            links.push_back(*c);
    };
    const SketchGeo g = *gp;
    switch (plan->kind) {
        case TrimPlan::CutStart:
        case TrimPlan::CutEnd: {
            const bool start = plan->kind == TrimPlan::CutStart;
            const GeoRef end{id, start ? PointPos::Start : PointPos::End};
            res.removedConstraints += dropPointConstraints(sk, end);
            res.removedConstraints += dropShapeConstraints(sk, id, true);
            gp = sk.find(id);
            if (g.kind == GeoKind::Line)
                (start ? gp->p1 : gp->p2) = start ? plan->pHi : plan->pLo;
            else if (start)
                gp->a0 = g.a0 + plan->hi;
            else
                gp->a1 = g.a0 + plan->lo;
            attachTo(end, start ? plan->pHi : plan->pLo, start ? plan->otherHi : plan->otherLo);
            break;
        }
        case TrimPlan::Split: {
            res.removedConstraints += dropShapeConstraints(sk, id, true);
            SketchGeo n = g;
            n.id = newUuid();
            n.ext = {};
            gp = sk.find(id);
            if (g.kind == GeoKind::Line) {
                n.p1 = plan->pHi;
                gp->p2 = plan->pLo;
            }
            else {
                n.a0 = g.a0 + plan->hi;
                n.a1 = g.a1;
                gp->a1 = g.a0 + plan->lo;
            }
            retargetPoint(sk, {id, PointPos::End}, {n.id, PointPos::End});
            sk.geos.push_back(n);
            res.geos.push_back(n.id);
            if (g.kind == GeoKind::Line) {
                auto c = mkC(ConstraintKind::Collinear, {id}, {n.id});
                sk.constraints.push_back(c);
                res.constraints.push_back(c.id);
            }
            else {
                auto c1 = mkC(ConstraintKind::Concentric, {id}, {n.id});
                auto c2 = mkC(ConstraintKind::Equal, {id}, {n.id});
                sk.constraints.push_back(c1);
                sk.constraints.push_back(c2);
                res.constraints.push_back(c1.id);
                res.constraints.push_back(c2.id);
            }
            attachTo({id, PointPos::End}, plan->pLo, plan->otherLo);
            attachTo({n.id, PointPos::Start}, plan->pHi, plan->otherHi);
            break;
        }
        case TrimPlan::CircleToArc: {
            // symmetric circles become arcs: the circle semantics of the symmetry no longer hold
            res.removedConstraints += dropShapeConstraints(sk, id, false);
            gp = sk.find(id);
            gp->kind = GeoKind::Arc;
            gp->a0 = plan->hi;
            gp->a1 = plan->lo;
            while (gp->a1 <= gp->a0)
                gp->a1 += 2 * kPi;
            attachTo({id, PointPos::Start}, plan->pHi, plan->otherHi);
            attachTo({id, PointPos::End}, plan->pLo, plan->otherLo);
            break;
        }
        default: break;
    }
    for (const Uuid& c : addIfIndependent(sk, links))
        res.constraints.push_back(c);
    return res;
}

// ================================================================== extend

namespace {

struct ExtendPlan
{
    bool atStart = false;
    double t = 0;  ///< line: new parameter; arc: angle delta (> 0)
    Vec2 p;
    Uuid other;
};

std::optional<ExtendPlan> planExtend(const Sketch& sk, const SketchGeo& g, Vec2 pick, std::string* why)
{
    if (g.kind != GeoKind::Line && g.kind != GeoKind::Arc) {
        if (why)
            *why = "Only lines and arcs can be extended";
        return std::nullopt;
    }
    ExtendPlan plan;
    plan.atStart = (pick - g.point(PointPos::Start)).length() < (pick - g.point(PointPos::End)).length();
    std::optional<double> best;
    for (const auto& x : isectsOnSupport(sk, g)) {
        double d;  // distance (parameter) beyond the extended end
        if (g.kind == GeoKind::Line) {
            const double L = (g.p2 - g.p1).length();
            d = plan.atStart ? -x.t : x.t - 1.0;
            if (d <= 1e-6 / L)
                continue;
        }
        else {
            const double span = g.a1 - g.a0;
            const double ang = angleOf(x.p - g.c);
            d = plan.atStart ? norm2pi(g.a0 - ang) : norm2pi(ang - g.a1);
            if (d <= 1e-6 / g.r || d >= 2 * kPi - span - 1e-9)
                continue;
        }
        if (!best || d < *best) {
            best = d;
            plan.p = x.p;
            plan.other = x.other;
        }
    }
    if (!best) {
        if (why)
            *why = "Nothing to extend to on that side";
        return std::nullopt;
    }
    plan.t = *best;
    return plan;
}

void applyExtend(SketchGeo& g, const ExtendPlan& plan)
{
    if (g.kind == GeoKind::Line)
        (plan.atStart ? g.p1 : g.p2) = plan.p;
    else if (plan.atStart)
        g.a0 -= plan.t;
    else
        g.a1 += plan.t;
}

CurvePiece pieceOf(const SketchGeo& g)
{
    CurvePiece p;
    p.kind = g.kind;
    p.p1 = g.p1;
    p.p2 = g.p2;
    p.c = g.c;
    p.r = g.r;
    p.a0 = g.a0;
    p.a1 = g.a1;
    return p;
}

}  // namespace

std::optional<CurvePiece> extendPreview(const Sketch& sk, const Uuid& id, Vec2 pick)
{
    const SketchGeo* g = sk.find(id);
    if (!g || g->external())
        return std::nullopt;
    auto plan = planExtend(sk, *g, pick, nullptr);
    if (!plan)
        return std::nullopt;
    SketchGeo e = *g;
    applyExtend(e, *plan);
    return pieceOf(e);
}

ToolResult extend(Sketch& sk, const Uuid& geoId, Vec2 pick)
{
    const Uuid id = geoId;
    SketchGeo* g = sk.find(id);
    if (!g)
        return fail("Unknown geometry");
    if (g->external())
        return fail("A projected edge cannot be extended");
    std::string why;
    auto plan = planExtend(sk, *g, pick, &why);
    if (!plan)
        return fail(why);
    const GeoRef end{id, plan->atStart ? PointPos::Start : PointPos::End};
    for (const auto& c : sk.constraints)
        if (c.kind == ConstraintKind::Coincident && c.referencesPoint(end))
            return fail("This end is connected to other geometry: delete the coincidence first");
    ToolResult res;
    res.ok = true;
    res.removedConstraints += dropPointConstraints(sk, end);
    res.removedConstraints += dropShapeConstraints(sk, id, true);
    g = sk.find(id);
    applyExtend(*g, *plan);
    if (auto c = attach(sk, end, plan->p, plan->other))
        for (const Uuid& cid : addIfIndependent(sk, {*c}))
            res.constraints.push_back(cid);
    return res;
}

// ================================================================== fillet

ToolResult filletLines(Sketch& sk, const Uuid& lineA, const Uuid& lineB, double radius)
{
    const Uuid idA = lineA, idB = lineB;  // copies: the arguments may point into sk.geos
    const SketchGeo* A = sk.find(idA);
    const SketchGeo* B = sk.find(idB);
    if (!A || !B || A->kind != GeoKind::Line || B->kind != GeoKind::Line || idA == idB)
        return fail("A sketch fillet needs two lines");
    if (A->external() || B->external())
        return fail("Projected edges cannot be filleted");
    if (radius <= kTol)
        return fail("The fillet radius must be positive");
    // shared corner
    PointPos pa = PointPos::None, pb = PointPos::None;
    double bestD = 1e-5;
    for (PointPos x : {PointPos::Start, PointPos::End})
        for (PointPos y : {PointPos::Start, PointPos::End}) {
            const double d = (A->point(x) - B->point(y)).length();
            if (d < bestD) {
                bestD = d;
                pa = x;
                pb = y;
            }
        }
    if (pa == PointPos::None)
        return fail("The two lines must share an end point");
    const PointPos fa = pa == PointPos::Start ? PointPos::End : PointPos::Start;
    const PointPos fb = pb == PointPos::Start ? PointPos::End : PointPos::Start;
    const Vec2 P = A->point(pa), FA = A->point(fa), FB = B->point(fb);
    const Vec2 uA = (FA - P).normalized(), uB = (FB - P).normalized();
    const double theta = std::acos(std::clamp(uA.dot(uB), -1.0, 1.0));
    if (theta < 1e-3 || theta > kPi - 1e-3)
        return fail("The lines are collinear: nothing to fillet");
    const double half = theta / 2;
    const double d = radius / std::tan(half);
    if (d >= (FA - P).length() - kTol || d >= (FB - P).length() - kTol)
        return fail("The fillet radius is too large for these lines");
    const Vec2 T1 = P + uA * d, T2 = P + uB * d;
    const Vec2 C = P + (uA + uB).normalized() * (radius / std::sin(half));

    ToolResult res;
    res.ok = true;
    // constraints on the corner disappear; length dimensions become point-line distances
    std::vector<std::pair<Uuid, Uuid>> lengthDims;  // (dimension, line) -> converted below
    for (const auto& c : sk.constraints)
        if (c.kind == ConstraintKind::Distance && !c.b.valid() && c.a.pos == PointPos::None &&
            (c.a.geo == idA || c.a.geo == idB))
            lengthDims.emplace_back(c.id, c.a.geo);
    res.removedConstraints += dropPointConstraints(sk, {idA, pa});
    res.removedConstraints += dropPointConstraints(sk, {idB, pb});
    for (auto& c : sk.constraints)
        for (auto& [dim, line] : lengthDims)
            if (c.id == dim) {
                const bool onA = line == idA;
                c.a = {line, onA ? fa : fb};
                c.b = {onA ? idB : idA, PointPos::None};
                c.expr.clear();
                c.value = 0;  // measured once the lines are trimmed
            }
    res.removedConstraints += dropShapeConstraints(sk, idA, true);
    res.removedConstraints += dropShapeConstraints(sk, idB, true);
    (pa == PointPos::Start ? sk.find(idA)->p1 : sk.find(idA)->p2) = T1;
    (pb == PointPos::Start ? sk.find(idB)->p1 : sk.find(idB)->p2) = T2;
    for (auto& c : sk.constraints)
        for (auto& [dim, line] : lengthDims)
            if (c.id == dim)
                c.value = sk.measure(c);

    double aT1 = angleOf(T1 - C), aT2 = angleOf(T2 - C);
    const double s = norm2pi(aT2 - aT1);
    const bool startOnA = s <= kPi;
    SketchGeo arc = startOnA ? mkArc(C, radius, aT1, aT1 + s, false) : mkArc(C, radius, aT2, aT2 + (2 * kPi - s), false);
    arc.construction = A->construction && B->construction;
    Builder b{sk, res};
    b.geo(arc);
    b.con(ConstraintKind::Coincident, {arc.id, PointPos::Start}, {startOnA ? idA : idB, startOnA ? pa : pb});
    b.con(ConstraintKind::Coincident, {arc.id, PointPos::End}, {startOnA ? idB : idA, startOnA ? pb : pa});
    b.con(ConstraintKind::Tangent, {idA}, {arc.id});
    b.con(ConstraintKind::Tangent, {idB}, {arc.id});
    res.driving = b.con(ConstraintKind::Radius, {arc.id}, {}, radius);
    res.named = {{"arc", arc.id}};
    return res;
}

ToolResult filletAtPoint(Sketch& sk, const GeoRef& corner, double radius)
{
    auto p = sk.pointOf(corner);
    const SketchGeo* g = sk.find(corner.geo);
    if (!p || !g || g->kind != GeoKind::Line)
        return fail("Click the corner shared by two lines");
    for (const SketchGeo& o : sk.geos)
        if (o.id != g->id && o.kind == GeoKind::Line &&
            ((o.p1 - *p).length() < 1e-5 || (o.p2 - *p).length() < 1e-5))
            return filletLines(sk, g->id, o.id, radius);
    return fail("No second line ends at this corner");
}

// ================================================================== offset

namespace {

struct Elem
{
    Uuid id;
    bool rev = false;
};

Vec2 travStart(const SketchGeo& g, bool rev) { return g.point(rev ? PointPos::End : PointPos::Start); }
Vec2 travEnd(const SketchGeo& g, bool rev) { return g.point(rev ? PointPos::Start : PointPos::End); }
PointPos travStartPos(bool rev) { return rev ? PointPos::End : PointPos::Start; }
PointPos travEndPos(bool rev) { return rev ? PointPos::Start : PointPos::End; }

void setTravPoint(SketchGeo& g, bool rev, bool atEnd, Vec2 p)
{
    const bool nativeEnd = atEnd != rev;  // which native end is moved
    if (g.kind == GeoKind::Line) {
        (nativeEnd ? g.p2 : g.p1) = p;
        return;
    }
    const double th = angleOf(p - g.c);
    if (nativeEnd)
        g.a1 = g.a0 + std::max(norm2pi(th - g.a0), 1e-9);
    else
        g.a0 = g.a1 - std::max(norm2pi(g.a1 - th), 1e-9);
}

}  // namespace

ToolResult offsetCurves(Sketch& sk, const std::vector<Uuid>& ids, double distance)
{
    if (std::abs(distance) < kTol)
        return fail("The offset distance must not be zero");
    std::vector<Uuid> curves, circles;
    for (const Uuid& id : ids) {
        const SketchGeo* g = sk.find(id);
        if (!g)
            continue;
        if (g->kind == GeoKind::Line || g->kind == GeoKind::Arc)
            curves.push_back(id);
        else if (g->kind == GeoKind::Circle)
            circles.push_back(id);
    }
    if (curves.empty() && circles.empty())
        return fail("Select lines, arcs or circles to offset");

    // ---- chains
    std::vector<std::vector<Elem>> chains;
    std::vector<bool> closedFlags;
    std::set<Uuid> used;
    auto findAt = [&](Vec2 p) -> std::optional<Elem> {
        for (const Uuid& id : curves) {
            if (used.count(id))
                continue;
            const SketchGeo* g = sk.find(id);
            if ((g->point(PointPos::Start) - p).length() < 1e-5)
                return Elem{id, false};
            if ((g->point(PointPos::End) - p).length() < 1e-5)
                return Elem{id, true};
        }
        return std::nullopt;
    };
    for (const Uuid& seed : curves) {
        if (used.count(seed))
            continue;
        used.insert(seed);
        std::vector<Elem> chain{{seed, false}};
        while (true) {  // forward
            const SketchGeo* last = sk.find(chain.back().id);
            auto nx = findAt(travEnd(*last, chain.back().rev));
            if (!nx)
                break;
            used.insert(nx->id);
            chain.push_back(*nx);
        }
        while (true) {  // backward
            const SketchGeo* first = sk.find(chain.front().id);
            auto pv = findAt(travStart(*first, chain.front().rev));
            if (!pv)
                break;
            used.insert(pv->id);
            pv->rev = !pv->rev;  // found by its start: traversed backwards to end here
            chain.insert(chain.begin(), *pv);
        }
        const bool closed = chain.size() >= 2 &&
                            (travEnd(*sk.find(chain.back().id), chain.back().rev) -
                             travStart(*sk.find(chain.front().id), chain.front().rev))
                                    .length() < 1e-5;
        chains.push_back(chain);
        closedFlags.push_back(closed);
    }

    ToolResult res;
    Builder b{sk, res};
    std::vector<Uuid> connectors;
    auto addConnector = [&](const SketchGeo orig, const SketchGeo off) {  // copies: sk.geos grows
        SketchGeo L;
        if (orig.kind == GeoKind::Line) {
            const Vec2 q = off.p1, d = (orig.p2 - orig.p1).normalized();
            const Vec2 f = orig.p1 + d * (q - orig.p1).dot(d);
            L = mkLine(q, f, true);
            b.geo(L);
            b.con(ConstraintKind::Coincident, {L.id, PointPos::Start}, {off.id, PointPos::Start});
            b.con(ConstraintKind::PointOnObject, {L.id, PointPos::End}, {orig.id});
            b.con(ConstraintKind::Perpendicular, {L.id}, {orig.id});
        }
        else if (orig.kind == GeoKind::Arc) {
            const Vec2 q = off.point(PointPos::Start);
            const Vec2 f = orig.c + (q - orig.c).normalized() * orig.r;
            L = mkLine(q, f, true);
            b.geo(L);
            b.con(ConstraintKind::Coincident, {L.id, PointPos::Start}, {off.id, PointPos::Start});
            b.con(ConstraintKind::PointOnObject, {L.id, PointPos::End}, {orig.id});
            b.con(ConstraintKind::PointOnObject, {orig.id, PointPos::Center}, {L.id});
        }
        else {  // circle
            L = mkLine(off.c + Vec2{off.r, 0}, orig.c + Vec2{orig.r, 0}, true);
            b.geo(L);
            b.con(ConstraintKind::PointOnObject, {L.id, PointPos::Start}, {off.id});
            b.con(ConstraintKind::PointOnObject, {L.id, PointPos::End}, {orig.id});
            b.con(ConstraintKind::PointOnObject, {orig.id, PointPos::Center}, {L.id});
            b.con(ConstraintKind::Horizontal, {L.id});
        }
        connectors.push_back(L.id);
    };

    // ---- offset geometry of every chain (computed first: nothing is added if it fails)
    struct Planned
    {
        std::vector<Elem> chain;
        std::vector<SketchGeo> geos;
        bool closed;
    };
    std::vector<Planned> planned;
    for (size_t ci = 0; ci < chains.size(); ++ci) {
        const auto& chain = chains[ci];
        const bool closed = closedFlags[ci];
        double oRight = -distance;  // open chain: positive distance = left side
        if (closed) {
            double area = 0;
            std::vector<Vec2> poly;
            for (const Elem& e : chain) {
                const SketchGeo* g = sk.find(e.id);
                if (g->kind == GeoKind::Line)
                    poly.push_back(travStart(*g, e.rev));
                else
                    for (int k = 0; k < 8; ++k) {
                        const double t = (g->a1 - g->a0) * k / 8;
                        poly.push_back(g->c + polar(e.rev ? g->a1 - t : g->a0 + t) * g->r);
                    }
            }
            for (size_t i = 0; i < poly.size(); ++i)
                area += poly[i].cross(poly[(i + 1) % poly.size()]);
            oRight = area > 0 ? distance : -distance;  // CCW: outside is on the right
        }
        Planned pl{chain, {}, closed};
        for (const Elem& e : chain) {
            SketchGeo n = *sk.find(e.id);
            n.id = newUuid();
            n.ext = {};
            n.construction = sk.find(e.id)->construction && !sk.find(e.id)->external();
            if (n.kind == GeoKind::Line) {
                const Vec2 u = (n.p2 - n.p1).normalized();
                const Vec2 nr = Vec2{u.y, -u.x} * (e.rev ? -1.0 : 1.0);
                n.p1 = n.p1 + nr * oRight;
                n.p2 = n.p2 + nr * oRight;
            }
            else {
                n.r += e.rev ? -oRight : oRight;
                if (n.r <= kTol)
                    return fail("The offset distance is larger than an arc radius");
            }
            pl.geos.push_back(n);
        }
        // joints
        const size_t n = chain.size();
        for (size_t i = 0; i + (closed ? 0 : 1) < n; ++i) {
            const size_t j = (i + 1) % n;
            SketchGeo &gi = pl.geos[i], &gj = pl.geos[j];
            const Vec2 ei = travEnd(gi, chain[i].rev), sj = travStart(gj, chain[j].rev);
            if ((ei - sj).length() < 1e-7)
                continue;
            const auto xs = supportIntersections(gi, gj);
            if (xs.empty())
                return fail("The offset cannot join these curves (distance too large?)");
            const Vec2 mid = (ei + sj) * 0.5;
            Vec2 best = xs[0];
            for (Vec2 x : xs)
                if ((x - mid).length() < (best - mid).length())
                    best = x;
            setTravPoint(gi, chain[i].rev, true, best);
            setTravPoint(gj, chain[j].rev, false, best);
        }
        for (size_t i = 0; i < n; ++i) {
            const SketchGeo& o = *sk.find(chain[i].id);
            const SketchGeo& g = pl.geos[i];
            if (g.kind == GeoKind::Line && (g.p2 - g.p1).dot(o.p2 - o.p1) <= kTol)
                return fail("The offset distance is too large for this contour");
        }
        planned.push_back(std::move(pl));
    }
    for (const Uuid& id : circles) {
        const SketchGeo* o = sk.find(id);
        if (o->r + distance <= kTol)
            return fail("The offset distance is larger than a circle radius");
    }

    // ---- commit geometry + constraints
    std::vector<SketchConstraint> softTangents;  // around a closed smooth loop the last one is implied
    for (const Planned& pl : planned) {
        const size_t n = pl.chain.size();
        for (size_t i = 0; i < n; ++i) {
            b.geo(pl.geos[i]);
            const SketchGeo& o = *sk.find(pl.chain[i].id);
            b.con(o.kind == GeoKind::Line ? ConstraintKind::Parallel : ConstraintKind::Concentric, {o.id}, {pl.geos[i].id});
        }
        for (size_t i = 0; i + (pl.closed ? 0 : 1) < n; ++i) {
            const size_t j = (i + 1) % n;
            b.con(ConstraintKind::Coincident, {pl.geos[i].id, travEndPos(pl.chain[i].rev)},
                  {pl.geos[j].id, travStartPos(pl.chain[j].rev)});
            // smooth (G1) joint with a curve: keep it tangent (and avoid a singular joint)
            const SketchGeo &oi = *sk.find(pl.chain[i].id), &oj = *sk.find(pl.chain[j].id);
            if (oi.kind != GeoKind::Line || oj.kind != GeoKind::Line) {
                auto tangentAt = [](const SketchGeo& g, Vec2 p) {
                    return g.kind == GeoKind::Line ? (g.p2 - g.p1).normalized() : Vec2{-(p - g.c).y, (p - g.c).x}.normalized();
                };
                const Vec2 jp = travEnd(oi, pl.chain[i].rev);
                if (std::abs(tangentAt(oi, jp).cross(tangentAt(oj, jp))) < 1e-6)
                    softTangents.push_back(mkC(ConstraintKind::Tangent, {pl.geos[i].id}, {pl.geos[j].id}));
            }
        }
        for (size_t i = 0; i < n; ++i)
            addConnector(*sk.find(pl.chain[i].id), *sk.find(pl.geos[i].id));
    }
    for (const Uuid& id : circles) {
        SketchGeo n = *sk.find(id);
        n.id = newUuid();
        n.ext = {};
        n.construction = sk.find(id)->construction && !sk.find(id)->external();
        n.r += distance;
        b.geo(n);
        b.con(ConstraintKind::Concentric, {id}, {n.id});
        addConnector(*sk.find(id), *sk.find(n.id));
    }
    res.driving = b.con(ConstraintKind::Distance, {connectors[0]}, {}, std::abs(distance));
    for (const Uuid& c : addIfIndependent(sk, softTangents))
        res.constraints.push_back(c);
    std::vector<SketchConstraint> equal;  // some are implied (collinear pieces, tangent joints)
    for (size_t k = 1; k < connectors.size(); ++k)
        equal.push_back(mkC(ConstraintKind::Equal, {connectors[0]}, {connectors[k]}));
    for (const Uuid& c : addIfIndependent(sk, equal))
        res.constraints.push_back(c);
    res.ok = true;
    return res;
}

// ================================================================== mirror & patterns

namespace {

SketchGeo transformed(const SketchGeo& g, const std::function<Vec2(Vec2)>& f, double rot, bool mirror, double phi)
{
    SketchGeo n = g;
    n.id = newUuid();
    n.ext = {};
    n.construction = g.construction || g.external();
    n.p1 = f(g.p1);
    n.p2 = f(g.p2);
    n.c = f(g.c);
    if (g.kind == GeoKind::Arc) {
        if (mirror) {
            n.a0 = 2 * phi - g.a1;
            n.a1 = 2 * phi - g.a0;
        }
        else {
            n.a0 = g.a0 + rot;
            n.a1 = g.a1 + rot;
        }
        while (n.a1 <= n.a0)
            n.a1 += 2 * kPi;
    }
    return n;
}

GeoRef anchorOf(const SketchGeo& g)
{
    switch (g.kind) {
        case GeoKind::Line:
        case GeoKind::Point: return {g.id, PointPos::Start};
        default: return {g.id, PointPos::Center};
    }
}

bool absoluteOrientation(ConstraintKind k)
{
    return k == ConstraintKind::Horizontal || k == ConstraintKind::Vertical || k == ConstraintKind::Angle ||
           k == ConstraintKind::DistanceX || k == ConstraintKind::DistanceY || k == ConstraintKind::Fixed;
}

/// Copies the relations between the selected geometry onto a copy (ids remapped).
std::vector<SketchConstraint> copyInternal(const Sketch& sk, const std::map<Uuid, Uuid>& remap, bool keepAbsolute)
{
    std::vector<SketchConstraint> out;
    for (const auto& c : sk.constraints) {
        if (isDimensional(c.kind) || c.kind == ConstraintKind::Fixed || (!keepAbsolute && absoluteOrientation(c.kind)))
            continue;
        bool inside = true;
        for (const GeoRef* r : {&c.a, &c.b, &c.c})
            if (r->valid() && !remap.count(r->geo))
                inside = false;
        if (!inside)
            continue;
        SketchConstraint n = c;
        n.id = newUuid();
        for (GeoRef* r : {&n.a, &n.b, &n.c})
            if (r->valid())
                r->geo = remap.at(r->geo);
        out.push_back(n);
    }
    return out;
}

/// Copies (not pointers: the geometry vector grows while the pattern is built).
std::vector<SketchGeo> patternSource(const Sketch& sk, const std::vector<Uuid>& ids)
{
    std::vector<SketchGeo> src;
    for (const Uuid& id : ids)
        if (const SketchGeo* g = sk.find(id))
            src.push_back(*g);
    return src;
}

}  // namespace

ToolResult mirrorCurves(Sketch& sk, const std::vector<Uuid>& ids, const GeoRef& axisRef)
{
    const GeoRef axis = axisRef;
    auto ax = sk.geoOrReserved(axis.geo);
    if (!ax || ax->kind != GeoKind::Line)
        return fail("The mirror axis must be a line or the X / Y axis");
    const Vec2 A = ax->p1, u = (ax->p2 - ax->p1).normalized();
    const double phi = angleOf(u);
    auto f = [&](Vec2 p) { return reflect(p, A, u); };
    ToolResult res;
    Builder b{sk, res};
    const auto src = patternSource(sk, ids);
    std::vector<SketchGeo> copies;
    std::vector<std::pair<Uuid, Uuid>> pairs;
    for (const SketchGeo& g : src) {
        if (g.id == axis.geo)
            continue;
        if (g.kind == GeoKind::Line && std::abs(u.cross(g.p1 - A)) < kTol && std::abs(u.cross(g.p2 - A)) < kTol)
            continue;  // lies on the axis: its own mirror image
        copies.push_back(transformed(g, f, 0, true, phi));
        pairs.emplace_back(g.id, copies.back().id);
    }
    if (copies.empty())
        return fail("Select the geometry to mirror (and a mirror line)");
    for (const auto& c : copies)
        b.geo(c);
    for (const auto& [o, n] : pairs) {
        const SketchGeo* g = sk.find(o);
        const GeoRef ra = g->kind == GeoKind::Point ? GeoRef{o, PointPos::Start} : GeoRef{o, PointPos::None};
        const GeoRef rb = g->kind == GeoKind::Point ? GeoRef{n, PointPos::Start} : GeoRef{n, PointPos::None};
        b.con(makeSymmetric(sk, ra, rb, {axis.geo, PointPos::None}));
    }
    res.ok = true;
    return res;
}

ToolResult linearPattern(Sketch& sk, const std::vector<Uuid>& ids, Vec2 dir, int count, double spacing)
{
    if (count < 2)
        return fail("A pattern needs at least 2 occurrences");
    if (std::abs(spacing) < kTol || dir.length() < 1e-12)
        return fail("The pattern spacing must not be zero");
    const auto src = patternSource(sk, ids);
    if (src.empty())
        return fail("Select the geometry to repeat");
    const Vec2 u = dir.normalized();
    ToolResult res;
    Builder b{sk, res};
    std::vector<SketchConstraint> soft;
    GeoRef prevAnchor = anchorOf(src[0]);
    Uuid pitch0;
    for (int k = 1; k < count; ++k) {
        const Vec2 t = u * (spacing * k);
        std::map<Uuid, Uuid> remap;
        for (const SketchGeo& g : src) {
            SketchGeo n = transformed(g, [&](Vec2 p) { return p + t; }, 0, false, 0);
            remap[g.id] = n.id;
            b.geo(n);
        }
        for (auto& c : copyInternal(sk, remap, true))
            soft.push_back(c);
        for (const SketchGeo& g : src) {
            if (g.kind == GeoKind::Point)
                continue;
            soft.push_back(mkC(ConstraintKind::Equal, {g.id}, {remap[g.id]}));
            if (g.kind == GeoKind::Line)
                soft.push_back(mkC(ConstraintKind::Parallel, {g.id}, {remap[g.id]}));
        }
        const GeoRef anchor{remap[src[0].id], anchorOf(src[0]).pos};
        const Vec2 pa = *sk.pointOf(prevAnchor), pb = *sk.pointOf(anchor);
        const Uuid pitch = b.geo(mkLine(pa, pb, true));
        b.con(ConstraintKind::Coincident, {pitch, PointPos::Start}, prevAnchor);
        b.con(ConstraintKind::Coincident, {pitch, PointPos::End}, anchor);
        if (k == 1) {
            pitch0 = pitch;
            res.driving = b.con(ConstraintKind::Distance, {pitch}, {}, std::abs(spacing));
            if (std::abs(u.y) < 1e-12)
                b.con(ConstraintKind::Horizontal, {pitch});
            else if (std::abs(u.x) < 1e-12)
                b.con(ConstraintKind::Vertical, {pitch});
            else
                b.con(ConstraintKind::Angle, {pitch}, {}, angleOf(pb - pa));
        }
        else {
            b.con(ConstraintKind::Equal, {pitch0}, {pitch});
            b.con(ConstraintKind::Parallel, {pitch0}, {pitch});
        }
        prevAnchor = anchor;
    }
    for (const Uuid& c : addIfIndependent(sk, soft))
        res.constraints.push_back(c);
    res.ok = true;
    return res;
}

ToolResult circularPattern(Sketch& sk, const std::vector<Uuid>& ids, const GeoRef& centerRef, int count, double totalAngle)
{
    const GeoRef center = centerRef;
    if (count < 2)
        return fail("A pattern needs at least 2 occurrences");
    auto cp = sk.pointOf(center);
    if (!cp)
        return fail("Pick the center point of the pattern");
    const auto src = patternSource(sk, ids);
    if (src.empty())
        return fail("Select the geometry to repeat");
    const bool full = std::abs(std::abs(totalAngle) - 2 * kPi) < 1e-9;
    const double step = full ? totalAngle / count : totalAngle / (count - 1);
    const Vec2 C = *cp;
    const GeoRef anchor0 = anchorOf(src[0]);
    if ((*sk.pointOf(anchor0) - C).length() < kTol)
        return fail("The first selected entity is centered on the pattern center");
    ToolResult res;
    Builder b{sk, res};
    std::vector<SketchConstraint> soft;
    auto radial = [&](const GeoRef& anchor) {
        const Uuid l = b.geo(mkLine(C, *sk.pointOf(anchor), true));
        b.con(ConstraintKind::Coincident, {l, PointPos::Start}, center);
        b.con(ConstraintKind::Coincident, {l, PointPos::End}, anchor);
        return l;
    };
    const Uuid r0 = radial(anchor0);
    Uuid prev = r0;
    std::optional<Uuid> firstLine;
    for (const SketchGeo& g : src)
        if (g.kind == GeoKind::Line) {
            firstLine = g.id;
            break;
        }
    for (int k = 1; k < count; ++k) {
        const double a = step * k;
        const double ca = std::cos(a), sa = std::sin(a);
        auto rot = [&](Vec2 p) {
            const Vec2 v = p - C;
            return C + Vec2{v.x * ca - v.y * sa, v.x * sa + v.y * ca};
        };
        std::map<Uuid, Uuid> remap;
        for (const SketchGeo& g : src) {
            SketchGeo n = transformed(g, rot, a, false, 0);
            remap[g.id] = n.id;
            b.geo(n);
        }
        for (auto& c : copyInternal(sk, remap, false))
            soft.push_back(c);
        for (const SketchGeo& g : src)
            if (g.kind != GeoKind::Point)
                soft.push_back(mkC(ConstraintKind::Equal, {g.id}, {remap[g.id]}));
        if (firstLine) {  // copies turn with the pattern
            auto c = mkC(ConstraintKind::Angle, {*firstLine}, {remap[*firstLine]});
            c.value = sk.measure(c);
            soft.push_back(c);
        }
        const Uuid rk = radial({remap[src[0].id], anchor0.pos});
        b.con(ConstraintKind::Equal, {r0}, {rk});
        auto ang = mkC(ConstraintKind::Angle, {prev}, {rk});
        ang.value = sk.measure(ang);
        const Uuid aid = b.con(ang);
        if (k == 1)
            res.driving = aid;
        prev = rk;
    }
    for (const Uuid& c : addIfIndependent(sk, soft))
        res.constraints.push_back(c);
    res.ok = true;
    return res;
}

}  // namespace silex::sketchtools
