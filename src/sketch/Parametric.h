#pragma once
// Cascade update of the document when global parameters (or bound fields) change:
// parameters are evaluated, bound feature fields and sketch dimensions are rewritten and the
// affected sketches are re-solved (PlaneGCS). The timeline then recomputes only from the first
// modified feature (incremental cache of the kernel).

#include "model/Bindings.h"
#include "model/Document.h"

namespace silex {

/// applyFeatureBindings + sketch solving. Never throws (errors are left in the parameter table).
DocState applyParameters(const DocState& s);

/// Wraps any command so that its result is always parametrically consistent.
class ParametricCommand final : public Command
{
public:
    explicit ParametricCommand(const Command& inner) : m_inner(inner) {}
    std::string label() const override { return m_inner.label(); }
    DocState apply(const DocState& s) const override { return applyParameters(m_inner.apply(s)); }

private:
    const Command& m_inner;
};

}  // namespace silex
