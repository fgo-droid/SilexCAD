// v0.5 lot 1 — standard fastener holes (Hole / Insert tool).
//
// Each hole is a single tool solid cut from the body, drilled along the inward normal of the base
// face (or plane):
//   clearance / counterbore / countersink / insert: revolution of a (radius, depth) profile, which gives
//     one clean solid per hole (cylinder, counterbore step, 90 degree cone, insert entry chamfer);
//   nut: hexagonal prism (width across flats + FDM clearance) fused with the clearance hole.
// The tool starts a little outside the face (no coplanar boolean) and "through" goes as deep as the
// model. The tool is registered in featureTools, so mirror / patterns can repeat the holes.

#include <cmath>
#include <vector>

#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <gp_Ax1.hxx>

#include "kernel/TopoNaming.h"
#include "kernel/ops/Ops.h"

namespace silex::kernel {
namespace {

gp_Pnt pnt(const Vec3& v) { return gp_Pnt(v.x, v.y, v.z); }

class OpFastenerHole final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<FastenerHoleParams>();
        const auto dims = fastenerHoleDims(p);
        if (!dims)
            throw FeatureError("Unknown hole size or type ('" + p.size + "', '" + p.kind + "')");
        if (p.centers.empty())
            throw FeatureError("The hole has no position");

        Uuid body = p.targetBody;
        if (p.plane.kind == "Face") {
            if (auto r = resolveRef(ctx.state.bodies, p.plane.face, TopAbs_FACE, ctx.state.modelSize()))
                body = r->owner;
        }
        const Placement pl = resolveBasePlane(ctx, p.plane);
        const Vec3 n = pl.normal.normalized(), x = pl.xDir.normalized(), y = pl.yDir().normalized();
        const double over = 0.2;  // start outside the face: no coplanar boolean
        double through = std::max(10.0, 2.5 * ctx.state.modelSize());
        {
            // "through all": just through the target body (a short tool also reads better in the preview)
            const Uuid tb = body.empty() ? ctx.state.lastEditableBody() : body;
            if (auto it = ctx.state.bodies.find(tb); it != ctx.state.bodies.end()) {
                Bnd_Box bb;
                BRepBndLib::Add(it->second.shape(), bb);
                if (!bb.IsVoid()) {
                    double lo = 1e300;
                    for (int k = 0; k < 8; ++k) {
                        const gp_Pnt c((k & 1) ? bb.CornerMax().X() : bb.CornerMin().X(),
                                       (k & 2) ? bb.CornerMax().Y() : bb.CornerMin().Y(),
                                       (k & 4) ? bb.CornerMax().Z() : bb.CornerMin().Z());
                        lo = std::min(lo, (Vec3{c.X(), c.Y(), c.Z()} - pl.origin).dot(n));
                    }
                    through = std::max(1.0, -lo + 1.0);
                }
            }
        }
        double depth = p.depth > 0 ? p.depth : through;
        if (p.kind == "insert")
            depth = dims->headDepth;
        if (depth <= dims->headDepth - 1e-9 && p.kind != "insert" && p.kind != "clearance")
            throw FeatureError("The hole is shallower than its head (counterbore / countersink / nut)");

        // (radius, depth below the face) profile of the revolved holes, from the axis outwards
        auto revolved = [&](const Vec3& c) {
            std::vector<std::pair<double, double>> rz;  // depth grows into the material
            const double r = dims->holeD / 2;
            if (p.kind == "clearance")
                rz = {{0, -over}, {r, -over}, {r, depth}, {0, depth}};
            else if (p.kind == "counterbore") {
                const double R = dims->headD / 2, h = dims->headDepth;
                rz = {{0, -over}, {R, -over}, {R, h}, {r, h}, {r, depth}, {0, depth}};
            }
            else if (p.kind == "countersink") {
                const double R = dims->headD / 2, h = dims->headDepth;
                rz = {{0, -over}, {R + over, -over}, {R, 0}, {r, h}, {r, depth}, {0, depth}};
            }
            else {  // insert: pocket with an entry chamfer
                const double ch = dims->chamfer;
                rz = {{0, -over}, {r + ch + over, -over}, {r + ch, 0}, {r, ch}, {r, depth}, {0, depth}};
            }
            BRepBuilderAPI_MakePolygon poly;
            for (const auto& [rho, z] : rz)
                poly.Add(pnt(c + x * rho - n * z));
            poly.Close();
            BRepBuilderAPI_MakeFace face(poly.Wire(), true);
            if (!face.IsDone())
                throw FeatureError("The hole profile could not be built");
            BRepPrimAPI_MakeRevol rev(face.Face(), gp_Ax1(pnt(c), gp_Dir(n.x, n.y, n.z)), 2 * M_PI);
            rev.Build();
            if (!rev.IsDone())
                throw FeatureError("The hole could not be built");
            return rev.Shape();
        };
        auto nutTrap = [&](const Vec3& c) {
            const double R = dims->headD / 2, a0 = p.angle * M_PI / 180.0;
            BRepBuilderAPI_MakePolygon hex;
            for (int i = 0; i < 6; ++i) {
                const double a = a0 + i * M_PI / 3;
                hex.Add(pnt(c + x * (R * std::cos(a)) + y * (R * std::sin(a)) + n * over));
            }
            hex.Close();
            BRepBuilderAPI_MakeFace face(hex.Wire(), true);
            const Vec3 h = n * -(dims->headDepth + over);
            BRepPrimAPI_MakePrism prism(face.Face(), gp_Vec(h.x, h.y, h.z));
            const double r = dims->holeD / 2;
            BRepBuilderAPI_MakePolygon poly;
            for (const auto& [rho, z] : std::vector<std::pair<double, double>>{
                     {0, dims->headDepth - over}, {r, dims->headDepth - over}, {r, depth}, {0, depth}})
                poly.Add(pnt(c + x * rho - n * z));
            poly.Close();
            BRepPrimAPI_MakeRevol shaft(BRepBuilderAPI_MakeFace(poly.Wire(), true).Face(),
                                        gp_Ax1(pnt(c), gp_Dir(n.x, n.y, n.z)), 2 * M_PI);
            BRepAlgoAPI_Fuse fuse(prism.Shape(), shaft.Shape());
            if (!fuse.IsDone() || fuse.HasErrors())
                throw FeatureError("The nut trap could not be built");
            return fuse.Shape();
        };

        TopoDS_Compound all;
        BRep_Builder bb;
        bb.MakeCompound(all);
        for (const Vec2& c2 : p.centers) {
            const Vec3 c = pl.toWorld(c2);
            bb.Add(all, p.kind == "nut" ? nutTrap(c) : revolved(c));
        }
        ctx.throwIfCancelled();
        TopoDS_Shape toolShape = all;
        if (p.centers.size() == 1) {
            TopoDS_Iterator it(all);
            toolShape = it.Value();
        }
        NamedShape tool(toolShape);
        for (int i = 1; i <= tool.faceCount(); ++i)
            tool.setFaceName(i, m_node.id + ":hole" + std::to_string(i));
        nameEdgesFromFaces(tool);
        if (ctx.wantPreview) {
            ctx.preview = tool.shape();
            ctx.previewOp = BooleanOp::Cut;
        }
        const Uuid target = applyBoolean(ctx, tool, BooleanOp::Cut, body);
        ctx.state.featureTools[m_node.id] = FeatureTool{tool, BooleanOp::Cut, target};
    }
};

}  // namespace

std::unique_ptr<ParametricFeature> makeOpFastenerHole(const FeatureNode& n) { return std::make_unique<OpFastenerHole>(n); }

}  // namespace silex::kernel
