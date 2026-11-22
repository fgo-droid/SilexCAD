#pragma once
// Lightweight per-body material: display colour (RGBA, alpha < 1 = translucent body) and density
// used for the estimated mass. No PBR shading on purpose (cheap, readable assemblies).

#include <string>
#include <vector>

#include "model/Json.h"

namespace silex {

struct BodyMaterial
{
    std::string name = "PLA Blanc";
    float r = 0.93f, g = 0.93f, b = 0.91f, a = 1.0f;  ///< 0..1
    double density = 1.24;                             ///< g/cm3

    bool operator==(const BodyMaterial&) const = default;
};

/// Default material of a body without assignment (PLA, 1.24 g/cm3).
BodyMaterial defaultMaterial();
/// Built-in presets: PLA Blanc, PETG Noir, ABS Gris, Résine Translucide, PCB Vert, Aluminium, Acier.
const std::vector<BodyMaterial>& materialPresets();
/// Preset by (case-insensitive, accent-tolerant) name or id ("pla", "petg", "abs", "resin", "pcb",
/// "aluminium", "steel"...). Returns nullptr when unknown.
const BodyMaterial* findMaterialPreset(const std::string& nameOrId);

/// Mass in grams of `volumeMm3` mm3 of material.
inline double massGrams(double volumeMm3, double densityGcm3) { return volumeMm3 * 1e-3 * densityGcm3; }

void to_json(Json& j, const BodyMaterial& m);
void from_json(const Json& j, BodyMaterial& m);

}  // namespace silex
