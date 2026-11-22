#pragma once
// Numeric fields of features driven by global parameters.
//
// A feature stores its bound fields in params["expr"]: { "<json pointer>": "<expression>" },
// e.g. {"/distance": "epaisseur_paroi * 2", "/dir1/count": "n_trous"}. A sketch dimension stores
// its expression in SketchConstraint::expr. The evaluated value is always written back into the
// regular field, so the kernel and every reader only ever see plain numbers.

#include <set>
#include <string>
#include <vector>

#include "model/Document.h"

namespace silex {

/// Expression bound to a field ("" when the field holds a literal value).
std::string fieldExpression(const Json& params, const std::string& pointer);
/// Binds (or unbinds with an empty / plain-number expression) a field.
void setFieldExpression(Json& params, const std::string& pointer, const std::string& expr);

struct BindingResult
{
    DocState state;
    std::set<Uuid> sketchesToSolve;     ///< sketches whose dimensions changed (solver must run)
    std::vector<std::string> errors;    ///< "<feature name>: <field>: <message>"
};

/// v0.7: replaces every {expression} of a text template by its value ("W={largeur}" -> "W=60").
/// "{expr:N}" forces N decimals; "{{" and "}}" are literal braces. Whole numbers are written
/// without decimals, others with at most 3. Throws ExpressionError (with the faulty expression).
std::string interpolateText(const std::string& tpl, const ParameterLookup& lookup);
/// Formats a parameter value the way interpolateText does.
std::string formatNumber(double v, int decimals = -1);

/// Evaluates the parameter table and writes every bound value into its field.
BindingResult applyFeatureBindings(const DocState& s);

}  // namespace silex
