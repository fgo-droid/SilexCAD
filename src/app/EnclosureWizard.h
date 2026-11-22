#pragma once
// v0.5 lot 4 — "Enclosure in 3 clicks". Instead of an opaque block, the wizard writes a clean,
// commented timeline the maker can inspect and edit step by step:
//   parameters (int_longueur, int_largeur, int_hauteur, paroi, rayon_coins) -> Box -> Fillet (corners)
//   -> Shell -> Split (box / lid) -> closure: snap-fit lip, 4 M3 screw posts with heat-set inserts,
//   or a sliding lid in grooves.
// Every step carries a bilingual annotation (guided tour) and its sizes are bound to the parameters.

#include <optional>
#include <string>

#include "model/Document.h"

class QWidget;

namespace silex::ui {

struct EnclosureSpec
{
    double length = 80, width = 50, height = 30;  ///< inner dimensions (mm)
    double wall = 2.0;
    double cornerRadius = 3.0;                     ///< outer corner radius (0 = sharp)
    std::string closure = "lip";                   ///< "lip" | "screws" | "slide"
    double clearance = 0.2;                        ///< FDM play between box and lid
};

struct EnclosureResult
{
    CommandPtr command;  ///< one undo step: parameters + every feature
    Uuid baseBody, lidBody;
    int featureCount = 0;
};

/// Builds the commands (computing the intermediate states to pick edges / faces). nullopt + error.
std::optional<EnclosureResult> buildEnclosure(const DocState& doc, const EnclosureSpec& spec, std::string* error);

/// Modal dialog asking for the spec (nullopt = cancelled).
std::optional<EnclosureSpec> askEnclosureSpec(QWidget* parent, bool french);

}  // namespace silex::ui
