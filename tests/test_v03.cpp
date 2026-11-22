// Silex v0.3 non-regression tests.
//   Lot 1: expressions & global parameters (cascade recompute), measure, interference / clearance
//   Lot 2: Sweep (bent pipe) & Loft (square -> round adapter), BRepCheck validity
//   Lot 3: HLR projection of the reference bracket (3 views, hidden edges)
//   Lot 4: materials, 3MF export (2 bodies, mm, colours, watertight meshes), schema 0.3

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

#include "TestHelpers.h"
#include "kernel/Zip.h"
#include "model/Bindings.h"
#include "sketch/Parametric.h"

using namespace silex;
using namespace silex::test;

namespace {

void expectAllOk(const Analysis& a)
{
    for (const auto& r : a.results)
        EXPECT_EQ(r.status, FeatureStatus::Ok) << r.id << ": " << r.message;
}

const BodyMetrics* body(const Analysis& a, const Uuid& id)
{
    for (const auto& b : a.bodies)
        if (b.id == id)
            return &b;
    return nullptr;
}

/// w x h rectangle with lower-left corner `c` (unconstrained lines: good enough for the kernel).
Sketch rectAt(Vec2 c, double w, double h)
{
    Sketch s;
    const Vec2 p[4] = {c, c + Vec2{w, 0}, c + Vec2{w, h}, c + Vec2{0, h}};
    for (int i = 0; i < 4; ++i)
        s.geos.push_back(makeLine(p[i], p[(i + 1) % 4]));
    return s;
}

FeaturePtr block(Vec2 corner, double w, double h, double d, double z0 = 0, FeaturePtr* sketchOut = nullptr)
{
    SketchPlaneRef pl;
    pl.offset = z0;
    auto sk = sketchFeature(rectAt(corner, w, h), pl);
    if (sketchOut)
        *sketchOut = sk;
    return extrudeFeature(sk->id, d);
}

std::string readFile(const std::string& path)
{
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::filesystem::path tempFile(const std::string& name)
{
    return std::filesystem::temp_directory_path() / name;
}

}  // namespace

// ================================================================== Lot 1 — expressions

TEST(V03, ExpressionEvaluator)
{
    EXPECT_DOUBLE_EQ(evaluateExpression("2 * (3 + 4)"), 14.0);
    EXPECT_DOUBLE_EQ(evaluateExpression("-2^2"), -4.0);
    EXPECT_DOUBLE_EQ(evaluateExpression("2^3^2"), 512.0);  // right associative
    EXPECT_DOUBLE_EQ(evaluateExpression("2^-1"), 0.5);
    EXPECT_DOUBLE_EQ(evaluateExpression("10 mm + 1 cm"), 20.0);
    EXPECT_NEAR(evaluateExpression("sin(30)"), 0.5, 1e-12);
    EXPECT_NEAR(evaluateExpression("atan2(1, 1)"), 45.0, 1e-12);
    EXPECT_NEAR(evaluateExpression("pi"), kPi, 1e-15);
    EXPECT_DOUBLE_EQ(evaluateExpression("max(1, 7, 3) + min(4, 2)"), 9.0);
    EXPECT_THROW(evaluateExpression("1 / 0"), ExpressionError);
    EXPECT_THROW(evaluateExpression("2 * (3"), ExpressionError);
    EXPECT_THROW(evaluateExpression("inconnu + 1"), ExpressionError);
    EXPECT_TRUE(isPlainNumber("12.5 mm"));
    EXPECT_FALSE(isPlainNumber("jeu * 2"));
    EXPECT_EQ(expressionIdentifiers("a * b + sqrt(c) - pi"), (std::set<std::string>{"a", "b", "c"}));
    EXPECT_FALSE(isValidParameterName("2x"));
    EXPECT_FALSE(isValidParameterName("sin"));
    EXPECT_TRUE(isValidParameterName("epaisseur_paroi"));
}

TEST(V03, ParameterTableCrossReferencesAndCycles)
{
    std::vector<Parameter> ps{
        {"entraxe_x", "largeur - 2 * marge", "mm", "", 0, ""},
        {"largeur", "65", "mm", "", 0, ""},
        {"marge", "jeu_montage * 14", "mm", "", 0, ""},
        {"jeu_montage", "0.25", "mm", "", 0, ""},
        {"a", "b + 1", "mm", "", 0, ""},
        {"b", "a * 2", "mm", "", 0, ""},
        {"c", "inconnu", "mm", "", 0, ""},
    };
    evaluateParameters(ps);
    EXPECT_TRUE(ps[0].error.empty()) << ps[0].error;
    EXPECT_NEAR(ps[0].value, 65 - 2 * 3.5, 1e-12);  // 58
    EXPECT_NE(ps[4].error.find("Circular"), std::string::npos);
    EXPECT_NE(ps[5].error.find("Circular"), std::string::npos);
    EXPECT_FALSE(ps[6].error.empty());
    EXPECT_EQ(parameterValues(ps).count("a"), 0u);
}

TEST(V03, GlobalParameterCascadeRecompute)
{
    // A 40 x 30 plate whose width is the sketch dimension bound to "largeur" and whose thickness
    // is the extrusion distance bound to "epaisseur_paroi * 2".
    auto rect = makeRect(40, 30);
    for (auto& c : rect.sk.constraints)
        if (c.id == rect.widthConstraint)
            c.expr = "largeur";
    auto sk = sketchFeature(rect.sk);
    auto ex = extrudeFeature(sk->id, 1.0);
    auto exNode = std::make_shared<FeatureNode>(*ex);
    setFieldExpression(exNode->params, "/distance", "epaisseur_paroi * 2");
    DocState d = docOf({sk, exNode});
    d = SetParametersCommand({{"epaisseur_paroi", "2.0", "mm", "paroi", 0, ""}, {"largeur", "40", "mm", "", 0, ""}})
            .apply(d);
    d = applyParameters(d);
    EXPECT_NEAR(d.find(ex->id)->params["distance"].get<double>(), 4.0, 1e-12);
    Analysis a = KernelEngine::analyze(d);
    expectAllOk(a);
    EXPECT_NEAR(a.bodies.front().volume, 40 * 30 * 4.0, 1e-6);

    // Changing the global parameters re-solves the sketch and recomputes the timeline.
    const SetParametersCommand edit({{"epaisseur_paroi", "3", "mm", "", 0, ""}, {"largeur", "60 - 5", "mm", "", 0, ""}});
    d = ParametricCommand(edit).apply(d);
    EXPECT_NEAR(d.find(ex->id)->params["distance"].get<double>(), 6.0, 1e-12);
    a = KernelEngine::analyze(d);
    expectAllOk(a);
    EXPECT_NEAR(a.bodies.front().volume, 55 * 30 * 6.0, 1e-6);
    EXPECT_TRUE(a.bodies.front().valid);

    // Negative distance through an expression = reversed extrusion.
    const SetParametersCommand neg({{"epaisseur_paroi", "-1", "mm", "", 0, ""}, {"largeur", "55", "mm", "", 0, ""}});
    d = ParametricCommand(neg).apply(d);
    EXPECT_TRUE(d.find(ex->id)->params["reversed"].get<bool>());
    a = KernelEngine::analyze(d);
    EXPECT_NEAR(a.bodies.front().bbox.min.z, -2.0, 1e-6);

    // Bound pattern counts are rounded to integers.
    Json params = Json{{"dir1", {{"count", 2}}}};
    setFieldExpression(params, "/dir1/count", "n");
    auto lp = makeFeature(FeatureType::LinearPattern, "LP", params);
    DocState d2 = docOf({lp});
    d2.parameters = {{"n", "7 / 2", "", "", 0, ""}};
    d2 = applyParameters(d2);
    EXPECT_EQ(d2.features[0]->params["dir1"]["count"].get<int>(), 4);
}

// ================================================================== Lot 1 — measure & interference

TEST(V03, MeasureDistanceAngleAndCylinder)
{
    FeaturePtr sk;
    auto ex = block({0, 0}, 40, 30, 20, 0, &sk);
    Sketch hole;
    hole.geos.push_back(makeCircle({20, 15}, 4));  // Ø8
    SketchPlaneRef top;
    top.offset = 20;
    auto hs = sketchFeature(hole, top);
    ExtrudeParams hp;
    hp.profile.sketch = hs->id;
    hp.distance = 20;
    hp.reversed = true;
    hp.op = BooleanOp::Cut;
    auto cut = makeFeature(FeatureType::Extrude, "Hole", Json(hp));
    const DocState d = docOf({sk, ex, hs, cut});
    auto snap = KernelEngine::computeSnapshot(d);
    auto topF = pickFace(*snap, {20, 15, 20});
    auto bottomF = pickFace(*snap, {20, 15, 0});
    auto sideF = pickFace(*snap, {0, 15, 10});
    ASSERT_TRUE(topF && bottomF && sideF);

    MeasureRef a{MeasureRef::Kind::Face, {}, *topF}, b{MeasureRef::Kind::Face, {}, *bottomF};
    MeasureResult m = KernelEngine::measure(d, {a, b});
    ASSERT_TRUE(m.ok) << m.error;
    EXPECT_NEAR(m.distance, 20.0, 1e-7);
    EXPECT_NEAR(std::abs(m.delta.z), 20.0, 1e-7);
    EXPECT_TRUE(m.hasAngle);
    EXPECT_NEAR(m.angleDeg, 0.0, 1e-7);

    m = KernelEngine::measure(d, {a, MeasureRef{MeasureRef::Kind::Face, {}, *sideF}});
    ASSERT_TRUE(m.ok) << m.error;
    EXPECT_NEAR(m.angleDeg, 90.0, 1e-7);
    EXPECT_NEAR(m.distance, 0.0, 1e-7);

    // cylindrical face of the hole: diameter and centre
    std::optional<TopoRef> cyl;
    for (const auto& f : snap->bodies.front().mesh.faces)
        if (f.sig.type == 1)
            cyl = TopoRef{f.name, f.sig};
    ASSERT_TRUE(cyl);
    m = KernelEngine::measure(d, {MeasureRef{MeasureRef::Kind::Face, {}, *cyl}});
    ASSERT_TRUE(m.ok) << m.error;
    ASSERT_EQ(m.circles.size(), 1u);
    EXPECT_NEAR(m.circles[0].radius * 2, 8.0, 1e-7);
    EXPECT_NEAR(m.circles[0].center.x, 20.0, 1e-7);
    EXPECT_NEAR(m.circles[0].center.y, 15.0, 1e-7);
    EXPECT_NEAR(m.circles[0].center.z, 10.0, 1e-6);
}

TEST(V03, InterferenceDetectsOverlappingBlocks)
{
    FeaturePtr s1, s2;
    auto b1 = block({0, 0}, 20, 20, 20, 0, &s1);
    auto b2 = block({10, 0}, 20, 20, 20, 0, &s2);
    const DocState d = docOf({s1, b1, s2, b2});
    const InterferenceReport r = KernelEngine::checkInterference(d, {b1->id, b2->id}, true);
    ASSERT_TRUE(r.error.empty()) << r.error;
    ASSERT_EQ(r.pairs.size(), 1u);
    EXPECT_TRUE(r.collision);
    EXPECT_EQ(r.pairs[0].status, "collision");
    EXPECT_NEAR(r.pairs[0].overlapVolume, 10 * 20 * 20, 1e-6);
    EXPECT_FALSE(r.pairs[0].overlap.indices.empty());  // red overlap mesh for the viewport
}

TEST(V03, ClearanceBoxAndLid)
{
    // Box 60 x 40 x 25 hollowed (2 mm walls, open top) + lid 60 x 40 x 3 placed 0.25 mm above.
    FeaturePtr sBox, sLid, sTight;
    auto box = block({0, 0}, 60, 40, 25, 0, &sBox);
    const DocState pre = docOf({sBox, box});
    auto snap = KernelEngine::computeSnapshot(pre);
    ShellParams sp;
    sp.removedFaces = {*pickFace(*snap, {30, 20, 25})};
    sp.thickness = 2;
    auto shell = makeFeature(FeatureType::Shell, "Shell", Json(sp));
    auto lid = block({0, 0}, 60, 40, 3, 25.25, &sLid);
    const DocState d = docOf({sBox, box, shell, sLid, lid});
    InterferenceReport r = KernelEngine::checkInterference(d, {});
    ASSERT_TRUE(r.error.empty()) << r.error;
    ASSERT_EQ(r.pairs.size(), 1u);
    EXPECT_FALSE(r.collision);
    EXPECT_EQ(r.pairs[0].status, "ok");
    EXPECT_NEAR(r.pairs[0].clearance, 0.25, 1e-7);
    EXPECT_NEAR(r.minClearance, 0.25, 1e-7);

    // 0.10 mm: very tight fit (orange)
    auto tight = block({0, 0}, 60, 40, 3, 25.10, &sTight);
    r = KernelEngine::checkInterference(docOf({sBox, box, shell, sTight, tight}), {});
    ASSERT_EQ(r.pairs.size(), 1u);
    EXPECT_EQ(r.pairs[0].status, "tight");
    EXPECT_NEAR(r.pairs[0].clearance, 0.10, 1e-7);
}

// ================================================================== Lot 2 — sweep & loft

TEST(V03, SweepBentPipe)
{
    // Path in XY: 50 mm straight, 90° bend R30, 50 mm straight. Profile Ø10 on the YZ plane.
    Sketch path;
    path.geos.push_back(makeLine({0, 0}, {50, 0}));
    SketchGeo arc;
    arc.id = newUuid();
    arc.kind = GeoKind::Arc;
    arc.c = {50, 30};
    arc.r = 30;
    arc.a0 = -kPi / 2;
    arc.a1 = 0;
    path.geos.push_back(arc);
    path.geos.push_back(makeLine({80, 30}, {80, 80}));
    auto pathSk = sketchFeature(path);

    Sketch prof;
    prof.geos.push_back(makeCircle({0, 0}, 5));
    SketchPlaneRef yz;
    yz.kind = "YZ";
    auto profSk = sketchFeature(prof, yz);

    SweepParams sp;
    sp.profile.sketch = profSk->id;
    sp.path.sketch = pathSk->id;
    auto sweep = makeFeature(FeatureType::Sweep, "Sweep", Json(sp));
    const Analysis a = KernelEngine::analyze(docOf({pathSk, profSk, sweep}));
    expectAllOk(a);
    ASSERT_EQ(a.bodies.size(), 1u);
    EXPECT_TRUE(a.bodies[0].valid);
    const double length = 50 + 30 * kPi / 2 + 50;
    EXPECT_NEAR(a.bodies[0].volume, kPi * 25 * length, kPi * 25 * length * 1e-4);
    EXPECT_NEAR(a.bodies[0].bbox.max.y, 80.0, 1e-3);
    // TNP: caps and lateral face named from the profile curve
    const auto& names = a.bodies[0].faceNames;
    EXPECT_NE(std::find(names.begin(), names.end(), sweep->id + ":start"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), sweep->id + ":end"), names.end());
    EXPECT_TRUE(std::any_of(names.begin(), names.end(),
                            [&](const std::string& n) { return n.rfind(sweep->id + ":side(", 0) == 0; }));

    // Frenet and Fixed modes also produce a valid solid on this planar path
    for (const char* mode : {"Frenet"}) {
        sp.orientation = mode;
        auto s2 = makeFeature(FeatureType::Sweep, "Sweep", Json(sp), sweep->id);
        const Analysis b = KernelEngine::analyze(docOf({pathSk, profSk, s2}));
        expectAllOk(b);
        EXPECT_TRUE(b.bodies[0].valid);
    }
}

TEST(V03, LoftSquareToRound)
{
    Sketch sq = rectAt({-20, -20}, 40, 40);
    auto s1 = sketchFeature(sq);
    Sketch ci;
    ci.geos.push_back(makeCircle({0, 0}, 12));
    SketchPlaneRef up;
    up.offset = 50;
    auto s2 = sketchFeature(ci, up);
    LoftParams lp;
    lp.sections = {ProfileRef{s1->id, {}}, ProfileRef{s2->id, {}}};
    auto loft = makeFeature(FeatureType::Loft, "Loft", Json(lp));
    Analysis a = KernelEngine::analyze(docOf({s1, s2, loft}));
    expectAllOk(a);
    ASSERT_EQ(a.bodies.size(), 1u);
    EXPECT_TRUE(a.bodies[0].valid);
    EXPECT_NEAR(a.bodies[0].bbox.min.z, 0.0, 1e-6);
    EXPECT_NEAR(a.bodies[0].bbox.max.z, 50.0, 1e-6);
    // between the circular cylinder and the square prism
    EXPECT_GT(a.bodies[0].volume, kPi * 144 * 50);
    EXPECT_LT(a.bodies[0].volume, 1600.0 * 50);

    lp.ruled = true;
    auto ruled = makeFeature(FeatureType::Loft, "Loft", Json(lp), loft->id);
    a = KernelEngine::analyze(docOf({s1, s2, ruled}));
    expectAllOk(a);
    EXPECT_TRUE(a.bodies[0].valid);

    // same plane twice: clean error, no crash
    lp.sections = {ProfileRef{s1->id, {}}, ProfileRef{s1->id, {}}};
    auto bad = makeFeature(FeatureType::Loft, "Loft", Json(lp), loft->id);
    a = KernelEngine::analyze(docOf({s1, s2, bad}));
    EXPECT_EQ(a.result(loft->id)->status, FeatureStatus::Error);
}

// ================================================================== Lot 3 — drawing

TEST(V03, HlrProjectionOfReferenceBracket)
{
    std::ifstream f(std::filesystem::path(SILEX_DOCS_DIR) / "manual" / "equerre.silex");
    ASSERT_TRUE(f.good());
    const DocState d = docFromJson(Json::parse(f));
    const Analysis a = KernelEngine::analyze(d);
    ASSERT_EQ(a.bodies.size(), 1u);
    const BBox bb = a.bodies[0].bbox;

    const DrawingProjection p = KernelEngine::projectDrawing(d);
    ASSERT_TRUE(p.error.empty()) << p.error;
    ASSERT_EQ(p.views.size(), 3u);
    EXPECT_NEAR(p.volume, a.bodies[0].volume, 1e-6);
    EXPECT_NEAR(p.mass, a.bodies[0].volume * 1.24e-3, 1e-6);  // default PLA
    std::map<std::string, const DrawingViewData*> v;
    int hidden = 0, axes = 0;
    for (const auto& view : p.views) {
        v[view.name] = &view;
        int visible = 0;
        for (const auto& c : view.curves) {
            visible += c.kind == DrawLineKind::Visible;
            hidden += c.kind == DrawLineKind::Hidden;
            axes += c.kind == DrawLineKind::Axis;
        }
        EXPECT_GT(visible, 4) << view.name;
        EXPECT_FALSE(view.vertices.empty()) << view.name;
    }
    EXPECT_GT(hidden, 0);  // holes and hidden faces drawn dashed
    EXPECT_GT(axes, 0);    // centre lines of the holes
    // front = XZ, top = XY, right = YZ (extents match the bounding box)
    EXPECT_NEAR(v["front"]->width(), bb.max.x - bb.min.x, 1e-3);
    EXPECT_NEAR(v["front"]->height(), bb.max.z - bb.min.z, 1e-3);
    EXPECT_NEAR(v["top"]->width(), bb.max.x - bb.min.x, 1e-3);
    EXPECT_NEAR(v["top"]->height(), bb.max.y - bb.min.y, 1e-3);
    EXPECT_NEAR(v["right"]->width(), bb.max.y - bb.min.y, 1e-3);
    EXPECT_NEAR(v["right"]->height(), bb.max.z - bb.min.z, 1e-3);
    bool circle = false;
    for (const auto& view : p.views)
        circle = circle || !view.circles.empty();
    EXPECT_TRUE(circle);
}

// ================================================================== Lot 4 — materials & 3MF

TEST(V03, MaterialPresetsAndMass)
{
    ASSERT_EQ(materialPresets().size(), 7u);
    EXPECT_DOUBLE_EQ(defaultMaterial().density, 1.24);
    ASSERT_NE(findMaterialPreset("petg"), nullptr);
    EXPECT_DOUBLE_EQ(findMaterialPreset("PETG Noir")->density, 1.27);
    EXPECT_DOUBLE_EQ(findMaterialPreset("steel")->density, 7.85);
    EXPECT_NEAR(findMaterialPreset("resine")->a, 0.45f, 1e-6);
    EXPECT_NEAR(massGrams(1000.0, 1.24), 1.24, 1e-12);  // 1 cm3 of PLA

    FeaturePtr sk;
    auto ex = block({0, 0}, 10, 10, 10, 0, &sk);
    DocState d = docOf({sk, ex});
    d = SetBodyMaterialCommand(ex->id, *findMaterialPreset("aluminium")).apply(d);
    auto snap = KernelEngine::computeSnapshot(d);
    ASSERT_EQ(snap->bodies.size(), 1u);
    EXPECT_NEAR(snap->bodies[0].volume, 1000.0, 1e-6);
    EXPECT_NEAR(snap->bodies[0].area, 600.0, 1e-6);
    EXPECT_EQ(snap->bodies[0].material.name, "Aluminium");
    EXPECT_NEAR(snap->bodies[0].mass(), 2.70, 1e-9);
}

TEST(V03, ThreeMfExportTwoBodies)
{
    FeaturePtr sBase, sLid;
    auto base = block({0, 0}, 50, 30, 15, 0, &sBase);
    auto lid = block({0, 0}, 50, 30, 3, 15.3, &sLid);
    DocState d = docOf({sBase, base, sLid, lid});
    d = SetBodyMaterialCommand(base->id, *findMaterialPreset("PETG Noir")).apply(d);
    d = SetBodyMaterialCommand(lid->id, *findMaterialPreset("resine")).apply(d);

    const auto path = tempFile("silex_v03_test.3mf");
    std::filesystem::remove(path);
    ThreeMfOptions o;
    o.title = "Boitier & couvercle";
    o.designer = "Silex tests";
    const ExportSummary s = KernelEngine::export3mf(d, path.string(), o);
    ASSERT_TRUE(s.error.empty()) << s.error;
    EXPECT_EQ(s.objects, 2);
    const std::string zip = readFile(path.string());
    ASSERT_EQ(zip.size(), s.bytes);

    std::string types, rels, model;
    ASSERT_TRUE(kernel::zipRead(zip, "[Content_Types].xml", &types));
    ASSERT_TRUE(kernel::zipRead(zip, "_rels/.rels", &rels));
    ASSERT_TRUE(kernel::zipRead(zip, "3D/3dmodel.model", &model));
    EXPECT_NE(rels.find("/3D/3dmodel.model"), std::string::npos);
    EXPECT_NE(model.find("unit=\"millimeter\""), std::string::npos);
    EXPECT_NE(model.find("Boitier &amp; couvercle"), std::string::npos);
    EXPECT_NE(model.find("Silex v0.3"), std::string::npos);
    EXPECT_NE(model.find("displaycolor=\"#2A2C30FF\""), std::string::npos);  // opaque black PETG
    EXPECT_NE(model.find("displaycolor=\"#CFE6F273\""), std::string::npos);  // 45 % translucent resin
    EXPECT_NE(model.find("name=\"Body1\""), std::string::npos);
    EXPECT_NE(model.find("name=\"Body2\""), std::string::npos);

    // two separate, watertight objects: every edge shared by exactly two triangles
    const std::regex objRe("<object id=\"(\\d+)\"[^>]*>([\\s\\S]*?)</object>");
    const std::regex triRe("<triangle v1=\"(\\d+)\" v2=\"(\\d+)\" v3=\"(\\d+)\"/>");
    int objects = 0;
    for (auto it = std::sregex_iterator(model.begin(), model.end(), objRe); it != std::sregex_iterator(); ++it) {
        ++objects;
        const std::string body = (*it)[2];
        std::map<std::pair<int, int>, int> edges;
        int tris = 0;
        for (auto t = std::sregex_iterator(body.begin(), body.end(), triRe); t != std::sregex_iterator(); ++t) {
            const int v[3] = {std::stoi((*t)[1]), std::stoi((*t)[2]), std::stoi((*t)[3])};
            for (int k = 0; k < 3; ++k)
                ++edges[{std::min(v[k], v[(k + 1) % 3]), std::max(v[k], v[(k + 1) % 3])}];
            ++tris;
        }
        EXPECT_GE(tris, 12);
        for (const auto& [e, n] : edges)
            EXPECT_EQ(n, 2) << "open edge " << e.first << "-" << e.second;
    }
    EXPECT_EQ(objects, 2);
    std::filesystem::remove(path);
}

TEST(V03, SchemaV03RoundTripAndOlderFiles)
{
    FeaturePtr sk;
    auto ex = block({0, 0}, 20, 10, 5, 7.5, &sk);
    DocState d = docOf({sk, ex});
    d = SetParametersCommand({{"jeu_montage", "0.25", "mm", "jeu FDM", 0, ""}}).apply(d);
    d = SetBodyMaterialCommand(ex->id, *findMaterialPreset("abs")).apply(d);
    d = SetDrawingCommand(Json{{"sheet", "A3"}}).apply(d);
    const Json j = docToJson(d);
    EXPECT_EQ(j.at("schema_version"), "0.7");  // v0.7 since the "Trio Makers" (text, SVG/DXF, canvas)
    ASSERT_TRUE(j.contains("parameters"));
    EXPECT_EQ(j["parameters"][0]["name"], "jeu_montage");
    ASSERT_TRUE(j.contains("bodies"));
    EXPECT_EQ(j["bodies"][0]["material"]["name"], "ABS Gris");

    const DocState back = docFromJson(Json::parse(j.dump()));
    ASSERT_EQ(back.parameters.size(), 1u);
    EXPECT_NEAR(back.parameters[0].value, 0.25, 1e-12);
    EXPECT_EQ(back.parameters[0].comment, "jeu FDM");
    EXPECT_EQ(back.materialOf(ex->id).name, "ABS Gris");
    EXPECT_EQ(back.drawing.value("sheet", ""), "A3");
    const Analysis a = KernelEngine::analyze(back);
    EXPECT_NEAR(a.bodies[0].bbox.min.z, 7.5, 1e-9);  // sketch plane offset persisted

    // v0.2 documents still open (no parameters, default materials)
    std::ifstream f(std::filesystem::path(SILEX_DOCS_DIR) / "manual" / "v02_mirror.silex");
    ASSERT_TRUE(f.good());
    const DocState v02 = docFromJson(Json::parse(f));
    EXPECT_TRUE(v02.parameters.empty());
    EXPECT_FALSE(KernelEngine::analyze(v02).bodies.empty());
}

// ================================================================== projected edges & sketch axes

TEST(V03, ProjectedEdgeIsFixedForTheSolver)
{
    Sketch s;
    SketchGeo ext = makeLine({40, 0}, {40, 30});
    ext.construction = true;
    ext.ext = TopoRef{"E[some|edge]", {}};
    s.geos.push_back(ext);
    SketchGeo l = makeLine({10, 10}, {20, 12});
    s.geos.push_back(l);
    // the free line's end is glued to the projected edge, and the line is parallel to the X axis
    s.constraints.push_back(makeConstraint(ConstraintKind::Coincident, {l.id, PointPos::End}, {ext.id, PointPos::End}));
    s.constraints.push_back(makeConstraint(ConstraintKind::Parallel, {l.id}, {kXAxisId}));
    const SolveResult r = SketchSolver::solve(s);
    ASSERT_TRUE(r.converged) << r.message;
    EXPECT_EQ(r.state, SketchDofState::UnderConstrained);
    EXPECT_EQ(r.dof, 1);  // only the start x of the free line remains
    EXPECT_EQ(s.geos[0].p1, (Vec2{40, 0}));  // the projected edge did not move
    EXPECT_EQ(s.geos[0].p2, (Vec2{40, 30}));
    EXPECT_NEAR(s.geos[1].p2.x, 40, 1e-9);
    EXPECT_NEAR(s.geos[1].p2.y, 30, 1e-9);
    EXPECT_NEAR(s.geos[1].p1.y, 30, 1e-9);
    EXPECT_TRUE(r.fullyConstrainedGeos.count(ext.id));

    // persisted as a construction curve with its model reference
    const Json j = s.geos[0];
    EXPECT_EQ(j["ref"]["name"], "E[some|edge]");
    EXPECT_TRUE(j["construction"].get<bool>());
    EXPECT_TRUE(j.get<SketchGeo>().external());
}

TEST(V03, ProjectedEdgeFollowsTheModel)
{
    // 40 x 30 x 10 block; on its top plane, a Ø4 hole whose centre is dimensioned 5 mm from the
    // projected right edge of the block and 15 mm from the sketch X axis.
    auto rect = makeRect(40, 30);
    auto s1 = sketchFeature(rect.sk);
    auto ex = extrudeFeature(s1->id, 10);
    auto snap = KernelEngine::computeSnapshot(docOf({s1, ex}));
    auto edge = pickEdge(*snap, {40, 15, 10}, {0, 1, 0});
    ASSERT_TRUE(edge);

    Sketch s2;
    SketchGeo proj = makeLine({0, 0}, {1, 0});  // placeholder coordinates: the kernel projects the edge
    proj.construction = true;
    proj.ext = *edge;
    s2.geos.push_back(proj);
    SketchGeo hole = makeCircle({34, 14}, 2);
    s2.geos.push_back(hole);
    s2.constraints.push_back(makeConstraint(ConstraintKind::Distance, {hole.id, PointPos::Center}, {proj.id}, 5));
    s2.constraints.push_back(makeConstraint(ConstraintKind::Distance, {hole.id, PointPos::Center}, {kXAxisId}, 15));
    s2.constraints.push_back(makeConstraint(ConstraintKind::Diameter, {hole.id}, {}, 4));
    SketchPlaneRef top;
    top.offset = 10;
    auto s2f = sketchFeature(s2, top);
    ExtrudeParams cp;
    cp.profile.sketch = s2f->id;
    cp.distance = 10;
    cp.reversed = true;
    cp.op = BooleanOp::Cut;
    auto cut = makeFeature(FeatureType::Extrude, "Hole", Json(cp));

    auto holeX = [](const ModelSnapshot& sn) {
        for (const auto& f : sn.bodies.front().mesh.faces)
            if (f.sig.type == 1)  // cylinder
                return f.sig.center.x;
        return -1.0;
    };
    DocState d = docOf({s1, ex, s2f, cut});
    snap = KernelEngine::computeSnapshot(d);
    for (const auto& r : snap->results) {
        EXPECT_NE(r.status, FeatureStatus::Error) << r.message;
        EXPECT_TRUE(r.message.empty()) << r.message;
    }
    EXPECT_NEAR(holeX(*snap), 35.0, 1e-6);
    // the snapshot carries the re-projected sketch for the UI
    const SketchView* sv = snap->sketch(s2f->id);
    ASSERT_TRUE(sv);
    EXPECT_NEAR(sv->sketch.geos[0].p1.x, 40.0, 1e-9);

    // the block gets wider: the projected edge moves, the hole follows (still 5 mm from the edge)
    setDimension(rect.sk, rect.widthConstraint, 60);
    SketchParams sp1 = s1->as<SketchParams>();
    sp1.sketch = rect.sk;
    d = UpdateFeatureCommand(makeFeature(FeatureType::Sketch, "Sketch", Json(sp1), s1->id)).apply(d);
    snap = KernelEngine::computeSnapshot(d);
    for (const auto& r : snap->results)
        EXPECT_NE(r.status, FeatureStatus::Error) << r.message;
    EXPECT_NEAR(holeX(*snap), 55.0, 1e-6);
    EXPECT_TRUE(KernelEngine::analyze(d).bodies.front().valid);
}
