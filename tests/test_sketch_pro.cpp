// Sketcher Pro: new constraints (midpoint, symmetry, concentric, collinear), smart primitives
// (center rectangle, slot, polygon) and 2D editing tools (trim, extend, fillet, offset, mirror,
// patterns). Every sketch is solved by PlaneGCS; profiles are checked through the B-Rep kernel.

#include <gtest/gtest.h>

#include "TestHelpers.h"
#include "sketch/SketchTools.h"

using namespace silex;
using namespace silex::test;
namespace st = silex::sketchtools;

namespace {

/// Unconstrained-size rectangle (closed H/V lines) with its lower-left corner at `c`.
std::vector<Uuid> addRect(Sketch& sk, Vec2 c, double w, double h)
{
    const Vec2 p[4] = {c, c + Vec2{w, 0}, c + Vec2{w, h}, c + Vec2{0, h}};
    std::vector<Uuid> ids;
    for (int i = 0; i < 4; ++i) {
        auto l = makeLine(p[i], p[(i + 1) % 4]);
        ids.push_back(l.id);
        sk.geos.push_back(l);
    }
    for (int i = 0; i < 4; ++i)
        sk.constraints.push_back(makeConstraint(ConstraintKind::Coincident, {ids[size_t(i)], PointPos::End},
                                                {ids[size_t((i + 1) % 4)], PointPos::Start}));
    sk.constraints.push_back(makeConstraint(ConstraintKind::Horizontal, {ids[0]}));
    sk.constraints.push_back(makeConstraint(ConstraintKind::Horizontal, {ids[2]}));
    sk.constraints.push_back(makeConstraint(ConstraintKind::Vertical, {ids[1]}));
    sk.constraints.push_back(makeConstraint(ConstraintKind::Vertical, {ids[3]}));
    return ids;
}

Uuid add(Sketch& sk, SketchConstraint c)
{
    sk.constraints.push_back(c);
    return c.id;
}

SketchConstraint sym(GeoRef a, GeoRef b, GeoRef axis)
{
    auto c = makeConstraint(ConstraintKind::Symmetric, a, b);
    c.c = axis;
    return c;
}

void expectSolved(const SolveResult& r, int dof)
{
    EXPECT_TRUE(r.converged) << r.message;
    EXPECT_TRUE(r.conflicting.empty());
    EXPECT_TRUE(r.redundant.empty());
    EXPECT_EQ(r.dof, dof);
}

const BodyMetrics& onlyBody(const Analysis& a)
{
    EXPECT_EQ(a.bodies.size(), 1u);
    return a.bodies.front();
}

Analysis extrudeSketch(const Sketch& sk, double h, size_t* profiles = nullptr)
{
    auto f = sketchFeature(sk);
    auto doc = docOf({f, extrudeFeature(f->id, h)});
    if (profiles) {
        auto snap = KernelEngine::computeSnapshot(docOf({f}));
        *profiles = snap->profiles.size();
    }
    auto a = KernelEngine::analyze(doc);
    for (const auto& r : a.results)
        EXPECT_EQ(r.status, FeatureStatus::Ok) << r.message;
    return a;
}

}  // namespace

// ================================================================== required by the lot (ctest)

TEST(SketchPro, CenterRectangleFullyConstrainedWithTwoDimensions)
{
    Sketch sk;
    auto r = st::addCenterRectangle(sk, {0, 0}, {30, 20});
    ASSERT_TRUE(r) << r.error;
    add(sk, makeConstraint(ConstraintKind::Coincident, {r.named["center"], PointPos::Start}, {kOriginId, PointPos::Start}));
    expectSolved(SketchSolver::solve(sk), 2);

    const Uuid w = add(sk, makeConstraint(ConstraintKind::Distance, {r.named["bottom"]}, {}, 60));
    add(sk, makeConstraint(ConstraintKind::Distance, {r.named["left"]}, {}, 40));
    const auto res = SketchSolver::solve(sk);
    expectSolved(res, 0);
    EXPECT_EQ(res.state, SketchDofState::FullyConstrained);
    EXPECT_NEAR(sk.find(r.named["right"])->p1.x, 30, 1e-6);
    EXPECT_NEAR(sk.find(r.named["top"])->p1.y, 20, 1e-6);

    // central symmetry: changing the width grows the rectangle on both sides
    setDimension(sk, w, 80);
    EXPECT_NEAR(sk.find(r.named["right"])->p1.x, 40, 1e-6);
    EXPECT_NEAR(sk.find(r.named["left"])->p1.x, -40, 1e-6);
    EXPECT_NEAR(sk.find(r.named["top"])->p1.y, 20, 1e-6);
}

TEST(SketchPro, SlotFullyConstrainedAndExtrudes)
{
    Sketch sk;
    auto r = st::addSlot(sk, {0, 0}, {30, 0}, 4);
    ASSERT_TRUE(r) << r.error;
    expectSolved(SketchSolver::solve(sk), 5);  // 2 centers + radius
    add(sk, makeConstraint(ConstraintKind::Coincident, {r.named["axis"], PointPos::Start}, {kOriginId, PointPos::Start}));
    add(sk, makeConstraint(ConstraintKind::Horizontal, {r.named["axis"]}));
    add(sk, makeConstraint(ConstraintKind::Distance, {r.named["axis"]}, {}, 30));
    add(sk, makeConstraint(ConstraintKind::Radius, {r.named["arc1"]}, {}, 4));
    const auto res = SketchSolver::solve(sk);
    expectSolved(res, 0);
    EXPECT_EQ(res.state, SketchDofState::FullyConstrained);

    size_t profiles = 0;
    const auto a = extrudeSketch(sk, 5, &profiles);
    EXPECT_EQ(profiles, 1u);
    const auto& b = onlyBody(a);
    EXPECT_TRUE(b.valid);
    EXPECT_EQ(b.solids, 1);
    EXPECT_NEAR(b.volume, (30 * 8 + kPi * 16) * 5, 1e-6);
}

TEST(SketchPro, SymmetricHolesFollowTheirSpacing)
{
    Sketch sk;
    auto c1 = makeCircle({-18, 9}, 2.5), c2 = makeCircle({21, 11}, 3.5);
    sk.geos = {c1, c2};
    add(sk, sym({c1.id}, {c2.id}, {kYAxisId}));
    add(sk, makeConstraint(ConstraintKind::Diameter, {c1.id}, {}, 6));
    const Uuid spacing = add(sk, makeConstraint(ConstraintKind::Distance, {c1.id, PointPos::Center}, {c2.id, PointPos::Center}, 40));
    add(sk, makeConstraint(ConstraintKind::Distance, {c1.id, PointPos::Center}, {kXAxisId}, 10));
    expectSolved(SketchSolver::solve(sk), 0);
    EXPECT_NEAR(sk.find(c1.id)->c.x, -20, 1e-6);
    EXPECT_NEAR(sk.find(c2.id)->c.x, 20, 1e-6);
    EXPECT_NEAR(sk.find(c2.id)->r, 3, 1e-6);  // symmetric circles have the same size

    setDimension(sk, spacing, 60);
    EXPECT_NEAR(sk.find(c1.id)->c.x, -30, 1e-6);
    EXPECT_NEAR(sk.find(c2.id)->c.x, 30, 1e-6);
    EXPECT_NEAR(sk.find(c1.id)->c.y, sk.find(c2.id)->c.y, 1e-6);
}

TEST(SketchPro, TrimTwoCrossingRectanglesGivesACross)
{
    Sketch sk;
    const auto h = addRect(sk, {-30, -10}, 60, 20);  // bottom, right, top, left
    const auto v = addRect(sk, {-10, -30}, 20, 60);
    ASSERT_TRUE(st::trim(sk, h[2], {0, 10}));
    ASSERT_TRUE(st::trim(sk, h[0], {0, -10}));
    ASSERT_TRUE(st::trim(sk, v[3], {-10, 0}));
    ASSERT_TRUE(st::trim(sk, v[1], {10, 0}));
    EXPECT_EQ(sk.geos.size(), 12u);

    const auto res = SketchSolver::solve(sk);
    EXPECT_TRUE(res.converged);
    EXPECT_NE(res.state, SketchDofState::OverConstrained);
    EXPECT_NE(res.state, SketchDofState::Failed);

    size_t profiles = 0;
    const auto a = extrudeSketch(sk, 5, &profiles);
    EXPECT_EQ(profiles, 1u);  // one closed cross, no inner square left
    const auto& b = onlyBody(a);
    EXPECT_TRUE(b.valid);
    EXPECT_NEAR(b.volume, (60 * 20 + 20 * 60 - 20 * 20) * 5.0, 1e-6);

    // the cross is still one connected outline when it is dragged around
    DragTarget d{{h[1], PointPos::Start}, {35, -10}, {}};
    EXPECT_TRUE(SketchSolver::solve(sk, d, false).converged);
    size_t after = 0;
    extrudeSketch(sk, 5, &after);
    EXPECT_EQ(after, 1u);
}

// ================================================================== constraints

TEST(SketchPro, MidpointOnLineAndArc)
{
    Sketch sk;
    auto l = makeLine({0, 0}, {20, 4});
    SketchGeo p;
    p.id = newUuid();
    p.kind = GeoKind::Point;
    p.p1 = {3, 7};
    sk.geos = {l, p};
    add(sk, makeConstraint(ConstraintKind::Midpoint, {p.id, PointPos::Start}, {l.id}));
    ASSERT_TRUE(SketchSolver::solve(sk).converged);
    const SketchGeo* L = sk.find(l.id);
    const Vec2 m = (L->p1 + L->p2) * 0.5;
    EXPECT_NEAR((sk.find(p.id)->p1 - m).length(), 0, 1e-8);

    // origin at the curvilinear middle of an arc
    Sketch s2;
    SketchGeo arc;
    arc.id = newUuid();
    arc.kind = GeoKind::Arc;
    arc.c = {1, -9};
    arc.r = 10;
    arc.a0 = kPi / 6;
    arc.a1 = 2 * kPi / 3;
    s2.geos = {arc};
    add(s2, makeConstraint(ConstraintKind::Midpoint, {kOriginId, PointPos::Start}, {arc.id}));
    ASSERT_TRUE(SketchSolver::solve(s2).converged);
    const SketchGeo* A = s2.find(arc.id);
    const Vec2 mid = A->c + Vec2{std::cos((A->a0 + A->a1) / 2), std::sin((A->a0 + A->a1) / 2)} * A->r;
    EXPECT_NEAR(mid.length(), 0, 1e-7);
}

TEST(SketchPro, ConcentricAndCollinear)
{
    Sketch sk;
    auto c = makeCircle({0, 0}, 5);
    SketchGeo arc;
    arc.id = newUuid();
    arc.kind = GeoKind::Arc;
    arc.c = {2, 1};
    arc.r = 8;
    arc.a0 = 0;
    arc.a1 = kPi;
    auto l1 = makeLine({0, 20}, {10, 21}), l2 = makeLine({15, 25}, {30, 26});
    sk.geos = {c, arc, l1, l2};
    add(sk, makeConstraint(ConstraintKind::Concentric, {c.id}, {arc.id}));
    add(sk, makeConstraint(ConstraintKind::Collinear, {l1.id}, {l2.id}));
    const auto res = SketchSolver::solve(sk);
    ASSERT_TRUE(res.converged);
    EXPECT_NEAR((sk.find(c.id)->c - sk.find(arc.id)->c).length(), 0, 1e-8);
    const SketchGeo *A = sk.find(l1.id), *B = sk.find(l2.id);
    const Vec2 d = (A->p2 - A->p1).normalized();
    EXPECT_NEAR(d.cross(B->p1 - A->p1), 0, 1e-8);
    EXPECT_NEAR(d.cross(B->p2 - A->p1), 0, 1e-8);
    // each removes 2 degrees of freedom: 3 + 5 + 4 + 4 - 4 (arc rules) ... checked through dof
    EXPECT_EQ(res.dof, 3 + 5 + 4 + 4 - 2 - 2);
    EXPECT_TRUE(st::checkConstraint(sk, makeConstraint(ConstraintKind::Collinear, {l1.id}, {c.id})).size() > 0);
}

TEST(SketchPro, NewConstraintsRoundTripJson)
{
    Sketch sk;
    auto l1 = makeLine({-5, 0}, {-1, 3}), l2 = makeLine({5, 0}, {1, 3});
    sk.geos = {l1, l2};
    const auto c = st::makeSymmetric(sk, {l1.id}, {l2.id}, {kYAxisId});
    sk.constraints.push_back(c);
    Json j = sk;
    Sketch back = j.get<Sketch>();
    ASSERT_EQ(back.constraints.size(), 1u);
    EXPECT_EQ(back.constraints[0].kind, ConstraintKind::Symmetric);
    EXPECT_EQ(back.constraints[0].c.geo, kYAxisId);
    EXPECT_EQ(back.constraints[0].value, c.value);
    EXPECT_EQ(std::string(toString(ConstraintKind::Collinear)), "Collinear");
    EXPECT_EQ(constraintKindFromString("Midpoint"), ConstraintKind::Midpoint);
    EXPECT_TRUE(SketchSolver::solve(back).converged);
}

// ================================================================== primitives

TEST(SketchPro, HexagonForANut)
{
    Sketch sk;
    auto r = st::addPolygon(sk, {0, 0}, {10, 0}, 6, true);
    ASSERT_TRUE(r) << r.error;
    expectSolved(SketchSolver::solve(sk), 4);  // center, size, rotation
    add(sk, makeConstraint(ConstraintKind::Coincident, {r.named["circle"], PointPos::Center}, {kOriginId, PointPos::Start}));
    add(sk, makeConstraint(ConstraintKind::Diameter, {r.named["circle"]}, {}, 20));
    add(sk, makeConstraint(ConstraintKind::Horizontal, {r.named["side1"]}));
    expectSolved(SketchSolver::solve(sk), 0);
    size_t profiles = 0;
    const auto a = extrudeSketch(sk, 2, &profiles);
    EXPECT_EQ(profiles, 1u);
    EXPECT_NEAR(onlyBody(a).volume, 1.5 * std::sqrt(3.0) * 100 * 2, 1e-6);

    Sketch s2;  // circumscribed: 13 mm across flats
    auto c = st::addPolygon(s2, {0, 0}, {6.5, 0}, 6, false);
    ASSERT_TRUE(c) << c.error;
    expectSolved(SketchSolver::solve(s2), 4);
    const SketchGeo* side = s2.find(c.named["side0"]);
    const Vec2 m = (side->p1 + side->p2) * 0.5;
    EXPECT_NEAR(m.length(), 6.5, 1e-9);
}

// ================================================================== editing tools

TEST(SketchPro, FilletKeepsTheRectangleFullyConstrained)
{
    auto r = makeRect(20, 10);
    auto f = st::filletAtPoint(r.sk, {r.lines[1], PointPos::Start}, 3);  // bottom-right corner
    ASSERT_TRUE(f) << f.error;
    const auto res = SketchSolver::solve(r.sk);
    expectSolved(res, 0);
    // the width is still driven (converted into a point - line distance)
    setDimension(r.sk, r.widthConstraint, 30);
    EXPECT_NEAR(r.sk.find(r.lines[1])->p2.x, 30, 1e-6);
    setDimension(r.sk, f.driving, 4);
    EXPECT_NEAR(r.sk.find(f.named["arc"])->r, 4, 1e-6);
    const auto a = extrudeSketch(r.sk, 5);
    EXPECT_TRUE(onlyBody(a).valid);
    EXPECT_NEAR(onlyBody(a).volume, (300 - (16 - kPi * 4)) * 5, 1e-6);
}

TEST(SketchPro, OffsetRectangleAndSlotAreDrivenByOneDimension)
{
    auto r = makeRect(20, 10);
    auto o = st::offsetCurves(r.sk, {r.lines[0], r.lines[1], r.lines[2], r.lines[3]}, 2);
    ASSERT_TRUE(o) << o.error;
    expectSolved(SketchSolver::solve(r.sk), 0);
    auto bbox = [&](const std::vector<Uuid>& ids) {
        double x0 = 1e9, x1 = -1e9;
        for (const Uuid& id : ids)
            if (const SketchGeo* g = r.sk.find(id); g && g->kind == GeoKind::Line && !g->construction) {
                x0 = std::min({x0, g->p1.x, g->p2.x});
                x1 = std::max({x1, g->p1.x, g->p2.x});
            }
        return std::pair{x0, x1};
    };
    EXPECT_NEAR(bbox(o.geos).first, -2, 1e-6);
    EXPECT_NEAR(bbox(o.geos).second, 22, 1e-6);
    setDimension(r.sk, o.driving, 3);
    EXPECT_NEAR(bbox(o.geos).first, -3, 1e-6);
    EXPECT_NEAR(bbox(o.geos).second, 23, 1e-6);

    // inward offset of a fully constrained slot (tangent joints)
    Sketch sk;
    auto s = st::addSlot(sk, {0, 0}, {30, 0}, 4);
    add(sk, makeConstraint(ConstraintKind::Coincident, {s.named["axis"], PointPos::Start}, {kOriginId, PointPos::Start}));
    add(sk, makeConstraint(ConstraintKind::Horizontal, {s.named["axis"]}));
    add(sk, makeConstraint(ConstraintKind::Distance, {s.named["axis"]}, {}, 30));
    add(sk, makeConstraint(ConstraintKind::Radius, {s.named["arc1"]}, {}, 4));
    auto so = st::offsetCurves(sk, {s.named["arc1"], s.named["side1"], s.named["arc2"], s.named["side2"]}, -1.5);
    ASSERT_TRUE(so) << so.error;
    expectSolved(SketchSolver::solve(sk), 0);
    for (const Uuid& id : so.geos)
        if (const SketchGeo* g = sk.find(id); g->kind == GeoKind::Arc)
            EXPECT_NEAR(g->r, 2.5, 1e-6);
}

TEST(SketchPro, ExtendLineToTheNextCurve)
{
    Sketch sk;
    auto a = makeLine({0, 0}, {5, 0}), wall = makeLine({10, -5}, {10, 5});
    sk.geos = {a, wall};
    ASSERT_TRUE(st::extendPreview(sk, a.id, {4.5, 0.2}).has_value());
    auto r = st::extend(sk, a.id, {4.5, 0.2});
    ASSERT_TRUE(r) << r.error;
    EXPECT_NEAR(sk.find(a.id)->p2.x, 10, 1e-9);
    ASSERT_EQ(sk.constraints.size(), 1u);
    EXPECT_EQ(sk.constraints[0].kind, ConstraintKind::PointOnObject);
    EXPECT_FALSE(st::extend(sk, a.id, {0.5, 0}));  // nothing on the other side
}

TEST(SketchPro, TrimCircleBecomesArcAndPreview)
{
    Sketch sk;
    auto c = makeCircle({0, 0}, 10);
    auto l = makeLine({-20, 0}, {20, 0});
    sk.geos = {c, l};
    auto prev = st::trimPreview(sk, c.id, {0, 10});
    ASSERT_TRUE(prev.has_value());
    EXPECT_EQ(prev->kind, GeoKind::Arc);
    ASSERT_TRUE(st::trim(sk, c.id, {0, 10}));
    const SketchGeo* g = sk.find(c.id);
    EXPECT_EQ(g->kind, GeoKind::Arc);
    EXPECT_NEAR(std::sin((g->a0 + g->a1) / 2), -1, 1e-9);  // the lower half is kept
    EXPECT_TRUE(SketchSolver::solve(sk).converged);
    // the outer pieces of the line are cut off: the diameter closes the half-disc
    ASSERT_TRUE(st::trim(sk, l.id, {-15, 0}));
    ASSERT_TRUE(st::trim(sk, l.id, {15, 0}));
    EXPECT_EQ(sk.geos.size(), 2u);
    EXPECT_NEAR(sk.find(l.id)->p1.x, -10, 1e-9);
    EXPECT_NEAR(sk.find(l.id)->p2.x, 10, 1e-9);
    const auto res = SketchSolver::solve(sk);
    EXPECT_TRUE(res.converged);
    EXPECT_TRUE(res.redundant.empty());
    size_t profiles = 0;
    extrudeSketch(sk, 1, &profiles);
    EXPECT_EQ(profiles, 1u);  // closed half-disc
}

TEST(SketchPro, MirrorFollowsTheOriginal)
{
    Sketch sk;
    auto l = makeLine({5, 0}, {10, 5});
    auto c = makeCircle({8, 12}, 2);
    sk.geos = {l, c};
    auto r = st::mirrorCurves(sk, {l.id, c.id}, {kYAxisId});
    ASSERT_TRUE(r) << r.error;
    ASSERT_EQ(r.geos.size(), 2u);
    const auto d0 = SketchSolver::solve(sk);
    EXPECT_TRUE(d0.converged);
    EXPECT_EQ(d0.dof, 4 + 3);  // the copies have no freedom of their own
    const SketchGeo* ml = sk.find(r.geos[0]);
    EXPECT_NEAR(ml->p1.x, -5, 1e-9);
    EXPECT_NEAR(ml->p2.y, 5, 1e-9);
    DragTarget d{{l.id, PointPos::End}, {14, 6}, {}};
    ASSERT_TRUE(SketchSolver::solve(sk, d, false).converged);
    EXPECT_NEAR(sk.find(r.geos[0])->p2.x, -14, 1e-4);
}

TEST(SketchPro, LinearAndCircularPatterns)
{
    Sketch sk;
    auto c = makeCircle({10, 10}, 2);
    sk.geos = {c};
    add(sk, makeConstraint(ConstraintKind::Fixed, {c.id}));
    auto r = st::linearPattern(sk, {c.id}, {1, 0}, 4, 15);
    ASSERT_TRUE(r) << r.error;
    expectSolved(SketchSolver::solve(sk), 0);
    setDimension(sk, r.driving, 20);
    const SketchGeo* last = sk.find(r.geos[r.geos.size() - 2]);  // last circle (before its pitch line)
    ASSERT_EQ(last->kind, GeoKind::Circle);
    EXPECT_NEAR(last->c.x, 70, 1e-6);
    EXPECT_NEAR(last->c.y, 10, 1e-6);
    EXPECT_NEAR(last->r, 2, 1e-6);

    Sketch s2;
    auto h = makeCircle({20, 0}, 3);
    s2.geos = {h};
    add(s2, makeConstraint(ConstraintKind::Fixed, {h.id}));
    auto cr = st::circularPattern(s2, {h.id}, {kOriginId, PointPos::Start}, 6, 2 * kPi);
    ASSERT_TRUE(cr) << cr.error;
    expectSolved(SketchSolver::solve(s2), 0);
    int n = 0;
    for (const SketchGeo& g : s2.geos)
        if (g.kind == GeoKind::Circle) {
            EXPECT_NEAR(g.c.length(), 20, 1e-6);
            EXPECT_NEAR(g.r, 3, 1e-6);
            ++n;
        }
    EXPECT_EQ(n, 6);
}
