// v0.4 lot 1 — mechanical productivity:
//  * OpConstructionPlane — offset / angled / mid planes (timeline + "Construction" folder);
//  * OpRib               — reinforcing rib from an open sketch line, filling the space up to the
//                          neighbouring walls of the body.

#include <cmath>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Trsf.hxx>

#include "kernel/GeomRefs.h"
#include "kernel/TopoNaming.h"
#include "kernel/ops/Ops.h"

namespace silex::kernel {
namespace {

Vec3 toV(const gp_XYZ& p) { return {p.X(), p.Y(), p.Z()}; }
gp_Pnt toP(const Vec3& v) { return gp_Pnt(v.x, v.y, v.z); }

// ------------------------------------------------------------------ construction planes

class OpConstructionPlane final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<ConstructionPlaneParams>();
        Placement pl;
        if (p.mode == "offset") {
            pl = resolveBasePlane(ctx, p.base);
        }
        else if (p.mode == "angle") {
            pl = resolveBasePlane(ctx, p.base);
            const gp_Ax1 ax = resolveAxis(ctx, p.axis);
            gp_Trsf r;
            r.SetRotation(ax, p.angle * M_PI / 180.0);
            const gp_Pnt o = toP(pl.origin).Transformed(r);
            const gp_Dir n = gp_Dir(pl.normal.x, pl.normal.y, pl.normal.z).Transformed(r);
            const gp_Dir x = gp_Dir(pl.xDir.x, pl.xDir.y, pl.xDir.z).Transformed(r);
            pl.origin = toV(o.XYZ());
            pl.normal = toV(n.XYZ());
            pl.xDir = toV(x.XYZ());
        }
        else if (p.mode == "mid") {
            const double size = ctx.state.modelSize();
            auto planeOf = [&](const TopoRef& ref, gp_Pnt* c) -> Placement {
                auto r = resolveRef(ctx.state.bodies, ref, TopAbs_FACE, size);
                if (!r)
                    throw FeatureError("A face of the mid plane no longer exists");
                auto fp = facePlacement(TopoDS::Face(r->shape));
                if (!fp)
                    throw FeatureError("The mid plane needs two planar faces");
                GProp_GProps g;
                BRepGProp::SurfaceProperties(r->shape, g);
                *c = g.CentreOfMass();
                return *fp;
            };
            gp_Pnt c1, c2;
            const Placement a = planeOf(p.face1, &c1), b = planeOf(p.face2, &c2);
            if (std::abs(a.normal.normalized().dot(b.normal.normalized())) < 1 - 1e-6)
                throw FeatureError("The two faces of a mid plane must be parallel");
            pl = a;
            pl.origin = (toV(c1.XYZ()) + toV(c2.XYZ())) * 0.5;
        }
        else
            throw FeatureError("Unknown construction plane mode '" + p.mode + "'");
        ctx.state.planes[m_node.id] = pl;
    }
};

// ------------------------------------------------------------------ rib

class OpRib final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<RibParams>();
        if (p.thickness <= 1e-6)
            throw FeatureError("The rib thickness must be positive");
        auto sit = ctx.state.sketches.find(p.sketch);
        if (sit == ctx.state.sketches.end())
            throw FeatureError("The rib sketch no longer exists");
        const SketchOutput& so = sit->second;
        const SketchGeo* line = nullptr;
        for (const SketchGeo& g : so.sketch.geos)
            if (g.kind == GeoKind::Line && !g.construction && (p.curve.empty() || g.id == p.curve)) {
                line = &g;
                break;
            }
        if (!line)
            throw FeatureError("The rib needs an open line in its sketch");
        Uuid bodyId = p.body;
        if (bodyId.empty())
            bodyId = ctx.state.lastEditableBody();
        requireEditable(ctx.state, bodyId);
        auto bit = ctx.state.bodies.find(bodyId);
        if (bit == ctx.state.bodies.end())
            throw FeatureError("There is no body to reinforce");
        NamedShape& body = bit->second;

        const Placement& pl = so.plane;
        const Vec3 A = pl.toWorld(line->p1), B = pl.toWorld(line->p2);
        const Vec3 dir = (B - A).normalized();
        const Vec3 n = pl.normal.normalized();
        const Vec3 m = n.cross(dir).normalized();  // in the sketch plane, perpendicular to the line
        Bnd_Box bb;
        BRepBndLib::Add(body.shape(), bb);
        const double L = std::sqrt(bb.SquareExtent()) * 2 + 10;
        const Vec3 A2 = A - dir * L, B2 = B + dir * L;  // the line is extended through the walls

        // try both sides of the line: the rib is the pocket closed by the walls of the body
        for (const double side : {1.0, -1.0}) {
            BRepBuilderAPI_MakePolygon poly(toP(A2), toP(B2), toP(B2 + m * (side * L)), toP(A2 + m * (side * L)), true);
            const TopoDS_Face strip = BRepBuilderAPI_MakeFace(poly.Wire(), true).Face();
            gp_Trsf half;
            half.SetTranslation(gp_Vec(n.x, n.y, n.z) * (-p.thickness / 2));
            const TopoDS_Shape stripMoved = strip.Moved(TopLoc_Location(half));
            BRepPrimAPI_MakePrism slab(stripMoved, gp_Vec(n.x, n.y, n.z) * p.thickness);
            BRepAlgoAPI_Cut cut(slab.Shape(), body.shape());
            if (!cut.IsDone() || cut.HasErrors())
                continue;
            const Vec3 probe = (A + B) * 0.5 + m * (side * std::max(p.thickness * 0.05, 1e-3));
            for (TopExp_Explorer ex(cut.Shape(), TopAbs_SOLID); ex.More(); ex.Next()) {
                BRepClass3d_SolidClassifier cls(ex.Current(), toP(probe), 1e-6);
                if (cls.State() != TopAbs_IN)
                    continue;
                // closed by the walls? the piece must not reach the far edges of the strip
                Bnd_Box pb;
                BRepBndLib::Add(ex.Current(), pb);
                const double reach = std::sqrt(pb.SquareExtent());
                if (reach > L * 0.9)
                    break;  // open on this side
                NamedShape tool(ex.Current());
                for (int i = 1; i <= tool.faceCount(); ++i)
                    tool.setFaceName(i, m_node.id + ":rib" + std::to_string(i));
                nameEdgesFromFaces(tool);
                if (ctx.wantPreview) {
                    ctx.preview = tool.shape();
                    ctx.previewOp = BooleanOp::Join;
                }
                applyBoolean(ctx, tool, BooleanOp::Join, bodyId);
                ctx.state.featureTools[m_node.id] = FeatureTool{tool, BooleanOp::Join, bodyId};
                return;
            }
        }
        throw FeatureError("No closed space between the line and the walls of the body: draw the rib line "
                           "from one wall to the other (it may stop short, it is extended)");
    }
};

}  // namespace

std::unique_ptr<ParametricFeature> makeOpConstructionPlane(const FeatureNode& n) { return std::make_unique<OpConstructionPlane>(n); }
std::unique_ptr<ParametricFeature> makeOpRib(const FeatureNode& n) { return std::make_unique<OpRib>(n); }

}  // namespace silex::kernel
