// v0.4 lot 0 — direct management of bodies and faces, as parametric timeline steps:
//  * OpRemoveBody    — bodies become inactive from this step on (history untouched, undoable);
//  * OpDefeature     — removes faces and heals the solid (BRepAlgoAPI_Defeaturing), STEP included;
//  * OpSplitBody     — splits a body with a plane (BOPAlgo_Splitter) into <name>_1 / <name>_2;
//  * OpTransformBody — moves / rotates (or copies) bodies; lot 1 "mates" compute the transform.

#include <algorithm>
#include <cmath>

#include <BOPAlgo_Splitter.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepTools.hxx>
#include <gp_Ax1.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <BRepAlgoAPI_Defeaturing.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>

#include "kernel/TopoNaming.h"
#include "kernel/ops/Mate.h"
#include "kernel/ops/Ops.h"

namespace silex::kernel {
namespace {

void insertBodyAfter(ModelState& st, const Uuid& after, const Uuid& id)
{
    auto it = std::find(st.bodyOrder.begin(), st.bodyOrder.end(), after);
    st.bodyOrder.insert(it == st.bodyOrder.end() ? st.bodyOrder.end() : it + 1, id);
}

// ------------------------------------------------------------------ RemoveBody

class OpRemoveBody final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<RemoveBodyParams>();
        if (p.bodies.empty())
            throw FeatureError("Select the body (or bodies) to remove");
        int removed = 0;
        for (const Uuid& id : p.bodies) {
            if (!ctx.state.bodies.count(id)) {
                ctx.warnings.push_back("A body to remove no longer exists");
                continue;
            }
            ctx.state.bodies.erase(id);
            std::erase(ctx.state.bodyOrder, id);
            ctx.state.bodyNames.erase(id);
            ++removed;
        }
        if (!removed)
            throw FeatureError("None of the bodies to remove exists anymore");
    }
};

// ------------------------------------------------------------------ DefeatureFace

class OpDefeature final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<DefeatureParams>();
        if (p.faces.empty())
            throw FeatureError("Select the faces to delete");
        Uuid bodyId;
        TopTools_ListOfShape faces;
        for (const TopoRef& ref : p.faces) {
            auto r = resolveRef(ctx.state.bodies, ref, TopAbs_FACE, ctx.state.modelSize());
            if (!r)
                throw FeatureError("A face to delete no longer exists");
            if (!r->byName)
                ctx.warnings.push_back("A face reference was re-attached by geometry");
            if (bodyId.empty())
                bodyId = r->owner;
            else if (r->owner != bodyId)
                throw FeatureError("All the deleted faces must belong to the same body");
            faces.Append(r->shape);
        }
        requireEditable(ctx.state, bodyId);
        NamedShape& body = ctx.state.bodies.at(bodyId);
        BRepAlgoAPI_Defeaturing df;
        df.SetShape(body.shape());
        df.AddFacesToRemove(faces);
        df.SetToFillHistory(true);
        df.Build(ctx.progress());
        ctx.throwIfCancelled();
        if (!df.IsDone() || df.HasErrors())
            throw FeatureError("These faces cannot be deleted: the neighbouring faces do not close the gap "
                               "(try deleting the whole fillet / pocket chain)");
        NamedShape result = propagateNames(df.Shape(), {&body}, historyOf(df.History()), m_node.id, "heal");
        checkSolid(result.shape(), "The face deletion");
        if (ctx.wantPreview) {
            ctx.preview = result.shape();
            ctx.previewOp = BooleanOp::Join;
            ctx.previewKeepsInput = true;  // keep the faces pickable while editing
        }
        body = std::move(result);
    }
};

// ------------------------------------------------------------------ SplitBody

class OpSplitBody final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<SplitBodyParams>();
        Uuid bodyId = p.body;
        if (bodyId.empty())
            bodyId = ctx.state.lastEditableBody();
        auto it = ctx.state.bodies.find(bodyId);
        if (it == ctx.state.bodies.end())
            throw FeatureError("The body to split no longer exists");
        requireEditable(ctx.state, bodyId);
        const NamedShape body = it->second;
        const Placement pl = resolveBasePlane(ctx, p.plane);
        const gp_Pln pln(gp_Pnt(pl.origin.x, pl.origin.y, pl.origin.z), gp_Dir(pl.normal.x, pl.normal.y, pl.normal.z));

        // a planar face larger than the body
        Bnd_Box bb;
        BRepBndLib::Add(body.shape(), bb);
        const double R = std::sqrt(bb.SquareExtent()) * 2 + 10;
        const gp_Pnt c = gp_Pnt((bb.CornerMin().XYZ() + bb.CornerMax().XYZ()) * 0.5);
        const gp_Pnt c0 = c.Translated(gp_Vec(pln.Axis().Direction()) * gp_Vec(pln.Location(), c).Dot(gp_Vec(pln.Axis().Direction())) * -1);
        const gp_Pln centered(c0, pln.Axis().Direction());
        NamedShape tool(BRepBuilderAPI_MakeFace(centered, -R, R, -R, R).Face());
        tool.setFaceName(1, m_node.id + ":split");

        BOPAlgo_Splitter sp;
        TopTools_ListOfShape args, tools;
        args.Append(body.shape());
        tools.Append(tool.shape());
        sp.SetArguments(args);
        sp.SetTools(tools);
        sp.SetToFillHistory(true);
        sp.Perform(ctx.progress());
        ctx.throwIfCancelled();
        if (sp.HasErrors())
            throw FeatureError("The split failed");
        const ShapeHistory hist = historyOf(sp.History());

        struct Part
        {
            TopoDS_Shape solid;
            double side;
        };
        std::vector<Part> parts;
        for (TopExp_Explorer ex(sp.Shape(), TopAbs_SOLID); ex.More(); ex.Next()) {
            GProp_GProps g;
            BRepGProp::VolumeProperties(ex.Current(), g);
            if (g.Mass() < 1e-9)
                continue;
            parts.push_back({ex.Current(), gp_Vec(pln.Location(), g.CentreOfMass()).Dot(gp_Vec(pln.Axis().Direction()))});
        }
        if (parts.size() < 2)
            throw FeatureError("The plane does not cut the body");
        // deterministic order: below the plane first (side < 0), then along the normal
        std::sort(parts.begin(), parts.end(), [](const Part& a, const Part& b) { return a.side < b.side; });

        const std::string baseName = ctx.state.bodyNames.count(bodyId) ? ctx.state.bodyNames[bodyId] : "Body";
        Uuid prev = bodyId;
        BRep_Builder bld;
        TopoDS_Compound preview;
        bld.MakeCompound(preview);
        for (size_t k = 0; k < parts.size(); ++k) {
            NamedShape ns = propagateNames(parts[k].solid, {&body, &tool}, hist, m_node.id);
            const std::string cut = m_node.id + ":split";
            for (int i = 1; i <= ns.faceCount(); ++i)  // each half gets its own name for the cut face
                if (ns.faceName(i) == cut)
                    ns.setFaceName(i, cut + "_" + std::to_string(k + 1));
            nameEdgesFromFaces(ns);
            const Uuid id = k == 0 ? bodyId : m_node.id + "#" + std::to_string(k + 1);
            if (k > 0)
                insertBodyAfter(ctx.state, prev, id);
            ctx.state.bodies[id] = std::move(ns);
            ctx.state.bodyNames[id] = baseName + "_" + std::to_string(k + 1);
            prev = id;
            bld.Add(preview, parts[k].solid);
        }
        if (p.pegs) {
            if (parts.size() != 2)
                ctx.warnings.push_back("Alignment pins need a cut into exactly two parts: no pins added");
            else
                addPegs(ctx, p, pln, bodyId, prev);
            bld.MakeCompound(preview);
            bld.Add(preview, ctx.state.bodies.at(bodyId).shape());
            bld.Add(preview, ctx.state.bodies.at(prev).shape());
        }
        if (ctx.wantPreview) {
            ctx.preview = preview;
            ctx.previewOp = BooleanOp::Join;
        }
    }

private:
    /// v0.5 lot 4: N pins on part 1 (protruding through the cut plane) and matching holes (+ clearance)
    /// in part 2. The pins are spread as far apart as possible inside the cut section, keeping a wall of
    /// at least 1.2 mm around the holes.
    void addPegs(ExecContext& ctx, const SplitBodyParams& p, const gp_Pln& pln, const Uuid& lowId, const Uuid& highId) const
    {
        const double r = p.pegDiameter / 2, rh = r + std::max(0.0, p.pegClearance), half = p.pegLength / 2;
        if (r <= 0.05 || half <= 0.1)
            throw FeatureError("The pin diameter and length must be positive");
        const std::string cutName = m_node.id + ":split_1";
        NamedShape& low = ctx.state.bodies.at(lowId);
        std::vector<TopoDS_Face> section;
        for (int i = 1; i <= low.faceCount(); ++i)
            if (low.faceName(i) == cutName)
                section.push_back(TopoDS::Face(low.face(i)));
        if (section.empty())
            throw FeatureError("The cut section was not found for the pins");
        // candidate points on a grid of the section, kept when far enough from its boundary
        const gp_Dir n = pln.Axis().Direction();
        const gp_Dir xd = pln.Position().XDirection(), yd = pln.Position().YDirection();
        Bnd_Box sb;
        for (const auto& f : section)
            BRepBndLib::Add(f, sb);
        const gp_Pnt lo = sb.CornerMin(), hi = sb.CornerMax();
        double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
        for (int k = 0; k < 8; ++k) {
            const gp_Pnt c((k & 1) ? hi.X() : lo.X(), (k & 2) ? hi.Y() : lo.Y(), (k & 4) ? hi.Z() : lo.Z());
            const gp_Vec d(pln.Location(), c);
            u0 = std::min(u0, d.Dot(gp_Vec(xd)));
            u1 = std::max(u1, d.Dot(gp_Vec(xd)));
            v0 = std::min(v0, d.Dot(gp_Vec(yd)));
            v1 = std::max(v1, d.Dot(gp_Vec(yd)));
        }
        const double margin = rh + 1.2;
        const int steps = 36;
        std::vector<gp_Pnt> cands;
        for (int i = 0; i <= steps; ++i)
            for (int j = 0; j <= steps; ++j) {
                const double u = u0 + (u1 - u0) * i / steps, v = v0 + (v1 - v0) * j / steps;
                const gp_Pnt q = pln.Location().Translated(gp_Vec(xd) * u + gp_Vec(yd) * v);
                for (const auto& f : section) {
                    BRepClass_FaceClassifier fc(f, q, 1e-6);
                    if (fc.State() != TopAbs_IN)
                        continue;
                    BRepExtrema_DistShapeShape dist(BRepBuilderAPI_MakeVertex(q).Vertex(), BRepTools::OuterWire(f));
                    bool ok = dist.IsDone() && dist.Value() >= margin;
                    // inner boundaries (holes of the section) too
                    for (TopExp_Explorer w(f, TopAbs_WIRE); ok && w.More(); w.Next()) {
                        BRepExtrema_DistShapeShape dw(BRepBuilderAPI_MakeVertex(q).Vertex(), w.Current());
                        ok = dw.IsDone() && dw.Value() >= margin;
                    }
                    if (ok)
                        cands.push_back(q);
                    break;
                }
            }
        if (cands.empty())
            throw FeatureError("The cut section is too thin for pins of this diameter (try a smaller diameter)");
        // farthest-point selection: spread the pins
        GProp_GProps sp;
        for (const auto& f : section)
            BRepGProp::SurfaceProperties(f, sp);
        const gp_Pnt g = sp.CentreOfMass();
        std::vector<gp_Pnt> pins;
        auto farthest = [&](auto score) {
            size_t best = 0;
            double bs = -1;
            for (size_t i = 0; i < cands.size(); ++i)
                if (const double s = score(cands[i]); s > bs) {
                    bs = s;
                    best = i;
                }
            return cands[best];
        };
        pins.push_back(farthest([&](const gp_Pnt& q) { return q.Distance(g); }));
        const int count = std::clamp(p.pegCount, 1, 8);
        while (int(pins.size()) < count) {
            const gp_Pnt q = farthest([&](const gp_Pnt& c) {
                double m = 1e300;
                for (const auto& pp : pins)
                    m = std::min(m, c.Distance(pp));
                return m;
            });
            if (std::any_of(pins.begin(), pins.end(), [&](const gp_Pnt& x) { return x.Distance(q) < 2 * margin; }))
                break;  // no room for more pins
            pins.push_back(q);
        }
        if (int(pins.size()) < count)
            ctx.warnings.push_back("Only " + std::to_string(pins.size()) + " pin(s) fit in the cut section");

        // pins: from 0.5 mm inside part 1 to `half` above the plane, 0.4 mm chamfer at the tip;
        // holes: Ø + 2 clearance, half + 0.4 mm deep
        auto revolved = [&](const gp_Pnt& c, const std::vector<std::pair<double, double>>& rz) {
            BRepBuilderAPI_MakePolygon poly;
            for (const auto& [rho, z] : rz)
                poly.Add(c.Translated(gp_Vec(xd) * rho + gp_Vec(n) * z));
            poly.Close();
            BRepPrimAPI_MakeRevol rev(BRepBuilderAPI_MakeFace(poly.Wire(), true).Face(), gp_Ax1(c, n), 2 * M_PI);
            return rev.Shape();
        };
        const double ch = std::min(0.4, r * 0.3);
        BRep_Builder bb;
        TopoDS_Compound pinsC, holesC;
        bb.MakeCompound(pinsC);
        bb.MakeCompound(holesC);
        for (const auto& c : pins) {
            bb.Add(pinsC, revolved(c, {{0, -0.5}, {r, -0.5}, {r, half - ch}, {r - ch, half}, {0, half}}));
            bb.Add(holesC, revolved(c, {{0, -0.2}, {rh, -0.2}, {rh, half + 0.4}, {0, half + 0.4}}));
        }
        NamedShape pinTool(pinsC), holeTool(holesC);
        for (int i = 1; i <= pinTool.faceCount(); ++i)
            pinTool.setFaceName(i, m_node.id + ":peg" + std::to_string(i));
        for (int i = 1; i <= holeTool.faceCount(); ++i)
            holeTool.setFaceName(i, m_node.id + ":peghole" + std::to_string(i));
        nameEdgesFromFaces(pinTool);
        nameEdgesFromFaces(holeTool);
        applyBoolean(ctx, pinTool, BooleanOp::Join, lowId);
        applyBoolean(ctx, holeTool, BooleanOp::Cut, highId);
    }
};

// ------------------------------------------------------------------ TransformBody

class OpTransformBody final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<TransformBodyParams>();
        if (p.bodies.empty() && p.mate.is_object() && p.mate.value("mode", std::string{}) == "bed") {
            // lay flat: the body is the owner of the face
            auto r = resolveRef(ctx.state.bodies, p.mate.value("moving", TopoRef{}), TopAbs_FACE, ctx.state.modelSize());
            if (!r)
                throw FeatureError("The face to lay on the bed no longer exists");
            TransformBodyParams q = p;
            q.bodies = {r->owner};
            FeatureNode n = m_node;
            n.params = Json(q);
            OpTransformBody(n).execute(ctx);
            return;
        }
        if (p.bodies.empty())
            throw FeatureError("Select the body (or bodies) to move");
        std::vector<Uuid> ids;
        for (const Uuid& id : p.bodies) {
            if (ctx.state.bodies.count(id))
                ids.push_back(id);
            else
                ctx.warnings.push_back("A body to move no longer exists");
        }
        if (ids.empty())
            throw FeatureError("None of the bodies to move exists anymore");

        gp_Trsf trsf;
        if (!p.mate.is_null() && !p.mate.empty())
            trsf = mateTransform(ctx, p.mate, ids.front());  // lot 1: alignment between bodies
        else {
            Bnd_Box bb;
            for (const Uuid& id : ids)
                BRepBndLib::Add(ctx.state.bodies.at(id).shape(), bb);
            const gp_Pnt c = gp_Pnt((bb.CornerMin().XYZ() + bb.CornerMax().XYZ()) * 0.5);
            gp_Trsf rot;
            const gp_Dir axes[3] = {gp::DX(), gp::DY(), gp::DZ()};
            const double angles[3] = {p.rx, p.ry, p.rz};
            for (int k = 0; k < 3; ++k)
                if (std::abs(angles[k]) > 1e-12) {
                    gp_Trsf r;
                    r.SetRotation(gp_Ax1(c, axes[k]), angles[k] * M_PI / 180.0);
                    rot = r * rot;  // X first, then Y, then Z
                }
            gp_Trsf mv;
            mv.SetTranslation(gp_Vec(p.dx, p.dy, p.dz));
            trsf = mv * rot;
        }

        BRep_Builder bld;
        TopoDS_Compound preview;
        bld.MakeCompound(preview);
        int n = 0;
        for (const Uuid& id : ids) {
            NamedShape moved = ctx.state.bodies.at(id).moved(TopLoc_Location(trsf));
            bld.Add(preview, moved.shape());
            if (!p.copy) {
                ctx.state.bodies[id] = std::move(moved);
                continue;
            }
            // copy: a new body whose names are unique (prefixed by this step)
            for (int i = 1; i <= moved.faceCount(); ++i)
                moved.setFaceName(i, "copy(" + m_node.id + ")/" + moved.faceName(i));
            nameEdgesFromFaces(moved);
            const Uuid cid = m_node.id + "#" + std::to_string(++n);
            insertBodyAfter(ctx.state, ctx.state.bodyOrder.back(), cid);
            ctx.state.bodies[cid] = std::move(moved);
            ctx.state.bodyNames[cid] = (ctx.state.bodyNames.count(id) ? ctx.state.bodyNames[id] : "Body") + " (copy)";
        }
        if (ctx.wantPreview) {
            ctx.preview = preview;
            ctx.previewOp = BooleanOp::Join;
            ctx.previewKeepsInput = true;  // ghost at the new place, the original stays pickable
        }
    }
};

}  // namespace

std::unique_ptr<ParametricFeature> makeOpRemoveBody(const FeatureNode& n) { return std::make_unique<OpRemoveBody>(n); }
std::unique_ptr<ParametricFeature> makeOpDefeature(const FeatureNode& n) { return std::make_unique<OpDefeature>(n); }
std::unique_ptr<ParametricFeature> makeOpSplitBody(const FeatureNode& n) { return std::make_unique<OpSplitBody>(n); }
std::unique_ptr<ParametricFeature> makeOpTransformBody(const FeatureNode& n) { return std::make_unique<OpTransformBody>(n); }

}  // namespace silex::kernel
