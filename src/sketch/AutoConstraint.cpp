#include "sketch/AutoConstraint.h"

#include <algorithm>

namespace silex {

namespace {
bool excluded(const std::vector<Uuid>& ex, const Uuid& id)
{
    return std::find(ex.begin(), ex.end(), id) != ex.end();
}

double distToSegment(Vec2 p, Vec2 a, Vec2 b, Vec2* foot)
{
    const Vec2 d = b - a;
    const double l2 = d.dot(d);
    double t = l2 > 1e-18 ? (p - a).dot(d) / l2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const Vec2 f = a + d * t;
    if (foot)
        *foot = f;
    return (p - f).length();
}

bool angleInArc(double ang, double a0, double a1)
{
    while (ang < a0) ang += 2 * kPi;
    while (ang > a0 + 2 * kPi) ang -= 2 * kPi;
    return ang <= a1;
}
}  // namespace

Snap snapPoint(const Sketch& sk, Vec2 raw, double tol, const std::vector<Uuid>& exclude)
{
    Snap best{raw, std::nullopt, std::nullopt};
    double bestD = tol;

    auto tryPoint = [&](const Uuid& id, PointPos pos, Vec2 p) {
        const double d = (p - raw).length();
        if (d < bestD) {
            bestD = d;
            best = {p, GeoRef{id, pos}, std::nullopt};
        }
    };
    tryPoint(kOriginId, PointPos::Start, {0, 0});
    for (const SketchGeo& g : sk.geos) {
        if (excluded(exclude, g.id))
            continue;
        switch (g.kind) {
            case GeoKind::Point: tryPoint(g.id, PointPos::Start, g.p1); break;
            case GeoKind::Line:
                tryPoint(g.id, PointPos::Start, g.p1);
                tryPoint(g.id, PointPos::End, g.p2);
                break;
            case GeoKind::Circle: tryPoint(g.id, PointPos::Center, g.c); break;
            case GeoKind::Arc:
                tryPoint(g.id, PointPos::Start, g.point(PointPos::Start));
                tryPoint(g.id, PointPos::End, g.point(PointPos::End));
                tryPoint(g.id, PointPos::Center, g.c);
                break;
        }
    }
    if (best.point)
        return best;

    // Curves (half tolerance so that points win near their ends).
    double bestC = tol * 0.6;
    for (const SketchGeo& g : sk.geos) {
        if (excluded(exclude, g.id) || g.kind == GeoKind::Point)
            continue;
        if (g.kind == GeoKind::Line) {
            Vec2 foot;
            const double d = distToSegment(raw, g.p1, g.p2, &foot);
            if (d < bestC) {
                bestC = d;
                best = {foot, std::nullopt, g.id};
            }
        }
        else {
            const Vec2 v = raw - g.c;
            const double len = v.length();
            if (len < 1e-12)
                continue;
            const double d = std::abs(len - g.r);
            if (d >= bestC)
                continue;
            if (g.kind == GeoKind::Arc && !angleInArc(std::atan2(v.y, v.x), g.a0, g.a1))
                continue;
            bestC = d;
            best = {g.c + v * (g.r / len), std::nullopt, g.id};
        }
    }
    // Axes
    if (std::abs(raw.y) < bestC && !best.curve)
        best = {{raw.x, 0}, std::nullopt, kXAxisId};
    else if (std::abs(raw.x) < bestC && !best.curve)
        best = {{0, raw.y}, std::nullopt, kYAxisId};
    return best;
}

LineHint inferLineDirection(Vec2 a, Vec2 b, double tolDeg)
{
    const Vec2 d = b - a;
    if (d.length() < 1e-9)
        return LineHint::None;
    const double ang = std::abs(std::atan2(d.y, d.x)) * 180.0 / kPi;  // [0, 180]
    if (ang < tolDeg || ang > 180.0 - tolDeg)
        return LineHint::Horizontal;
    if (std::abs(ang - 90.0) < tolDeg)
        return LineHint::Vertical;
    return LineHint::None;
}

Vec2 applyHint(Vec2 a, Vec2 b, LineHint hint)
{
    if (hint == LineHint::Horizontal)
        return {b.x, a.y};
    if (hint == LineHint::Vertical)
        return {a.x, b.y};
    return b;
}

std::optional<Uuid> inferTangentAtPoint(const Sketch& sk, const GeoRef& start, Vec2 dir, double tolDeg)
{
    const SketchGeo* g = sk.find(start.geo);
    if (!g || g->kind != GeoKind::Arc || (start.pos != PointPos::Start && start.pos != PointPos::End))
        return std::nullopt;
    const double ang = start.pos == PointPos::Start ? g->a0 : g->a1;
    // Tangent of a CCW arc at angle t is (-sin t, cos t).
    const Vec2 t{-std::sin(ang), std::cos(ang)};
    const Vec2 d = dir.normalized();
    const double cosA = std::abs(t.dot(d));
    if (cosA >= std::cos(tolDeg * kPi / 180.0))
        return g->id;
    return std::nullopt;
}

Vec2 snapToGrid(Vec2 p, double step)
{
    if (step <= 0)
        return p;
    return {std::round(p.x / step) * step, std::round(p.y / step) * step};
}

std::vector<SketchConstraint> constraintsFromSnap(const Snap& s, const GeoRef& newPoint)
{
    std::vector<SketchConstraint> out;
    if (s.point) {
        SketchConstraint c;
        c.id = newUuid();
        c.kind = ConstraintKind::Coincident;
        c.a = newPoint;
        c.b = *s.point;
        out.push_back(c);
    }
    else if (s.curve) {
        SketchConstraint c;
        c.id = newUuid();
        c.kind = ConstraintKind::PointOnObject;
        c.a = newPoint;
        c.b = GeoRef{*s.curve, PointPos::None};
        out.push_back(c);
    }
    return out;
}

}  // namespace silex
