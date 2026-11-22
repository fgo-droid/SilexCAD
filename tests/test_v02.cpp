// Silex v0.2 non-regression tests: Shell, Mirror (+TNP), Linear / Circular Pattern (+TNP),
// document schema 0.2 with 0.1 backward compatibility.

#include <gtest/gtest.h>

#include <algorithm>

#include "TestHelpers.h"

using namespace silex;
using namespace silex::test;

namespace {

FeaturePtr circleSketch(Vec2 c, double r, Uuid id = {})
{
    Sketch s;
    s.geos.push_back(makeCircle(c, r));
    return sketchFeature(s, {}, id);
}

FeaturePtr feature(const char* type, const Json& params, Uuid id = {})
{
    return makeFeature(type, type, params, id);
}

void expectAllOk(const Analysis& a)
{
    for (const auto& r : a.results)
        EXPECT_EQ(r.status, FeatureStatus::Ok) << r.id << ": " << r.message;
}

const BodyMetrics& single(const Analysis& a)
{
    EXPECT_EQ(a.bodies.size(), 1u);
    return a.bodies.front();
}

DocState with(DocState s, const FeaturePtr& f) { return UpdateFeatureCommand(f).apply(s); }

}  // namespace

// ------------------------------------------------------------------ Shell

TEST(V02, ShellOpenTopBlock)
{
    auto rect = makeRect(80, 50);
    auto sk = sketchFeature(rect.sk);
    auto ex = extrudeFeature(sk->id, 30);
    auto snap = KernelEngine::computeSnapshot(docOf({sk, ex}));
    auto top = pickFace(*snap, {40, 25, 30});
    ASSERT_TRUE(top.has_value());

    ShellParams sp;
    sp.removedFaces = {*top};
    sp.thickness = 2;
    auto shell = feature(FeatureType::Shell, Json(sp));
    const Analysis a = KernelEngine::analyze(docOf({sk, ex, shell}));
    expectAllOk(a);
    EXPECT_TRUE(single(a).valid);
    EXPECT_NEAR(single(a).volume, 22112.0, kEps);  // 80*50*30 - 76*46*28

    sp.outward = true;  // walls grow outside: the cavity is the original block
    auto shellOut = feature(FeatureType::Shell, Json(sp));
    const Analysis b = KernelEngine::analyze(docOf({sk, ex, shellOut}));
    expectAllOk(b);
    EXPECT_TRUE(single(b).valid);
    EXPECT_GT(single(b).volume, 22112.0);
    EXPECT_NEAR(single(b).bbox.max.z, 30.0, 1e-6);  // still open at the top
    EXPECT_NEAR(single(b).bbox.min.z, -2.0, 1e-6);
}

// ------------------------------------------------------------------ Mirror + TNP

TEST(V02, MirrorKeepsFilletsWhenDimensionChanges)
{
    // Half body 40 x 30 x 10 with a hole and a fillet, mirrored across YZ, filleted again.
    auto rect = makeRect(40, 30);
    auto sk = sketchFeature(rect.sk);
    auto ex = extrudeFeature(sk->id, 10);
    auto hs = circleSketch({20, 15}, 3);
    auto hole = extrudeFeature(hs->id, 10, BooleanOp::Cut);
    auto s0 = KernelEngine::computeSnapshot(docOf({sk, ex, hs, hole}));
    auto e1 = pickEdge(*s0, {40, 30, 5}, {0, 0, 1});
    ASSERT_TRUE(e1);
    auto f1 = blendFeature(FeatureType::Fillet, {*e1}, 3);

    MirrorParams mp;
    mp.targets = {"body", {ex->id}};
    mp.plane = {"YZ", {}};
    mp.merge = true;
    auto mirror = feature(FeatureType::Mirror, Json(mp));
    DocState doc = docOf({sk, ex, hs, hole, f1, mirror});
    {
        const Analysis a = KernelEngine::analyze(doc);
        expectAllOk(a);
        EXPECT_EQ(single(a).solids, 1);  // touching halves were merged
    }
    // post-mirror fillet on a corner that only exists in the mirrored half
    auto s1 = KernelEngine::computeSnapshot(doc);
    auto e2 = pickEdge(*s1, {-40, 0, 5}, {0, 0, 1});
    ASSERT_TRUE(e2);
    EXPECT_NE(e2->name.find("mirror("), std::string::npos) << e2->name;
    auto f2 = blendFeature(FeatureType::Fillet, {*e2}, 3);
    doc = docOf({sk, ex, hs, hole, f1, mirror, f2});

    auto corner = [](double r, double h) { return (r * r - kPi * r * r / 4) * h; };
    const double hole2 = 2 * kPi * 9 * 10;
    {
        const Analysis a = KernelEngine::analyze(doc);
        expectAllOk(a);
        EXPECT_TRUE(single(a).valid);
        EXPECT_NEAR(single(a).volume, 2 * 40 * 30 * 10 - hole2 - 3 * corner(3, 10), kEps);
    }

    // Change the initial width 40 -> 50: everything rebuilds by name, fillets survive.
    auto rect2 = rect;
    setDimension(rect2.sk, rect2.widthConstraint, 50);
    doc = with(doc, sketchFeature(rect2.sk, {}, sk->id));
    const Analysis a = KernelEngine::analyze(doc);
    expectAllOk(a);  // Ok, not Warning: resolved by name
    EXPECT_TRUE(single(a).valid);
    EXPECT_NEAR(single(a).volume, 2 * 50 * 30 * 10 - hole2 - 3 * corner(3, 10), kEps);
    EXPECT_NEAR(single(a).bbox.min.x, -50.0, 1e-6);
}

TEST(V02, MirrorSeparateBodyWhenNotTouching)
{
    Sketch s;
    s.geos.push_back(makeLine({10, 0}, {30, 0}));
    s.geos.push_back(makeLine({30, 0}, {30, 10}));
    s.geos.push_back(makeLine({30, 10}, {10, 10}));
    s.geos.push_back(makeLine({10, 10}, {10, 0}));
    auto sk = sketchFeature(s);
    auto ex = extrudeFeature(sk->id, 5);
    MirrorParams mp;
    mp.targets = {"body", {ex->id}};
    mp.plane = {"YZ", {}};
    const Analysis a = KernelEngine::analyze(docOf({sk, ex, feature(FeatureType::Mirror, Json(mp))}));
    expectAllOk(a);
    ASSERT_EQ(a.bodies.size(), 2u);
    EXPECT_NEAR(a.bodies[1].volume, 1000.0, kEps);
    EXPECT_NEAR(a.bodies[1].bbox.max.x, -10.0, 1e-6);
}

// ------------------------------------------------------------------ Linear pattern

TEST(V02, LinearPatternOfHole)
{
    auto rect = makeRect(100, 20);
    auto sk = sketchFeature(rect.sk);
    auto ex = extrudeFeature(sk->id, 5);
    auto hs = circleSketch({10, 10}, 2);
    auto hole = extrudeFeature(hs->id, 5, BooleanOp::Cut);
    LinearPatternParams lp;
    lp.targets = {"feature", {hole->id}};
    lp.dir1 = {{"X", {}}, 5, 20.0, false, false};
    auto pat = feature(FeatureType::LinearPattern, Json(lp));
    const Analysis a = KernelEngine::analyze(docOf({sk, ex, hs, hole, pat}));
    expectAllOk(a);
    EXPECT_TRUE(single(a).valid);
    EXPECT_NEAR(single(a).volume, 100 * 20 * 5 - 5 * kPi * 4 * 5, kEps);

    lp.dir2 = {{"Y", {}}, 2, 5.0, true, false};  // second row 5 mm lower... reversed => y = 5
    auto pat2 = feature(FeatureType::LinearPattern, Json(lp), pat->id);
    const Analysis b = KernelEngine::analyze(docOf({sk, ex, hs, hole, pat2}));
    expectAllOk(b);
    EXPECT_NEAR(single(b).volume, 100 * 20 * 5 - 10 * kPi * 4 * 5, kEps);
}

// ------------------------------------------------------------------ Circular pattern + TNP

TEST(V02, CircularPatternChamfersSurviveCountChange)
{
    auto disk = circleSketch({0, 0}, 40);
    auto ex = extrudeFeature(disk->id, 8);
    auto hs = circleSketch({30, 0}, 3);
    auto hole = extrudeFeature(hs->id, 8, BooleanOp::Cut);
    CircularPatternParams cp;
    cp.targets = {"feature", {hole->id}};
    cp.axis = {"Z", {}};
    cp.count = 6;
    auto pat = feature(FeatureType::CircularPattern, Json(cp));

    const DocState base6 = docOf({disk, ex, hs, hole, pat});
    const Analysis a6 = KernelEngine::analyze(base6);
    expectAllOk(a6);
    EXPECT_NEAR(single(a6).volume, kPi * 1600 * 8 - 6 * kPi * 9 * 8, kEps);

    // chamfer the top edge of the 6 holes
    auto snap = KernelEngine::computeSnapshot(base6);
    std::vector<TopoRef> tops;
    for (const auto& e : snap->bodies[0].mesh.edges) {
        const double rc = std::hypot(e.sig.center.x, e.sig.center.y);
        if (e.sig.type == 1 && std::abs(e.sig.center.z - 8) < 1e-6 && std::abs(rc - 30) < 1e-6)
            tops.push_back({e.name, e.sig});
    }
    ASSERT_EQ(tops.size(), 6u);
    for (const auto& t : tops)
        EXPECT_TRUE(t.name.find("pattern_circ(") != std::string::npos || t.name.find(hole->id) != std::string::npos);
    auto ch = blendFeature(FeatureType::Chamfer, tops, 0.5);
    const Analysis c6 = KernelEngine::analyze(docOf({disk, ex, hs, hole, pat, ch}));
    expectAllOk(c6);
    const double perChamfer = (single(a6).volume - single(c6).volume) / 6.0;
    EXPECT_GT(perChamfer, 0.0);

    // 6 -> 8 occurrences in the timeline
    cp.count = 8;
    auto pat8 = feature(FeatureType::CircularPattern, Json(cp), pat->id);
    const Analysis a8 = KernelEngine::analyze(docOf({disk, ex, hs, hole, pat8}));
    const DocState doc8 = docOf({disk, ex, hs, hole, pat8, ch});
    const Analysis c8 = KernelEngine::analyze(doc8);
    expectAllOk(c8);  // the chamfer resolves its 6 edges by name (Ok, not Warning)
    EXPECT_TRUE(single(c8).valid);
    EXPECT_NEAR(single(a8).volume, kPi * 1600 * 8 - 8 * kPi * 9 * 8, kEps);
    EXPECT_NEAR(single(c8).volume, single(a8).volume - 6 * perChamfer, 1e-6);
    const auto& names = single(c8).faceNames;
    EXPECT_EQ(std::count_if(names.begin(), names.end(), [&](const std::string& n) {
                  return n.find(ch->id + ":chamfer(") == 0;
              }),
              6);
}

// ------------------------------------------------------------------ Schema 0.2

TEST(V02, SchemaVersionAndBackwardCompatibility)
{
    auto rect = makeRect(20, 10);
    auto sk = sketchFeature(rect.sk);
    const Json j = docToJson(docOf({sk, extrudeFeature(sk->id, 5)}));
    EXPECT_EQ(j.at("schema_version"), kSchemaVersion);  // "0.3" since v0.3

    Json old = j;  // a v0.1 file: no schema_version, "version": 1
    old.erase("schema_version");
    old["version"] = 1;
    const DocState back = docFromJson(old);
    EXPECT_EQ(back.features.size(), 2u);
    EXPECT_NEAR(single(KernelEngine::analyze(back)).volume, 1000.0, kEps);

    Json future = j;
    future["schema_version"] = "0.9";
    EXPECT_THROW(docFromJson(future), std::runtime_error);
}
