#pragma once
// v0.4 lot 2 — FDM printability analysis on the display tessellation (OCCT-free, unit-testable):
//  * overhangs: angle of each downward-facing triangle measured from the vertical
//    (0° = vertical wall, 90° = flat ceiling); 45-60° = orange, > 60° = red (supports needed);
//    triangles lying on the build plate are not overhangs;
//  * minimum wall thickness: a ray is cast inwards from every triangle, the distance to the
//    opposite side of the material is the local wall thickness (default threshold 0.8 mm =
//    2 lines of a 0.4 mm nozzle).

#include <cstdint>
#include <vector>

#include "model/RenderModel.h"

namespace silex {

struct PrintabilityOptions
{
    Vec3 buildDir{0, 0, 1};    ///< "up" of the printer (plate normal)
    double warnAngle = 45.0;   ///< degrees from vertical: orange from here
    double failAngle = 60.0;   ///< red above
    double minWall = 0.8;      ///< mm
    bool checkWalls = true;
};

enum class TriClass : uint8_t { Ok = 0, Warn = 1, Fail = 2 };

struct PrintabilityResult
{
    double area = 0;            ///< total area (mm2)
    double warnArea = 0;        ///< overhang 45-60° (mm2)
    double failArea = 0;        ///< overhang > 60° (mm2)
    double thinArea = 0;        ///< walls thinner than minWall (mm2)
    double minThickness = -1;   ///< thinnest wall found (mm), -1 if none measured
    std::vector<TriClass> overhang;  ///< per triangle
    std::vector<uint8_t> thin;       ///< per triangle: 1 = wall too thin

    double overhangArea() const { return warnArea + failArea; }
};

PrintabilityResult analyzePrintability(const RenderMesh& mesh, const PrintabilityOptions& o = {});

/// Sub-mesh of the triangles flagged by `keep` (for coloured overlays), pushed `lift` mm outwards.
RenderMesh extractTriangles(const RenderMesh& mesh, const std::vector<bool>& keep, double lift = 0.02);

}  // namespace silex
