// Topological Naming Problem: references must survive upstream modifications.

#include <gtest/gtest.h>

#include <algorithm>

#include "TestHelpers.h"

using namespace silex;
using namespace silex::test;

namespace {
DocState replace(DocState s, const FeaturePtr& f) { return UpdateFeatureCommand(f).apply(s); }

const FeatureResult* resultOf(const Analysis& a, const Uuid& id)
{
    const FeatureResult* r = a.result(id);
    EXPECT_NE(r, nullptr);
    return r;
}
}  // namespace

TEST(TNP, NamesAreStableAcrossRecomputes)
{
    auto rect = makeRect(20, 10);
    auto sk = sketchFeature(rect.sk);
    auto ex = extrudeFeature(sk->id, 5);
    auto a1 = KernelEngine::analyze(docOf({sk, ex}));
    auto a2 = KernelEngine::analyze(docOf({sk, ex}));
    ASSERT_EQ(a1.bodies.size(), 1u);
    auto n1 = a1.bodies[0].faceNames, n2 = a2.bodies[0].faceNames;
    std::sort(n1.begin(), n1.end());
    std::sort(n2.begin(), n2.end());
    EXPECT_EQ(n1, n2);
    // semantic names: two caps + one side per sketch line
    EXPECT_EQ(std::count_if(n1.begin(), n1.end(), [](auto& n) { return n.find(":side(") != std::string::npos; }), 4);
    EXPECT_NE(std::find(n1.begin(), n1.end(), ex->id + ":end"), n1.end());
    EXPECT_NE(std::find(n1.begin(), n1.end(), ex->id + ":start"), n1.end());
}

TEST(TNP, FilletSurvivesSketchDimensionChange)
{
    auto rect = makeRect(20, 10);
    auto sk = sketchFeature(rect.sk);
    auto ex = extrudeFeature(sk->id, 5);
    auto snap = KernelEngine::computeSnapshot(docOf({sk, ex}));
    // Fillet the vertical edge at the (20,10) corner, i.e. between the right and top sides.
    auto edge = pickEdge(*snap, {20, 10, 2.5}, {0, 0, 1});
    ASSERT_TRUE(edge.has_value());
    auto fillet = blendFeature(FeatureType::Fillet, {*edge}, 1.0);
    DocState doc = docOf({sk, ex, fillet});
    ASSERT_EQ(resultOf(KernelEngine::analyze(doc), fillet->id)->status, FeatureStatus::Ok);

    // Change the sketch width 20 -> 30: OCCT renumbers faces/edges, the name must hold.
    auto rect2 = rect;
    setDimension(rect2.sk, rect2.widthConstraint, 30);
    doc = replace(doc, sketchFeature(rect2.sk, {}, sk->id));
    auto a = KernelEngine::analyze(doc);
    const auto* r = resultOf(a, fillet->id);
    EXPECT_EQ(r->status, FeatureStatus::Ok) << r->message;  // resolved by name, not by luck
    ASSERT_EQ(a.bodies.size(), 1u);
    EXPECT_TRUE(a.bodies[0].valid);
    EXPECT_NEAR(a.bodies[0].volume, 30 * 10 * 5 - 5.0 * (1.0 - kPi / 4.0), kEps);

    // ... and the rounded edge is still the logical one (now at x = 30).
    auto snap2 = KernelEngine::computeSnapshot(doc);
    bool found = false;
    for (const auto& f : snap2->bodies[0].mesh.faces)
        if (f.name.find(fillet->id + ":fillet(") == 0) {
            found = true;
            EXPECT_GT(f.sig.center.x, 29.0);
            EXPECT_GT(f.sig.center.y, 9.0);
        }
    EXPECT_TRUE(found);
}

TEST(TNP, SketchOnFaceFollowsHeightChange)
{
    auto rect = makeRect(20, 10);
    auto sk = sketchFeature(rect.sk);
    auto ex = extrudeFeature(sk->id, 5);
    auto snap = KernelEngine::computeSnapshot(docOf({sk, ex}));
    auto top = pickFace(*snap, {10, 5, 5});
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->name, ex->id + ":end");

    // Sketch a circle on the top face and cut 2 mm deep (reversed into the material).
    Sketch hole;
    hole.geos.push_back(makeCircle({10, 5}, 2));  // local coords of the top face frame
    SketchPlaneRef plane;
    plane.kind = "Face";
    plane.face = *top;
    auto sk2 = sketchFeature(hole, plane);
    ExtrudeParams cp;
    cp.profile.sketch = sk2->id;
    cp.distance = 2;
    cp.reversed = true;
    cp.op = BooleanOp::Cut;
    auto cut = makeFeature(FeatureType::Extrude, "Cut", Json(cp));
    DocState doc = docOf({sk, ex, sk2, cut});
    auto a1 = KernelEngine::analyze(doc);
    for (const auto& r : a1.results)
        EXPECT_EQ(r.status, FeatureStatus::Ok) << r.message;
    EXPECT_NEAR(a1.bodies[0].volume, 1000.0 - kPi * 4 * 2, kEps);

    // Taller extrusion: the sketch must move up with the top face.
    doc = replace(doc, extrudeFeature(sk->id, 8, BooleanOp::NewBody, {}, ex->id));
    auto a2 = KernelEngine::analyze(doc);
    for (const auto& r : a2.results)
        EXPECT_EQ(r.status, FeatureStatus::Ok) << r.message;
    EXPECT_NEAR(a2.bodies[0].volume, 20 * 10 * 8 - kPi * 4 * 2, kEps);
    auto s2 = KernelEngine::computeSnapshot(doc);
    const SketchView* sv = s2->sketch(sk2->id);
    ASSERT_NE(sv, nullptr);
    EXPECT_NEAR(sv->plane.origin.z, 8.0, 1e-9);
}

TEST(TNP, ChamferSurvivesExtrusionChange)
{
    auto rect = makeRect(20, 10);
    auto sk = sketchFeature(rect.sk);
    auto ex = extrudeFeature(sk->id, 5);
    auto snap = KernelEngine::computeSnapshot(docOf({sk, ex}));
    // top edge along X on the front side (y = 0, z = 5)
    auto edge = pickEdge(*snap, {10, 0, 5}, {1, 0, 0});
    ASSERT_TRUE(edge.has_value());
    auto ch = blendFeature(FeatureType::Chamfer, {*edge}, 1.0);
    DocState doc = docOf({sk, ex, ch});
    doc = replace(doc, extrudeFeature(sk->id, 12, BooleanOp::NewBody, {}, ex->id));
    auto a = KernelEngine::analyze(doc);
    EXPECT_EQ(resultOf(a, ch->id)->status, FeatureStatus::Ok) << resultOf(a, ch->id)->message;
    EXPECT_NEAR(a.bodies[0].volume, 20 * 10 * 12 - 20 * 0.5, kEps);
}
