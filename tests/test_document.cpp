#include <gtest/gtest.h>

#include "TestHelpers.h"

using namespace silex;
using namespace silex::test;

TEST(Document, AddUndoRedoAndMarker)
{
    History h;
    auto rect = makeRect(20, 10);
    auto sk = sketchFeature(rect.sk);
    h.execute(AddFeatureCommand(sk));
    h.execute(AddFeatureCommand(extrudeFeature(sk->id, 5)));
    ASSERT_EQ(h.current().features.size(), 2u);
    EXPECT_EQ(h.current().marker, 2);

    h.execute(MoveMarkerCommand(1));
    EXPECT_FALSE(h.current().isActive(h.current().features[1]->id));
    // Inserting while rolled back inserts at the marker (Fusion behaviour)
    h.execute(AddFeatureCommand(sketchFeature(rect.sk)));
    EXPECT_EQ(h.current().features.size(), 3u);
    EXPECT_EQ(h.current().marker, 2);
    EXPECT_EQ(h.current().features[1]->type, FeatureType::Sketch);

    ASSERT_TRUE(h.undo());
    ASSERT_TRUE(h.undo());
    EXPECT_EQ(h.current().marker, 2);
    ASSERT_TRUE(h.redo());
    EXPECT_EQ(h.current().marker, 1);
}

TEST(Document, JsonRoundTrip)
{
    auto rect = makeRect(20, 10);
    auto sk = sketchFeature(rect.sk);
    DocState s = docOf({sk, extrudeFeature(sk->id, 5)});
    const std::string text = docToJson(s).dump(2);
    DocState back = docFromJson(Json::parse(text));
    ASSERT_EQ(back.features.size(), 2u);
    EXPECT_EQ(back.features[0]->id, sk->id);
    EXPECT_EQ(docToJson(back), docToJson(s));
    const auto sp = back.features[0]->as<SketchParams>();
    EXPECT_EQ(sp.sketch.geos.size(), 4u);
    EXPECT_EQ(sp.sketch.constraints.size(), rect.sk.constraints.size());
}

TEST(Document, DependenciesAndReorderGuard)
{
    auto rect = makeRect(20, 10);
    auto sk = sketchFeature(rect.sk);
    auto ex = extrudeFeature(sk->id, 5);
    DocState s = docOf({sk, ex});
    EXPECT_TRUE(dependenciesOf(*ex).count(sk->id));
    EXPECT_TRUE(dependentsOf(s, sk->id).count(ex->id));
    // Moving the extrusion before its sketch would break the DAG: refused.
    DocState r = ReorderFeatureCommand(ex->id, 0).apply(s);
    EXPECT_EQ(r.features[0]->id, sk->id);
}
