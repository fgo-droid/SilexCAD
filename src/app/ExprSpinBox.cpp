#include "ExprSpinBox.h"

#include <QLineEdit>
#include <cmath>

namespace silex::ui {

namespace {
std::function<ParameterLookup()>& source()
{
    static std::function<ParameterLookup()> s;
    return s;
}
}  // namespace

ExprSpinBox::ExprSpinBox(QWidget* parent) : QDoubleSpinBox(parent)
{
    setToolTip(tr("Value or expression using the global parameters (Ctrl+P), e.g. epaisseur_paroi * 2"));
    setCorrectionMode(QAbstractSpinBox::CorrectToPreviousValue);
}

void ExprSpinBox::setParameterSource(std::function<ParameterLookup()> s) { source() = std::move(s); }

ParameterLookup ExprSpinBox::parameterLookup()
{
    if (source())
        return source()();
    return [](const std::string&) -> std::optional<double> { return std::nullopt; };
}

QString ExprSpinBox::stripAffixes(QString t) const
{
    t = t.trimmed();
    if (!prefix().isEmpty() && t.startsWith(prefix()))
        t.remove(0, prefix().size());
    const QString suf = suffix().trimmed();
    if (!suf.isEmpty() && t.endsWith(suf) && !(suf == "mm" && !isPlainNumber(t.toStdString())))
        t.chop(suf.size());
    t.remove(QChar(0xB0));
    t = t.trimmed();
    if (!t.contains('('))
        t.replace(',', '.');  // French decimal comma
    return t;
}

std::optional<double> ExprSpinBox::evaluate(const QString& t, QString* error) const
{
    if (t.isEmpty())
        return std::nullopt;
    try {
        return evaluateExpression(t.toStdString(), parameterLookup());
    }
    catch (const ExpressionError& e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return std::nullopt;
    }
}

QValidator::State ExprSpinBox::validate(QString& text, int&) const
{
    const auto v = evaluate(stripAffixes(text));
    if (!v)
        return QValidator::Intermediate;
    return (*v >= minimum() - 1e-12 && *v <= maximum() + 1e-12) ? QValidator::Acceptable : QValidator::Intermediate;
}

double ExprSpinBox::valueFromText(const QString& text) const
{
    const QString t = stripAffixes(text);
    const auto v = evaluate(t);
    if (!v)
        return value();
    const_cast<ExprSpinBox*>(this)->setExpr(isPlainNumber(t.toStdString()) ? QString() : t);
    m_exprValue = *v;
    return *v;
}

QString ExprSpinBox::textFromValue(double v) const
{
    if (!m_expr.isEmpty() && std::abs(v - m_exprValue) < std::pow(10.0, -decimals()))
        return m_expr;
    return QString::number(v, 'f', decimals());
}

void ExprSpinBox::stepBy(int steps)
{
    setExpr({});  // arrows / wheel: back to a literal value
    QDoubleSpinBox::stepBy(steps);
}

void ExprSpinBox::focusOutEvent(QFocusEvent* e)
{
    QString err;
    const QString t = stripAffixes(lineEdit()->text());
    if (!evaluate(t, &err) && !err.isEmpty())
        setToolTip(tr("Invalid expression: %1").arg(err));
    QDoubleSpinBox::focusOutEvent(e);
}

void ExprSpinBox::setExpr(const QString& e)
{
    if (e == m_expr)
        return;
    m_expr = e;
    emit exprChanged(m_expr);
}

void ExprSpinBox::setValueAndExpression(double v, const std::string& expr)
{
    const bool block = blockSignals(true);
    m_expr = QString::fromStdString(expr);
    if (!m_expr.isEmpty())
        if (auto x = evaluate(m_expr))
            v = *x;
    m_exprValue = v;
    setValue(v);
    lineEdit()->setText(prefix() + textFromValue(value()) + suffix());
    blockSignals(block);
}

}  // namespace silex::ui
