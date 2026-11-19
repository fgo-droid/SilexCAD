// OpRevolve: revolution of sketch profiles around a sketch axis or a sketch line.

#include <algorithm>
#include <cmath>

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>

#include "kernel/TopoNaming.h"
#include "kernel/ops/Ops.h"

namespace silex::kernel {
namespace {

class OpRevolve final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<RevolveParams>();
        if (std::abs(p.angle) < 1e-6)
            throw FeatureError("The revolution angle must not be zero");
        const auto regions = selectRegions(ctx, p.profile);
        const SketchOutput& so = ctx.state.sketches.at(p.profile.sketch);
        const gp_Ax1 axis = resolveAxis(so, p.axis);

        const bool full = std::abs(std::abs(p.angle) - 360.0) < 1e-9;
        const double angle = std::clamp(p.angle, -360.0, 360.0) * kPi / 180.0;

        std::vector<NamedShape> tools;
        for (const SketchRegion* r : regions) {
            BRepPrimAPI_MakeRevol mk(r->named.shape(), axis, angle, Standard_False);
            if (!mk.IsDone())
                throw FeatureError("The revolution failed (does the profile cross the axis?)");
            NamedShape tool = propagateNames(mk.Shape(), {&r->named}, historyOf(mk), m_node.id, "side");
            if (!full) {
                nameCaps(tool, mk.FirstShape(), m_node.id + ":start");
                nameCaps(tool, mk.LastShape(), m_node.id + ":end");
            }
            nameUnreportedSides(tool, r->named);
            nameEdgesFromFaces(tool);
            checkSolid(tool.shape(), "The revolution");
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

private:
    /// OCCT's MakeRevol history omits the planar faces swept by profile edges perpendicular
    /// to the axis (they would otherwise fall back to "<feat>:new<n>"). Such a face contains
    /// its source edge, so it gets the same semantic name as the other sides.
    void nameUnreportedSides(NamedShape& tool, const NamedShape& profile) const
    {
        const std::string fallback = m_node.id + ":new";
        for (int fi = 1; fi <= tool.faceCount(); ++fi) {
            if (tool.faceName(fi).rfind(fallback, 0) != 0)
                continue;
            const TopoDS_Face face = TopoDS::Face(tool.face(fi));
            for (int ei = 1; ei <= profile.edgeCount(); ++ei) {
                const std::string& en = profile.edgeName(ei);
                if (en.empty())
                    continue;
                BRepAdaptor_Curve c(TopoDS::Edge(profile.edge(ei)));
                const gp_Pnt mid = c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
                BRepExtrema_DistShapeShape dist(BRepBuilderAPI_MakeVertex(mid).Vertex(), face);
                if (dist.IsDone() && dist.Value() < 1e-6) {
                    tool.setFaceName(fi, m_node.id + ":side(" + en + ")");
                    break;
                }
            }
        }
    }

    static void nameCaps(NamedShape& ns, const TopoDS_Shape& cap, const std::string& name)
    {
        for (TopExp_Explorer ex(cap, TopAbs_FACE); ex.More(); ex.Next())
            if (const int i = ns.faceIndex(ex.Current()); i > 0)
                ns.setFaceName(i, name);
    }

    static gp_Ax1 resolveAxis(const SketchOutput& so, const std::string& axis)
    {
        const Placement& pl = so.plane;
        auto P = [](const Vec3& v) { return gp_Pnt(v.x, v.y, v.z); };
        auto D = [](const Vec3& v) { return gp_Dir(v.x, v.y, v.z); };
        if (axis == "X")
            return gp_Ax1(P(pl.origin), D(pl.xDir));
        if (axis == "Y")
            return gp_Ax1(P(pl.origin), D(pl.yDir()));
        const SketchGeo* g = so.sketch.find(axis);
        if (!g || g->kind != GeoKind::Line || (g->p2 - g->p1).length() < 1e-9)
            throw FeatureError("The revolution axis is not a valid sketch line");
        const Vec3 a = pl.toWorld(g->p1), b = pl.toWorld(g->p2);
        return gp_Ax1(P(a), D((b - a).normalized()));
    }
};

}  // namespace

std::unique_ptr<ParametricFeature> makeOpRevolve(const FeatureNode& n) { return std::make_unique<OpRevolve>(n); }

}  // namespace silex::kernel
