#pragma once
// v0.3 Lot 1.4 — global parameters table (Ctrl+P):
// Name | Expression | Computed value | Unit (mm / deg) | Comment. Expressions may reference
// other parameters; applying the table recomputes the timeline in cascade.

#include <QDialog>
#include <vector>

#include "model/Expression.h"

class QTableWidget;
class QLabel;

namespace silex::ui {

class ModelController;

class ParametersDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ParametersDialog(ModelController* model, QWidget* parent = nullptr);

private:
    void load(const std::vector<Parameter>& params);
    std::vector<Parameter> collect() const;
    void reevaluate();
    void addRow(const Parameter& p);
    bool apply();

    ModelController* m_model;
    QTableWidget* m_table = nullptr;
    QLabel* m_status = nullptr;
    bool m_updating = false;
};

}  // namespace silex::ui
