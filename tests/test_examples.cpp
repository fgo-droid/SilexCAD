// The models shipped in examples/ must always recompute cleanly.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "TestHelpers.h"

using namespace silex;
using namespace silex::test;

namespace {
DocState loadExample(const char* name)
{
    const auto path = std::filesystem::path(SILEX_EXAMPLES_DIR) / name;
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << path;
    std::stringstream ss;
    ss << in.rdbuf();
    return docFromJson(Json::parse(ss.str()));
}

void expectClean(const Analysis& a)
{
    for (const auto& r : a.results)
        EXPECT_EQ(r.status, FeatureStatus::Ok) << r.id << ": " << r.message;
    ASSERT_EQ(a.bodies.size(), 1u);
    EXPECT_TRUE(a.bodies[0].valid);
    EXPECT_EQ(a.bodies[0].solids, 1);
}
}  // namespace

TEST(Examples, DrilledPlate)
{
    const auto a = KernelEngine::analyze(loadExample("plaque_percee.silex"));
    expectClean(a);
    const double fillets = 4 * 8 * 8 * (1 - kPi / 4) * 8;  // 4 corners, r=8, h=8
    const double holes = (4 * kPi * 16 + kPi * 100) * 8;
    EXPECT_NEAR(a.bodies[0].volume, 100 * 60 * 8 - fillets - holes, kEps);
}

TEST(Examples, Enclosure)
{
    const auto a = KernelEngine::analyze(loadExample("boitier.silex"));
    expectClean(a);
    const double outer = 80 * 50 * 30 - 4 * 16 * (1 - kPi / 4) * 30;
    const double pocket = 74 * 44 * 27;
    EXPECT_NEAR(a.bodies[0].volume, outer - pocket, kEps);
}

TEST(Examples, ShoulderedBushing)
{
    const auto a = KernelEngine::analyze(loadExample("bague_epaulee.silex"));
    expectClean(a);
    // flange + shaft minus bore, before fillet/chamfer (both change it by < 1%)
    const double raw = kPi * (30 * 30 - 25) * 6 + kPi * (14 * 14 - 25) * 34;
    EXPECT_NEAR(a.bodies[0].volume, raw, raw * 0.01);
}
