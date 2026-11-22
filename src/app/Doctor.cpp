#include "Doctor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QStandardPaths>
#include <cstdio>

#include "AutosaveManager.h"
#include "BuildInfo.h"
#include "LessonRecorder.h"
#include "ModelController.h"
#include "ResourceLocator.h"
#include "ipc/IpcCommandDispatcher.hpp"
#include "ipc/IpcServer.hpp"
#include "kernel/KernelEngine.h"
#include "model/Pedagogy.h"
#include "sketch/SketchSolver.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif

namespace silex::ui {

void attachParentConsole()
{
#ifdef _WIN32
    // Output already redirected (pipe / file): keep it. Otherwise borrow the terminal of the parent.
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN)
        return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        SetConsoleOutputCP(CP_UTF8);
    }
#endif
}

namespace {

enum class Level { Ok, Warn, Fail };

struct Report
{
    int ok = 0, warn = 0, fail = 0;

    void line(Level l, const QString& text)
    {
        const char* tag = l == Level::Ok ? "[OK]  " : l == Level::Warn ? "[WARN]" : "[FAIL]";
        (l == Level::Ok ? ok : l == Level::Warn ? warn : fail)++;
        std::printf("%s %s\n", tag, text.toUtf8().constData());
        std::fflush(stdout);
    }
    void detail(const QString& text)
    {
        std::printf("       %s\n", text.toUtf8().constData());
        std::fflush(stdout);
    }
    void section(const char* title)
    {
        std::printf("\n%s\n", title);
    }
};

QString origins(const QStringList& files)
{
    QStringList kinds;
    for (const QString& f : files) {
        const QString k = ResourceLocator::originName(ResourceLocator::originOf(f));
        if (!kinds.contains(k))
            kinds << k;
    }
    return kinds.join(" + ");
}

// ------------------------------------------------------------------ 1. resources

void checkResources(Report& r)
{
    r.section("1. Embedded resources");
    const QStringList roots = ResourceLocator::roots();
    if (roots.isEmpty())
        r.detail("No resources folder next to the executable: the copies embedded in silex.exe are used.");
    else
        for (const QString& root : roots)
            r.detail("Resource folder: " + QDir::toNativeSeparators(root));

    // lessons 01..09 (valid JSON, parsable lesson)
    QStringList lessons, bad, files;
    for (const QString& f : ResourceLocator::entries("tutorials", {"0*.json"})) {
        const QString rel = "tutorials/" + f;
        QFile file(ResourceLocator::path(rel));
        try {
            if (!file.open(QIODevice::ReadOnly))
                throw std::runtime_error("unreadable");
            lessonFromJson(Json::parse(file.readAll().toStdString()), true);
            lessons << f;
            files << rel;
        }
        catch (...) {
            bad << f;
        }
    }
    const Level lessonLevel = lessons.size() >= 9 && bad.isEmpty() ? Level::Ok : Level::Fail;
    r.line(lessonLevel, QString("Tutorials: %1/9 lessons readable (%2)").arg(lessons.size()).arg(origins(files)));
    if (!bad.isEmpty())
        r.detail("Invalid: " + bad.join(", "));

    // annotated examples referenced by the lessons
    QStringList examples, missingEx;
    for (const QString& f : lessons) {
        QFile file(ResourceLocator::path("tutorials/" + f));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const Json j = Json::parse(file.readAll().toStdString(), nullptr, false);
        const std::string ex = j.is_object() ? j.value("example", std::string{}) : std::string{};
        if (ex.empty())
            continue;
        const QString rel = "examples/" + QString::fromStdString(ex) + ".silex";
        (ResourceLocator::path(rel).isEmpty() ? missingEx : examples) << rel;
    }
    r.line(missingEx.isEmpty() ? Level::Ok : Level::Fail,
           QString("Annotated examples: %1 found (%2)").arg(examples.size()).arg(origins(examples)));
    if (!missingEx.isEmpty())
        r.detail("Missing: " + missingEx.join(", "));

    // components: metadata + STEP model
    QStringList comps, compFiles, missingStep;
    for (const QString& f : ResourceLocator::entries("components", {"*.json"})) {
        QFile file(ResourceLocator::path("components/" + f));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const Json j = Json::parse(file.readAll().toStdString(), nullptr, false);
        if (!j.is_object())
            continue;
        const QString step = "components/" + QString::fromStdString(j.value("step", j.value("id", std::string{}) + ".step"));
        if (ResourceLocator::path(step).isEmpty())
            missingStep << step;
        else {
            comps << f;
            compFiles << "components/" + f << step;
        }
    }
    r.line(comps.size() >= 9 && missingStep.isEmpty() ? Level::Ok : Level::Fail,
           QString("Components: %1/9 with their STEP model (%2)").arg(comps.size()).arg(origins(compFiles)));
    if (!missingStep.isEmpty())
        r.detail("Missing: " + missingStep.join(", "));

    // repair challenges: lesson + broken part
    QStringList repairs, repairFiles, missingPart;
    for (const QString& f : ResourceLocator::entries("tutorials", {"repair_*.json"})) {
        const QString part = "tutorials/" + QFileInfo(f).completeBaseName() + ".silex";
        if (ResourceLocator::path(part).isEmpty())
            missingPart << part;
        else {
            repairs << f;
            repairFiles << "tutorials/" + f << part;
        }
    }
    r.line(repairs.size() >= 5 && missingPart.isEmpty() ? Level::Ok : Level::Fail,
           QString("Repair challenges: %1/5 with their broken part (%2)").arg(repairs.size()).arg(origins(repairFiles)));
    if (!missingPart.isEmpty())
        r.detail("Missing: " + missingPart.join(", "));

    // v0.7: bundled font of the parametric text, read by FreeType
    ResourceLocator::registerBundledFonts();
    const auto font = KernelEngine::measureText("Silex", "Liberation Sans", 6);
    const bool bundled = font.ok && QString::fromStdString(font.fontFile).contains("LiberationSans");
    r.line(bundled ? Level::Ok : Level::Fail,
           bundled ? QString("Text font: Liberation Sans (%1, %2)")
                         .arg(origins({"fonts/LiberationSans-Regular.ttf"}),
                              QDir::toNativeSeparators(QString::fromStdString(font.fontFile)))
                   : QString("Text font: Liberation Sans unusable (%1)").arg(QString::fromStdString(font.error)));
}

// ------------------------------------------------------------------ 2. geometric core

void checkKernel(Report& r)
{
    r.section("2. Geometric core");
    const KernelSelfTest k = KernelEngine::selfTest();
    r.line(k.ok ? Level::Ok : Level::Fail, "OpenCascade (BRepPrimAPI_MakeBox + BRepCheck_Analyzer): " +
                                               QString::fromStdString(k.detail));

    // PlaneGCS: circle centred on the origin with a radius => fully constrained
    try {
        Sketch sk;
        SketchGeo c;
        c.id = newUuid();
        c.kind = GeoKind::Circle;
        c.c = {3, 4};
        c.r = 2;
        sk.geos.push_back(c);
        SketchConstraint co;
        co.id = newUuid();
        co.kind = ConstraintKind::Coincident;
        co.a = {c.id, PointPos::Center};
        co.b = {kOriginId, PointPos::Start};
        SketchConstraint ra;
        ra.id = newUuid();
        ra.kind = ConstraintKind::Radius;
        ra.a = {c.id, PointPos::None};
        ra.value = 5;
        sk.constraints = {co, ra};
        const SolveResult res = SketchSolver::solve(sk);
        const SketchGeo& g = sk.geos.front();
        const bool ok = res.converged && res.dof == 0 && std::abs(g.r - 5) < 1e-9 && std::abs(g.c.x) < 1e-9 &&
                        std::abs(g.c.y) < 1e-9;
        r.line(ok ? Level::Ok : Level::Fail,
               QString("PlaneGCS solver: circle constrained to the origin, R5 -> %1, DOF %2")
                   .arg(res.converged ? "converged" : "NOT converged")
                   .arg(res.dof));
    }
    catch (const std::exception& e) {
        r.line(Level::Fail, QString("PlaneGCS solver: %1").arg(e.what()));
    }
}

// ------------------------------------------------------------------ 3. user folders

void checkWritable(Report& r, const QString& what, const QString& dir)
{
    const QString native = QDir::toNativeSeparators(dir);
    if (!QDir().mkpath(dir)) {
        r.line(Level::Fail, what + ": cannot create " + native);
        return;
    }
    QFile probe(QDir(dir).filePath(QString(".silex_doctor_%1.tmp").arg(QCoreApplication::applicationPid())));
    const bool ok = probe.open(QIODevice::WriteOnly) && probe.write("ok", 2) == 2;
    probe.close();
    probe.remove();
    r.line(ok ? Level::Ok : Level::Fail, what + (ok ? ": writable " : ": NOT writable ") + native);
}

void checkUserFolders(Report& r)
{
    r.section("3. User folders");
    checkWritable(r, "Autosave / recovery", AutosaveManager::recoveryDir());
    checkWritable(r, "My lessons", LessonRecorder::userLessonDir());
    const auto orphans = AutosaveManager::orphans();
    if (!orphans.empty())
        r.line(Level::Warn, QString("%1 autosave file(s) waiting for recovery (offered at the next start-up)")
                                .arg(orphans.size()));
}

// ------------------------------------------------------------------ 4. IPC / MCP

QString runVersion(const QString& exe, const QStringList& args, int* code)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(exe, args);
    if (!p.waitForFinished(8000)) {
        p.kill();
        *code = -1;
        return {};
    }
    *code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    return QString::fromUtf8(p.readAll()).trimmed();
}

void checkIpc(Report& r, const QString& pipeName)
{
    r.section("4. IPC / MCP");
    {
        ModelController model;
        ipc::IpcCommandDispatcher dispatcher(&model);
        ipc::IpcServer server(&dispatcher);
        const QString probe = QString("silex_doctor_%1").arg(QCoreApplication::applicationPid());
        QString err;
        const bool ok = server.listen(probe, &err);
        r.line(ok ? Level::Ok : Level::Fail,
               ok ? "IPC server: can listen on " + server.endpoint() : "IPC server: cannot listen (" + err + ")");
        const Json pong = Json::parse(dispatcher.handleLine(R"({"id":1,"method":"ping"})"), nullptr, false);
        const bool pingOk = pong.is_object() && pong.contains("result");
        r.line(pingOk ? Level::Ok : Level::Fail,
               pingOk ? QString("IPC protocol %1: %2 methods")
                            .arg(ipc::kIpcProtocolVersion)
                            .arg(pong["result"]["methods"].size())
                      : "IPC dispatcher does not answer ping");
    }

    QLocalSocket s;
    s.connectToServer(ipc::IpcServer::endpointFor(pipeName));
    if (s.waitForConnected(500)) {
        r.line(Level::Ok, "Endpoint " + ipc::IpcServer::endpointFor(pipeName) + ": a Silex instance is serving it");
        s.disconnectFromServer();
    }
    else
        r.line(Level::Ok, "Endpoint " + ipc::IpcServer::endpointFor(pipeName) + ": free (Silex will listen on it)");

    // Python runtime of the MCP bridge (optional: only needed to connect an AI agent)
    QString python;
    for (const char* name : {"python", "python3", "py"}) {
        python = QStandardPaths::findExecutable(name);
        if (!python.isEmpty())
            break;
    }
    int code = 0;
    const QString version = python.isEmpty() ? QString() : runVersion(python, {"--version"}, &code);
    if (python.isEmpty() || code != 0 || !version.startsWith("Python")) {
        r.line(Level::Warn, "Python runtime not found: needed only by the MCP bridge (AI agents), "
                            "install Python 3.10+");
    }
    else {
        r.line(Level::Ok, version + " (" + QDir::toNativeSeparators(python) + ")");
        runVersion(python, {"-c", "import mcp"}, &code);
        r.line(code == 0 ? Level::Ok : Level::Warn,
               code == 0 ? "Python package 'mcp' installed"
                         : "Python package 'mcp' missing: pip install -r requirements.txt (MCP bridge only)");
    }
    const QString bridge = QDir(QCoreApplication::applicationDirPath()).filePath("mcp_server.py");
    r.line(QFileInfo::exists(bridge) ? Level::Ok : Level::Warn,
           QFileInfo::exists(bridge) ? "MCP bridge: " + QDir::toNativeSeparators(bridge)
                                     : "MCP bridge mcp_server.py not found next to silex.exe");
}

}  // namespace

int runDoctor(const QString& pipeName)
{
    Report r;
    std::printf("Silex %s (commit %s) - installation diagnostic\n", kSilexVersion, kSilexGitCommit);
    std::printf("Executable: %s\n",
                QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toUtf8().constData());
    checkResources(r);
    checkKernel(r);
    checkUserFolders(r);
    checkIpc(r, pipeName);
    std::printf("\nSummary: %d OK, %d WARN, %d FAIL -> %s\n", r.ok, r.warn, r.fail,
                r.fail ? "Silex is NOT operational" : "Silex is operational");
    std::fflush(stdout);
    return r.fail ? 1 : 0;
}

}  // namespace silex::ui
