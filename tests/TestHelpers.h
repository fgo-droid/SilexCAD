#pragma once
// Builders shared by the regression tests (they mimic what the UI produces).

#include <optional>

#include "kernel/KernelEngine.h"
#include "model/Document.h"
#include "model/Features.h"
#include "sketch/SketchSolver.h"

namespace silex::test {

constexpr double kEps = 1e-6;

inline SketchGeo makeLine(Vec2 a, Vec2 b)
{
    SketchGeo g;
    g.id = newUuid();
    g.kind = GeoKind::Line;
    g.p1 = a;
    g.p2 = b;
    return g;
}

inline SketchGeo makeCircle(Vec2 c, double r)
{
    SketchGeo g;
    g.id = newUuid();
    g.kind = GeoKind::Circle;
    g.c = c;
    g.r = r;
    return g;
}

inline SketchConstraint makeConstraint(ConstraintKind k, GeoRef a, GeoRef b = {}, double v = 0)
{
    SketchConstraint c;
    c.id = newUuid();
    c.kind = k;
    c.a = a;
    c.b = b;
    c.value = v;
    return c;
}

/// Fully constrained w x h rectangle with its lower-left corner on the origin.
/// lines: 0 bottom, 1 right, 2 top, 3 left.  constraints[widthIndex] is the width.
struct Rect
{
    Sketch sk;
    Uuid lines[4];
    Uuid widthConstraint, heightConstraint;
};

inline Rect makeRect(double w, double h)
{
    Rect r;
    const Vec2 p[4] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
    for (int i = 0; i < 4; ++i) {
        auto l = makeLine(p[i], p[(i + 1) % 4]);
        r.lines[i] = l.id;
        r.sk.geos.push_back(l);
    }
    for (int i = 0; i < 4; ++i)
        r.sk.constraints.push_back(makeConstraint(ConstraintKind::Coincident, {r.lines[i], PointPos::End},
                                                  {r.lines[(i + 1) % 4], PointPos::Start}));
    r.sk.constraints.push_back(makeConstraint(ConstraintKind::Horizontal, {r.lines[0]}));
    r.sk.constraints.push_back(makeConstraint(ConstraintKind::Horizontal, {r.lines[2]}));
    r.sk.constraints.push_back(makeConstraint(ConstraintKind::Vertical, {r.lines[1]}));
    r.sk.constraints.push_back(makeConstraint(ConstraintKind::Vertical, {r.lines[3]}));
    r.sk.constraints.push_back(
        makeConstraint(ConstraintKind::Coincident, {r.lines[0], PointPos::Start}, {kOriginId, PointPos::Start}));
    auto wc = makeConstraint(ConstraintKind::Distance, {r.lines[0]}, {}, w);
    auto hc = makeConstraint(ConstraintKind::Distance, {r.lines[3]}, {}, h);
    r.widthConstraint = wc.id;
    r.heightConstraint = hc.id;
    r.sk.constraints.push_back(wc);
    r.sk.constraints.push_back(hc);
    return r;
}

inline void setDimension(Sketch& sk, const Uuid& constraint, double value)
{
    for (auto& c : sk.constraints)
        if (c.id == constraint)
            c.value = value;
    SketchSolver::solve(sk);
}

inline FeaturePtr sketchFeature(const Sketch& sk, SketchPlaneRef plane = {}, Uuid id = {})
{
    SketchParams p;
    p.plane = plane;
    p.sketch = sk;
    return makeFeature(FeatureType::Sketch, "Sketch", Json(p), id);
}

inline FeaturePtr extrudeFeature(const Uuid& sketch, double d, BooleanOp op = BooleanOp::NewBody,
                                 std::vector<std::string> regions = {}, Uuid id = {})
{
    ExtrudeParams p;
    p.profile.sketch = sketch;
    p.profile.regions = std::move(regions);
    p.distance = d;
    p.op = op;
    return makeFeature(FeatureType::Extrude, "Extrude", Json(p), id);
}

inline FeaturePtr blendFeature(const char* type, std::vector<TopoRef> edges, double v, Uuid id = {})
{
    EdgeBlendParams p;
    p.edges = std::move(edges);
    p.value = v;
    return makeFeature(type, type, Json(p), id);
}

inline DocState docOf(std::vector<FeaturePtr> fs)
{
    DocState s;
    s.features = std::move(fs);
    s.marker = static_cast<int>(s.features.size());
    return s;
}

/// Picks an edge in the display mesh the way the UI does: nearest mid-point.
inline std::optional<TopoRef> pickEdge(const ModelSnapshot& snap, Vec3 near, Vec3 dir = {})
{
    std::optional<TopoRef> best;
    double bestD = 1e100;
    for (const auto& b : snap.bodies)
        for (const auto& e : b.mesh.edges) {
            if (dir.length() > 0 && std::abs(e.sig.dir.dot(dir)) < 0.99)
                continue;
            const double d = (e.sig.center - near).length();
            if (d < bestD) {
                bestD = d;
                best = TopoRef{e.name, e.sig};
            }
        }
    return best;
}

inline std::optional<TopoRef> pickFace(const ModelSnapshot& snap, Vec3 near)
{
    std::optional<TopoRef> best;
    double bestD = 1e100;
    for (const auto& b : snap.bodies)
        for (const auto& f : b.mesh.faces) {
            const double d = (f.sig.center - near).length();
            if (d < bestD) {
                bestD = d;
                best = TopoRef{f.name, f.sig};
            }
        }
    return best;
}

}  // namespace silex::test
