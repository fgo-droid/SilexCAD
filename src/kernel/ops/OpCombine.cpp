// OpCombine: boolean between existing bodies (target Join / Cut / Intersect tools).
// Names of both sides are propagated through the boolean history, so later features keep
// their references on the combined body.

#include <algorithm>

#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>

#include "kernel/TopoNaming.h"
#include "kernel/ops/Ops.h"

namespace silex::kernel {
namespace {

class OpCombine final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<CombineParams>();
        if (p.op == BooleanOp::NewBody)
            throw FeatureError("Combine needs Join, Cut or Intersect");
        auto& bodies = ctx.state.bodies;
        if (p.target.empty() || !bodies.count(p.target))
            throw FeatureError("The target body no longer exists");
        std::vector<NamedShape> tools;
        for (const Uuid& id : p.tools) {
            if (id == p.target)
                throw FeatureError("A body cannot be combined with itself");
            auto it = bodies.find(id);
            if (it == bodies.end())
                throw FeatureError("A tool body no longer exists");
            tools.push_back(it->second);
        }
        if (tools.empty())
            throw FeatureError("Select at least one tool body");

        if (ctx.wantPreview) {
            // Show the tool bodies tinted by the operation; the model keeps its inputs so the
            // user can still pick bodies while editing.
            BRep_Builder b;
            TopoDS_Compound c;
            b.MakeCompound(c);
            for (const auto& t : tools)
                b.Add(c, t.shape());
            ctx.preview = c;
            ctx.previewOp = p.op;
            ctx.previewKeepsInput = true;
        }

        const NamedShape tool = fuseAll(ctx, tools);
        applyBoolean(ctx, tool, p.op, p.target);

        if (!p.keepTools)
            for (const Uuid& id : p.tools) {
                bodies.erase(id);
                std::erase(ctx.state.bodyOrder, id);
                ctx.state.bodyNames.erase(id);
            }
    }
};

}  // namespace

std::unique_ptr<ParametricFeature> makeOpCombine(const FeatureNode& n) { return std::make_unique<OpCombine>(n); }

}  // namespace silex::kernel
