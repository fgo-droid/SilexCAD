// OpFillet / OpChamfer: constant radius fillets and symmetric chamfers on edges designated
// by persistent topological references.

#include <algorithm>
#include <map>

#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>

#include "kernel/TopoNaming.h"
#include "kernel/ops/Ops.h"

namespace silex::kernel {
namespace {

class OpBlend final : public ParametricFeature
{
public:
    OpBlend(const FeatureNode& n, bool fillet) : ParametricFeature(n), m_fillet(fillet) {}

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<EdgeBlendParams>();
        const char* what = m_fillet ? "fillet" : "chamfer";
        if (p.edges.empty())
            throw FeatureError(std::string("Select at least one edge to ") + what);
        if (p.value <= 0)
            throw FeatureError(std::string("The ") + what + " size must be positive");

        // Resolve every reference (TNP) and group edges per body
        const double size = ctx.state.modelSize();
        std::map<Uuid, std::vector<TopoDS_Edge>> perBody;
        int lost = 0;
        for (const TopoRef& ref : p.edges) {
            auto r = resolveRef(ctx.state.bodies, ref, TopAbs_EDGE, size);
            if (!r) {
                ++lost;
                continue;
            }
            if (!r->byName)
                ctx.warnings.push_back("An edge reference was re-attached by geometry");
            auto& v = perBody[r->owner];
            const TopoDS_Edge e = TopoDS::Edge(r->shape);
            if (std::none_of(v.begin(), v.end(), [&](const TopoDS_Edge& x) { return x.IsSame(e); }))
                v.push_back(e);
        }
        if (perBody.empty())
            throw FeatureError("None of the selected edges exist anymore");
        if (lost)
            ctx.warnings.push_back(std::to_string(lost) + " edge reference(s) lost");

        for (auto& [bodyId, edges] : perBody) {
            NamedShape& body = ctx.state.bodies.at(bodyId);
            NamedShape result;
            if (m_fillet) {
                BRepFilletAPI_MakeFillet mk(body.shape());
                for (const auto& e : edges)
                    mk.Add(p.value, e);
                mk.Build(ctx.progress());
                ctx.throwIfCancelled();
                if (!mk.IsDone())
                    throw FeatureError("The fillet failed (radius too large for the adjacent faces?)");
                result = propagateNames(mk.Shape(), {&body}, historyOf(mk), m_node.id, "fillet");
            }
            else {
                BRepFilletAPI_MakeChamfer mk(body.shape());
                for (const auto& e : edges)
                    mk.Add(p.value, e);
                mk.Build(ctx.progress());
                ctx.throwIfCancelled();
                if (!mk.IsDone())
                    throw FeatureError("The chamfer failed (distance too large?)");
                result = propagateNames(mk.Shape(), {&body}, historyOf(mk), m_node.id, "chamfer");
            }
            checkSolid(result.shape(), m_fillet ? "The fillet" : "The chamfer");
            body = std::move(result);
        }
        if (ctx.wantPreview) {
            BRep_Builder b;
            TopoDS_Compound c;
            b.MakeCompound(c);
            for (const auto& [bodyId, edges] : perBody)
                b.Add(c, ctx.state.bodies.at(bodyId).shape());
            ctx.preview = c;
            ctx.previewOp = BooleanOp::Join;
            ctx.previewKeepsInput = true;
        }
    }

private:
    bool m_fillet;
};

}  // namespace

std::unique_ptr<ParametricFeature> makeOpFillet(const FeatureNode& n) { return std::make_unique<OpBlend>(n, true); }
std::unique_ptr<ParametricFeature> makeOpChamfer(const FeatureNode& n) { return std::make_unique<OpBlend>(n, false); }

}  // namespace silex::kernel
