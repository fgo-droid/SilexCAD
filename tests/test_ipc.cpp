// End-to-end IPC test: starts `silex --headless`, builds a drilled, filleted box through the
// local pipe exactly like an MCP agent would, then checks validity, volume, TNP survival,
// offscreen capture and STEP export.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QThread>
#include <cmath>
#include <nlohmann/json.hpp>

using Json = nlohmann::json;

namespace {

constexpr double kPi = 3.14159265358979323846;

class SilexProcess
{
public:
    SilexProcess()
    {
        m_pipe = QString("silex_test_%1").arg(QCoreApplication::applicationPid());
        m_proc.setProgram(QString::fromUtf8(SILEX_EXE));
        m_proc.setArguments({"--headless", "--pipe", m_pipe});
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
        if (reply.contains("error"))
            throw std::runtime_error(method + ": " + reply["error"]["message"].get<std::string>());
        return reply["result"];
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
