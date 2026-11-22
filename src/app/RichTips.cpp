#include "RichTips.h"

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QMap>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace silex::ui {

namespace {

const QColor kInk(0xdd, 0xe1, 0xe6), kBlue(0x4a, 0xa3, 0xff), kOrange(0xf0, 0xa0, 0x40), kPart(0x3d, 0x7c, 0xc4),
    kGrey(0x8c, 0x92, 0x9a), kGreen(0x5c, 0xd6, 0x8a), kRed(0xff, 0x5a, 0x50);

using Draw = std::function<void(QPainter&, const QRectF&)>;

// ---------------------------------------------------------------- drawing helpers (isometric-ish)
QPointF iso(const QRectF& r, double x, double y, double z)
{
    // x right-down, y left-down, z up; unit = r.width() / 100
    const double u = r.width() / 100.0;
    return QPointF(r.center().x() + (x - y) * 0.87 * u, r.center().y() + 18 * u + (x + y) * 0.5 * u - z * u);
}

void prism(QPainter& p, const QRectF& r, double x0, double y0, double z0, double dx, double dy, double dz, QColor c)
{
    auto P = [&](double x, double y, double z) { return iso(r, x, y, z); };
    const QPolygonF top{P(x0, y0, z0 + dz), P(x0 + dx, y0, z0 + dz), P(x0 + dx, y0 + dy, z0 + dz), P(x0, y0 + dy, z0 + dz)};
    const QPolygonF right{P(x0 + dx, y0, z0), P(x0 + dx, y0 + dy, z0), P(x0 + dx, y0 + dy, z0 + dz), P(x0 + dx, y0, z0 + dz)};
    const QPolygonF left{P(x0, y0 + dy, z0), P(x0 + dx, y0 + dy, z0), P(x0 + dx, y0 + dy, z0 + dz), P(x0, y0 + dy, z0 + dz)};
    p.setPen(QPen(kInk, 1.0));
    p.setBrush(c.darker(125));
    p.drawPolygon(right);
    p.setBrush(c.darker(105));
    p.drawPolygon(left);
    p.setBrush(c.lighter(125));
    p.drawPolygon(top);
}

void block(QPainter& p, const QRectF& r, double h = 18, QColor c = kPart) { prism(p, r, -28, -28, 0, 56, 56, h, c); }

void flatShape(QPainter& p, const QRectF& r, const QPolygonF& pts2d, QColor pen = kBlue, double z = 0)
{
    QPolygonF poly;
    for (const QPointF& q : pts2d)
        poly << iso(r, q.x(), q.y(), z);
    p.setPen(QPen(pen, 1.6));
    p.setBrush(QColor(pen.red(), pen.green(), pen.blue(), 40));
    p.drawPolygon(poly);
}

void topEllipse(QPainter& p, const QRectF& r, double x, double y, double z, double rad, QColor fill)
{
    QPolygonF e;
    for (int k = 0; k < 24; ++k) {
        const double a = k * M_PI / 12;
        e << iso(r, x + rad * std::cos(a), y + rad * std::sin(a), z);
    }
    p.setPen(QPen(kInk, 1.0));
    p.setBrush(fill);
    p.drawPolygon(e);
}

void cyl(QPainter& p, const QRectF& r, double x, double y, double z, double rad, double h, QColor c)
{
    QPolygonF side;
    for (int k = 0; k <= 12; ++k) {
        const double a = -M_PI / 4 + k * M_PI / 12;
        side << iso(r, x + rad * std::cos(a), y + rad * std::sin(a), z);
    }
    for (int k = 12; k >= 0; --k) {
        const double a = -M_PI / 4 + k * M_PI / 12;
        side << iso(r, x + rad * std::cos(a), y + rad * std::sin(a), z + h);
    }
    p.setPen(QPen(kInk, 1.0));
    p.setBrush(c.darker(115));
    p.drawPolygon(side);
    topEllipse(p, r, x, y, z + h, rad, c.lighter(120));
}

void line2d(QPainter& p, const QRectF& r, QPointF a, QPointF b, QColor c = kBlue, double w = 1.8, Qt::PenStyle st = Qt::SolidLine)
{
    p.setPen(QPen(c, w, st, Qt::RoundCap));
    p.drawLine(QPointF(r.left() + a.x() * r.width() / 100, r.top() + a.y() * r.height() / 100),
               QPointF(r.left() + b.x() * r.width() / 100, r.top() + b.y() * r.height() / 100));
}

QPointF at2d(const QRectF& r, double x, double y) { return {r.left() + x * r.width() / 100, r.top() + y * r.height() / 100}; }

void dot(QPainter& p, const QRectF& r, double x, double y, QColor c = kInk)
{
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(at2d(r, x, y), 2.6, 2.6);
}

struct Card
{
    QString fr, en;           ///< bilingual name
    QString sayFr, sayEn;     ///< maker sentence
    Draw before, after;
};

QMap<QString, Card> cards()
{
    QMap<QString, Card> c;
    auto square = [](double s = 26) { return QPolygonF{{-s, -s}, {s, -s}, {s, s}, {-s, s}}; };
    // ---------------------------------------------------------------- model
    c["Sketch"] = {QString::fromUtf8("Esquisse"), "Sketch",
                   QString::fromUtf8("Dessine un profil 2D à plat sur un plan ou une face : c'est le point de départ de presque tout."),
                   "Draw a flat 2D profile on a plane or a face: the starting point of almost everything.",
                   [](QPainter& p, const QRectF& r) { flatShape(p, r, {{-30, -30}, {30, -30}, {30, 30}, {-30, 30}}, kGrey); },
                   [=](QPainter& p, const QRectF& r) {
                       flatShape(p, r, {{-30, -30}, {30, -30}, {30, 30}, {-30, 30}}, kGrey);
                       flatShape(p, r, {{-20, -20}, {20, -20}, {20, 5}, {0, 20}, {-20, 5}}, kBlue);
                   }};
    c["Extrude"] = {"Extrusion", "Extrude",
                    QString::fromUtf8("Donne de l'épaisseur à un profil d'esquisse : il « pousse » tout droit."),
                    "Gives thickness to a sketch profile: it grows straight up.",
                    [=](QPainter& p, const QRectF& r) { flatShape(p, r, square(24)); },
                    [](QPainter& p, const QRectF& r) { prism(p, r, -24, -24, 0, 48, 48, 30, kPart); }};
    c["Press/Pull"] = {"Pousser / Tirer", "Press / Pull",
                       QString::fromUtf8("Attrape une face et pousse-la ou tire-la : la pièce grandit ou se creuse, sans esquisse."),
                       "Grab a face and push or pull it: the part grows or digs in, no sketch needed.",
                       [](QPainter& p, const QRectF& r) { block(p, r, 14); },
                       [](QPainter& p, const QRectF& r) {
                           block(p, r, 30);
                           line2d(p, r, {50, 12}, {50, 2}, kOrange, 2);
                       }};
    c["Revolve"] = {QString::fromUtf8("Révolution"), "Revolve",
                    QString::fromUtf8("Fait tourner un demi-profil autour d'un axe : idéal pour les poulies, bouchons, vases."),
                    "Spins a half profile around an axis: ideal for pulleys, caps, vases.",
                    [](QPainter& p, const QRectF& r) {
                        line2d(p, r, {30, 10}, {30, 90}, kOrange, 1.4, Qt::DashLine);
                        p.setPen(QPen(kBlue, 1.8));
                        p.drawPolyline(QPolygonF{at2d(r, 30, 20), at2d(r, 60, 20), at2d(r, 60, 45), at2d(r, 45, 55), at2d(r, 45, 80), at2d(r, 30, 80)});
                    },
                    [](QPainter& p, const QRectF& r) {
                        cyl(p, r, 0, 0, 0, 22, 14, kPart);
                        cyl(p, r, 0, 0, 14, 12, 22, kPart);
                    }};
    c["Sweep"] = {"Balayage", "Sweep", QString::fromUtf8("Pousse une section le long d'une trajectoire : tuyaux, poignées, coudes."),
                  "Pushes a section along a path: pipes, handles, elbows.",
                  [](QPainter& p, const QRectF& r) {
                      QPainterPath path(at2d(r, 15, 80));
                      path.cubicTo(at2d(r, 20, 20), at2d(r, 60, 20), at2d(r, 85, 35));
                      p.setPen(QPen(kOrange, 1.6, Qt::DashLine));
                      p.setBrush(Qt::NoBrush);
                      p.drawPath(path);
                      p.setPen(QPen(kBlue, 1.8));
                      p.drawEllipse(at2d(r, 15, 80), 7, 7);
                  },
                  [](QPainter& p, const QRectF& r) {
                      QPainterPath path(at2d(r, 15, 80));
                      path.cubicTo(at2d(r, 20, 20), at2d(r, 60, 20), at2d(r, 85, 35));
                      p.setPen(QPen(kPart.lighter(120), 12, Qt::SolidLine, Qt::RoundCap));
                      p.setBrush(Qt::NoBrush);
                      p.drawPath(path);
                  }};
    c["Loft"] = {"Lissage", "Loft", QString::fromUtf8("Relie deux sections différentes (carré → rond) par une forme douce."),
                 "Blends two different sections (square -> round) with a smooth shape.",
                 [=](QPainter& p, const QRectF& r) {
                     flatShape(p, r, square(24));
                     topEllipse(p, r, 0, 0, 40, 16, QColor(0x4a, 0xa3, 0xff, 50));
                 },
                 [](QPainter& p, const QRectF& r) {
                     QPolygonF s{iso(r, 24, -24, 0), iso(r, 24, 24, 0), iso(r, -24, 24, 0), iso(r, -11, 11, 40), iso(r, 11, -11, 40)};
                     p.setPen(QPen(kInk, 1));
                     p.setBrush(kPart);
                     p.drawPolygon(s);
                     topEllipse(p, r, 0, 0, 40, 16, kPart.lighter(125));
                 }};
    c["Fillet"] = {QString::fromUtf8("Congé"), "Fillet", QString::fromUtf8("Arrondit les arêtes vives : plus doux en main, plus solide, plus beau."),
                   "Rounds the sharp edges: softer to hold, stronger, nicer.",
                   [](QPainter& p, const QRectF& r) { block(p, r, 24); },
                   [](QPainter& p, const QRectF& r) {
                       block(p, r, 24);
                       p.setPen(QPen(kOrange, 3, Qt::SolidLine, Qt::RoundCap));
                       p.drawLine(iso(r, 28, -28, 24), iso(r, 28, 28, 24));
                   }};
    c["Chamfer"] = {"Chanfrein", "Chamfer", QString::fromUtf8("Casse une arête en biseau : parfait pour l'impression 3D (pas de porte-à-faux)."),
                    "Bevels an edge: perfect for 3D printing (no overhang).",
                    [](QPainter& p, const QRectF& r) { block(p, r, 24); },
                    [](QPainter& p, const QRectF& r) {
                        prism(p, r, -28, -28, 0, 56, 56, 18, kPart);
                        prism(p, r, -28, -28, 18, 48, 56, 6, kPart);
                    }};
    c["Shell"] = {"Coque", "Shell", QString::fromUtf8("Creuse un volume plein en gardant une épaisseur de paroi constante."),
                  "Hollows a solid volume while keeping a constant wall thickness.",
                  [](QPainter& p, const QRectF& r) { block(p, r, 26); },
                  [](QPainter& p, const QRectF& r) {
                      block(p, r, 26);
                      prism(p, r, -22, -22, 4, 44, 44, 22, QColor(0x22, 0x26, 0x2c));
                  }};
    c["Split Body"] = {QString::fromUtf8("Scinder le corps"), "Split Body",
                       QString::fromUtf8("Coupe une pièce en deux par un plan : boîte + couvercle, ou une grande pièce en deux impressions."),
                       "Cuts a part in two with a plane: box + lid, or a large part in two prints.",
                       [](QPainter& p, const QRectF& r) { block(p, r, 30); },
                       [](QPainter& p, const QRectF& r) {
                           prism(p, r, -28, -28, 0, 56, 56, 16, kPart);
                           prism(p, r, -28, -28, 24, 56, 56, 12, kGreen.darker(140));
                       }};
    c["Delete Face"] = {"Supprimer la face", "Delete Face",
                        QString::fromUtf8("Efface un trou, un bossage ou un congé, même sur un STEP sans historique : la pièce se referme."),
                        "Erases a hole, a boss or a fillet, even on a STEP without history: the part heals.",
                        [](QPainter& p, const QRectF& r) {
                            block(p, r, 18);
                            cyl(p, r, 0, 0, 18, 10, 12, kOrange);
                        },
                        [](QPainter& p, const QRectF& r) { block(p, r, 18); }};
    c["Move/Copy"] = {QString::fromUtf8("Déplacer / Copier"), "Move / Copy", QString::fromUtf8("Déplace ou tourne un corps avec les flèches du gizmo, ou en fait une copie."),
                      "Moves or turns a body with the gizmo arrows, or makes a copy.",
                      [](QPainter& p, const QRectF& r) { prism(p, r, -30, -10, 0, 24, 24, 20, kPart); },
                      [](QPainter& p, const QRectF& r) {
                          prism(p, r, -30, -10, 0, 24, 24, 20, kGrey);
                          prism(p, r, 8, -10, 0, 24, 24, 20, kPart);
                      }};
    c["Align"] = {"Aligner", "Align", QString::fromUtf8("Colle une face contre une autre (ou deux axes) : l'assemblage se met en place tout seul."),
                  "Puts a face against another one (or two axes): the assembly falls into place.",
                  [](QPainter& p, const QRectF& r) {
                      prism(p, r, -30, -20, 0, 30, 40, 14, kPart);
                      prism(p, r, 10, -10, 22, 20, 20, 10, kGreen.darker(140));
                  },
                  [](QPainter& p, const QRectF& r) {
                      prism(p, r, -30, -20, 0, 30, 40, 14, kPart);
                      prism(p, r, -25, -10, 14, 20, 20, 10, kGreen.darker(140));
                  }};
    c["Lay Flat on Bed"] = {"Poser à plat sur le plateau", "Lay Flat on Bed",
                            QString::fromUtf8("Clique la face qui doit toucher le plateau : la pièce se couche dessus, prête à trancher."),
                            "Click the face that must touch the plate: the part lies down on it, ready to slice.",
                            [](QPainter& p, const QRectF& r) {
                                flatShape(p, r, {{-40, -40}, {40, -40}, {40, 40}, {-40, 40}}, kBlue);
                                prism(p, r, -8, -8, 0, 16, 16, 44, kPart);
                            },
                            [](QPainter& p, const QRectF& r) {
                                flatShape(p, r, {{-40, -40}, {40, -40}, {40, 40}, {-40, 40}}, kBlue);
                                prism(p, r, -22, -8, 0, 44, 16, 16, kPart);
                            }};
    c["Mirror"] = {"Symétrie", "Mirror", QString::fromUtf8("Ne modélise qu'une moitié : la symétrie crée l'autre, et suit chaque modification."),
                   "Model only one half: the mirror makes the other one and follows every change.",
                   [](QPainter& p, const QRectF& r) {
                       prism(p, r, -2, -20, 0, 30, 40, 16, kPart);
                       line2d(p, r, {50, 5}, {50, 95}, kOrange, 1.2, Qt::DashLine);
                   },
                   [](QPainter& p, const QRectF& r) {
                       prism(p, r, -28, -20, 0, 30, 40, 16, kPart);
                       prism(p, r, -2, -20, 0, 30, 40, 16, kPart);
                   }};
    c["Linear Pattern"] = {QString::fromUtf8("Répétition linéaire"), "Linear Pattern",
                           QString::fromUtf8("Répète un trou ou un bossage en ligne : change le nombre, tout se recalcule."),
                           "Repeats a hole or a boss in a row: change the count, everything updates.",
                           [](QPainter& p, const QRectF& r) {
                               prism(p, r, -36, -14, 0, 72, 28, 8, kPart);
                               cyl(p, r, -24, 0, 8, 6, 8, kOrange);
                           },
                           [](QPainter& p, const QRectF& r) {
                               prism(p, r, -36, -14, 0, 72, 28, 8, kPart);
                               for (int k = 0; k < 4; ++k)
                                   cyl(p, r, -24 + 16 * k, 0, 8, 6, 8, kOrange);
                           }};
    c["Circular Pattern"] = {QString::fromUtf8("Répétition circulaire"), "Circular Pattern",
                             QString::fromUtf8("Répète autour d'un axe : trous de bride, rayons de roue, dents."),
                             "Repeats around an axis: flange holes, wheel spokes, teeth.",
                             [](QPainter& p, const QRectF& r) {
                                 cyl(p, r, 0, 0, 0, 34, 8, kPart);
                                 topEllipse(p, r, 22, 0, 8, 5, QColor(0x22, 0x26, 0x2c));
                             },
                             [](QPainter& p, const QRectF& r) {
                                 cyl(p, r, 0, 0, 0, 34, 8, kPart);
                                 for (int k = 0; k < 6; ++k)
                                     topEllipse(p, r, 22 * std::cos(k * M_PI / 3), 22 * std::sin(k * M_PI / 3), 8, 5, QColor(0x22, 0x26, 0x2c));
                             }};
    c["Combine"] = {"Combiner", "Combine", QString::fromUtf8("Fusionne, soustrait ou intersecte des corps : la boîte à outils booléenne."),
                    "Merges, subtracts or intersects bodies: the boolean toolbox.",
                    [](QPainter& p, const QRectF& r) {
                        block(p, r, 18);
                        cyl(p, r, 10, 10, 0, 14, 34, kOrange);
                    },
                    [](QPainter& p, const QRectF& r) {
                        block(p, r, 18);
                        topEllipse(p, r, 10, 10, 18, 14, QColor(0x22, 0x26, 0x2c));
                    }};
    c["Thread"] = {"Filetage", "Thread", QString::fromUtf8("Un vrai filetage ISO imprimable, avec le jeu FDM : la vis imprimée rentre sans forcer."),
                   "A real printable ISO thread, with the FDM play: the printed screw goes in without forcing.",
                   [](QPainter& p, const QRectF& r) { cyl(p, r, 0, 0, 0, 14, 44, kGrey); },
                   [](QPainter& p, const QRectF& r) {
                       cyl(p, r, 0, 0, 0, 14, 44, kGrey);
                       p.setPen(QPen(kOrange, 1.6));
                       for (int k = 0; k < 6; ++k)
                           p.drawLine(iso(r, -10, 10, 6 + 6 * k), iso(r, 10, -10, 9 + 6 * k));
                   }};
    c["Box"] = {QString::fromUtf8("Boîte"), "Box", QString::fromUtf8("Un pavé paramétrique en un clic, posé sur un plan ou une face."),
                "A parametric block in one click, placed on a plane or a face.",
                [](QPainter& p, const QRectF& r) { flatShape(p, r, {{-30, -30}, {30, -30}, {30, 30}, {-30, 30}}, kGrey); },
                [](QPainter& p, const QRectF& r) { prism(p, r, -20, -20, 0, 40, 40, 26, kPart); }};
    c["Cylinder"] = {"Cylindre", "Cylinder", QString::fromUtf8("Un cylindre paramétrique en un clic : bossage, axe, ou perçage en mode Cut."),
                     "A parametric cylinder in one click: boss, shaft, or hole in Cut mode.",
                     [](QPainter& p, const QRectF& r) { flatShape(p, r, {{-30, -30}, {30, -30}, {30, 30}, {-30, 30}}, kGrey); },
                     [](QPainter& p, const QRectF& r) { cyl(p, r, 0, 0, 0, 20, 30, kPart); }};
    c["Rib"] = {"Nervure", "Rib", QString::fromUtf8("Une simple ligne devient une paroi de renfort entre deux faces : l'angle ne plie plus."),
                "A single line becomes a reinforcing wall between two faces: the corner stops bending.",
                [](QPainter& p, const QRectF& r) {
                    prism(p, r, -30, -10, 0, 60, 20, 6, kPart);
                    prism(p, r, -30, -10, 6, 6, 20, 34, kPart);
                },
                [](QPainter& p, const QRectF& r) {
                    prism(p, r, -30, -10, 0, 60, 20, 6, kPart);
                    prism(p, r, -30, -10, 6, 6, 20, 34, kPart);
                    QPolygonF rib{iso(r, -24, 0, 6), iso(r, 10, 0, 6), iso(r, -24, 0, 30)};
                    p.setBrush(kOrange);
                    p.drawPolygon(rib);
                }};
    c["Hole / Insert"] = {QString::fromUtf8("Perçage / Insert"), "Hole / Insert",
                          QString::fromUtf8("Perçage normalisé en un clic : passage, lamage, fraisage, insert laiton ou écrou prisonnier, aux bonnes cotes."),
                          "A standard hole in one click: clearance, counterbore, countersink, brass insert or nut trap, at the right size.",
                          [](QPainter& p, const QRectF& r) { block(p, r, 18); },
                          [](QPainter& p, const QRectF& r) {
                              block(p, r, 18);
                              topEllipse(p, r, 0, 0, 18, 12, QColor(0x22, 0x26, 0x2c));
                              topEllipse(p, r, 0, 0, 18, 6, QColor(0x10, 0x12, 0x15));
                          }};
    c["Insert Component"] = {QString::fromUtf8("Insérer un composant"), "Insert Component",
                             QString::fromUtf8("Pose une carte (Raspberry Pi, ESP32…) aux vraies cotes : ses trous guident tes entretoises."),
                             "Drops a board (Raspberry Pi, ESP32...) at its real size: its holes guide your standoffs.",
                             [](QPainter& p, const QRectF& r) { prism(p, r, -36, -26, 0, 72, 52, 6, kGrey); },
                             [](QPainter& p, const QRectF& r) {
                                 prism(p, r, -36, -26, 0, 72, 52, 6, kGrey);
                                 prism(p, r, -26, -18, 10, 52, 36, 3, QColor(0x1f, 0x7a, 0x3a));
                                 prism(p, r, -6, -6, 13, 12, 12, 3, QColor(0x2a, 0x2c, 0x30));
                             }};
    c["Enclosure Wizard"] = {QString::fromUtf8("Assistant boîtier"), "Enclosure Wizard",
                             QString::fromUtf8("Un boîtier en 3 clics, mais avec une timeline commentée que tu peux modifier étape par étape."),
                             "An enclosure in 3 clicks, with a commented timeline you can edit step by step.",
                             [](QPainter& p, const QRectF& r) { flatShape(p, r, {{-30, -20}, {30, -20}, {30, 20}, {-30, 20}}, kGrey); },
                             [](QPainter& p, const QRectF& r) {
                                 prism(p, r, -30, -20, 0, 60, 40, 22, kPart);
                                 prism(p, r, -26, -16, 3, 52, 32, 19, QColor(0x22, 0x26, 0x2c));
                                 prism(p, r, -30, -20, 30, 60, 40, 4, kGreen.darker(140));
                             }};
    c["Measure"] = {"Mesurer", "Measure", QString::fromUtf8("Distance, angle, diamètre : clique deux éléments, Silex donne la cote."),
                    "Distance, angle, diameter: click two entities, Silex gives the dimension.",
                    [](QPainter& p, const QRectF& r) { block(p, r, 20); },
                    [](QPainter& p, const QRectF& r) {
                        block(p, r, 20);
                        p.setPen(QPen(kOrange, 1.6));
                        p.drawLine(iso(r, -28, 34, 0), iso(r, 28, 34, 0));
                        p.drawText(iso(r, 0, 44, 0) + QPointF(-14, 6), "56 mm");
                    }};
    c["Interference"] = {QString::fromUtf8("Interférences"), "Interference",
                         QString::fromUtf8("Montre en rouge les pièces qui se chevauchent avant d'imprimer : fini les couvercles bloqués."),
                         "Shows overlapping parts in red before printing: no more stuck lids.",
                         [](QPainter& p, const QRectF& r) {
                             prism(p, r, -30, -20, 0, 40, 40, 20, kPart);
                             prism(p, r, -2, -10, 6, 30, 20, 20, kGreen.darker(140));
                         },
                         [](QPainter& p, const QRectF& r) {
                             prism(p, r, -30, -20, 0, 40, 40, 20, kPart);
                             prism(p, r, -2, -10, 6, 30, 20, 20, kGreen.darker(140));
                             prism(p, r, -2, -10, 6, 12, 20, 14, kRed);
                         }};
    c["Section"] = {"Coupe", "Section", QString::fromUtf8("Regarde à l'intérieur de la pièce : épaisseurs, trous, cavités."),
                    "Look inside the part: thicknesses, holes, cavities.",
                    [](QPainter& p, const QRectF& r) { block(p, r, 26); },
                    [](QPainter& p, const QRectF& r) {
                        prism(p, r, -28, -28, 0, 56, 28, 26, kPart);
                        flatShape(p, r, {{-28, 0}, {28, 0}, {28, 0.01}, {-28, 0.01}}, kOrange);
                    }};
    c["Printability"] = {QString::fromUtf8("Imprimabilité"), "Printability",
                         QString::fromUtf8("Colorie les porte-à-faux et les parois trop fines selon la direction d'impression."),
                         "Colors the overhangs and too-thin walls for the build direction.",
                         [](QPainter& p, const QRectF& r) {
                             prism(p, r, -10, -10, 0, 20, 20, 24, kPart);
                             prism(p, r, -10, -10, 24, 40, 20, 8, kPart);
                         },
                         [](QPainter& p, const QRectF& r) {
                             prism(p, r, -10, -10, 0, 20, 20, 24, kGreen.darker(130));
                             prism(p, r, -10, -10, 24, 40, 20, 8, kRed);
                         }};
    c["Offset Plane"] = {QString::fromUtf8("Plan décalé"), "Offset Plane", QString::fromUtf8("Un plan parallèle à distance donnée : pour esquisser en hauteur."),
                         "A parallel plane at a given distance: to sketch higher up.",
                         [](QPainter& p, const QRectF& r) { flatShape(p, r, {{-30, -30}, {30, -30}, {30, 30}, {-30, 30}}, kGrey); },
                         [](QPainter& p, const QRectF& r) {
                             flatShape(p, r, {{-30, -30}, {30, -30}, {30, 30}, {-30, 30}}, kGrey);
                             flatShape(p, r, {{-30, -30}, {30, -30}, {30, 30}, {-30, 30}}, kOrange, 30);
                         }};
    c["Angled Plane"] = c["Offset Plane"];
    c["Angled Plane"].fr = QString::fromUtf8("Plan incliné");
    c["Angled Plane"].en = "Angled Plane";
    c["Angled Plane"].sayFr = QString::fromUtf8("Un plan tourné d'un angle autour d'un axe : pour les faces en pente.");
    c["Angled Plane"].sayEn = "A plane turned by an angle around an axis: for sloped faces.";
    c["Midplane"] = c["Offset Plane"];
    c["Midplane"].fr = "Plan milieu";
    c["Midplane"].en = "Midplane";
    c["Midplane"].sayFr = QString::fromUtf8("Le plan exactement entre deux faces parallèles : parfait pour une symétrie.");
    c["Midplane"].sayEn = "The plane exactly between two parallel faces: perfect for a mirror.";
    c["Drawing"] = {"Mise en plan", "Drawing", QString::fromUtf8("Les vues 2D cotées de ta pièce, prêtes à imprimer en PDF."),
                    "The dimensioned 2D views of your part, ready to print as a PDF.",
                    [](QPainter& p, const QRectF& r) { block(p, r, 20); },
                    [](QPainter& p, const QRectF& r) {
                        p.setPen(QPen(kInk, 1.2));
                        p.setBrush(Qt::NoBrush);
                        p.drawRect(QRectF(at2d(r, 10, 15), at2d(r, 45, 50)));
                        p.drawRect(QRectF(at2d(r, 55, 15), at2d(r, 90, 50)));
                        p.drawRect(QRectF(at2d(r, 10, 58), at2d(r, 45, 85)));
                        line2d(p, r, {10, 92}, {45, 92}, kOrange, 1.2);
                    }};
    c["Parameters"] = {QString::fromUtf8("Paramètres"), "Parameters", QString::fromUtf8("Des noms pour tes cotes (largeur, paroi…) : change une valeur, toute la pièce suit."),
                       "Names for your dimensions (width, wall...): change one value, the whole part follows.",
                       [](QPainter& p, const QRectF& r) { prism(p, r, -20, -20, 0, 40, 40, 16, kPart); },
                       [](QPainter& p, const QRectF& r) { prism(p, r, -34, -20, 0, 68, 40, 16, kPart); }};
    c["Fillet / Chamfer"] = c["Fillet"];
    c["Fillet / Chamfer"].fr = QString::fromUtf8("Congé / Chanfrein");
    c["Fillet / Chamfer"].en = "Fillet / Chamfer";
    c["Export STL/3MF"] = {"Exporter STL / 3MF", "Export STL / 3MF", QString::fromUtf8("Le fichier pour ton trancheur (Cura, PrusaSlicer, Bambu Studio)."),
                           "The file for your slicer (Cura, PrusaSlicer, Bambu Studio).",
                           [](QPainter& p, const QRectF& r) { block(p, r, 20); },
                           [](QPainter& p, const QRectF& r) {
                               p.setPen(QPen(kGrey, 0.8));
                               for (int k = 0; k < 6; ++k)
                                   p.drawLine(iso(r, -28 + k * 11, -28, 20), iso(r, -28, -28 + k * 11, 20));
                               block(p, r, 20, QColor(0x3d, 0x7c, 0xc4, 120));
                           }};
    // ---------------------------------------------------------------- sketch
    auto rectAfter = [](QPainter& p, const QRectF& r) {
        p.setPen(QPen(kBlue, 1.8));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(at2d(r, 20, 25), at2d(r, 80, 75)));
        dot(p, r, 20, 75);
        dot(p, r, 80, 25);
    };
    c["sk:Line"] = {"Ligne", "Line", QString::fromUtf8("Clique point après point : les traits s'enchaînent et se ferment en profil."),
                    "Click point after point: the lines chain and close into a profile.",
                    [](QPainter& p, const QRectF& r) {
                        dot(p, r, 20, 75);
                        dot(p, r, 80, 30);
                    },
                    [](QPainter& p, const QRectF& r) {
                        line2d(p, r, {20, 75}, {80, 30});
                        dot(p, r, 20, 75);
                        dot(p, r, 80, 30);
                    }};
    c["sk:Rectangle"] = {"Rectangle", "Rectangle", QString::fromUtf8("Deux coins opposés suffisent : les côtés restent horizontaux et verticaux."),
                         "Two opposite corners are enough: the sides stay horizontal and vertical.",
                         [](QPainter& p, const QRectF& r) {
                             dot(p, r, 20, 75);
                             dot(p, r, 80, 25);
                         },
                         rectAfter};
    c["sk:Center Rectangle"] = {"Rectangle par le centre", "Center Rectangle",
                                QString::fromUtf8("Le centre puis un coin : la pièce reste symétrique quand tu changes la cote."),
                                "The center then a corner: the part stays symmetric when the size changes.",
                                [](QPainter& p, const QRectF& r) { dot(p, r, 50, 50, kOrange); }, rectAfter};
    c["sk:Polygon"] = {"Polygone", "Polygon", QString::fromUtf8("Hexagone d'écrou, octogone… : le centre, puis un sommet."),
                       "Nut hexagon, octagon...: the center, then one vertex.",
                       [](QPainter& p, const QRectF& r) { dot(p, r, 50, 50, kOrange); },
                       [](QPainter& p, const QRectF& r) {
                           QPolygonF h;
                           for (int k = 0; k < 6; ++k)
                               h << at2d(r, 50 + 30 * std::cos(k * M_PI / 3), 50 + 30 * std::sin(k * M_PI / 3));
                           p.setPen(QPen(kBlue, 1.8));
                           p.setBrush(Qt::NoBrush);
                           p.drawPolygon(h);
                       }};
    c["sk:Circle"] = {"Cercle", "Circle", QString::fromUtf8("Le centre puis le rayon : trous, bossages, axes."), "The center then the radius: holes, bosses, shafts.",
                      [](QPainter& p, const QRectF& r) { dot(p, r, 50, 50, kOrange); },
                      [](QPainter& p, const QRectF& r) {
                          p.setPen(QPen(kBlue, 1.8));
                          p.setBrush(Qt::NoBrush);
                          p.drawEllipse(at2d(r, 50, 50), r.width() * 0.28, r.width() * 0.28);
                      }};
    c["sk:Arc"] = {"Arc", "Arc", QString::fromUtf8("Centre, début, fin : pour les contours arrondis et les trajectoires."),
                   "Center, start, end: for rounded contours and paths.",
                   [](QPainter& p, const QRectF& r) {
                       dot(p, r, 50, 70, kOrange);
                       dot(p, r, 20, 70);
                       dot(p, r, 80, 70);
                   },
                   [](QPainter& p, const QRectF& r) {
                       p.setPen(QPen(kBlue, 1.8));
                       p.setBrush(Qt::NoBrush);
                       p.drawArc(QRectF(at2d(r, 20, 40), at2d(r, 80, 100)), 0, 180 * 16);
                   }};
    c["sk:Slot"] = {"Oblong", "Slot", QString::fromUtf8("Un trou allongé : il laisse du réglage au montage."), "An elongated hole: it leaves adjustment at assembly.",
                    [](QPainter& p, const QRectF& r) {
                        dot(p, r, 30, 50, kOrange);
                        dot(p, r, 70, 50, kOrange);
                    },
                    [](QPainter& p, const QRectF& r) {
                        p.setPen(QPen(kBlue, 1.8));
                        p.setBrush(Qt::NoBrush);
                        p.drawRoundedRect(QRectF(at2d(r, 18, 36), at2d(r, 82, 64)), r.height() * 0.14, r.height() * 0.14);
                    }};
    c["sk:Dimension"] = {"Cote", "Dimension", QString::fromUtf8("Fixe une longueur ou un diamètre : l'esquisse se cale à la bonne taille."),
                         "Sets a length or a diameter: the sketch snaps to the right size.",
                         [](QPainter& p, const QRectF& r) { line2d(p, r, {20, 60}, {75, 60}); },
                         [](QPainter& p, const QRectF& r) {
                             line2d(p, r, {20, 60}, {80, 60});
                             line2d(p, r, {20, 40}, {80, 40}, kOrange, 1.2);
                             p.setPen(kOrange);
                             p.drawText(at2d(r, 40, 34), "60");
                         }};
    c["sk:Project"] = {"Projeter", "Project", QString::fromUtf8("Ramène une arête du modèle dans l'esquisse : ton dessin suit la pièce."),
                       "Brings a model edge into the sketch: your drawing follows the part.",
                       [](QPainter& p, const QRectF& r) { prism(p, r, -20, -20, 0, 40, 40, 18, kPart); },
                       [](QPainter& p, const QRectF& r) {
                           prism(p, r, -20, -20, 0, 40, 40, 18, kPart);
                           flatShape(p, r, {{-20, -20}, {20, -20}, {20, 20}, {-20, 20}}, kOrange, 18);
                       }};
    c["sk:Construction"] = {"Construction", "Construction", QString::fromUtf8("Un trait d'aide en pointillé : il guide sans faire partie du profil."),
                            "A dashed helper line: it guides without being part of the profile.",
                            [](QPainter& p, const QRectF& r) { line2d(p, r, {15, 50}, {85, 50}); },
                            [](QPainter& p, const QRectF& r) { line2d(p, r, {15, 50}, {85, 50}, kOrange, 1.4, Qt::DashLine); }};
    c["sk:Trim"] = {"Ajuster", "Trim", QString::fromUtf8("Clique un bout de trait qui dépasse : il disparaît jusqu'à l'intersection."),
                    "Click a line end that sticks out: it vanishes up to the intersection.",
                    [](QPainter& p, const QRectF& r) {
                        line2d(p, r, {15, 50}, {85, 50});
                        line2d(p, r, {50, 15}, {50, 85});
                    },
                    [](QPainter& p, const QRectF& r) {
                        line2d(p, r, {15, 50}, {50, 50});
                        line2d(p, r, {50, 15}, {50, 85});
                    }};
    c["sk:Extend"] = {"Prolonger", "Extend", QString::fromUtf8("Allonge un trait jusqu'au prochain : pour refermer un contour."),
                      "Extends a line up to the next one: to close a contour.",
                      [](QPainter& p, const QRectF& r) {
                          line2d(p, r, {15, 50}, {38, 50});
                          line2d(p, r, {50, 15}, {50, 85});
                      },
                      [](QPainter& p, const QRectF& r) {
                          line2d(p, r, {15, 50}, {50, 50});
                          line2d(p, r, {50, 15}, {50, 85});
                      }};
    c["sk:Fillet"] = {QString::fromUtf8("Congé 2D"), "Sketch Fillet", QString::fromUtf8("Arrondit un coin de l'esquisse : moins de contraintes dans la pièce."),
                      "Rounds a sketch corner: less stress in the part.",
                      [](QPainter& p, const QRectF& r) {
                          p.setPen(QPen(kBlue, 1.8));
                          p.drawPolyline(QPolygonF{at2d(r, 20, 20), at2d(r, 20, 80), at2d(r, 85, 80)});
                      },
                      [](QPainter& p, const QRectF& r) {
                          QPainterPath path(at2d(r, 20, 20));
                          path.lineTo(at2d(r, 20, 55));
                          path.quadTo(at2d(r, 20, 80), at2d(r, 45, 80));
                          path.lineTo(at2d(r, 85, 80));
                          p.setPen(QPen(kBlue, 1.8));
                          p.setBrush(Qt::NoBrush);
                          p.drawPath(path);
                      }};
    c["sk:Offset"] = {QString::fromUtf8("Décalage"), "Offset", QString::fromUtf8("Une copie parallèle du contour : parois, rainures, joints."),
                      "A parallel copy of the contour: walls, grooves, gaskets.",
                      [](QPainter& p, const QRectF& r) {
                          p.setPen(QPen(kBlue, 1.8));
                          p.setBrush(Qt::NoBrush);
                          p.drawRect(QRectF(at2d(r, 30, 30), at2d(r, 70, 70)));
                      },
                      [](QPainter& p, const QRectF& r) {
                          p.setPen(QPen(kBlue, 1.8));
                          p.setBrush(Qt::NoBrush);
                          p.drawRect(QRectF(at2d(r, 30, 30), at2d(r, 70, 70)));
                          p.drawRect(QRectF(at2d(r, 18, 18), at2d(r, 82, 82)));
                      }};
    c["sk:Mirror"] = {QString::fromUtf8("Symétrie 2D"), "Sketch Mirror", QString::fromUtf8("Dessine une moitié du profil, la symétrie fait l'autre."),
                      "Draw half of the profile, the mirror makes the other half.",
                      [](QPainter& p, const QRectF& r) {
                          line2d(p, r, {50, 10}, {50, 90}, kOrange, 1.2, Qt::DashLine);
                          p.setPen(QPen(kBlue, 1.8));
                          p.drawPolyline(QPolygonF{at2d(r, 50, 20), at2d(r, 25, 35), at2d(r, 25, 80), at2d(r, 50, 80)});
                      },
                      [](QPainter& p, const QRectF& r) {
                          line2d(p, r, {50, 10}, {50, 90}, kOrange, 1.2, Qt::DashLine);
                          p.setPen(QPen(kBlue, 1.8));
                          p.drawPolyline(QPolygonF{at2d(r, 50, 20), at2d(r, 25, 35), at2d(r, 25, 80), at2d(r, 75, 80), at2d(r, 75, 35), at2d(r, 50, 20)});
                      }};
    c["sk:Pattern"] = {QString::fromUtf8("Répétition 2D"), "Sketch Pattern", QString::fromUtf8("Répète des éléments de l'esquisse en ligne ou en cercle."),
                       "Repeats sketch entities in a row or around a circle.",
                       [](QPainter& p, const QRectF& r) {
                           p.setPen(QPen(kBlue, 1.8));
                           p.drawEllipse(at2d(r, 20, 50), 6, 6);
                       },
                       [](QPainter& p, const QRectF& r) {
                           p.setPen(QPen(kBlue, 1.8));
                           for (int k = 0; k < 4; ++k)
                               p.drawEllipse(at2d(r, 20 + 20 * k, 50), 6, 6);
                       }};
    return c;
}

const QMap<QString, Card>& allCards()
{
    static const QMap<QString, Card> c = cards();
    return c;
}

class Thumb final : public QWidget
{
public:
    Thumb(Draw before, Draw after, QWidget* parent) : QWidget(parent), m_before(std::move(before)), m_after(std::move(after))
    {
        setFixedSize(230, 92);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF a(4, 2, 92, 88), b(134, 2, 92, 88);
        p.fillRect(rect(), QColor(0x1b, 0x1e, 0x23));
        p.setPen(QPen(QColor(0x3a, 0x40, 0x48), 1));
        p.drawRoundedRect(a.adjusted(-2, 0, 2, 0), 4, 4);
        p.drawRoundedRect(b.adjusted(-2, 0, 2, 0), 4, 4);
        p.save();
        m_before(p, a);
        p.restore();
        p.save();
        m_after(p, b);
        p.restore();
        // arrow
        p.setPen(QPen(kOrange, 2.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(104, 46), QPointF(126, 46));
        p.drawLine(QPointF(120, 40), QPointF(126, 46));
        p.drawLine(QPointF(120, 52), QPointF(126, 46));
    }

private:
    Draw m_before, m_after;
};

}  // namespace

RichTips::RichTips(std::function<bool()> french, QObject* parent) : QObject(parent), m_french(std::move(french))
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setInterval(1200);
    connect(m_timer, &QTimer::timeout, this, [this] {
        if (m_pendingAnchor && m_pendingAnchor->isVisible())
            showFor(m_pendingAnchor, m_pendingTool, m_pendingKey, m_pendingAt);
    });
}

bool RichTips::hasCard(const QString& tool) { return allCards().contains(tool); }

void RichTips::watch(QToolBar* bar)
{
    const QString prefix = bar->objectName() == "sketchBar" ? "sk:" : "";
    for (QToolButton* b : bar->findChildren<QToolButton*>()) {
        b->setProperty("richPrefix", prefix);
        b->installEventFilter(this);
        if (b->menu())
            watchMenu(b->menu(), prefix);
    }
}

void RichTips::watchMenu(QMenu* menu, const QString& prefix)
{
    menu->installEventFilter(this);
    connect(menu, &QMenu::hovered, this, [this, menu, prefix](QAction* a) {
        hide();
        const QString tool =
            prefix + (a->data().toString().isEmpty() ? a->text().section('\t', 0, 0) : a->data().toString());
        if (!hasCard(tool)) {
            m_timer->stop();
            return;
        }
        m_pendingAnchor = menu;
        m_pendingTool = tool;
        m_pendingKey = a->text().section('\t', 1);
        const QRect ar = menu->actionGeometry(a);
        m_pendingAt = menu->mapToGlobal(QPoint(menu->width() + 6, ar.top()));
        m_timer->start();
    });
    connect(menu, &QMenu::aboutToHide, this, [this] {
        m_timer->stop();
        hide();
    });
}

bool RichTips::eventFilter(QObject* o, QEvent* e)
{
    auto* b = qobject_cast<QToolButton*>(o);
    if (b) {
        const QAction* a = b->defaultAction();
        QString tool = a ? a->text() : QString();
        if (tool.isEmpty() || tool.contains(QChar(0x25BE)))
            tool = b->property("richTool").toString();
        const QString key = b->property("richPrefix").toString() + tool;
        switch (e->type()) {
            case QEvent::Enter:
                if (hasCard(key)) {
                    m_pendingAnchor = b;
                    m_pendingTool = key;
                    const QString tip = a ? a->toolTip() : b->toolTip();
                    const int open = tip.lastIndexOf('('), close = tip.lastIndexOf(')');
                    m_pendingKey = open >= 0 && close > open ? tip.mid(open + 1, close - open - 1) : QString();
                    m_pendingAt = b->mapToGlobal(QPoint(0, b->height() + 4));
                    m_timer->start();
                }
                break;
            case QEvent::Leave:
            case QEvent::MouseButtonPress:
                m_timer->stop();
                hide();
                break;
            case QEvent::ToolTip:
                if (hasCard(key))
                    return true;  // the card replaces the plain tooltip
                break;
            default: break;
        }
    }
    return QObject::eventFilter(o, e);
}

QWidget* RichTips::makeCard(const QString& tool, const QString& shortcut) const
{
    if (!hasCard(tool))
        return nullptr;
    const Card& c = allCards()[tool];
    const bool fr = m_french();
    auto* w = new QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint);
    w->setAttribute(Qt::WA_ShowWithoutActivating);
    w->setStyleSheet("QWidget { background: #23272e; color: #dde1e6; } QLabel { background: transparent; }");
    auto* lay = new QVBoxLayout(w);
    lay->setContentsMargins(10, 8, 10, 10);
    lay->setSpacing(6);
    QString title = QString("<b>%1</b> <span style='color:#8a939e'>/ %2</span>").arg(fr ? c.fr : c.en, fr ? c.en : c.fr);
    if (!shortcut.isEmpty())
        title += QString("&nbsp;&nbsp;<span style='background:#3a404a; padding:1px 4px; color:#ffd54f'>&nbsp;%1&nbsp;</span>").arg(shortcut.toHtmlEscaped());
    auto* head = new QLabel(title, w);
    lay->addWidget(head);
    lay->addWidget(new Thumb(c.before, c.after, w));
    auto* say = new QLabel(fr ? c.sayFr : c.sayEn, w);
    say->setWordWrap(true);
    say->setFixedWidth(230);
    say->setStyleSheet("color: #c7ced6;");
    lay->addWidget(say);
    w->adjustSize();
    return w;
}

void RichTips::showFor(QWidget* anchor, const QString& tool, const QString& shortcut, QPoint at)
{
    hide();
    QWidget* card = makeCard(tool, shortcut);
    if (!card)
        return;
    m_card = card;
    card->setAttribute(Qt::WA_DeleteOnClose);
    // keep the card on the screen of the anchor
    if (QScreen* s = anchor->screen()) {
        const QRect g = s->availableGeometry();
        at.setX(std::min(at.x(), g.right() - card->width() - 4));
        at.setY(std::min(at.y(), g.bottom() - card->height() - 4));
    }
    card->move(at);
    card->show();
}

void RichTips::hide()
{
    if (m_card)
        m_card->close();
    m_card = nullptr;
}

}  // namespace silex::ui
