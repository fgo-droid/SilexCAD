// v0.3 Lot 3.1 — HlrProjector: hidden line removal (HLRBRep_Algo + HLRBRep_HLRToShape) of the
// bodies in the 3 standard orthographic views, first-angle (European) convention:
//   front view  : seen from -Y, X to the right, Z up      ("Vue de face (XZ)")
//   top view    : seen from +Z, X to the right, Y up      ("Vue de dessus (XY)")
//   right view  : seen from +X, Y to the right, Z up      ("Vue de droite (YZ)")
// Visible edges = VCompound + OutLineVCompound (sharp edges and silhouettes), hidden edges =
// HCompound + OutLineHCompound. Circles seen face-on get centre marks; cylinders seen side-on
// get their axis (thin chain line).

#include <algorithm>
#include <cmath>
#include <map>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <HLRAlgo_Projector.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_HLRToShape.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Circ.hxx>
#include <gp_Cylinder.hxx>

#include "kernel/Analysis.h"
#include "kernel/KernelEngine.h"
#include "kernel/Recompute.h"

namespace silex {

using namespace kernel;

namespace {

struct ViewDef
{
    const char* name;
    const char* label;
    gp_Ax2 cs;  ///< main direction towards the viewer
};

std::vector<ViewDef> standardViews()
{
    return {
        {"front", "Vue de face (XZ)", gp_Ax2(gp::Origin(), gp_Dir(0, -1, 0), gp_Dir(1, 0, 0))},
        {"top", "Vue de dessus (XY)", gp_Ax2(gp::Origin(), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))},
        {"right", "Vue de droite (YZ)", gp_Ax2(gp::Origin(), gp_Dir(1, 0, 0), gp_Dir(0, 1, 0))},
    };
}

Vec2 to2d(const gp_Ax2& cs, const gp_Pnt& p)
{
    const gp_Vec v(cs.Location(), p);
    return {v.Dot(gp_Vec(cs.XDirection())), v.Dot(gp_Vec(cs.YDirection()))};
}

void grow(DrawingViewData& v, const Vec2& p, bool& first)
{
    if (first) {
        v.min = v.max = p;
        first = false;
        return;
    }
    v.min = {std::min(v.min.x, p.x), std::min(v.min.y, p.y)};
    v.max = {std::max(v.max.x, p.x), std::max(v.max.y, p.y)};
}

/// Edges of an HLR result compound (already expressed in the projection plane: x, y, z = 0).
void collect(const TopoDS_Shape& comp, DrawLineKind kind, double deflection, DrawingViewData& view, bool& first,
             std::vector<Vec2>& ends)
{
    if (comp.IsNull())
        return;
    for (TopExp_Explorer ex(comp, TopAbs_EDGE); ex.More(); ex.Next()) {
        const TopoDS_Edge e = TopoDS::Edge(ex.Current());
        try {
            BRepAdaptor_Curve c(e);
            DrawingCurve dc;
            dc.kind = kind;
            if (c.GetType() == GeomAbs_Line) {
                const gp_Pnt a = c.Value(c.FirstParameter()), b = c.Value(c.LastParameter());
                dc.pts = {{a.X(), a.Y()}, {b.X(), b.Y()}};
            }
            else {
                GCPnts_TangentialDeflection disc(c, 0.1, deflection);
                for (int i = 1; i <= disc.NbPoints(); ++i) {
                    const gp_Pnt p = disc.Value(i);
                    dc.pts.push_back({p.X(), p.Y()});
                }
            }
            if (dc.pts.size() < 2)
                continue;
            if ((dc.pts.front() - dc.pts.back()).length() < 1e-9 && dc.pts.size() == 2)
                continue;
            for (const Vec2& p : dc.pts)
                grow(view, p, first);
            if (c.GetType() == GeomAbs_Circle) {
                const gp_Circ ci = c.Circle();
                DrawingCircle circ;
                circ.center = {ci.Location().X(), ci.Location().Y()};
                circ.radius = ci.Radius();
                const double span = std::abs(c.LastParameter() - c.FirstParameter());
                circ.full = span > 2 * kPi - 1e-6;
                if (!circ.full) {
                    const gp_Pnt a = c.Value(c.FirstParameter()), b = c.Value(c.LastParameter());
                    circ.a0 = std::atan2(a.Y() - circ.center.y, a.X() - circ.center.x);
                    circ.a1 = std::atan2(b.Y() - circ.center.y, b.X() - circ.center.x);
                }
                circ.visible = kind == DrawLineKind::Visible;
                view.circles.push_back(circ);
            }
            if (kind == DrawLineKind::Visible) {
                ends.push_back(dc.pts.front());
                ends.push_back(dc.pts.back());
            }
            view.curves.push_back(std::move(dc));
        }
        catch (const Standard_Failure&) {
        }
    }
}

/// Merges circle arcs sharing a centre and radius (HLR splits circles at silhouettes / seams).
void mergeCircles(DrawingViewData& v)
{
    std::vector<DrawingCircle> out;
    for (const DrawingCircle& c : v.circles) {
        bool merged = false;
        for (DrawingCircle& o : out)
            if ((o.center - c.center).length() < 1e-6 && std::abs(o.radius - c.radius) < 1e-6) {
                o.visible = o.visible || c.visible;
                if (!c.full) {
                    // two half arcs make a full circle (good enough for centre marks and diameters)
                    o.full = true;
                }
                merged = true;
                break;
            }
        if (!merged)
            out.push_back(c);
    }
    v.circles = std::move(out);
}

void addAxis(DrawingViewData& v, Vec2 a, Vec2 b)
{
    DrawingCurve dc;
    dc.kind = DrawLineKind::Axis;
    dc.pts = {a, b};
    // de-duplicate (concentric cylinders share their axis)
    for (const DrawingCurve& c : v.curves)
        if (c.kind == DrawLineKind::Axis && c.pts.size() == 2 &&
            (((c.pts[0] - a).length() < 1e-6 && (c.pts[1] - b).length() < 1e-6) ||
             ((c.pts[0] - b).length() < 1e-6 && (c.pts[1] - a).length() < 1e-6)))
            return;
    v.curves.push_back(std::move(dc));
}

/// Centre marks of circles and axes of cylinders seen side-on.
void addCenterLines(DrawingViewData& v, const TopoDS_Shape& shape, const gp_Ax2& cs)
{
    const double ext = 2.5;  // overshoot of axis lines (mm, drawing scale 1:1)
    for (const DrawingCircle& c : v.circles) {
        const double r = c.radius + ext;
        addAxis(v, {c.center.x - r, c.center.y}, {c.center.x + r, c.center.y});
        addAxis(v, {c.center.x, c.center.y - r}, {c.center.x, c.center.y + r});
    }
    const gp_Dir view = cs.Direction();
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face f = TopoDS::Face(ex.Current());
        BRepAdaptor_Surface s(f);
        if (s.GetType() != GeomAbs_Cylinder && s.GetType() != GeomAbs_Cone)
            continue;
        const gp_Ax1 ax = s.GetType() == GeomAbs_Cylinder ? s.Cylinder().Axis() : s.Cone().Axis();
        if (std::abs(ax.Direction().Dot(view)) > 1e-3)
            continue;  // not seen side-on
        // extent of the face along its axis
        double t0 = 1e300, t1 = -1e300;
        for (TopExp_Explorer vx(f, TopAbs_VERTEX); vx.More(); vx.Next()) {
            const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vx.Current()));
            const double t = gp_Vec(ax.Location(), p).Dot(gp_Vec(ax.Direction()));
            t0 = std::min(t0, t);
            t1 = std::max(t1, t);
        }
        if (t1 - t0 < 1e-6)
            continue;
        const gp_Pnt a = ax.Location().Translated(gp_Vec(ax.Direction()) * (t0 - ext));
        const gp_Pnt b = ax.Location().Translated(gp_Vec(ax.Direction()) * (t1 + ext));
        addAxis(v, to2d(cs, a), to2d(cs, b));
    }
}

}  // namespace

DrawingProjection KernelEngine::projectDrawing(const DocState& state, const std::vector<Uuid>& bodies)
{
    DrawingProjection out;
    try {
        Recompute r;
        ComputeOutput co = r.run(state, {}, {});
        BRep_Builder bb;
        TopoDS_Compound all;
        bb.MakeCompound(all);
        int n = 0;
        for (const Uuid& id : co.state.bodyOrder) {
            if (!bodies.empty() && std::find(bodies.begin(), bodies.end(), id) == bodies.end())
                continue;
            const TopoDS_Shape& s = co.state.bodies.at(id).shape();
            bb.Add(all, s);
            const MassProps mp = massProps(s);
            out.volume += mp.volume;
            out.mass += massGrams(mp.volume, state.materialOf(id).density);
            ++n;
        }
        if (n == 0)
            throw std::runtime_error("There is no body to draw");

        Bnd_Box box;
        BRepBndLib::Add(all, box);
        const double size = box.IsVoid() ? 100.0 : std::sqrt(box.SquareExtent());
        const double deflection = std::clamp(size * 2e-4, 0.002, 0.5);

        for (const ViewDef& vd : standardViews()) {
            Handle(HLRBRep_Algo) algo = new HLRBRep_Algo();
            algo->Add(all);
            algo->Projector(HLRAlgo_Projector(vd.cs));
            algo->Update();
            algo->Hide();
            HLRBRep_HLRToShape hs(algo);

            DrawingViewData v;
            v.name = vd.name;
            v.label = vd.label;
            bool first = true;
            std::vector<Vec2> ends;
            collect(hs.VCompound(), DrawLineKind::Visible, deflection, v, first, ends);
            collect(hs.OutLineVCompound(), DrawLineKind::Visible, deflection, v, first, ends);
            collect(hs.HCompound(), DrawLineKind::Hidden, deflection, v, first, ends);
            collect(hs.OutLineHCompound(), DrawLineKind::Hidden, deflection, v, first, ends);
            mergeCircles(v);
            addCenterLines(v, all, vd.cs);
            // snap vertices: unique ends of visible edges + circle centres
            for (const Vec2& p : ends)
                if (std::none_of(v.vertices.begin(), v.vertices.end(),
                                 [&](const Vec2& q) { return (q - p).length() < 1e-6; }))
                    v.vertices.push_back(p);
            out.views.push_back(std::move(v));
        }
    }
    catch (const Standard_Failure& e) {
        out.error = std::string("Geometry kernel error: ") + (e.GetMessageString() ? e.GetMessageString() : "unknown");
    }
    catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

}  // namespace silex
