// OpImportStep: a body from an imported STEP file. The B-Rep is embedded in the document
// (OCCT text format) so the project stays self-contained, offline and diffable.

#include <sstream>

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <TopExp_Explorer.hxx>

#include "kernel/TopoNaming.h"
#include "kernel/ops/Ops.h"

namespace silex::kernel {
namespace {

class OpImportStep final : public ParametricFeature
{
public:
    using ParametricFeature::ParametricFeature;

    void execute(ExecContext& ctx) const override
    {
        const auto p = m_node.as<ImportParams>();
        if (p.brep.empty())
            throw FeatureError("The imported geometry is empty");
        std::istringstream in(p.brep);
        TopoDS_Shape shape;
        BRep_Builder builder;
        BRepTools::Read(shape, in, builder);
        if (shape.IsNull())
            throw FeatureError("Could not read the embedded B-Rep");

        NamedShape ns(shape);
        for (int i = 1; i <= ns.faceCount(); ++i)
            ns.setFaceName(i, m_node.id + ":f" + std::to_string(i));
        nameEdgesFromFaces(ns);

        TopExp_Explorer solids(shape, TopAbs_SOLID);
        if (!solids.More())
            ctx.warnings.push_back("The imported file contains no solid (surfaces only)");

        ctx.state.bodies[m_node.id] = std::move(ns);
        ctx.state.bodyOrder.push_back(m_node.id);
        ctx.state.bodyNames[m_node.id] = m_node.name;
    }
};

}  // namespace

std::unique_ptr<ParametricFeature> makeOpImportStep(const FeatureNode& n)
{
    return std::make_unique<OpImportStep>(n);
}

}  // namespace silex::kernel
