#pragma once
// Global parameters (named variables) and the arithmetic expression evaluator used by every
// numeric field of the model (sketch dimensions, extrusion distance, fillet radius, shell
// thickness, pattern counts and spacings...). OCCT-free and Qt-free.
//
// Grammar (usual precedence, ^ right associative):
//   expr   := term (('+' | '-') term)*
//   term   := unary (('*' | '/' | '%') unary)*
//   unary  := ('+' | '-') unary | power
//   power  := atom ('^' unary)?
//   atom   := number [unit] | identifier | identifier '(' args ')' | '(' expr ')'
// Units after a number: mm (1), cm (10), m (1000), in (25.4), deg (1), rad (180/pi).
// Lengths are millimetres, angles are degrees. Functions: sin cos tan (degrees), asin acos
// atan atan2 (degrees), sqrt abs min max round floor ceil pow hypot. Constant: pi.

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "model/Json.h"

namespace silex {

class ExpressionError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// Value of a named parameter, or nullopt when it does not exist.
using ParameterLookup = std::function<std::optional<double>(const std::string&)>;

/// Throws ExpressionError (syntax error, unknown name, division by zero...).
double evaluateExpression(const std::string& expr, const ParameterLookup& lookup = {});
/// Identifiers referenced by an expression (functions and constants excluded). Throws on syntax errors.
std::set<std::string> expressionIdentifiers(const std::string& expr);
/// True when `text` is just a number (optionally with a unit): no parameter involved.
bool isPlainNumber(const std::string& text);
/// [A-Za-z_][A-Za-z0-9_]*, not a function / constant / unit name.
bool isValidParameterName(const std::string& name);

struct Parameter
{
    std::string name;
    std::string expression = "0";
    std::string unit = "mm";  ///< "mm" | "deg" | "" (unitless)
    std::string comment;
    // computed by evaluateParameters (persisted for readability, recomputed on load)
    double value = 0;
    std::string error;
};

void to_json(Json& j, const Parameter& p);
void from_json(const Json& j, Parameter& p);

/// Evaluates the table (any order, cross references allowed). Cycles, unknown names and
/// syntax errors are reported in Parameter::error (value then keeps its previous content).
void evaluateParameters(std::vector<Parameter>& params);
/// name -> value of the parameters without error.
std::map<std::string, double> parameterValues(const std::vector<Parameter>& params);
ParameterLookup lookupIn(const std::map<std::string, double>& values);

}  // namespace silex
