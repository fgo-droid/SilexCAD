// OpExtrude: linear extrusion of sketch profiles, combined with a body
// (new body / join / cut / intersect), one-sided, reversed or symmetric.

#include <cmath>

#include <BRepPrimAPI_MakePrism.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include "kernel/TopoNaming.h"
#include "kernel/ops/Ops.h"

namespace silex::kernel {
namespace {

class OpExtrude final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<ExtrudeParams>();
        if (std::abs(p.distance) < 1e-7)
            throw FeatureError("The extrusion distance must not be zero");
        const auto regions = selectRegions(ctx, p.profile);
        const Placement& pl = ctx.state.sketches.at(p.profile.sketch).plane;

        double d = p.distance * (p.reversed ? -1.0 : 1.0);
        gp_Vec dir(pl.normal.x, pl.normal.y, pl.normal.z);
        gp_Vec vec = dir * d;

        std::vector<NamedShape> tools;
        for (const SketchRegion* r : regions) {
            NamedShape profile = r->named;
            if (p.symmetric) {
                gp_Trsf t;
                t.SetTranslation(-vec * 0.5);
                profile = profile.moved(TopLoc_Location(t));
            }
            BRepPrimAPI_MakePrism mk(profile.shape(), vec, Standard_False, Standard_True);
            if (!mk.IsDone())
                throw FeatureError("The extrusion failed");
            NamedShape tool = propagateNames(mk.Shape(), {&profile}, historyOf(mk), m_node.id, "side");
            nameCaps(tool, mk.FirstShape(), m_node.id + ":start");
            nameCaps(tool, mk.LastShape(), m_node.id + ":end");
            nameEdgesFromFaces(tool);
            tools.push_back(std::move(tool));
            ctx.throwIfCancelled();
        }

        NamedShape tool = fuseAll(ctx, tools);
        if (ctx.wantPreview) {
            ctx.preview = tool.shape();
            ctx.previewOp = p.op;
        }
        const Uuid body = applyBoolean(ctx, tool, p.op, p.targetBody);
        ctx.state.featureTools[m_node.id] =
            FeatureTool{tool, p.op == BooleanOp::NewBody ? BooleanOp::Join : p.op, body};
    }

    static void nameCaps(NamedShape& ns, const TopoDS_Shape& cap, const std::string& name)
    {
        for (TopExp_Explorer ex(cap, TopAbs_FACE); ex.More(); ex.Next())
            if (const int i = ns.faceIndex(ex.Current()); i > 0)
                ns.setFaceName(i, name);
    }
};

}  // namespace

std::unique_ptr<ParametricFeature> makeOpExtrude(const FeatureNode& n) { return std::make_unique<OpExtrude>(n); }

}  // namespace silex::kernel
