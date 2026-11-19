#pragma once
// Public, OCCT-free facade of the geometric core.
//
// KernelEngine owns a dedicated worker thread. submit() never blocks: a newer request
// cancels the one in flight (std::stop_token => OCCT progress user-break) so that dragging
// a value recomputes "at the speed of the hand" without queuing stale work.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "model/Document.h"
#include "model/RenderModel.h"

namespace silex {

struct SolidMetrics
{
    double volume = 0;
    Vec3 center;
    BBox bbox;
};

struct BodyMetrics
{
    std::vector<SolidMetrics> solidList;  ///< per solid (useful for imported assemblies)
    Uuid id;
    std::string name;
    double volume = 0;
    double area = 0;
    Vec3 centerOfMass;
    BBox bbox;
    bool valid = false;  ///< BRepCheck_Analyzer
    int solids = 0, faces = 0, edges = 0;
    std::vector<std::string> faceNames, edgeNames;
};

struct Analysis
{
    std::vector<FeatureResult> results;
    std::vector<BodyMetrics> bodies;
    const FeatureResult* result(const Uuid& id) const
    {
        for (const auto& r : results)
            if (r.id == id)
                return &r;
        return nullptr;
    }
};

class KernelEngine
{
public:
    using Callback = std::function<void(std::shared_ptr<const ModelSnapshot>)>;

    /// `onResult` is invoked on the worker thread.
    explicit KernelEngine(Callback onResult);
    ~KernelEngine();
    KernelEngine(const KernelEngine&) = delete;
    KernelEngine& operator=(const KernelEngine&) = delete;

    /// Asynchronous recompute. `previewFeature` (may be empty) gets its tool body returned
    /// in ModelSnapshot::preview for transparent display.
    void submit(const DocState& state, const Uuid& previewFeature, uint64_t revision);

    // ---- synchronous services (any thread) ------------------------------------------------
    static std::shared_ptr<const ModelSnapshot> computeSnapshot(const DocState& state,
                                                                const Uuid& previewFeature = {});
    static Analysis analyze(const DocState& state);
    /// Exports every body (or only `bodyId`). Returns an error message on failure.
    static std::optional<std::string> exportStep(const DocState& state, const std::string& path,
                                                 const Uuid& bodyId = {});
    static std::optional<std::string> exportStl(const DocState& state, const std::string& path,
                                                const Uuid& bodyId = {}, double linearDeflection = 0.05);
    /// Reads a STEP file and returns the shape as an embeddable B-Rep string.
    static std::string importStepAsBrep(const std::string& path);  // throws std::runtime_error

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace silex
