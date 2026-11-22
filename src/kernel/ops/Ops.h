#pragma once
// Factories of the built-in operations (each lives in its own translation unit).

#include <memory>

#include "kernel/Feature.h"

namespace silex::kernel {

std::unique_ptr<ParametricFeature> makeOpSketch(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpExtrude(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpRevolve(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpFillet(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpChamfer(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpImportStep(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpShell(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpCombine(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpMirror(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpLinearPattern(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpCircularPattern(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpSweep(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpLoft(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpOffsetFace(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpBox(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpCylinder(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpRemoveBody(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpDefeature(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpSplitBody(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpTransformBody(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpConstructionPlane(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpRib(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpThread(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpFastenerHole(const FeatureNode& n);
std::unique_ptr<ParametricFeature> makeOpText(const FeatureNode& n);

/// Plane of a SketchPlaneRef for direct features (on a face: origin at the face centroid,
/// X = world X projected on the face).
Placement resolveBasePlane(ExecContext& ctx, const SketchPlaneRef& ref);

}  // namespace silex::kernel
