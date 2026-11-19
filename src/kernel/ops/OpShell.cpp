// OpShell: hollows a solid (BRepOffsetAPI_MakeThickSolid), removed faces become openings.

#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <TopoDS.hxx>

#include "kernel/TopoNaming.h"
#include "kernel/ops/Ops.h"

namespace silex::kernel {
namespace {

class OpShell final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<ShellParams>();
        if (p.thickness <= 0)
            throw FeatureError("The shell thickness must be positive");

        const double size = ctx.state.modelSize();
        Uuid bodyId = p.body;
        NCollection_List<TopoDS_Shape> faces;
        for (const TopoRef& ref : p.removedFaces) {
            auto r = resolveRef(ctx.state.bodies, ref, TopAbs_FACE, size);
            if (!r)
                throw FeatureError("A face to remove no longer exists");
            if (!r->byName)
                ctx.warnings.push_back("A face reference was re-attached by geometry");
            if (bodyId.empty())
                bodyId = r->owner;
            if (r->owner != bodyId)
                throw FeatureError("All removed faces must belong to the shelled body");
            faces.Append(r->shape);
        }
        if (bodyId.empty() && !ctx.state.bodyOrder.empty())
            bodyId = ctx.state.bodyOrder.back();
        auto it = ctx.state.bodies.find(bodyId);
        if (it == ctx.state.bodies.end())
            throw FeatureError("There is no body to shell");
        NamedShape& body = it->second;

        BRepOffsetAPI_MakeThickSolid mk;
        mk.MakeThickSolidByJoin(body.shape(), faces, p.outward ? p.thickness : -p.thickness, 1e-3, BRepOffset_Skin, false, false,
                                GeomAbs_Arc, false, ctx.progress());
        ctx.throwIfCancelled();
        if (!mk.IsDone())
            throw FeatureError("The shell failed (thickness too large for the geometry?)");
        NamedShape result = propagateNames(mk.Shape(), {&body}, historyOf(mk), m_node.id, "shell");
        checkSolid(result.shape(), "The shell");
        if (ctx.wantPreview) {
            ctx.preview = result.shape();
            ctx.previewOp = BooleanOp::Join;
            ctx.previewKeepsInput = true;  // keep the original faces pickable while editing
        }
        body = std::move(result);
    }
};

}  // namespace

std::unique_ptr<ParametricFeature> makeOpShell(const FeatureNode& n) { return std::make_unique<OpShell>(n); }

}  // namespace silex::kernel
