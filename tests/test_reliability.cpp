// Sprint "Fiabilité, Portabilité & Conformité": undo groups, autosave and crash recovery.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "TestHelpers.h"
#include "model/Recovery.h"

using namespace silex;
using namespace silex::test;
namespace fs = std::filesystem;

namespace {

DocState loadExample(const char* name)
{
    const auto path = fs::path(SILEX_EXAMPLES_DIR) / name;
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << path;
    std::stringstream ss;
    ss << in.rdbuf();
    return docFromJson(Json::parse(ss.str()));
}

FeaturePtr plateFeatures(std::vector<FeaturePtr>& out, double w, double h, double t)
{
    const Rect r = makeRect(w, h);
    FeaturePtr sk = sketchFeature(r.sk);
    FeaturePtr ex = extrudeFeature(sk->id, t);
    out = {sk, ex};
    return ex;
}

/// Fresh, empty folder under the system temp directory.
fs::path tempDir(const std::string& name)
{
    const fs::path d = fs::temp_directory_path() / ("silex_test_" + name);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

}  // namespace

// ------------------------------------------------------------------ command groups

TEST(Reliability, CommandGroupIsASingleUndoStep)
{
    History h;
    std::vector<FeaturePtr> fs;
    plateFeatures(fs, 40, 30, 5);

    h.execute(AddFeatureCommand(fs[0]));  // before the group: its own undo step
    ASSERT_TRUE(h.beginGroup("AI: plate"));
    EXPECT_FALSE(h.beginGroup("nested"));  // groups do not nest
    h.execute(AddFeatureCommand(fs[1]));
    h.execute(RenameFeatureCommand(fs[1]->id, "Plate"));
    h.execute(SetSuppressedCommand(fs[1]->id, false));
    EXPECT_EQ(h.groupSize(), 3);
    EXPECT_EQ(h.endGroup(), 3);
    EXPECT_FALSE(h.groupOpen());
    EXPECT_EQ(h.undoLabel(), "AI: plate");
    ASSERT_EQ(h.current().features.size(), 2u);
    EXPECT_EQ(h.current().features[1]->name, "Plate");

    // ONE undo reverts the whole group, the next one the sketch
    ASSERT_TRUE(h.undo());
    EXPECT_EQ(h.current().features.size(), 1u);
    ASSERT_TRUE(h.redo());
    EXPECT_EQ(h.current().features.size(), 2u);
    EXPECT_EQ(h.current().features[1]->name, "Plate");
    ASSERT_TRUE(h.undo());
    ASSERT_TRUE(h.undo());
    EXPECT_TRUE(h.current().features.empty());
    EXPECT_FALSE(h.canUndo());
}

TEST(Reliability, UndoInsideAGroupStepsBackOneCommand)
{
    // what the IPC dispatcher does when a command of the group fails to rebuild
    History h;
    std::vector<FeaturePtr> fs;
    plateFeatures(fs, 40, 30, 5);
    ASSERT_TRUE(h.beginGroup("AI"));
    h.execute(AddFeatureCommand(fs[0]));
    h.execute(AddFeatureCommand(fs[1]));
    ASSERT_TRUE(h.undo());  // rolls back the extrusion only
    EXPECT_TRUE(h.groupOpen());
    EXPECT_EQ(h.current().features.size(), 1u);
    EXPECT_EQ(h.endGroup(), 1);
    ASSERT_TRUE(h.canUndo());
    ASSERT_TRUE(h.undo());
    EXPECT_TRUE(h.current().features.empty());
    EXPECT_FALSE(h.canUndo());

    // a group whose commands were all undone leaves no empty undo step
    ASSERT_TRUE(h.beginGroup("empty"));
    h.execute(AddFeatureCommand(fs[0]));
    ASSERT_TRUE(h.undo());
    EXPECT_EQ(h.endGroup(), 0);
    EXPECT_FALSE(h.canUndo());
}

// ------------------------------------------------------------------ autosave / recovery

TEST(Reliability, AutosaveFileNames)
{
    using recovery::autosaveFileName;
    const std::string a = autosaveFileName("C:/parts/Équerre v2.silex", "s1");
    EXPECT_EQ(a, autosaveFileName("C:/parts/Équerre v2.silex", "other-session"));  // keyed by the file
    EXPECT_NE(a, autosaveFileName("D:/parts/Équerre v2.silex", "s1"));
    EXPECT_NE(autosaveFileName("", "s1"), autosaveFileName("", "s2"));  // two unsaved sessions
    EXPECT_EQ(a.rfind("Équerre_v2_", 0), 0u) << a;
    EXPECT_TRUE(a.size() > 14 && a.substr(a.size() - 14) == ".autosave.json");
    EXPECT_EQ(recovery::displayNameFor("C:/parts/bride.silex"), "bride");
    EXPECT_EQ(recovery::displayNameFor(""), "Untitled");
}

TEST(Reliability, AutosaveDetectedAndParametricTreeRestoredExactly)
{
    const fs::path dir = tempDir("recovery");

    // 1. a session opens a design and modifies it (parameter change + a new feature)
    History session;
    session.reset(loadExample("de_parametrique.silex"));
    const size_t before = session.current().features.size();
    ASSERT_GT(before, 5u);
    std::vector<FeaturePtr> fs;
    plateFeatures(fs, 12, 8, 3);
    session.execute(AddFeatureCommand(fs[0]));
    session.execute(AddFeatureCommand(fs[1]));
    ASSERT_TRUE(session.dirty());
    const DocState edited = session.current();

    // 2. the autosave timer fires
    const std::string source = "C:/Users/maker/Documents/de_parametrique.silex";
    recovery::AutosaveInfo meta;
    meta.sourcePath = source;
    meta.pid = 4242;
    const fs::path file = dir / recovery::autosaveFileName(source, "session-1");
    recovery::writeAutosave(file, edited, meta);
    ASSERT_TRUE(fs::is_regular_file(file));
    EXPECT_FALSE(fs::exists(fs::path(file).concat(".tmp")));  // atomic write leaves no temporary
    { std::ofstream junk(dir / "broken.autosave.json"); junk << "{ not json"; }  // ignored by the scan
    { std::ofstream other(dir / "notes.json"); other << "{}"; }                  // not an autosave

    // 3. Silex "crashes"; a new manager scans the folder at start-up
    const auto found = recovery::scan(dir);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].file, file);
    EXPECT_EQ(found[0].sourcePath, source);
    EXPECT_EQ(found[0].displayName, "de_parametrique");
    EXPECT_EQ(found[0].pid, 4242);
    EXPECT_GT(found[0].savedAtEpoch, 1700000000);
    EXPECT_EQ(found[0].savedAt.size(), 20u);  // 2026-11-22T10:42:00Z

    // 4. restore: the parametric tree is identical, feature by feature, and rebuilds identically
    recovery::AutosaveInfo read;
    History restored;
    restored.reset(recovery::readAutosave(file, &read));
    restored.markDirty();  // recovered work is unsaved work
    EXPECT_TRUE(restored.dirty());
    EXPECT_EQ(read.sourcePath, source);
    EXPECT_EQ(docToJson(restored.current()), docToJson(edited));
    ASSERT_EQ(restored.current().features.size(), before + 2);
    for (size_t i = 0; i < edited.features.size(); ++i) {
        EXPECT_EQ(restored.current().features[i]->id, edited.features[i]->id);
        EXPECT_EQ(restored.current().features[i]->params, edited.features[i]->params);
    }
    EXPECT_EQ(restored.current().parameters.size(), edited.parameters.size());

    const Analysis a = KernelEngine::analyze(edited), b = KernelEngine::analyze(restored.current());
    ASSERT_EQ(a.bodies.size(), b.bodies.size());
    for (size_t i = 0; i < a.bodies.size(); ++i)
        EXPECT_NEAR(a.bodies[i].volume, b.bodies[i].volume, 1e-6);
    for (const auto& r : b.results)
        EXPECT_NE(r.status, FeatureStatus::Error) << r.message;

    // 5. a newer autosave of the same file replaces the previous one
    session.execute(RenameFeatureCommand(fs[1]->id, "Renamed"));
    recovery::writeAutosave(file, session.current(), meta);
    const auto again = recovery::scan(dir);
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(recovery::readAutosave(file).features.back()->name, "Renamed");

    std::error_code ec;
    fs::remove_all(dir, ec);
}
