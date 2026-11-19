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

}  // namespace silex::kernel
