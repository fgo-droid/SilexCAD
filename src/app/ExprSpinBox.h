#pragma once
// Numeric field that also accepts an expression over the global parameters
// (e.g. "epaisseur_paroi * 2", "entraxe_x / 2 + 1 mm"). The evaluated value is the spin box
// value; the expression itself is kept and stored in the feature (params["expr"]).

#include <QDoubleSpinBox>
#include <functional>
#include <string>

#include "model/Expression.h"

namespace silex::ui {

class ExprSpinBox : public QDoubleSpinBox
{
    Q_OBJECT
public:
    explicit ExprSpinBox(QWidget* parent = nullptr);

    /// Where parameter values come from (set once by the main window).
    static void setParameterSource(std::function<ParameterLookup()> source);
    static ParameterLookup parameterLookup();

    /// Bound expression ("" when the field holds a literal value).
    std::string expression() const { return m_expr.toStdString(); }
    /// Shows `expr` (or `value` when expr is empty) without emitting exprChanged.
    void setValueAndExpression(double value, const std::string& expr);

signals:
    void exprChanged(const QString& expr);

protected:
    QValidator::State validate(QString& text, int& pos) const override;
    double valueFromText(const QString& text) const override;
    QString textFromValue(double v) const override;
    void stepBy(int steps) override;
    void focusOutEvent(QFocusEvent* e) override;

private:
    QString stripAffixes(QString t) const;
    std::optional<double> evaluate(const QString& t, QString* error = nullptr) const;
    void setExpr(const QString& e);

    QString m_expr;
    mutable double m_exprValue = 0;
};

}  // namespace silex::ui
