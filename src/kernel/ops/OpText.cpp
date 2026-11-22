// v0.7 lot A — parametric text: relief (depth > 0, Join) or engraving (depth < 0, Cut) on a planar face,
// an origin plane or a construction plane.
//
// The letters (TextOutline) are extruded along the plane normal (BRepPrimAPI_MakePrism). The tool starts
// 0.01 mm on the other side of the face so that no boolean is coplanar; the volume added or removed is
// still exactly area x depth. Faces are named "<id>:text<i>" (deterministic), so a fillet or a sketch
// on a letter survives a recompute as long as the text keeps the same letters.

#include <cmath>
#include <cstdio>

#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Builder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>

#include "kernel/TopoNaming.h"
#include "kernel/ops/Ops.h"
#include "kernel/ops/TextOutline.h"

namespace silex::kernel {
namespace {

class OpText final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<EmbossTextParams>();
        if (std::abs(p.depth) < 1e-6)
            throw FeatureError("The depth must not be zero (> 0 relief, < 0 engraving)");
        if (p.height <= 0)
            throw FeatureError("The letter height must be positive");
        const std::string text = !p.resolved.empty() || p.text.find('{') != std::string::npos ? p.resolved : p.text;

        Uuid body = p.targetBody;
        if (p.plane.kind == "Face") {
            if (auto r = resolveRef(ctx.state.bodies, p.plane.face, TopAbs_FACE, ctx.state.modelSize()))
                body = r->owner;
            else
                throw FeatureError("The face holding the text no longer exists");
        }
        const Placement pl = resolveBasePlane(ctx, p.plane);
        const Vec3 n = pl.normal.normalized();
        const bool relief = p.depth > 0;
        const double eps = 0.01;
        // letters drawn on a plane shifted to where the tool starts
        Placement start = pl;
        start.origin = pl.origin + n * (relief ? -eps : eps);

        TextLayout L;
        L.text = text;
        L.font = p.font;
        L.height = p.height;
        L.align = p.align;
        L.lineSpacing = p.lineSpacing;
        L.angleDeg = p.angle;
        L.anchor = {p.x, p.y};
        TextOutline outline;
        try {
            outline = buildTextOutline(L, start);
        }
        catch (const std::exception& e) {
            throw FeatureError(e.what());
        }
        ctx.throwIfCancelled();
        for (const auto& w : outline.warnings)
            ctx.warnings.push_back(w);
        if (outline.strokeWidth > 0 && outline.strokeWidth < p.nozzle - 1e-9) {
            char buf[200];
            std::snprintf(buf, sizeof buf,
                          "Letter strokes are about %.2f mm wide, thinner than the %.2f mm nozzle: increase the height "
                          "(at least %.1f mm) or use a bolder font",
                          outline.strokeWidth, p.nozzle, p.height * p.nozzle / outline.strokeWidth);
            ctx.warnings.push_back(buf);
        }

        const double len = std::abs(p.depth) + eps;
        const Vec3 v = n * (relief ? len : -len);
        BRep_Builder bb;
        TopoDS_Compound solids;
        bb.MakeCompound(solids);
        int count = 0;
        for (TopExp_Explorer ex(outline.faces, TopAbs_FACE); ex.More(); ex.Next()) {
            BRepPrimAPI_MakePrism prism(ex.Current(), gp_Vec(v.x, v.y, v.z));
            if (!prism.IsDone())
                throw FeatureError("A letter could not be extruded");
            bb.Add(solids, prism.Shape());
            ++count;
        }
        if (count == 0)
            throw FeatureError("The text is empty");

        NamedShape tool(solids);
        for (int i = 1; i <= tool.faceCount(); ++i)
            tool.setFaceName(i, m_node.id + ":text" + std::to_string(i));
        nameEdgesFromFaces(tool);
        const BooleanOp op = relief ? BooleanOp::Join : BooleanOp::Cut;
        if (ctx.wantPreview) {
            ctx.preview = tool.shape();
            ctx.previewOp = op;
        }
        if (ctx.state.bodies.empty() && relief) {
            // no body yet: the letters become a body of their own (a name plate, a stencil)
            const Uuid target = applyBoolean(ctx, tool, BooleanOp::NewBody, {});
            ctx.state.featureTools[m_node.id] = FeatureTool{tool, BooleanOp::Join, target};
            return;
        }
        const Uuid target = applyBoolean(ctx, tool, op, body);
        ctx.state.featureTools[m_node.id] = FeatureTool{tool, op, target};
    }
};

}  // namespace

std::unique_ptr<ParametricFeature> makeOpText(const FeatureNode& n) { return std::make_unique<OpText>(n); }

}  // namespace silex::kernel
