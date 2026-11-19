#include "kernel/Feature.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <Bnd_Box.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopExp_Explorer.hxx>

#include "kernel/TopoNaming.h"
#include "kernel/ops/Ops.h"

namespace silex::kernel {

double ModelState::modelSize() const
{
    Bnd_Box box;
    for (const auto& [id, ns] : bodies)
        if (!ns.isNull())
            BRepBndLib::Add(ns.shape(), box);
    for (const auto& [id, sk] : sketches)
        for (const auto& r : sk.regions)
            BRepBndLib::Add(r.named.shape(), box);
    if (box.IsVoid())
        return 100.0;
    const double d = std::sqrt(box.SquareExtent());
    return d > 1e-6 ? d : 100.0;
}

Message_ProgressRange ExecContext::progress()
{
    if (m_progress.IsNull())
        m_progress = new StopTokenProgress(stop);
    return m_progress->Start();
}

void ExecContext::throwIfCancelled() const
{
    if (stop.stop_requested())
        throw FeatureError("cancelled");
}

FeatureRegistry& FeatureRegistry::instance()
{
    static FeatureRegistry r;
    return r;
}

std::unique_ptr<ParametricFeature> FeatureRegistry::create(const FeatureNode& n) const
{
    auto it = m_factories.find(n.type);
    if (it == m_factories.end())
        throw FeatureError("Unknown feature type '" + n.type + "'");
    return it->second(n);
}

void registerBuiltinFeatures()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    auto& r = FeatureRegistry::instance();
    r.add(FeatureType::Sketch, makeOpSketch);
    r.add(FeatureType::Extrude, makeOpExtrude);
    r.add(FeatureType::Revolve, makeOpRevolve);
    r.add(FeatureType::Fillet, makeOpFillet);
    r.add(FeatureType::Chamfer, makeOpChamfer);
    r.add(FeatureType::ImportStep, makeOpImportStep);
    r.add(FeatureType::Shell, makeOpShell);
    r.add(FeatureType::Combine, makeOpCombine);
    r.add(FeatureType::Mirror, makeOpMirror);
    r.add(FeatureType::LinearPattern, makeOpLinearPattern);
    r.add(FeatureType::CircularPattern, makeOpCircularPattern);
}

// ------------------------------------------------------------------ helpers

namespace {
std::set<std::string> splitKey(const std::string& key)
{
    std::set<std::string> out;
    std::stringstream ss(key);
    std::string tok;
    while (std::getline(ss, tok, ','))
        if (!tok.empty())
            out.insert(tok);
    return out;
}
}  // namespace

std::vector<const SketchRegion*> selectRegions(const ExecContext& ctx, const ProfileRef& ref)
{
    auto it = ctx.state.sketches.find(ref.sketch);
    if (it == ctx.state.sketches.end())
        throw FeatureError("The profile sketch is missing or failed");
    const SketchOutput& so = it->second;
    std::vector<const SketchRegion*> out;
    if (ref.regions.empty()) {
        for (const auto& r : so.regions)
            out.push_back(&r);
    }
    else {
        for (const std::string& key : ref.regions) {
            const SketchRegion* found = nullptr;
            for (const auto& r : so.regions)
                if (r.key == key)
                    found = &r;
            if (!found) {
                // Region identity changed (curve added/removed): best Jaccard match.
                const auto want = splitKey(key);
                double best = 0.5;
                for (const auto& r : so.regions) {
                    const auto have = splitKey(r.key);
                    size_t inter = 0;
                    for (const auto& s : want)
                        inter += have.count(s);
                    const double j = static_cast<double>(inter) /
                                     static_cast<double>(want.size() + have.size() - inter);
                    if (j > best) {
                        best = j;
                        found = &r;
                    }
                }
                if (found)
                    const_cast<ExecContext&>(ctx).warnings.push_back("A profile was re-attached to a modified region");
            }
            if (!found)
                throw FeatureError("A selected profile no longer exists in the sketch");
            if (std::find(out.begin(), out.end(), found) == out.end())
                out.push_back(found);
        }
    }
    if (out.empty())
        throw FeatureError("The sketch has no closed profile");
    return out;
}

void checkSolid(const TopoDS_Shape& s, const std::string& what)
{
    if (s.IsNull())
        throw FeatureError(what + " produced no geometry");
    TopExp_Explorer ex(s, TopAbs_SOLID);
    if (!ex.More())
        throw FeatureError(what + " produced an empty result");
    BRepCheck_Analyzer ana(s);
    if (!ana.IsValid())
        throw FeatureError(what + " produced an invalid solid");
}

NamedShape fuseAll(ExecContext& ctx, const std::vector<NamedShape>& tools)
{
    if (tools.empty())
        throw FeatureError("Nothing to build");
    if (tools.size() == 1)
        return tools.front();
    TopTools_ListOfShape args, others;
    args.Append(tools.front().shape());
    for (size_t i = 1; i < tools.size(); ++i)
        others.Append(tools[i].shape());
    BRepAlgoAPI_Fuse fuse;
    fuse.SetArguments(args);
    fuse.SetTools(others);
    fuse.Build(ctx.progress());
    ctx.throwIfCancelled();
    if (!fuse.IsDone() || fuse.HasErrors())
        throw FeatureError("Could not merge the profiles");
    std::vector<const NamedShape*> in;
    for (const auto& t : tools)
        in.push_back(&t);
    return propagateNames(fuse.Shape(), in, historyOf(fuse), ctx.node.id);
}

Uuid applyBoolean(ExecContext& ctx, const NamedShape& tool, BooleanOp op, const Uuid& targetBody)
{
    ModelState& st = ctx.state;
    const Uuid& fid = ctx.node.id;

    Uuid target = targetBody;
    if (!target.empty() && !st.bodies.count(target)) {
        ctx.warnings.push_back("Target body not found: using the last body");
        target.clear();
    }
    if (target.empty() && !st.bodyOrder.empty())
        target = st.bodyOrder.back();

    if (op == BooleanOp::NewBody || target.empty()) {
        if (op == BooleanOp::Cut || op == BooleanOp::Intersect)
            throw FeatureError("There is no body to cut or intersect");
        checkSolid(tool.shape(), "The feature");
        st.bodies[fid] = tool;
        st.bodyOrder.push_back(fid);
        st.bodyNames[fid] = "Body" + std::to_string(st.bodyOrder.size());
        return fid;
    }

    NamedShape& base = st.bodies[target];
    TopTools_ListOfShape args, tools;
    args.Append(base.shape());
    tools.Append(tool.shape());

    std::unique_ptr<BRepAlgoAPI_BooleanOperation> alg;
    switch (op) {
        case BooleanOp::Cut: alg = std::make_unique<BRepAlgoAPI_Cut>(); break;
        case BooleanOp::Intersect: alg = std::make_unique<BRepAlgoAPI_Common>(); break;
        default: alg = std::make_unique<BRepAlgoAPI_Fuse>(); break;
    }
    alg->SetArguments(args);
    alg->SetTools(tools);
    alg->Build(ctx.progress());
    ctx.throwIfCancelled();
    if (!alg->IsDone() || alg->HasErrors())
        throw FeatureError("The boolean operation failed");

    NamedShape raw = propagateNames(alg->Shape(), {&base, &tool}, historyOf(*alg), fid);

    // Merge coplanar / co-cylindrical faces like commercial modelers do.
    ShapeUpgrade_UnifySameDomain usd(raw.shape(), Standard_True, Standard_True, Standard_False);
    usd.Build();
    NamedShape unified = propagateNames(usd.Shape(), {&raw}, historyOf(usd.History()), fid);

    const char* what = op == BooleanOp::Cut         ? "The cut"
                       : op == BooleanOp::Intersect ? "The intersection"
                                                    : "The join";
    checkSolid(unified.shape(), what);
    base = std::move(unified);
    return target;
}

}  // namespace silex::kernel
