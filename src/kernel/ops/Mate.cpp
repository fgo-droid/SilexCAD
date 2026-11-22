#include "kernel/ops/Mate.h"

#include <algorithm>
#include <cmath>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <TopLoc_Location.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>

#include "kernel/TopoNaming.h"

namespace silex::kernel {
namespace {

struct Frame
{
    gp_Pnt p;
    gp_Dir d;
};

/// Point + direction of a mate reference: planar face (centroid, outward normal), cylindrical face or
/// circular edge (point on the axis, axis direction).
Frame frameOf(ExecContext& ctx, const TopoRef& ref, bool axis, const Uuid* requiredOwner, std::string* owner)
{
    const double size = ctx.state.modelSize();
    if (axis) {
        if (auto r = resolveRef(ctx.state.bodies, ref, TopAbs_EDGE, size)) {
            BRepAdaptor_Curve c(TopoDS::Edge(r->shape));
            if (c.GetType() == GeomAbs_Circle) {
                if (owner)
                    *owner = r->owner;
                return {c.Circle().Location(), c.Circle().Axis().Direction()};
            }
        }
    }
    auto r = resolveRef(ctx.state.bodies, ref, TopAbs_FACE, size);
    if (!r)
        throw FeatureError("An alignment reference no longer exists");
    if (requiredOwner && r->owner != *requiredOwner)
        throw FeatureError("The first alignment reference must belong to the moved body");
    if (owner)
        *owner = r->owner;
    const TopoDS_Face f = TopoDS::Face(r->shape);
    GProp_GProps g;
    BRepGProp::SurfaceProperties(f, g);
    if (!axis) {
        auto pl = facePlacement(f);
        if (!pl)
            throw FeatureError("Face-to-face alignment needs planar faces");
        return {g.CentreOfMass(), gp_Dir(pl->normal.x, pl->normal.y, pl->normal.z)};
    }
    BRepAdaptor_Surface s(f);
    if (s.GetType() != GeomAbs_Cylinder)
        throw FeatureError("Coaxial alignment needs cylindrical faces or circular edges");
    const gp_Ax1 ax = s.Cylinder().Axis();
    // point of the axis closest to the face centroid
    const gp_Vec v(ax.Location(), g.CentreOfMass());
    const gp_Pnt p = ax.Location().Translated(gp_Vec(ax.Direction()) * v.Dot(gp_Vec(ax.Direction())));
    return {p, ax.Direction()};
}

/// Smallest rotation (around `pivot`) bringing direction a onto b.
gp_Trsf rotationBetween(const gp_Dir& a, const gp_Dir& b, const gp_Pnt& pivot)
{
    gp_Trsf t;
    const double c = std::clamp(a.Dot(b), -1.0, 1.0);
    if (c > 1 - 1e-12)
        return t;
    gp_Vec axis = gp_Vec(a).Crossed(gp_Vec(b));
    if (axis.Magnitude() < 1e-9) {  // opposite: any perpendicular axis
        axis = gp_Vec(a).Crossed(gp_Vec(gp::DX()));
        if (axis.Magnitude() < 1e-9)
            axis = gp_Vec(a).Crossed(gp_Vec(gp::DY()));
    }
    t.SetRotation(gp_Ax1(pivot, gp_Dir(axis)), std::acos(c));
    return t;
}

}  // namespace

gp_Trsf mateTransform(ExecContext& ctx, const Json& mate, const Uuid& movingBody)
{
    const std::string mode = mate.value("mode", std::string("face"));
    if (mode == "bed") {
        // v0.5 lot 4 "Lay flat on bed": the planar face looks down (-Z) and rests on Z = 0;
        // center = the body is centered on the plate (X = Y = 0)
        const Frame m = frameOf(ctx, mate.value("moving", TopoRef{}), false, &movingBody, nullptr);
        const gp_Trsf rot = rotationBetween(m.d, gp_Dir(0, 0, -1), m.p);
        gp_Pnt goal = m.p.Transformed(rot);
        gp_Vec t(0, 0, -goal.Z());
        if (mate.value("center", true)) {
            Bnd_Box bb;
            BRepBndLib::Add(ctx.state.bodies.at(movingBody).shape().Moved(TopLoc_Location(rot)), bb);
            const gp_XYZ c = (bb.CornerMin().XYZ() + bb.CornerMax().XYZ()) * 0.5;
            t += gp_Vec(-c.X(), -c.Y(), 0);
        }
        gp_Trsf mv;
        mv.SetTranslation(t);
        const gp_Trsf all = mv * rot;
        Bnd_Box bb;
        BRepBndLib::Add(ctx.state.bodies.at(movingBody).shape().Moved(TopLoc_Location(all)), bb);
        if (!bb.IsVoid() && bb.CornerMin().Z() < -0.05)
            ctx.warnings.push_back("Part of the body goes below the bed: this face is not the lowest one (supports needed)");
        return all;
    }
    if (mode != "face" && mode != "axis")
        throw FeatureError("Unknown alignment mode '" + mode + "'");
    const bool axis = mode == "axis";
    const TopoRef mv = mate.value("moving", TopoRef{}), fx = mate.value("fixed", TopoRef{});
    const double offset = mate.value("offset", 0.0);
    const bool flip = mate.value("flip", false);

    std::string fixedOwner;
    const Frame m = frameOf(ctx, mv, axis, &movingBody, nullptr);
    const Frame f = frameOf(ctx, fx, axis, nullptr, &fixedOwner);
    if (fixedOwner == movingBody)
        throw FeatureError("Align two different bodies");

    // face to face: normals opposed (flip: same direction); axis: same direction (flip: opposed)
    const gp_Dir target = (axis != flip) ? f.d : f.d.Reversed();
    const gp_Trsf rot = rotationBetween(m.d, target, m.p);
    const gp_Pnt goal = f.p.Translated(gp_Vec(f.d) * offset);
    gp_Trsf mv2;
    mv2.SetTranslation(gp_Vec(m.p.Transformed(rot), goal));
    return mv2 * rot;
}

}  // namespace silex::kernel
