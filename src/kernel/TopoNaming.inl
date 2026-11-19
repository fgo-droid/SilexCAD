#pragma once
// Template implementation of resolveRef (bodies = any range of pair<id, NamedShape>).

#include <limits>

#include <TopoDS.hxx>

namespace silex::kernel {

template<typename BodyRange>
std::optional<ResolvedRef> resolveRef(const BodyRange& bodies, const TopoRef& ref, TopAbs_ShapeEnum type,
                                      double modelSize)
{
    const bool faces = type == TopAbs_FACE;
    auto sigOf = [&](const TopoDS_Shape& s) {
        return faces ? faceSignature(TopoDS::Face(s)) : edgeSignature(TopoDS::Edge(s));
    };

    // 1) exact name match (disambiguated by signature)
    std::optional<ResolvedRef> best;
    double bestScore = std::numeric_limits<double>::max();
    for (const auto& [id, ns] : bodies) {
        const int n = faces ? ns.faceCount() : ns.edgeCount();
        for (int i = 1; i <= n; ++i) {
            const bool match = faces ? ns.faceHasName(i, ref.name) : ns.edgeName(i) == ref.name;
            if (!match)
                continue;
            const TopoDS_Shape& s = faces ? ns.face(i) : ns.edge(i);
            const double score = ref.sig.distance(sigOf(s), modelSize);
            if (score < bestScore) {
                bestScore = score;
                best = ResolvedRef{s, id, true};
            }
        }
    }
    if (best)
        return best;

    // 2) geometric fallback (the named entity disappeared upstream)
    for (const auto& [id, ns] : bodies) {
        const int n = faces ? ns.faceCount() : ns.edgeCount();
        for (int i = 1; i <= n; ++i) {
            const TopoDS_Shape& s = faces ? ns.face(i) : ns.edge(i);
            const double score = ref.sig.distance(sigOf(s), modelSize);
            if (score < bestScore) {
                bestScore = score;
                best = ResolvedRef{s, id, false};
            }
        }
    }
    if (best && bestScore < 0.05)
        return best;
    return std::nullopt;
}

}  // namespace silex::kernel
