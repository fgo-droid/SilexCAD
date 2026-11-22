// v0.5 — Makers & Pedagogy: lot 1 (standard holes, locked components), lot 4 (pins, lay flat, closed
// shell), lot 2 (repair challenges with robustness tests), lot 5 (lesson recorder).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

#include "TestHelpers.h"
#include "model/Pedagogy.h"
#include "sketch/SketchCoach.h"

using namespace silex;
using namespace silex::test;

namespace {

FeaturePtr box(double l, double w, double h, Vec2 c = {}, BooleanOp op = BooleanOp::NewBody, double z = 0)
{
    PrimitiveParams p;
    p.length = l;
    p.width = w;
    p.height = h;
    p.centerX = c.x;
    p.centerY = c.y;
    p.op = op;
    p.plane.offset = z;
    return makeFeature(FeatureType::Box, "Box", Json(p));
}

double volumeOf(const Analysis& a, const Uuid& id)
{
    for (const auto& b : a.bodies)
        if (b.id == id)
            return b.volume;
    return -1;
}

const FeatureResult* resultOf(const Analysis& a, const Uuid& id) { return a.result(id); }

Json readJson(const std::string& path)
{
    std::ifstream in(path);
    EXPECT_TRUE(in.good()) << path;
    Json j;
    in >> j;
    return j;
}

/// A hole drilled from the top face of a 12 mm plate (XY plane offset 12, through).
FeaturePtr hole(const std::string& kind, Vec2 c, const std::string& size = "M3", double depth = 0)
{
    FastenerHoleParams h;
    h.plane.kind = "XY";
    h.plane.offset = 12;
    h.centers = {c};
    h.kind = kind;
    h.size = size;
    h.depth = depth;
    return makeFeature(FeatureType::FastenerHole, "Hole", Json(h));
}

}  // namespace

// ================================================================== lot 1: standard holes

TEST(V05Holes, EveryKindCutsTheExpectedVolume)
{
    const double r = 1.7;  // M3 normal fit: 3.4
    struct Case
    {
        const char* kind;
        double removed;
    } cases[] = {
        {"clearance", kPi * r * r * 12},
        {"counterbore", kPi * (3.05 * 3.05 * 3.2 + r * r * 8.8)},
        {"countersink", kPi * 1.5 / 3 * (3.2 * 3.2 + 3.2 * r + r * r) + kPi * r * r * 10.5},
        {"insert", kPi * 2.0 * 2.0 * 5.7},  // + the 0.4 mm entry chamfer
        {"nut", std::sqrt(3.0) / 2 * 5.8 * 5.8 * 2.6 + kPi * r * r * 9.4},
    };
    for (const auto& c : cases) {
        auto b = box(60, 40, 12);
        auto h = hole(c.kind, {0, 0});
        const Analysis a = KernelEngine::analyze(docOf({b, h}));
        ASSERT_EQ(resultOf(a, h->id)->status, FeatureStatus::Ok) << c.kind << ": " << resultOf(a, h->id)->message;
        const double removed = 60 * 40 * 12 - volumeOf(a, b->id);
        if (std::string(c.kind) == "insert")
            EXPECT_GT(removed, c.removed) << c.kind;  // chamfer adds a little
        EXPECT_NEAR(removed, c.removed, std::string(c.kind) == "insert" ? 2.0 : 0.05) << c.kind;
    }
}

TEST(V05Holes, SeveralCentersBlindDepthAndPatternTool)
{
    auto b = box(80, 40, 12);
    FastenerHoleParams h;
    h.plane.kind = "XY";
    h.plane.offset = 12;
    h.centers = {{-30, 0}, {0, 0}, {30, 0}};
    h.size = "M5";
    h.fit = "close";
    h.depth = 6;
    auto f = makeFeature(FeatureType::FastenerHole, "Holes", Json(h));
    const Analysis a = KernelEngine::analyze(docOf({b, f}));
    ASSERT_EQ(resultOf(a, f->id)->status, FeatureStatus::Ok);
    EXPECT_NEAR(80 * 40 * 12 - volumeOf(a, b->id), 3 * kPi * 2.6 * 2.6 * 6, 1e-3);
    // the dimensions shown by the panel
    const auto d = fastenerHoleDims(h);
    ASSERT_TRUE(d);
    EXPECT_DOUBLE_EQ(d->holeD, 5.2);
    EXPECT_FALSE(fastenerSpec("M7"));
}

// ================================================================== lot 1: locked reference components

TEST(V05Components, LockedBodyIsSkippedAndProtected)
{
    // a "component": any imported body marked locked
    auto part = box(30, 30, 10);
    const std::string brep = [] {
        // the B-Rep of a small box, through a STEP round trip
        const std::string path = (std::filesystem::temp_directory_path() / "silex_v05_comp.step").string();
        auto b = box(10, 10, 2, {50, 0});
        EXPECT_FALSE(KernelEngine::exportStep(docOf({b}), path, {}).has_value());
        return KernelEngine::importStepAsBrep(path);
    }();
    ImportParams ip;
    ip.brep = brep;
    ip.component = "test_board";
    ip.locked = true;
    ip.material = "PCB Vert";
    auto comp = makeFeature(FeatureType::ImportStep, "Board", Json(ip));
    // a join without explicit target goes to the last *editable* body (the part), not the board
    auto boss = box(5, 5, 5, {0, 0}, BooleanOp::Join, 10);
    Analysis a = KernelEngine::analyze(docOf({part, comp, boss}));
    EXPECT_EQ(resultOf(a, boss->id)->status, FeatureStatus::Ok);
    EXPECT_NEAR(volumeOf(a, part->id), 30 * 30 * 10 + 125, 1e-6);
    EXPECT_NEAR(volumeOf(a, comp->id), 200, 1e-6);
    // the snapshot flags it (inspector lock, exports and printability skip it) with its material
    auto snap = KernelEngine::computeSnapshot(docOf({part, comp}));
    int locked = 0;
    for (const auto& b : snap->bodies)
        if (b.locked()) {
            ++locked;
            EXPECT_EQ(b.material.name, "PCB Vert");
        }
    EXPECT_EQ(locked, 1);
    // modifying the board explicitly is refused with a clear message
    PrimitiveParams cut;
    cut.length = cut.width = cut.height = 4;
    cut.centerX = 50;
    cut.op = BooleanOp::Cut;
    cut.targetBody = comp->id;
    auto bad = makeFeature(FeatureType::Box, "Cut", Json(cut));
    a = KernelEngine::analyze(docOf({part, comp, bad}));
    EXPECT_EQ(resultOf(a, bad->id)->status, FeatureStatus::Error);
    EXPECT_NE(resultOf(a, bad->id)->message.find("locked"), std::string::npos);
}

TEST(V05Components, CatalogFilesAreComplete)
{
    const std::filesystem::path dir = std::filesystem::path(SILEX_RESOURCES_DIR) / "components";
    std::set<std::string> ids;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() != ".json")
            continue;
        const Json c = readJson(e.path().string());
        ids.insert(c.value("id", std::string{}));
        EXPECT_TRUE(std::filesystem::exists(dir / c.value("step", std::string{}))) << e.path();
        for (const char* k : {"fr", "en"}) {
            EXPECT_FALSE(c["name"].value(k, std::string{}).empty()) << e.path();
            EXPECT_FALSE(c["description"].value(k, std::string{}).empty()) << e.path();
        }
        EXPECT_FALSE(c.value("material", std::string{}).empty());
    }
    EXPECT_EQ(ids, (std::set<std::string>{"rpi4b", "rpi5", "pico", "esp32_devkit", "arduino_uno", "oled_096", "button_6x6",
                                          "usb_c_receptacle", "fan_40"}));
    // official Raspberry Pi mounting holes: 58 x 49 mm
    const Json pi = readJson((dir / "rpi4b.json").string());
    const auto& h = pi["mounting_holes"];
    ASSERT_EQ(h.size(), 4u);
    EXPECT_DOUBLE_EQ(h[1]["x"].get<double>() - h[0]["x"].get<double>(), 58);
    EXPECT_DOUBLE_EQ(h[2]["y"].get<double>() - h[0]["y"].get<double>(), 49);
}

// ================================================================== lot 4: printing helpers

TEST(V05Print, SplitWithPegsAddsPinsAndMatchingHoles)
{
    auto b = box(200, 40, 30);
    SplitBodyParams sp;
    sp.body = b->id;
    sp.plane.kind = "YZ";
    sp.pegs = true;
    auto split = makeFeature(FeatureType::SplitBody, "Split", Json(sp));
    const DocState d = docOf({b, split});
    const Analysis a = KernelEngine::analyze(d);
    ASSERT_EQ(resultOf(a, split->id)->status, FeatureStatus::Ok) << resultOf(a, split->id)->message;
    ASSERT_EQ(a.bodies.size(), 2u);
    const double half = 100 * 40 * 30;
    // 2 pins Ø4 protruding 4 mm (tip chamfer 0.4) / 2 holes Ø4.4, 4.4 mm deep
    const double pin = kPi * 4 * 3.6 + kPi * 0.4 / 3 * (4 + 2 * 1.6 + 1.6 * 1.6);
    EXPECT_NEAR(volumeOf(a, b->id) - half, 2 * pin, 0.05);
    EXPECT_NEAR(half - volumeOf(a, split->id + "#2"), 2 * kPi * 2.2 * 2.2 * 4.4, 0.05);
    const InterferenceReport r = KernelEngine::checkInterference(d, {}, false);
    EXPECT_FALSE(r.collision);
}

TEST(V05Print, LayFlatPutsTheFaceOnTheBed)
{
    auto bar = box(20, 20, 120);  // standing bar
    auto snap = KernelEngine::computeSnapshot(docOf({bar}));
    const MeshFace* side = nullptr;
    for (const auto& f : snap->bodies[0].mesh.faces)
        if (f.planar && std::abs(f.plane.normal.x - 1) < 1e-9)
            side = &f;
    ASSERT_TRUE(side);
    TransformBodyParams tp;
    tp.bodies = {bar->id};
    tp.mate = Json{{"mode", "bed"}, {"moving", TopoRef{side->name, side->sig}}, {"center", true}};
    auto lay = makeFeature(FeatureType::TransformBody, "LayFlat", Json(tp));
    const Analysis a = KernelEngine::analyze(docOf({bar, lay}));
    ASSERT_EQ(resultOf(a, lay->id)->status, FeatureStatus::Ok);
    const BBox& bb = a.bodies[0].bbox;
    EXPECT_NEAR(bb.min.z, 0, 1e-6);
    EXPECT_NEAR(bb.max.z, 20, 1e-6);
    EXPECT_NEAR(bb.min.x + bb.max.x, 0, 1e-6);  // centered on the plate
    EXPECT_NEAR(bb.max.x - bb.min.x, 120, 1e-6);
}

TEST(V05Print, ClosedShellOfARoundedBlank)
{
    auto b = box(84, 54, 34);
    auto snap = KernelEngine::computeSnapshot(docOf({b}));
    EdgeBlendParams fp;
    for (const auto& e : snap->bodies[0].mesh.edges)
        if (e.sig.type == 0 && std::abs(e.sig.dir.normalized().z) > 0.999)
            fp.edges.push_back(TopoRef{e.name, e.sig});
    ASSERT_EQ(fp.edges.size(), 4u);
    fp.value = 3;
    auto fil = makeFeature(FeatureType::Fillet, "Fillet", Json(fp));
    ShellParams sh;
    sh.body = b->id;
    sh.thickness = 2;
    auto shell = makeFeature(FeatureType::Shell, "Shell", Json(sh));
    const Analysis a = KernelEngine::analyze(docOf({b, fil, shell}));
    ASSERT_EQ(resultOf(a, shell->id)->status, FeatureStatus::Ok) << resultOf(a, shell->id)->message;
    // outer rounded blank minus the inner one (80 x 50 x 30, corner radius 1)
    const double outer = 84 * 54 * 34 - 4 * 9 * (1 - kPi / 4) * 34;
    const double inner = 80 * 50 * 30 - 4 * 1 * (1 - kPi / 4) * 30;
    EXPECT_NEAR(volumeOf(a, b->id), outer - inner, 1.0);
}

// ================================================================== lot 2: repair challenges

namespace {
const char* const kRepairs[] = {"repair_1_esquisse_elastique", "repair_2_couvercle_bloque", "repair_3_conge_impossible",
                                "repair_4_contour_ouvert", "repair_5_piece_step"};

int progressOf(const Lesson& l, const DocState& d)
{
    ModelFacts f = collectFacts(d, *KernelEngine::computeSnapshot(d));
    std::vector<Json> all;
    for (const auto& st : l.steps)
        all.insert(all.end(), st.criteria.begin(), st.criteria.end());
    KernelEngine::enrichLessonFacts(f, d, all);
    std::vector<bool> done;
    return updateLessonProgress(l, f, done);
}
}  // namespace

TEST(V05Repair, FiveBilingualChallengesAreValid)
{
    std::set<int> orders;
    for (const char* name : kRepairs) {
        const Json j = readJson(std::string(SILEX_RESOURCES_DIR) + "/tutorials/" + name + ".json");
        const auto problems = validateLessonJson(j);
        std::string all;
        for (const auto& p : problems)
            all += p + "\n";
        EXPECT_TRUE(problems.empty()) << name << ":\n" << all;
        const Lesson fr = lessonFromJson(j, true), en = lessonFromJson(j, false);
        EXPECT_EQ(fr.category, "repair");
        EXPECT_EQ(fr.start, name);
        EXPECT_EQ(fr.minutes, 5);
        EXPECT_NE(fr.title, en.title);
        orders.insert(fr.order);
        // the start file exists next to the lesson
        EXPECT_TRUE(std::filesystem::exists(std::string(SILEX_RESOURCES_DIR) + "/tutorials/" + name + ".silex"));
    }
    EXPECT_EQ(orders, (std::set<int>{1, 2, 3, 4, 5}));
}

/// The broken part does not pass its challenge; one correct repair does — robustness test included
/// (e.g. the elastic sketch: the holes are symmetric at 60 mm in both files, only the repaired one
/// keeps them symmetric when Silex sets the width to 80 mm).
TEST(V05Repair, BrokenPartFailsAndRepairedPartPasses)
{
    for (int i = 0; i < 5; ++i) {
        const std::string name = kRepairs[i];
        const Lesson l = lessonFromJson(readJson(std::string(SILEX_RESOURCES_DIR) + "/tutorials/" + name + ".json"), true);
        const DocState broken = docFromJson(readJson(std::string(SILEX_RESOURCES_DIR) + "/tutorials/" + name + ".silex"));
        const DocState fixed =
            docFromJson(readJson(std::string(SILEX_TESTDATA_DIR) + "/repair_" + std::to_string(i + 1) + "_fixed.silex"));
        EXPECT_LT(progressOf(l, broken), int(l.steps.size())) << name << ": the broken part already passes";
        EXPECT_EQ(progressOf(l, fixed), int(l.steps.size())) << name << ": the repaired part fails";
    }
}

TEST(V05Repair, RobustnessTestReallyRecomputes)
{
    // elastic sketch: both files pass the symmetry at 60 mm, only the repaired one at 80 mm
    const Lesson l = lessonFromJson(readJson(std::string(SILEX_RESOURCES_DIR) + "/tutorials/repair_1_esquisse_elastique.json"), true);
    const Json& robust = l.steps[0].criteria.back();
    ASSERT_EQ(robust.value("type", std::string{}), "robust");
    for (const auto& [file, expect] : {std::pair<std::string, bool>{std::string(SILEX_RESOURCES_DIR) + "/tutorials/repair_1_esquisse_elastique.silex", false},
                                       {std::string(SILEX_TESTDATA_DIR) + "/repair_1_fixed.silex", true}}) {
        const DocState d = docFromJson(readJson(file));
        ModelFacts f = collectFacts(d, *KernelEngine::computeSnapshot(d));
        EXPECT_TRUE(evaluateCriterion(Json{{"type", "holes_symmetric"}}, f)) << file;
        KernelEngine::enrichLessonFacts(f, d, {robust});
        EXPECT_EQ(evaluateCriterion(robust, f), expect) << file;
    }
}

// ================================================================== lot 5: lesson recorder

TEST(V05Recorder, LessonFromProjectIsValidAndReplays)
{
    const DocState full = docFromJson(readJson(std::string(SILEX_EXAMPLES_DIR) + "/de_parametrique.silex"));
    auto snap = KernelEngine::computeSnapshot(full);
    const Json lj = lessonFromProject(full, *snap, "de_test", "Mon dé", "My dice");
    const auto problems = validateLessonJson(lj);
    std::string all;
    for (const auto& p : problems)
        all += p + "\n";
    EXPECT_TRUE(problems.empty()) << all;
    const Lesson l = lessonFromJson(lj, true);
    ASSERT_EQ(l.steps.size(), full.features.size());
    // replaying the trainer's timeline step by step validates every step
    std::vector<bool> done;
    int active = 0;
    for (int m = 1; m <= int(full.features.size()); ++m) {
        DocState d = full;
        d.marker = m;
        active = updateLessonProgress(l, collectFacts(d, *KernelEngine::computeSnapshot(d)), done);
    }
    EXPECT_EQ(active, int(l.steps.size()));
    // and an empty model validates nothing
    std::vector<bool> none;
    EXPECT_EQ(updateLessonProgress(l, ModelFacts{}, none), 0);
    // the key dimensions were extracted (extrusion distance, fillet radius)
    const std::string dump = lj.dump();
    EXPECT_NE(dump.find("/distance"), std::string::npos);
    EXPECT_NE(dump.find("sketch_dof"), std::string::npos);
    EXPECT_NE(dump.find("body_volume"), std::string::npos);
}
