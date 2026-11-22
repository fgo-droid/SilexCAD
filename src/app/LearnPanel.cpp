#include "LearnPanel.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "Icons.h"
#include "ModelController.h"
#include "Workspace.h"
#include "kernel/KernelEngine.h"
#include "LessonRecorder.h"
#include "ResourceLocator.h"
#include "sketch/SketchCoach.h"

namespace silex::ui {

std::vector<Lesson> LearnPanel::builtinLessons(bool french)
{
    std::vector<Lesson> out;
    for (const QString& f : ResourceLocator::entries("tutorials", {"*.json"})) {
        QFile file(ResourceLocator::path("tutorials/" + f));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        try {
            out.push_back(lessonFromJson(Json::parse(file.readAll().toStdString()), french));
        }
        catch (...) {
        }
    }
    // v0.5 lot 5: lessons created with the recorder ("My lessons")
    QDir mine(LessonRecorder::userLessonDir());
    for (const QString& f : mine.entryList({"*.json"}, QDir::Files, QDir::Name)) {
        QFile file(mine.filePath(f));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        try {
            Lesson l = lessonFromJson(Json::parse(file.readAll().toStdString()), french);
            l.baseDir = mine.absolutePath().toStdString();
            out.push_back(std::move(l));
        }
        catch (...) {
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Lesson& a, const Lesson& b) {
        return std::tie(a.levelRank, a.order) < std::tie(b.levelRank, b.order);
    });
    return out;
}

/// Lessons grouped under non-selectable level headers (Débutant / Intermédiaire / Avancé).
void LearnPanel::fillLessonBox()
{
    const QSignalBlocker block(m_lessonBox);
    const bool fr = m_model->pedagogyFrench();
    m_lessonBox->clear();
    m_lessonBox->addItem(fr ? tr("— choisis un parcours —") : tr("— choose a lesson —"), -1);
    auto* model = qobject_cast<QStandardItemModel*>(m_lessonBox->model());
    int rank = -1;
    for (size_t i = 0; i < m_lessons.size(); ++i) {
        const Lesson& l = m_lessons[i];
        if (l.levelRank != rank) {
            rank = l.levelRank;
            const QString head = l.category == "repair" ? (fr ? QString::fromUtf8("Défis de dépannage (5 min)") : tr("Repair challenges (5 min)"))
                                                        : QString::fromStdString(l.level);
            m_lessonBox->addItem(QString::fromUtf8("\u2014 %1 \u2014").arg(head.toUpper()), -1);
            if (model)
                if (QStandardItem* it = model->item(m_lessonBox->count() - 1)) {
                    it->setFlags(it->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled));
                    QFont f = it->font();
                    f.setBold(true);
                    it->setFont(f);
                }
        }
        m_lessonBox->addItem(QString("   %1  (%2 min)").arg(QString::fromStdString(l.title)).arg(l.minutes), int(i));
        if (int(i) == m_lesson)
            m_lessonBox->setCurrentIndex(m_lessonBox->count() - 1);
    }
}

void LearnPanel::reloadLanguage()
{
    m_lessons = builtinLessons(m_model->pedagogyFrench());
    fillLessonBox();
    refresh();
    if (m_tour >= 0)
        tourStep(0);
}

LearnPanel::LearnPanel(ModelController* model, Workspace* ws, QWidget* parent)
    : QWidget(parent), m_model(model), m_ws(ws)
{
    QSettings settings;
    m_model->setPedagogyFrench(settings.value("pedagogy/french", true).toBool());
    m_lessons = builtinLessons(m_model->pedagogyFrench());

    auto* lay = new QVBoxLayout(this);
    auto* top = new QHBoxLayout();
    m_lessonBox = new QComboBox(this);
    m_lessonBox->setMaxVisibleItems(16);
    fillLessonBox();
    top->addWidget(m_lessonBox, 1);
    m_langBox = new QComboBox(this);
    m_langBox->addItems({"FR", "EN"});
    m_langBox->setCurrentIndex(m_model->pedagogyFrench() ? 0 : 1);
    m_langBox->setToolTip(tr("Language of the explanations (coach, diagnostics)"));
    top->addWidget(m_langBox);
    lay->addLayout(top);
    connect(m_lessonBox, &QComboBox::currentIndexChanged, this,
            [this](int i) { startLesson(m_lessonBox->itemData(i).toInt()); });
    connect(m_langBox, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_model->setPedagogyFrench(i == 0);
        QSettings().setValue("pedagogy/french", i == 0);
        reloadLanguage();
    });

    m_intro = new QLabel(this);
    m_intro->setWordWrap(true);
    m_intro->setStyleSheet("color: #9aa3ad;");
    lay->addWidget(m_intro);
    m_steps = new QLabel(this);
    m_steps->setWordWrap(true);
    m_steps->setTextFormat(Qt::RichText);
    lay->addWidget(m_steps);
    m_current = new QLabel(this);
    m_current->setWordWrap(true);
    m_current->setTextFormat(Qt::RichText);
    m_current->setStyleSheet("background: #1b2633; border-radius: 6px; padding: 8px;");
    lay->addWidget(m_current);
    m_criteria = new QLabel(this);
    m_criteria->setWordWrap(true);
    m_criteria->setTextFormat(Qt::RichText);
    lay->addWidget(m_criteria);
    m_hint = new QPushButton(icon(IconId::Hint), tr("Hint"), this);
    m_hint->setToolTip(tr("Shows where to look (the tool pulses) without doing it for you"));
    connect(m_hint, &QPushButton::clicked, this, [this] {
        if (m_lesson < 0 || m_active >= int(m_lessons[size_t(m_lesson)].steps.size()))
            return;
        const LessonStep& st = m_lessons[size_t(m_lesson)].steps[size_t(m_active)];
        emit hintRequested(QString::fromStdString(st.hintTool), QString::fromStdString(st.hintText));
    });
    lay->addWidget(m_hint);
    m_example = new QPushButton(icon(IconId::Open), tr("Open the annotated example"), this);
    m_example->setToolTip(tr("Opens the finished part of this lesson, annotated step by step (then Guided tour)"));
    connect(m_example, &QPushButton::clicked, this, [this] {
        if (m_lesson >= 0 && m_lessons[size_t(m_lesson)].category == "repair") {
            openRepairPart();
            return;
        }
        if (m_lesson < 0 || m_lessons[size_t(m_lesson)].example.empty())
            return;
        const QString name = QString::fromStdString(m_lessons[size_t(m_lesson)].example) + ".silex";
        // a writable copy: the user can explore and modify it freely
        const QString dst = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath("silex_" + name);
        QFile::remove(dst);
        const std::string& base = m_lessons[size_t(m_lesson)].baseDir;
        const QString src = base.empty() ? ResourceLocator::path("examples/" + name) : QDir(QString::fromStdString(base)).filePath(name);
        if (QFile::copy(src, dst)) {
            QFile(dst).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            emit openExampleRequested(dst);
        }
    });
    lay->addWidget(m_example);

    // guided tour of annotated designs
    auto* sep = new QLabel(tr("<b>Design discovery</b>"), this);
    lay->addSpacing(8);
    lay->addWidget(sep);
    m_tourBtn = new QPushButton(icon(IconId::Learn), tr("Guided tour"), this);
    m_tourBtn->setToolTip(tr("Walks through the timeline step by step with the design intent of each step"));
    connect(m_tourBtn, &QPushButton::clicked, this, [this] { startTour(); });
    lay->addWidget(m_tourBtn);
    m_tourCard = new QLabel(this);
    m_tourCard->setWordWrap(true);
    m_tourCard->setTextFormat(Qt::RichText);
    m_tourCard->setStyleSheet("background: #2a2416; border-radius: 6px; padding: 8px;");
    lay->addWidget(m_tourCard);
    auto* nav = new QHBoxLayout();
    m_prev = new QPushButton(tr("< Previous"), this);
    m_next = new QPushButton(tr("Next >"), this);
    m_stop = new QPushButton(tr("End"), this);
    connect(m_prev, &QPushButton::clicked, this, [this] { tourStep(-1); });
    connect(m_next, &QPushButton::clicked, this, [this] { tourStep(+1); });
    connect(m_stop, &QPushButton::clicked, this, [this] { stopTour(); });
    nav->addWidget(m_prev);
    nav->addWidget(m_next);
    nav->addWidget(m_stop);
    lay->addLayout(nav);

    // v0.5 lot 5: trainers & AI tutor
    lay->addSpacing(8);
    lay->addWidget(new QLabel(tr("<b>Trainers & AI tutor</b>"), this));
    m_record = new QPushButton(icon(IconId::Recorder), tr("Create a lesson from this project"), this);
    m_record->setToolTip(tr("Turns the timeline of this part into an interactive lesson (automatic criteria, your "
                            "instructions and hints), exported as a .json + .silex pack"));
    connect(m_record, &QPushButton::clicked, this, [this] {
        LessonRecorder dlg(m_model, this);
        connect(&dlg, &LessonRecorder::lessonAdded, this, [this] { reloadLanguage(); });
        dlg.exec();
    });
    lay->addWidget(m_record);
    m_tutor = new QPushButton(icon(IconId::Tutor), tr("AI tutor (Socratic mode)"), this);
    m_tutor->setCheckable(true);
    m_tutor->setStyleSheet("QPushButton:checked { background: #1f4a7a; border: 1px solid #4aa3ff; }");
    m_tutor->setChecked(m_model->tutorMode());
    m_tutor->setToolTip(tr("The AI assistant connected through MCP can only observe (model tree, degrees of freedom, "
                           "screenshots): modelling commands are locked, it guides you with questions and hints"));
    connect(m_tutor, &QPushButton::toggled, this, [this](bool on) { m_model->setTutorMode(on); });
    connect(m_model, &ModelController::tutorModeChanged, this, [this](bool on) {
        const QSignalBlocker b(m_tutor);
        m_tutor->setChecked(on);
        render();
    });
    lay->addWidget(m_tutor);
    lay->addStretch(1);

    connect(m_model, &ModelController::snapshotChanged, this, [this] { refresh(); });
    connect(m_model, &ModelController::eventsChanged, this, [this] { refresh(); });
    connect(m_model, &ModelController::documentChanged, this, [this] { render(); });
    render();
}

void LearnPanel::showLessonList() { m_lessonBox->showPopup(); }

void LearnPanel::selectLesson(int index)
{
    const int i = m_lessonBox->findData(index);
    m_lessonBox->setCurrentIndex(i >= 0 ? i : 0);
}

void LearnPanel::startLesson(int index)
{
    m_lesson = index >= 0 && index < int(m_lessons.size()) ? index : -1;
    m_done.clear();
    m_active = 0;
    if (m_lesson >= 0 && m_lessons[size_t(m_lesson)].category == "repair")
        openRepairPart();  // a challenge starts from its broken part
    refresh();
}

void LearnPanel::openRepairPart()
{
    if (m_lesson < 0 || m_lessons[size_t(m_lesson)].start.empty())
        return;
    const QString name = QString::fromStdString(m_lessons[size_t(m_lesson)].start) + ".silex";
    const QString dst = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath("silex_" + name);
    QFile::remove(dst);
    if (QFile::copy(ResourceLocator::path("tutorials/" + name), dst)) {
        QFile(dst).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        m_done.clear();
        m_active = 0;
        emit openRepairRequested(dst);
    }
}

ModelFacts LearnPanel::facts()
{
    auto snap = m_model->snapshot();
    if (!snap)
        return {};
    // the repair challenges need collisions / what-if recomputes: computed once per model revision
    if (m_factsRev == snap->revision && m_factsLesson == m_lesson && m_factsEvents == m_model->events())
        return m_facts;
    ModelFacts f = collectFacts(m_model->doc(), *snap);
    f.events = m_model->events();
    if (m_lesson >= 0) {
        std::vector<Json> all;
        for (const auto& st : m_lessons[size_t(m_lesson)].steps)
            all.insert(all.end(), st.criteria.begin(), st.criteria.end());
        KernelEngine::enrichLessonFacts(f, m_model->doc(), all);
    }
    m_facts = f;
    m_factsRev = snap->revision;
    m_factsLesson = m_lesson;
    m_factsEvents = m_model->events();
    return f;
}

void LearnPanel::refresh()
{
    if (m_lesson >= 0 && m_tour < 0 && !m_model->inPreview())
        if (m_model->snapshot())
            m_active = updateLessonProgress(m_lessons[size_t(m_lesson)], facts(), m_done);
    render();
}

void LearnPanel::render()
{
    const bool fr = m_model->pedagogyFrench();
    m_hint->setVisible(m_lesson >= 0);
    m_hint->setText(fr ? tr("Indice") : tr("Hint"));
    m_record->setText(fr ? QString::fromUtf8("Créer une leçon depuis ce projet") : tr("Create a lesson from this project"));
    m_record->setEnabled(!m_model->doc().features.empty());
    m_tutor->setText(fr ? QString::fromUtf8("Tuteur IA (mode socratique)") : tr("AI tutor (Socratic mode)"));
    const bool repair = m_lesson >= 0 && m_lessons[size_t(m_lesson)].category == "repair";
    m_example->setVisible(m_lesson >= 0 && (!m_lessons[size_t(m_lesson)].example.empty() || repair));
    m_example->setText(repair ? (fr ? QString::fromUtf8("Rouvrir la pièce cassée") : tr("Reopen the broken part"))
                              : fr ? tr("Ouvrir l'exemple annoté") : tr("Open the annotated example"));
    if (m_lesson < 0) {
        m_intro->setText(fr ? tr("Choisis un parcours : chaque étape se valide toute seule dès que ton modèle remplit "
                                 "l'objectif, quelle que soit la façon de dessiner.")
                            : tr("Choose a lesson: each step validates itself as soon as your model meets the goal, "
                                 "whatever the way you draw."));
        m_steps->clear();
        m_current->hide();
        m_criteria->clear();
        m_model->setTutorialState(Json());
    }
    else {
        const Lesson& l = m_lessons[size_t(m_lesson)];
        m_intro->setText(QString::fromStdString(l.intro));
        QString steps;
        for (size_t i = 0; i < l.steps.size(); ++i) {
            const bool done = i < m_done.size() && m_done[i];
            const bool active = int(i) == m_active;
            steps += QString("<div style='color:%1'>%2 %3. %4</div>")
                         .arg(done ? "#5cd68a" : active ? "#4aa3ff" : "#7b8590", done ? "&#10004;" : active ? "&#9654;" : "&#9675;")
                         .arg(i + 1)
                         .arg(QString::fromStdString(l.steps[i].title).toHtmlEscaped());
        }
        m_steps->setText(steps);
        Json crit = Json::array();
        if (m_active >= int(l.steps.size())) {
            m_current->setText(fr ? tr("<b>&#127881; Parcours terminé !</b><br>Tu as validé toutes les étapes.")
                                  : tr("<b>&#127881; Lesson complete!</b><br>Every step is validated."));
            m_criteria->clear();
        }
        else {
            const LessonStep& st = l.steps[size_t(m_active)];
            m_current->setText(QString("<b>%1</b><br>%2").arg(QString::fromStdString(st.title).toHtmlEscaped(),
                                                              QString::fromStdString(st.goal).toHtmlEscaped()));
            const ModelFacts f = facts();
            QString cs;
            for (const Json& c : st.criteria) {
                const bool ok = evaluateCriterion(c, f);
                cs += QString("<div style='color:%1'>%2 %3</div>")
                          .arg(ok ? "#5cd68a" : "#aab3be", ok ? "&#10004;" : "&#9744;",
                               QString::fromStdString(criterionLabel(c, fr)).toHtmlEscaped());
                crit.push_back({{"label", criterionLabel(c, fr)}, {"ok", ok}});
            }
            m_criteria->setText(cs);
        }
        m_current->show();
        // published for the AI tutor (MCP get_pedagogical_context)
        Json state{{"lesson_id", l.id}, {"lesson", l.title}, {"step_index", m_active}, {"step_count", l.steps.size()}};
        if (m_active < int(l.steps.size())) {
            state["step"] = l.steps[size_t(m_active)].title;
            state["goal"] = l.steps[size_t(m_active)].goal;
            state["criteria"] = crit;
        }
        else
            state["completed"] = true;
        m_model->setTutorialState(state);
    }
    const bool tour = m_tour >= 0;
    m_tourBtn->setVisible(!tour);
    m_tourBtn->setEnabled(!m_model->doc().features.empty());
    m_tourBtn->setToolTip(hasAnnotations() ? tr("This design is annotated: discover its design intent step by step")
                                           : tr("Walk through the timeline step by step"));
    for (QPushButton* b : {m_prev.data(), m_next.data(), m_stop.data()})
        b->setVisible(tour);
    m_tourCard->setVisible(tour);
}

bool LearnPanel::hasAnnotations() const
{
    for (const auto& f : m_model->doc().features)
        if (!f->annotation.empty())
            return true;
    return false;
}

void LearnPanel::startTour()
{
    if (m_model->doc().features.empty())
        return;
    m_tour = 0;
    tourStep(0);
}

void LearnPanel::tourStep(int delta)
{
    const DocState& d = m_model->doc();
    const int n = int(d.features.size());
    m_tour = std::clamp(m_tour + delta, 0, n - 1);
    DocState s = d;
    s.marker = m_tour + 1;  // the model as it was at this step
    m_model->preview(s, {});
    const auto& f = d.features[size_t(m_tour)];
    const bool fr = m_model->pedagogyFrench();
    const std::string& note = !fr && !f->annotationEn.empty() ? f->annotationEn : f->annotation;
    const QString text = note.empty()
                             ? (fr ? tr("<i>Pas d'annotation : observe ce que cette étape ajoute ou retire.</i>")
                                   : tr("<i>No annotation: look at what this step adds or removes.</i>"))
                             : QString::fromStdString(note).toHtmlEscaped();
    m_tourCard->setText(QString("<b>%1 / %2 — %3</b> <span style='color:#9aa3ad'>(%4)</span><br>%5")
                            .arg(m_tour + 1)
                            .arg(n)
                            .arg(QString::fromStdString(f->name).toHtmlEscaped(), QString::fromStdString(f->type), text));
    m_prev->setEnabled(m_tour > 0);
    m_next->setEnabled(m_tour < n - 1);
    render();
}

void LearnPanel::stopTour()
{
    m_tour = -1;
    m_model->endPreview();
    render();
}

}  // namespace silex::ui
