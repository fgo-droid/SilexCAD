#include "kernel/Recompute.h"

#include <chrono>
#include <functional>

#include <Standard_Failure.hxx>

namespace silex::kernel {

Recompute::Recompute() { registerBuiltinFeatures(); }

namespace {
size_t hashCombine(size_t seed, size_t v) { return seed ^ (v + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2)); }

size_t featureHash(const FeatureNode& f)
{
    size_t h = std::hash<std::string>{}(f.id);
    h = hashCombine(h, std::hash<std::string>{}(f.type));
    h = hashCombine(h, std::hash<std::string>{}(f.params.dump()));
    h = hashCombine(h, f.suppressed ? 1u : 0u);
    return h;
}
}  // namespace

ComputeOutput Recompute::run(const DocState& doc, const Uuid& previewFeature, std::stop_token stop)
{
    ComputeOutput out;
    size_t key = 0x5113;
    bool cacheValid = true;
    const size_t n = doc.features.size();

    for (size_t i = 0; i < n; ++i) {
        const FeatureNode& node = *doc.features[i];
        FeatureResult res;
        res.id = node.id;

        if (static_cast<int>(i) >= doc.marker) {
            res.status = FeatureStatus::RolledBack;
            out.results.push_back(res);
            continue;
        }
        if (stop.stop_requested()) {
            out.cancelled = true;
            return out;
        }

        const bool isPreview = !previewFeature.empty() && node.id == previewFeature;
        key = hashCombine(key, featureHash(node));

        // Incremental recompute: reuse the cached state of an identical prefix.
        if (cacheValid && !isPreview && i < m_cache.size() && m_cache[i].key == key) {
            out.state = m_cache[i].state;
            out.results.push_back(m_cache[i].result);
            continue;
        }
        cacheValid = false;

        if (node.suppressed) {
            res.status = FeatureStatus::Suppressed;
        }
        else {
            const auto t0 = std::chrono::steady_clock::now();
            ModelState next = out.state;  // shapes are shared handles: cheap copy
            ExecContext ctx{next, node, stop, isPreview};
            try {
                auto feature = FeatureRegistry::instance().create(node);
                feature->execute(ctx);
                if (!(isPreview && ctx.previewKeepsInput))
                    out.state = std::move(next);
                res.status = ctx.warnings.empty() ? FeatureStatus::Ok : FeatureStatus::Warning;
                for (const auto& w : ctx.warnings)
                    res.message += (res.message.empty() ? "" : "\n") + w;
                if (isPreview) {
                    out.preview = ctx.preview;
                    out.previewOp = ctx.previewOp;
                }
            }
            catch (const FeatureError& e) {
                res.status = FeatureStatus::Error;
                res.message = e.what();
            }
            catch (const Standard_Failure& e) {
                res.status = FeatureStatus::Error;
                res.message = std::string("Geometry kernel error: ") +
                              (e.GetMessageString() && *e.GetMessageString() ? e.GetMessageString()
                                                                             : "unknown OCCT failure");
            }
            catch (const std::exception& e) {
                res.status = FeatureStatus::Error;
                res.message = e.what();
            }
            catch (...) {
                res.status = FeatureStatus::Error;
                res.message = "Unknown error";
            }
            if (stop.stop_requested()) {
                out.cancelled = true;
                return out;
            }
            res.milliseconds =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
        out.results.push_back(res);

        if (!isPreview) {
            if (m_cache.size() <= i)
                m_cache.resize(i + 1);
            m_cache[i] = CacheEntry{key, out.state, res};
            m_cache.resize(i + 1);  // everything after an invalidated entry is stale
        }
    }
    return out;
}

}  // namespace silex::kernel
