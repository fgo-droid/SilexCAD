#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QStatusBar>
#include <QTimer>
#include <cstdio>
#include <memory>

#include "BuildInfo.h"
#include "Doctor.h"
#include "MainWindow.h"
#include "ResourceLocator.h"
#include "ModelController.h"
#include "Theme.h"
#include "ipc/IpcCommandDispatcher.hpp"
#include "ipc/IpcServer.hpp"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("Silex");
    QApplication::setApplicationVersion(silex::ui::kSilexVersion);
    QApplication::setOrganizationName("Silex");

    QCommandLineParser parser;
    parser.setApplicationDescription("Silex — open-source parametric 3D CAD");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption demo("demo", "Load a small demonstration part.");
    QCommandLineOption shot("screenshot", "Save a screenshot of the window to <file> and quit.", "file");
    QCommandLineOption headless("headless", "No window: serve IPC/MCP requests only (offscreen rendering).");
    QCommandLineOption pipe("pipe", "IPC endpoint name (default: cad_mcp).", "name", silex::ipc::IpcServer::defaultName());
    QCommandLineOption noIpc("no-ipc", "Do not start the local IPC server.");
    QCommandLineOption editOpt("edit", "Open the editor of the feature named <name> (docs, demos).", "name");
    QCommandLineOption toolOpt("tool", "Open a v0.3 tool once loaded: measure, interference, section, drawing, properties.",
                               "name");
    QCommandLineOption sizeOpt("size", "Window size WIDTHxHEIGHT (e.g. 1366x768, to check small screens).", "size");
    QCommandLineOption tutorOpt("tutor-mode", "Socratic AI tutor: the IPC / MCP server only accepts observation commands.");
    QCommandLineOption doctorOpt("doctor", "Check the installation (resources, OpenCascade, PlaneGCS, user folders, "
                                           "IPC/MCP, Python) and print [OK]/[WARN]/[FAIL]. Exit code 0 = operational.");
    QCommandLineOption noRecovery("no-recovery", "Do not offer to restore autosaved designs at start-up.");
    QCommandLineOption recoverOpt("recover", "Restore the autosave <file> (*.autosave.json) without asking.", "file");
    parser.addOptions({demo, shot, headless, pipe, noIpc, editOpt, toolOpt, sizeOpt, tutorOpt, doctorOpt, noRecovery,
                       recoverOpt});
    parser.addPositionalArgument("design", "A .silex design to open.");
    parser.process(app);

    // ---- installation diagnostic: console only, no window ----------------------------------
    if (parser.isSet(doctorOpt)) {
        silex::ui::attachParentConsole();
        return silex::ui::runDoctor(parser.value(pipe));
    }

    silex::ui::ResourceLocator::registerBundledFonts();  // v0.7: parametric text

    // ---- headless: model + IPC server, no visible UI ------------------------------------
    if (parser.isSet(headless)) {
        app.setQuitOnLastWindowClosed(false);
        auto* model = new silex::ui::ModelController(&app);
        model->setTutorMode(parser.isSet(tutorOpt));
        if (!parser.positionalArguments().isEmpty()) {
            QString err;
            if (!model->load(parser.positionalArguments().first(), &err)) {
                std::fprintf(stderr, "silex: %s\n", qPrintable(err));
                return 2;
            }
        }
        auto* dispatcher = new silex::ipc::IpcCommandDispatcher(model, &app);
        auto* server = new silex::ipc::IpcServer(dispatcher, &app);
        QString err;
        if (!server->listen(parser.value(pipe), &err)) {
            std::fprintf(stderr, "silex: cannot listen on %s: %s\n",
                         qPrintable(silex::ipc::IpcServer::endpointFor(parser.value(pipe))), qPrintable(err));
            return 3;
        }
        std::fprintf(stderr, "silex: headless, listening on %s\n", qPrintable(server->endpoint()));
        return app.exec();
    }

    // ---- interactive ---------------------------------------------------------------------
    silex::ui::applyDarkTheme(app);
    silex::ui::MainWindow w;
    w.resize(1500, 920);
    if (parser.isSet(sizeOpt)) {
        const QStringList wh = parser.value(sizeOpt).split('x');
        if (wh.size() == 2)
            w.resize(wh[0].toInt(), wh[1].toInt());
    }
    w.show();
    if (parser.isSet(tutorOpt))
        w.model()->setTutorMode(true);

    // The visible application can be driven too (an agent sees its changes live).
    std::unique_ptr<silex::ipc::IpcServer> server;
    if (!parser.isSet(noIpc)) {
        auto* dispatcher = new silex::ipc::IpcCommandDispatcher(w.model(), &w);
        server = std::make_unique<silex::ipc::IpcServer>(dispatcher);
        QString err;
        using St = silex::ui::MainWindow::McpState;
        if (server->listen(parser.value(pipe), &err)) {
            w.statusBar()->showMessage(QObject::tr("MCP/IPC endpoint: %1").arg(server->endpoint()), 6000);
            const QString ep = server->endpoint();
            w.setMcpState(St::Ready, ep);
            QObject::connect(server.get(), &silex::ipc::IpcServer::clientCountChanged, &w, [&w, ep](int n) {
                w.setMcpState(n > 0 ? St::Connected : St::Ready, ep);
            });
        }
        else {
            w.statusBar()->showMessage(QObject::tr("IPC disabled (%1)").arg(err), 6000);
            w.setMcpState(St::Disabled, err);
        }
    }

    if (parser.isSet(recoverOpt))
        w.recoverFrom(parser.value(recoverOpt));
    else if (!parser.positionalArguments().isEmpty())
        w.openFile(parser.positionalArguments().first());
    else if (parser.isSet(demo))
        w.loadDemo();
    if (parser.isSet(editOpt))
        w.scheduleEdit(parser.value(editOpt));
    if (parser.isSet(toolOpt))
        for (const QString& t : parser.value(toolOpt).split(',', Qt::SkipEmptyParts))
            w.scheduleTool(t.trimmed());
    if (parser.isSet(shot))
        w.scheduleScreenshot(parser.value(shot));
    // crash recovery: autosaves left by a session that ended unexpectedly (not in automated captures)
    else if (!parser.isSet(noRecovery) && qEnvironmentVariable("SILEX_NO_RECOVERY") != "1")
        QTimer::singleShot(0, &w, [&w] { w.offerRecovery(); });

    return app.exec();
}
