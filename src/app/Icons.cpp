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
