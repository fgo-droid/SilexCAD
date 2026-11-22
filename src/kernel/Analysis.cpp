// v0.3 Lot 1 — measure (BRepExtrema_DistShapeShape, angles, circles), physical properties
// (BRepGProp) and interference / clearance analysis (BRepAlgoAPI_Common + BRepExtrema).

#include "kernel/Analysis.h"

#include <algorithm>
#include <cmath>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Circ.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Lin.hxx>

#include "kernel/Tessellator.h"
#include "kernel/TopoNaming.h"

namespace silex::kernel {

namespace {

Vec3 V(const gp_XYZ& p) { return {p.X(), p.Y(), p.Z()}; }

const char* surfaceName(GeomAbs_SurfaceType t)
{
    switch (t) {
        case GeomAbs_Plane: return "plane";
        case GeomAbs_Cylinder: return "cylinder";
        case GeomAbs_Cone: return "cone";
        case GeomAbs_Sphere: return "sphere";
        case GeomAbs_Torus: return "torus";
        default: return "surface";
    }
}

const char* curveName(GeomAbs_CurveType t)
{
    switch (t) {
        case GeomAbs_Line: return "line";
        case GeomAbs_Circle: return "circle";
        case GeomAbs_Ellipse: return "ellipse";
        default: return "curve";
    }
}

struct Entity
{
    TopoDS_Shape shape;
    MeasureRef::Kind kind;
};

double fold90(double cosAbs) { return std::acos(std::clamp(cosAbs, 0.0, 1.0)) * 180.0 / kPi; }

}  // namespace

MassProps massProps(const TopoDS_Shape& s)
{
    MassProps m;
    if (s.IsNull())
        return m;
    GProp_GProps vp, sp;
    BRepGProp::VolumeProperties(s, vp);
    BRepGProp::SurfaceProperties(s, sp);
    m.volume = std::abs(vp.Mass());
    m.area = sp.Mass();
    return m;
}

MeasureResult measureIn(const ModelState& st, const std::vector<MeasureRef>& refs)
{
    MeasureResult r;
    if (refs.empty() || refs.size() > 2) {
        r.error = "Select one or two entities";
        return r;
    }
    try {
        std::vector<Entity> ents;
        const double size = st.modelSize();
        for (const MeasureRef& m : refs) {
            if (m.kind == MeasureRef::Kind::Body) {
                auto it = st.bodies.find(m.body);
                if (it == st.bodies.end())
                    throw FeatureError("Unknown body: " + m.body);
                ents.push_back({it->second.shape(), m.kind});
                r.types.push_back("body");
                continue;
            }
            const bool face = m.kind == MeasureRef::Kind::Face;
            auto res = resolveRef(st.bodies, m.ref, face ? TopAbs_FACE : TopAbs_EDGE, size);
            if (!res)
                throw FeatureError(std::string("Unknown ") + (face ? "face" : "edge") + ": " + m.ref.name);
            ents.push_back({res->shape, m.kind});
            if (face) {
                const TopoDS_Face f = TopoDS::Face(res->shape);
                BRepAdaptor_Surface s(f);
                r.types.push_back(surfaceName(s.GetType()));
                if (s.GetType() == GeomAbs_Cylinder) {
                    const gp_Cylinder cyl = s.Cylinder();
                    GProp_GProps p;
                    BRepGProp::SurfaceProperties(f, p);
                    // centre = foot of the face centroid on the axis
                    const gp_Lin axis(cyl.Axis());
                    const gp_Pnt c = p.CentreOfMass();
                    const double t = gp_Vec(axis.Location(), c).Dot(gp_Vec(axis.Direction()));
                    const gp_Pnt foot = axis.Location().Translated(gp_Vec(axis.Direction()) * t);
                    r.circles.push_back({V(foot.XYZ()), V(axis.Direction().XYZ()), cyl.Radius()});
                }
            }
            else {
                const TopoDS_Edge e = TopoDS::Edge(res->shape);
                BRepAdaptor_Curve c(e);
                r.types.push_back(curveName(c.GetType()));
                if (c.GetType() == GeomAbs_Circle) {
                    const gp_Circ ci = c.Circle();
                    r.circles.push_back({V(ci.Location().XYZ()), V(ci.Axis().Direction().XYZ()), ci.Radius()});
                }
            }
        }

        if (ents.size() == 1) {
            GProp_GProps p;
            if (ents[0].kind == MeasureRef::Kind::Edge) {
                BRepGProp::LinearProperties(ents[0].shape, p);
                r.length = p.Mass();
            }
            else if (ents[0].kind == MeasureRef::Kind::Face) {
                BRepGProp::SurfaceProperties(ents[0].shape, p);
                r.area = p.Mass();
            }
            r.ok = true;
            return r;
        }

        BRepExtrema_DistShapeShape dist(ents[0].shape, ents[1].shape);
        if (!dist.IsDone() || dist.NbSolution() < 1)
            throw FeatureError("The distance could not be computed");
        r.hasDistance = true;
        r.distance = dist.Value();
        r.p1 = V(dist.PointOnShape1(1).XYZ());
        r.p2 = V(dist.PointOnShape2(1).XYZ());
        r.delta = r.p2 - r.p1;

        // angle
        auto planeNormal = [](const TopoDS_Shape& s) -> std::optional<gp_Dir> {
            if (s.ShapeType() != TopAbs_FACE)
                return std::nullopt;
            BRepAdaptor_Surface a(TopoDS::Face(s));
            if (a.GetType() != GeomAbs_Plane)
                return std::nullopt;
            return a.Plane().Axis().Direction();
        };
        auto lineDir = [](const TopoDS_Shape& s) -> std::optional<gp_Dir> {
            if (s.ShapeType() != TopAbs_EDGE)
                return std::nullopt;
            BRepAdaptor_Curve c(TopoDS::Edge(s));
            if (c.GetType() != GeomAbs_Line)
                return std::nullopt;
            return c.Line().Direction();
        };
        const auto n1 = planeNormal(ents[0].shape), n2 = planeNormal(ents[1].shape);
        const auto l1 = lineDir(ents[0].shape), l2 = lineDir(ents[1].shape);
        if (n1 && n2) {
            r.hasAngle = true;
            r.angleDeg = fold90(std::abs(n1->Dot(*n2)));  // angle between the two planes
        }
        else if (l1 && l2) {
            r.hasAngle = true;
            r.angleDeg = fold90(std::abs(l1->Dot(*l2)));
        }
        else if ((n1 && l2) || (n2 && l1)) {
            const gp_Dir n = n1 ? *n1 : *n2, l = l1 ? *l1 : *l2;
            r.hasAngle = true;
            r.angleDeg = 90.0 - fold90(std::abs(n.Dot(l)));  // line / plane
        }
        r.ok = true;
    }
    catch (const FeatureError& e) {
        r.error = e.what();
    }
    catch (const Standard_Failure& e) {
        r.error = std::string("Geometry kernel error: ") + (e.GetMessageString() ? e.GetMessageString() : "");
    }
    return r;
}

InterferenceReport interferenceIn(const ModelState& st, const std::vector<Uuid>& wanted, bool meshes)
{
    InterferenceReport rep;
    std::vector<Uuid> ids = wanted.empty() ? st.bodyOrder : wanted;
    for (const Uuid& id : ids)
        if (!st.bodies.count(id)) {
            rep.error = "Unknown body id: " + id;
            return rep;
        }
    if (ids.size() < 2) {
        rep.error = "Select at least two bodies";
        return rep;
    }
    auto nameOf = [&](const Uuid& id) {
        auto it = st.bodyNames.find(id);
        return it != st.bodyNames.end() ? it->second : id;
    };
    const TessellationParams tp = tessellationFor(st.modelSize());
    bool anyClear = false;
    try {
        for (size_t i = 0; i < ids.size(); ++i)
            for (size_t j = i + 1; j < ids.size(); ++j) {
                const TopoDS_Shape& a = st.bodies.at(ids[i]).shape();
                const TopoDS_Shape& b = st.bodies.at(ids[j]).shape();
                InterferencePair p;
                p.a = ids[i];
                p.b = ids[j];
                p.nameA = nameOf(ids[i]);
                p.nameB = nameOf(ids[j]);

                Bnd_Box ba, bb;
                BRepBndLib::Add(a, ba);
                BRepBndLib::Add(b, bb);
                if (!ba.IsOut(bb)) {
                    BRepAlgoAPI_Common common(a, b);
                    if (common.IsDone() && !common.HasErrors()) {
                        const TopoDS_Shape c = common.Shape();
                        p.overlapVolume = massProps(c).volume;
                        if (p.overlapVolume > kCollisionVolume && meshes) {
                            TessellationParams flat = tp;
                            flat.withEdges = false;
                            p.overlap = tessellate(c, flat);
                        }
                    }
                }
                BRepExtrema_DistShapeShape dist(a, b);
                if (dist.IsDone() && dist.NbSolution() > 0) {
                    p.clearance = dist.Value();
                    p.p1 = V(dist.PointOnShape1(1).XYZ());
                    p.p2 = V(dist.PointOnShape2(1).XYZ());
                }
                if (p.overlapVolume > kCollisionVolume) {
                    p.status = "collision";
                    p.clearance = 0;
                    rep.collision = true;
                }
                else {
                    p.status = p.clearance < kTightClearance - 1e-3 ? "tight" : "ok";  // 1 µm: geometric approximation
                    rep.minClearance = anyClear ? std::min(rep.minClearance, p.clearance) : p.clearance;
                    anyClear = true;
                }
                rep.pairs.push_back(std::move(p));
            }
    }
    catch (const Standard_Failure& e) {
        rep.error = std::string("Geometry kernel error: ") + (e.GetMessageString() ? e.GetMessageString() : "");
    }
    return rep;
}

}  // namespace silex::kernel
