#include "ParametersDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "Icons.h"
#include "ModelController.h"
#include "Theme.h"

namespace silex::ui {

namespace {
enum Col { Name, Expr, Value, Unit, Comment, ColCount };
}

ParametersDialog::ParametersDialog(ModelController* model, QWidget* parent) : QDialog(parent), m_model(model)
{
    setWindowTitle(tr("Global parameters"));
    resize(820, 420);
    auto* lay = new QVBoxLayout(this);
    auto* help = new QLabel(
        tr("Named variables usable in every numeric field (sketch dimensions, extrusion, fillet, shell, "
           "patterns): type <i>epaisseur_paroi * 2</i> instead of a number. Operators + - * / ^ ( ), "
           "functions sin cos tan sqrt abs min max round, units mm cm m in deg."),
        this);
    help->setWordWrap(true);
    help->setStyleSheet("color: #9aa3ad;");
    lay->addWidget(help);

    m_table = new QTableWidget(0, ColCount, this);
    m_table->setHorizontalHeaderLabels({tr("Name"), tr("Expression"), tr("Computed value"), tr("Unit"), tr("Comment")});
    m_table->horizontalHeader()->setSectionResizeMode(Comment, QHeaderView::Stretch);
    m_table->setColumnWidth(Name, 160);
    m_table->setColumnWidth(Expr, 200);
    m_table->setColumnWidth(Value, 120);
    m_table->setColumnWidth(Unit, 70);
    m_table->verticalHeader()->hide();
    lay->addWidget(m_table, 1);
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem*) {
        if (!m_updating)
            reevaluate();
    });

    auto* row = new QHBoxLayout();
    auto* add = new QPushButton(tr("Add"), this);
    auto* del = new QPushButton(tr("Remove"), this);
    row->addWidget(add);
    row->addWidget(del);
    m_status = new QLabel(this);
    row->addWidget(m_status, 1);
    lay->addLayout(row);
    connect(add, &QPushButton::clicked, this, [this] {
        int n = m_table->rowCount() + 1;
        QString name;
        do
            name = QString("param%1").arg(n++);
        while (!m_table->findItems(name, Qt::MatchExactly).isEmpty());
        Parameter p;
        p.name = name.toStdString();
        p.expression = "10";
        addRow(p);
        reevaluate();
        m_table->editItem(m_table->item(m_table->rowCount() - 1, Name));
    });
    connect(del, &QPushButton::clicked, this, [this] {
        const int r = m_table->currentRow();
        if (r >= 0) {
            m_table->removeRow(r);
            reevaluate();
        }
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (apply())
            accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { apply(); });

    load(m_model->doc().parameters);
}

void ParametersDialog::addRow(const Parameter& p)
{
    m_updating = true;
    const int r = m_table->rowCount();
    m_table->insertRow(r);
    m_table->setItem(r, Name, new QTableWidgetItem(QString::fromStdString(p.name)));
    m_table->setItem(r, Expr, new QTableWidgetItem(QString::fromStdString(p.expression)));
    auto* v = new QTableWidgetItem();
    v->setFlags(v->flags() & ~Qt::ItemIsEditable);
    m_table->setItem(r, Value, v);
    auto* unit = new QComboBox(m_table);
    unit->addItem("mm", "mm");
    unit->addItem("deg", "deg");
    unit->addItem(tr("(none)"), "");
    unit->setCurrentIndex(std::max(0, unit->findData(QString::fromStdString(p.unit))));
    m_table->setCellWidget(r, Unit, unit);
    m_table->setItem(r, Comment, new QTableWidgetItem(QString::fromStdString(p.comment)));
    m_updating = false;
}

void ParametersDialog::load(const std::vector<Parameter>& params)
{
    m_table->setRowCount(0);
    for (const auto& p : params)
        addRow(p);
    reevaluate();
}

std::vector<Parameter> ParametersDialog::collect() const
{
    std::vector<Parameter> out;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        Parameter p;
        p.name = m_table->item(r, Name) ? m_table->item(r, Name)->text().trimmed().toStdString() : "";
        p.expression = m_table->item(r, Expr) ? m_table->item(r, Expr)->text().trimmed().toStdString() : "0";
        if (auto* c = qobject_cast<QComboBox*>(m_table->cellWidget(r, Unit)))
            p.unit = c->currentData().toString().toStdString();
        p.comment = m_table->item(r, Comment) ? m_table->item(r, Comment)->text().toStdString() : "";
        out.push_back(std::move(p));
    }
    return out;
}

void ParametersDialog::reevaluate()
{
    auto params = collect();
    evaluateParameters(params);
    m_updating = true;
    int errors = 0;
    for (int r = 0; r < m_table->rowCount() && r < int(params.size()); ++r) {
        QTableWidgetItem* v = m_table->item(r, Value);
        const Parameter& p = params[size_t(r)];
        if (p.error.empty()) {
            v->setText(QString::number(p.value, 'g', 10));
            v->setForeground(QBrush(QColor(0xe6, 0xe9, 0xed)));
            v->setToolTip({});
        }
        else {
            ++errors;
            v->setText(tr("error"));
            v->setForeground(QBrush(Colors::Error));
            v->setToolTip(QString::fromStdString(p.error));
        }
        if (QTableWidgetItem* e = m_table->item(r, Expr))
            e->setToolTip(QString::fromStdString(p.error));
    }
    m_updating = false;
    m_status->setText(errors ? QString("<span style='color:%1'>%2</span>")
                                   .arg(Colors::Error.name(), tr("%n parameter(s) in error", nullptr, errors))
                             : tr("%n parameter(s)", nullptr, int(params.size())));
}

bool ParametersDialog::apply()
{
    auto params = collect();
    evaluateParameters(params);
    for (const auto& p : params)
        if (!p.error.empty()) {
            m_status->setText(QString("<span style='color:%1'>%2: %3</span>")
                                  .arg(Colors::Error.name(), QString::fromStdString(p.name).toHtmlEscaped(),
                                       QString::fromStdString(p.error).toHtmlEscaped()));
            return false;
        }
    m_model->execute(SetParametersCommand(params));
    return true;
}

}  // namespace silex::ui
