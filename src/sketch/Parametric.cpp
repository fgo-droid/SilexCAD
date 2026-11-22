#include "sketch/Parametric.h"

#include "sketch/SketchSolver.h"

namespace silex {

DocState applyParameters(const DocState& s)
{
    BindingResult r = applyFeatureBindings(s);
    for (const Uuid& id : r.sketchesToSolve) {
        const int i = r.state.indexOf(id);
        if (i < 0)
            continue;
        auto f = std::make_shared<FeatureNode>(*r.state.features[size_t(i)]);
        try {
            SketchParams sp = f->as<SketchParams>();
            SketchSolver::solve(sp.sketch);
            f->params = Json(sp);
            r.state.features[size_t(i)] = std::move(f);
        }
        catch (const std::exception&) {
            // keep the patched values: the kernel will report the problem on rebuild
        }
    }
    return std::move(r.state);
}

}  // namespace silex
