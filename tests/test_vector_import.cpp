// SVG / DXF import into the sketcher (sketch/VectorImporter): parsing, units, merging, rigid
// blocks, and closed profiles with holes extruded through the B-Rep kernel.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "TestHelpers.h"
#include "sketch/VectorImporter.h"

using namespace silex;
using namespace silex::test;
namespace vi = silex::vectorimport;
namespace fs = std::filesystem;

namespace {

// 60 x 40 mm plate whose right end is a half circle (R20), with a circular (R8) and a
// rectangular (12 x 14) hole. 1 user unit = 1 mm.
const char* const kPlateSvg = R"SVG(<?xml version="1.0" encoding="UTF-8"?>
<!-- test plate -->
<svg xmlns="http://www.w3.org/2000/svg" width="80mm" height="50mm" viewBox="0 0 80 50">
  <defs><circle cx="1" cy="1" r="100"/></defs>
  <g stroke="black" fill="none">
    <path d="M10,5 H60 A20,20 0 0 1 60,45 H10 Z"/>
    <circle cx="30" cy="25" r="8"/>
    <rect x="40" y="18" width="12" height="14"/>
  </g>
  <text x="0" y="10">ignored</text>
</svg>
)SVG";

const double kPlateArea = 50.0 * 40.0 + kPi * 20.0 * 20.0 / 2.0 - kPi * 8.0 * 8.0 - 12.0 * 14.0;

fs::path writeTemp(const std::string& name, const std::string& text)
{
    const fs::path p = fs::temp_directory_path() / name;
    std::ofstream out(p, std::ios::binary);
    out << text;
    return p;
}

vi::ImportResult importText(Sketch& sk, const std::string& name, const std::string& text,
                            const vi::ImportOptions& o = {})
{
    const fs::path p = writeTemp(name, text);
    vi::ImportResult r = vi::importFile(sk, p, o);
    std::error_code ec;
    fs::remove(p, ec);
    return r;
}

/// ASCII DXF from group code / value pairs (CRLF, right-aligned codes like AutoCAD).
std::string dxfText(const std::vector<std::pair<int, std::string>>& pairs)
{
    std::string s;
    for (const auto& [code, value] : pairs) {
        std::string c = std::to_string(code);
        while (c.size() < 3)
            c.insert(c.begin(), ' ');
        s += c + "\r\n" + value + "\r\n";
    }
    return s;
}

const SketchGeo* firstOfKind(const Sketch& sk, GeoKind k)
{
    for (const auto& g : sk.geos)
        if (g.kind == k)
            return &g;
    return nullptr;
}

void expectPt(Vec2 got, Vec2 want, double tol = 1e-9)
{
    EXPECT_NEAR(got.x, want.x, tol);
    EXPECT_NEAR(got.y, want.y, tol);
}

void expectAllOk(const Analysis& a)
{
    for (const auto& r : a.results)
        EXPECT_EQ(r.status, FeatureStatus::Ok) << r.message;
}

}  // namespace

TEST(VectorImport, SvgPlateWithHolesExtrudes)
{
    Sketch sk;
    const vi::ImportResult r = importText(sk, "silex_vi_plate.svg", kPlateSvg);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_GE(r.arcs, 1);
    EXPECT_EQ(r.circles, 1);
    EXPECT_EQ(r.lines, 7);
    EXPECT_GT(r.merged, 0);
    EXPECT_FALSE(r.warnings.empty());  // <text> ignored
    EXPECT_NEAR(r.bboxMax.x - r.bboxMin.x, 70.0, 1e-9);
    EXPECT_NEAR(r.bboxMax.y - r.bboxMin.y, 40.0, 1e-9);
    expectPt((r.bboxMin + r.bboxMax) * 0.5, {0, 0});  // bbox center on the offset

    const SolveResult s = SketchSolver::solve(sk);
    EXPECT_TRUE(s.converged) << s.message;
    EXPECT_TRUE(s.conflicting.empty());
    EXPECT_TRUE(s.redundant.empty());

    // plate (outer contour minus holes) + the two hole interiors
    auto f = sketchFeature(sk);
    auto snap = KernelEngine::computeSnapshot(docOf({f}));
    ASSERT_EQ(snap->profiles.size(), 3u);
    const SketchGeo* arc = firstOfKind(sk, GeoKind::Arc);
    ASSERT_NE(arc, nullptr);
    std::string plate;
    for (const auto& pv : snap->profiles)
        if (pv.regionKey.find(arc->id) != std::string::npos)
            plate = pv.regionKey;
    ASSERT_FALSE(plate.empty());

    const auto a = KernelEngine::analyze(docOf({f, extrudeFeature(f->id, 5, BooleanOp::NewBody, {plate})}));
    expectAllOk(a);
    ASSERT_EQ(a.bodies.size(), 1u);
    EXPECT_TRUE(a.bodies.front().valid);
    const double expected = kPlateArea * 5.0;
    EXPECT_NEAR(a.bodies.front().volume, expected, 1e-3 * expected);
}

TEST(VectorImport, DxfPolylineWithBulgeExtrudes)
{
    // 50 x 30 mm rectangle whose right side is a half circle (bulge 1 = 180 deg CCW)
    const std::string text = dxfText({{0, "SECTION"},  {2, "HEADER"},   {9, "$INSUNITS"}, {70, "4"},
                                      {0, "ENDSEC"},   {0, "SECTION"},  {2, "ENTITIES"},  {0, "LWPOLYLINE"},
                                      {8, "0"},        {90, "4"},       {70, "1"},        {10, "0"},
                                      {20, "0"},       {10, "50"},      {20, "0"},        {42, "1"},
                                      {10, "50"},      {20, "30"},      {10, "0"},        {20, "30"},
                                      {0, "ENDSEC"},   {0, "EOF"}});
    Sketch sk;
    const vi::ImportResult r = importText(sk, "silex_vi_bulge.dxf", text);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.lines, 3);
    EXPECT_EQ(r.arcs, 1);
    EXPECT_EQ(r.merged, 4);
    EXPECT_NEAR(r.bboxMax.x - r.bboxMin.x, 65.0, 1e-9);

    const SolveResult s = SketchSolver::solve(sk);
    EXPECT_TRUE(s.converged) << s.message;

    auto f = sketchFeature(sk);
    const auto a = KernelEngine::analyze(docOf({f, extrudeFeature(f->id, 5)}));
    expectAllOk(a);
    ASSERT_EQ(a.bodies.size(), 1u);
    EXPECT_TRUE(a.bodies.front().valid);
    const double expected = (50.0 * 30.0 + kPi * 15.0 * 15.0 / 2.0) * 5.0;
    EXPECT_NEAR(a.bodies.front().volume, expected, 1e-3 * expected);
}

TEST(VectorImport, RigidBlockOnlyTranslates)
{
    Sketch sk;
    vi::ImportOptions o;
    o.rigid = true;
    o.offset = {3, -2};
    const vi::ImportResult r = importText(sk, "silex_vi_rigid.svg", kPlateSvg, o);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_FALSE(r.anchor.empty());

    SolveResult s = SketchSolver::solve(sk);
    EXPECT_TRUE(s.converged) << s.message;
    EXPECT_TRUE(s.conflicting.empty());
    EXPECT_TRUE(s.redundant.empty());
    EXPECT_EQ(s.dof, 2);

    const SketchGeo* anchor = sk.find(r.anchor);
    ASSERT_NE(anchor, nullptr);
    expectPt(anchor->p1, {3, -2}, 1e-7);  // nothing moved
    const Vec2 a0 = anchor->p1;
    const Vec2 circle0 = firstOfKind(sk, GeoKind::Circle)->c;
    const Vec2 arc0 = firstOfKind(sk, GeoKind::Arc)->c;
    const double arcR0 = firstOfKind(sk, GeoKind::Arc)->r;
    const double d0 = (circle0 - arc0).length();

    const Vec2 move{12.5, -7.25};
    sk.constraints.push_back(makeConstraint(ConstraintKind::DistanceX, {kOriginId, PointPos::Start},
                                            {r.anchor, PointPos::Start}, a0.x + move.x));
    sk.constraints.push_back(makeConstraint(ConstraintKind::DistanceY, {kOriginId, PointPos::Start},
                                            {r.anchor, PointPos::Start}, a0.y + move.y));
    s = SketchSolver::solve(sk);
    EXPECT_TRUE(s.converged) << s.message;
    EXPECT_TRUE(s.conflicting.empty());
    EXPECT_TRUE(s.redundant.empty());
    EXPECT_EQ(s.dof, 0);

    expectPt(sk.find(r.anchor)->p1, a0 + move, 1e-6);
    const Vec2 circle1 = firstOfKind(sk, GeoKind::Circle)->c;
    const Vec2 arc1 = firstOfKind(sk, GeoKind::Arc)->c;
    expectPt(circle1, circle0 + move, 1e-6);
    expectPt(arc1, arc0 + move, 1e-6);
    EXPECT_NEAR((circle1 - arc1).length(), d0, 1e-6);
    EXPECT_NEAR(firstOfKind(sk, GeoKind::Arc)->r, arcR0, 1e-6);
}

TEST(VectorImport, ParserEdgeCases)
{
    const std::string svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100mm" height="100mm" viewBox="0 0 100 100">
  <path d="M10,10l1.5.5-1-2 1e1,0z"/>
  <g transform="translate(10,5) scale(2)"><line x1="0" y1="0" x2="3" y2="4"/></g>
  <circle cx="5" cy="5" r="2" transform="scale(3)"/>
  <circle cx="5" cy="5" r="2" style="display:none"/>
</svg>)SVG";
    std::vector<std::string> warnings;
    const auto shapes = vi::parseSvg(svg, &warnings);
    ASSERT_EQ(shapes.size(), 6u);
    // relative lineto with compact numbers (mm, Y flipped)
    for (size_t i = 0; i < 4; ++i)
        EXPECT_EQ(shapes[i].kind, GeoKind::Line);
    expectPt(shapes[0].p1, {10, -10});
    expectPt(shapes[0].p2, {11.5, -10.5});
    expectPt(shapes[1].p2, {10.5, -8.5});
    expectPt(shapes[2].p2, {20.5, -8.5});
    expectPt(shapes[3].p2, {10, -10});
    // composed group transform
    expectPt(shapes[4].p1, {10, -5});
    expectPt(shapes[4].p2, {16, -13});
    // circle under a uniform scale stays a circle
    EXPECT_EQ(shapes[5].kind, GeoKind::Circle);
    expectPt(shapes[5].c, {15, -15});
    EXPECT_NEAR(shapes[5].r, 6.0, 1e-9);

    // default unit: 96 user units per inch
    const auto px = vi::parseSvg(R"(<svg><line x1="0" y1="0" x2="96" y2="0"/></svg>)");
    ASSERT_EQ(px.size(), 1u);
    EXPECT_NEAR((px[0].p2 - px[0].p1).length(), 25.4, 1e-9);

    // cubic Bezier: discretised within the chord tolerance, end points exact
    const auto bez = vi::parseSvg(
        R"(<svg width="20mm" height="20mm" viewBox="0 0 20 20"><path d="M0,0 C0,10 10,10 10,0"/></svg>)");
    ASSERT_GT(bez.size(), 4u);
    expectPt(bez.front().p1, {0, 0});
    expectPt(bez.back().p2, {10, 0});

    // DXF in inches
    const auto inch = vi::parseDxf(dxfText({{0, "SECTION"}, {2, "HEADER"}, {9, "$INSUNITS"}, {70, "1"},
                                            {0, "ENDSEC"}, {0, "SECTION"}, {2, "ENTITIES"}, {0, "LINE"},
                                            {10, "0"}, {20, "0"}, {11, "1"}, {21, "0"}, {0, "ENDSEC"},
                                            {0, "EOF"}}));
    ASSERT_EQ(inch.size(), 1u);
    EXPECT_NEAR((inch[0].p2 - inch[0].p1).length(), 25.4, 1e-9);

    // end point merging: 0.005 mm apart -> merged, 0.05 mm apart -> kept apart
    auto twoLines = [](const char* x) {
        return std::string(R"(<svg width="100mm" height="100mm" viewBox="0 0 100 100">)") +
               R"(<line x1="0" y1="0" x2="10" y2="0"/><line x1=")" + x + R"(" y1="0" x2="20" y2="0"/></svg>)";
    };
    {
        Sketch sk;
        const auto r = vi::importShapes(sk, vi::parseSvg(twoLines("10.005")), {});
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_EQ(r.merged, 1);
        ASSERT_EQ(r.constraints.size(), 1u);
        ASSERT_EQ(sk.geos.size(), 2u);
        EXPECT_EQ(sk.geos[0].p2, sk.geos[1].p1);  // snapped to one location
    }
    {
        Sketch sk;
        const auto r = vi::importShapes(sk, vi::parseSvg(twoLines("10.05")), {});
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_EQ(r.merged, 0);
        EXPECT_TRUE(r.constraints.empty());
    }

    // nothing importable / missing file: error, no exception
    {
        Sketch sk;
        const auto r = importText(sk, "silex_vi_empty.svg", R"(<svg width="10mm" height="10mm"></svg>)");
        EXPECT_FALSE(r.ok);
        EXPECT_FALSE(r.error.empty());
        const auto r2 = vi::importFile(sk, fs::temp_directory_path() / "silex_vi_missing.dxf", {});
        EXPECT_FALSE(r2.ok);
        EXPECT_TRUE(sk.geos.empty());
    }
}
