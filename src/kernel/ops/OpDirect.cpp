// Direct modelling without a sketch (ergonomics lot):
//  * OpOffsetFace — Press / Pull: a planar face of a solid is extruded along its outward normal
//    (BRepPrimAPI_MakePrism) and fused (distance > 0) or cut (distance < 0) with its body;
//  * OpBox / OpCylinder — quick parametric primitives on an origin plane or a planar face.
// All of them are ordinary timeline steps (editable, persistent names, preview).

#include <cmath>

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>
#include <gp_Vec.hxx>

#include "kernel/TopoNaming.h"
#include "kernel/ops/Ops.h"

namespace silex::kernel {
namespace {

gp_Pnt pnt(const Vec3& v) { return gp_Pnt(v.x, v.y, v.z); }
gp_Dir dir(const Vec3& v) { return gp_Dir(v.x, v.y, v.z); }

void nameCaps(NamedShape& ns, const TopoDS_Shape& cap, const std::string& name)
{
    for (TopExp_Explorer ex(cap, TopAbs_FACE); ex.More(); ex.Next())
        if (const int i = ns.faceIndex(ex.Current()); i > 0)
            ns.setFaceName(i, name);
}

// ------------------------------------------------------------------ Press / Pull

class OpOffsetFace final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<OffsetFaceParams>();
        if (std::abs(p.distance) < 1e-7)
            throw FeatureError("The press / pull distance must not be zero");
        auto r = resolveRef(ctx.state.bodies, p.face, TopAbs_FACE, ctx.state.modelSize());
        if (!r)
            throw FeatureError("The pushed / pulled face no longer exists");
        if (!r->byName)
            ctx.warnings.push_back("The face reference was re-attached by geometry");
        const TopoDS_Face face = TopoDS::Face(r->shape);
        const auto pl = facePlacement(face);
        if (!pl)
            throw FeatureError("Press / Pull needs a planar face");
        requireEditable(ctx.state, r->owner);
        const NamedShape& body = ctx.state.bodies.at(r->owner);

        // the face with the names of its edges: the side walls are named after them
        NamedShape profile(face);
        for (int i = 1; i <= profile.edgeCount(); ++i)
            if (const int k = body.edgeIndex(profile.edge(i)); k > 0)
                profile.setEdgeName(i, body.edgeName(k));
        if (const int k = body.faceIndex(face); k > 0)
            profile.setFaceName(1, body.faceName(k));

        const gp_Vec vec = gp_Vec(dir(pl->normal)) * p.distance;
        BRepPrimAPI_MakePrism mk(face, vec, Standard_False, Standard_True);
        if (!mk.IsDone())
            throw FeatureError("The press / pull failed");
        NamedShape tool = propagateNames(mk.Shape(), {&profile}, historyOf(mk), m_node.id, "side");
        nameCaps(tool, mk.FirstShape(), m_node.id + ":start");
        nameCaps(tool, mk.LastShape(), m_node.id + ":end");
        nameEdgesFromFaces(tool);
        ctx.throwIfCancelled();

        const BooleanOp op = p.distance > 0 ? BooleanOp::Join : BooleanOp::Cut;
        if (ctx.wantPreview) {
            ctx.preview = tool.shape();
            ctx.previewOp = op;            // blue when adding material, red when removing it
            ctx.previewKeepsInput = true;  // the pushed face stays pickable (arrow) while editing
        }
        const Uuid target = applyBoolean(ctx, tool, op, r->owner);
        ctx.state.featureTools[m_node.id] = FeatureTool{tool, op, target};
    }
};

// ------------------------------------------------------------------ primitives

Vec3 facePrimitiveXDir(const Vec3& normal)
{
    const Vec3 n = normal.normalized();
    for (const Vec3 w : {Vec3{1, 0, 0}, Vec3{0, 1, 0}}) {
        const Vec3 x = w - n * n.dot(w);
        if (x.length() > 1e-6)
            return x.normalized();
    }
    return {1, 0, 0};
}

Placement resolveBase(ExecContext& ctx, const SketchPlaneRef& ref)
{
    if (ref.kind == "Plane") {
        auto it = ctx.state.planes.find(ref.planeId);
        if (it == ctx.state.planes.end())
            throw FeatureError("The construction plane no longer exists (or comes later in the timeline)");
        return offsetPlacement(it->second, ref.offset);
    }
    if (ref.kind != "Face")
        return offsetPlacement(originPlacement(ref.kind), ref.offset);
    auto r = resolveRef(ctx.state.bodies, ref.face, TopAbs_FACE, ctx.state.modelSize());
    if (!r)
        throw FeatureError("The base face no longer exists");
    if (!r->byName)
        ctx.warnings.push_back("Base plane re-attached to the closest face");
    const TopoDS_Face face = TopoDS::Face(r->shape);
    auto pl = facePlacement(face);
    if (!pl)
        throw FeatureError("The base face is not planar anymore");
    // Predictable frame on a face: origin at the face centroid, X = world X projected on the face
    // (world Y when the face is perpendicular to X). OCCT's own plane origin is arbitrary.
    GProp_GProps props;
    BRepGProp::SurfaceProperties(face, props);
    const gp_Pnt g = props.CentreOfMass();
    pl->origin = {g.X(), g.Y(), g.Z()};
    pl->xDir = facePrimitiveXDir(pl->normal);
    return offsetPlacement(*pl, ref.offset);
}

/// Deterministic face names of a primitive built in the frame (xDir, yDir, normal).
void namePrimitiveFaces(NamedShape& ns, const Placement& frame, const std::string& id)
{
    const Vec3 n = frame.normal.normalized(), x = frame.xDir.normalized(), y = frame.yDir().normalized();
    for (int i = 1; i <= ns.faceCount(); ++i) {
        const TopoDS_Face f = TopoDS::Face(ns.face(i));
        std::string name = "side";
        if (auto fp = facePlacement(f)) {
            const Vec3 fn = fp->normal.normalized();
            if (fn.dot(n) < -0.9) name = "bottom";
            else if (fn.dot(n) > 0.9) name = "top";
            else if (fn.dot(x) < -0.9) name = "xmin";
            else if (fn.dot(x) > 0.9) name = "xmax";
            else if (fn.dot(y) < -0.9) name = "ymin";
            else if (fn.dot(y) > 0.9) name = "ymax";
        }
        ns.setFaceName(i, id + ":" + name);
    }
    nameEdgesFromFaces(ns);
}

class OpPrimitive final : public ParametricFeature
{
public:
    OpPrimitive(const FeatureNode& n, bool cylinder) : ParametricFeature(n), m_cylinder(cylinder) {}

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<PrimitiveParams>();
        if (std::abs(p.height) < 1e-7)
            throw FeatureError("The height must not be zero");
        const Placement pl = resolveBase(ctx, p.plane);
        const Vec3 n = pl.normal.normalized(), x = pl.xDir.normalized();
        // base below the plane when the height is negative
        const Vec3 base = pl.toWorld({p.centerX, p.centerY}) + (p.height < 0 ? n * p.height : Vec3{});
        const double h = std::abs(p.height);

        TopoDS_Shape shape;
        if (m_cylinder) {
            if (p.diameter <= 1e-7)
                throw FeatureError("The cylinder diameter must be positive");
            BRepPrimAPI_MakeCylinder mk(gp_Ax2(pnt(base), dir(n), dir(x)), p.diameter / 2, h);
            mk.Build();
            if (!mk.IsDone())
                throw FeatureError("The cylinder could not be built");
            shape = mk.Shape();
        }
        else {
            if (p.length <= 1e-7 || p.width <= 1e-7)
                throw FeatureError("The box length and width must be positive");
            const Vec3 corner = base - x * (p.length / 2) - pl.yDir().normalized() * (p.width / 2);
            BRepPrimAPI_MakeBox mk(gp_Ax2(pnt(corner), dir(n), dir(x)), p.length, p.width, h);
            mk.Build();
            if (!mk.IsDone())
                throw FeatureError("The box could not be built");
            shape = mk.Shape();
        }
        NamedShape tool(shape);
        namePrimitiveFaces(tool, pl, m_node.id);
        ctx.throwIfCancelled();
        if (ctx.wantPreview) {
            ctx.preview = tool.shape();
            ctx.previewOp = p.op;
        }
        const Uuid body = applyBoolean(ctx, tool, p.op, p.targetBody);
        ctx.state.featureTools[m_node.id] =
            FeatureTool{tool, p.op == BooleanOp::NewBody ? BooleanOp::Join : p.op, body};
    }

private:
    bool m_cylinder;
};

}  // namespace

Placement resolveBasePlane(ExecContext& ctx, const SketchPlaneRef& ref) { return resolveBase(ctx, ref); }

std::unique_ptr<ParametricFeature> makeOpOffsetFace(const FeatureNode& n) { return std::make_unique<OpOffsetFace>(n); }
std::unique_ptr<ParametricFeature> makeOpBox(const FeatureNode& n) { return std::make_unique<OpPrimitive>(n, false); }
std::unique_ptr<ParametricFeature> makeOpCylinder(const FeatureNode& n) { return std::make_unique<OpPrimitive>(n, true); }

}  // namespace silex::kernel
