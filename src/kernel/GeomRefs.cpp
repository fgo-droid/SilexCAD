#include "kernel/GeomRefs.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Circ.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>

#include "kernel/TopoNaming.h"

namespace silex::kernel {

namespace {

gp_Dir worldAxis(const std::string& k)
{
    if (k == "X") return gp::DX();
    if (k == "Y") return gp::DY();
    if (k == "Z") return gp::DZ();
    throw FeatureError("Unknown axis '" + k + "'");
}

TopoDS_Shape resolveEntity(ExecContext& ctx, const GeomRef& ref)
{
    const TopAbs_ShapeEnum type = ref.kind == "Edge" ? TopAbs_EDGE : TopAbs_FACE;
    auto r = resolveRef(ctx.state.bodies, ref.ref, type, ctx.state.modelSize());
    if (!r)
        throw FeatureError(std::string("The reference ") + (type == TopAbs_EDGE ? "edge" : "face") +
                           " no longer exists");
    if (!r->byName)
        ctx.warnings.push_back("A reference was re-attached by geometry");
    return r->shape;
}

gp_Ax1 edgeAxis(const TopoDS_Edge& e)
{
    BRepAdaptor_Curve c(e);
    switch (c.GetType()) {
        case GeomAbs_Line: {
            gp_Ax1 ax = c.Line().Position();
            if (e.Orientation() == TopAbs_REVERSED)
                ax.Reverse();
            return ax;
        }
        case GeomAbs_Circle: return c.Circle().Axis();
        default: throw FeatureError("The reference edge must be straight or circular");
    }
}

gp_Ax1 faceAxis(const TopoDS_Face& f)
{
    BRepAdaptor_Surface s(f);
    switch (s.GetType()) {
        case GeomAbs_Plane: {
            GProp_GProps props;
            BRepGProp::SurfaceProperties(f, props);
            auto pl = facePlacement(f);
            return gp_Ax1(props.CentreOfMass(), gp_Dir(pl->normal.x, pl->normal.y, pl->normal.z));
        }
        case GeomAbs_Cylinder: return s.Cylinder().Axis();
        case GeomAbs_Cone: return s.Cone().Axis();
        default: throw FeatureError("The reference face must be planar, cylindrical or conical");
    }
}

}  // namespace

gp_Ax1 resolveAxis(ExecContext& ctx, const GeomRef& ref)
{
    if (ref.kind == "Edge")
        return edgeAxis(TopoDS::Edge(resolveEntity(ctx, ref)));
    if (ref.kind == "Face")
        return faceAxis(TopoDS::Face(resolveEntity(ctx, ref)));
    return gp_Ax1(gp::Origin(), worldAxis(ref.kind));
}

gp_Dir resolveDirection(ExecContext& ctx, const GeomRef& ref)
{
    if (ref.kind == "Edge") {
        const gp_Ax1 ax = edgeAxis(TopoDS::Edge(resolveEntity(ctx, ref)));
        BRepAdaptor_Curve c(TopoDS::Edge(resolveEntity(ctx, ref)));
        if (c.GetType() != GeomAbs_Line)
            throw FeatureError("A pattern direction edge must be straight");
        return ax.Direction();
    }
    if (ref.kind == "Face") {
        const TopoDS_Face f = TopoDS::Face(resolveEntity(ctx, ref));
        auto pl = facePlacement(f);
        if (!pl)
            throw FeatureError("A pattern direction face must be planar (its normal is used)");
        return gp_Dir(pl->normal.x, pl->normal.y, pl->normal.z);
    }
    return worldAxis(ref.kind);
}

gp_Ax2 resolvePlane(ExecContext& ctx, const GeomRef& ref)
{
    if (ref.kind == "XY") return gp_Ax2(gp::Origin(), gp::DZ(), gp::DX());
    if (ref.kind == "XZ") return gp_Ax2(gp::Origin(), gp::DY(), gp::DX());
    if (ref.kind == "YZ") return gp_Ax2(gp::Origin(), gp::DX(), gp::DY());
    if (ref.kind == "Face") {
        const TopoDS_Face f = TopoDS::Face(resolveEntity(ctx, ref));
        BRepAdaptor_Surface s(f);
        if (s.GetType() != GeomAbs_Plane)
            throw FeatureError("The mirror plane must be a planar face");
        return s.Plane().Position().Ax2();
    }
    throw FeatureError("Unknown plane '" + ref.kind + "'");
}

NamedShape transformedCopy(const NamedShape& src, const gp_Trsf& trsf,
                           const std::function<std::string(const std::string&)>& rename)
{
    BRepBuilderAPI_Transform mk(src.shape(), trsf, Standard_True);  // copy: keeps a valid B-Rep
    if (!mk.IsDone())
        throw FeatureError("Could not transform the geometry");
    NamedShape out(mk.Shape());
    for (int i = 1; i <= src.faceCount(); ++i) {
        const auto& names = src.faceAliases(i);
        if (names.empty())
            continue;
        const TopTools_ListOfShape mods = mk.Modified(src.face(i));
        for (TopTools_ListIteratorOfListOfShape it(mods); it.More(); it.Next())
            if (const int j = out.faceIndex(it.Value()); j > 0)
                for (const auto& n : names)
                    out.addFaceName(j, rename(n));
    }
    nameEdgesFromFaces(out);
    return out;
}

void replicateTargets(ExecContext& ctx, const TransformTargets& targets, const std::vector<gp_Trsf>& trsfs,
                      const std::vector<std::string>& tags)
{
    if (targets.ids.empty())
        throw FeatureError("Select at least one body or feature to copy");
    if (trsfs.empty())
        throw FeatureError("The pattern produces no copy (count must be at least 2)");

    BRep_Builder bb;
    TopoDS_Compound preview;
    bb.MakeCompound(preview);
    BooleanOp lastOp = BooleanOp::Join;

    for (const Uuid& id : targets.ids) {
        const NamedShape* src = nullptr;
        BooleanOp op = BooleanOp::Join;
        Uuid body;
        if (targets.type == "feature") {
            auto it = ctx.state.featureTools.find(id);
            if (it == ctx.state.featureTools.end())
                throw FeatureError("The copied feature is missing, failed, or is not an extrusion / revolution / sweep / loft");
            src = &it->second.tool;
            op = it->second.op;
            body = ctx.state.bodies.count(it->second.body) ? it->second.body : Uuid{};
        }
        else {
            auto it = ctx.state.bodies.find(id);
            if (it == ctx.state.bodies.end())
                throw FeatureError("A copied body no longer exists");
            src = &it->second;
            body = id;
        }
        std::vector<NamedShape> copies;
        for (size_t k = 0; k < trsfs.size(); ++k) {
            const std::string tag = tags[k];
            copies.push_back(transformedCopy(*src, trsfs[k], [&](const std::string& n) { return tag + n + ")"; }));
            bb.Add(preview, copies.back().shape());
            ctx.throwIfCancelled();
        }
        const NamedShape tool = fuseAll(ctx, copies);
        applyBoolean(ctx, tool, op, body);
        lastOp = op;
    }
    if (ctx.wantPreview) {
        ctx.preview = preview;
        ctx.previewOp = lastOp;  // red for subtractive features, blue otherwise
    }
}

}  // namespace silex::kernel
