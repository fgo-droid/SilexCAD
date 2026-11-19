#pragma once
#include <string>

#include <TopoDS_Shape.hxx>

namespace silex::kernel {

/// Reads a STEP file into a single shape (throws std::runtime_error on failure).
TopoDS_Shape readStep(const std::string& path);
/// Writes shapes to STEP AP214 (throws std::runtime_error on failure).
void writeStep(const TopoDS_Shape& shape, const std::string& path);

std::string shapeToBrep(const TopoDS_Shape& s);
TopoDS_Shape brepToShape(const std::string& brep);

}  // namespace silex::kernel
