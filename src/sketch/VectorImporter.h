#pragma once
// SVG / DXF import into the 2D sketcher (pure C++, no Qt, no OCCT).
//
// The parsers turn a drawing into flat pieces (lines, true circular arcs, circles) expressed in
// millimetres with the Y axis pointing up and arcs running counter-clockwise, like the sketch
// model. Curves that are not circular (Béziers, ellipses, splines, non-uniformly scaled arcs) are
// discretised into lines within a chord tolerance. importShapes() then places the drawing in a
// sketch (scale / target width, rotation, offset), merges the end points that touch and can bind
// the whole drawing into a rigid block that only translates.

#include <filesystem>
#include <string>
#include <vector>

#include "model/Sketch.h"

namespace silex::vectorimport {

/// One imported piece: mm, Y up, arcs CCW from a0 to a1 (radians).
struct VectorShape
{
    GeoKind kind = GeoKind::Line;  ///< Line, Arc or Circle
    Vec2 p1, p2;                   ///< Line
    Vec2 c;                        ///< Arc / Circle
    double r = 0, a0 = 0, a1 = 0;
};

struct ImportOptions
{
    double scale = 1.0;            ///< multiplier applied after unit conversion
    double targetWidth = 0;        ///< mm; > 0 overrides scale so the bounding box width equals it
    Vec2 offset{0, 0};             ///< mm, applied last; the drawing's bounding-box CENTER lands on `offset`
    double angleDeg = 0;           ///< rotation about the bbox center (CCW)
    bool rigid = false;            ///< "Group as rigid block"
    bool construction = false;
    double chordTolerance = 0.02;  ///< mm, max sagitta when discretising Béziers/ellipses/splines
    double mergeTolerance = 0.01;  ///< mm, endpoints closer than this are merged (+ Coincident)
};

struct ImportResult
{
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
    std::vector<Uuid> geos, constraints;
    Uuid anchor;  ///< construction Point of a rigid block (empty when not rigid)
    int lines = 0, arcs = 0, circles = 0, merged = 0;
    Vec2 bboxMin, bboxMax;  ///< in sketch mm after transform

    explicit operator bool() const { return ok; }
};

/// Parses an SVG document. `chordTolerance` (mm, in the parser's output space) drives the
/// discretisation of non-circular curves. Throws std::runtime_error on malformed input.
std::vector<VectorShape> parseSvg(const std::string& text, std::vector<std::string>* warnings = nullptr,
                                  double chordTolerance = 0.02);
/// Parses an ASCII DXF document (ENTITIES section). Throws std::runtime_error.
std::vector<VectorShape> parseDxf(const std::string& text, std::vector<std::string>* warnings = nullptr,
                                  double chordTolerance = 0.02);
/// Reads and parses a .svg / .dxf file (chosen by extension). Throws std::runtime_error.
std::vector<VectorShape> loadVectorFile(const std::filesystem::path& file, std::vector<std::string>* warnings = nullptr,
                                        double chordTolerance = 0.02);

/// Adds the shapes to the sketch (see ImportOptions). The geometry satisfies every created
/// constraint exactly; the solver is not run.
ImportResult importShapes(Sketch& sk, std::vector<VectorShape> shapes, const ImportOptions& o);
/// loadVectorFile + importShapes. Never throws: errors are reported in the result.
ImportResult importFile(Sketch& sk, const std::filesystem::path& file, const ImportOptions& o);

}  // namespace silex::vectorimport
