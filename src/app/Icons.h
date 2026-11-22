#pragma once
#include <QIcon>

namespace silex::ui {

enum class IconId {
    New, Open, Save, Undo, Redo, ImportStep, ExportStep,
    Sketch, Extrude, Revolve, Fillet, Chamfer, Finish, Cancel,
    Line, Rectangle, Circle, Arc, Dimension, Construction,
    Horizontal, Vertical, Coincident, Parallel, Perpendicular, Tangent, Equal, Fix,
    Fit, Body, Warning, Error, Combine, ExportStl, Shell, Mirror, LinearPattern, CircularPattern,
    // v0.3
    Measure, Interference, Section, Parameters, Sweep, Loft, Drawing, Export3mf, Material, Project,
    // Sketcher Pro
    CenterRectangle, Polygon, Slot, Trim, Extend, SketchFillet, Offset, SketchMirror, SketchPattern,
    Midpoint, Symmetry, Concentric, Collinear, UnderConstrained,
    // ergonomics lot
    PressPull, Box, Cylinder,
    // v0.4
    SplitBody, MoveBody, Align, DeleteFace, Eye, EyeOff, Plane, Rib, Printability, Thread, Learn, Hint,
    // v0.5
    Hole, Component, Bed, LayFlat, Pegs, Enclosure, Ghost, Repair, Profile, Recorder, Tutor,
    // v0.7
    Text, ImportVector, Canvas, Calibrate,
};

/// Vector icons painted at runtime (no binary assets in the repository).
QIcon icon(IconId id);

}  // namespace silex::ui
