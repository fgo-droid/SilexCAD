#include "model/Expression.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

#include "model/Math.h"

namespace silex {

namespace {

constexpr double kDeg = kPi / 180.0;

const std::map<std::string, double>& units()
{
    static const std::map<std::string, double> u{{"mm", 1.0}, {"cm", 10.0}, {"m", 1000.0}, {"in", 25.4},
                                                 {"deg", 1.0}, {"rad", 180.0 / kPi}};
    return u;
}

const std::set<std::string>& functionNames()
{
    static const std::set<std::string> f{"sin",   "cos",   "tan",  "asin", "acos", "atan", "atan2", "sqrt",
                                         "abs",   "min",   "max",  "round", "floor", "ceil", "pow", "hypot"};
    return f;
}

struct Token
{
    enum Kind { End, Number, Ident, Op } kind = End;
    double number = 0;
    std::string text;
    size_t pos = 0;
};

class Parser
{
public:
    Parser(const std::string& s, const ParameterLookup* lookup, std::set<std::string>* idents)
        : m_s(s), m_lookup(lookup), m_idents(idents)
    {
        next();
    }

    double parse()
    {
        if (m_tok.kind == Token::End)
            throw ExpressionError("Empty expression");
        const double v = expr();
        if (m_tok.kind != Token::End)
            fail("Unexpected '" + m_tok.text + "'");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& msg) const
    {
        throw ExpressionError(msg + " (position " + std::to_string(m_tok.pos + 1) + ")");
    }

    void next()
    {
        while (m_i < m_s.size() && std::isspace(static_cast<unsigned char>(m_s[m_i])))
            ++m_i;
        m_tok = Token{};
        m_tok.pos = m_i;
        if (m_i >= m_s.size())
            return;
        const char c = m_s[m_i];
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && m_i + 1 < m_s.size() &&
                                                            std::isdigit(static_cast<unsigned char>(m_s[m_i + 1])))) {
            const char* begin = m_s.c_str() + m_i;
            char* end = nullptr;
            m_tok.number = std::strtod(begin, &end);
            m_tok.kind = Token::Number;
            m_tok.text.assign(begin, static_cast<size_t>(end - begin));
            m_i += static_cast<size_t>(end - begin);
            return;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = m_i;
            while (j < m_s.size() && (std::isalnum(static_cast<unsigned char>(m_s[j])) || m_s[j] == '_'))
                ++j;
            m_tok.kind = Token::Ident;
            m_tok.text = m_s.substr(m_i, j - m_i);
            m_i = j;
            return;
        }
        if (std::string("+-*/%^(),").find(c) != std::string::npos) {
            m_tok.kind = Token::Op;
            m_tok.text = std::string(1, c);
            ++m_i;
            return;
        }
        fail(std::string("Invalid character '") + c + "'");
    }

    bool isOp(const char* op) const { return m_tok.kind == Token::Op && m_tok.text == op; }

    double expr()
    {
        double v = term();
        while (isOp("+") || isOp("-")) {
            const bool plus = m_tok.text == "+";
            next();
            const double r = term();
            v = plus ? v + r : v - r;
        }
        return v;
    }

    double term()
    {
        double v = unary();
        while (isOp("*") || isOp("/") || isOp("%")) {
            const char op = m_tok.text[0];
            next();
            const double r = unary();
            if (op == '*')
                v *= r;
            else {
                if (std::abs(r) < 1e-300)
                    fail("Division by zero");
                v = op == '/' ? v / r : std::fmod(v, r);
            }
        }
        return v;
    }

    // -2^2 = -(2^2) like in mathematics; 2^-1 is accepted (exponent may carry a sign)
    double unary()
    {
        if (isOp("-")) {
            next();
            return -unary();
        }
        if (isOp("+")) {
            next();
            return unary();
        }
        return power();
    }

    double power()
    {
        const double base = atom();
        if (isOp("^")) {
            next();
            return std::pow(base, unary());
        }
        return base;
    }

    double atom()
    {
        if (m_tok.kind == Token::Number) {
            double v = m_tok.number;
            next();
            // a unit directly after a number
            if (m_tok.kind == Token::Ident) {
                auto u = units().find(m_tok.text);
                if (u != units().end()) {
                    v *= u->second;
                    next();
                }
            }
            return v;
        }
        if (isOp("(")) {
            next();
            const double v = expr();
            if (!isOp(")"))
                fail("Missing ')'");
            next();
            return v;
        }
        if (m_tok.kind == Token::Ident) {
            const std::string name = m_tok.text;
            next();
            if (isOp("(")) {
                if (!functionNames().count(name))
                    fail("Unknown function '" + name + "'");
                next();
                std::vector<double> args;
                if (!isOp(")")) {
                    args.push_back(expr());
                    while (isOp(",")) {
                        next();
                        args.push_back(expr());
                    }
                }
                if (!isOp(")"))
                    fail("Missing ')'");
                next();
                return call(name, args);
            }
            if (name == "pi" || name == "PI")
                return kPi;
            if (m_idents)
                m_idents->insert(name);
            if (m_lookup && *m_lookup)
                if (auto v = (*m_lookup)(name))
                    return *v;
            if (!m_lookup)
                return 1.0;  // identifier collection only
            throw ExpressionError("Unknown parameter '" + name + "'");
        }
        if (m_tok.kind == Token::End)
            fail("Unexpected end of expression");
        fail("Unexpected '" + m_tok.text + "'");
    }

    double call(const std::string& f, const std::vector<double>& a)
    {
        auto need = [&](size_t n) {
            if (a.size() != n)
                fail(f + "() expects " + std::to_string(n) + " argument(s)");
        };
        if (f == "min" || f == "max") {
            if (a.empty())
                fail(f + "() expects at least one argument");
            double v = a.front();
            for (double x : a)
                v = f == "min" ? std::min(v, x) : std::max(v, x);
            return v;
        }
        if (f == "atan2" || f == "pow" || f == "hypot") {
            need(2);
            if (f == "atan2") return std::atan2(a[0], a[1]) / kDeg;
            if (f == "pow") return std::pow(a[0], a[1]);
            return std::hypot(a[0], a[1]);
        }
        need(1);
        const double x = a[0];
        if (f == "sin") return std::sin(x * kDeg);
        if (f == "cos") return std::cos(x * kDeg);
        if (f == "tan") return std::tan(x * kDeg);
        if (f == "asin") return std::asin(x) / kDeg;
        if (f == "acos") return std::acos(x) / kDeg;
        if (f == "atan") return std::atan(x) / kDeg;
        if (f == "sqrt") {
            if (x < 0)
                fail("sqrt() of a negative number");
            return std::sqrt(x);
        }
        if (f == "abs") return std::abs(x);
        if (f == "round") return std::round(x);
        if (f == "floor") return std::floor(x);
        return std::ceil(x);
    }

    const std::string& m_s;
    const ParameterLookup* m_lookup;
    std::set<std::string>* m_idents;
    size_t m_i = 0;
    Token m_tok;
};

}  // namespace

double evaluateExpression(const std::string& expr, const ParameterLookup& lookup)
{
    static const ParameterLookup none = [](const std::string&) -> std::optional<double> { return std::nullopt; };
    Parser p(expr, lookup ? &lookup : &none, nullptr);
    const double v = p.parse();
    if (!std::isfinite(v))
        throw ExpressionError("The result is not a finite number");
    return v;
}

std::set<std::string> expressionIdentifiers(const std::string& expr)
{
    std::set<std::string> ids;
    Parser p(expr, nullptr, &ids);
    p.parse();
    return ids;
}

bool isPlainNumber(const std::string& text)
{
    try {
        return expressionIdentifiers(text).empty();
    }
    catch (const ExpressionError&) {
        return false;
    }
}

bool isValidParameterName(const std::string& name)
{
    if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_'))
        return false;
    for (char c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
            return false;
    return !functionNames().count(name) && !units().count(name) && name != "pi" && name != "PI";
}

void to_json(Json& j, const Parameter& p)
{
    j = Json{{"name", p.name}, {"expression", p.expression}, {"unit", p.unit}, {"value", p.value}};
    if (!p.comment.empty())
        j["comment"] = p.comment;
}

void from_json(const Json& j, Parameter& p)
{
    p.name = j.value("name", std::string{});
    p.expression = j.value("expression", std::string("0"));
    p.unit = j.value("unit", std::string("mm"));
    p.comment = j.value("comment", std::string{});
    p.value = j.value("value", 0.0);
    p.error.clear();
}

void evaluateParameters(std::vector<Parameter>& params)
{
    std::map<std::string, size_t> index;
    for (size_t i = 0; i < params.size(); ++i) {
        params[i].error.clear();
        if (!isValidParameterName(params[i].name))
            params[i].error = "Invalid name";
        else if (index.count(params[i].name))
            params[i].error = "Duplicate name";
        else
            index[params[i].name] = i;
    }
    enum class St { Todo, Busy, Done, Failed };
    std::vector<St> st(params.size(), St::Todo);

    std::function<std::optional<double>(size_t)> eval = [&](size_t i) -> std::optional<double> {
        if (st[i] == St::Done)
            return params[i].value;
        if (st[i] == St::Failed)
            return std::nullopt;
        if (st[i] == St::Busy) {
            params[i].error = "Circular reference";
            return std::nullopt;
        }
        st[i] = St::Busy;
        std::optional<double> v;
        if (params[i].error.empty()) {
            try {
                std::string dependencyError;
                const double x = evaluateExpression(params[i].expression, [&](const std::string& n) -> std::optional<double> {
                    auto it = index.find(n);
                    if (it == index.end())
                        return std::nullopt;
                    auto r = eval(it->second);
                    if (!r) {
                        dependencyError = params[it->second].error == "Circular reference"
                                              ? "Circular reference"
                                              : "Depends on '" + n + "' which has an error";
                        throw ExpressionError(dependencyError);
                    }
                    return r;
                });
                v = x;
            }
            catch (const ExpressionError& e) {
                if (params[i].error.empty())
                    params[i].error = e.what();
            }
        }
        if (v) {
            params[i].value = *v;
            st[i] = St::Done;
        }
        else {
            st[i] = St::Failed;
        }
        return v;
    };
    for (size_t i = 0; i < params.size(); ++i)
        eval(i);
}

std::map<std::string, double> parameterValues(const std::vector<Parameter>& params)
{
    std::map<std::string, double> out;
    for (const auto& p : params)
        if (p.error.empty())
            out[p.name] = p.value;
    return out;
}

ParameterLookup lookupIn(const std::map<std::string, double>& values)
{
    return [&values](const std::string& n) -> std::optional<double> {
        auto it = values.find(n);
        if (it == values.end())
            return std::nullopt;
        return it->second;
    };
}

}  // namespace silex
