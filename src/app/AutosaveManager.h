#pragma once
// Background autosave and crash recovery of the open design.
//
// Every N minutes (2 by default, Help ▾ > Autosave), and only when the document has unsaved
// changes, the document is written silently to <recoveryDir()>/<name>_<hash>.autosave.json.
// The file is deleted as soon as the document becomes clean again (Ctrl+S, open, new) and when
// Silex closes normally: a file found at start-up therefore comes from a session that ended
// unexpectedly, and offerRecovery() proposes to restore it.

#include <QObject>
#include <QString>
#include <QTimer>
#include <vector>

#include "model/Recovery.h"

class QWidget;

namespace silex::ui {

class ModelController;

class AutosaveManager : public QObject
{
    Q_OBJECT
public:
    explicit AutosaveManager(ModelController* model, QObject* parent = nullptr);
    ~AutosaveManager() override;

    /// $SILEX_RECOVERY_DIR, or <AppData/Local>/Silex/recovery.
    static QString recoveryDir();
    /// Autosave files left by sessions that are no longer running (newest first).
    static std::vector<recovery::AutosaveInfo> orphans();

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);
    int intervalMinutes() const { return m_minutes; }
    void setIntervalMinutes(int minutes);

    /// Writes the autosave now if the document has unsaved changes. Returns false on I/O error.
    bool saveNow(QString* error = nullptr);
    /// Deletes this session's autosave file (clean close, document saved).
    void discard();
    QString currentFile() const { return m_written; }

    /// Start-up dialog: "Silex closed unexpectedly. Restore the autosave of <name> (X minutes ago)?"
    /// Restore / Ignore / Delete for every orphan. Returns true if a document was restored.
    bool offerRecovery(QWidget* parent);
    /// Restores one autosave file into the document (marked modified), protects it with this
    /// session's autosave, then deletes the restored file. `silex --recover <file>`, dialog.
    bool restoreFile(const QString& file, QString* error = nullptr);

private:
    void onDocumentChanged();
    QString targetFile() const;
    int intervalMs() const;

    ModelController* m_model;
    QTimer m_timer;
    QString m_session;   ///< key of an unsaved document
    QString m_written;   ///< autosave file currently on disk for this session
    bool m_enabled = true;
    int m_minutes = 2;
};

}  // namespace silex::ui
