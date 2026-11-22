// v0.4 — lot 0 (direct management of bodies & faces), lot 1 (construction planes, rib, mates),
// lot 2 (printability, threads), lot 3 (diagnostics, tutorials).

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

#include "TestHelpers.h"

using namespace silex;
using namespace silex::test;

namespace {

void expectAllOk(const Analysis& a)
{
    for (const auto& r : a.results)
        EXPECT_EQ(r.status, FeatureStatus::Ok) << r.message;
}

FeaturePtr box(double l, double w, double h, Vec2 c = {}, BooleanOp op = BooleanOp::NewBody, SketchPlaneRef plane = {})
{
    PrimitiveParams p;
    p.length = l;
    p.width = w;
    p.height = h;
    p.centerX = c.x;
    p.centerY = c.y;
    p.op = op;
    p.plane = plane;
    return makeFeature(FeatureType::Box, "Box", Json(p));
}

FeaturePtr cylinder(double d, double h, Vec2 c = {}, BooleanOp op = BooleanOp::NewBody, SketchPlaneRef plane = {})
{
    PrimitiveParams p;
    p.diameter = d;
    p.height = h;
    p.centerX = c.x;
    p.centerY = c.y;
    p.op = op;
    p.plane = plane;
    return makeFeature(FeatureType::Cylinder, "Cylinder", Json(p));
}

const BodyMetrics* bodyNamed(const Analysis& a, const std::string& name)
{
    for (const auto& b : a.bodies)
        if (b.name == name)
            return &b;
    return nullptr;
}

}  // namespace

// ================================================================== lot 0

TEST(V04Lot0, SplitBoxKeepsTheVolume)
{
    auto b = box(80, 50, 30);
    SplitBodyParams sp;
    sp.plane.kind = "XY";
    sp.plane.offset = 22;
    auto split = makeFeature(FeatureType::SplitBody, "Split", Json(sp));
    const auto a = KernelEngine::analyze(docOf({b, split}));
    expectAllOk(a);
    ASSERT_EQ(a.bodies.size(), 2u);
    const BodyMetrics *bottom = bodyNamed(a, "Body1_1"), *lid = bodyNamed(a, "Body1_2");
    ASSERT_NE(bottom, nullptr);
    ASSERT_NE(lid, nullptr);
    EXPECT_TRUE(bottom->valid);
    EXPECT_TRUE(lid->valid);
    EXPECT_NEAR(bottom->volume + lid->volume, 80 * 50 * 30.0, 1e-6);
    EXPECT_NEAR(bottom->volume, 80 * 50 * 22.0, 1e-6);
    EXPECT_NEAR(lid->bbox.min.z, 22, 1e-6);
    EXPECT_EQ(bottom->id, b->id);  // part 1 keeps the body id: later references survive
}

TEST(V04Lot0, DefeatureRestoresTheSharpEdge)
{
    auto rect = makeRect(20, 10);
    auto sk = sketchFeature(rect.sk);
    auto ex = extrudeFeature(sk->id, 5);
    auto snap = KernelEngine::computeSnapshot(docOf({sk, ex}));
    auto edge = pickEdge(*snap, {20, 10, 2.5}, {0, 0, 1});
    ASSERT_TRUE(edge.has_value());
    auto fil = blendFeature(FeatureType::Fillet, {*edge}, 2.0);
    DocState d = docOf({sk, ex, fil});
    auto fillet = pickFace(*KernelEngine::computeSnapshot(d), {20 - 2 + 2 * 0.7071, 10 - 2 + 2 * 0.7071, 2.5});
    ASSERT_TRUE(fillet.has_value());

    DefeatureParams dp;
    dp.faces = {*fillet};
    auto df = makeFeature(FeatureType::DefeatureFace, "DeleteFace", Json(dp));
    const auto a = KernelEngine::analyze(docOf({sk, ex, fil, df}));
    expectAllOk(a);
    ASSERT_EQ(a.bodies.size(), 1u);
    EXPECT_TRUE(a.bodies[0].valid);
    EXPECT_EQ(a.bodies[0].faces, 6);
    EXPECT_EQ(a.bodies[0].edges, 12);
    EXPECT_NEAR(a.bodies[0].volume, 1000.0, 1e-6);  // sharp corner back
}

TEST(V04Lot0, DefeatureAHoleOfAnImportedStep)
{
    // a body without history: export a drilled block to STEP and import it back
    auto b = box(40, 30, 10);
    auto h = cylinder(8, 10, {5, 0}, BooleanOp::Cut);
    const auto path = std::filesystem::temp_directory_path() / "silex_v04_defeature.step";
    ASSERT_FALSE(KernelEngine::exportStep(docOf({b, h}), path.string()).has_value());  // nullopt = no error
    ImportParams ip;
    ip.sourcePath = "drilled.step";
    ip.brep = KernelEngine::importStepAsBrep(path.string());
    auto imp = makeFeature(FeatureType::ImportStep, "Import", Json(ip));
    DocState d = docOf({imp});
    auto hole = pickFace(*KernelEngine::computeSnapshot(d), {5 + 4, 0, 5});
    ASSERT_TRUE(hole.has_value());
    DefeatureParams dp;
    dp.faces = {*hole};
    const auto a = KernelEngine::analyze(docOf({imp, makeFeature(FeatureType::DefeatureFace, "DeleteFace", Json(dp))}));
    expectAllOk(a);
    ASSERT_EQ(a.bodies.size(), 1u);
    EXPECT_TRUE(a.bodies[0].valid);
    EXPECT_NEAR(a.bodies[0].volume, 40 * 30 * 10.0, 1e-6);
}

TEST(V04Lot0, RemoveBodyIsAStepAndUndoable)
{
    auto b1 = box(20, 20, 10);
    auto b2 = box(10, 10, 10, {40, 0});
    RemoveBodyParams rp;
    rp.bodies = {b2->id};
    auto rm = makeFeature(FeatureType::RemoveBody, "RemoveBody", Json(rp));

    History h;
    h.reset(docOf({b1, b2}));
    h.execute(AddFeatureCommand(rm));
    const auto a = KernelEngine::analyze(h.current());
    expectAllOk(a);
    ASSERT_EQ(a.bodies.size(), 1u);
    EXPECT_EQ(a.bodies[0].id, b1->id);
    EXPECT_EQ(h.current().features.size(), 3u);  // the creating features are untouched

    ASSERT_TRUE(h.undo());
    const auto u = KernelEngine::analyze(h.current());
    expectAllOk(u);
    EXPECT_EQ(u.bodies.size(), 2u);
}

TEST(V04Lot0, TransformMovesRotatesAndCopies)
{
    auto b = box(40, 20, 10);
    TransformBodyParams tp;
    tp.bodies = {b->id};
    tp.dx = 100;
    tp.rz = 90;
    auto mv = makeFeature(FeatureType::TransformBody, "Move", Json(tp));
    auto a = KernelEngine::analyze(docOf({b, mv}));
    expectAllOk(a);
    ASSERT_EQ(a.bodies.size(), 1u);
    EXPECT_NEAR(a.bodies[0].bbox.min.x, 100 - 10, 1e-6);  // turned by 90 deg around its center
    EXPECT_NEAR(a.bodies[0].bbox.max.y, 20, 1e-6);
    EXPECT_NEAR(a.bodies[0].volume, 8000, 1e-6);

    tp.copy = true;
    tp.rz = 0;
    auto cp = makeFeature(FeatureType::TransformBody, "Copy", Json(tp));
    a = KernelEngine::analyze(docOf({b, cp}));
    expectAllOk(a);
    ASSERT_EQ(a.bodies.size(), 2u);
    EXPECT_NEAR(a.bodies[1].bbox.min.x, 80, 1e-6);
    EXPECT_NEAR(a.bodies[1].volume, a.bodies[0].volume, 1e-9);
    // names stay unique: a fillet on the copy does not touch the original
    auto snap = KernelEngine::computeSnapshot(docOf({b, cp}));
    auto e = pickEdge(*snap, {120, 10, 5}, {0, 0, 1});
    ASSERT_TRUE(e.has_value());
    a = KernelEngine::analyze(docOf({b, cp, blendFeature(FeatureType::Fillet, {*e}, 2.0)}));
    expectAllOk(a);
    EXPECT_NEAR(a.bodies[0].volume, 8000, 1e-6);
    EXPECT_LT(a.bodies[1].volume, 8000);
}

// ================================================================== lot 1

namespace {
FeaturePtr plane(ConstructionPlaneParams p) { return makeFeature(FeatureType::ConstructionPlane, "Plane", Json(p)); }
}  // namespace

TEST(V04Lot1, OffsetPlaneCarriesASketchAndASplit)
{
    ConstructionPlaneParams cp;
    cp.mode = "offset";
    cp.base.kind = "XY";
    cp.base.offset = 12;
    auto pl = plane(cp);
    SketchPlaneRef onPlane;
    onPlane.kind = "Plane";
    onPlane.planeId = pl->id;
    auto rect = makeRect(20, 10);
    auto sk = sketchFeature(rect.sk, onPlane);
    auto ex = extrudeFeature(sk->id, 6);
    auto a = KernelEngine::analyze(docOf({pl, sk, ex}));
    expectAllOk(a);
    ASSERT_EQ(a.bodies.size(), 1u);
    EXPECT_NEAR(a.bodies[0].bbox.min.z, 12, 1e-6);
    EXPECT_NEAR(a.bodies[0].bbox.max.z, 18, 1e-6);
    const auto snap = KernelEngine::computeSnapshot(docOf({pl, sk, ex}));
    ASSERT_EQ(snap->planes.size(), 1u);
    EXPECT_NEAR(snap->planes[0].plane.origin.z, 12, 1e-9);

    // the same plane splits a box (box 80 x 50 x 30, plane at z = 12)
    auto b = box(80, 50, 30);
    SplitBodyParams sp;
    sp.body = b->id;
    sp.plane = onPlane;
    a = KernelEngine::analyze(docOf({pl, b, makeFeature(FeatureType::SplitBody, "Split", Json(sp))}));
    expectAllOk(a);
    ASSERT_EQ(a.bodies.size(), 2u);
    EXPECT_NEAR(a.bodies[0].volume, 80 * 50 * 12.0, 1e-6);
}

TEST(V04Lot1, AngledAndMidPlanes)
{
    ConstructionPlaneParams ang;
    ang.mode = "angle";
    ang.base.kind = "XY";
    ang.axis.kind = "X";
    ang.angle = 30;
    auto b = box(40, 30, 10);
    DocState d = docOf({b});
    auto snap0 = KernelEngine::computeSnapshot(d);
    auto top = pickFace(*snap0, {0, 0, 10}), bottom = pickFace(*snap0, {0, 0, 0});
    ASSERT_TRUE(top && bottom);
    ConstructionPlaneParams mid;
    mid.mode = "mid";
    mid.face1 = *top;
    mid.face2 = *bottom;
    const auto snap = KernelEngine::computeSnapshot(docOf({b, plane(ang), plane(mid)}));
    ASSERT_EQ(snap->planes.size(), 2u);
    for (const auto& pv : snap->planes) {
        if (std::abs(pv.plane.origin.z - 5) < 1e-6) {  // mid plane
            EXPECT_NEAR(std::abs(pv.plane.normal.z), 1, 1e-9);
        }
        else {  // XY turned by 30 deg around X
            EXPECT_NEAR(pv.plane.normal.z, std::cos(kPi / 6), 1e-9);
            EXPECT_NEAR(pv.plane.normal.y, -std::sin(kPi / 6), 1e-9);
        }
    }
}

TEST(V04Lot1, RibFillsTheBracketCorner)
{
    // L bracket: base 40 x 20 x 5 + wall 5 x 20 x 40
    auto base = box(40, 20, 5, {20, 10});
    auto wall = box(5, 20, 40, {2.5, 10}, BooleanOp::Join);
    Sketch s;
    s.geos.push_back(makeLine({5, 30}, {30, 5}));  // sketch on XZ: y -> world Z
    SketchPlaneRef xz;
    xz.kind = "XZ";
    xz.offset = -10;  // XZ normal is -Y: the plane sits at y = 10 (middle of the bracket)
    auto sk = sketchFeature(s, xz);
    RibParams rp;
    rp.sketch = sk->id;
    rp.thickness = 3;
    auto rib = makeFeature(FeatureType::Rib, "Rib", Json(rp));
    const auto a = KernelEngine::analyze(docOf({base, wall, sk, rib}));
    expectAllOk(a);
    ASSERT_EQ(a.bodies.size(), 1u);
    EXPECT_TRUE(a.bodies[0].valid);
    EXPECT_EQ(a.bodies[0].solids, 1);
    EXPECT_NEAR(a.bodies[0].volume, 40 * 20 * 5 + 5 * 20 * 35 + 25 * 25 / 2.0 * 3, 1e-6);
}

TEST(V04Lot1, MateLidOnBoxWithClearanceAndCoaxialPin)
{
    auto base = box(60, 40, 20);
    SketchPlaneRef high;
    high.kind = "XY";
    high.offset = 7;
    auto lid = box(60, 40, 3, {100, 50}, BooleanOp::NewBody, high);
    DocState d = docOf({base, lid});
    auto snap = KernelEngine::computeSnapshot(d);
    auto baseTop = pickFace(*snap, {0, 0, 20}), lidBottom = pickFace(*snap, {100, 50, 7});
    ASSERT_TRUE(baseTop && lidBottom);
    TransformBodyParams tp;
    tp.bodies = {lid->id};
    tp.mate = Json{{"mode", "face"}, {"moving", *lidBottom}, {"fixed", *baseTop}, {"offset", 0.25}, {"flip", false}};
    auto mate = makeFeature(FeatureType::TransformBody, "Align", Json(tp));
    d = docOf({base, lid, mate});
    const auto a = KernelEngine::analyze(d);
    expectAllOk(a);
    ASSERT_EQ(a.bodies.size(), 2u);
    const BodyMetrics& l = a.bodies[1].id == lid->id ? a.bodies[1] : a.bodies[0];
    EXPECT_NEAR(l.bbox.min.z, 20.25, 1e-6);
    EXPECT_NEAR(l.bbox.max.z, 23.25, 1e-6);
    EXPECT_NEAR(l.bbox.min.x, -30, 1e-6);
    EXPECT_NEAR(l.bbox.min.y, -20, 1e-6);
    const auto rep = KernelEngine::checkInterference(d, {}, false);
    EXPECT_FALSE(rep.collision);
    EXPECT_NEAR(rep.minClearance, 0.25, 1e-6);  // green: functional clearance

    // coaxial: a pin lying elsewhere goes into a hole of the box
    auto hole = cylinder(8, 20, {10, 0}, BooleanOp::Cut);
    SketchPlaneRef far;
    far.kind = "YZ";
    auto pin = cylinder(6, 15, {40, 60}, BooleanOp::NewBody, far);
    d = docOf({base, hole, pin});
    snap = KernelEngine::computeSnapshot(d);
    auto holeFace = pickFace(*snap, {14, 0, 10}), pinSide = pickFace(*snap, {7.5, 40 + 3, 60});
    ASSERT_TRUE(holeFace && pinSide);
    TransformBodyParams cx;
    cx.bodies = {pin->id};
    cx.mate = Json{{"mode", "axis"}, {"moving", *pinSide}, {"fixed", *holeFace}, {"offset", 0.0}, {"flip", false}};
    const auto b = KernelEngine::analyze(docOf({base, hole, pin, makeFeature(FeatureType::TransformBody, "Align", Json(cx))}));
    expectAllOk(b);
    const BodyMetrics& p = b.bodies[1].id == pin->id ? b.bodies[1] : b.bodies[0];
    EXPECT_NEAR((p.bbox.min.x + p.bbox.max.x) / 2, 10, 1e-6);
    EXPECT_NEAR((p.bbox.min.y + p.bbox.max.y) / 2, 0, 1e-6);
    EXPECT_NEAR(p.bbox.max.x - p.bbox.min.x, 6, 1e-6);  // now vertical
    EXPECT_NEAR(p.bbox.max.z - p.bbox.min.z, 15, 1e-6);
}

// ================================================================== lot 2

#include "model/Printability.h"

TEST(V04Lot2, OverhangAreasAreExact)
{
    // box 20 x 10 x 5 turned by 35 deg around Y: its bottom face hangs at 90 - 35 = 55 deg (orange)
    auto b = box(20, 10, 5);
    TransformBodyParams tp;
    tp.bodies = {b->id};
    tp.ry = 35;
    auto snap = KernelEngine::computeSnapshot(docOf({b, makeFeature(FeatureType::TransformBody, "Tilt", Json(tp))}));
    ASSERT_EQ(snap->bodies.size(), 1u);
    PrintabilityOptions o;
    o.checkWalls = false;
    auto r = analyzePrintability(snap->bodies[0].mesh, o);
    EXPECT_NEAR(r.warnArea, 200, 1e-4);  // float display mesh
    EXPECT_NEAR(r.failArea, 0, 1e-9);

    // T shape: a 40 x 10 plate on a 10 x 10 column: 300 mm2 of flat ceiling (red), the foot is on the plate
    auto column = box(10, 10, 20);
    SketchPlaneRef top;
    top.kind = "XY";
    top.offset = 20;
    auto plate = box(40, 10, 5, {}, BooleanOp::Join, top);
    snap = KernelEngine::computeSnapshot(docOf({column, plate}));
    r = analyzePrintability(snap->bodies[0].mesh, o);
    EXPECT_NEAR(r.failArea, 300, 1e-4);
    EXPECT_NEAR(r.warnArea, 0, 1e-9);
    // printed upside down (build direction -Z) the plate lies on the bed: no support needed at all
    o.buildDir = {0, 0, -1};
    r = analyzePrintability(snap->bodies[0].mesh, o);
    EXPECT_NEAR(r.failArea, 0, 1e-9);
    EXPECT_NEAR(r.warnArea, 0, 1e-9);
}

TEST(V04Lot2, ThinWallsAreFlagged)
{
    auto b = box(40, 30, 20);
    auto snap0 = KernelEngine::computeSnapshot(docOf({b}));
    auto top = pickFace(*snap0, {0, 0, 20});
    ASSERT_TRUE(top.has_value());
    for (double wall : {2.0, 0.5}) {
        ShellParams sp;
        sp.removedFaces = {*top};
        sp.thickness = wall;
        auto snap = KernelEngine::computeSnapshot(docOf({b, makeFeature(FeatureType::Shell, "Shell", Json(sp))}));
        const auto r = analyzePrintability(snap->bodies[0].mesh);
        EXPECT_NEAR(r.minThickness, wall, 1e-3) << wall;
        if (wall >= 0.8)
            EXPECT_NEAR(r.thinArea, 0, 1e-9);
        else
            EXPECT_GT(r.thinArea, 1000);  // the walls and the floor
    }
}

TEST(V04Lot2, M6ScrewAndNutWithFdmClearanceDoNotCollide)
{
    ASSERT_TRUE(metricThread("M6").has_value());
    EXPECT_EQ(metricThread("M6")->pitch, 1.0);
    auto shaft = cylinder(6, 24);
    SketchPlaneRef nutBase;
    nutBase.kind = "XY";
    nutBase.offset = 6;
    auto nut = box(14, 14, 8, {}, BooleanOp::NewBody, nutBase);
    auto drill = cylinder(5, 8, {}, BooleanOp::Cut, nutBase);  // cuts the last body: the nut
    DocState d = docOf({shaft, nut, drill});
    auto snap = KernelEngine::computeSnapshot(d);
    auto shaftSide = pickFace(*snap, {0, 0, 12}), hole = pickFace(*snap, {0, 0, 10});
    ASSERT_TRUE(shaftSide && hole);
    ThreadParams ext, in;
    ext.face = *shaftSide;
    in.face = *hole;
    auto t1 = makeFeature(FeatureType::Thread, "Thread", Json(ext));
    auto t2 = makeFeature(FeatureType::Thread, "Thread", Json(in));
    d = docOf({shaft, nut, drill, t1, t2});
    const auto a = KernelEngine::analyze(d);
    expectAllOk(a);
    ASSERT_EQ(a.bodies.size(), 2u);
    for (const auto& b : a.bodies)
        EXPECT_TRUE(b.valid) << b.name;
    EXPECT_LT(a.bodies[0].volume, kPi * 9 * 24 * 0.9);  // grooves removed from the shaft
    EXPECT_GT(a.bodies[0].faces, 10);                    // helical flanks
    const auto rep = KernelEngine::checkInterference(d, {}, false);
    ASSERT_EQ(rep.pairs.size(), 1u);
    EXPECT_EQ(rep.pairs[0].status, "ok");  // green
    EXPECT_FALSE(rep.collision);  // green with 0.15 mm of play
    EXPECT_NEAR(rep.minClearance, 0.15, 0.002);  // the flank play is the requested clearance

    // cosmetic thread: no geometry, an annotation
    ext.modeled = false;
    const auto c = KernelEngine::computeSnapshot(docOf({shaft, makeFeature(FeatureType::Thread, "Thread", Json(ext))}));
    ASSERT_EQ(c->threads.size(), 1u);
    EXPECT_EQ(c->threads[0].label, "M6x1");
    EXPECT_NEAR(c->bodies[0].volume, kPi * 9 * 24, 1e-6);
}

// ================================================================== lot 3

#include <fstream>

#include "model/Pedagogy.h"
#include "sketch/SketchCoach.h"

namespace {
Lesson loadLesson(const char* file)
{
    std::ifstream in(std::string(SILEX_RESOURCES_DIR) + "/tutorials/" + file);
    EXPECT_TRUE(in.good()) << file;
    Json j;
    in >> j;
    return lessonFromJson(j);
}
ModelFacts factsOf(const DocState& d) { return collectFacts(d, *KernelEngine::computeSnapshot(d)); }
std::string join(const std::vector<CoachItem>& items)
{
    std::string s;
    for (const auto& i : items)
        s += i.text + "\n";
    return s;
}
}  // namespace

const char* const kLessonFiles[] = {"01_equerre.json", "02_boitier.json", "03_bride.json",  "04_de.json",     "05_pince.json",
                                    "06_poulie.json",  "07_palier.json",  "08_raccord.json", "09_coude.json"};

TEST(V04Lot3, LessonsLoad)
{
    for (const char* f : kLessonFiles) {
        const Lesson l = loadLesson(f);
        EXPECT_FALSE(l.title.empty());
        EXPECT_GE(l.steps.size(), 5u) << f;
        std::vector<bool> done;
        EXPECT_EQ(updateLessonProgress(l, ModelFacts{}, done), 0) << f;  // nothing done on an empty model
        for (const auto& st : l.steps) {
            EXPECT_FALSE(st.goal.empty());
            EXPECT_FALSE(st.criteria.empty());
        }
    }
}

TEST(V04Lot3, Lesson1BracketValidatesEndToEnd)
{
    const Lesson l = loadLesson("01_equerre.json");
    std::vector<bool> done;
    DocState d;

    // step 1: the base sketch, fully constrained 85 x 40
    auto rect = makeRect(85, 40);
    auto sk = sketchFeature(rect.sk);
    d = docOf({sk});
    EXPECT_EQ(updateLessonProgress(l, factsOf(d), done), 1);

    // step 2: extruded 10 mm
    auto ex = extrudeFeature(sk->id, 10);
    d = docOf({sk, ex});
    EXPECT_EQ(updateLessonProgress(l, factsOf(d), done), 2);

    // step 3: an upright joined on top
    SketchPlaneRef top;
    top.kind = "XY";
    top.offset = 10;
    Sketch up;
    for (auto g : {makeLine({0, 0}, {85, 0}), makeLine({85, 0}, {85, 10}), makeLine({85, 10}, {0, 10}), makeLine({0, 10}, {0, 0})})
        up.geos.push_back(g);
    for (int i = 0; i < 4; ++i)
        up.constraints.push_back(makeConstraint(ConstraintKind::Coincident, {up.geos[size_t(i)].id, PointPos::End},
                                                {up.geos[size_t((i + 1) % 4)].id, PointPos::Start}));
    auto sk2 = sketchFeature(up, top);
    auto ex2 = extrudeFeature(sk2->id, 50, BooleanOp::Join);
    d = docOf({sk, ex, sk2, ex2});
    EXPECT_EQ(updateLessonProgress(l, factsOf(d), done), 3);

    // step 4: a hole
    auto hole = cylinder(10, 10, {42.5, 25}, BooleanOp::Cut);
    d = docOf({sk, ex, sk2, ex2, hole});
    EXPECT_EQ(updateLessonProgress(l, factsOf(d), done), 4);

    // step 5: fillets
    auto e = pickEdge(*KernelEngine::computeSnapshot(d), {85, 40, 5}, {0, 0, 1});
    ASSERT_TRUE(e.has_value());
    auto fil = blendFeature(FeatureType::Fillet, {*e}, 3);
    d = docOf({sk, ex, sk2, ex2, hole, fil});
    EXPECT_EQ(updateLessonProgress(l, factsOf(d), done), 5);

    // step 6: the parametric magic: the base becomes 15 mm thick
    auto ep = ex->as<ExtrudeParams>();
    ep.distance = 15;
    auto ex15 = std::make_shared<FeatureNode>(*ex);
    ex15->params = Json(ep);
    d = UpdateFeatureCommand(ex15).apply(d);
    EXPECT_EQ(updateLessonProgress(l, factsOf(d), done), int(l.steps.size()));  // lesson complete
    EXPECT_TRUE(std::all_of(done.begin(), done.end(), [](bool b) { return b; }));
}

TEST(V04Lot3, PlainLanguageDiagnostics)
{
    // 1) fillet far too large on a 10 mm high block
    auto b = box(40, 30, 10);
    auto snap = KernelEngine::computeSnapshot(docOf({b}));
    auto e = pickEdge(*snap, {0, 15, 10}, {1, 0, 0});
    ASSERT_TRUE(e.has_value());
    auto fil = blendFeature(FeatureType::Fillet, {*e}, 15);
    auto a = KernelEngine::analyze(docOf({b, fil}));
    const FeatureResult* r = a.result(fil->id);
    ASSERT_TRUE(r && r->status == FeatureStatus::Error);
    ErrorContext ctx;
    ctx.smallestBodyExtent = 10;
    std::string t = explainFeatureError(fil->type, r->message, fil->params, ctx, true);
    EXPECT_NE(t.find("trop grand"), std::string::npos) << t;
    EXPECT_NE(t.find("sous 5 mm"), std::string::npos) << t;

    // 2) extrusion of an open outline
    Sketch open;
    open.geos = {makeLine({0, 0}, {20, 0}), makeLine({20, 0}, {20, 10}), makeLine({20, 10}, {0, 12})};
    auto osk = sketchFeature(open);
    auto oex = extrudeFeature(osk->id, 5);
    a = KernelEngine::analyze(docOf({osk, oex}));
    r = a.result(oex->id);
    ASSERT_TRUE(r && r->status == FeatureStatus::Error);
    t = explainFeatureError(oex->type, r->message, oex->params, {}, true);
    EXPECT_NE(t.find("Aucun contour fermé"), std::string::npos) << r->message;

    // 3) revolution through the axis
    t = explainFeatureError("Revolve", "The revolution failed (does the profile cross the axis?)", Json::object(), {}, true);
    EXPECT_NE(t.find("traverse l'axe"), std::string::npos);

    // 4) a dimensioned rectangle that still floats
    auto rect = makeRect(40, 85);
    rect.sk.constraints.erase(std::remove_if(rect.sk.constraints.begin(), rect.sk.constraints.end(),
                                             [](const SketchConstraint& c) { return c.b.geo == kOriginId; }),
                              rect.sk.constraints.end());
    t = join(explainSketch(rect.sk, true));
    EXPECT_NE(t.find("40 × 85 mm"), std::string::npos) << t;
    EXPECT_NE(t.find("glisser en X et Y"), std::string::npos) << t;
    EXPECT_NE(t.find("Milieu (M)"), std::string::npos) << t;

    // 5) a circle with its diameter but no position
    Sketch cs;
    auto c = makeCircle({12, 7}, 5);
    cs.geos = {c};
    cs.constraints.push_back(makeConstraint(ConstraintKind::Diameter, {c.id}, {}, 10));
    t = join(explainSketch(cs, true));
    EXPECT_NE(t.find("Ø10"), std::string::npos) << t;
    EXPECT_NE(t.find("distance au bord"), std::string::npos) << t;

    // 6) over-constrained sketch, and the English version
    auto over = makeRect(20, 10);
    over.sk.constraints.push_back(makeConstraint(ConstraintKind::Distance, {over.lines[2]}, {}, 25));
    t = join(explainSketch(over.sk, false));
    EXPECT_NE(t.find("contradicts"), std::string::npos) << t;
    EXPECT_NE(join(explainSketch(makeRect(20, 10).sk, true)).find("entièrement contrainte"), std::string::npos);
}

// ================================================================== finitions: 9 lessons, 6 cas d'école

namespace {
Json readJson(const std::string& path)
{
    std::ifstream in(path);
    EXPECT_TRUE(in.good()) << path;
    Json j;
    in >> j;
    return j;
}
}  // namespace

TEST(Finitions, NineBilingualLessonsAreValid)
{
    std::set<int> orders;
    std::map<int, int> perLevel;
    for (const char* f : kLessonFiles) {
        const Json j = readJson(std::string(SILEX_RESOURCES_DIR) + "/tutorials/" + f);
        const auto problems = validateLessonJson(j);
        std::string all;
        for (const auto& p : problems)
            all += p + "\n";
        EXPECT_TRUE(problems.empty()) << f << ":\n" << all;
        const Lesson fr = lessonFromJson(j, true), en = lessonFromJson(j, false);
        EXPECT_NE(fr.title, en.title) << f;  // really translated
        ASSERT_EQ(fr.steps.size(), en.steps.size());
        for (size_t i = 0; i < fr.steps.size(); ++i)
            EXPECT_NE(fr.steps[i].goal, en.steps[i].goal) << f << " step " << i + 1;
        orders.insert(fr.order);
        ++perLevel[fr.levelRank];
        std::vector<bool> done;
        EXPECT_EQ(updateLessonProgress(fr, ModelFacts{}, done), 0) << f;
    }
    EXPECT_EQ(orders, (std::set<int>{1, 2, 3, 4, 5, 6, 7, 8, 9}));
    EXPECT_EQ(perLevel[0], 2);  // Débutant: 1, 4
    EXPECT_EQ(perLevel[1], 4);  // Intermédiaire: 2, 5, 6, 7
    EXPECT_EQ(perLevel[2], 3);  // Avancé: 3, 8, 9

    // the validator really catches mistakes
    Json bad = readJson(std::string(SILEX_RESOURCES_DIR) + "/tutorials/04_de.json");
    bad["steps"][0]["criteria"][0]["type"] = "volum";
    bad["steps"][1]["title"].erase("en");
    bad["steps"][2]["criteria"].push_back({{"type", "event"}, {"name", "nope"}, {"label", {{"fr", "x"}, {"en", "y"}}}});
    EXPECT_EQ(validateLessonJson(bad).size(), 3u);
}

/// Each annotated example is the reference solution of its lesson: replaying its timeline step by
/// step (as a student would build it) validates every step of the lesson.
TEST(Finitions, EachAnnotatedExampleCompletesItsLesson)
{
    for (const char* f : {"04_de.json", "05_pince.json", "06_poulie.json", "07_palier.json", "08_raccord.json",
                          "09_coude.json"}) {
        const Json lj = readJson(std::string(SILEX_RESOURCES_DIR) + "/tutorials/" + f);
        const Lesson l = lessonFromJson(lj, true);
        ASSERT_FALSE(l.example.empty()) << f;
        const DocState full = docFromJson(readJson(std::string(SILEX_EXAMPLES_DIR) + "/" + l.example + ".silex"));
        int annotated = 0, annotatedEn = 0;
        for (const auto& n : full.features) {
            annotated += !n->annotation.empty();
            annotatedEn += !n->annotationEn.empty();
        }
        EXPECT_GE(annotated, 5) << l.example;
        EXPECT_EQ(annotated, annotatedEn) << l.example;  // bilingual guided tour

        std::vector<bool> done;
        int active = 0;
        ModelFacts last;
        for (int m = 1; m <= int(full.features.size()); ++m) {
            DocState d = full;
            d.marker = m;
            last = factsOf(d);
            active = updateLessonProgress(l, last, done);
        }
        EXPECT_TRUE(std::none_of(last.features.begin(), last.features.end(),
                                 [](const ModelFacts::Feature& x) { return x.status == FeatureStatus::Error; }))
            << l.example;
        // UI events (exports, printability check) cannot come from a file: the student triggers them
        last.events = {"drawing_exported", "3mf_exported", "printability_ok", "interference_ok"};
        active = updateLessonProgress(l, last, done);
        EXPECT_EQ(active, int(l.steps.size())) << l.example << " stops at step " << active + 1;
    }
}
