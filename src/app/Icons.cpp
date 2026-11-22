#include "Icons.h"

#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <cmath>

#include "Theme.h"

namespace silex::ui {

namespace {

const QColor kInk(0xdd, 0xe1, 0xe6);
const QColor kBlue(0x4a, 0xa3, 0xff);
const QColor kOrange(0xf0, 0xa0, 0x40);
const QColor kGreen(0x5c, 0xd6, 0x8a);
const QColor kRed(0xff, 0x5a, 0x50);

void paintIcon(QPainter& p, IconId id)
{
    QPen ink(kInk, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    QPen blue(kBlue, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(ink);
    p.setBrush(Qt::NoBrush);
    QFont f = p.font();
    f.setPixelSize(10);
    f.setBold(true);
    p.setFont(f);

    auto text = [&](const QString& t, QColor c = kInk) {
        p.setPen(c);
        p.drawText(QRectF(0, 0, 24, 24), Qt::AlignCenter, t);
    };
    auto box = [&](QColor fill) {
        // small isometric box
        QPolygonF top{{12, 4}, {20, 8}, {12, 12}, {4, 8}};
        QPolygonF left{{4, 8}, {12, 12}, {12, 21}, {4, 17}};
        QPolygonF right{{12, 12}, {20, 8}, {20, 17}, {12, 21}};
        p.setPen(QPen(kInk, 1.2));
        p.setBrush(fill.lighter(130));
        p.drawPolygon(top);
        p.setBrush(fill);
        p.drawPolygon(left);
        p.setBrush(fill.darker(130));
        p.drawPolygon(right);
    };

    switch (id) {
        case IconId::New:
            p.drawPolyline(QPolygonF{{14, 3}, {6, 3}, {6, 21}, {18, 21}, {18, 7}, {14, 3}, {14, 7}, {18, 7}});
            break;
        case IconId::Open:
            p.drawPolyline(QPolygonF{{3, 19}, {3, 5}, {9, 5}, {11, 7}, {19, 7}, {19, 10}});
            p.drawPolygon(QPolygonF{{3, 19}, {6, 10}, {22, 10}, {19, 19}});
            break;
        case IconId::Save:
            p.drawRoundedRect(QRectF(4, 4, 16, 16), 2, 2);
            p.drawRect(QRectF(8, 4, 8, 5));
            p.drawRect(QRectF(7, 13, 10, 7));
            break;
        case IconId::Undo:
        case IconId::Redo: {
            if (id == IconId::Redo) {
                p.translate(24, 0);
                p.scale(-1, 1);
            }
            QPainterPath path;
            path.moveTo(6, 10);
            path.cubicTo(10, 4, 20, 6, 19, 14);
            path.cubicTo(18, 18, 14, 19, 11, 19);
            p.drawPath(path);
            p.drawPolyline(QPolygonF{{5, 4}, {5, 10}, {11, 10}});
            break;
        }
        case IconId::ImportStep:
        case IconId::ExportStep:
            box(QColor(0x5a, 0x6b, 0x80));
            p.setPen(QPen(id == IconId::ImportStep ? kGreen : kOrange, 2.2, Qt::SolidLine, Qt::RoundCap));
            if (id == IconId::ImportStep) {
                p.drawLine(QPointF(19, 1), QPointF(19, 8));
                p.drawPolyline(QPolygonF{{16, 5}, {19, 8}, {22, 5}});
            }
            else {
                p.drawLine(QPointF(19, 8), QPointF(19, 1));
                p.drawPolyline(QPolygonF{{16, 4}, {19, 1}, {22, 4}});
            }
            break;
        case IconId::Sketch:
            p.setPen(blue);
            p.drawRect(QRectF(3, 9, 12, 11));
            p.drawEllipse(QPointF(15, 9), 5, 5);
            p.setPen(QPen(kOrange, 2.0, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(14, 18), QPointF(21, 4));
            break;
        case IconId::Extrude:
            box(QColor(0x3d, 0x7c, 0xc4));
            p.setPen(QPen(kOrange, 2.0, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(12, 9), QPointF(12, 0.8));
            p.drawPolyline(QPolygonF{{9, 3.5}, {12, 0.8}, {15, 3.5}});
            break;
        case IconId::Revolve: {
            p.setPen(QPen(kBlue, 1.8));
            p.drawEllipse(QRectF(3, 6, 18, 12));
            p.setPen(QPen(kInk, 1.2, Qt::DashLine));
            p.drawLine(QPointF(12, 2), QPointF(12, 22));
            p.setPen(QPen(kOrange, 2.0, Qt::SolidLine, Qt::RoundCap));
            p.drawPolyline(QPolygonF{{17, 15}, {21, 13}, {21, 18}});
            break;
        }
        case IconId::Fillet: {
            QPainterPath path;
            path.moveTo(4, 20);
            path.lineTo(4, 12);
            path.quadTo(4, 4, 12, 4);
            path.lineTo(20, 4);
            p.setPen(QPen(kInk, 1.4));
            p.drawPolyline(QPolygonF{{4, 12}, {4, 4}, {12, 4}});
            p.setPen(QPen(kBlue, 2.2));
            p.drawPath(path);
            break;
        }
        case IconId::Chamfer:
            p.setPen(QPen(kInk, 1.4));
            p.drawPolyline(QPolygonF{{4, 11}, {4, 4}, {11, 4}});
            p.setPen(QPen(kBlue, 2.2));
            p.drawPolyline(QPolygonF{{4, 20}, {4, 11}, {11, 4}, {20, 4}});
            break;
        case IconId::Finish:
            p.setPen(QPen(kGreen, 2.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPolyline(QPolygonF{{4, 12}, {10, 18}, {20, 6}});
            break;
        case IconId::Cancel:
            p.setPen(QPen(kRed, 2.4, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(6, 6), QPointF(18, 18));
            p.drawLine(QPointF(18, 6), QPointF(6, 18));
            break;
        case IconId::Line:
            p.setPen(blue);
            p.drawLine(QPointF(4, 19), QPointF(20, 5));
            p.setBrush(kInk);
            p.setPen(Qt::NoPen);
            p.drawEllipse(QPointF(4, 19), 2, 2);
            p.drawEllipse(QPointF(20, 5), 2, 2);
            break;
        case IconId::Rectangle:
            p.setPen(blue);
            p.drawRect(QRectF(4, 6, 16, 12));
            break;
        case IconId::Circle:
            p.setPen(blue);
            p.drawEllipse(QPointF(12, 12), 8, 8);
            p.setBrush(kInk);
            p.setPen(Qt::NoPen);
            p.drawEllipse(QPointF(12, 12), 1.6, 1.6);
            break;
        case IconId::Arc:
            p.setPen(blue);
            p.drawArc(QRectF(4, 6, 16, 16), 0, 180 * 16);
            break;
        case IconId::Dimension:
            p.setPen(QPen(kInk, 1.2));
            p.drawLine(QPointF(3, 8), QPointF(3, 20));
            p.drawLine(QPointF(21, 8), QPointF(21, 20));
            p.drawLine(QPointF(3, 16), QPointF(21, 16));
            p.drawPolyline(QPolygonF{{6, 14}, {3, 16}, {6, 18}});
            p.drawPolyline(QPolygonF{{18, 14}, {21, 16}, {18, 18}});
            f.setPixelSize(8);
            p.setFont(f);
            p.drawText(QRectF(0, 2, 24, 12), Qt::AlignCenter, "10");
            break;
        case IconId::Construction:
            p.setPen(QPen(kOrange, 1.8, Qt::DashLine));
            p.drawLine(QPointF(4, 20), QPointF(20, 4));
            break;
        case IconId::Horizontal: text("H", Colors::Constraint); break;
        case IconId::Vertical: text("V", Colors::Constraint); break;
        case IconId::Coincident:
            p.setPen(QPen(Colors::Constraint, 1.6));
            p.drawLine(QPointF(4, 18), QPointF(12, 12));
            p.drawLine(QPointF(12, 12), QPointF(20, 18));
            p.setBrush(Colors::Constraint);
            p.drawEllipse(QPointF(12, 12), 2.6, 2.6);
            break;
        case IconId::Parallel: text("//", Colors::Constraint); break;
        case IconId::Perpendicular:
            p.setPen(QPen(Colors::Constraint, 1.8));
            p.drawLine(QPointF(5, 19), QPointF(19, 19));
            p.drawLine(QPointF(12, 19), QPointF(12, 5));
            break;
        case IconId::Tangent:
            p.setPen(QPen(Colors::Constraint, 1.6));
            p.drawEllipse(QPointF(12, 14), 6, 6);
            p.drawLine(QPointF(3, 8), QPointF(21, 8));
            break;
        case IconId::Equal: text("=", Colors::Constraint); break;
        case IconId::Fix:
            p.setPen(QPen(Colors::Constraint, 1.6));
            p.drawRoundedRect(QRectF(6, 11, 12, 9), 1.5, 1.5);
            p.drawArc(QRectF(8, 5, 8, 10), 0, 180 * 16);
            break;
        case IconId::Fit:
            p.drawPolyline(QPolygonF{{3, 9}, {3, 3}, {9, 3}});
            p.drawPolyline(QPolygonF{{15, 3}, {21, 3}, {21, 9}});
            p.drawPolyline(QPolygonF{{21, 15}, {21, 21}, {15, 21}});
            p.drawPolyline(QPolygonF{{9, 21}, {3, 21}, {3, 15}});
            p.setPen(blue);
            p.drawRect(QRectF(8, 8, 8, 8));
            break;
        case IconId::Body: box(QColor(0x7a, 0x86, 0x96)); break;
        case IconId::Combine:
            // two overlapping solids
            p.setPen(QPen(kInk, 1.3));
            p.setBrush(QColor(0x3d, 0x7c, 0xc4));
            p.drawRoundedRect(QRectF(3, 7, 12, 12), 2, 2);
            p.setBrush(QColor(0xf0, 0xa0, 0x40, 200));
            p.drawEllipse(QRectF(9, 3, 12, 12));
            break;
        case IconId::Shell: {
            // open box seen in section: thick walls, hollow inside
            p.setPen(QPen(kInk, 1.2));
            p.setBrush(QColor(0x3d, 0x7c, 0xc4));
            QPainterPath path;
            path.moveTo(3, 6);
            path.lineTo(7, 6);
            path.lineTo(7, 16);
            path.lineTo(17, 16);
            path.lineTo(17, 6);
            path.lineTo(21, 6);
            path.lineTo(21, 20);
            path.lineTo(3, 20);
            path.closeSubpath();
            p.drawPath(path);
            p.setPen(QPen(kOrange, 1.6, Qt::DashLine));
            p.drawLine(QPointF(7, 6), QPointF(17, 6));
            break;
        }
        case IconId::Mirror: {
            p.setPen(QPen(kOrange, 1.6, Qt::DashLine));
            p.drawLine(QPointF(12, 2), QPointF(12, 22));
            p.setPen(QPen(kInk, 1.2));
            p.setBrush(QColor(0x3d, 0x7c, 0xc4));
            p.drawPolygon(QPolygonF{{3, 18}, {9, 18}, {9, 6}});
            p.setBrush(QColor(0x3d, 0x7c, 0xc4, 110));
            p.drawPolygon(QPolygonF{{21, 18}, {15, 18}, {15, 6}});
            break;
        }
        case IconId::LinearPattern: {
            p.setPen(QPen(kInk, 1.1));
            for (int i = 0; i < 3; ++i) {
                p.setBrush(i == 0 ? QColor(0x3d, 0x7c, 0xc4) : QColor(0x3d, 0x7c, 0xc4, 120));
                p.drawRoundedRect(QRectF(2 + i * 7.5, 8, 5.5, 8), 1, 1);
            }
            p.setPen(QPen(kOrange, 1.6, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(3, 20), QPointF(21, 20));
            p.drawPolyline(QPolygonF{{18, 18}, {21, 20}, {18, 22}});
            break;
        }
        case IconId::CircularPattern: {
            p.setPen(QPen(kOrange, 1.2, Qt::DashLine));
            p.drawEllipse(QPointF(12, 12), 7.5, 7.5);
            p.setPen(QPen(kInk, 1.0));
            for (int k = 0; k < 6; ++k) {
                const double a = k * 60.0 * 3.14159265 / 180.0;
                p.setBrush(k == 0 ? QColor(0x3d, 0x7c, 0xc4) : QColor(0x3d, 0x7c, 0xc4, 120));
                p.drawEllipse(QPointF(12 + 7.5 * std::cos(a), 12 - 7.5 * std::sin(a)), 2.4, 2.4);
            }
            break;
        }
        case IconId::ExportStl: {
            // triangulated facet + arrow
            p.setPen(QPen(kInk, 1.2));
            p.setBrush(QColor(0x5a, 0x6b, 0x80));
            p.drawPolygon(QPolygonF{{3, 20}, {12, 6}, {19, 20}});
            p.drawLine(QPointF(12, 6), QPointF(11, 20));
            p.drawLine(QPointF(7.5, 13), QPointF(19, 20));
            p.setPen(QPen(kOrange, 2.2, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(19, 9), QPointF(19, 1));
            p.drawPolyline(QPolygonF{{16, 4}, {19, 1}, {22, 4}});
            break;
        }
        case IconId::Measure: {
            // caliper-like dimension
            p.setPen(QPen(kInk, 1.4));
            p.drawLine(QPointF(4, 6), QPointF(4, 18));
            p.drawLine(QPointF(20, 6), QPointF(20, 18));
            p.setPen(QPen(kOrange, 1.6, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(5, 12), QPointF(19, 12));
            p.drawPolyline(QPolygonF{{8, 9}, {5, 12}, {8, 15}});
            p.drawPolyline(QPolygonF{{16, 9}, {19, 12}, {16, 15}});
            break;
        }
        case IconId::Interference: {
            p.setPen(QPen(kInk, 1.2));
            p.setBrush(QColor(0x3d, 0x7c, 0xc4, 150));
            p.drawRect(QRectF(3, 5, 11, 11));
            p.setBrush(QColor(0x5c, 0xd6, 0x8a, 150));
            p.drawRect(QRectF(10, 9, 11, 11));
            p.setPen(Qt::NoPen);
            p.setBrush(kRed);
            p.drawRect(QRectF(10, 9, 4, 7));
            break;
        }
        case IconId::Section: {
            box(QColor(0x5a, 0x6b, 0x80));
            p.setPen(QPen(kRed, 1.8));
            p.drawLine(QPointF(2, 14), QPointF(22, 10));
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0xee, 0x6b, 0x3d));
            p.drawPolygon(QPolygonF{{12, 12}, {20, 10.4}, {20, 17}, {12, 21}});
            break;
        }
        case IconId::Parameters: {
            p.setPen(QPen(kInk, 1.3));
            p.drawRect(QRectF(3, 4, 18, 16));
            p.drawLine(QPointF(3, 9), QPointF(21, 9));
            p.drawLine(QPointF(10, 4), QPointF(10, 20));
            QFont ff = p.font();
            ff.setPixelSize(8);
            ff.setItalic(true);
            p.setFont(ff);
            p.setPen(kOrange);
            p.drawText(QRectF(3, 9, 7, 11), Qt::AlignCenter, "x");
            p.setPen(kBlue);
            p.drawText(QRectF(10, 9, 11, 11), Qt::AlignCenter, "=2");
            break;
        }
        case IconId::Sweep: {
            QPainterPath path;
            path.moveTo(4, 20);
            path.cubicTo(4, 8, 10, 5, 20, 5);
            p.setPen(QPen(kOrange, 1.4, Qt::DashLine));
            p.drawPath(path);
            p.setPen(QPen(QColor(0x3d, 0x7c, 0xc4), 6, Qt::SolidLine, Qt::RoundCap));
            QPainterPath tube;
            tube.moveTo(4, 19);
            tube.cubicTo(4, 9, 10, 6, 19, 6);
            p.drawPath(tube);
            p.setPen(QPen(kInk, 1.2));
            p.setBrush(QColor(0x9c, 0xc8, 0xff));
            p.drawEllipse(QPointF(4, 20), 3, 1.5);
            break;
        }
        case IconId::Loft: {
            p.setPen(QPen(kInk, 1.2));
            p.setBrush(QColor(0x3d, 0x7c, 0xc4, 140));
            QPolygonF body{{4, 19}, {20, 19}, {16, 6}, {8, 6}};
            p.drawPolygon(body);
            p.setBrush(QColor(0x9c, 0xc8, 0xff));
            p.drawEllipse(QPointF(12, 6), 4, 1.6);
            p.setPen(QPen(kOrange, 1.6));
            p.drawLine(QPointF(4, 19), QPointF(20, 19));
            break;
        }
        case IconId::Drawing: {
            p.setPen(QPen(kInk, 1.2));
            p.setBrush(QColor(0xf2, 0xf4, 0xf7));
            p.drawRect(QRectF(3, 4, 18, 16));
            p.setPen(QPen(QColor(0x20, 0x24, 0x2a), 1.1));
            p.drawRect(QRectF(6, 7, 6, 5));
            p.drawRect(QRectF(6, 13, 6, 4));
            p.drawRect(QRectF(13, 7, 4, 5));
            p.setPen(QPen(kBlue, 1.0));
            p.drawLine(QPointF(6, 18.5), QPointF(12, 18.5));
            break;
        }
        case IconId::Export3mf: {
            box(QColor(0x3d, 0x7c, 0xc4));
            p.setPen(kOrange);
            QFont ff = p.font();
            ff.setPixelSize(8);
            ff.setBold(true);
            p.setFont(ff);
            p.drawText(QRectF(0, 15, 24, 9), Qt::AlignCenter, "3MF");
            break;
        }
        case IconId::Material: {
            p.setPen(QPen(kInk, 1.0));
            const QColor cs[4] = {QColor(0xee, 0xee, 0xe8), QColor(0x2a, 0x2c, 0x30), QColor(0x1f, 0x7a, 0x3a),
                                  QColor(0xc8, 0xcc, 0xd2)};
            for (int k = 0; k < 4; ++k) {
                p.setBrush(cs[k]);
                p.drawEllipse(QPointF(7 + (k % 2) * 10, 7 + (k / 2) * 10), 4.2, 4.2);
            }
            break;
        }
        case IconId::Project: {
            box(QColor(0x5a, 0x6b, 0x80));
            p.setPen(QPen(Colors::SketchProjected, 2.0, Qt::DashLine, Qt::RoundCap));
            p.drawLine(QPointF(3, 22), QPointF(21, 22));
            p.setPen(QPen(Colors::SketchProjected, 1.6, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(20, 18), QPointF(20, 21));
            break;
        }
        // ---------------------------------------------------------------- Sketcher Pro
        case IconId::CenterRectangle:
            p.setPen(blue);
            p.drawRect(QRectF(4, 6, 16, 12));
            p.setPen(QPen(kOrange, 1.1, Qt::DashLine));
            p.drawLine(QPointF(4, 6), QPointF(20, 18));
            p.drawLine(QPointF(20, 6), QPointF(4, 18));
            p.setPen(Qt::NoPen);
            p.setBrush(kInk);
            p.drawEllipse(QPointF(12, 12), 1.8, 1.8);
            break;
        case IconId::Polygon: {
            QPolygonF hex;
            for (int k = 0; k < 6; ++k)
                hex << QPointF(12 + 9 * std::cos(k * M_PI / 3), 12 + 9 * std::sin(k * M_PI / 3));
            p.setPen(blue);
            p.drawPolygon(hex);
            p.setPen(Qt::NoPen);
            p.setBrush(kInk);
            p.drawEllipse(QPointF(12, 12), 1.6, 1.6);
            break;
        }
        case IconId::Slot: {
            QPainterPath path;
            path.addRoundedRect(QRectF(2, 7, 20, 10), 5, 5);
            p.setPen(blue);
            p.drawPath(path);
            p.setPen(QPen(kOrange, 1.1, Qt::DashLine));
            p.drawLine(QPointF(7, 12), QPointF(17, 12));
            break;
        }
        case IconId::Trim:  // scissors
            p.setPen(QPen(kInk, 1.6));
            p.drawEllipse(QPointF(6, 18), 3, 3);
            p.drawEllipse(QPointF(18, 18), 3, 3);
            p.drawLine(QPointF(8, 16), QPointF(17, 4));
            p.drawLine(QPointF(16, 16), QPointF(7, 4));
            p.setPen(QPen(kRed, 1.4, Qt::DotLine));
            p.drawLine(QPointF(2, 10), QPointF(22, 10));
            break;
        case IconId::Extend:
            p.setPen(blue);
            p.drawLine(QPointF(3, 14), QPointF(13, 14));
            p.setPen(QPen(kGreen, 1.6, Qt::DashLine));
            p.drawLine(QPointF(13, 14), QPointF(19, 14));
            p.setPen(QPen(kInk, 1.8));
            p.drawLine(QPointF(20, 4), QPointF(20, 21));
            break;
        case IconId::SketchFillet:
            p.setPen(QPen(kInk, 1.3));
            p.drawPolyline(QPolygonF{{4, 20}, {4, 4}, {20, 4}});
            p.setPen(blue);
            p.drawArc(QRectF(4, 4, 16, 16), 90 * 16, 90 * 16);
            break;
        case IconId::Offset:
            p.setPen(QPen(kInk, 1.4));
            p.drawRect(QRectF(8, 8, 8, 8));
            p.setPen(blue);
            p.drawRoundedRect(QRectF(3, 3, 18, 18), 3, 3);
            break;
        case IconId::SketchMirror:
            p.setPen(QPen(kOrange, 1.4, Qt::DashLine));
            p.drawLine(QPointF(12, 2), QPointF(12, 22));
            p.setPen(blue);
            p.drawPolyline(QPolygonF{{9, 5}, {3, 12}, {9, 19}});
            p.setPen(QPen(kBlue.lighter(130), 1.4, Qt::DashLine));
            p.drawPolyline(QPolygonF{{15, 5}, {21, 12}, {15, 19}});
            break;
        case IconId::SketchPattern:
            p.setPen(blue);
            for (int k = 0; k < 3; ++k)
                p.drawEllipse(QPointF(5 + 7 * k, 12), 3, 3);
            p.setPen(QPen(kOrange, 1.0, Qt::DashLine));
            p.drawLine(QPointF(5, 18), QPointF(19, 18));
            break;
        case IconId::Midpoint:
            p.setPen(QPen(Colors::Constraint, 1.6));
            p.drawLine(QPointF(3, 15), QPointF(21, 15));
            p.setBrush(Colors::Constraint);
            p.drawRect(QRectF(10, 13, 4, 4));
            p.drawLine(QPointF(12, 5), QPointF(12, 11));
            break;
        case IconId::Symmetry:
            p.setPen(QPen(Colors::Constraint, 1.2, Qt::DashLine));
            p.drawLine(QPointF(12, 2), QPointF(12, 22));
            p.setPen(Qt::NoPen);
            p.setBrush(Colors::Constraint);
            p.drawEllipse(QPointF(5, 12), 2.5, 2.5);
            p.drawEllipse(QPointF(19, 12), 2.5, 2.5);
            break;
        case IconId::Concentric:
            p.setPen(QPen(Colors::Constraint, 1.6));
            p.drawEllipse(QPointF(12, 12), 9, 9);
            p.drawEllipse(QPointF(12, 12), 4.5, 4.5);
            break;
        case IconId::Collinear:
            p.setPen(QPen(Colors::Constraint, 2.0));
            p.drawLine(QPointF(2, 16), QPointF(10, 12));
            p.drawLine(QPointF(14, 10), QPointF(22, 6));
            p.setPen(QPen(Colors::Constraint, 0.8, Qt::DotLine));
            p.drawLine(QPointF(10, 12), QPointF(14, 10));
            break;
        case IconId::UnderConstrained:
            p.setPen(QPen(Colors::SketchUnder, 1.8));
            p.drawLine(QPointF(4, 18), QPointF(16, 6));
            p.setBrush(QColor(Colors::SketchUnder.red(), Colors::SketchUnder.green(), Colors::SketchUnder.blue(), 90));
            p.drawEllipse(QPointF(16, 6), 4, 4);
            p.drawEllipse(QPointF(4, 18), 2.5, 2.5);
            break;
        // ---------------------------------------------------------------- ergonomics lot
        case IconId::PressPull: {
            p.setPen(QPen(kInk, 1.2));
            p.setBrush(QColor(0x3d, 0x7c, 0xc4));
            p.drawPolygon(QPolygonF{{3, 20}, {15, 20}, {21, 15}, {9, 15}});
            p.setBrush(QColor(0x4a, 0xa3, 0xff, 120));
            p.drawPolygon(QPolygonF{{9, 15}, {21, 15}, {21, 10}, {9, 10}});
            p.setPen(QPen(kOrange, 2.0, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(12, 13), QPointF(12, 3));
            p.setBrush(kOrange);
            p.drawPolygon(QPolygonF{{12, 1}, {15.5, 5.5}, {8.5, 5.5}});
            break;
        }
        case IconId::Box: box(QColor(0x3d, 0x7c, 0xc4)); break;
        case IconId::Cylinder:
            p.setPen(QPen(kInk, 1.2));
            p.setBrush(QColor(0x3d, 0x7c, 0xc4));
            p.drawRect(QRectF(5, 7, 14, 12));
            p.setPen(Qt::NoPen);
            p.drawEllipse(QRectF(5, 15, 14, 7));
            p.setPen(QPen(kInk, 1.2));
            p.drawArc(QRectF(5, 15, 14, 7), 180 * 16, 180 * 16);
            p.drawLine(QPointF(5, 7), QPointF(5, 18.5));
            p.drawLine(QPointF(19, 7), QPointF(19, 18.5));
            p.setBrush(QColor(0x4a, 0xa3, 0xff));
            p.drawEllipse(QRectF(5, 3.5, 14, 7));
            break;
        // ---------------------------------------------------------------- v0.4
        case IconId::SplitBody:
            p.setPen(QPen(kInk, 1.2));
            p.setBrush(QColor(0x3d, 0x7c, 0xc4));
            p.drawRect(QRectF(4, 13, 16, 7));
            p.setBrush(QColor(0x4a, 0xa3, 0xff));
            p.drawRect(QRectF(4, 4, 16, 6));
            p.setPen(QPen(kOrange, 1.6, Qt::DashLine));
            p.drawLine(QPointF(1, 11.5), QPointF(23, 11.5));
            break;
        case IconId::MoveBody:
            p.setPen(QPen(kInk, 1.2));
            p.setBrush(QColor(0x3d, 0x7c, 0xc4));
            p.drawRect(QRectF(8, 8, 8, 8));
            p.setPen(QPen(kOrange, 1.8, Qt::SolidLine, Qt::RoundCap));
            for (auto [a, b] : {std::pair{QPointF(12, 7), QPointF(12, 1.5)}, std::pair{QPointF(12, 17), QPointF(12, 22.5)},
                                std::pair{QPointF(7, 12), QPointF(1.5, 12)}, std::pair{QPointF(17, 12), QPointF(22.5, 12)}})
                p.drawLine(a, b);
            break;
        case IconId::Align:
            p.setPen(QPen(kInk, 1.2));
            p.setBrush(QColor(0x3d, 0x7c, 0xc4));
            p.drawRect(QRectF(3, 14, 18, 7));
            p.setBrush(QColor(0x4a, 0xa3, 0xff, 140));
            p.drawRect(QRectF(6, 3, 12, 7));
            p.setPen(QPen(kOrange, 1.6, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(12, 10.5), QPointF(12, 13));
            break;
        case IconId::DeleteFace:
            box(QColor(0x3d, 0x7c, 0xc4));
            p.setPen(QPen(kRed, 2.2, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(14, 14), QPointF(21, 21));
            p.drawLine(QPointF(21, 14), QPointF(14, 21));
            break;
        case IconId::Eye:
        case IconId::EyeOff: {
            QPainterPath eye;
            eye.moveTo(2, 12);
            eye.quadTo(12, 2, 22, 12);
            eye.quadTo(12, 22, 2, 12);
            p.setPen(QPen(kInk, 1.5));
            p.drawPath(eye);
            p.setBrush(kInk);
            p.drawEllipse(QPointF(12, 12), 3, 3);
            if (id == IconId::EyeOff) {
                p.setPen(QPen(kRed, 2.0, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(QPointF(4, 20), QPointF(20, 4));
            }
            break;
        }
        case IconId::Plane:
            p.setPen(QPen(kOrange, 1.4));
            p.setBrush(QColor(0xf0, 0xa0, 0x40, 70));
            p.drawPolygon(QPolygonF{{2, 16}, {9, 8}, {22, 8}, {15, 16}});
            p.setPen(QPen(kInk, 1.2, Qt::DashLine));
            p.drawLine(QPointF(12, 12), QPointF(12, 22));
            break;
        case IconId::Rib:
            p.setPen(QPen(kInk, 1.2));
            p.setBrush(QColor(0x3d, 0x7c, 0xc4));
            p.drawPolygon(QPolygonF{{3, 21}, {21, 21}, {21, 4}, {18, 4}, {18, 18}, {3, 18}});
            p.setBrush(kOrange);
            p.drawPolygon(QPolygonF{{6, 18}, {18, 18}, {18, 7}});
            break;
        case IconId::Printability:
            p.setPen(QPen(kInk, 1.2));
            p.setBrush(QColor(0x5c, 0xd6, 0x8a));
            p.drawRect(QRectF(3, 16, 10, 5));
            p.setBrush(kOrange);
            p.drawPolygon(QPolygonF{{13, 16}, {21, 9}, {21, 12}, {13, 21}});
            p.setPen(QPen(kInk, 1.4));
            p.drawLine(QPointF(1, 22), QPointF(23, 22));
            break;
        case IconId::Thread:
            p.setPen(QPen(kInk, 1.3));
            p.setBrush(QColor(0x3d, 0x7c, 0xc4));
            p.drawRect(QRectF(8, 3, 8, 18));
            p.setPen(QPen(kOrange, 1.4));
            for (int k = 0; k < 5; ++k)
                p.drawLine(QPointF(6, 6 + 3.5 * k), QPointF(18, 4 + 3.5 * k));
            break;
        // ---------------------------------------------------------------- v0.5
        case IconId::Text: {  // raised "Aa" on a plate
            p.setPen(QPen(kInk, 1.2));
            p.setBrush(QColor(0x8c, 0x92, 0x9a));
            p.drawRect(QRectF(2, 15, 20, 6));
            QFont f = p.font();
            f.setBold(true);
            f.setPixelSize(14);
            p.setFont(f);
            p.setPen(kOrange);
            p.drawText(QRectF(1, 1, 22, 16), Qt::AlignCenter, "Aa");
            break;
        }
        case IconId::ImportVector: {  // a Bezier path with its control handles, arrow into the sketch
            p.setPen(QPen(kInk, 1.6));
            p.setBrush(Qt::NoBrush);
            QPainterPath path(QPointF(3, 19));
            path.cubicTo(QPointF(6, 4), QPointF(16, 22), QPointF(21, 6));
            p.drawPath(path);
            p.setPen(QPen(kOrange, 1.1));
            p.drawLine(QPointF(3, 19), QPointF(6, 4));
            p.drawLine(QPointF(21, 6), QPointF(16, 22));
            p.setBrush(kOrange);
            p.drawRect(QRectF(4.5, 2.5, 3, 3));
            p.drawRect(QRectF(14.5, 20.5, 3, 3));
            break;
        }
        case IconId::Canvas:  // photo frame with a mountain and a sun
            p.setPen(QPen(kInk, 1.3));
            p.setBrush(QColor(0x30, 0x4a, 0x66));
            p.drawRect(QRectF(2, 4, 20, 16));
            p.setBrush(QColor(0x7b, 0xd8, 0x8f));
            p.drawPolygon(QPolygonF{{3, 19}, {9, 10}, {13, 15}, {16, 12}, {21, 19}});
            p.setBrush(kOrange);
            p.setPen(Qt::NoPen);
            p.drawEllipse(QPointF(17, 8), 2, 2);
            break;
        case IconId::Calibrate:  // ruler with two picked points
            p.setPen(QPen(kInk, 1.2));
            p.setBrush(QColor(0xe0, 0xc0, 0x60));
            p.drawRect(QRectF(2, 13, 20, 6));
            for (int i = 0; i < 6; ++i)
                p.drawLine(QPointF(4 + i * 3.2, 13), QPointF(4 + i * 3.2, i % 2 ? 15 : 16.5));
            p.setPen(QPen(kOrange, 1.6));
            p.drawLine(QPointF(5, 9), QPointF(19, 9));
            p.setBrush(kOrange);
            p.drawEllipse(QPointF(5, 9), 2, 2);
            p.drawEllipse(QPointF(19, 9), 2, 2);
            break;
        case IconId::Hole:  // counterbored hole in section
            p.setPen(QPen(kInk, 1.3));
            p.setBrush(QColor(0x8c, 0x92, 0x9a));
            p.drawPolygon(QPolygonF{{2, 6}, {7, 6}, {7, 11}, {10, 11}, {10, 21}, {2, 21}});
            p.drawPolygon(QPolygonF{{22, 6}, {17, 6}, {17, 11}, {14, 11}, {14, 21}, {22, 21}});
            p.setPen(QPen(kOrange, 1.2, Qt::DashLine));
            p.drawLine(QPointF(12, 2), QPointF(12, 23));
            break;
        case IconId::Component:  // green board with a chip and 4 holes
            p.setPen(QPen(kInk, 1.2));
            p.setBrush(QColor(0x1f, 0x7a, 0x3a));
            p.drawRoundedRect(QRectF(2, 5, 20, 14), 2, 2);
            p.setBrush(QColor(0x2a, 0x2c, 0x30));
            p.drawRect(QRectF(9, 9, 6, 6));
            p.setBrush(kInk);
            for (QPointF c : {QPointF(4.5, 7.5), QPointF(19.5, 7.5), QPointF(4.5, 16.5), QPointF(19.5, 16.5)})
                p.drawEllipse(c, 1.1, 1.1);
            break;
        case IconId::Bed:  // build plate grid in perspective
            p.setPen(QPen(kBlue, 1.2));
            p.drawPolygon(QPolygonF{{2, 18}, {8, 8}, {22, 8}, {16, 18}});
            for (int k = 1; k < 3; ++k) {
                p.drawLine(QPointF(2 + 2 * k * 1.0 + 4 * k / 2.0, 18 - 10.0 * k / 3), QPointF(16 + 6.0 * k / 3, 18 - 10.0 * k / 3));
                p.drawLine(QPointF(2 + 14.0 * k / 3, 18), QPointF(8 + 14.0 * k / 3, 8));
            }
            p.setPen(QPen(kInk, 1.2, Qt::DashLine));
            p.drawLine(QPointF(8, 8), QPointF(8, 2));
            p.drawLine(QPointF(22, 8), QPointF(22, 2));
            break;
        case IconId::LayFlat:  // a part turning onto the plate
            p.setPen(QPen(kInk, 1.3));
            p.setBrush(QColor(0x3d, 0x7c, 0xc4));
            p.drawPolygon(QPolygonF{{6, 19}, {14, 19}, {14, 13}, {6, 13}});
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(kOrange, 1.4));
            p.drawArc(QRectF(9, 3, 12, 12), 0 * 16, 120 * 16);
            p.drawLine(QPointF(21, 9), QPointF(19, 6));
            p.setPen(QPen(kBlue, 1.6));
            p.drawLine(QPointF(2, 21), QPointF(22, 21));
            break;
        case IconId::Pegs:  // split part with two pins
            p.setPen(QPen(kInk, 1.3));
            p.setBrush(QColor(0x3d, 0x7c, 0xc4));
            p.drawRect(QRectF(3, 14, 18, 7));
            p.setBrush(QColor(0x5c, 0xd6, 0x8a));
            p.drawRect(QRectF(3, 3, 18, 7));
            p.setBrush(kOrange);
            p.drawRect(QRectF(6, 10, 3, 4));
            p.drawRect(QRectF(15, 10, 3, 4));
            break;
        case IconId::Enclosure:  // open box with a lid
            box(QColor(0x3d, 0x7c, 0xc4));
            p.setPen(QPen(kOrange, 1.4));
            p.drawLine(QPointF(4, 5), QPointF(12, 1));
            p.drawLine(QPointF(12, 1), QPointF(20, 5));
            break;
        case IconId::Ghost:  // before (dashed) / after (solid)
            p.setPen(QPen(QColor(0xa0, 0xa6, 0xae), 1.2, Qt::DashLine));
            p.drawRect(QRectF(3, 8, 12, 12));
            p.setPen(QPen(kBlue, 1.6));
            p.drawRect(QRectF(9, 4, 12, 16));
            break;
        case IconId::Repair:  // wrench
            p.setPen(QPen(kOrange, 2.2, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(5, 19), QPointF(14, 10));
            p.setPen(QPen(kOrange, 1.8));
            p.drawArc(QRectF(12, 3, 9, 9), 200 * 16, 280 * 16);
            break;
        case IconId::Profile:  // simple / full toggle
            p.setPen(QPen(kInk, 1.3));
            p.setBrush(QColor(0x5c, 0xd6, 0x8a));
            p.drawRoundedRect(QRectF(2, 7, 20, 10), 5, 5);
            p.setBrush(kInk);
            p.drawEllipse(QPointF(17, 12), 3.5, 3.5);
            break;
        case IconId::Recorder:  // record dot + lines
            p.setPen(Qt::NoPen);
            p.setBrush(kRed);
            p.drawEllipse(QPointF(7, 12), 4.5, 4.5);
            p.setPen(QPen(kInk, 1.4));
            p.drawLine(QPointF(14, 8), QPointF(22, 8));
            p.drawLine(QPointF(14, 12), QPointF(22, 12));
            p.drawLine(QPointF(14, 16), QPointF(20, 16));
            break;
        case IconId::Tutor:  // speech bubble with a question mark
            p.setPen(QPen(kBlue, 1.5));
            p.drawRoundedRect(QRectF(2, 3, 20, 14), 4, 4);
            p.drawLine(QPointF(7, 17), QPointF(5, 22));
            p.drawLine(QPointF(5, 22), QPointF(11, 17));
            text("?", kBlue);
            break;
        case IconId::Learn:
            p.setPen(QPen(kInk, 1.3));
            p.setBrush(QColor(0x4a, 0xa3, 0xff));
            p.drawPolygon(QPolygonF{{2, 9}, {12, 4}, {22, 9}, {12, 14}});
            p.setBrush(Qt::NoBrush);
            p.drawPolyline(QPolygonF{{6, 11}, {6, 17}, {12, 20}, {18, 17}, {18, 11}});
            p.drawLine(QPointF(22, 9), QPointF(22, 16));
            break;
        case IconId::Hint:
            p.setPen(QPen(QColor(0xff, 0xd0, 0x40), 1.6));
            p.drawEllipse(QPointF(12, 10), 6.5, 6.5);
            p.drawLine(QPointF(9.5, 18), QPointF(14.5, 18));
            p.drawLine(QPointF(10, 21), QPointF(14, 21));
            break;
        case IconId::Warning:
            p.setPen(QPen(Colors::Warning, 1.6));
            p.drawPolygon(QPolygonF{{12, 3}, {22, 21}, {2, 21}});
            text("!", Colors::Warning);
            break;
        case IconId::Error:
            p.setPen(QPen(Colors::Error, 1.6));
            p.drawEllipse(QPointF(12, 12), 9, 9);
            text("×", Colors::Error);
            break;
    }
}

}  // namespace

QIcon icon(IconId id)
{
    static QHash<int, QIcon> cache;
    const int key = static_cast<int>(id);
    if (auto it = cache.find(key); it != cache.end())
        return *it;
    QIcon ic;
    for (int size : {24, 48}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(size / 24.0, size / 24.0);
        paintIcon(p, id);
        p.end();
        ic.addPixmap(pm);
    }
    cache.insert(key, ic);
    return ic;
}

}  // namespace silex::ui
