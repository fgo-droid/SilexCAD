// OpLinearPattern / OpCircularPattern: repeated copies of bodies or feature tools.
// Every occurrence is indexed in its TNP names so that downstream features (fillets, chamfers)
// on existing occurrences survive a change of the number of occurrences:
//   pattern_lin(<featId>,<i>,<j>,<source name>)      pattern_circ(<featId>,<k>,<source name>)

#include <cmath>

#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include "kernel/GeomRefs.h"
#include "kernel/ops/Ops.h"

namespace silex::kernel {
namespace {

std::vector<int> indices(const PatternDirection& d)
{
    std::vector<int> out;
    if (d.symmetric)
        for (int i = -(d.count - 1); i <= d.count - 1; ++i)
            out.push_back(i);
    else
        for (int i = 0; i < d.count; ++i)
            out.push_back(i);
    return out;
}

class OpLinearPattern final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<LinearPatternParams>();
        if (p.dir1.count < 2)
            throw FeatureError("The number of occurrences must be at least 2");
        if (p.dir1.spacing <= 0 || (p.dir2.count > 1 && p.dir2.spacing <= 0))
            throw FeatureError("The spacing must be positive");
        if (p.dir1.count * std::max(1, p.dir2.count) > 2000)
            throw FeatureError("Too many occurrences (2000 max)");

        gp_Vec d1(resolveDirection(ctx, p.dir1.ref));
        d1 *= p.dir1.spacing * (p.dir1.reverse ? -1.0 : 1.0);
        gp_Vec d2(0, 0, 0);
        PatternDirection dir2 = p.dir2;
        if (dir2.count > 1) {
            d2 = gp_Vec(resolveDirection(ctx, dir2.ref));
            d2 *= dir2.spacing * (dir2.reverse ? -1.0 : 1.0);
        }
        else {
            dir2.count = 1;
            dir2.symmetric = false;
        }

        std::vector<gp_Trsf> trsfs;
        std::vector<std::string> tags;
        for (int i : indices(p.dir1))
            for (int j : indices(dir2)) {
                if (i == 0 && j == 0)
                    continue;  // the original
                gp_Trsf t;
                t.SetTranslation(d1 * i + d2 * j);
                trsfs.push_back(t);
                tags.push_back("pattern_lin(" + m_node.id + "," + std::to_string(i) + "," + std::to_string(j) + ",");
            }
        replicateTargets(ctx, p.targets, trsfs, tags);
    }
};

class OpCircularPattern final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<CircularPatternParams>();
        if (p.count < 2)
            throw FeatureError("The number of occurrences must be at least 2");
        if (p.count > 2000)
            throw FeatureError("Too many occurrences (2000 max)");
        if (std::abs(p.totalAngle) < 1e-9)
            throw FeatureError("The angle must not be zero");

        const gp_Ax1 axis = resolveAxis(ctx, p.axis);
        const bool full = std::abs(std::abs(p.totalAngle) - 360.0) < 1e-9;
        double step = p.equalSpacing ? (full ? p.totalAngle / p.count : p.totalAngle / (p.count - 1)) : p.totalAngle;
        if (p.reverse)
            step = -step;

        std::vector<gp_Trsf> trsfs;
        std::vector<std::string> tags;
        for (int k = 1; k < p.count; ++k) {
            gp_Trsf t;
            t.SetRotation(axis, k * step * kPi / 180.0);
            trsfs.push_back(t);
            tags.push_back("pattern_circ(" + m_node.id + "," + std::to_string(k) + ",");
        }
        replicateTargets(ctx, p.targets, trsfs, tags);
    }
};

}  // namespace

std::unique_ptr<ParametricFeature> makeOpLinearPattern(const FeatureNode& n)
{
    return std::make_unique<OpLinearPattern>(n);
}
std::unique_ptr<ParametricFeature> makeOpCircularPattern(const FeatureNode& n)
{
    return std::make_unique<OpCircularPattern>(n);
}

}  // namespace silex::kernel
