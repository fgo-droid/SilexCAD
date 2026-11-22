// v0.3 Lot 2 — OpSweep (BRepOffsetAPI_MakePipeShell) and OpLoft (BRepOffsetAPI_ThruSections).
//
// TNP: lateral faces are named from the profile edges they are generated from
// ("<featId>:side(<sketchId>/<geoId>)"), the end caps "<featId>:start" / "<featId>:end".

#include <algorithm>
#include <cmath>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>

#include "kernel/TopoNaming.h"
#include "kernel/ops/Ops.h"

namespace silex::kernel {
namespace {

gp_Pnt P(const Vec3& v) { return {v.x, v.y, v.z}; }
gp_Dir D(const Vec3& v) { return {v.x, v.y, v.z}; }

void nameCaps(NamedShape& ns, const TopoDS_Shape& cap, const std::string& name)
{
    if (cap.IsNull())
        return;
    for (TopExp_Explorer ex(cap, TopAbs_FACE); ex.More(); ex.Next())
        if (const int i = ns.faceIndex(ex.Current()); i > 0)
            ns.setFaceName(i, name);
}

/// 3D edges of the (non-construction) curves of a sketch.
std::vector<TopoDS_Edge> sketchEdges(const SketchOutput& so, const std::vector<Uuid>& only)
{
    std::vector<TopoDS_Edge> edges;
    const Placement& pl = so.plane;
    for (const SketchGeo& g : so.sketch.geos) {
        if (g.kind == GeoKind::Point)
            continue;
        if (only.empty() ? g.construction : std::find(only.begin(), only.end(), g.id) == only.end())
            continue;
        switch (g.kind) {
            case GeoKind::Line:
                if ((g.p2 - g.p1).length() > 1e-7)
                    edges.push_back(BRepBuilderAPI_MakeEdge(P(pl.toWorld(g.p1)), P(pl.toWorld(g.p2))).Edge());
                break;
            case GeoKind::Circle:
                if (g.r > 1e-7)
                    edges.push_back(
                        BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(P(pl.toWorld(g.c)), D(pl.normal), D(pl.xDir)), g.r)).Edge());
                break;
            case GeoKind::Arc:
                if (g.r > 1e-7 && g.a1 - g.a0 > 1e-9)
                    edges.push_back(BRepBuilderAPI_MakeEdge(
                                        gp_Circ(gp_Ax2(P(pl.toWorld(g.c)), D(pl.normal), D(pl.xDir)), g.r), g.a0, g.a1)
                                        .Edge());
                break;
            default: break;
        }
    }
    return edges;
}

TopoDS_Wire buildPath(ExecContext& ctx, const PathRef& path)
{
    std::vector<TopoDS_Edge> edges;
    if (!path.sketch.empty()) {
        auto it = ctx.state.sketches.find(path.sketch);
        if (it == ctx.state.sketches.end())
            throw FeatureError("The path sketch is missing or failed");
        edges = sketchEdges(it->second, path.curves);
    }
    else {
        for (const TopoRef& r : path.edges) {
            auto res = resolveRef(ctx.state.bodies, r, TopAbs_EDGE, ctx.state.modelSize());
            if (!res)
                throw FeatureError("A path edge no longer exists");
            if (!res->byName)
                ctx.warnings.push_back("A path edge was re-attached by geometry");
            edges.push_back(TopoDS::Edge(res->shape));
        }
    }
    if (edges.empty())
        throw FeatureError("The path is empty: select a sketch with curves or model edges");
    TopTools_ListOfShape list;
    for (const auto& e : edges)
        list.Append(e);
    BRepBuilderAPI_MakeWire mw;
    mw.Add(list);
    if (!mw.IsDone())
        throw FeatureError("The path curves must form one continuous chain");
    TopoDS_Wire w = mw.Wire();
    int n = 0;
    for (TopExp_Explorer ex(w, TopAbs_EDGE); ex.More(); ex.Next())
        ++n;
    if (n != int(edges.size()))
        throw FeatureError("The path curves must form one single connected chain (no branch)");
    return w;
}

gp_Pnt centroid(const TopoDS_Shape& s)
{
    GProp_GProps p;
    BRepGProp::SurfaceProperties(s, p);
    return p.CentreOfMass();
}

class OpSweep final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<SweepParams>();
        const auto regions = selectRegions(ctx, p.profile);
        TopoDS_Wire spine = buildPath(ctx, p.path);
        const Placement& pl = ctx.state.sketches.at(p.profile.sketch).plane;

        std::vector<NamedShape> tools;
        for (const SketchRegion* r : regions) {
            const TopoDS_Face face = TopoDS::Face(r->named.face(1));
            // The profile belongs at the start of the path: reverse the spine if it is closer to its end.
            TopoDS_Vertex v0, v1;
            TopExp::Vertices(spine, v0, v1);
            if (!v0.IsNull() && !v1.IsNull() && !v0.IsSame(v1)) {
                const gp_Pnt c = centroid(face);
                if (c.Distance(BRep_Tool::Pnt(v1)) + 1e-9 < c.Distance(BRep_Tool::Pnt(v0)))
                    spine.Reverse();
            }
            NamedShape tool = sweepWire(ctx, spine, BRepTools::OuterWire(face), r->named, p.orientation, pl);
            // holes of the profile: swept and removed
            for (TopExp_Explorer wx(face, TopAbs_WIRE); wx.More(); wx.Next()) {
                const TopoDS_Wire w = TopoDS::Wire(wx.Current());
                if (w.IsSame(BRepTools::OuterWire(face)))
                    continue;
                NamedShape hole = sweepWire(ctx, spine, w, r->named, p.orientation, pl);
                BRepAlgoAPI_Cut cut(tool.shape(), hole.shape());
                if (!cut.IsDone() || cut.HasErrors())
                    throw FeatureError("Could not hollow the swept profile");
                tool = propagateNames(cut.Shape(), {&tool, &hole}, historyOf(cut), m_node.id);
            }
            checkSolid(tool.shape(), "The sweep");
            tools.push_back(std::move(tool));
            ctx.throwIfCancelled();
        }
        NamedShape tool = fuseAll(ctx, tools);
        if (ctx.wantPreview) {
            ctx.preview = tool.shape();
            ctx.previewOp = p.op;
        }
        const Uuid body = applyBoolean(ctx, tool, p.op, p.targetBody);
        ctx.state.featureTools[m_node.id] = FeatureTool{tool, p.op == BooleanOp::NewBody ? BooleanOp::Join : p.op, body};
    }

private:
    NamedShape sweepWire(ExecContext& ctx, const TopoDS_Wire& spine, const TopoDS_Wire& profile, const NamedShape& named,
                         const std::string& mode, const Placement& pl) const
    {
        BRepOffsetAPI_MakePipeShell mk(spine);
        if (mode == "Frenet")
            mk.SetMode(Standard_True);
        else if (mode == "Fixed")
            mk.SetMode(gp_Ax2(P(pl.origin), D(pl.normal), D(pl.xDir)));
        else
            mk.SetMode(Standard_False);  // CorrectedFrenet: no twist along 3D curves
        mk.Add(profile, Standard_False, Standard_False);
        mk.Build(ctx.progress());
        ctx.throwIfCancelled();
        if (!mk.IsDone())
            throw FeatureError("The sweep failed (check that the profile is at the start of the path and smaller "
                               "than its curvature radius)");
        if (!mk.MakeSolid())
            throw FeatureError("The swept shape could not be closed into a solid");
        NamedShape tool = propagateNames(mk.Shape(), {&named}, historyOf(mk), m_node.id, "side");
        nameCaps(tool, mk.FirstShape(), m_node.id + ":start");
        nameCaps(tool, mk.LastShape(), m_node.id + ":end");
        nameEdgesFromFaces(tool);
        return tool;
    }
};

class OpLoft final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<LoftParams>();
        if (p.sections.size() < 2)
            throw FeatureError("A loft needs at least 2 profiles");
        BRepOffsetAPI_ThruSections ts(p.solid ? Standard_True : Standard_False, p.ruled ? Standard_True : Standard_False);
        ts.CheckCompatibility(Standard_True);
        std::vector<NamedShape> inputs;
        std::vector<Placement> planes;
        for (const ProfileRef& ref : p.sections) {
            const auto regions = selectRegions(ctx, ref);
            if (regions.size() > 1)
                ctx.warnings.push_back("Only the first region of a multi-region loft section is used");
            const SketchRegion* r = regions.front();
            const Placement& pl = ctx.state.sketches.at(ref.sketch).plane;
            for (const Placement& q : planes)
                if (std::abs(q.normal.dot(pl.normal)) > 1 - 1e-9 && std::abs((pl.origin - q.origin).dot(q.normal)) < 1e-6)
                    throw FeatureError("Two loft profiles lie on the same plane");
            planes.push_back(pl);
            inputs.push_back(r->named);
            ts.AddWire(BRepTools::OuterWire(TopoDS::Face(r->named.face(1))));
        }
        ts.Build(ctx.progress());
        ctx.throwIfCancelled();
        if (!ts.IsDone())
            throw FeatureError("The loft failed (profiles incompatible or self-intersecting)");
        std::vector<const NamedShape*> in;
        for (const auto& s : inputs)
            in.push_back(&s);
        NamedShape tool = propagateNames(ts.Shape(), in, historyOf(ts), m_node.id, "side");
        nameCaps(tool, ts.FirstShape(), m_node.id + ":start");
        nameCaps(tool, ts.LastShape(), m_node.id + ":end");
        nameEdgesFromFaces(tool);
        if (p.solid)
            checkSolid(tool.shape(), "The loft");
        if (ctx.wantPreview) {
            ctx.preview = tool.shape();
            ctx.previewOp = p.op;
        }
        const Uuid body = applyBoolean(ctx, tool, p.op, p.targetBody);
        ctx.state.featureTools[m_node.id] = FeatureTool{tool, p.op == BooleanOp::NewBody ? BooleanOp::Join : p.op, body};
    }
};

}  // namespace

std::unique_ptr<ParametricFeature> makeOpSweep(const FeatureNode& n) { return std::make_unique<OpSweep>(n); }
std::unique_ptr<ParametricFeature> makeOpLoft(const FeatureNode& n) { return std::make_unique<OpLoft>(n); }

}  // namespace silex::kernel
