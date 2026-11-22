#pragma once
// v0.4 lot 3 — "Learn / Guided mode" panel:
//  * structured lessons (resources/tutorials/*.json) validated automatically by inspecting the
//    model (TutorialEngine), never imposing one way of drawing;
//  * progressive hint: the expected tool pulses in the tool bar (without doing the work);
//  * guided tour of annotated designs: the timeline marker walks through the steps with their
//    design intent (FeatureNode::annotation).

#include <QPointer>
#include <QWidget>
#include <vector>

#include "model/Pedagogy.h"

class QComboBox;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace silex::ui {

class ModelController;
class Workspace;

class LearnPanel final : public QWidget
{
    Q_OBJECT
public:
    LearnPanel(ModelController* model, Workspace* ws, QWidget* parent = nullptr);

    /// Lesson files embedded in the application (":/tutorials"), sorted by level then number.
    static std::vector<Lesson> builtinLessons(bool french = true);

    void refresh();
    void selectLesson(int index);  ///< 0-based, -1 = none
    void startTour();
    void showLessonList();  ///< opens the lesson drop-down (docs / QA screenshots)

signals:
    void hintRequested(const QString& tool, const QString& text);
    /// The lesson's annotated example (a copy of ":/examples/<name>.silex") should be opened.
    void openExampleRequested(const QString& path);
    /// v0.5: a repair challenge opens its broken part (no guided tour).
    void openRepairRequested(const QString& path);

private:
    void startLesson(int index);
    void openRepairPart();
    void fillLessonBox();
    ModelFacts facts();  ///< facts of the current model (cached per revision)
    void reloadLanguage();
    void render();
    void tourStep(int delta);
    void stopTour();
    bool hasAnnotations() const;

    ModelController* m_model;
    Workspace* m_ws;
    std::vector<Lesson> m_lessons;
    int m_lesson = -1;
    int m_active = 0;
    std::vector<bool> m_done;
    int m_tour = -1;  ///< step of the guided tour (-1: off)

    QPointer<QComboBox> m_lessonBox, m_langBox;
    QPointer<QLabel> m_intro, m_steps, m_current, m_criteria, m_tourCard;
    QPointer<QPushButton> m_hint, m_tourBtn, m_prev, m_next, m_stop, m_example, m_record, m_tutor;
    ModelFacts m_facts;
    uint64_t m_factsRev = ~0ull;
    int m_factsLesson = -2;
    std::set<std::string> m_factsEvents;
};

}  // namespace silex::ui
