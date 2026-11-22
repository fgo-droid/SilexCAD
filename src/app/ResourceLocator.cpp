#include "ResourceLocator.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include "kernel/KernelEngine.h"

namespace silex::ui::ResourceLocator {

namespace {
struct Root
{
    QString dir;
    Origin origin;
};

std::vector<Root> fsRoots()
{
    std::vector<Root> out;
    const QString env = qEnvironmentVariable("SILEX_RESOURCE_DIR");
    if (!env.isEmpty() && QFileInfo(env).isDir())
        out.push_back({QDir(env).absolutePath(), Origin::Override});
    if (QCoreApplication::instance()) {
        const QString bundle = QDir(QCoreApplication::applicationDirPath()).filePath("resources");
        if (QFileInfo(bundle).isDir())
            out.push_back({bundle, Origin::Bundle});
    }
    return out;
}
}  // namespace

QStringList roots()
{
    QStringList r;
    for (const Root& root : fsRoots())
        r << root.dir;
    return r;
}

static std::pair<QString, Origin> resolve(const QString& relative)
{
    for (const Root& root : fsRoots()) {
        const QString p = QDir(root.dir).filePath(relative);
        if (QFileInfo(p).isFile())
            return {p, root.origin};
    }
    const QString embedded = ":/" + relative;
    if (QFileInfo(embedded).isFile())
        return {embedded, Origin::Embedded};
    return {QString(), Origin::Missing};
}

QString path(const QString& relative)
{
    return resolve(relative).first;
}

Origin originOf(const QString& relative)
{
    return resolve(relative).second;
}

QStringList entries(const QString& folder, const QStringList& nameFilters)
{
    QStringList dirs;
    for (const Root& root : fsRoots())
        dirs << QDir(root.dir).filePath(folder);
    dirs << ":/" + folder;
    QStringList names;
    for (const QString& d : dirs)
        for (const QString& f : QDir(d).entryList(nameFilters, QDir::Files, QDir::Name))
            if (!names.contains(f))
                names << f;
    names.sort();
    return names;
}

QString originName(Origin o)
{
    switch (o) {
    case Origin::Override: return QStringLiteral("SILEX_RESOURCE_DIR");
    case Origin::Bundle: return QStringLiteral("bundle");
    case Origin::Embedded: return QStringLiteral("embedded");
    case Origin::Missing: break;
    }
    return QStringLiteral("missing");
}

QString registerBundledFonts()
{
    QString dir;
    for (const QString& f : entries("fonts", {"*.ttf", "*.otf"})) {
        const QString src = path("fonts/" + f);
        if (src.startsWith(":/")) {
            const QString out =
                QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath("Silex/fonts");
            QDir().mkpath(out);
            const QString dst = QDir(out).filePath(f);
            QFile in(src);
            if (QFileInfo(dst).size() != in.size() && in.open(QIODevice::ReadOnly)) {
                QFile::setPermissions(dst, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                QFile o(dst);
                if (o.open(QIODevice::WriteOnly | QIODevice::Truncate))
                    o.write(in.readAll());
            }
            dir = out;
        }
        else
            dir = QFileInfo(src).absolutePath();
        silex::KernelEngine::addFontDirectory(QDir::toNativeSeparators(dir).toStdString());
    }
    return dir;
}

}  // namespace silex::ui::ResourceLocator
