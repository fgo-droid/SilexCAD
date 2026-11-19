#pragma once
#include <QIcon>

namespace silex::ui {

enum class IconId {
    New, Open, Save, Undo, Redo, ImportStep, ExportStep,
    Sketch, Extrude, Revolve, Fillet, Chamfer, Finish, Cancel,
    Line, Rectangle, Circle, Arc, Dimension, Construction,
    Horizontal, Vertical, Coincident, Parallel, Perpendicular, Tangent, Equal, Fix,
    Fit, Body, Warning, Error, Combine, ExportStl, Shell, Mirror, LinearPattern, CircularPattern,
};

/// Vector icons painted at runtime (no binary assets in the repository).
QIcon icon(IconId id);

}  // namespace silex::ui
