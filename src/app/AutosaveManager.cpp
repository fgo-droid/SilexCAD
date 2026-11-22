#include "AutosaveManager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>

#include "ModelController.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#endif

namespace silex::ui {

namespace {
std::filesystem::path toPath(const QString& s)
{
    return std::filesystem::path(s.toStdWString());
}

bool processAlive(int64_t pid)
{
    if (pid <= 0)
        return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h)
        return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return ::kill(pid_t(pid), 0) == 0;
#endif
}

bool french()
{
    return QLocale().language() == QLocale::French;
}
}  // namespace

int AutosaveManager::intervalMs() const
{
    // QA / tests: SILEX_AUTOSAVE_INTERVAL_S overrides the preference (seconds)
    const int s = qEnvironmentVariableIntValue("SILEX_AUTOSAVE_INTERVAL_S");
    return s > 0 ? s * 1000 : m_minutes * 60 * 1000;
}


AutosaveManager::AutosaveManager(ModelController* model, QObject* parent)
    : QObject(parent), m_model(model), m_session(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    QSettings s;
    m_enabled = s.value("autosave/enabled", true).toBool();
    m_minutes = qBound(1, s.value("autosave/interval_min", 2).toInt(), 120);
    m_timer.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, [this] { saveNow(); });
    connect(m_model, &ModelController::documentChanged, this, &AutosaveManager::onDocumentChanged);
    if (m_enabled)
        m_timer.start(intervalMs());
}

AutosaveManager::~AutosaveManager()
{
    // destroyed with the main window: Silex is closing normally (the user already chose to save
    // or to discard the changes), nothing has to be recovered
    discard();
}

QString AutosaveManager::recoveryDir()
{
    const QString env = qEnvironmentVariable("SILEX_RECOVERY_DIR");
    if (!env.isEmpty())
        return QDir(env).absolutePath();
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath("Silex/recovery");
}

std::vector<recovery::AutosaveInfo> AutosaveManager::orphans()
{
    std::vector<recovery::AutosaveInfo> out;
    const int64_t self = QCoreApplication::applicationPid();
    for (auto& i : recovery::scan(toPath(recoveryDir())))
        if (i.pid == self || !processAlive(i.pid))  // another running Silex still owns its file
            out.push_back(std::move(i));
    return out;
}

void AutosaveManager::setEnabled(bool on)
{
    m_enabled = on;
    QSettings().setValue("autosave/enabled", on);
    if (on)
        m_timer.start(intervalMs());
    else {
        m_timer.stop();
        discard();
    }
}

void AutosaveManager::setIntervalMinutes(int minutes)
{
    m_minutes = qBound(1, minutes, 120);
    QSettings().setValue("autosave/interval_min", m_minutes);
    if (m_enabled)
        m_timer.start(intervalMs());
}

QString AutosaveManager::targetFile() const
{
    const std::string name = recovery::autosaveFileName(m_model->filePath().toStdString(), m_session.toStdString());
    return QDir(recoveryDir()).filePath(QString::fromStdString(name));
}

bool AutosaveManager::saveNow(QString* error)
{
    if (!m_model->isDirty())
        return true;
    const QString file = targetFile();
    try {
        recovery::AutosaveInfo meta;
        meta.sourcePath = m_model->filePath().toStdString();
        meta.pid = QCoreApplication::applicationPid();
        recovery::writeAutosave(toPath(file), m_model->doc(), meta);
    }
    catch (const std::exception& e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return false;
    }
    if (!m_written.isEmpty() && m_written != file)
        QFile::remove(m_written);  // the document was renamed (Save as) since the last autosave
    m_written = file;
    return true;
}

void AutosaveManager::discard()
{
    if (!m_written.isEmpty())
        QFile::remove(m_written);
    m_written.clear();
}

void AutosaveManager::onDocumentChanged()
{
    // saved, opened or new document: nothing left to protect
    if (!m_model->isDirty())
        discard();
}

bool AutosaveManager::offerRecovery(QWidget* parent)
{
    bool restored = false;
    for (const recovery::AutosaveInfo& info : orphans()) {
        const QString name = QString::fromStdString(info.displayName);
        const qint64 secs = QDateTime::currentSecsSinceEpoch() - info.savedAtEpoch;
        const int minutes = int(qMax<qint64>(0, secs) / 60);
        const QString source = QString::fromStdString(info.sourcePath);
        const bool fr = french();

        QMessageBox box(parent);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(fr ? "Récupération — Silex" : "Recovery — Silex");
        box.setText(fr ? QString("Silex s'est fermé de manière inattendue. Voulez-vous restaurer la sauvegarde "
                                 "automatique de <b>%1</b> (il y a %2 minute(s)) ?").arg(name.toHtmlEscaped()).arg(minutes)
                       : QString("Silex closed unexpectedly. Do you want to restore the autosave of <b>%1</b> "
                                 "(%2 minute(s) ago)?").arg(name.toHtmlEscaped()).arg(minutes));
        box.setInformativeText(source.isEmpty()
                                   ? (fr ? "Ce projet n'avait jamais été enregistré." : "This project was never saved.")
                                   : (fr ? "Fichier d'origine : " : "Original file: ") + QDir::toNativeSeparators(source));
        QPushButton* restore = box.addButton(fr ? "Restaurer" : "Restore", QMessageBox::AcceptRole);
        QPushButton* ignore = box.addButton(fr ? "Ignorer" : "Ignore", QMessageBox::RejectRole);
        QPushButton* del = box.addButton(fr ? "Supprimer" : "Delete", QMessageBox::DestructiveRole);
        box.setDefaultButton(restore);
        box.setEscapeButton(ignore);
        if (restored) {
            restore->hide();  // one document at a time: the others stay for the next start-up
            box.setDefaultButton(ignore);
        }
        box.exec();

        const QString file = QString::fromStdWString(info.file.wstring());
        if (box.clickedButton() == del) {
            QFile::remove(file);
            continue;
        }
        if (box.clickedButton() != restore || restored)
            continue;  // Ignore: asked again at the next start-up
        QString err;
        if (restoreFile(file, &err))
            restored = true;
        else
            QMessageBox::critical(parent, box.windowTitle(), err);
    }
    return restored;
}

bool AutosaveManager::restoreFile(const QString& file, QString* error)
{
    try {
        recovery::AutosaveInfo info;
        DocState doc = recovery::readAutosave(toPath(file), &info);
        m_model->restore(std::move(doc), QString::fromStdString(info.sourcePath));
    }
    catch (const std::exception& e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return false;
    }
    saveNow();            // protected again by this session's own autosave...
    if (QFileInfo(file).absoluteFilePath() != QFileInfo(m_written).absoluteFilePath())
        QFile::remove(file);  // ...before the orphan disappears
    return true;
}

}  // namespace silex::ui
