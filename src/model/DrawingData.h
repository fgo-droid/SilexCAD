#pragma once
// v0.3 — 2D technical drawing data produced by the kernel (HLR projection) and laid out by the UI.
// OCCT-free: polylines in view coordinates (millimetres, x to the right, y up).

#include <string>
#include <vector>

#include "model/Math.h"

namespace silex {

enum class DrawLineKind { Visible, Hidden, Axis };

struct DrawingCurve
{
    DrawLineKind kind = DrawLineKind::Visible;
    std::vector<Vec2> pts;
};

/// Circle or arc seen face-on in a view (snap target for diameter / radius dimensions).
struct DrawingCircle
{
    Vec2 center;
    double radius = 0;
    bool full = true;
    double a0 = 0, a1 = 0;  ///< arc angles (radians), when !full
    bool visible = true;
};

struct DrawingViewData
{
    std::string name;   ///< "front" | "top" | "right"
    std::string label;  ///< "Vue de face (XZ)" ...
    std::vector<DrawingCurve> curves;
    std::vector<DrawingCircle> circles;
    std::vector<Vec2> vertices;  ///< snap points (ends of visible edges)
    Vec2 min{0, 0}, max{0, 0};   ///< extent of the visible + hidden geometry

    double width() const { return max.x - min.x; }
    double height() const { return max.y - min.y; }
};

struct DrawingProjection
{
    std::vector<DrawingViewData> views;  ///< front, top, right (first-angle / European layout)
    double volume = 0;                   ///< mm3, projected bodies
    double mass = 0;                     ///< g, with each body's material density
    std::string error;
};

}  // namespace silex
