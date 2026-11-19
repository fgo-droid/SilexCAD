#include <gtest/gtest.h>

#include "TestHelpers.h"
#include "sketch/AutoConstraint.h"

using namespace silex;
using namespace silex::test;

TEST(SketchSolver, FullyConstrainedRectangle)
{
    auto r = makeRect(20, 10);
    auto res = SketchSolver::solve(r.sk);
    EXPECT_TRUE(res.converged);
    EXPECT_EQ(res.dof, 0);
    EXPECT_EQ(res.state, SketchDofState::FullyConstrained);
    EXPECT_EQ(res.fullyConstrainedGeos.size(), 4u);
}

TEST(SketchSolver, DimensionDrivesGeometry)
{
    auto r = makeRect(20, 10);
    setDimension(r.sk, r.widthConstraint, 35);
    const SketchGeo* right = r.sk.find(r.lines[1]);
    ASSERT_NE(right, nullptr);
    EXPECT_NEAR(right->p1.x, 35, kEps);
    EXPECT_NEAR(right->p2.x, 35, kEps);
    EXPECT_NEAR(r.sk.find(r.lines[2])->p1.y, 10, kEps);
}

TEST(SketchSolver, UnderConstrainedIsBlue)
{
    auto r = makeRect(20, 10);
    r.sk.removeConstraint(r.widthConstraint);
    auto res = SketchSolver::solve(r.sk);
    EXPECT_EQ(res.dof, 1);
    EXPECT_EQ(res.state, SketchDofState::UnderConstrained);
    // the left edge stays fully constrained, the right one can move
    EXPECT_TRUE(res.fullyConstrainedGeos.count(r.lines[3]));
    EXPECT_FALSE(res.fullyConstrainedGeos.count(r.lines[1]));
}

TEST(SketchSolver, OverConstrainedIsRed)
{
    auto r = makeRect(20, 10);
    r.sk.constraints.push_back(makeConstraint(ConstraintKind::Distance, {r.lines[2]}, {}, 25));
    auto res = SketchSolver::solve(r.sk);
    EXPECT_EQ(res.state == SketchDofState::OverConstrained || res.state == SketchDofState::Failed, true);
    EXPECT_FALSE(res.conflicting.empty() && res.redundant.empty() && res.converged);
}

TEST(SketchSolver, DragKeepsConstraints)
{
    auto r = makeRect(20, 10);
    r.sk.removeConstraint(r.widthConstraint);
    DragTarget d{{r.lines[1], PointPos::Start}, {30, 3}, {}};
    auto res = SketchSolver::solve(r.sk, d, false);
    EXPECT_TRUE(res.converged);
    const SketchGeo* right = r.sk.find(r.lines[1]);
    EXPECT_NEAR(right->p1.x, 30, 1e-4);  // free direction follows the cursor
    EXPECT_NEAR(right->p1.y, 0, 1e-6);   // constrained direction does not
}

TEST(AutoConstraint, Inference)
{
    EXPECT_EQ(inferLineDirection({0, 0}, {10, 0.2}), LineHint::Horizontal);
    EXPECT_EQ(inferLineDirection({0, 0}, {-0.1, 10}), LineHint::Vertical);
    EXPECT_EQ(inferLineDirection({0, 0}, {10, 10}), LineHint::None);

    Sketch sk;
    auto l = makeLine({0, 0}, {10, 0});
    sk.geos.push_back(l);
    auto s = snapPoint(sk, {10.2, 0.1}, 0.5);
    ASSERT_TRUE(s.point.has_value());
    EXPECT_EQ(s.point->geo, l.id);
    EXPECT_EQ(s.point->pos, PointPos::End);
    auto c = snapPoint(sk, {5, 0.1}, 0.5);
    EXPECT_TRUE(c.curve.has_value());
}
