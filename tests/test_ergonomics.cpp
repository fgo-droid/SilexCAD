// Ergonomics lot: Press / Pull on faces (OffsetFace) and quick primitives (Box / Cylinder).
// Validity (BRepCheck_Analyzer), exact volumes and timeline editability.

#include <gtest/gtest.h>

#include "TestHelpers.h"

using namespace silex;
using namespace silex::test;

namespace {

FeaturePtr pressPull(const TopoRef& face, double d)
{
    OffsetFaceParams p;
    p.face = face;
    p.distance = d;
    return makeFeature(FeatureType::OffsetFace, "PressPull", Json(p));
}

FeaturePtr primitive(const char* type, PrimitiveParams p) { return makeFeature(type, type, Json(p)); }

void expectAllOk(const Analysis& a)
{
    for (const auto& r : a.results)
        EXPECT_EQ(r.status, FeatureStatus::Ok) << r.message;
}

}  // namespace

TEST(Ergonomics, PressPullTopThenSide)
{
    auto rect = makeRect(40, 40);
    auto sk = sketchFeature(rect.sk);
    auto ex = extrudeFeature(sk->id, 10);
    DocState d = docOf({sk, ex});

    // +5 mm on the top face
    auto top = pickFace(*KernelEngine::computeSnapshot(d), {20, 20, 10});
    ASSERT_TRUE(top.has_value());
    auto pp1 = pressPull(*top, 5);
    d = docOf({sk, ex, pp1});
    auto a1 = KernelEngine::analyze(d);
    expectAllOk(a1);
    ASSERT_EQ(a1.bodies.size(), 1u);
    EXPECT_NEAR(a1.bodies[0].volume, 40 * 40 * 15.0, 1e-6);
    EXPECT_EQ(a1.bodies[0].faces, 6);  // the side walls are merged with the block's

    // -3 mm on the side face x = 40 (now 15 mm high)
    auto side = pickFace(*KernelEngine::computeSnapshot(d), {40, 20, 7.5});
    ASSERT_TRUE(side.has_value());
    auto pp2 = pressPull(*side, -3);
    d = docOf({sk, ex, pp1, pp2});
    auto a2 = KernelEngine::analyze(d);
    expectAllOk(a2);
    ASSERT_EQ(a2.bodies.size(), 1u);
    EXPECT_TRUE(a2.bodies[0].valid);
    EXPECT_NEAR(a2.bodies[0].volume, (40 - 3) * 40 * (10 + 5.0), 1e-6);  // 22 200 mm3

    // the distances stay editable in the timeline (persistent references survive the change)
    auto p1 = pp1->as<OffsetFaceParams>();
    p1.distance = 8;
    auto edited = std::make_shared<FeatureNode>(*pp1);
    edited->params = Json(p1);
    d = UpdateFeatureCommand(edited).apply(d);
    auto a3 = KernelEngine::analyze(d);
    expectAllOk(a3);
    EXPECT_NEAR(a3.bodies[0].volume, 37 * 40 * 18.0, 1e-6);
    EXPECT_NEAR(a3.bodies[0].bbox.max.x, 37, 1e-6);
}

TEST(Ergonomics, PressPullRoundTripJson)
{
    OffsetFaceParams p;
    p.face.name = "abc:end";
    p.distance = -2.5;
    const OffsetFaceParams q = Json(p).get<OffsetFaceParams>();
    EXPECT_EQ(q.face.name, "abc:end");
    EXPECT_EQ(q.distance, -2.5);
}

TEST(Ergonomics, BoxAndCylinderCut)
{
    PrimitiveParams b;
    b.length = 40;
    b.width = 30;
    b.height = 20;
    auto box = primitive(FeatureType::Box, b);

    PrimitiveParams c;
    c.diameter = 10;
    c.height = 20;
    c.centerX = 5;
    c.op = BooleanOp::Cut;
    auto cyl = primitive(FeatureType::Cylinder, c);

    auto a = KernelEngine::analyze(docOf({box, cyl}));
    expectAllOk(a);
    ASSERT_EQ(a.bodies.size(), 1u);
    EXPECT_TRUE(a.bodies[0].valid);
    EXPECT_NEAR(a.bodies[0].volume, 40 * 30 * 20 - kPi * 25 * 20, 1e-6);
    EXPECT_NEAR(a.bodies[0].bbox.min.x, -20, 1e-6);
    EXPECT_NEAR(a.bodies[0].bbox.max.y, 15, 1e-6);
    EXPECT_NEAR(a.bodies[0].bbox.max.z, 20, 1e-6);
    // deterministic face names
    EXPECT_NE(std::find(a.bodies[0].faceNames.begin(), a.bodies[0].faceNames.end(), box->id + ":top"),
              a.bodies[0].faceNames.end());
}

TEST(Ergonomics, CylinderOnAFaceAndNegativeHeight)
{
    PrimitiveParams b;
    b.length = b.width = 30;
    b.height = 10;
    auto box = primitive(FeatureType::Box, b);
    DocState d = docOf({box});
    auto top = pickFace(*KernelEngine::computeSnapshot(d), {0, 0, 10});
    ASSERT_TRUE(top.has_value());

    PrimitiveParams c;  // boss on the top face, 4 mm right of its center
    c.plane.kind = "Face";
    c.plane.face = *top;
    c.centerX = 4;
    c.diameter = 12;
    c.height = 8;
    c.op = BooleanOp::Join;
    auto boss = primitive(FeatureType::Cylinder, c);

    PrimitiveParams h;  // pocket from the bottom plane, upwards into the box: negative = below XY
    h.plane.kind = "XY";
    h.length = h.width = 6;
    h.centerX = 10;
    h.height = -5;
    h.op = BooleanOp::Join;
    auto foot = primitive(FeatureType::Box, h);

    auto a = KernelEngine::analyze(docOf({box, boss, foot}));
    expectAllOk(a);
    ASSERT_EQ(a.bodies.size(), 1u);
    EXPECT_TRUE(a.bodies[0].valid);
    EXPECT_EQ(a.bodies[0].solids, 1);  // the boss and the foot are really attached to the box
    EXPECT_NEAR(a.bodies[0].bbox.max.z, 18, 1e-6);
    EXPECT_NEAR(a.bodies[0].bbox.min.z, -5, 1e-6);
    // boss position: the top-most solid slice is centered at x = 4 (face centroid + centerX)
    const auto snap = KernelEngine::computeSnapshot(docOf({box, boss, foot}));
    auto bossTop = pickFace(*snap, {4, 0, 18});
    ASSERT_TRUE(bossTop.has_value());
    EXPECT_NEAR(bossTop->sig.center.x, 4, 1e-6);
    EXPECT_NEAR(bossTop->sig.center.z, 18, 1e-6);
    EXPECT_NEAR(a.bodies[0].volume, 30 * 30 * 10 + kPi * 36 * 8 + 6 * 6 * 5, 1e-6);
}
