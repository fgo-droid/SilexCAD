#include "kernel/KernelEngine.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stop_token>
#include <thread>

#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <Bnd_Box.hxx>
#include <StlAPI_Writer.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>

#include "kernel/Recompute.h"
#include "kernel/StepIO.h"
#include "kernel/Tessellator.h"
#include "kernel/TopoNaming.h"

namespace silex {

using namespace kernel;

namespace {

struct MeshCacheEntry
{
    TopoDS_Shape shape;
    RenderMesh mesh;
    uint64_t version = 0;
};

struct MeshCache
{
    std::map<Uuid, MeshCacheEntry> entries;
    uint64_t nextVersion = 1;
};

std::shared_ptr<ModelSnapshot> buildSnapshot(const ComputeOutput& out, MeshCache* cache)
{
    auto snap = std::make_shared<ModelSnapshot>();
    const TessellationParams tp = tessellationFor(out.state.modelSize());

    std::map<Uuid, MeshCacheEntry> nextEntries;
    for (const Uuid& id : out.state.bodyOrder) {
        const NamedShape& ns = out.state.bodies.at(id);
        BodyView bv;
        bv.id = id;
        auto nameIt = out.state.bodyNames.find(id);
        bv.name = nameIt != out.state.bodyNames.end() ? nameIt->second : "Body";
        if (cache) {
            auto it = cache->entries.find(id);
            if (it != cache->entries.end() && it->second.shape.IsEqual(ns.shape())) {
                bv.mesh = it->second.mesh;
                bv.meshVersion = it->second.version;
            }
            else {
                bv.mesh = tessellate(ns, tp);
                bv.meshVersion = cache->nextVersion++;
            }
            nextEntries[id] = MeshCacheEntry{ns.shape(), bv.mesh, bv.meshVersion};
        }
        else {
            bv.mesh = tessellate(ns, tp);
            static std::atomic<uint64_t> uncachedVersion{1ull << 40};  // never reused: forces re-upload
            bv.meshVersion = uncachedVersion++;
        }
        snap->bbox.add(bv.mesh.bbox);
        snap->bodies.push_back(std::move(bv));
    }
    if (cache)
        cache->entries = std::move(nextEntries);

    TessellationParams flat = tp;
    flat.withEdges = false;
    for (const auto& [sid, so] : out.state.sketches) {
        snap->sketches.push_back(SketchView{sid, so.plane, true});
        for (const auto& r : so.regions) {
            ProfileView pv;
            pv.sketchId = sid;
            pv.regionKey = r.key;
            pv.mesh = tessellate(r.named, flat);
            if (!pv.mesh.faces.empty())
                pv.centroid = pv.mesh.faces.front().sig.center;
            snap->bbox.add(pv.mesh.bbox);
            snap->profiles.push_back(std::move(pv));
        }
    }

    if (out.preview && !out.preview->IsNull()) {
        snap->preview = tessellate(*out.preview, flat);
        snap->previewOp = out.previewOp;
    }
    snap->results = out.results;
    return snap;
}

}  // namespace

struct KernelEngine::Impl
{
    struct Job
    {
        DocState state;
        Uuid preview;
        uint64_t revision = 0;
    };

    Callback callback;
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<Job> pending;
    std::stop_source current;
    bool quit = false;
    Recompute recompute;
    MeshCache meshCache;
    std::thread thread;

    void loop()
    {
        for (;;) {
            Job job;
            std::stop_token token;
            {
                std::unique_lock lk(mutex);
                cv.wait(lk, [&] { return quit || pending.has_value(); });
                if (quit)
                    return;
                job = std::move(*pending);
                pending.reset();
                current = std::stop_source();
                token = current.get_token();
            }
            try {
                const auto t0 = std::chrono::steady_clock::now();
                ComputeOutput out = recompute.run(job.state, job.preview, token);
                if (out.cancelled || token.stop_requested())
                    continue;
                auto snap = buildSnapshot(out, &meshCache);
                if (token.stop_requested())
                    continue;
                snap->revision = job.revision;
                snap->computeMs =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                callback(std::move(snap));
            }
            catch (const Standard_Failure&) {
                recompute.clearCache();
            }
            catch (const std::exception&) {
                recompute.clearCache();
            }
        }
    }
};

KernelEngine::KernelEngine(Callback onResult) : d(std::make_unique<Impl>())
{
    d->callback = std::move(onResult);
    d->thread = std::thread([this] { d->loop(); });
}

KernelEngine::~KernelEngine()
{
    {
        std::lock_guard lk(d->mutex);
        d->quit = true;
        d->current.request_stop();
    }
    d->cv.notify_all();
    if (d->thread.joinable())
        d->thread.join();
}

void KernelEngine::submit(const DocState& state, const Uuid& previewFeature, uint64_t revision)
{
    {
        std::lock_guard lk(d->mutex);
        d->pending = Impl::Job{state, previewFeature, revision};
        d->current.request_stop();  // cancel the computation in flight
    }
    d->cv.notify_one();
}

std::shared_ptr<const ModelSnapshot> KernelEngine::computeSnapshot(const DocState& state, const Uuid& previewFeature)
{
    Recompute r;
    ComputeOutput out = r.run(state, previewFeature, {});
    return buildSnapshot(out, nullptr);
}

Analysis KernelEngine::analyze(const DocState& state)
{
    Recompute r;
    ComputeOutput out = r.run(state, {}, {});
    Analysis a;
    a.results = out.results;
    for (const Uuid& id : out.state.bodyOrder) {
        const NamedShape& ns = out.state.bodies.at(id);
        BodyMetrics m;
        m.id = id;
        m.name = out.state.bodyNames.count(id) ? out.state.bodyNames.at(id) : "";
        GProp_GProps vp, sp;
        BRepGProp::VolumeProperties(ns.shape(), vp);
        BRepGProp::SurfaceProperties(ns.shape(), sp);
        m.volume = vp.Mass();
        m.area = sp.Mass();
        const gp_Pnt cg = vp.CentreOfMass();
        m.centerOfMass = {cg.X(), cg.Y(), cg.Z()};
        Bnd_Box box;
        BRepBndLib::AddOptimal(ns.shape(), box, false, false);
        if (!box.IsVoid()) {
            double x0, y0, z0, x1, y1, z1;
            box.Get(x0, y0, z0, x1, y1, z1);
            m.bbox.add(Vec3{x0, y0, z0});
            m.bbox.add(Vec3{x1, y1, z1});
        }
        m.valid = BRepCheck_Analyzer(ns.shape()).IsValid();
        for (TopExp_Explorer ex(ns.shape(), TopAbs_SOLID); ex.More(); ex.Next()) {
            ++m.solids;
            SolidMetrics sm;
            GProp_GProps p;
            BRepGProp::VolumeProperties(ex.Current(), p);
            sm.volume = p.Mass();
            const gp_Pnt c = p.CentreOfMass();
            sm.center = {c.X(), c.Y(), c.Z()};
            Bnd_Box sb;
            BRepBndLib::AddOptimal(ex.Current(), sb, false, false);
            if (!sb.IsVoid()) {
                double x0, y0, z0, x1, y1, z1;
                sb.Get(x0, y0, z0, x1, y1, z1);
                sm.bbox.add(Vec3{x0, y0, z0});
                sm.bbox.add(Vec3{x1, y1, z1});
            }
            m.solidList.push_back(sm);
        }
        m.faces = ns.faceCount();
        m.edges = ns.edgeCount();
        for (int i = 1; i <= ns.faceCount(); ++i)
            m.faceNames.push_back(ns.faceName(i));
        for (int i = 1; i <= ns.edgeCount(); ++i)
            m.edgeNames.push_back(ns.edgeName(i));
        a.bodies.push_back(std::move(m));
    }
    return a;
}

namespace {
/// Compound of every body (or of one body). Throws std::runtime_error.
TopoDS_Compound bodiesCompound(const ComputeOutput& out, const Uuid& bodyId)
{
    BRep_Builder b;
    TopoDS_Compound c;
    b.MakeCompound(c);
    int n = 0;
    for (const Uuid& id : out.state.bodyOrder)
        if (bodyId.empty() || id == bodyId) {
            b.Add(c, out.state.bodies.at(id).shape());
            ++n;
        }
    if (n == 0)
        throw std::runtime_error(bodyId.empty() ? "There is no body to export" : "Unknown body id: " + bodyId);
    return c;
}
}  // namespace

std::optional<std::string> KernelEngine::exportStl(const DocState& state, const std::string& path,
                                                   const Uuid& bodyId, double linearDeflection)
{
    try {
        Recompute r;
        ComputeOutput out = r.run(state, {}, {});
        const TopoDS_Compound c = bodiesCompound(out, bodyId);
        BRepMesh_IncrementalMesh mesh(c, std::max(linearDeflection, 1e-4), false, 0.3, true);
        StlAPI_Writer w;
        w.ASCIIMode() = false;
        if (!w.Write(c, path.c_str()))
            return std::string("Cannot write STL file: ") + path;
        return std::nullopt;
    }
    catch (const Standard_Failure& e) {
        return std::string("Geometry kernel error: ") + (e.GetMessageString() ? e.GetMessageString() : "unknown");
    }
    catch (const std::exception& e) {
        return std::string(e.what());
    }
}

std::optional<std::string> KernelEngine::exportStep(const DocState& state, const std::string& path, const Uuid& bodyId)
{
    try {
        Recompute r;
        ComputeOutput out = r.run(state, {}, {});
        writeStep(bodiesCompound(out, bodyId), path);
        return std::nullopt;
    }
    catch (const Standard_Failure& e) {
        return std::string("Geometry kernel error: ") + (e.GetMessageString() ? e.GetMessageString() : "unknown");
    }
    catch (const std::exception& e) {
        return std::string(e.what());
    }
}

std::string KernelEngine::importStepAsBrep(const std::string& path)
{
    try {
        return shapeToBrep(readStep(path));
    }
    catch (const Standard_Failure& e) {
        throw std::runtime_error(std::string("Geometry kernel error: ") + (e.GetMessageString() ? e.GetMessageString() : "unknown"));
    }
}

}  // namespace silex
