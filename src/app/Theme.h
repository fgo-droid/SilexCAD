#pragma once
#include <QColor>

class QApplication;

namespace silex::ui {

/// Dark theme (Phase 4 item brought forward: it costs nothing and CAD users expect it).
namespace Colors {
inline const QColor Background{0x26, 0x29, 0x2e};
inline const QColor BackgroundTop{0x3a, 0x3f, 0x47};
inline const QColor Panel{0x2d, 0x30, 0x36};
inline const QColor Accent{0x2f, 0x9b, 0xff};
inline const QColor Body{0xb4, 0xbc, 0xc8};
inline const QColor Edge{0x16, 0x18, 0x1c};
inline const QColor Hover{0x4d, 0xc3, 0xff};
inline const QColor Selected{0x2f, 0x9b, 0xff};
inline const QColor TargetBody{0xff, 0x9f, 0x2e};  ///< first selected body = boolean target
inline const QColor Profile{0x5a, 0x9f, 0xd8};
inline const QColor PreviewJoin{0x40, 0x9c, 0xff};
inline const QColor PreviewCut{0xff, 0x4d, 0x4d};
inline const QColor PreviewNew{0x4c, 0xd9, 0x7b};
// Sketch colour code (guidelines §4.3)
inline const QColor SketchUnder{0x4a, 0xa3, 0xff};        ///< blue: degrees of freedom left
inline const QColor SketchFull{0xf2, 0xf4, 0xf7};         ///< white (dark mode): fully constrained
inline const QColor SketchOver{0xff, 0x45, 0x3a};         ///< red: over-constrained / invalid
inline const QColor SketchConstruction{0xe0, 0x9b, 0x3d}; ///< orange dashed
inline const QColor SketchInactive{0x8a, 0x93, 0xa0};
inline const QColor Constraint{0x9d, 0xd8, 0x6b};
inline const QColor Dimension{0xe8, 0xe8, 0xe8};
inline const QColor Warning{0xff, 0xb0, 0x20};
inline const QColor Error{0xff, 0x45, 0x3a};
}  // namespace Colors

void applyDarkTheme(QApplication& app);

}  // namespace silex::ui
