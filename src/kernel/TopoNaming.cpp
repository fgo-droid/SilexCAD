#include "kernel/TopoNaming.h"

#include <algorithm>
#include <cmath>
#include <tuple>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>

namespace silex::kernel {

namespace {
Vec3 toVec(const gp_XYZ& p) { return {p.X(), p.Y(), p.Z()}; }
}  // namespace

GeomSignature faceSignature(const TopoDS_Face& f)
{
    GeomSignature s;
    GProp_GProps props;
    BRepGProp::SurfaceProperties(f, props);
    s.size = props.Mass();
    s.center = toVec(props.CentreOfMass().XYZ());

    BRepAdaptor_Surface a(f);
    s.type = static_cast<int>(a.GetType());
    gp_Dir n(0, 0, 1);
    if (a.GetType() == GeomAbs_Plane) {
        const gp_Ax3 ax = a.Plane().Position();
        n = ax.Direction();
        if (!ax.Direct())
            n.Reverse();
    }
    else {
        const double u = 0.5 * (a.FirstUParameter() + a.LastUParameter());
        const double v = 0.5 * (a.FirstVParameter() + a.LastVParameter());
        BRepLProp_SLProps lp(a, u, v, 1, 1e-7);
        if (lp.IsNormalDefined())
            n = lp.Normal();
    }
    if (f.Orientation() == TopAbs_REVERSED)
        n.Reverse();
    s.dir = toVec(n.XYZ());
    return s;
}

GeomSignature edgeSignature(const TopoDS_Edge& e)
{
    GeomSignature s;
    if (BRep_Tool::Degenerated(e)) {
        s.type = -1;
        return s;
    }
    GProp_GProps props;
    BRepGProp::LinearProperties(e, props);
    s.size = props.Mass();
    s.center = toVec(props.CentreOfMass().XYZ());
    BRepAdaptor_Curve c(e);
    s.type = static_cast<int>(c.GetType());
    const double m = 0.5 * (c.FirstParameter() + c.LastParameter());
    gp_Pnt p;
    gp_Vec v;
    c.D1(m, p, v);
    if (v.Magnitude() > 1e-12)
        s.dir = toVec(v.Normalized().XYZ());
    return s;
}

std::optional<Placement> facePlacement(const TopoDS_Face& f)
{
    BRepAdaptor_Surface a(f);
    if (a.GetType() != GeomAbs_Plane)
        return std::nullopt;
    const gp_Ax3 ax = a.Plane().Position();
    gp_Dir n = ax.Direction();
    if (!ax.Direct())
        n.Reverse();
    if (f.Orientation() == TopAbs_REVERSED)
        n.Reverse();
    Placement p;
    p.origin = toVec(ax.Location().XYZ());
    p.normal = toVec(n.XYZ());
    p.xDir = toVec(ax.XDirection().XYZ());
    return p;
}

ShapeHistory historyOf(BRepBuilderAPI_MakeShape& op)
{
    ShapeHistory h;
    h.modified = [&op](const TopoDS_Shape& s) -> const TopTools_ListOfShape& { return op.Modified(s); };
    h.generated = [&op](const TopoDS_Shape& s) -> const TopTools_ListOfShape& { return op.Generated(s); };
    h.isDeleted = [&op](const TopoDS_Shape& s) { return static_cast<bool>(op.IsDeleted(s)); };
    return h;
}

ShapeHistory historyOf(const Handle(BRepTools_History)& hist)
{
    ShapeHistory h;
    h.modified = [hist](const TopoDS_Shape& s) -> const TopTools_ListOfShape& { return hist->Modified(s); };
    h.generated = [hist](const TopoDS_Shape& s) -> const TopTools_ListOfShape& { return hist->Generated(s); };
    h.isDeleted = [hist](const TopoDS_Shape& s) { return static_cast<bool>(hist->IsRemoved(s)); };
    return h;
}

void nameEdgesFromFaces(NamedShape& ns)
{
    if (ns.isNull())
        return;
    TopTools_IndexedDataMapOfShapeListOfShape anc;
    TopExp::MapShapesAndAncestors(ns.shape(), TopAbs_EDGE, TopAbs_FACE, anc);
    for (int i = 1; i <= ns.edgeCount(); ++i) {
        const int idx = anc.FindIndex(ns.edge(i));
        if (idx <= 0)
            continue;
        std::vector<std::string> names;
        for (TopTools_ListIteratorOfListOfShape it(anc(idx)); it.More(); it.Next()) {
            const int fi = ns.faceIndex(it.Value());
            names.push_back(fi > 0 ? ns.faceName(fi) : std::string("?"));
        }
        if (names.empty())
            continue;
        if (names.size() == 1)
            names.push_back("-");
        std::sort(names.begin(), names.end());
        std::string n = "E[";
        for (size_t k = 0; k < names.size(); ++k) {
            if (k)
                n += '|';
            n += names[k];
        }
        n += ']';
        ns.setEdgeName(i, n);
    }
}

NamedShape propagateNames(const TopoDS_Shape& result, const std::vector<const NamedShape*>& inputs,
                          const ShapeHistory& h, const std::string& featId, const std::string& generatedKind)
{
    NamedShape out(result);

    // 1) faces kept or modified
    for (const NamedShape* in : inputs) {
        if (!in)
            continue;
        for (int i = 1; i <= in->faceCount(); ++i) {
            const auto& names = in->faceAliases(i);
            if (names.empty())
                continue;
            const TopoDS_Shape& f = in->face(i);
            if (const int j = out.faceIndex(f); j > 0) {
                for (const auto& n : names)
                    out.addFaceName(j, n);
                continue;
            }
            if (h.isDeleted && h.isDeleted(f))
                continue;
            if (h.modified) {
                const TopTools_ListOfShape mods = h.modified(f);  // copy: OCCT reuses the list
                for (TopTools_ListIteratorOfListOfShape it(mods); it.More(); it.Next())
                    if (const int j = out.faceIndex(it.Value()); j > 0)
                        for (const auto& n : names)
                            out.addFaceName(j, n);
            }
        }
    }

    // 2) faces generated from named edges (extrusion sides, fillet / chamfer faces)
    if (h.generated) {
        for (const NamedShape* in : inputs) {
            if (!in)
                continue;
            for (int i = 1; i <= in->edgeCount(); ++i) {
                const std::string& en = in->edgeName(i);
                if (en.empty())
                    continue;
                const TopTools_ListOfShape gens = h.generated(in->edge(i));
                for (TopTools_ListIteratorOfListOfShape it(gens); it.More(); it.Next())
                    if (const int j = out.faceIndex(it.Value()); j > 0)
                        out.addFaceName(j, featId + ":" + generatedKind + "(" + en + ")");
            }
        }
    }

    // 3) leftovers, ordered geometrically for determinism
    std::vector<std::pair<GeomSignature, int>> unnamed;
    for (int j = 1; j <= out.faceCount(); ++j)
        if (out.faceAliases(j).empty())
            unnamed.emplace_back(faceSignature(TopoDS::Face(out.face(j))), j);
    auto q = [](double v) { return std::llround(v * 1e4); };
    std::sort(unnamed.begin(), unnamed.end(), [&](const auto& a, const auto& b) {
        return std::make_tuple(q(a.first.center.x), q(a.first.center.y), q(a.first.center.z), q(a.first.size)) <
               std::make_tuple(q(b.first.center.x), q(b.first.center.y), q(b.first.center.z), q(b.first.size));
    });
    int k = 1;
    for (const auto& [sig, j] : unnamed)
        out.setFaceName(j, featId + ":new" + std::to_string(k++));

    nameEdgesFromFaces(out);
    return out;
}

}  // namespace silex::kernel
