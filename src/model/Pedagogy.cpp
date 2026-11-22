#include "model/Pedagogy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

namespace silex {

namespace {

bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

std::string num(double v)
{
    char b[32];
    if (std::abs(v - std::round(v)) < 1e-9)
        std::snprintf(b, sizeof b, "%.0f", v);
    else
        std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

}  // namespace

std::string explainFeatureError(const std::string& type, const std::string& message, const Json& params,
                                const ErrorContext& ctx, bool fr)
{
    const double avail = ctx.smallestBodyExtent;
    if ((type == "Fillet" || type == "Chamfer") && (has(message, "failed") || has(message, "too large"))) {
        const double v = params.value("value", 0.0);
        const bool fil = type == "Fillet";
        if (fr)
            return std::string(fil ? "Le rayon demandé (" : "La distance de chanfrein demandée (") + num(v) +
                   " mm) est trop grande pour l'arête sélectionnée" +
                   (avail > 0 ? " (hauteur disponible : " + num(avail) + " mm). Réduis-la sous " + num(avail / 2) + " mm."
                              : ". Réduis-la, ou choisis une arête plus longue.");
        return std::string(fil ? "The requested radius (" : "The requested chamfer distance (") + num(v) +
               " mm) is too large for the selected edge" +
               (avail > 0 ? " (available height: " + num(avail) + " mm). Use less than " + num(avail / 2) + " mm."
                          : ". Reduce it, or pick a longer edge.");
    }
    if (has(message, "no closed profile") || (type == "Extrude" && has(message, "profile")))
        return fr ? "Aucun contour fermé détecté : zoome sur les sommets bleus pour vérifier qu'ils sont bien "
                    "collés (carré jaune à l'accrochage), ou supprime le trait en trop qui dépasse."
                  : "No closed outline found: zoom on the blue vertices to check they are really joined (yellow "
                    "square when snapping), or delete the stray line that sticks out.";
    if (type == "Revolve" && has(message, "axis"))
        return fr ? "Le profil traverse l'axe de rotation : décale-le d'un seul côté de l'axe (ou choisis un autre axe)."
                  : "The profile crosses the rotation axis: move it to one side of the axis (or choose another axis).";
    if (type == "Shell" && has(message, "thickness"))
        return fr ? "L'épaisseur de coque est trop grande pour cette pièce" +
                        (avail > 0 ? std::string(" (la partie la plus fine fait ") + num(avail) + " mm). Essaie moins de " + num(avail / 3) + " mm."
                                   : std::string(". Réduis-la."))
                  : "The shell thickness is too large for this part" +
                        (avail > 0 ? std::string(" (its thinnest part is ") + num(avail) + " mm). Try less than " + num(avail / 3) + " mm."
                                   : std::string(". Reduce it."));
    if (has(message, "no body to cut"))
        return fr ? "Il n'y a encore aucune pièce à creuser : commence par une extrusion « New body »."
                  : "There is no part to cut yet: start with a 'New body' extrusion.";
    if (has(message, "no longer exists"))
        return fr ? "Une face ou arête utilisée par cette étape a disparu après une modification en amont. "
                    "Double-clique l'étape et re-sélectionne-la."
                  : "A face or edge used by this step disappeared after an upstream change. Double-click the step "
                    "and pick it again.";
    if (has(message, "re-attached"))
        return fr ? "Une référence a été retrouvée par sa forme : vérifie que l'opération est toujours au bon endroit."
                  : "A reference was found again by its shape: check that the operation is still at the right place.";
    if (has(message, "thread") && has(message, "diameter"))
        return fr ? "Le diamètre de la face ne correspond pas au filetage choisi : une vis se modélise au diamètre "
                    "nominal (M6 → Ø6), un trou au diamètre de perçage (M6 → Ø5)."
                  : "The face diameter does not match the thread: model a screw at the nominal diameter (M6 -> 6 mm) "
                    "and a hole at the tap drill (M6 -> 5 mm).";
    if (has(message, "does not cut the body"))
        return fr ? "Le plan de coupe ne traverse pas la pièce : change son décalage." : "The cutting plane misses the body: change its offset.";
    if (has(message, "boolean"))
        return fr ? "L'opération booléenne a échoué : les formes se touchent peut-être juste par une face ou une "
                    "arête. Décale légèrement l'une d'elles (0,1 mm) ou augmente la distance."
                  : "The boolean failed: the shapes probably only touch along a face or an edge. Shift one of them "
                    "slightly (0.1 mm) or increase the distance.";
    return {};
}

// ------------------------------------------------------------------ lessons

std::string localizedText(const Json& v, bool french)
{
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_object()) {
        const char* a = french ? "fr" : "en";
        const char* b = french ? "en" : "fr";
        if (v.contains(a) && v[a].is_string())
            return v[a].get<std::string>();
        if (v.contains(b) && v[b].is_string())
            return v[b].get<std::string>();
    }
    return {};
}

Lesson lessonFromJson(const Json& j, bool french)
{
    auto txt = [&](const Json& o, const char* key) { return o.contains(key) ? localizedText(o[key], french) : std::string{}; };
    Lesson l;
    l.french = french;
    l.id = j.value("id", std::string{});
    l.title = txt(j, "title");
    l.level = txt(j, "level");
    l.order = j.value("order", 0);
    const std::string lv = j.contains("level") ? localizedText(j["level"], false) : std::string{};
    const std::string lvFr = j.contains("level") ? localizedText(j["level"], true) : std::string{};
    l.levelRank = j.value("level_rank", (lv == "Advanced" || lvFr.rfind("Avanc", 0) == 0) ? 2
                                        : (lv == "Intermediate" || lvFr.rfind("Interm", 0) == 0) ? 1
                                                                                                   : 0);
    l.minutes = j.value("minutes", 0);
    l.intro = txt(j, "intro");
    l.example = j.value("example", std::string{});
    l.category = j.value("category", std::string("course"));
    l.start = j.value("start", std::string{});
    if (l.category == "repair" && !j.contains("level_rank"))
        l.levelRank = 3;  // after the advanced courses
    for (const Json& s : j.value("steps", Json::array())) {
        LessonStep st;
        st.title = txt(s, "title");
        st.goal = txt(s, "goal");
        const Json hint = s.value("hint", Json::object());
        st.hintTool = hint.value("tool", std::string{});
        st.hintText = txt(hint, "text");
        for (const Json& c : s.value("criteria", Json::array()))
            st.criteria.push_back(c);
        l.steps.push_back(std::move(st));
    }
    return l;
}

namespace {
bool okStatus(FeatureStatus s) { return s == FeatureStatus::Ok || s == FeatureStatus::Warning; }

const Json* pointerValue(const Json& params, const std::string& ptr)
{
    try {
        const Json::json_pointer jp(ptr);
        if (params.contains(jp))
            return &params.at(jp);
    }
    catch (...) {
    }
    return nullptr;
}
}  // namespace

bool evaluateCriterion(const Json& c, const ModelFacts& f)
{
    const std::string type = c.value("type", std::string{});
    if (type == "feature") {
        const std::string ft = c.value("feature", std::string{});
        const std::string op = c.value("op", std::string{});
        int n = 0;
        for (const auto& x : f.features)
            if (x.type == ft && okStatus(x.status) && (op.empty() || x.params.value("op", std::string{}) == op))
                ++n;
        return n >= c.value("min", 1);
    }
    if (type == "sketch_dof") {
        if (f.sketchDof.empty())
            return false;
        return f.sketchDof.back() == c.value("value", 0);
    }
    if (type == "body_count") {
        const int n = int(f.bodies.size());
        return n >= c.value("min", 1) && n <= c.value("max", 1 << 20);
    }
    if (type == "body_volume" && !c.contains("value")) {
        const double lo = c.value("min", 0.0), hi = c.value("max", 1e300);
        return std::any_of(f.bodies.begin(), f.bodies.end(),
                           [&](const ModelFacts::Body& b) { return b.volume >= lo && b.volume <= hi; });
    }
    if (type == "body_volume") {
        const double v = c.value("value", 0.0), tol = c.value("tol", 0.01);
        return std::any_of(f.bodies.begin(), f.bodies.end(),
                           [&](const ModelFacts::Body& b) { return std::abs(b.volume - v) <= tol * std::max(1.0, std::abs(v)); });
    }
    if (type == "parameter") {
        auto it = f.parameters.find(c.value("name", std::string{}));
        if (it == f.parameters.end())
            return false;
        if (c.contains("value"))
            return std::abs(it->second - c["value"].get<double>()) <= c.value("tol", 1e-6);
        return it->second >= c.value("min", -1e300) && it->second <= c.value("max", 1e300);
    }
    if (type == "feature_param") {
        const std::string ft = c.value("feature", std::string{});
        const std::string which = c.value("which", std::string("last"));
        const ModelFacts::Feature* hit = nullptr;
        for (const auto& x : f.features)
            if (x.type == ft && okStatus(x.status)) {
                hit = &x;
                if (which == "first")
                    break;
            }
        if (!hit)
            return false;
        const Json* v = pointerValue(hit->params, c.value("pointer", std::string{}));
        if (!v)
            return false;
        if (v->is_string())
            return c.contains("equals") && v->get<std::string>() == c["equals"].get<std::string>();
        if (!v->is_number() && !v->is_array() && !v->is_boolean())
            return false;
        const double x = v->is_array() ? double(v->size()) : v->is_boolean() ? double(v->get<bool>()) : v->get<double>();
        if (c.contains("value"))
            return std::abs(x - c["value"].get<double>()) <= c.value("tol", 1e-6);
        return x >= c.value("min", -1e300) && x <= c.value("max", 1e300);
    }
    if (type == "sketch_constraint") {  // e.g. a Concentric constraint in some sketch
        const std::string kind = c.value("kind", std::string{});
        int n = 0;
        for (const auto& x : f.features)
            if (x.type == "Sketch" && x.params.contains("sketch"))
                for (const Json& k : x.params["sketch"].value("constraints", Json::array()))
                    n += k.value("kind", std::string{}) == kind;
        return n >= c.value("min", 1);
    }
    if (type == "no_errors")
        return std::none_of(f.features.begin(), f.features.end(),
                            [](const ModelFacts::Feature& x) { return x.status == FeatureStatus::Error; });
    // ---- v0.5 lot 2: repair challenges
    if (type == "all_sketches_dof")
        return !f.sketchDof.empty() &&
               std::all_of(f.sketchDof.begin(), f.sketchDof.end(), [&](int d) { return d == c.value("value", 0); });
    if (type == "holes_symmetric") {
        if (f.holes.empty() || !f.bounds.valid())
            return false;
        const bool alongY = c.value("axis", std::string("x")) == "y";
        const double mid = alongY ? (f.bounds.min.y + f.bounds.max.y) / 2 : (f.bounds.min.x + f.bounds.max.x) / 2;
        const double tol = c.value("tol", 0.05);
        for (const Vec3& h : f.holes) {
            const double m = 2 * mid - (alongY ? h.y : h.x);
            const bool partner = std::any_of(f.holes.begin(), f.holes.end(), [&](const Vec3& o) {
                return std::abs((alongY ? o.y : o.x) - m) <= tol && std::abs((alongY ? o.x : o.y) - (alongY ? h.x : h.y)) <= tol;
            });
            if (!partner)
                return false;
        }
        return true;
    }
    if (type == "hole_at" || type == "no_hole_at") {
        const double x = c.value("x", 0.0), y = c.value("y", 0.0), tol = c.value("tol", 0.5);
        const bool there = std::any_of(f.holes.begin(), f.holes.end(),
                                       [&](const Vec3& h) { return std::abs(h.x - x) <= tol && std::abs(h.y - y) <= tol; });
        return type == "hole_at" ? there : !there;
    }
    if (type == "no_collision")
        return f.collisions == 0;
    if (type == "robust") {
        auto it = f.robust.find(c.dump());
        return it != f.robust.end() && it->second;
    }
    if (type == "event")
        return f.events.count(c.value("name", std::string{})) > 0;
    if (type == "any_of") {
        for (const Json& sub : c.value("of", Json::array()))
            if (evaluateCriterion(sub, f))
                return true;
        return false;
    }
    return false;
}

std::string criterionLabel(const Json& c, bool french)
{
    if (c.contains("label"))
        return localizedText(c["label"], french);
    return c.value("type", std::string("?"));
}

std::vector<std::string> validateLessonJson(const Json& j)
{
    std::vector<std::string> bad;
    auto bilingual = [&](const Json& o, const char* key, const std::string& where, bool required) {
        if (!o.contains(key)) {
            if (required)
                bad.push_back(where + ": missing '" + key + "'");
            return;
        }
        const Json& v = o[key];
        if (!v.is_object() || !v.contains("fr") || !v.contains("en") || !v["fr"].is_string() || !v["en"].is_string() ||
            v["fr"].get<std::string>().empty() || v["en"].get<std::string>().empty())
            bad.push_back(where + ": '" + key + "' needs non-empty \"fr\" and \"en\" texts");
    };
    // required keys of each criterion type
    static const std::map<std::string, std::vector<std::string>> kReq = {
        {"feature", {"feature"}},     {"sketch_dof", {"value"}}, {"body_count", {}},
        {"body_volume", {}},          {"parameter", {"name"}},   {"feature_param", {"feature", "pointer"}},
        {"no_errors", {}},            {"event", {"name"}},       {"any_of", {"of"}},
        {"sketch_constraint", {"kind"}},
        // v0.5 lot 2
        {"all_sketches_dof", {}},     {"holes_symmetric", {}},   {"hole_at", {"x", "y"}},
        {"no_hole_at", {"x", "y"}},   {"no_collision", {}},      {"robust", {"parameter", "value", "check"}}};
    static const std::set<std::string> kEvents = {"interference_ok", "drawing_exported", "3mf_exported",
                                                  "printability_ok", "component_inserted", "enclosure_created"};
    std::function<void(const Json&, const std::string&, bool)> crit = [&](const Json& c, const std::string& where,
                                                                         bool top) {
        const std::string t = c.value("type", std::string{});
        auto it = kReq.find(t);
        if (it == kReq.end()) {
            bad.push_back(where + ": unknown criterion type '" + t + "'");
            return;
        }
        for (const auto& k : it->second)
            if (!c.contains(k))
                bad.push_back(where + ": criterion '" + t + "' needs '" + k + "'");
        if (t == "body_volume" && !c.contains("value") && !c.contains("min") && !c.contains("max"))
            bad.push_back(where + ": body_volume needs 'value' or 'min'/'max'");
        if (t == "event" && !kEvents.count(c.value("name", std::string{})))
            bad.push_back(where + ": unknown event '" + c.value("name", std::string{}) + "'");
        if (t == "any_of")
            for (const Json& sub : c.value("of", Json::array()))
                crit(sub, where + "/any_of", false);
        if (t == "robust") {
            if (!c.value("check", Json()).is_array() || c["check"].empty())
                bad.push_back(where + ": robust needs a non-empty 'check' list");
            else
                for (const Json& sub : c["check"])
                    crit(sub, where + "/robust", false);
        }
        if (top)
            bilingual(c, "label", where, true);
    };
    if (!j.is_object())
        return {"not a JSON object"};
    if (j.value("id", std::string{}).empty())
        bad.push_back("missing 'id'");
    for (const char* k : {"title", "level", "intro"})
        bilingual(j, k, "lesson", true);
    if (!j.value("minutes", 0))
        bad.push_back("lesson: missing 'minutes'");
    const Json steps = j.value("steps", Json::array());
    if (steps.empty())
        bad.push_back("lesson: no steps");
    for (size_t i = 0; i < steps.size(); ++i) {
        const std::string w = "step " + std::to_string(i + 1);
        bilingual(steps[i], "title", w, true);
        bilingual(steps[i], "goal", w, true);
        if (steps[i].contains("hint"))
            bilingual(steps[i]["hint"], "text", w + "/hint", true);
        if (steps[i].contains("annotation"))
            bilingual(steps[i], "annotation", w, false);
        const Json cs = steps[i].value("criteria", Json::array());
        if (cs.empty())
            bad.push_back(w + ": no criteria");
        for (const Json& c : cs)
            crit(c, w, true);
    }
    return bad;
}

int updateLessonProgress(const Lesson& l, const ModelFacts& f, std::vector<bool>& done)
{
    done.resize(l.steps.size(), false);
    for (size_t i = 0; i < l.steps.size(); ++i) {
        if (done[i])
            continue;
        const auto& cs = l.steps[i].criteria;
        const bool ok = std::all_of(cs.begin(), cs.end(), [&](const Json& c) { return evaluateCriterion(c, f); });
        if (!ok)
            return int(i);
        done[i] = true;  // validated, possibly several steps at once
    }
    return int(l.steps.size());
}

}  // namespace silex
