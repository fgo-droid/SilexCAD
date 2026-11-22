#pragma once
// v0.5 lot 1 — built-in library of electronic components (resources/components/<id>.json + .step).
// A component is inserted as an ImportStep step marked `component` + `locked`: a reference body
// (pre-assigned material) that can be moved / aligned and projected in sketches, but never modified,
// printed or exported with the parts.

#include <optional>
#include <string>
#include <vector>

#include "model/Document.h"
#include "model/Json.h"

namespace silex::ui {

struct ComponentHole
{
    double x = 0, y = 0, d = 0;  ///< position in the component frame, diameter
};

struct ComponentInfo
{
    std::string id, category, step, material, screw;
    std::string nameFr, nameEn, descFr, descEn;
    double standoff = 0;  ///< recommended standoff height (mm)
    std::vector<ComponentHole> holes;
    Json raw;             ///< the whole catalog entry (openings, pcb...)

    std::string name(bool french) const { return french ? nameFr : nameEn; }
    std::string description(bool french) const { return french ? descFr : descEn; }
};

class ComponentLibrary
{
public:
    /// Catalog read once from ":/components/*.json", sorted by category then name.
    static const std::vector<ComponentInfo>& all();
    static const ComponentInfo* find(const std::string& id);
    /// The Import step of a component (embedded B-Rep, locked, material). nullopt + error on failure.
    static std::optional<FeaturePtr> makeFeature(const DocState& doc, const std::string& id, bool french,
                                                 std::string* error = nullptr);
};

}  // namespace silex::ui
