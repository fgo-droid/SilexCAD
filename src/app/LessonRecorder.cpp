#include "LessonRecorder.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QVBoxLayout>

#include "ModelController.h"
#include "model/Pedagogy.h"
#include "sketch/SketchCoach.h"

namespace silex::ui {

namespace {
QString slug(const QString& s)
{
    QString t = s.normalized(QString::NormalizationForm_D).toLower();
    t.remove(QRegularExpression("[\\x{0300}-\\x{036f}]"));
    t.replace(QRegularExpression("[^a-z0-9]+"), "_");
    t.remove(QRegularExpression("^_+|_+$"));
    return t.isEmpty() ? QString("ma_lecon") : t.left(40);
}
}  // namespace

QString LessonRecorder::userLessonDir()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("lessons");
}

QString LessonRecorder::writePack(ModelController* model, const Json& lesson, const QString& dir)
{
    if (!QDir().mkpath(dir))
        return QObject::tr("Cannot create the folder %1").arg(dir);
    const QString id = QString::fromStdString(lesson.value("id", std::string("lesson")));
    const auto problems = validateLessonJson(lesson);
    if (!problems.empty())
        return QObject::tr("The lesson is not valid: %1").arg(QString::fromStdString(problems.front()));
    QFile f(QDir(dir).filePath(id + ".json"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QObject::tr("Cannot write %1").arg(f.fileName());
    f.write(QByteArray::fromStdString(lesson.dump(2)));
    f.close();
    // the reference part, for the guided tour ("Open the annotated example")
    const DocState doc = model->doc();
    QFile s(QDir(dir).filePath(id + ".silex"));
    if (!s.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QObject::tr("Cannot write %1").arg(s.fileName());
    s.write(QByteArray::fromStdString(docToJson(doc).dump(1)));
    return {};
}

LessonRecorder::LessonRecorder(ModelController* model, QWidget* parent) : QDialog(parent), m_model(model)
{
    const bool fr = model->pedagogyFrench();
    setWindowTitle(fr ? QString::fromUtf8("Créer une leçon depuis ce projet") : tr("Create a lesson from this project"));
    resize(980, 620);
    QString base = QFileInfo(model->filePath()).completeBaseName();
    if (base.isEmpty())
        base = fr ? QString::fromUtf8("Ma pièce") : tr("My part");
    if (auto snap = model->snapshot())
        m_draft = lessonFromProject(model->doc(), *snap, slug(base).toStdString(),
                                    (QString::fromUtf8("Leçon : ") + base).toStdString(), ("Lesson: " + base).toStdString());

    auto* lay = new QVBoxLayout(this);
    auto* intro = new QLabel(fr ? QString::fromUtf8("Silex a lu la timeline de ta pièce : une étape par fonction, avec ses critères de "
                                                    "validation automatiques (type d'étape, esquisse contrainte, cotes clés, volume final). "
                                                    "Écris la consigne et l'indice de chaque étape, puis exporte le pack (.json + .silex).")
                                : tr("Silex read the timeline of your part: one step per feature, with automatic validation criteria "
                                     "(step type, constrained sketch, key dimensions, final volume). Write the instruction and the hint "
                                     "of each step, then export the pack (.json + .silex)."),
                             this);
    intro->setWordWrap(true);
    lay->addWidget(intro);
    auto* form = new QFormLayout();
    m_titleFr = new QLineEdit(QString::fromStdString(localizedText(m_draft.value("title", Json()), true)), this);
    m_titleEn = new QLineEdit(QString::fromStdString(localizedText(m_draft.value("title", Json()), false)), this);
    m_level = new QComboBox(this);
    m_level->addItem(QString::fromUtf8("Débutant / Beginner"), 0);
    m_level->addItem(QString::fromUtf8("Intermédiaire / Intermediate"), 1);
    m_level->addItem(QString::fromUtf8("Avancé / Advanced"), 2);
    m_minutes = new QSpinBox(this);
    m_minutes->setRange(5, 240);
    m_minutes->setSuffix(" min");
    m_minutes->setValue(m_draft.value("minutes", 10));
    form->addRow(tr("Title (FR)"), m_titleFr);
    form->addRow(tr("Title (EN)"), m_titleEn);
    form->addRow(tr("Level"), m_level);
    form->addRow(tr("Duration"), m_minutes);
    lay->addLayout(form);

    const Json steps = m_draft.value("steps", Json::array());
    m_table = new QTableWidget(int(steps.size()), 6, this);
    m_table->setHorizontalHeaderLabels({tr("Step"), QString::fromUtf8("Consigne (FR)"), tr("Instruction (EN)"),
                                        QString::fromUtf8("Indice (FR)"), tr("Hint (EN)"), tr("Automatic criteria")});
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setWordWrap(true);
    for (int i = 0; i < int(steps.size()); ++i) {
        const Json& st = steps[size_t(i)];
        auto item = [&](int col, const std::string& text, bool editable) {
            auto* it = new QTableWidgetItem(QString::fromStdString(text));
            if (!editable)
                it->setFlags(it->flags() & ~Qt::ItemIsEditable);
            m_table->setItem(i, col, it);
        };
        item(0, localizedText(st["title"], true), true);
        item(1, localizedText(st["goal"], true), true);
        item(2, localizedText(st["goal"], false), true);
        const Json hint = st.value("hint", Json::object());
        item(3, hint.contains("text") ? localizedText(hint["text"], true) : std::string{}, true);
        item(4, hint.contains("text") ? localizedText(hint["text"], false) : std::string{}, true);
        std::string crit;
        for (const Json& c : st.value("criteria", Json::array()))
            crit += (crit.empty() ? "" : "\n") + criterionLabel(c, fr);
        item(5, crit, false);
    }
    m_table->setColumnWidth(0, 150);
    for (int c = 1; c <= 4; ++c)
        m_table->setColumnWidth(c, 170);
    m_table->resizeRowsToContents();
    lay->addWidget(m_table, 1);

    auto* bb = new QDialogButtonBox(this);
    auto* exp = bb->addButton(fr ? QString::fromUtf8("Exporter le pack…") : tr("Export the pack..."), QDialogButtonBox::ActionRole);
    auto* mine = bb->addButton(fr ? QString::fromUtf8("Ajouter à mes leçons") : tr("Add to my lessons"), QDialogButtonBox::AcceptRole);
    bb->addButton(QDialogButtonBox::Close);
    connect(exp, &QPushButton::clicked, this, &LessonRecorder::exportPack);
    connect(mine, &QPushButton::clicked, this, &LessonRecorder::addToMyLessons);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
    if (steps.empty()) {
        exp->setEnabled(false);
        mine->setEnabled(false);
        intro->setText(fr ? QString::fromUtf8("Le projet est vide : modélise d'abord la pièce de référence.")
                          : tr("The project is empty: model the reference part first."));
    }
}

Json LessonRecorder::lesson() const
{
    Json l = m_draft;
    const QString tf = m_titleFr->text().trimmed(), te = m_titleEn->text().trimmed();
    l["title"] = {{"fr", tf.toStdString()}, {"en", (te.isEmpty() ? tf : te).toStdString()}};
    l["id"] = slug(tf.isEmpty() ? te : tf).toStdString();
    l["example"] = l["id"];
    l["level_rank"] = m_level->currentData().toInt();
    static const char* fr[] = {"Débutant", "Intermédiaire", "Avancé"};
    static const char* en[] = {"Beginner", "Intermediate", "Advanced"};
    const int r = std::clamp(m_level->currentData().toInt(), 0, 2);
    l["level"] = {{"fr", fr[r]}, {"en", en[r]}};
    l["minutes"] = m_minutes->value();
    Json& steps = l["steps"];
    for (int i = 0; i < m_table->rowCount() && i < int(steps.size()); ++i) {
        auto text = [&](int c) { return m_table->item(i, c) ? m_table->item(i, c)->text().trimmed() : QString(); };
        auto both = [&](const QString& a, const QString& b) {
            return Json{{"fr", (a.isEmpty() ? b : a).toStdString()}, {"en", (b.isEmpty() ? a : b).toStdString()}};
        };
        Json& st = steps[size_t(i)];
        st["title"] = both(text(0), text(0));
        if (!text(1).isEmpty() || !text(2).isEmpty())
            st["goal"] = both(text(1), text(2));
        if (!text(3).isEmpty() || !text(4).isEmpty()) {
            if (!st.contains("hint"))
                st["hint"] = Json::object();
            st["hint"]["text"] = both(text(3), text(4));
        }
    }
    return l;
}

void LessonRecorder::exportPack()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Folder of the lesson pack"));
    if (dir.isEmpty())
        return;
    const Json l = lesson();
    if (const QString err = writePack(m_model, l, dir); !err.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), err);
        return;
    }
    m_model->noteEvent("lesson_recorded");
    QMessageBox::information(this, windowTitle(),
                             tr("Lesson pack written: %1.json + %1.silex in %2").arg(QString::fromStdString(l["id"].get<std::string>()), dir));
}

void LessonRecorder::addToMyLessons()
{
    const Json l = lesson();
    if (const QString err = writePack(m_model, l, userLessonDir()); !err.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), err);
        return;
    }
    m_model->noteEvent("lesson_recorded");
    emit lessonAdded();
    accept();
}

}  // namespace silex::ui
