// v0.7 lot A — parametric text (EmbossText): relief, engraving, {parameters}, FDM guard.

#include <gtest/gtest.h>

#include <cmath>

#include "TestHelpers.h"
#include "model/Bindings.h"
#include "sketch/Parametric.h"

using namespace silex;
using namespace silex::test;

namespace {

constexpr double kFont = 6.0;

/// Block 60 x 30 x 10 whose length follows the parameter "largeur", plus a text on its top face.
DocState blockWithText(const std::string& text, double depth, double height = kFont)
{
    DocState d;
    Parameter largeur;
    largeur.name = "largeur";
    largeur.expression = "60";
    d.parameters = {largeur};

    PrimitiveParams b;
    b.length = 60;
    b.width = 30;
    b.height = 10;
    Json bj = Json(b);
    setFieldExpression(bj, "/length", "largeur");
    FeaturePtr box = makeFeature(FeatureType::Box, "Block", bj);
    d.features = {box};
    d.marker = 1;
    d = applyParameters(d);

    // the top face, picked like the UI does
    const auto snap = KernelEngine::computeSnapshot(d);
    const auto top = pickFace(*snap, {0, 0, 10});
    EXPECT_TRUE(top.has_value());
    EmbossTextParams tp;
    tp.plane.kind = "Face";
    tp.plane.face = *top;
    tp.text = text;
    tp.height = height;
    tp.depth = depth;
    d.features.push_back(makeFeature(FeatureType::EmbossText, "Text", Json(tp)));
    d.marker = 2;
    return applyParameters(d);
}

double volumeOf(const Analysis& a)
{
    EXPECT_EQ(a.bodies.size(), 1u);
    return a.bodies.empty() ? 0 : a.bodies[0].volume;
}

}  // namespace

TEST(Text, InterpolationOfParameters)
{
    const std::map<std::string, double> v{{"largeur", 60}, {"hauteur", 42.5}, {"version", 2}};
    const auto look = lookupIn(v);
    EXPECT_EQ(interpolateText("W={largeur}", look), "W=60");
    EXPECT_EQ(interpolateText("Box v{version} - {largeur}x{hauteur} mm", look), "Box v2 - 60x42.5 mm");
    EXPECT_EQ(interpolateText("{largeur / 7:2}", look), "8.57");
    EXPECT_EQ(interpolateText("{largeur * 2 + 1}", look), "121");
    EXPECT_EQ(interpolateText("{{literal}} {version}", look), "{literal} 2");
    EXPECT_EQ(interpolateText("no braces", look), "no braces");
    EXPECT_THROW(interpolateText("{inconnu}", look), ExpressionError);
    EXPECT_THROW(interpolateText("{largeur", look), ExpressionError);
}

TEST(Text, BundledFontIsFound)
{
    const auto m = KernelEngine::measureText("Silex", "Liberation Sans", 6);
    ASSERT_TRUE(m.ok) << m.error;
    EXPECT_NE(m.fontFile.find("LiberationSans-Regular.ttf"), std::string::npos) << m.fontFile;
    EXPECT_GT(m.area, 1.0);
    EXPECT_GT(m.faces, 4);
    // the area grows with the square of the height
    const auto m2 = KernelEngine::measureText("Silex", "Liberation Sans", 12);
    EXPECT_NEAR(m2.area, 4 * m.area, 1e-6 * m.area);
    // 'e' has a hole: "ee" must not be twice a filled blob
    const auto o = KernelEngine::measureText("o", "Liberation Sans", 10);
    ASSERT_TRUE(o.ok);
    EXPECT_LT(o.area, 0.5 * 10 * 10);  // a ring, far below its 10 x ~7 mm bounding box
}

TEST(Text, ReliefEngravingAndParameterUpdate)
{
    // reference block alone
    DocState plain = blockWithText("W={largeur}", 1.2);
    plain.features.pop_back();
    plain.marker = 1;
    const double v0 = volumeOf(KernelEngine::analyze(plain));
    EXPECT_NEAR(v0, 60 * 30 * 10.0, 1e-6);

    // relief +1.2 mm: exactly area x 1.2 added
    DocState relief = blockWithText("W={largeur}", 1.2);
    EXPECT_EQ(relief.features[1]->params.value("resolved", std::string()), "W=60");
    const auto area60 = KernelEngine::measureText("W=60", "Liberation Sans", kFont);
    ASSERT_TRUE(area60.ok) << area60.error;
    Analysis a = KernelEngine::analyze(relief);
    for (const auto& r : a.results)
        EXPECT_EQ(r.status, FeatureStatus::Ok) << r.message;
    ASSERT_EQ(a.bodies.size(), 1u);
    EXPECT_TRUE(a.bodies[0].valid);
    EXPECT_NEAR(a.bodies[0].volume - v0, area60.area * 1.2, 1e-4 * area60.area);
    EXPECT_NEAR(a.bodies[0].bbox.max.z, 11.2, 1e-6);

    // engraving -0.8 mm: exactly area x 0.8 removed, nothing above the face
    DocState engraved = blockWithText("W={largeur}", -0.8);
    a = KernelEngine::analyze(engraved);
    ASSERT_EQ(a.bodies.size(), 1u);
    EXPECT_TRUE(a.bodies[0].valid);
    EXPECT_NEAR(v0 - a.bodies[0].volume, area60.area * 0.8, 1e-4 * area60.area);
    EXPECT_NEAR(a.bodies[0].bbox.max.z, 10.0, 1e-6);

    // deterministic names of the letter faces
    const auto snap = KernelEngine::computeSnapshot(relief);
    int textFaces = 0;
    for (const auto& f : snap->bodies[0].mesh.faces)
        textFaces += f.name.find(relief.features[1]->id + ":text") != std::string::npos;
    EXPECT_GT(textFaces, 10);

    // largeur 60 -> 80: the block grows and the text follows ("W=80")
    std::vector<Parameter> params = relief.parameters;
    params[0].expression = "80";
    const DocState wider = applyParameters(SetParametersCommand(params).apply(relief));
    EXPECT_EQ(wider.features[1]->params.value("resolved", std::string()), "W=80");
    const auto area80 = KernelEngine::measureText("W=80", "Liberation Sans", kFont);
    ASSERT_TRUE(area80.ok);
    EXPECT_GT(std::abs(area80.area - area60.area), 0.01);  // "8" and "6" differ
    a = KernelEngine::analyze(wider);
    ASSERT_EQ(a.bodies.size(), 1u);
    EXPECT_TRUE(a.bodies[0].valid);
    EXPECT_NEAR(a.bodies[0].volume, 80 * 30 * 10.0 + area80.area * 1.2, 1e-4 * area80.area);
    for (const auto& r : a.results)
        EXPECT_EQ(r.status, FeatureStatus::Ok) << r.message;
}

TEST(Text, ThinStrokesRaiseAPrintWarning)
{
    // 1.5 mm letters: Liberation Sans stems are about 0.15 x the height, i.e. well under 0.4 mm
    const Analysis a = KernelEngine::analyze(blockWithText("tiny", 0.6, 1.5));
    const FeatureResult& r = a.results.back();
    EXPECT_EQ(r.status, FeatureStatus::Warning);
    EXPECT_NE(r.message.find("nozzle"), std::string::npos) << r.message;
    // 6 mm letters are fine
    EXPECT_EQ(KernelEngine::analyze(blockWithText("ok", 0.6, 6)).results.back().status, FeatureStatus::Ok);
}

TEST(Text, JsonRoundTripAndBadInput)
{
    EmbossTextParams tp;
    tp.text = "Line 1\nLigne {n}";
    tp.font = "LiberationSans-Regular.ttf";
    tp.align = "right";
    tp.angle = 30;
    tp.x = 4;
    tp.y = -2;
    const auto back = Json(tp).get<EmbossTextParams>();
    EXPECT_EQ(back.text, tp.text);
    EXPECT_EQ(back.font, tp.font);
    EXPECT_EQ(back.align, "right");
    EXPECT_DOUBLE_EQ(back.angle, 30);
    EXPECT_DOUBLE_EQ(back.y, -2);

    const auto unknown = KernelEngine::measureText("x", "No Such Font 12345", 5);
    EXPECT_TRUE(unknown.ok);  // falls back to the bundled font
    const auto empty = KernelEngine::measureText("   ", "Liberation Sans", 5);
    EXPECT_FALSE(empty.ok);
    // multi-line text with right alignment builds
    const auto ml = KernelEngine::measureText("AB\nC", "Liberation Sans", 5);
    ASSERT_TRUE(ml.ok) << ml.error;
}
