// v0.7 lot C — calibrated reference image (SketchCanvas).

#include <gtest/gtest.h>

#include <cmath>

#include "TestHelpers.h"

using namespace silex;
using namespace silex::test;

namespace {
void expectNear(Vec2 a, Vec2 b, double eps = 1e-9)
{
    EXPECT_NEAR(a.x, b.x, eps);
    EXPECT_NEAR(a.y, b.y, eps);
}
}  // namespace

TEST(Canvas, TwoPointCalibration)
{
    // 1000 x 500 px photo inserted at an arbitrary size and place
    SketchCanvas c = makeCanvas(1000, 500, 37.0, {5, -3});
    EXPECT_NEAR(c.heightMm, 18.5, 1e-12);
    const Vec2 p1{100, 250}, p2{600, 250};
    const Vec2 before = canvasPixelToSketch(c, p1);
    std::string err;
    ASSERT_TRUE(calibrateCanvas(c, p1, p2, 50.0, &err)) << err;
    EXPECT_DOUBLE_EQ(c.widthMm, 100.0);
    EXPECT_DOUBLE_EQ(c.heightMm, 50.0);
    // the first point did not move; the second is now exactly 50 mm away
    expectNear(canvasPixelToSketch(c, p1), before);
    const Vec2 a = canvasPixelToSketch(c, p1), b = canvasPixelToSketch(c, p2);
    EXPECT_NEAR(std::hypot(b.x - a.x, b.y - a.y), 50.0, 1e-9);

    // errors
    EXPECT_FALSE(calibrateCanvas(c, p1, p1, 50.0, &err));
    EXPECT_FALSE(calibrateCanvas(c, p1, p2, 0.0, &err));
    SketchCanvas none;
    EXPECT_FALSE(calibrateCanvas(none, p1, p2, 50.0, &err));
}

TEST(Canvas, PixelMappingWithRotationAndFlips)
{
    SketchCanvas c = makeCanvas(1000, 500, 100.0, {10, 20});
    // top-left pixel is up-left of the center (Y up in the sketch)
    expectNear(canvasPixelToSketch(c, {0, 0}), {10 - 50, 20 + 25});
    expectNear(canvasPixelToSketch(c, {500, 250}), {10, 20});
    for (double angle : {0.0, 30.0, -90.0, 145.0})
        for (bool fh : {false, true})
            for (bool fv : {false, true}) {
                c.angleDeg = angle;
                c.flipH = fh;
                c.flipV = fv;
                for (const Vec2 px : {Vec2{0, 0}, Vec2{1000, 500}, Vec2{123.5, 456.25}}) {
                    expectNear(canvasSketchToPixel(c, canvasPixelToSketch(c, px)), px, 1e-9);
                }
                // calibration still gives a 1:1 scale whatever the orientation
                SketchCanvas k = c;
                ASSERT_TRUE(calibrateCanvas(k, {100, 250}, {600, 250}, 50.0));
                const Vec2 a = canvasPixelToSketch(k, {100, 250}), b = canvasPixelToSketch(k, {600, 250});
                EXPECT_NEAR(std::hypot(b.x - a.x, b.y - a.y), 50.0, 1e-9);
                EXPECT_DOUBLE_EQ(k.widthMm, 100.0);
            }
}

TEST(Canvas, SavedAndReloadedInTheSilexFile)
{
    SketchParams sp;
    sp.sketch = makeRect(40, 20).sk;
    SketchCanvas c = makeCanvas(1000, 500, 80.0, {1.5, 2.5});
    ASSERT_TRUE(calibrateCanvas(c, {100, 250}, {600, 250}, 50.0));
    c.imagePath = "photos/support_casse.jpg";
    c.imageData = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg==";
    c.mime = "image/jpeg";
    c.opacity = 0.35;
    c.angleDeg = 12;
    c.flipH = true;
    sp.canvas = c;
    FeaturePtr sk = makeFeature(FeatureType::Sketch, "Photo", Json(sp));
    FeaturePtr ex = extrudeFeature(sk->id, 4);
    const DocState d = docOf({sk, ex});

    const DocState back = docFromJson(Json::parse(docToJson(d).dump()));
    const auto rp = back.features[0]->as<SketchParams>();
    ASSERT_TRUE(rp.canvas.has_value());
    EXPECT_EQ(rp.canvas->imageData, c.imageData);
    EXPECT_EQ(rp.canvas->imagePath, c.imagePath);
    EXPECT_EQ(rp.canvas->mime, "image/jpeg");
    EXPECT_EQ(rp.canvas->pxWidth, 1000);
    EXPECT_EQ(rp.canvas->pxHeight, 500);
    EXPECT_DOUBLE_EQ(rp.canvas->widthMm, 100.0);
    EXPECT_DOUBLE_EQ(rp.canvas->heightMm, 50.0);
    EXPECT_DOUBLE_EQ(rp.canvas->opacity, 0.35);
    EXPECT_DOUBLE_EQ(rp.canvas->angleDeg, 12);
    EXPECT_TRUE(rp.canvas->flipH);
    EXPECT_FALSE(rp.canvas->flipV);
    EXPECT_DOUBLE_EQ(rp.canvas->center.x, c.center.x);

    // the image is display only: the part rebuilds as without it
    const Analysis a = KernelEngine::analyze(back);
    ASSERT_EQ(a.bodies.size(), 1u);
    EXPECT_NEAR(a.bodies[0].volume, 40 * 20 * 4.0, 1e-6);

    // a sketch without image stays without "canvas" in the file
    SketchParams plain;
    plain.sketch = makeRect(10, 10).sk;
    EXPECT_FALSE(Json(plain).contains("canvas"));
}
