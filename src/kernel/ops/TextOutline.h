#pragma once
// v0.7 lot A — text to planar B-Rep faces.
//
// The OpenCascade build of the project has no FreeType support (Font_BRepFont is unavailable), so the
// glyph outlines are read with FreeType directly and converted the same way Font_BRepFont does: line
// segments and quadratic / cubic Bézier arcs become exact edges (Geom_BezierCurve), contours are
// sorted into outer boundaries and holes by nesting depth, and each letter part becomes a planar face.

#include <string>
#include <vector>

#include <TopoDS_Shape.hxx>

#include "model/Math.h"

namespace silex::kernel {

struct TextLayout
{
    std::string text;               ///< UTF-8, '\n' separates lines
    std::string font = "Liberation Sans";
    double height = 6;              ///< capital letter height (mm)
    std::string align = "center";   ///< left | center | right (per line, about x = 0)
    double lineSpacing = 1.4;       ///< baseline distance in letter heights
    double angleDeg = 0;            ///< rotation about the anchor
    Vec2 anchor{0, 0};              ///< the text block is centered vertically on the anchor
};

struct TextOutline
{
    TopoDS_Shape faces;             ///< compound of planar faces in the plane (z = 0 unless placed)
    int faceCount = 0;
    double area = 0;                ///< mm²
    double strokeWidth = 0;         ///< estimated stem width (mm), from the 'l' / 'I' glyph
    std::string fontFile;           ///< resolved font file
    std::vector<std::string> warnings;  ///< missing glyphs...
};

/// Builds the faces of the text in the plane `pl` (local coordinates = layout coordinates).
/// Throws std::runtime_error (unknown font, empty text...).
TextOutline buildTextOutline(const TextLayout& layout, const Placement& pl);

/// Font file of a family name ("Liberation Sans"), a file name ("arial.ttf") or a path.
/// Empty when not found.
std::string resolveFontFile(const std::string& font);
/// Folders searched for fonts, before the defaults (<exe>/resources/fonts, $SILEX_RESOURCE_DIR/fonts,
/// the system font folders).
void addFontDirectory(const std::string& dir);
/// Family names of the usable fonts (regular style), sorted.
std::vector<std::string> fontFamilies();

}  // namespace silex::kernel
