// OpSketch: places a solved 2D sketch in 3D and computes its closed regions (profiles).
//
// Regions are found by splitting a large planar face with every sketch curve
// (BRepAlgoAPI_Splitter): the split faces not touching the outer frame are exactly the
// regions a user can pick (holes, overlapping circles, ... all handled by OCCT's
// General Fuse). Edge names come from the splitter history => "<sketchId>/<geoId>".

#include <algorithm>
#include <cmath>
#include <set>

#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepTools.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>

#include "kernel/TopoNaming.h"
#include "kernel/ops/Ops.h"

namespace silex::kernel {
namespace {

gp_Pnt P(const Vec3& v) { return {v.x, v.y, v.z}; }
gp_Dir D(const Vec3& v) { return {v.x, v.y, v.z}; }

class OpSketch final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<SketchParams>();
        SketchOutput out;
        out.sketch = p.sketch;
        out.plane = resolvePlane(ctx, p.plane);

        const gp_Ax3 ax(P(out.plane.origin), D(out.plane.normal), D(out.plane.xDir));
        const gp_Pln pln(ax);

        // 1) 3D edges of every non-construction curve
        std::vector<std::pair<Uuid, TopoDS_Edge>> edges;
        double umin = 0, umax = 0, vmin = 0, vmax = 0;
        auto grow = [&](Vec2 q, double r = 0) {
            umin = std::min(umin, q.x - r);
            umax = std::max(umax, q.x + r);
            vmin = std::min(vmin, q.y - r);
            vmax = std::max(vmax, q.y + r);
        };
        for (const SketchGeo& g : p.sketch.geos) {
            if (g.construction || g.kind == GeoKind::Point)
                continue;
            try {
                switch (g.kind) {
                    case GeoKind::Line: {
                        if ((g.p2 - g.p1).length() < 1e-7)
                            continue;
                        edges.emplace_back(g.id, BRepBuilderAPI_MakeEdge(P(out.plane.toWorld(g.p1)),
                                                                         P(out.plane.toWorld(g.p2))).Edge());
                        grow(g.p1);
                        grow(g.p2);
                        break;
                    }
                    case GeoKind::Circle: {
                        if (g.r < 1e-7)
                            continue;
                        gp_Circ c(gp_Ax2(P(out.plane.toWorld(g.c)), D(out.plane.normal), D(out.plane.xDir)), g.r);
                        edges.emplace_back(g.id, BRepBuilderAPI_MakeEdge(c).Edge());
                        grow(g.c, g.r);
                        break;
                    }
                    case GeoKind::Arc: {
                        if (g.r < 1e-7 || g.a1 - g.a0 < 1e-9)
                            continue;
                        gp_Circ c(gp_Ax2(P(out.plane.toWorld(g.c)), D(out.plane.normal), D(out.plane.xDir)), g.r);
                        edges.emplace_back(g.id, BRepBuilderAPI_MakeEdge(c, g.a0, g.a1).Edge());
                        grow(g.c, g.r);
                        break;
                    }
                    default: break;
                }
            }
            catch (const Standard_Failure&) {
                ctx.warnings.push_back("A degenerated sketch curve was ignored");
            }
        }

        // 2) regions
        if (!edges.empty())
            out.regions = buildRegions(ctx, pln, edges, umin, umax, vmin, vmax);
        ctx.state.sketches[m_node.id] = std::move(out);
    }

private:
    Placement resolvePlane(ExecContext& ctx, const SketchPlaneRef& ref) const
    {
        if (ref.kind != "Face")
            return originPlacement(ref.kind);
        auto r = resolveRef(ctx.state.bodies, ref.face, TopAbs_FACE, ctx.state.modelSize());
        if (!r)
            throw FeatureError("The sketch plane (face) no longer exists");
        if (!r->byName)
            ctx.warnings.push_back("Sketch plane re-attached to the closest face");
        auto pl = facePlacement(TopoDS::Face(r->shape));
        if (!pl)
            throw FeatureError("The sketch face is not planar anymore");
        return *pl;
    }

    std::vector<SketchRegion> buildRegions(ExecContext& ctx, const gp_Pln& pln,
                                           const std::vector<std::pair<Uuid, TopoDS_Edge>>& edges, double umin,
                                           double umax, double vmin, double vmax) const
    {
        const double m = std::max({umax - umin, vmax - vmin, 1.0}) * 0.25 + 1.0;
        const TopoDS_Face frame = BRepBuilderAPI_MakeFace(pln, umin - m, umax + m, vmin - m, vmax + m).Face();

        TopTools_ListOfShape args, tools;
        args.Append(frame);
        for (const auto& [id, e] : edges)
            tools.Append(e);
        BRepAlgoAPI_Splitter sp;
        sp.SetArguments(args);
        sp.SetTools(tools);
        sp.Build(ctx.progress());
        ctx.throwIfCancelled();
        if (!sp.IsDone() || sp.HasErrors())
            throw FeatureError("Could not compute the sketch profiles");

        // Images of the frame border (to discard the outer region)
        TopTools_IndexedMapOfShape border;
        for (TopExp_Explorer ex(frame, TopAbs_EDGE); ex.More(); ex.Next()) {
            const TopTools_ListOfShape mods = sp.Modified(ex.Current());
            if (mods.IsEmpty())
                border.Add(ex.Current());
            for (TopTools_ListIteratorOfListOfShape it(mods); it.More(); it.Next())
                border.Add(it.Value());
        }
        // Images of each sketch curve -> geo id
        TopTools_IndexedMapOfShape imgs;
        std::vector<Uuid> imgIds;
        for (const auto& [id, e] : edges) {
            const TopTools_ListOfShape mods = sp.Modified(e);
            if (mods.IsEmpty()) {
                imgs.Add(e);
                imgIds.push_back(id);
            }
            for (TopTools_ListIteratorOfListOfShape it(mods); it.More(); it.Next())
                if (imgs.Add(it.Value()) > static_cast<int>(imgIds.size()))
                    imgIds.push_back(id);
        }
        auto geoOf = [&](const TopoDS_Shape& e) -> Uuid {
            const int i = imgs.FindIndex(e);
            return i > 0 ? imgIds[static_cast<size_t>(i - 1)] : Uuid{};
        };

        std::vector<SketchRegion> regions;
        for (TopExp_Explorer fx(sp.Shape(), TopAbs_FACE); fx.More(); fx.Next()) {
            const TopoDS_Face f = TopoDS::Face(fx.Current());
            bool outer = false;
            for (TopExp_Explorer ex(f, TopAbs_EDGE); ex.More() && !outer; ex.Next())
                outer = border.Contains(ex.Current());
            if (outer)
                continue;

            std::set<Uuid> keyIds;
            const TopoDS_Wire ow = BRepTools::OuterWire(f);
            for (TopExp_Explorer ex(ow, TopAbs_EDGE); ex.More(); ex.Next())
                if (auto id = geoOf(ex.Current()); !id.empty())
                    keyIds.insert(id);
            std::string key;
            for (const auto& id : keyIds)
                key += (key.empty() ? "" : ",") + id;

            SketchRegion r;
            r.key = key;
            r.named.reset(f);
            r.named.setFaceName(1, m_node.id + ":region(" + key + ")");
            for (int i = 1; i <= r.named.edgeCount(); ++i)
                if (auto id = geoOf(r.named.edge(i)); !id.empty())
                    r.named.setEdgeName(i, m_node.id + "/" + id);
            regions.push_back(std::move(r));
        }
        // Deterministic order; regions sharing the same bounding curves (e.g. a circle cut
        // by a line) are told apart by their position along the plane axes.
        std::vector<std::pair<GeomSignature, size_t>> order;
        for (size_t i = 0; i < regions.size(); ++i)
            order.emplace_back(faceSignature(TopoDS::Face(regions[i].named.face(1))), i);
        const gp_Dir xd = pln.Position().XDirection(), yd = pln.Position().YDirection();
        auto u = [&](const GeomSignature& s) { return s.center.x * xd.X() + s.center.y * xd.Y() + s.center.z * xd.Z(); };
        auto v = [&](const GeomSignature& s) { return s.center.x * yd.X() + s.center.y * yd.Y() + s.center.z * yd.Z(); };
        std::sort(order.begin(), order.end(), [&](const auto& a, const auto& b) {
            const auto& ra = regions[a.second];
            const auto& rb = regions[b.second];
            if (ra.key != rb.key)
                return ra.key < rb.key;
            if (std::abs(u(a.first) - u(b.first)) > 1e-7)
                return u(a.first) < u(b.first);
            return v(a.first) < v(b.first);
        });
        std::vector<SketchRegion> sorted;
        for (size_t k = 0; k < order.size(); ++k) {
            SketchRegion r = regions[order[k].second];
            int dup = 0;
            for (size_t j = 0; j < k; ++j)
                if (regions[order[j].second].key == r.key)
                    ++dup;
            if (dup > 0) {
                r.key += "#" + std::to_string(dup);
                r.named.setFaceName(1, m_node.id + ":region(" + r.key + ")");
            }
            sorted.push_back(std::move(r));
        }
        return sorted;
    }
};

}  // namespace

std::unique_ptr<ParametricFeature> makeOpSketch(const FeatureNode& n) { return std::make_unique<OpSketch>(n); }

}  // namespace silex::kernel
