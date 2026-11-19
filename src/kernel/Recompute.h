#pragma once
// Timeline evaluation with an incremental cache (unchanged prefixes are not recomputed)
// and cooperative cancellation.

#include <cstddef>
#include <optional>
#include <stop_token>
#include <vector>

#include <TopoDS_Shape.hxx>

#include "kernel/Feature.h"
#include "model/Document.h"
#include "model/RenderModel.h"

namespace silex::kernel {

struct ComputeOutput
{
    ModelState state;
    std::vector<FeatureResult> results;
    std::optional<TopoDS_Shape> preview;
    BooleanOp previewOp = BooleanOp::NewBody;
    bool cancelled = false;
};

class Recompute
{
public:
    Recompute();
    ComputeOutput run(const DocState& doc, const Uuid& previewFeature, std::stop_token stop);
    void clearCache() { m_cache.clear(); }

private:
    struct CacheEntry
    {
        size_t key = 0;
        ModelState state;
        FeatureResult result;
    };
    std::vector<CacheEntry> m_cache;
};

}  // namespace silex::kernel
