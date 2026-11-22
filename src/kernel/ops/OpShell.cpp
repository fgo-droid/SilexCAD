// OpShell: hollows a solid (BRepOffsetAPI_MakeThickSolid), removed faces become openings.

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <gp_Pln.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepOffsetAPI_MakeOffsetShape.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Plane.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_MapOfShape.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <memory>

#include "kernel/TopoNaming.h"
#include "kernel/ops/Ops.h"

namespace silex::kernel {
namespace {

double volumeOf(const TopoDS_Shape& s)
{
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return std::abs(g.Mass());
}

class OpShell final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<ShellParams>();
        if (p.thickness <= 0)
            throw FeatureError("The shell thickness must be positive");

        const double size = ctx.state.modelSize();
        Uuid bodyId = p.body;
        NCollection_List<TopoDS_Shape> faces;
        for (const TopoRef& ref : p.removedFaces) {
            auto r = resolveRef(ctx.state.bodies, ref, TopAbs_FACE, size);
            if (!r)
                throw FeatureError("A face to remove no longer exists");
            if (!r->byName)
                ctx.warnings.push_back("A face reference was re-attached by geometry");
            if (bodyId.empty())
                bodyId = r->owner;
            if (r->owner != bodyId)
                throw FeatureError("All removed faces must belong to the shelled body");
            faces.Append(r->shape);
        }
        if (bodyId.empty())
            bodyId = ctx.state.lastEditableBody();
        requireEditable(ctx.state, bodyId);
        auto it = ctx.state.bodies.find(bodyId);
        if (it == ctx.state.bodies.end())
            throw FeatureError("There is no body to shell");
        NamedShape& body = it->second;

        // MakeThickSolid can report success and hand back the untouched solid (seen on lofts whose
        // B-spline side faces meet tangentially): the volume tells whether it really hollowed the
        // body; otherwise other join settings are tried before giving up with a clear message.
        const double v0 = volumeOf(body.shape());
        if (faces.IsEmpty() && !p.outward) {
            // v0.5: no removed face = closed hollow body (enclosure blank split later into box + lid)
            // inner offset: Arc joins first (rounded bodies), then Intersection (sharp boxes)
            std::unique_ptr<BRepAlgoAPI_Cut> found;
            std::string why;
            for (const GeomAbs_JoinType join : {GeomAbs_Arc, GeomAbs_Intersection}) {
                BRepOffsetAPI_MakeOffsetShape off;
                try {
                    off.PerformByJoin(body.shape(), -p.thickness, 1e-3, BRepOffset_Skin, false, false, join);
                }
                catch (const Standard_Failure&) {
                    why += " offset-exception";
                    continue;
                }
                if (!off.IsDone() || off.Shape().IsNull()) {
                    why += " offset-failed(" + std::to_string(int(off.MakeOffset().Error())) + ")";
                    continue;
                }
                auto c = std::make_unique<BRepAlgoAPI_Cut>(body.shape(), off.Shape());
                if (!c->IsDone() || c->HasErrors()) {
                    // the boolean refuses a tool entirely inside the body (rounded blanks): build the
                    // hollow solid directly = outer skin + reversed inner skin
                    TopExp_Explorer outer(body.shape(), TopAbs_SHELL), inner(off.Shape(), TopAbs_SHELL);
                    if (outer.More() && inner.More()) {
                        TopoDS_Solid hollow;
                        BRep_Builder hb;
                        hb.MakeSolid(hollow);
                        hb.Add(hollow, outer.Current());
                        hb.Add(hollow, inner.Current().Reversed());
                        const double v = volumeOf(hollow);
                        if (v > 1e-6 && v < v0 * 0.999 && BRepCheck_Analyzer(hollow).IsValid()) {
                            NamedShape result = propagateNames(hollow, {&body}, ShapeHistory{}, m_node.id, "shell");
                            for (int i = 1; i <= result.faceCount(); ++i)
                                if (result.faceName(i).empty() || result.faceName(i).rfind(m_node.id, 0) == 0)
                                    result.setFaceName(i, m_node.id + ":inner" + std::to_string(i));
                            nameEdgesFromFaces(result);
                            if (ctx.wantPreview) {
                                ctx.preview = result.shape();
                                ctx.previewOp = BooleanOp::Join;
                                ctx.previewKeepsInput = true;
                            }
                            body = std::move(result);
                            return;
                        }
                        why += " hollow v=" + std::to_string(v);
                    }
                    why += " cut-failed";
                    continue;
                }
                const double v = volumeOf(c->Shape());
                why += " v=" + std::to_string(v) + "/" + std::to_string(v0) + (BRepCheck_Analyzer(c->Shape()).IsValid() ? "" : " invalid");
                if (v > 1e-6 && v < v0 * 0.999 && BRepCheck_Analyzer(c->Shape()).IsValid()) {
                    found = std::move(c);
                    break;
                }
            }
            if (!found)
                throw FeatureError("The shell failed (thickness too large for the geometry?)");
            (void)why;  // diagnostic kept for debugging sessions
            BRepAlgoAPI_Cut& cut = *found;
            NamedShape result = propagateNames(cut.Shape(), {&body}, historyOf(cut), m_node.id, "shell");
            for (int i = 1; i <= result.faceCount(); ++i)
                if (result.faceName(i).empty() || result.faceName(i).rfind(m_node.id, 0) == 0)
                    result.setFaceName(i, m_node.id + ":inner" + std::to_string(i));
            nameEdgesFromFaces(result);
            checkSolid(result.shape(), "The shell");
            if (ctx.wantPreview) {
                ctx.preview = result.shape();
                ctx.previewOp = BooleanOp::Join;
                ctx.previewKeepsInput = true;
            }
            body = std::move(result);
            return;
        }
        struct Try
        {
            GeomAbs_JoinType join;
            bool intersection;
            double tol;
        };
        const Try tries[] = {{GeomAbs_Arc, false, 1e-3}, {GeomAbs_Intersection, false, 1e-3},
                             {GeomAbs_Arc, true, 1e-3}, {GeomAbs_Arc, false, 1e-2}, {GeomAbs_Intersection, true, 1e-2}};
        const double offset = p.outward ? p.thickness : -p.thickness;
        std::unique_ptr<BRepOffsetAPI_MakeThickSolid> done;
        for (const Try& t : tries) {
            auto mk = std::make_unique<BRepOffsetAPI_MakeThickSolid>();
            try {
                mk->MakeThickSolidByJoin(body.shape(), faces, offset, t.tol, BRepOffset_Skin, t.intersection, false,
                                         t.join, false, ctx.progress());
            }
            catch (const Standard_Failure&) {
                continue;
            }
            ctx.throwIfCancelled();
            if (!mk->IsDone() || mk->Shape().IsNull())
                continue;
            const double v = volumeOf(mk->Shape());
            if (v > 1e-9 && std::abs(v - v0) > 1e-6 * std::max(1.0, v0)) {
                done = std::move(mk);
                break;
            }
        }
        TopoDS_Shape shape;
        ShapeHistory hist;
        if (done) {
            shape = done->Shape();
            hist = historyOf(*done);
        }
        else if (!faces.IsEmpty()) {
            // Fallback for smooth bodies (lofts): the open skin (body minus removed faces) thickened
            // by a simple normal offset. Accepted only if it gives a valid solid of plausible volume.
            TopTools_MapOfShape removed;
            for (const TopoDS_Shape& f : faces)
                removed.Add(f);
            TopoDS_Shell skin;
            BRep_Builder bb;
            bb.MakeShell(skin);
            GProp_GProps areaProps;
            for (TopExp_Explorer ex(body.shape(), TopAbs_FACE); ex.More(); ex.Next())
                if (!removed.Contains(ex.Current())) {
                    bb.Add(skin, ex.Current());
                    BRepGProp::SurfaceProperties(ex.Current(), areaProps);
                }
            auto mk = std::make_unique<BRepOffsetAPI_MakeThickSolid>();
            try {
                mk->MakeThickSolidBySimple(skin, offset);
            }
            catch (const Standard_Failure&) {
            }
            if (mk->IsDone() && !mk->Shape().IsNull() && BRepCheck_Analyzer(mk->Shape()).IsValid()) {
                const double v = volumeOf(mk->Shape()), expect = areaProps.Mass() * p.thickness;
                if (v > 0.6 * expect && v < 1.4 * expect) {
                    shape = mk->Shape();
                    hist = historyOf(*mk);
                    ctx.warnings.push_back("Shell built by a simple offset of the skin (smooth body)");
                }
            }
        }
        if (shape.IsNull() && !p.outward) {
            // Second fallback: body minus its inward offset, then the openings cut through the walls
            // by short prisms on the removed faces (planar faces only: end caps of lofts, sweeps...).
            try {
                BRepOffsetAPI_MakeOffsetShape off;
                off.PerformByJoin(body.shape(), -p.thickness, 1e-3, BRepOffset_Skin, false, false, GeomAbs_Intersection);
                bool planar = true;
                for (const TopoDS_Shape& f : faces)
                    planar = planar && !Handle(Geom_Plane)::DownCast(BRep_Tool::Surface(TopoDS::Face(f))).IsNull();
                if (off.IsDone() && planar) {
                    BRepAlgoAPI_Cut cut(body.shape(), off.Shape());
                    TopoDS_Shape walls = cut.Shape();
                    for (const TopoDS_Shape& f : faces) {
                        const TopoDS_Face face = TopoDS::Face(f);
                        const gp_Pln pl = Handle(Geom_Plane)::DownCast(BRep_Tool::Surface(face))->Pln();
                        // outward normal: the side of the face where the body is not
                        GProp_GProps fp;
                        BRepGProp::SurfaceProperties(face, fp);
                        gp_Dir n = pl.Axis().Direction();
                        BRepClass3d_SolidClassifier cls(body.shape(), fp.CentreOfMass().Translated(gp_Vec(n) * (0.5 * p.thickness)), 1e-6);
                        if (cls.State() == TopAbs_IN)
                            n.Reverse();
                        // the matching cap of the inner solid (parallel, one thickness inside), pushed out
                        // through the floor: only the floor goes, the walls keep their full length
                        for (TopExp_Explorer ex(off.Shape(), TopAbs_FACE); ex.More(); ex.Next()) {
                            const TopoDS_Face inner = TopoDS::Face(ex.Current());
                            auto ip = Handle(Geom_Plane)::DownCast(BRep_Tool::Surface(inner));
                            if (ip.IsNull() || !ip->Pln().Axis().Direction().IsParallel(n, 1e-6) ||
                                std::abs(pl.Distance(ip->Pln().Location()) - p.thickness) > 1e-3 * std::max(1.0, p.thickness))
                                continue;
                            BRepPrimAPI_MakePrism prism(inner, gp_Vec(n) * (2.0 * p.thickness));
                            BRepAlgoAPI_Cut open(walls, prism.Shape());
                            if (open.IsDone())
                                walls = open.Shape();
                        }
                    }
                    const double v = volumeOf(walls);
                    if (cut.IsDone() && v > 1e-6 && v < v0 * 0.999 && BRepCheck_Analyzer(walls).IsValid()) {
                        shape = walls;
                        ctx.warnings.push_back("Shell built as the body minus its inner offset (smooth body)");
                    }
                }
            }
            catch (const Standard_Failure&) {
            }
        }
        if (shape.IsNull())
            throw FeatureError("The shell failed (thickness too large for the geometry?)");
        NamedShape result = propagateNames(shape, {&body}, hist, m_node.id, "shell");
        checkSolid(result.shape(), "The shell");
        if (ctx.wantPreview) {
            ctx.preview = result.shape();
            ctx.previewOp = BooleanOp::Join;
            ctx.previewKeepsInput = true;  // keep the original faces pickable while editing
        }
        body = std::move(result);
    }
};

}  // namespace

std::unique_ptr<ParametricFeature> makeOpShell(const FeatureNode& n) { return std::make_unique<OpShell>(n); }

}  // namespace silex::kernel
