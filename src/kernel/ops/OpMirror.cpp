// OpMirror: symmetric copy of bodies or of feature tools across a plane (origin plane or any
// planar face). Copies are named "mirror(<featId>,<source name>)" for the TNP layer.

#include <BRepExtrema_DistShapeShape.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Trsf.hxx>

#include "kernel/GeomRefs.h"
#include "kernel/TopoNaming.h"
#include "kernel/ops/Ops.h"

namespace silex::kernel {
namespace {

class OpMirror final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<MirrorParams>();
        gp_Trsf trsf;
        trsf.SetMirror(resolvePlane(ctx, p.plane));
        const std::string tag = "mirror(" + m_node.id + ",";

        if (p.targets.type == "feature") {
            replicateTargets(ctx, p.targets, {trsf}, {tag});
            return;
        }
        if (p.targets.ids.empty())
            throw FeatureError("Select at least one body to mirror");

        BRep_Builder bb;
        TopoDS_Compound preview;
        bb.MakeCompound(preview);
        int extra = 0;
        for (const Uuid& id : p.targets.ids) {
            auto it = ctx.state.bodies.find(id);
            if (it == ctx.state.bodies.end())
                throw FeatureError("A mirrored body no longer exists");
            NamedShape copy = transformedCopy(it->second, trsf, [&](const std::string& n) { return tag + n + ")"; });
            bb.Add(preview, copy.shape());
            bool touching = false;
            if (p.merge) {
                BRepExtrema_DistShapeShape dist(it->second.shape(), copy.shape());
                touching = dist.IsDone() && dist.Value() < 1e-6;
            }
            if (touching) {
                applyBoolean(ctx, copy, BooleanOp::Join, id);
            }
            else {  // separate body
                const Uuid nid = extra == 0 ? m_node.id : m_node.id + "#" + std::to_string(extra);
                ++extra;
                checkSolid(copy.shape(), "The mirror");
                ctx.state.bodies[nid] = std::move(copy);
                ctx.state.bodyOrder.push_back(nid);
                const auto nm = ctx.state.bodyNames.find(id);
                ctx.state.bodyNames[nid] = "Mirror of " + (nm != ctx.state.bodyNames.end() ? nm->second : "body");
            }
            ctx.throwIfCancelled();
        }
        if (ctx.wantPreview) {
            ctx.preview = preview;
            ctx.previewOp = BooleanOp::Join;
        }
    }
};

}  // namespace

std::unique_ptr<ParametricFeature> makeOpMirror(const FeatureNode& n) { return std::make_unique<OpMirror>(n); }

}  // namespace silex::kernel
