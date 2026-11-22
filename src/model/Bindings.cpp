#include "model/Bindings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace silex {

std::string fieldExpression(const Json& params, const std::string& pointer)
{
    if (!params.is_object() || !params.contains("expr") || !params["expr"].is_object())
        return {};
    return params["expr"].value(pointer, std::string{});
}

void setFieldExpression(Json& params, const std::string& pointer, const std::string& expr)
{
    if (!params.is_object())
        return;
    if (expr.empty() || isPlainNumber(expr)) {
        if (params.contains("expr") && params["expr"].is_object()) {
            params["expr"].erase(pointer);
            if (params["expr"].empty())
                params.erase("expr");
        }
        return;
    }
    params["expr"][pointer] = expr;
}

std::string formatNumber(double v, int decimals)
{
    char buf[64];
    if (decimals >= 0) {
        std::snprintf(buf, sizeof buf, "%.*f", std::min(decimals, 12), v);
        return buf;
    }
    if (std::abs(v - std::round(v)) < 1e-9 && std::abs(v) < 1e15) {
        std::snprintf(buf, sizeof buf, "%.0f", std::round(v) == 0 ? 0.0 : std::round(v));
        return buf;
    }
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0')
        s.pop_back();
    if (!s.empty() && s.back() == '.')
        s.pop_back();
    return s;
}

std::string interpolateText(const std::string& tpl, const ParameterLookup& lookup)
{
    std::string out;
    for (size_t i = 0; i < tpl.size(); ++i) {
        const char ch = tpl[i];
        if (ch == '{' && i + 1 < tpl.size() && tpl[i + 1] == '{') {
            out += '{';
            ++i;
            continue;
        }
        if (ch == '}' && i + 1 < tpl.size() && tpl[i + 1] == '}') {
            out += '}';
            ++i;
            continue;
        }
        if (ch != '{') {
            out += ch;
            continue;
        }
        const size_t close = tpl.find('}', i + 1);
        if (close == std::string::npos)
            throw ExpressionError("Missing '}' after '" + tpl.substr(i) + "'");
        std::string expr = tpl.substr(i + 1, close - i - 1);
        int decimals = -1;
        if (const size_t colon = expr.rfind(':'); colon != std::string::npos) {
            const std::string spec = expr.substr(colon + 1);
            if (!spec.empty() && spec.size() <= 2 && std::all_of(spec.begin(), spec.end(), ::isdigit)) {
                decimals = std::stoi(spec);
                expr.resize(colon);
            }
        }
        try {
            out += formatNumber(evaluateExpression(expr, lookup), decimals);
        }
        catch (const ExpressionError& e) {
            throw ExpressionError("{" + expr + "}: " + e.what());
        }
        i = close;
    }
    return out;
}

BindingResult applyFeatureBindings(const DocState& s)
{
    BindingResult out;
    out.state = s;
    DocState& n = out.state;
    evaluateParameters(n.parameters);
    const auto values = parameterValues(n.parameters);
    const ParameterLookup lookup = lookupIn(values);

    for (auto& fp : n.features) {
        const FeatureNode& f = *fp;
        std::shared_ptr<FeatureNode> patched;
        auto node = [&]() -> FeatureNode& {
            if (!patched)
                patched = std::make_shared<FeatureNode>(f);
            return *patched;
        };

        // ---- feature fields
        if (f.params.is_object() && f.params.contains("expr") && f.params["expr"].is_object()) {
            for (const auto& [ptr, je] : f.params["expr"].items()) {
                if (!je.is_string())
                    continue;
                double v = 0;
                try {
                    v = evaluateExpression(je.get<std::string>(), lookup);
                }
                catch (const ExpressionError& e) {
                    out.errors.push_back(f.name + ": " + ptr + ": " + e.what());
                    continue;
                }
                Json::json_pointer jp;
                try {
                    jp = Json::json_pointer(ptr);
                }
                catch (...) {
                    out.errors.push_back(f.name + ": invalid field '" + ptr + "'");
                    continue;
                }
                if (!f.params.contains(jp) || !f.params[jp].is_number())
                    continue;
                const Json& cur = f.params[jp];
                // Extrusion: a negative distance means "reversed".
                if (f.type == FeatureType::Extrude && ptr == "/distance") {
                    const bool rev = v < 0;
                    if (std::abs(cur.get<double>() - std::abs(v)) > 1e-12 || f.params.value("reversed", false) != rev) {
                        node().params[jp] = std::abs(v);
                        node().params["reversed"] = rev;
                    }
                    continue;
                }
                if (cur.is_number_integer()) {
                    const auto iv = static_cast<long long>(std::llround(v));
                    if (cur.get<long long>() != iv)
                        node().params[jp] = iv;
                }
                else if (std::abs(cur.get<double>() - v) > 1e-12) {
                    node().params[jp] = v;
                }
            }
        }

        // ---- v0.7: parametric text
        if (f.type == FeatureType::EmbossText && f.params.is_object()) {
            const std::string tpl = f.params.value("text", std::string{});
            std::string resolved = tpl;
            try {
                resolved = interpolateText(tpl, lookup);
            }
            catch (const ExpressionError& e) {
                out.errors.push_back(f.name + ": text: " + e.what());
            }
            if (f.params.value("resolved", std::string{}) != resolved)
                node().params["resolved"] = resolved;
        }

        // ---- sketch dimensions
        if (f.type == FeatureType::Sketch && f.params.contains("sketch")) {
            const Json& cons = f.params["sketch"].value("constraints", Json::array());
            for (size_t i = 0; i < cons.size(); ++i) {
                const std::string expr = cons[i].value("expr", std::string{});
                if (expr.empty())
                    continue;
                double v = 0;
                try {
                    v = evaluateExpression(expr, lookup);
                }
                catch (const ExpressionError& e) {
                    out.errors.push_back(f.name + ": dimension: " + e.what());
                    continue;
                }
                const std::string kind = cons[i].value("kind", std::string{});
                const double cur = cons[i].value("value", 0.0);
                if (kind == "Angle")
                    v = v * kPi / 180.0;
                else if (kind == "DistanceX" || kind == "DistanceY")
                    v = std::copysign(std::abs(v), cur == 0 ? (v < 0 ? -1.0 : 1.0) : cur);
                else if (v <= 0) {
                    out.errors.push_back(f.name + ": dimension: '" + expr + "' must be positive");
                    continue;
                }
                if (std::abs(cur - v) > 1e-12) {
                    node().params["sketch"]["constraints"][i]["value"] = v;
                    out.sketchesToSolve.insert(f.id);
                }
            }
        }
        if (patched)
            fp = std::move(patched);
    }
    return out;
}

}  // namespace silex
