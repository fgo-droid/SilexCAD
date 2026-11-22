// End-to-end IPC test: starts `silex --headless`, builds a drilled, filleted box through the
// local pipe exactly like an MCP agent would, then checks validity, volume, TNP survival,
// offscreen capture and STEP export.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <nlohmann/json.hpp>

#include "model/Recovery.h"

using Json = nlohmann::json;

namespace {

constexpr double kPi = 3.14159265358979323846;

/// silex(.exe): $SILEX_EXE, next to this test executable (bin/<config>), then the build tree.
QString silexExe()
{
    const QString env = qEnvironmentVariable("SILEX_EXE");
    if (!env.isEmpty())
        return env;
#ifdef Q_OS_WIN
    const QString beside = QDir(QCoreApplication::applicationDirPath()).filePath("silex.exe");
#else
    const QString beside = QDir(QCoreApplication::applicationDirPath()).filePath("silex");
#endif
    if (QFileInfo::exists(beside) || qEnvironmentVariable("SILEX_PORTABLE_ONLY") == "1")
        return beside;
    return QString::fromUtf8(SILEX_BUILD_EXE);
}

/// examples/ deployed next to the executables (resources/examples), else the source tree.
QString examplesDir()
{
    const QString beside = QDir(QCoreApplication::applicationDirPath()).filePath("resources/examples");
    if (QFileInfo(beside).isDir() || qEnvironmentVariable("SILEX_PORTABLE_ONLY") == "1")
        return beside;
    return QString::fromUtf8(SILEX_SRC_EXAMPLES_DIR);
}

class SilexProcess
{
public:
    /// `extra`: more arguments. `gui`: the windowed application (offscreen) instead of --headless.
    explicit SilexProcess(const QStringList& extra = {}, bool gui = false,
                          const QProcessEnvironment& env = QProcessEnvironment::systemEnvironment())
    {
        static int n = 0;
        m_pipe = QString("silex_test_%1_%2").arg(QCoreApplication::applicationPid()).arg(++n);
        m_proc.setProgram(silexExe());
        m_proc.setArguments(QStringList{gui ? "--no-recovery" : "--headless", "--pipe", m_pipe} + extra);
        m_proc.setProcessEnvironment(env);
        m_proc.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        m_proc.start();
        if (!m_proc.waitForStarted(20000))
            return;
        for (int i = 0; i < 200 && !m_sock.isOpen(); ++i) {
#ifdef Q_OS_WIN
            m_sock.connectToServer("\\\\.\\pipe\\" + m_pipe);
#else
            m_sock.connectToServer("/tmp/" + m_pipe + ".sock");
#endif
            if (m_sock.waitForConnected(250))
                break;
            QThread::msleep(100);
        }
    }

    ~SilexProcess()
    {
        if (m_sock.state() == QLocalSocket::ConnectedState) {
            try {
                call("shutdown");
            }
            catch (...) {
            }
        }
        if (!m_proc.waitForFinished(10000))
            m_proc.kill();
    }

    bool connected() const { return m_sock.state() == QLocalSocket::ConnectedState; }

    /// Returns "result"; throws std::runtime_error with the error message.
    Json call(const std::string& method, Json params = Json::object())
    {
        const Json reply = callRaw(method, std::move(params));
        if (reply.contains("error"))
            throw std::runtime_error(method + ": " + reply["error"]["message"].get<std::string>());
        return reply["result"];
    }

    /// The whole JSON-RPC reply ({"id", "result"} or {"id", "error"}).
    Json callRaw(const std::string& method, Json params = Json::object())
    {
        const Json req{{"id", ++m_id}, {"method", method}, {"params", std::move(params)}};
        const std::string line = req.dump() + "\n";
        m_sock.write(line.data(), qint64(line.size()));
        m_sock.flush();
        while (!m_buf.contains('\n')) {
            if (!m_sock.waitForReadyRead(120000))
                throw std::runtime_error("timeout waiting for " + method);
            m_buf += m_sock.readAll();
        }
        const int nl = int(m_buf.indexOf('\n'));
        const Json reply = Json::parse(m_buf.left(nl).toStdString());
        m_buf.remove(0, nl + 1);
        EXPECT_EQ(reply["id"], m_id);
        return reply;
    }

    QProcess& process() { return m_proc; }
    /// Simulates a crash: the process is killed without any chance to clean up.
    void crash()
    {
        m_proc.kill();
        m_proc.waitForFinished(10000);
        m_sock.abort();
    }

private:
    QString m_pipe;
    QProcess m_proc;
    QLocalSocket m_sock;
    QByteArray m_buf;
    int m_id = 0;
};

double expectedVolume(double h, double holeDepth)
{
    const double fillets = 4 * 16 * (1 - kPi / 4) * h;  // R4 on 4 vertical edges
    return 80 * 50 * h - fillets - kPi * 64 * holeDepth;  // hole d16
}

}  // namespace

TEST(Ipc, AgentBuildsDrilledFilletedBox)
{
    int argc = 1;
    char name[] = "silex_ipc_tests";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);

    SilexProcess silex;
    ASSERT_TRUE(silex.connected()) << "could not connect to silex --headless";

    const Json ping = silex.call("ping");
    EXPECT_EQ(ping["name"], "Silex");

    // 1. base sketch: fully constrained 80 x 50 rectangle at the origin
    const std::string sk = silex.call("create_sketch", {{"plane_or_face_id", "XY"}, {"name", "Base"}})["sketch_id"];
    const Json geo = silex.call("sketch_add_geometry",
                                {{"sketch_id", sk},
                                 {"elements", Json::array({{{"type", "rectangle"}, {"corner", {0, 0}}, {"width", 80}, {"height", 50}}})}});
    const Json edges = geo["elements"][0]["edges"];
    const Json cons = silex.call(
        "sketch_add_constraints",
        {{"sketch_id", sk},
         {"constraints", Json::array({{{"type", "coincident"}, {"a", edges["bottom"].get<std::string>() + ":start"}, {"b", "origin"}},
                                      {{"type", "distance"}, {"a", edges["bottom"]}, {"value", 80}},
                                      {{"type", "distance"}, {"a", edges["left"]}, {"value", 50}}})}});
    EXPECT_EQ(cons["dof"], 0);
    EXPECT_EQ(cons["state"], "fully_constrained");

    // 2. extrude 30 mm
    const Json ex = silex.call("extrude", {{"sketch_id", sk}, {"distance_mm", 30}, {"operation", "new_body"}});
    const std::string extrudeId = ex["feature_id"], body = ex["body_id"];
    EXPECT_EQ(ex["status"], "OK");

    // 3. fillet the 4 vertical edges (persistent ids from query_topology)
    const Json vertical = silex.call("query_topology", {{"body_id", body}, {"entity_type", "edge"}, {"filter", "vertical"}});
    ASSERT_EQ(vertical["count"], 4);
    Json edgeIds = Json::array();
    for (const auto& e : vertical["entities"])
        edgeIds.push_back(e["id"]);
    const Json fil = silex.call("fillet_or_chamfer", {{"type", "fillet"}, {"edge_ids", edgeIds}, {"radius_or_distance_mm", 4}});
    EXPECT_EQ(fil["status"], "OK");

    // 4. hole: sketch on the top face, circle d16, cut 30 mm down
    const Json top = silex.call("query_topology", {{"body_id", body}, {"entity_type", "face"}, {"filter", "top"}});
    ASSERT_EQ(top["count"], 1);
    const std::string sk2 = silex.call("create_sketch", {{"plane_or_face_id", top["entities"][0]["id"]}})["sketch_id"];
    silex.call("sketch_add_geometry",
               {{"sketch_id", sk2}, {"elements", Json::array({{{"type", "circle"}, {"center", {40, 25}}, {"radius", 8}}})}});
    const Json cut = silex.call("extrude", {{"sketch_id", sk2}, {"distance_mm", -30}, {"operation", "cut"}});
    EXPECT_EQ(cut["status"], "OK");

    // 5. validity + volume
    Json info = silex.call("inspect_body", {{"body_id", body}});
    EXPECT_TRUE(info["valid"].get<bool>());
    EXPECT_NEAR(info["volume_mm3"].get<double>(), expectedVolume(30, 30), 1e-6);

    // 6. TNP: change the height, fillets and the sketch on the top face must follow
    const Json upd = silex.call("update_feature_parameter", {{"feature_id", extrudeId}, {"param_name", "distance"}, {"new_value", 40}});
    EXPECT_EQ(upd["status"], "OK");
    info = silex.call("inspect_body", {{"body_id", body}});
    EXPECT_TRUE(info["valid"].get<bool>());
    EXPECT_NEAR(info["volume_mm3"].get<double>(), expectedVolume(40, 30), 1e-6);
    const Json tree = silex.call("get_model_tree");
    for (const auto& f : tree["features"])
        EXPECT_EQ(f["status"], "OK") << f.dump();

    // 7. undo restores the previous height
    silex.call("undo");
    info = silex.call("inspect_body", {{"body_id", body}});
    EXPECT_NEAR(info["volume_mm3"].get<double>(), expectedVolume(30, 30), 1e-6);

    // 8. offscreen capture
    const Json img = silex.call("capture_viewport", {{"view", "iso"}, {"width", 320}, {"height", 240}});
    EXPECT_EQ(img["format"], "png");
    EXPECT_EQ(img["data"].get<std::string>().rfind("iVBORw0KGgo", 0), 0u);  // PNG signature

    // 8b. regression: a second capture after the geometry changed (stale GPU mesh => crash)
    silex.call("update_feature_parameter", {{"feature_id", extrudeId}, {"param_name", "distance"}, {"new_value", 36}});
    const Json img2 = silex.call("capture_viewport", {{"view", "front"}, {"width", 320}, {"height", 240}});
    EXPECT_EQ(img2["data"].get<std::string>().rfind("iVBORw0KGgo", 0), 0u);
    silex.call("undo");

    // 9. STEP + STL export
    const QString step = QDir::temp().filePath("silex_ipc_box.step");
    const QString stl = QDir::temp().filePath("silex_ipc_box.stl");
    silex.call("export_step", {{"file_path", step.toStdString()}, {"body_id", body}});
    silex.call("export_stl", {{"file_path", stl.toStdString()}, {"body_id", body}});
    EXPECT_GT(QFileInfo(step).size(), 1000);
    EXPECT_GT(QFileInfo(stl).size(), 1000);

    // 10. errors are reported, not fatal
    const Json topEdges = silex.call("query_topology", {{"body_id", body}, {"entity_type", "edge"}, {"filter", "top"}});
    ASSERT_GT(topEdges["count"].get<int>(), 0);
    EXPECT_THROW(silex.call("fillet_or_chamfer", {{"type", "fillet"}, {"edge_ids", Json::array({topEdges["entities"][0]["id"]})}, {"radius_or_distance_mm", 500}}),
                 std::runtime_error);
    EXPECT_THROW(silex.call("no_such_method"), std::runtime_error);
    info = silex.call("inspect_body", {{"body_id", body}});
    EXPECT_TRUE(info["valid"].get<bool>());
}

// v0.2 through the pipe: circular pattern of a hole (count 6 -> 8), mirror, linear pattern.
TEST(Ipc, V02PatternsAndMirrorHeadless)
{
    int argc = 1;
    char name[] = "silex_ipc_tests";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    SilexProcess silex;
    ASSERT_TRUE(silex.connected());

    auto circle = [&](const std::string& sk, double x, double y, double r) {
        silex.call("sketch_add_geometry",
                   {{"sketch_id", sk}, {"elements", Json::array({{{"type", "circle"}, {"center", {x, y}}, {"radius", r}}})}});
    };
    // flange Ø80 x 8 with one Ø6 hole at r = 30
    const std::string s1 = silex.call("create_sketch", {{"plane_or_face_id", "XY"}})["sketch_id"];
    circle(s1, 0, 0, 40);
    const std::string body = silex.call("extrude", {{"sketch_id", s1}, {"distance_mm", 8}})["body_id"];
    const std::string s2 = silex.call("create_sketch", {{"plane_or_face_id", "XY"}})["sketch_id"];
    circle(s2, 30, 0, 3);
    const std::string hole =
        silex.call("extrude", {{"sketch_id", s2}, {"distance_mm", 8}, {"operation", "cut"}})["feature_id"];

    const Json cp = silex.call("circular_pattern", {{"target_type", "feature"},
                                                    {"target_ids", Json::array({hole})},
                                                    {"axis_id", "Z"},
                                                    {"count", 6}});
    EXPECT_EQ(cp["status"], "OK");
    const double disk = kPi * 1600 * 8, h = kPi * 9 * 8;
    EXPECT_NEAR(cp["bodies"][0]["volume_mm3"].get<double>(), disk - 6 * h, 1e-6);

    const Json up = silex.call("update_feature_parameter",
                               {{"feature_id", cp["feature_id"]}, {"param_name", "count"}, {"new_value", 8}});
    EXPECT_EQ(up["status"], "OK");
    EXPECT_NEAR(up["bodies"][0]["volume_mm3"].get<double>(), disk - 8 * h, 1e-6);

    // linear pattern of the flange body along X (3 copies, 100 mm apart => separate solids fused as compound)
    const Json lp = silex.call("linear_pattern", {{"target_type", "body"},
                                                  {"target_ids", Json::array({body})},
                                                  {"direction_1_id", "X"},
                                                  {"count_1", 2},
                                                  {"spacing_1_mm", 100}});
    EXPECT_EQ(lp["status"], "OK");
    EXPECT_NEAR(lp["bodies"][0]["volume_mm3"].get<double>(), 2 * (disk - 8 * h), 1e-6);

    // mirror across YZ: the flange at x=0 overlaps its own mirror image => merged ("Join if touching"),
    // giving three flanges at x = -100, 0, +100 in a single body
    const Json mr = silex.call("mirror", {{"target_type", "body"},
                                          {"target_ids", Json::array({body})},
                                          {"mirror_plane_id", "YZ"}});
    EXPECT_EQ(mr["status"], "OK");
    ASSERT_EQ(mr["bodies"].size(), 1u);
    EXPECT_NEAR(mr["bodies"][0]["volume_mm3"].get<double>(), 3 * (disk - 8 * h), 1e-6);

    const Json info = silex.call("inspect_body", {{"body_id", body}});
    EXPECT_TRUE(info["valid"].get<bool>());
    EXPECT_THROW(silex.call("circular_pattern", {{"target_type", "feature"},
                                                 {"target_ids", Json::array({s1})},
                                                 {"axis_id", "Z"},
                                                 {"count", 3}}),
                 std::runtime_error);  // a sketch is not a copyable feature
}

// v0.3 through the pipe: global parameters + expressions (cascade), box + lid clearance,
// measure, materials, 3MF, sweep, loft, technical drawing (PDF / SVG).
TEST(Ipc, V03AgentWorkflowHeadless)
{
    int argc = 1;
    char name[] = "silex_ipc_tests";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    SilexProcess silex;
    ASSERT_TRUE(silex.connected());
    const Json methods = silex.call("ping")["methods"];
    for (const char* m : {"sweep", "loft", "set_parameter", "get_parameters", "check_interference", "measure",
                          "set_body_material", "export_3mf", "export_drawing"})
        EXPECT_NE(std::find(methods.begin(), methods.end(), m), methods.end()) << m;

    auto rect = [&](const std::string& sk, double w, double h) {
        silex.call("sketch_add_geometry", {{"sketch_id", sk},
                                           {"elements", Json::array({{{"type", "rectangle"}, {"width", w}, {"height", h}}})}});
    };

    // ---- parameters
    silex.call("set_parameter", {{"name", "hauteur"}, {"expression", "25"}, {"comment", "hauteur boitier"}});
    Json params = silex.call("set_parameter", {{"name", "jeu_montage"}, {"expression", "0.25"}});
    EXPECT_EQ(params["parameters"].size(), 2u);
    EXPECT_THROW(silex.call("set_parameter", {{"name", "boucle"}, {"expression", "boucle + 1"}}), std::runtime_error);
    EXPECT_THROW(silex.call("set_parameter", {{"name", "sin"}, {"expression", "1"}}), std::runtime_error);

    // ---- box 60 x 40 x hauteur, shelled 2 mm (open top)
    const std::string sBox = silex.call("create_sketch", {{"plane_or_face_id", "XY"}})["sketch_id"];
    rect(sBox, 60, 40);
    const Json ex = silex.call("extrude", {{"sketch_id", sBox}, {"distance_mm", 10}});
    const std::string box = ex["body_id"];
    silex.call("update_feature_parameter",
               {{"feature_id", ex["feature_id"]}, {"param_name", "distance"}, {"expression", "hauteur"}});
    EXPECT_NEAR(silex.call("inspect_body", {{"body_id", box}})["volume_mm3"].get<double>(), 60 * 40 * 25.0, 1e-6);
    const Json top = silex.call("query_topology", {{"body_id", box}, {"entity_type", "face"}, {"filter", "top"}});
    silex.call("shell", {{"body_id", box}, {"removed_face_ids", Json::array({top["entities"][0]["id"]})}, {"thickness_mm", 2}});

    // ---- lid on a plane offset by hauteur + jeu (literal 25.25 here)
    const std::string sLid = silex.call("create_sketch", {{"plane_or_face_id", "XY"}, {"offset_mm", 25.25}})["sketch_id"];
    rect(sLid, 60, 40);
    const std::string lid = silex.call("extrude", {{"sketch_id", sLid}, {"distance_mm", 3}})["body_id"];

    Json itf = silex.call("check_interference", {{"body_ids", Json::array({box, lid})}});
    EXPECT_FALSE(itf["collision"].get<bool>());
    ASSERT_EQ(itf["pairs"].size(), 1u);
    EXPECT_EQ(itf["pairs"][0]["status"], "ok");
    EXPECT_NEAR(itf["min_clearance_mm"].get<double>(), 0.25, 1e-7);

    // raising the box by a parameter change makes the parts collide (cascade recompute)
    silex.call("set_parameter", {{"name", "hauteur"}, {"expression", "27"}});
    itf = silex.call("check_interference", {});
    EXPECT_TRUE(itf["collision"].get<bool>());
    EXPECT_GT(itf["pairs"][0]["overlap_volume_mm3"].get<double>(), 1e-4);
    silex.call("set_parameter", {{"name", "hauteur"}, {"expression", "25"}});

    // ---- measure: lid bottom face <-> box top rim
    const Json lidFaces = silex.call("query_topology", {{"body_id", lid}, {"entity_type", "face"}, {"filter", "bottom"}});
    const Json rim = silex.call("query_topology", {{"body_id", box}, {"entity_type", "face"}, {"filter", "top"}});
    const Json m =
        silex.call("measure", {{"entity_ids", Json::array({lidFaces["entities"][0]["id"], rim["entities"][0]["id"]})}});
    EXPECT_NEAR(m["distance"]["min_distance_mm"].get<double>(), 0.25, 1e-7);
    EXPECT_NEAR(m["angle_deg"].get<double>(), 0.0, 1e-7);

    // ---- materials & mass
    Json mat = silex.call("set_body_material", {{"body_id", box}, {"preset", "PETG Noir"}});
    EXPECT_EQ(mat["material"]["name"], "PETG Noir");
    mat = silex.call("set_body_material", {{"body_id", lid}, {"preset", "resine"}});
    EXPECT_NEAR(mat["mass_g"].get<double>(), 60 * 40 * 3 * 1e-3 * 1.10, 1e-6);
    EXPECT_EQ(silex.call("inspect_body", {{"body_id", lid}})["material"]["color"], "#CFE6F273");

    const QString threeMf = QDir::temp().filePath("silex_ipc_v03.3mf");
    QFile::remove(threeMf);
    const Json e3 = silex.call("export_3mf", {{"file_path", threeMf.toStdString()}, {"title", "Boitier"}});
    EXPECT_EQ(e3["objects"], 2);
    EXPECT_GT(QFileInfo(threeMf).size(), 500);

    // ---- drawing (3 views, PDF + SVG)
    const QString pdf = QDir::temp().filePath("silex_ipc_v03.pdf");
    const QString svg = QDir::temp().filePath("silex_ipc_v03.svg");
    const Json dp =
        silex.call("export_drawing", {{"file_path", pdf.toStdString()}, {"format", "pdf"}, {"body_ids", Json::array({box})}});
    ASSERT_EQ(dp["views"].size(), 3u);
    EXPECT_GT(dp["views"][0]["visible_edges"].get<int>(), 0);
    EXPECT_GT(dp["views"][0]["hidden_edges"].get<int>(), 0);  // inner walls of the shell
    QFile fp(pdf);
    ASSERT_TRUE(fp.open(QIODevice::ReadOnly));
    EXPECT_TRUE(fp.read(5).startsWith("%PDF"));
    silex.call("export_drawing", {{"file_path", svg.toStdString()}, {"sheet", "A3"}});
    QFile fs(svg);
    ASSERT_TRUE(fs.open(QIODevice::ReadOnly));
    const QByteArray svgText = fs.readAll();
    EXPECT_TRUE(svgText.contains("<svg"));
    EXPECT_TRUE(svgText.contains("width=\"420mm\""));
    EXPECT_TRUE(svgText.contains("stroke-dasharray"));  // hidden edges

    // ---- sweep: elbow pipe, loft: square -> round
    const std::string path = silex.call("create_sketch", {{"plane_or_face_id", "XY"}, {"offset_mm", 60}})["sketch_id"];
    silex.call("sketch_add_geometry",
               {{"sketch_id", path},
                {"elements", Json::array({{{"type", "line"}, {"start", {100, 0}}, {"end", {150, 0}}},
                                          {{"type", "arc"}, {"center", {150, 30}}, {"radius", 30}, {"start_angle", -90}, {"end_angle", 0}},
                                          {{"type", "line"}, {"start", {180, 30}}, {"end", {180, 80}}}})}});
    const std::string prof = silex.call("create_sketch", {{"plane_or_face_id", "YZ"}, {"offset_mm", 100}})["sketch_id"];
    silex.call("sketch_add_geometry",
               {{"sketch_id", prof}, {"elements", Json::array({{{"type", "circle"}, {"center", {0, 60}}, {"radius", 5}}})}});
    const Json sw = silex.call("sweep", {{"profile_sketch_id", prof}, {"path_sketch_id", path}});
    EXPECT_EQ(sw["status"], "OK");
    EXPECT_TRUE(sw["body"]["valid"].get<bool>());
    const double pipe = kPi * 25 * (100 + 15 * kPi);
    EXPECT_NEAR(sw["body"]["volume_mm3"].get<double>(), pipe, pipe * 1e-4);

    const std::string sq = silex.call("create_sketch", {{"plane_or_face_id", "XY"}, {"offset_mm", 100}})["sketch_id"];
    silex.call("sketch_add_geometry", {{"sketch_id", sq},
                                       {"elements", Json::array({{{"type", "rectangle"}, {"width", 40}, {"height", 40},
                                                                  {"centered", true}, {"center", {0, 0}}}})}});
    const std::string ci = silex.call("create_sketch", {{"plane_or_face_id", "XY"}, {"offset_mm", 150}})["sketch_id"];
    silex.call("sketch_add_geometry",
               {{"sketch_id", ci}, {"elements", Json::array({{{"type", "circle"}, {"center", {0, 0}}, {"radius", 12}}})}});
    const Json lo = silex.call("loft", {{"profile_sketch_ids", Json::array({sq, ci})}});
    EXPECT_EQ(lo["status"], "OK");
    EXPECT_TRUE(lo["body"]["valid"].get<bool>());

    const Json gp = silex.call("get_parameters");
    ASSERT_TRUE(gp["usage"].contains("hauteur"));
    EXPECT_EQ(gp["usage"]["hauteur"][0]["field"], "/distance");

    // the design round-trips through schema 0.3
    const QString file = QDir::temp().filePath("silex_ipc_v03.silex");
    silex.call("save_design", {{"file_path", file.toStdString()}});
    QFile f(file);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const Json saved = Json::parse(f.readAll().toStdString());
    EXPECT_EQ(saved["schema_version"], "0.7");
    EXPECT_EQ(saved["parameters"].size(), 2u);
    EXPECT_EQ(saved["bodies"].size(), 2u);
}

TEST(Ipc, SketchProHeadless)
{
    int argc = 1;
    char name[] = "silex_ipc_tests";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    SilexProcess silex;
    ASSERT_TRUE(silex.connected());

    // plate: 60 x 40 center rectangle (2 dimensions) with a slot and a hexagonal pocket
    const std::string sk = silex.call("create_sketch", {{"plane_or_face_id", "XY"}})["sketch_id"];
    Json r = silex.call("sketch_add_geometry",
                        {{"sketch_id", sk},
                         {"elements", Json::array({{{"type", "center_rectangle"}, {"width", 60}, {"height", 40}, {"dimension", true}}})}});
    EXPECT_EQ(r["dof"], 0);
    EXPECT_EQ(r["state"], "fully_constrained");
    const Json named = r["elements"][0]["named"];
    const Json ext = silex.call("extrude", {{"sketch_id", sk}, {"distance_mm", 5}});
    EXPECT_NEAR(silex.call("inspect_body", {{"body_id", ext["body_id"]}})["volume_mm3"].get<double>(), 60 * 40 * 5.0, 1e-6);

    // slot 30 x 8, fully constrained, extruded on its own
    const std::string s2 = silex.call("create_sketch", {{"plane_or_face_id", "XY"}, {"offset_mm", 20}})["sketch_id"];
    r = silex.call("sketch_add_geometry",
                   {{"sketch_id", s2},
                    {"elements", Json::array({{{"type", "slot"}, {"center", {0, 0}}, {"length", 30}, {"width", 8}, {"dimension", true}}})}});
    const Json slot = r["elements"][0]["named"];
    r = silex.call("sketch_add_constraints",
                   {{"sketch_id", s2}, {"constraints", Json::array({{{"type", "horizontal"}, {"a", slot["axis"]}}})}});
    EXPECT_EQ(r["dof"], 0);
    const Json e2 = silex.call("extrude", {{"sketch_id", s2}, {"distance_mm", 2}});
    const Json body = silex.call("inspect_body", {{"body_id", e2["body_id"]}});
    EXPECT_TRUE(body["valid"].get<bool>());
    EXPECT_NEAR(body["volume_mm3"].get<double>(), (30 * 8 + kPi * 16) * 2, 1e-6);

    // new constraints: symmetric holes about the Y axis, concentric / collinear / midpoint
    const std::string s3 = silex.call("create_sketch", {{"plane_or_face_id", "XY"}, {"offset_mm", 40}})["sketch_id"];
    r = silex.call("sketch_add_geometry",
                   {{"sketch_id", s3},
                    {"elements", Json::array({{{"type", "circle"}, {"center", {-20, 5}}, {"radius", 3}},
                                              {{"type", "circle"}, {"center", {21, 6}}, {"radius", 3}},
                                              {{"type", "line"}, {"start", {-10, -10}}, {"end", {10, -9}}}})}});
    const std::string h1 = r["elements"][0]["geo_ids"][0], h2 = r["elements"][1]["geo_ids"][0];
    const std::string ln = r["elements"][2]["geo_ids"][0];
    r = silex.call("sketch_add_constraints",
                   {{"sketch_id", s3},
                    {"constraints", Json::array({{{"type", "symmetry"}, {"a", h1}, {"b", h2}, {"axis", "y_axis"}},
                                                 {{"type", "midpoint"}, {"a", "origin"}, {"b", ln}}})}});
    EXPECT_EQ(r["state"], "under_constrained");
    EXPECT_THROW(silex.call("sketch_add_constraints",
                            {{"sketch_id", s3}, {"constraints", Json::array({{{"type", "collinear"}, {"a", h1}, {"b", ln}}})}}),
                 std::runtime_error);
    EXPECT_THROW(silex.call("sketch_add_constraints",
                            {{"sketch_id", s3}, {"constraints", Json::array({{{"type", "symmetry"}, {"a", h1}, {"b", h2}}})}}),
                 std::runtime_error);

    // editing tools: fillet a corner of the plate, offset it inwards, trim, polygon
    r = silex.call("sketch_add_geometry",
                   {{"sketch_id", sk},
                    {"elements", Json::array({{{"type", "sketch_fillet"}, {"point", named["bottom"].get<std::string>() + ":end"}, {"radius", 5}},
                                              {{"type", "polygon"}, {"sides", 6}, {"radius", 6.5}, {"mode", "circumscribed"}, {"dimension", true}}})}});
    EXPECT_FALSE(r["elements"][0]["driving_constraint_id"].get<std::string>().empty());
    EXPECT_EQ(r["state"], "under_constrained");  // the hexagon may still turn
    EXPECT_THROW(silex.call("sketch_add_geometry",
                            {{"sketch_id", sk}, {"elements", Json::array({{{"type", "trim"}, {"geo_id", "nope"}, {"point", {0, 0}}}})}}),
                 std::runtime_error);
}

TEST(Ipc, ErgonomicsPrimitivesAndPressPullHeadless)
{
    int argc = 1;
    char name[] = "silex_ipc_tests";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    SilexProcess silex;
    ASSERT_TRUE(silex.connected());
    const Json methods = silex.call("ping")["methods"];
    for (const char* m : {"create_box", "create_cylinder", "press_pull"})
        EXPECT_NE(std::find(methods.begin(), methods.end(), m), methods.end()) << m;

    // box 60 x 40 x 20 with a through hole d10 (cylinder, operation cut)
    const Json box = silex.call("create_box", {{"length_mm", 60}, {"width_mm", 40}, {"height_mm", 20}});
    EXPECT_EQ(box["status"], "OK");
    EXPECT_NEAR(box["body"]["volume_mm3"].get<double>(), 60 * 40 * 20.0, 1e-6);
    const Json hole = silex.call("create_cylinder",
                                 {{"diameter_mm", 10}, {"height_mm", 20}, {"center", {15, 0}}, {"operation", "cut"}});
    EXPECT_EQ(hole["body_id"], box["body_id"]);
    const Json body = silex.call("inspect_body", {{"body_id", box["body_id"]}});
    EXPECT_TRUE(body["valid"].get<bool>());
    EXPECT_NEAR(body["volume_mm3"].get<double>(), 60 * 40 * 20.0 - kPi * 25 * 20, 1e-6);

    // press / pull the top face by +5 mm, then change it through the timeline parameter
    const Json top = silex.call("query_topology", {{"body_id", box["body_id"]}, {"entity_type", "face"}, {"filter", "top"}});
    const Json pp = silex.call("press_pull", {{"face_id", top["entities"][0]["id"]}, {"distance_mm", 5}});
    EXPECT_NEAR(pp["body"]["volume_mm3"].get<double>(), (60 * 40 - kPi * 25) * 25.0, 1e-6);
    const Json upd = silex.call("update_feature_parameter",
                                {{"feature_id", pp["feature_id"]}, {"param_name", "distance"}, {"new_value", 10}});
    EXPECT_NEAR(upd["bodies"][0]["volume_mm3"].get<double>(), (60 * 40 - kPi * 25) * 30.0, 1e-6);

    EXPECT_THROW(silex.call("create_cylinder", {{"diameter_mm", 0}, {"height_mm", 5}}), std::runtime_error);
    EXPECT_THROW(silex.call("press_pull", {{"face_id", "nope"}, {"distance_mm", 5}}), std::runtime_error);
}

TEST(Ipc, V04BodiesPlanesAndMatesHeadless)
{
    int argc = 1;
    char name[] = "silex_ipc_tests";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    SilexProcess silex;
    ASSERT_TRUE(silex.connected());

    // lot 0: closed box 80 x 50 x 30 split at z = 22 into base + lid
    const Json box = silex.call("create_box", {{"length_mm", 80}, {"width_mm", 50}, {"height_mm", 30}});
    Json r = silex.call("split_body", {{"body_id", box["body_id"]}, {"plane", "XY"}, {"offset_mm", 22}});
    ASSERT_EQ(r["bodies"].size(), 2u);
    EXPECT_NEAR(r["bodies"][0]["volume_mm3"].get<double>() + r["bodies"][1]["volume_mm3"].get<double>(), 80 * 50 * 30.0, 1e-6);
    EXPECT_EQ(r["bodies"][1]["name"], "Body1_2");
    const std::string lid = r["bodies"][1]["body_id"];

    // copy the lid aside, then remove the copy (timeline step) and undo it
    r = silex.call("transform_body", {{"body_ids", {lid}}, {"dx_mm", 120}, {"copy", true}});
    ASSERT_EQ(r["bodies"].size(), 3u);
    const std::string copy = r["bodies"][2]["body_id"];
    r = silex.call("remove_body", {{"body_ids", {copy}}});
    EXPECT_EQ(r["bodies"].size(), 2u);
    silex.call("undo");
    EXPECT_EQ(silex.call("get_model_tree")["bodies"].size(), 3u);
    silex.call("redo");

    // lot 1: lid placed face to face on the base with 0.25 mm of clearance
    r = silex.call("transform_body", {{"body_ids", {lid}}, {"dz_mm", 15}});  // move it away first
    const Json lidFaces = silex.call("query_topology", {{"body_id", lid}, {"entity_type", "face"}, {"filter", "bottom"}});
    const Json baseFaces = silex.call("query_topology", {{"body_id", box["body_id"]}, {"entity_type", "face"}, {"filter", "top"}});
    r = silex.call("align_bodies", {{"moving_face_id", lidFaces["entities"][0]["id"]},
                                    {"fixed_face_id", baseFaces["entities"][0]["id"]},
                                    {"offset_mm", 0.25}});
    EXPECT_EQ(r["status"], "OK");
    const Json itf = silex.call("check_interference", {{"body_ids", {box["body_id"], lid}}});
    EXPECT_FALSE(itf["collision"].get<bool>());
    EXPECT_NEAR(itf["min_clearance_mm"].get<double>(), 0.25, 1e-6);

    // construction plane + sketch on it
    const Json pl = silex.call("create_construction_plane", {{"mode", "offset"}, {"base", "XY"}, {"offset_mm", 40}});
    EXPECT_NEAR(pl["origin"][2].get<double>(), 40, 1e-9);
    const Json sk = silex.call("create_sketch", {{"plane_or_face_id", pl["plane_id"]}});
    EXPECT_NEAR(sk["plane"]["origin"][2].get<double>(), 40, 1e-9);
}

TEST(Ipc, V04PrintabilityAndThreadHeadless)
{
    int argc = 1;
    char name[] = "silex_ipc_tests";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    SilexProcess silex;
    ASSERT_TRUE(silex.connected());

    // T shape: 300 mm2 of flat ceiling when printed upright, nothing upside down
    silex.call("create_box", {{"length_mm", 10}, {"width_mm", 10}, {"height_mm", 20}});
    silex.call("create_box", {{"length_mm", 40}, {"width_mm", 10}, {"height_mm", 5}, {"offset_mm", 20}, {"operation", "join"}});
    Json r = silex.call("check_printability", Json::object());
    ASSERT_EQ(r["bodies"].size(), 1u);
    EXPECT_NEAR(r["bodies"][0]["overhang_over_60_mm2"].get<double>(), 300, 1e-3);
    EXPECT_TRUE(r["bodies"][0]["needs_supports"].get<bool>());
    r = silex.call("check_printability", {{"build_direction", "-Z"}});
    EXPECT_FALSE(r["bodies"][0]["needs_supports"].get<bool>());

    // M5 thread on a 5 mm shaft standing apart
    const Json shaft = silex.call("create_cylinder", {{"diameter_mm", 5}, {"height_mm", 12}, {"center", {60, 0}}});
    const Json faces = silex.call("query_topology", {{"body_id", shaft["body_id"]}, {"entity_type", "face"}});
    std::string side;
    for (const auto& f : faces["entities"])
        if (f["geom_type"] == "cylinder")
            side = f["id"];
    ASSERT_FALSE(side.empty());
    r = silex.call("create_thread", {{"face_id", side}, {"size", "M5"}});
    EXPECT_EQ(r["status"], "OK");
    EXPECT_TRUE(r["body"]["valid"].get<bool>());
    EXPECT_LT(r["body"]["volume_mm3"].get<double>(), kPi * 6.25 * 12 * 0.9);
}

TEST(Ipc, V04TutorContextHeadless)
{
    int argc = 1;
    char name[] = "silex_ipc_tests";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    SilexProcess silex;
    ASSERT_TRUE(silex.connected());
    const std::string sk = silex.call("create_sketch", {{"plane_or_face_id", "XY"}})["sketch_id"];
    // a circle with its diameter but no position: the coach must say what is missing
    Json r = silex.call("sketch_add_geometry",
                        {{"sketch_id", sk}, {"elements", Json::array({{{"type", "circle"}, {"center", {12, 7}}, {"radius", 5}}})}});
    const std::string circle = r["elements"][0]["geo_ids"][0];
    silex.call("sketch_add_constraints", {{"sketch_id", sk}, {"constraints", Json::array({{{"type", "diameter"}, {"a", circle}, {"value", 10}}})}});
    const Json ctx = silex.call("get_pedagogical_context", {{"lang", "fr"}});
    ASSERT_EQ(ctx["sketches"].size(), 1u);
    EXPECT_EQ(ctx["sketches"][0]["dof"], 2);
    ASSERT_GE(ctx["sketches"][0]["explanations"].size(), 1u);
    const std::string text = ctx["sketches"][0]["explanations"][0]["text"];
    EXPECT_NE(text.find("Ø10"), std::string::npos) << text;
    EXPECT_FALSE(ctx["tutor_guidelines"].get<std::string>().empty());
    EXPECT_TRUE(ctx["tutorial"].is_null());  // no lesson running headless
    const Json en = silex.call("get_pedagogical_context", {{"lang", "en"}});
    EXPECT_NE(en["sketches"][0]["explanations"][0]["text"].get<std::string>().find("diameter"), std::string::npos);
}

// v0.5: component library, enclosure wizard, build volume, lesson recorder, Socratic tutor mode
TEST(Ipc, V05MakersAndTutorHeadless)
{
    int argc = 1;
    char name[] = "silex_ipc_tests";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    {
        SilexProcess silex;
        ASSERT_TRUE(silex.connected());
        Json r = silex.call("list_components", {{"lang", "en"}});
        EXPECT_EQ(r["components"].size(), 9u);
        // enclosure with 4 M3 posts: every step computes, box and lid do not collide
        r = silex.call("create_enclosure", {{"inner_length_mm", 85}, {"inner_width_mm", 56}, {"inner_height_mm", 25},
                                            {"closure", "screws"}});
        EXPECT_GE(r["feature_count"].get<int>(), 9);
        for (const auto& st : r["steps"])
            EXPECT_NE(st["status"], "Error") << st["name"];
        r = silex.call("check_interference", Json::object());
        EXPECT_FALSE(r["collision"].get<bool>());
        // a Raspberry Pi inside: locked reference, ignored by the build volume check
        r = silex.call("insert_component", {{"component_id", "rpi4b"}, {"position", {-42.5, -28, 8}}});
        EXPECT_TRUE(r["locked"].get<bool>());
        EXPECT_EQ(r["mounting_holes"].size(), 4u);
        r = silex.call("check_build_volume", {{"preset", "prusa_mini"}});
        EXPECT_TRUE(r["all_fit"].get<bool>());
        EXPECT_EQ(r["bodies"].size(), 2u);  // box + lid, not the board
        // lesson recorder
        r = silex.call("record_lesson", {{"lesson_id", "boitier_pi"}, {"title_fr", "Boîtier Pi"}, {"title_en", "Pi case"}});
        EXPECT_TRUE(r["valid"].get<bool>()) << r["problems"].dump();
        EXPECT_GE(r["lesson"]["steps"].size(), 9u);
    }
    {
        SilexProcess tutor({"--tutor-mode"});
        ASSERT_TRUE(tutor.connected());
        EXPECT_TRUE(tutor.call("ping")["tutor_mode"].get<bool>());
        EXPECT_TRUE(tutor.call("get_pedagogical_context")["tutor_mode"].get<bool>());
        tutor.call("get_model_tree");
        try {
            tutor.call("create_box", {{"length_mm", 10}, {"width_mm", 10}, {"height_mm", 10}});
            ADD_FAILURE() << "tutor mode accepted a modelling command";
        }
        catch (const std::runtime_error& e) {
            EXPECT_NE(std::string(e.what()).find("Tutor mode"), std::string::npos) << e.what();
        }
    }
}

// ------------------------------------------------------------------ sprint "Fiabilité"

namespace {

Json expectError(SilexProcess& s, const std::string& method, Json params, const std::string& code)
{
    const Json r = s.callRaw(method, std::move(params));
    EXPECT_TRUE(r.contains("error")) << method << " should fail with " << code;
    if (!r.contains("error"))
        return Json();
    const Json& e = r["error"];
    EXPECT_EQ(e.value("error_code", std::string()), code) << e.dump();
    EXPECT_FALSE(e.value("message", std::string()).empty()) << e.dump();
    EXPECT_FALSE(e.value("hint", std::string()).empty()) << e.dump();
    EXPECT_TRUE(e.contains("code")) << e.dump();
    return e;
}

}  // namespace

TEST(Ipc, McpDocumentCycleHeadless)
{
    int argc = 1;
    char name[] = "silex_ipc_tests";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    SilexProcess silex;
    ASSERT_TRUE(silex.connected());
    const Json ping = silex.call("ping");
    EXPECT_GE(ping["protocol_version"].get<int>(), 2);
    const auto methods = ping["methods"].get<std::vector<std::string>>();
    for (const char* m : {"new_document", "open_document", "get_active_document_info", "begin_command_group",
                          "end_command_group", "verify_dimensions", "create_fastener_hole", "record_lesson"})
        EXPECT_NE(std::find(methods.begin(), methods.end(), m), methods.end()) << m;

    // 1. open a file (a copy of a shipped example)
    const QString src = QDir(examplesDir()).filePath("plaque_percee.silex");
    const QString part = tmp.filePath("plaque.silex");
    ASSERT_TRUE(QFile::copy(src, part)) << src.toStdString();
    expectError(silex, "open_document", {{"path", tmp.filePath("missing.silex").toStdString()}}, "FILE_NOT_FOUND");
    Json info = silex.call("open_document", {{"path", part.toStdString()}});
    EXPECT_EQ(QFileInfo(QString::fromStdString(info["path"].get<std::string>())).canonicalFilePath().toStdString(),
              QFileInfo(part).canonicalFilePath().toStdString());
    EXPECT_EQ(info["name"], "plaque");
    EXPECT_EQ(info["modified"], false);
    EXPECT_EQ(info["schema_version"], "0.7");
    EXPECT_EQ(info["errors"], 0);
    const int features0 = info["feature_count"];
    const int bodies0 = info["body_count"];
    ASSERT_GT(features0, 0);
    ASSERT_EQ(bodies0, 1);

    // 2. several operations, ONE undo step
    expectError(silex, "end_command_group", Json::object(), "NO_GROUP_OPEN");
    silex.call("begin_command_group", {{"name", "AI: rounded block"}});
    expectError(silex, "begin_command_group", {{"name", "nested"}}, "GROUP_ALREADY_OPEN");
    const Json box = silex.call("create_box", {{"length_mm", 40}, {"width_mm", 30}, {"height_mm", 20}});
    const std::string body = box["body_id"];
    const Json vertical = silex.call("query_topology", {{"body_id", body}, {"entity_type", "edge"}, {"filter", "vertical"}});
    ASSERT_EQ(vertical["count"], 4);
    Json edgeIds = Json::array();
    for (const auto& e : vertical["entities"])
        edgeIds.push_back(e["id"]);
    EXPECT_EQ(silex.call("fillet_or_chamfer", {{"type", "fillet"}, {"edge_ids", edgeIds}, {"radius_or_distance_mm", 5}})["status"], "OK");
    // a failing operation inside the group is rolled back alone, with a structured explanation
    const Json top = silex.call("query_topology", {{"body_id", body}, {"entity_type", "edge"}, {"filter", "top"}});
    ASSERT_GT(top["count"].get<int>(), 0);
    expectError(silex, "fillet_or_chamfer",
                {{"type", "fillet"}, {"edge_ids", Json::array({top["entities"][0]["id"]})}, {"radius_or_distance_mm", 500}},
                "REBUILD_FAILED");
    Json chamferIds = Json::array();
    for (const auto& e : top["entities"])
        chamferIds.push_back(e["id"]);
    EXPECT_EQ(silex.call("fillet_or_chamfer", {{"type", "chamfer"}, {"edge_ids", chamferIds}, {"radius_or_distance_mm", 1}})["status"], "OK");
    info = silex.call("get_active_document_info");
    EXPECT_EQ(info["command_group"]["open"], true);
    EXPECT_EQ(info["command_group"]["commands"], 3);
    const Json closed = silex.call("end_command_group");
    EXPECT_EQ(closed["commands"], 3);
    EXPECT_EQ(closed["undo_steps"], 1);
    EXPECT_EQ(closed["undo_label"], "AI: rounded block");

    info = silex.call("get_active_document_info");
    EXPECT_EQ(info["feature_count"], features0 + 3);
    EXPECT_EQ(info["body_count"], 2);
    EXPECT_EQ(info["modified"], true);

    // 3. finished size (fillets and chamfers do not change the overall size)
    Json v = silex.call("verify_dimensions", {{"body_id", body}, {"expected_bbox_mm", {40, 30, 20}}, {"tolerance_mm", 1e-6}});
    EXPECT_EQ(v["ok"], true) << v.dump();
    EXPECT_EQ(v["valid_solid"], true);
    v = silex.call("verify_dimensions", {{"body_id", body}, {"expected_bbox_mm", {{"x", 40}, {"y", 30}, {"z", 21}}}, {"tolerance_mm", 0.1}});
    EXPECT_EQ(v["ok"], false);
    EXPECT_NEAR(v["axes"][2]["deviation_mm"].get<double>(), -1.0, 1e-9);
    EXPECT_EQ(v["axes"][2]["ok"], false);
    EXPECT_EQ(v["axes"][0]["ok"], true);
    expectError(silex, "verify_dimensions", {{"body_id", "nope"}, {"expected_bbox_mm", {1, 2, 3}}}, "UNKNOWN_ID");
    expectError(silex, "verify_dimensions", {{"body_id", body}, {"expected_bbox_mm", {1, 2}}}, "INVALID_PARAMS");
    expectError(silex, "verify_dimensions", {{"expected_bbox_mm", {1, 2, 3}}}, "MISSING_PARAMETER");  // 2 bodies

    // 4. ONE undo removes the whole group; redo brings it back
    Json u = silex.call("undo");
    EXPECT_EQ(u["feature_count"], features0);
    EXPECT_EQ(silex.call("get_active_document_info")["body_count"], bodies0);
    u = silex.call("redo");
    EXPECT_EQ(u["feature_count"], features0 + 3);

    // 5. unsaved changes are protected
    const Json dirty = expectError(silex, "open_document", {{"path", part.toStdString()}}, "DOCUMENT_DIRTY");
    EXPECT_NE(dirty["hint"].get<std::string>().find("save_design"), std::string::npos);
    expectError(silex, "new_document", Json::object(), "DOCUMENT_DIRTY");

    // 6. save, re-read, check again
    const QString out = tmp.filePath("plaque_v2.silex");
    silex.call("save_design", {{"file_path", out.toStdString()}});
    EXPECT_EQ(silex.call("get_active_document_info")["modified"], false);
    info = silex.call("new_document");  // clean now: allowed
    EXPECT_EQ(info["feature_count"], 0);
    EXPECT_TRUE(info["path"].is_null());
    info = silex.call("open_document", {{"path", out.toStdString()}});
    EXPECT_EQ(info["feature_count"], features0 + 3);
    EXPECT_EQ(info["body_count"], 2);
    EXPECT_EQ(info["errors"], 0);
    EXPECT_EQ(info["can_undo"], false);
    v = silex.call("verify_dimensions", {{"body_id", body}, {"expected_bbox_mm", {40, 30, 20}}, {"tolerance_mm", 1e-6}});
    EXPECT_EQ(v["ok"], true) << v.dump();

    // 7. discard_changes and the other structured errors
    silex.call("create_box", {{"length_mm", 5}, {"width_mm", 5}, {"height_mm", 5}});
    info = silex.call("open_document", {{"path", out.toStdString()}, {"discard_changes", true}});
    EXPECT_EQ(info["feature_count"], features0 + 3);
    EXPECT_EQ(info["modified"], false);
    expectError(silex, "no_such_method", Json::object(), "METHOD_NOT_FOUND");
    expectError(silex, "inspect_body", {{"body_id", "nope"}}, "UNKNOWN_ID");
    expectError(silex, "create_box", {{"width_mm", 5}}, "MISSING_PARAMETER");
}

TEST(Ipc, AutosaveCrashRecoveryGui)
{
    int argc = 1;
    char name[] = "silex_ipc_tests";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    QTemporaryDir recoveryDir;
    ASSERT_TRUE(recoveryDir.isValid());
    const std::filesystem::path dir = std::filesystem::path(recoveryDir.path().toStdWString());

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("SILEX_RECOVERY_DIR", recoveryDir.path());
    env.insert("SILEX_AUTOSAVE_INTERVAL_S", "1");
    env.insert("QT_QPA_PLATFORM", "offscreen");

    std::string featureId;
    std::filesystem::path orphan;
    {
        // 1. the windowed application is modified through IPC, the autosave timer fires
        SilexProcess silex({}, true, env);
        ASSERT_TRUE(silex.connected()) << "could not connect to the windowed silex (offscreen)";
        const Json box = silex.call("create_box", {{"length_mm", 30}, {"width_mm", 20}, {"height_mm", 10}});
        featureId = box["feature_id"];
        EXPECT_EQ(silex.call("get_active_document_info")["modified"], true);
        std::vector<silex::recovery::AutosaveInfo> found;
        for (int i = 0; i < 100 && found.empty(); ++i) {
            QThread::msleep(100);
            found = silex::recovery::scan(dir);
        }
        ASSERT_EQ(found.size(), 1u) << "no autosave written";
        EXPECT_EQ(found[0].pid, silex.process().processId());
        EXPECT_EQ(found[0].displayName, "Untitled");
        EXPECT_EQ(silex::recovery::readAutosave(found[0].file).features.size(), 1u);
        orphan = found[0].file;

        // 2. crash: the process is killed, the autosave stays
        silex.crash();
    }
    ASSERT_TRUE(std::filesystem::exists(orphan));

    // 3. a new session restores it: the parametric tree is back, the document is "modified"
    {
        SilexProcess silex({"--recover", QString::fromStdWString(orphan.wstring())}, true, env);
        ASSERT_TRUE(silex.connected());
        const Json info = silex.call("get_active_document_info");
        EXPECT_EQ(info["feature_count"], 1);
        EXPECT_EQ(info["body_count"], 1);
        EXPECT_EQ(info["modified"], true);
        const Json tree = silex.call("get_model_tree");
        EXPECT_EQ(tree["features"][0]["id"], featureId);
        EXPECT_FALSE(std::filesystem::exists(orphan)) << "the restored orphan must be deleted";
        const auto now = silex::recovery::scan(dir);
        ASSERT_EQ(now.size(), 1u);  // protected by the new session's own autosave
        EXPECT_EQ(now[0].pid, silex.process().processId());

        // 4. an explicit save deletes the autosave
        QTemporaryDir out;
        silex.call("save_design", {{"file_path", out.filePath("recovered.silex").toStdString()}});
        EXPECT_TRUE(silex::recovery::scan(dir).empty());
    }
}
// ------------------------------------------------------------------ v0.7 "Trio Makers"

namespace {
/// Minimal uncompressed 24-bit BMP (readable by QImage), w x h pixels.
QByteArray bmp(int w, int h)
{
    const int row = (w * 3 + 3) & ~3;
    const quint32 size = 54 + quint32(row * h);
    QByteArray b(int(size), '\0');
    auto put32 = [&](int at, quint32 v) {
        for (int i = 0; i < 4; ++i)
            b[at + i] = char((v >> (8 * i)) & 0xFF);
    };
    b[0] = 'B';
    b[1] = 'M';
    put32(2, size);
    put32(10, 54);
    put32(14, 40);
    put32(18, quint32(w));
    put32(22, quint32(h));
    b[26] = 1;
    b[28] = 24;
    put32(34, quint32(row * h));
    for (int i = 54; i < int(size); ++i)
        b[i] = char(0x80);
    return b;
}
}  // namespace

TEST(Ipc, TrioMakersTextVectorCanvasHeadless)
{
    int argc = 1;
    char name[] = "silex_ipc_tests";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    SilexProcess silex;
    ASSERT_TRUE(silex.connected());

    // ---- A. parametric text on the top face
    silex.call("set_parameter", {{"name", "largeur"}, {"expression", "60"}});
    const Json box = silex.call("create_box", {{"length_mm", 60}, {"width_mm", 30}, {"height_mm", 10}});
    const double v0 = box["body"]["volume_mm3"];
    const Json top = silex.call("query_topology", {{"body_id", box["body_id"]}, {"entity_type", "face"}, {"filter", "top"}});
    Json t = silex.call("create_emboss_text", {{"face_id", top["entities"][0]["id"]},
                                               {"text", "W={largeur}"},
                                               {"height_mm", 6},
                                               {"depth_mm", -0.8}});
    EXPECT_EQ(t["status"], "OK") << t.dump();
    EXPECT_EQ(t["resolved_text"], "W=60");
    EXPECT_NE(t["font_file"].get<std::string>().find("LiberationSans"), std::string::npos);
    const double area = t["letters_area_mm2"];
    ASSERT_GT(area, 1.0);
    EXPECT_NEAR(t["bodies"][0]["volume_mm3"].get<double>(), v0 - 0.8 * area, 1e-3 * area);
    silex.call("set_parameter", {{"name", "largeur"}, {"expression", "80"}});
    const Json tree = silex.call("get_model_tree");
    std::string resolved;
    for (const auto& f : tree["features"])
        if (f["type"] == "EmbossText")
            resolved = f["params"].value("resolved", std::string{});
    EXPECT_EQ(resolved, "W=80");
    expectError(silex, "create_emboss_text", {{"face_id", "XY"}, {"text", "x"}, {"depth_mm", 0}}, "INVALID_PARAMS");
    expectError(silex, "create_emboss_text", {{"face_id", "XY"}, {"text", "{inconnu}"}, {"height_mm", "inconnu * 2"}},
                "INVALID_PARAMS");

    // ---- B. SVG into a sketch, then extrusion of the closed outline
    const QString svg = tmp.filePath("logo.svg");
    {
        QFile f(svg);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(R"(<svg xmlns="http://www.w3.org/2000/svg" width="40mm" height="20mm" viewBox="0 0 40 20">
                     <path d="M0 0 H40 V20 H0 Z"/><circle cx="20" cy="10" r="5"/></svg>)");
    }
    const std::string sk = silex.call("create_sketch", {{"plane_or_face_id", "XZ"}})["sketch_id"];
    const Json imp = silex.call("sketch_import_vector", {{"sketch_id", sk}, {"file_path", svg.toStdString()},
                                                         {"offset_xy", {0, 50}}, {"rigid", true}});
    EXPECT_EQ(imp["lines"], 4);
    EXPECT_EQ(imp["circles"], 1);
    EXPECT_GE(imp["closed_profiles"].get<int>(), 2);
    EXPECT_EQ(imp["dof"], 2);  // rigid block: translation only
    EXPECT_NEAR(imp["bbox_mm"]["min"][0].get<double>(), -20, 1e-9);
    EXPECT_NEAR(imp["bbox_mm"]["max"][1].get<double>(), 60, 1e-9);
    expectError(silex, "sketch_import_vector", {{"sketch_id", sk}, {"file_path", tmp.filePath("none.svg").toStdString()}},
                "FILE_NOT_FOUND");

    // ---- C. calibrated photo under the sketch, saved in the file
    const QString photo = tmp.filePath("photo.bmp");
    {
        QFile f(photo);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(bmp(100, 50));
    }
    Json cv = silex.call("sketch_set_canvas", {{"sketch_id", sk}, {"image_path", photo.toStdString()}, {"width_mm", 30}});
    EXPECT_EQ(cv["canvas"]["image_px"][0], 100);
    EXPECT_NEAR(cv["canvas"]["height_mm"].get<double>(), 15, 1e-9);
    cv = silex.call("sketch_set_canvas",
                    {{"sketch_id", sk}, {"calibrate", {{"p1_px", {10, 25}}, {"p2_px", {60, 25}}, {"distance_mm", 50}}}});
    EXPECT_NEAR(cv["canvas"]["width_mm"].get<double>(), 100, 1e-9);
    EXPECT_NEAR(cv["canvas"]["height_mm"].get<double>(), 50, 1e-9);
    EXPECT_NEAR(cv["canvas"]["mm_per_pixel"].get<double>(), 1.0, 1e-12);
    const QString out = tmp.filePath("trio.silex");
    silex.call("save_design", {{"file_path", out.toStdString()}});
    QFile f(out);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const Json saved = Json::parse(f.readAll().toStdString());
    bool found = false;
    for (const auto& feat : saved["features"])
        if (feat["id"] == sk) {
            ASSERT_TRUE(feat["params"].contains("canvas"));
            EXPECT_NEAR(feat["params"]["canvas"]["width_mm"].get<double>(), 100, 1e-9);
            EXPECT_FALSE(feat["params"]["canvas"]["image_data"].get<std::string>().empty());
            found = true;
        }
    EXPECT_TRUE(found);
    const Json reopened = silex.call("open_document", {{"path", out.toStdString()}});
    EXPECT_EQ(reopened["errors"], 0);
}