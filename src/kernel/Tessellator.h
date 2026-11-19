#pragma once
// B-Rep -> display mesh (triangles tagged by face, edge polylines tagged by edge).
// Runs on the recompute worker thread: the UI thread never tessellates.

#include <TopoDS_Shape.hxx>

#include "kernel/NamedShape.h"
#include "model/RenderModel.h"

namespace silex::kernel {

struct TessellationParams
{
    double linearDeflection = 0.05;   ///< absolute (mm)
    double angularDeflection = 0.35;  ///< radians
    bool withEdges = true;
};

/// Deflection adapted to the model size (fine for small parts, bounded for large ones).
TessellationParams tessellationFor(double modelSize);

RenderMesh tessellate(const NamedShape& ns, const TessellationParams& p);
RenderMesh tessellate(const TopoDS_Shape& s, const TessellationParams& p);

}  // namespace silex::kernel
