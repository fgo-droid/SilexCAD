#pragma once
// v0.5 lot 5 — "Create a lesson from this project" (Learn panel): Silex reads the timeline of the
// trainer's part, proposes one step per feature with automatic validation criteria, the trainer writes
// the instructions and hints (FR / EN), then exports a self-contained pack (.json + .silex) or adds it
// to "My lessons" (loaded by the Learn panel at startup).

#include <QDialog>
#include <QString>

#include "model/Json.h"

class QComboBox;
class QLineEdit;
class QSpinBox;
class QTableWidget;

namespace silex::ui {

class ModelController;

class LessonRecorder final : public QDialog
{
    Q_OBJECT
public:
    LessonRecorder(ModelController* model, QWidget* parent = nullptr);

    /// Folder of the user's lessons (<AppData>/lessons): <id>.json + <id>.silex.
    static QString userLessonDir();
    /// Writes <dir>/<id>.json and <dir>/<id>.silex (copy of the current project). Error message or "".
    static QString writePack(ModelController* model, const Json& lesson, const QString& dir);

signals:
    void lessonAdded();

private:
    Json lesson() const;  ///< the generated lesson with the edited texts
    void exportPack();
    void addToMyLessons();

    ModelController* m_model;
    Json m_draft;
    QLineEdit *m_titleFr, *m_titleEn;
    QComboBox* m_level;
    QSpinBox* m_minutes;
    QTableWidget* m_table;
};

}  // namespace silex::ui
