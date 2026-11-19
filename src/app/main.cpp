#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QStatusBar>
#include <cstdio>
#include <memory>

#include "MainWindow.h"
#include "ModelController.h"
#include "Theme.h"
#include "ipc/IpcCommandDispatcher.hpp"
#include "ipc/IpcServer.hpp"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("Silex");
    QApplication::setApplicationVersion("0.1.0");
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
    parser.addOptions({demo, shot, headless, pipe, noIpc, editOpt});
    parser.addPositionalArgument("design", "A .silex design to open.");
    parser.process(app);

    // ---- headless: model + IPC server, no visible UI ------------------------------------
    if (parser.isSet(headless)) {
        app.setQuitOnLastWindowClosed(false);
        auto* model = new silex::ui::ModelController(&app);
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
    w.show();

    // The visible application can be driven too (an agent sees its changes live).
    std::unique_ptr<silex::ipc::IpcServer> server;
    if (!parser.isSet(noIpc)) {
        auto* dispatcher = new silex::ipc::IpcCommandDispatcher(w.model(), &w);
        server = std::make_unique<silex::ipc::IpcServer>(dispatcher);
        QString err;
        if (server->listen(parser.value(pipe), &err))
            w.statusBar()->showMessage(QObject::tr("MCP/IPC endpoint: %1").arg(server->endpoint()), 6000);
        else
            w.statusBar()->showMessage(QObject::tr("IPC disabled (%1)").arg(err), 6000);
    }

    if (!parser.positionalArguments().isEmpty())
        w.openFile(parser.positionalArguments().first());
    else if (parser.isSet(demo))
        w.loadDemo();
    if (parser.isSet(editOpt))
        w.scheduleEdit(parser.value(editOpt));
    if (parser.isSet(shot))
        w.scheduleScreenshot(parser.value(shot));

    return app.exec();
}
