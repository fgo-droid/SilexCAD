// v0.4 lot 2 — ISO metric threads modelled for 3D printing.
//
// The exact cross-section of the thread (60° flanks, crests truncated by H/8) is swept along the axis
// while turning with an auxiliary helix (BRepOffsetAPI_MakePipeShell): its lateral faces are
// helicoids. A screw is cut by (cylinder - threaded rod), a nut by the tapped hole solid; the only
// intersections the boolean needs are with planes (helicoid / coaxial cylinder intersections are not
// reliable in OCCT):
//   external (screw): crest D/2 - c - H/8, root D/2 - H - c;
//   internal (nut):   crest D/2 - H + c + H/8, root D/2 + c.
// With the same clearance c on both parts the flank-to-flank play is exactly c. The helix phase is
// measured from the projection of the world origin on the axis, X = world X projected: two coaxial
// threads of the same pitch always mesh, whatever the way they were modelled.

#include <cmath>
#include <vector>

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <cstdio>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <GProp_GProps.hxx>
#include <Geom2d_Line.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Ax3.hxx>

#include "kernel/TopoNaming.h"
#include "kernel/ops/Ops.h"

namespace silex::kernel {
namespace {

class OpThread final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<ThreadParams>();
        const auto spec = metricThread(p.size);
        if (!spec)
            throw FeatureError("Unknown thread size '" + p.size + "' (M3 to M12)");
        const double P = p.pitch > 0 ? p.pitch : (p.fine ? spec->finePitch : spec->pitch);
        const double D = spec->diameter, H = P * std::sqrt(3.0) / 2, c = std::max(0.0, p.clearance);

        auto r = resolveRef(ctx.state.bodies, p.face, TopAbs_FACE, ctx.state.modelSize());
        if (!r)
            throw FeatureError("The threaded face no longer exists");
        if (!r->byName)
            ctx.warnings.push_back("The face reference was re-attached by geometry");
        const TopoDS_Face face = TopoDS::Face(r->shape);
        BRepAdaptor_Surface s(face);
        if (s.GetType() != GeomAbs_Cylinder)
            throw FeatureError("A thread needs a cylindrical face (shaft or hole)");
        const gp_Cylinder cyl = s.Cylinder();
        const gp_Dir d = cyl.Axis().Direction();
        const double rf = cyl.Radius();

        // external (screw) or internal (hole): outward normal vs radial direction
        const double um = (s.FirstUParameter() + s.LastUParameter()) / 2, vm = (s.FirstVParameter() + s.LastVParameter()) / 2;
        gp_Pnt pm;
        gp_Vec du, dv;
        s.D1(um, vm, pm, du, dv);
        gp_Vec n = du.Crossed(dv);
        if (face.Orientation() == TopAbs_REVERSED)
            n.Reverse();
        const gp_Vec toP(cyl.Location(), pm);
        const gp_Vec radial = toP - gp_Vec(d) * toP.Dot(gp_Vec(d));
        const bool external = n.Dot(radial) > 0;

        // axial extent of the face, measured from the world origin projected on the axis
        const gp_Pnt O0 = cyl.Location().Translated(gp_Vec(d) * gp_Vec(cyl.Location(), gp::Origin()).Dot(gp_Vec(d)));
        const double s0 = gp_Vec(O0, cyl.Location()).Dot(gp_Vec(d));
        double Z0 = s0 + std::min(s.FirstVParameter(), s.LastVParameter());
        double Z1 = s0 + std::max(s.FirstVParameter(), s.LastVParameter());
        if (p.length > 0 && p.length < Z1 - Z0)
            Z0 = Z1 - p.length;
        gp_Vec xr = gp_Vec(gp::DX()) - gp_Vec(d) * d.X();
        if (xr.Magnitude() < 1e-6)
            xr = gp_Vec(gp::DY()) - gp_Vec(d) * d.Y();
        const gp_Dir xRef(xr);

        if (external && std::abs(rf - D / 2) > std::max(0.6, 0.15 * D))
            ctx.warnings.push_back("The shaft diameter is far from the nominal " + p.size + " diameter");
        if (!external && std::abs(rf - (D / 2 - H)) > std::max(0.6, 0.15 * D))
            ctx.warnings.push_back("The hole diameter is far from the " + p.size + " tap drill");

        if (!p.modeled) {  // cosmetic: annotation only
            ThreadView tv;
            tv.id = m_node.id;
            char buf[32];
            std::snprintf(buf, sizeof buf, "%sx%g", p.size.c_str(), P);
            tv.label = buf;
            const gp_Pnt a = O0.Translated(gp_Vec(d) * Z0), b = O0.Translated(gp_Vec(d) * Z1);
            tv.a = {a.X(), a.Y(), a.Z()};
            tv.b = {b.X(), b.Y(), b.Z()};
            tv.radius = rf;
            ctx.state.cosmeticThreads.push_back(tv);
            return;
        }

        const Uuid body = r->owner;
        const double sq3 = std::sqrt(3.0);
        double Rc = D / 2 - c - H / 8;             // screw crest (flat P/8)
        const double Ri = D / 2 - H + c + H / 8;   // nut crest (flat P/8)
        const double Ra = D / 2 + c;               // nut root (sharp)
        if (external && rf < Rc + 0.01)
            Rc = rf - 0.02;  // thin shaft: the crest stays just inside it
        const double root = Rc - 7 * H / 8;        // screw root (sharp)
        if (root <= 0.1 || Ri <= 0.1)
            throw FeatureError("The thread is too fine for this diameter");

        auto axialCylinder = [&](double radius, double z0, double z1) {
            return BRepPrimAPI_MakeCylinder(gp_Ax2(O0.Translated(gp_Vec(d) * z0), d, xRef), radius, z1 - z0).Shape();
        };
        auto at = [&](double rho, double z) { return O0.Translated(gp_Vec(d) * z + gp_Vec(xRef) * rho); };
        // profile (rho, z) points swept along a right-hand helix (binormal = axis)
        auto helical = [&](const std::vector<std::pair<double, double>>& pts, double zc) {
            BRepBuilderAPI_MakePolygon prof;
            for (const auto& [rho, z] : pts)
                prof.Add(at(rho, z));
            prof.Close();
            const double Zs = zc - P;
            const int turns = int(std::ceil((Z1 - Zs) / P)) + 2;
            Handle(Geom_CylindricalSurface) surf = new Geom_CylindricalSurface(gp_Ax3(O0.Translated(gp_Vec(d) * zc), d, xRef), D);
            Handle(Geom2d_Line) line = new Geom2d_Line(gp_Pnt2d(0, 0), gp_Dir2d(2 * M_PI, P));
            TopoDS_Edge helix = BRepBuilderAPI_MakeEdge(line, surf, 0, std::sqrt(4 * M_PI * M_PI + P * P) * turns).Edge();
            BRepLib::BuildCurves3d(helix, 1e-6, GeomAbs_C1, 14, 400);
            BRepOffsetAPI_MakePipeShell pipe(BRepBuilderAPI_MakeWire(helix).Wire());
            pipe.SetMode(d);  // binormal = axis: the profile stays in axial planes
            pipe.Add(prof.Wire(), false, false);
            pipe.Build(ctx.progress());
            ctx.throwIfCancelled();
            if (!pipe.IsDone() || !pipe.MakeSolid())
                throw FeatureError("The helical sweep of the thread failed");
            TopoDS_Shape sh = pipe.Shape();
            for (TopExp_Explorer ex(sh, TopAbs_SOLID); ex.More(); ex.Next()) {
                TopoDS_Solid so = TopoDS::Solid(ex.Current());
                BRepLib::OrientClosedSolid(so);
                return TopoDS_Shape(so);
            }
            return sh;
        };
        const double Zs = P * std::floor(Z0 / P) - P;  // screw crests at k P (world phase)

        TopoDS_Shape cutter;
        if (external) {
            // ring around the screw = tube (bore = root) minus the swept thread ridges
            const double hw0 = P / 2 - 0.01;                     // ridges of adjacent turns never touch
            const double rho1 = Rc - (hw0 - P / 16) * sq3;       // flank radius at that half width
            const double base = root - 0.05;
            const TopoDS_Shape ridge = helical({{base, Zs - hw0}, {rho1, Zs - hw0}, {Rc, Zs - P / 16},
                                                {Rc, Zs + P / 16}, {rho1, Zs + hw0}, {base, Zs + hw0}}, Zs);
            BRepAlgoAPI_Cut tube(axialCylinder(rf + 1, Z0, Z1), axialCylinder(root, Z0 - 1, Z1 + 1));
            BRepAlgoAPI_Cut ring(tube.Shape(), ridge);
            if (!ring.IsDone() || ring.HasErrors())
                throw FeatureError("The thread could not be built");
            cutter = ring.Shape();
        }
        else {
            // open the hole to the nut crest, then cut the V groove (60° flanks past the crest)
            if (rf < Ri - 1e-6)
                cutTool(ctx, axialCylinder(Ri, Z0, Z1), body, "bore");
            const double crest = std::max(Ri, rf);
            const double eps = 0.05, over = crest - 0.3;
            const double w = 2 * (Ra - (crest - eps)) / sq3;  // groove width at the shoulder
            const TopoDS_Shape groove = helical({{Ra, Zs}, {crest - eps, Zs - w / 2}, {over, Zs - w / 2},
                                                 {over, Zs + w / 2}, {crest - eps, Zs + w / 2}}, Zs);
            BRepAlgoAPI_Common lim(groove, axialCylinder(Ra + 1, Z0, Z1));
            if (!lim.IsDone() || lim.HasErrors())
                throw FeatureError("The thread could not be built");
            cutter = lim.Shape();
        }
        GProp_GProps g;
        BRepGProp::VolumeProperties(cutter, g);
        if (g.Mass() < 1e-6)
            throw FeatureError("The thread could not be built (empty cutter)");
        cutTool(ctx, cutter, body, "thread");
    }

private:
    void cutTool(ExecContext& ctx, const TopoDS_Shape& shape, const Uuid& body, const std::string& kind) const
    {
        NamedShape tool(shape);
        for (int i = 1; i <= tool.faceCount(); ++i)
            tool.setFaceName(i, m_node.id + ":" + kind + std::to_string(i));
        nameEdgesFromFaces(tool);
        if (ctx.wantPreview) {
            ctx.preview = shape;
            ctx.previewOp = BooleanOp::Cut;
        }
        applyBoolean(ctx, tool, BooleanOp::Cut, body);
    }
};

}  // namespace

std::unique_ptr<ParametricFeature> makeOpThread(const FeatureNode& n) { return std::make_unique<OpThread>(n); }

}  // namespace silex::kernel
