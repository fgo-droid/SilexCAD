#pragma once
// v0.3 analysis services on a computed model state (measure, interference, mass properties).

#include <vector>

#include <TopoDS_Shape.hxx>

#include "kernel/Feature.h"
#include "kernel/KernelEngine.h"

namespace silex::kernel {

MeasureResult measureIn(const ModelState& st, const std::vector<MeasureRef>& refs);
InterferenceReport interferenceIn(const ModelState& st, const std::vector<Uuid>& bodies, bool meshes);

struct MassProps
{
    double volume = 0, area = 0;
};
MassProps massProps(const TopoDS_Shape& s);

}  // namespace silex::kernel
