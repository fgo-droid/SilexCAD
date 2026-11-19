// Geometric non-regression tests: topological validity (BRepCheck_Analyzer) and exact
// volumes (epsilon = 1e-6) after each B-Rep operation.

#include <gtest/gtest.h>

#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>

#include "TestHelpers.h"

using namespace silex;
using namespace silex::test;

namespace {
const BodyMetrics& onlyBody(const Analysis& a)
{
    EXPECT_EQ(a.bodies.size(), 1u);
    return a.bodies.front();
}
void expectAllOk(const Analysis& a)
{
    for (const auto& r : a.results)
        EXPECT_EQ(r.status, FeatureStatus::Ok) << r.message;
}
}  // namespace

TEST(Kernel, ExtrudeBox)
{
    auto rect = makeRect(20, 10);
    auto sk = sketchFeature(rect.sk);
    auto a = KernelEngine::analyze(docOf({sk, extrudeFeature(sk->id, 5)}));
    expectAllOk(a);
    const auto& b = onlyBody(a);
    EXPECT_TRUE(b.valid);
    EXPECT_EQ(b.solids, 1);
    EXPECT_EQ(b.faces, 6);
    EXPECT_EQ(b.edges, 12);
    EXPECT_NEAR(b.volume, 1000.0, kEps);
    EXPECT_NEAR(b.area, 2 * (200 + 100 + 50), kEps);
}

TEST(Kernel, SymmetricAndReversedExtrude)
{
    auto rect = makeRect(20, 10);
    auto sk = sketchFeature(rect.sk);
    ExtrudeParams p;
    p.profile.sketch = sk->id;
    p.distance = 6;
    p.symmetric = true;
    auto ex = makeFeature(FeatureType::Extrude, "Extrude", Json(p));
    auto snap = KernelEngine::computeSnapshot(docOf({sk, ex}));
    ASSERT_EQ(snap->bodies.size(), 1u);
    EXPECT_NEAR(snap->bodies[0].mesh.bbox.min.z, -3.0, 1e-3);
    EXPECT_NEAR(snap->bodies[0].mesh.bbox.max.z, 3.0, 1e-3);
}

TEST(Kernel, CutHoleThrough)
{
    auto rect = makeRect(20, 10);
    auto sk1 = sketchFeature(rect.sk);
    auto ex1 = extrudeFeature(sk1->id, 5);
    Sketch hole;
    hole.geos.push_back(makeCircle({10, 5}, 2));
    auto sk2 = sketchFeature(hole);
    ExtrudeParams p;
    p.profile.sketch = sk2->id;
    p.distance = 5;
    p.op = BooleanOp::Cut;
    auto cut = makeFeature(FeatureType::Extrude, "Cut", Json(p));
    auto a = KernelEngine::analyze(docOf({sk1, ex1, sk2, cut}));
    expectAllOk(a);
    const auto& b = onlyBody(a);
    EXPECT_TRUE(b.valid);
    EXPECT_NEAR(b.volume, 1000.0 - kPi * 4.0 * 5.0, kEps);
}

TEST(Kernel, SketchWithHoleGivesTwoRegions)
{
    auto rect = makeRect(20, 10);
    rect.sk.geos.push_back(makeCircle({10, 5}, 2));
    auto sk = sketchFeature(rect.sk);
    auto snap = KernelEngine::computeSnapshot(docOf({sk}));
    ASSERT_EQ(snap->profiles.size(), 2u);
    // Extruding only the outer (ring) region gives a plate with a hole.
    std::string ring;
    for (const auto& pv : snap->profiles)
        if (pv.regionKey.find(',') != std::string::npos)
            ring = pv.regionKey;
    ASSERT_FALSE(ring.empty());
    auto a = KernelEngine::analyze(docOf({sk, extrudeFeature(sk->id, 5, BooleanOp::NewBody, {ring})}));
    EXPECT_NEAR(onlyBody(a).volume, 1000.0 - kPi * 4.0 * 5.0, kEps);
}

TEST(Kernel, RevolveRing)
{
    Sketch s;
    // rectangle x in [5,10], y in [0,4] revolved around the sketch Y axis
    const Vec2 p[4] = {{5, 0}, {10, 0}, {10, 4}, {5, 4}};
    for (int i = 0; i < 4; ++i)
        s.geos.push_back(makeLine(p[i], p[(i + 1) % 4]));
    auto sk = sketchFeature(s);
    RevolveParams rp;
    rp.profile.sketch = sk->id;
    rp.axis = "Y";
    rp.angle = 360;
    auto rev = makeFeature(FeatureType::Revolve, "Revolve", Json(rp));
    auto a = KernelEngine::analyze(docOf({sk, rev}));
    expectAllOk(a);
    const auto& b = onlyBody(a);
    EXPECT_TRUE(b.valid);
    EXPECT_NEAR(b.volume, kPi * (100.0 - 25.0) * 4.0, 1e-6 * b.volume);

    rp.angle = 90;
    auto quarter = makeFeature(FeatureType::Revolve, "Revolve", Json(rp));
    auto q = KernelEngine::analyze(docOf({sk, quarter}));
    EXPECT_NEAR(onlyBody(q).volume, kPi * 75.0, 1e-6 * kPi * 75.0);
}

TEST(Kernel, FilletAndChamferVolumes)
{
    auto rect = makeRect(20, 10);
    auto sk = sketchFeature(rect.sk);
    auto ex = extrudeFeature(sk->id, 5);
    auto snap = KernelEngine::computeSnapshot(docOf({sk, ex}));
    auto edge = pickEdge(*snap, {20, 10, 2.5}, {0, 0, 1});
    ASSERT_TRUE(edge.has_value());

    auto fa = KernelEngine::analyze(docOf({sk, ex, blendFeature(FeatureType::Fillet, {*edge}, 1.0)}));
    expectAllOk(fa);
    EXPECT_TRUE(onlyBody(fa).valid);
    EXPECT_NEAR(onlyBody(fa).volume, 1000.0 - 5.0 * (1.0 - kPi / 4.0), kEps);

    auto ca = KernelEngine::analyze(docOf({sk, ex, blendFeature(FeatureType::Chamfer, {*edge}, 1.0)}));
    expectAllOk(ca);
    EXPECT_TRUE(onlyBody(ca).valid);
    EXPECT_NEAR(onlyBody(ca).volume, 1000.0 - 5.0 * 0.5, kEps);
}

TEST(Kernel, FailingFeatureKeepsLastValidGeometry)
{
    auto rect = makeRect(20, 10);
    auto sk = sketchFeature(rect.sk);
    auto ex = extrudeFeature(sk->id, 5);
    auto snap = KernelEngine::computeSnapshot(docOf({sk, ex}));
    auto edge = pickEdge(*snap, {20, 10, 2.5}, {0, 0, 1});
    ASSERT_TRUE(edge.has_value());
    auto bad = blendFeature(FeatureType::Fillet, {*edge}, 50.0);  // far too large
    auto a = KernelEngine::analyze(docOf({sk, ex, bad}));
    ASSERT_NE(a.result(bad->id), nullptr);
    EXPECT_EQ(a.result(bad->id)->status, FeatureStatus::Error);
    EXPECT_FALSE(a.result(bad->id)->message.empty());
    EXPECT_NEAR(onlyBody(a).volume, 1000.0, kEps);
}

TEST(Kernel, RollbackMarkerAndSuppression)
{
    auto rect = makeRect(20, 10);
    auto sk = sketchFeature(rect.sk);
    auto ex = extrudeFeature(sk->id, 5);
    DocState s = docOf({sk, ex});
    s.marker = 1;
    auto a = KernelEngine::analyze(s);
    EXPECT_TRUE(a.bodies.empty());
    EXPECT_EQ(a.result(ex->id)->status, FeatureStatus::RolledBack);

    DocState t = SetSuppressedCommand(ex->id, true).apply(docOf({sk, ex}));
    auto b = KernelEngine::analyze(t);
    EXPECT_TRUE(b.bodies.empty());
    EXPECT_EQ(b.result(ex->id)->status, FeatureStatus::Suppressed);
}

TEST(Kernel, StepRoundTrip)
{
    auto rect = makeRect(20, 10);
    auto sk = sketchFeature(rect.sk);
    DocState s = docOf({sk, extrudeFeature(sk->id, 5)});
    const auto path = (std::filesystem::temp_directory_path() / "silex_roundtrip.step").string();
    auto err = KernelEngine::exportStep(s, path);
    ASSERT_FALSE(err.has_value()) << *err;

    ImportParams ip;
    ip.sourcePath = path;
    ip.brep = KernelEngine::importStepAsBrep(path);
    auto imp = makeFeature(FeatureType::ImportStep, "Imported", Json(ip));
    auto a = KernelEngine::analyze(docOf({imp}));
    expectAllOk(a);
    EXPECT_TRUE(onlyBody(a).valid);
    EXPECT_NEAR(onlyBody(a).volume, 1000.0, 1e-4);
    std::remove(path.c_str());
}

TEST(Kernel, SerializedDocumentRecomputesIdentically)
{
    auto rect = makeRect(20, 10);
    auto sk = sketchFeature(rect.sk);
    DocState s = docOf({sk, extrudeFeature(sk->id, 5)});
    DocState back = docFromJson(Json::parse(docToJson(s).dump()));
    EXPECT_NEAR(onlyBody(KernelEngine::analyze(back)).volume, 1000.0, kEps);
}

TEST(Kernel, AsyncEngineDeliversLatestRevision)
{
    std::mutex m;
    std::condition_variable cv;
    std::shared_ptr<const ModelSnapshot> last;
    KernelEngine engine([&](std::shared_ptr<const ModelSnapshot> s) {
        std::lock_guard lk(m);
        last = std::move(s);
        cv.notify_all();
    });
    auto rect = makeRect(20, 10);
    auto sk = sketchFeature(rect.sk);
    for (int i = 1; i <= 10; ++i)  // a burst of edits: stale ones are cancelled
        engine.submit(docOf({sk, extrudeFeature(sk->id, i)}), {}, static_cast<uint64_t>(i));
    std::unique_lock lk(m);
    ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(30), [&] { return last && last->revision == 10; }));
    ASSERT_EQ(last->bodies.size(), 1u);
    EXPECT_NEAR(last->bodies[0].mesh.bbox.max.z, 10.0, 1e-3);
}

// ------------------------------------------------------------------ Combine (body booleans)

namespace {
Sketch boxSketch(double x0, double y0, double w, double h)
{
    Sketch s;
    const Vec2 p[4] = {{x0, y0}, {x0 + w, y0}, {x0 + w, y0 + h}, {x0, y0 + h}};
    for (int i = 0; i < 4; ++i)
        s.geos.push_back(makeLine(p[i], p[(i + 1) % 4]));
    return s;
}

struct TwoBoxes
{
    FeaturePtr skA, exA, skB, exB;
    std::vector<FeaturePtr> all() const { return {skA, exA, skB, exB}; }
};

/// A = [0,20]x[0,10]x[0,5], B = [10,30]x[0,10]x[0,5] (overlap 500 mm3)
TwoBoxes twoBoxes(double widthA = 20)
{
    TwoBoxes t;
    t.skA = sketchFeature(boxSketch(0, 0, widthA, 10));
    t.exA = extrudeFeature(t.skA->id, 5);
    t.skB = sketchFeature(boxSketch(10, 0, 20, 10));
    t.exB = extrudeFeature(t.skB->id, 5);
    return t;
}

FeaturePtr combineFeature(const Uuid& target, std::vector<Uuid> tools, BooleanOp op, bool keep = false)
{
    CombineParams p;
    p.target = target;
    p.tools = std::move(tools);
    p.op = op;
    p.keepTools = keep;
    return makeFeature(FeatureType::Combine, "Combine", Json(p));
}

Analysis run(const TwoBoxes& t, FeaturePtr extra)
{
    auto fs = t.all();
    fs.push_back(extra);
    return KernelEngine::analyze(docOf(fs));
}
}  // namespace

TEST(Kernel, CombineJoinCutIntersect)
{
    const TwoBoxes t = twoBoxes();
    const struct
    {
        BooleanOp op;
        double volume;
    } cases[] = {{BooleanOp::Join, 1500}, {BooleanOp::Cut, 500}, {BooleanOp::Intersect, 500}};
    for (const auto& c : cases) {
        const auto cb = combineFeature(t.exA->id, {t.exB->id}, c.op);
        const Analysis a = run(t, cb);
        expectAllOk(a);
        const auto& b = onlyBody(a);  // tool consumed
        EXPECT_EQ(b.id, t.exA->id);
        EXPECT_TRUE(b.valid);
        EXPECT_EQ(b.solids, 1);
        EXPECT_NEAR(b.volume, c.volume, kEps) << toString(c.op);
    }
}

TEST(Kernel, CombineKeepTools)
{
    const TwoBoxes t = twoBoxes();
    const Analysis a = run(t, combineFeature(t.exA->id, {t.exB->id}, BooleanOp::Cut, true));
    expectAllOk(a);
    ASSERT_EQ(a.bodies.size(), 2u);
    EXPECT_NEAR(a.bodies[0].volume, 500, kEps);
    EXPECT_NEAR(a.bodies[1].volume, 1000, kEps);
}

TEST(Kernel, CombineFollowsUpstreamChanges)
{
    // A widened to 25 => A ∩ B = [10,25] => 750 mm3
    const TwoBoxes t = twoBoxes(25);
    const Analysis a = run(t, combineFeature(t.exA->id, {t.exB->id}, BooleanOp::Intersect));
    expectAllOk(a);
    EXPECT_NEAR(onlyBody(a).volume, 750, kEps);
}

TEST(Kernel, CombineErrorsKeepGeometry)
{
    const TwoBoxes t = twoBoxes();
    const auto bad = combineFeature(t.exA->id, {"no-such-body"}, BooleanOp::Join);
    const Analysis a = run(t, bad);
    EXPECT_EQ(a.result(bad->id)->status, FeatureStatus::Error);
    EXPECT_EQ(a.bodies.size(), 2u);
}
