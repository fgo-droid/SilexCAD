// v0.5 lot 2 — facts of the repair challenges that need the kernel: collisions between bodies and the
// robustness test (Silex changes a parameter on a copy of the model, recomputes it and re-checks).

#include <algorithm>
#include <functional>

#include "kernel/KernelEngine.h"
#include "sketch/Parametric.h"
#include "sketch/SketchCoach.h"

namespace silex {

void KernelEngine::enrichLessonFacts(ModelFacts& f, const DocState& doc, const std::vector<Json>& criteria)
{
    std::function<void(const Json&)> visit = [&](const Json& c) {
        const std::string t = c.value("type", std::string{});
        if (t == "any_of")
            for (const Json& sub : c.value("of", Json::array()))
                visit(sub);
        if (t == "no_collision" && f.collisions < 0) {
            const InterferenceReport r = checkInterference(doc, {}, false);
            f.collisions = r.error.empty()
                               ? int(std::count_if(r.pairs.begin(), r.pairs.end(),
                                                   [](const InterferencePair& p) { return p.status == "collision"; }))
                               : -1;
        }
        if (t == "robust" && !f.robust.count(c.dump())) {
            const std::string name = c.value("parameter", std::string{});
            std::vector<Parameter> ps = doc.parameters;
            auto it = std::find_if(ps.begin(), ps.end(), [&](const Parameter& p) { return p.name == name; });
            bool ok = false;
            if (it != ps.end()) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%.10g", c.value("value", 0.0));
                it->expression = buf;
                const DocState what = ParametricCommand(SetParametersCommand(ps)).apply(doc);
                const auto snap = computeSnapshot(what);
                ModelFacts g = collectFacts(what, *snap);
                g.events = f.events;
                const std::vector<Json> checks = c.value("check", Json::array()).get<std::vector<Json>>();
                enrichLessonFacts(g, what, checks);
                ok = std::all_of(checks.begin(), checks.end(), [&](const Json& x) { return evaluateCriterion(x, g); });
            }
            f.robust[c.dump()] = ok;
        }
    };
    for (const Json& c : criteria)
        visit(c);
}

}  // namespace silex
