#include "ViewCube.h"

#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>
#include <cmath>

#include "Camera.h"
#include "Theme.h"
#include "Viewport.h"

namespace silex::ui {

namespace {

constexpr int kSize = 150;       // widget size (px)
constexpr float kHalf = 33.f;    // half edge of the cube on screen (px)
constexpr float kBand = 0.55f;   // inner limit of the edge / corner bands (cube half-size = 1)
constexpr int kAnimMs = 200;

struct FaceDef
{
    QVector3D n, right, up;  ///< outward normal; label axes as seen from outside
    const char* en;
    const char* fr;
};

// Z-up world, front view looks along +Y (see Camera::standardView).
const FaceDef kFaces[6] = {
    {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, "TOP", "DESSUS"},
    {{0, 0, -1}, {1, 0, 0}, {0, -1, 0}, "BOTTOM", "DESSOUS"},
    {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}, "FRONT", "FACE"},
    {{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}, "BACK", "ARRIÈRE"},
    {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, "RIGHT", "DROITE"},
    {{-1, 0, 0}, {0, -1, 0}, {0, 0, 1}, "LEFT", "GAUCHE"},
};

bool french() { return QLocale().language() == QLocale::French; }

}  // namespace

ViewCube::ViewCube(Viewport* vp) : QWidget(vp), m_vp(vp)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setAutoFillBackground(false);
    setFixedSize(kSize, kSize);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QObject::tr("Click a face, an edge or a corner to orient the view"));
    m_rot = vp->camera().rotation;
    reposition();
}

void ViewCube::reposition()
{
    if (QWidget* p = parentWidget())
        move(p->width() - width() - 6, 6);
    raise();
}

void ViewCube::syncCamera(const QQuaternion& rotation)
{
    if (qFuzzyCompare(rotation, m_rot))
        return;
    m_rot = rotation;
    update();
}

QQuaternion ViewCube::orientationLookingFrom(const QVector3D& dirToEye)
{
    const QVector3D back = dirToEye.normalized();
    QVector3D right = QVector3D::crossProduct({0, 0, 1}, back);
    if (right.length() < 1e-4f)  // top / bottom: keep +X to the right (like the standard top view)
        right = {1, 0, 0};
    right.normalize();
    const QVector3D up = QVector3D::crossProduct(back, right).normalized();
    return QQuaternion::fromAxes(right, up, back);
}

QPointF ViewCube::project(const QVector3D& p, float* depth) const
{
    const QVector3D v = m_rot.conjugated().rotatedVector(p);  // world -> camera
    if (depth)
        *depth = v.z();
    return {kSize * 0.5 + v.x() * kHalf, kSize * 0.5 + 6 - v.y() * kHalf};
}

std::vector<ViewCube::Cell> ViewCube::visibleCells() const
{
    std::vector<Cell> cells;
    const float b[4] = {-1.f, -kBand, kBand, 1.f};
    for (int f = 0; f < 6; ++f) {
        const FaceDef& fd = kFaces[f];
        if (m_rot.conjugated().rotatedVector(fd.n).z() <= 1e-3f)
            continue;  // back face
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                Cell c;
                c.face = f;
                for (auto [x, y] : {std::pair{b[i], b[j]}, std::pair{b[i + 1], b[j]}, std::pair{b[i + 1], b[j + 1]},
                                    std::pair{b[i], b[j + 1]}})
                    c.poly << project(fd.n + fd.right * x + fd.up * y);
                c.dir = fd.n + fd.right * float(i - 1) + fd.up * float(j - 1);
                cells.push_back(c);
            }
    }
    return cells;
}

bool ViewCube::alignedOnFace(QVector3D* axis) const
{
    const QVector3D fwd = m_rot.rotatedVector({0, 0, -1});
    for (const FaceDef& fd : kFaces)
        if (QVector3D::dotProduct(fwd, fd.n) > 0.999f) {
            if (axis)
                *axis = fd.n;
            return true;
        }
    return false;
}

QRectF ViewCube::buttonRect(Button b) const
{
    switch (b) {
        case Button::Home: return {4, 4, 22, 22};
        case Button::RollLeft: return {kSize - 52, 4, 22, 22};
        case Button::RollRight: return {kSize - 26, 4, 22, 22};
        default: return {};
    }
}

ViewCube::Button ViewCube::buttonAt(QPointF pos) const
{
    if (buttonRect(Button::Home).contains(pos))
        return Button::Home;
    if (alignedOnFace(nullptr)) {
        if (buttonRect(Button::RollLeft).contains(pos))
            return Button::RollLeft;
        if (buttonRect(Button::RollRight).contains(pos))
            return Button::RollRight;
    }
    return Button::None;
}

bool ViewCube::regionAt(QPointF pos, QVector3D* dir) const
{
    for (const Cell& c : visibleCells())
        if (c.poly.containsPoint(pos, Qt::OddEvenFill)) {
            *dir = c.dir;
            return true;
        }
    return false;
}

// ------------------------------------------------------------------ painting

void ViewCube::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool fr = french();
    const QColor faceCol(58, 66, 78, 235), lineCol(120, 130, 145), hoverCol(0x2f, 0x9b, 0xff, 220);

    const auto cells = visibleCells();
    for (const Cell& c : cells) {
        const bool hov = m_hoverRegion && c.dir == m_hoverDir;
        p.setPen(Qt::NoPen);
        p.setBrush(hov ? hoverCol : faceCol);
        p.drawPolygon(c.poly);
    }
    // face outlines + labels
    for (int f = 0; f < 6; ++f) {
        const FaceDef& fd = kFaces[f];
        if (m_rot.conjugated().rotatedVector(fd.n).z() <= 1e-3f)
            continue;
        QPolygonF quad;  // top-left, top-right, bottom-right, bottom-left (as read on the face)
        quad << project(fd.n - fd.right + fd.up) << project(fd.n + fd.right + fd.up) << project(fd.n + fd.right - fd.up)
             << project(fd.n - fd.right - fd.up);
        p.setPen(QPen(lineCol, 1.1));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(quad);
        QTransform t;
        if (QTransform::quadToQuad(QPolygonF(QRectF(0, 0, 100, 100)), quad, t)) {
            p.save();
            p.setTransform(t, true);
            QFont font = p.font();
            font.setPixelSize(fr ? 17 : 19);
            font.setBold(true);
            p.setFont(font);
            const bool hovFace = m_hoverRegion && m_hoverDir == fd.n;
            p.setPen(hovFace ? QColor(255, 255, 255) : QColor(215, 222, 230));
            p.drawText(QRectF(0, 0, 100, 100), Qt::AlignCenter, QString::fromUtf8(fr ? fd.fr : fd.en));
            p.restore();
        }
    }
    // small world triad at the bottom-left corner of the cube (X red, Y green, Z blue)
    const QPointF o = project({-1, -1, -1});
    float depth = 0;
    const QVector3D corner(-1, -1, -1);
    project(corner, &depth);
    const struct
    {
        QVector3D d;
        QColor c;
    } axes[3] = {{{1, 0, 0}, QColor(0xe0, 0x60, 0x60)}, {{0, 1, 0}, QColor(0x70, 0xc8, 0x70)}, {{0, 0, 1}, QColor(0x4a, 0xa3, 0xff)}};
    for (const auto& a : axes) {
        p.setPen(QPen(a.c, 2.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(o, project(corner + a.d * 0.9f));
    }

    // buttons
    auto buttonBg = [&](Button b) {
        const QRectF r = buttonRect(b);
        p.setPen(Qt::NoPen);
        p.setBrush(m_hoverButton == b ? hoverCol : QColor(40, 46, 55, 210));
        p.drawRoundedRect(r, 4, 4);
        p.setPen(QPen(QColor(225, 230, 236), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        return r;
    };
    {  // home
        const QRectF r = buttonBg(Button::Home);
        const QPointF c = r.center();
        QPolygonF house;
        house << c + QPointF(-6, 0) << c + QPointF(0, -6) << c + QPointF(6, 0);
        p.drawPolyline(house);
        p.drawRect(QRectF(c + QPointF(-4, 0), QSizeF(8, 6)));
    }
    if (alignedOnFace(nullptr)) {
        for (Button b : {Button::RollLeft, Button::RollRight}) {
            const QRectF r = buttonBg(b);
            const QRectF arc = r.adjusted(5, 5, -5, -5);
            const bool cw = b == Button::RollRight;
            p.drawArc(arc, cw ? 200 * 16 : -20 * 16, cw ? -230 * 16 : 230 * 16);
            // arrow head at the end of the arc
            const double endDeg = cw ? 200 - 230 : -20 + 230;
            const double a = endDeg * M_PI / 180.0;
            const QPointF tip(arc.center().x() + arc.width() / 2 * std::cos(a), arc.center().y() - arc.height() / 2 * std::sin(a));
            const QPointF tan = cw ? QPointF(std::sin(a), std::cos(a)) : QPointF(-std::sin(a), -std::cos(a));
            const QPointF nrm(-tan.y(), tan.x());
            p.setBrush(QColor(225, 230, 236));
            p.drawPolygon(QPolygonF{tip + tan * 4, tip - tan * 1.5 + nrm * 3, tip - tan * 1.5 - nrm * 3});
            p.setBrush(Qt::NoBrush);
        }
    }
}

// ------------------------------------------------------------------ interaction

void ViewCube::mouseMoveEvent(QMouseEvent* e)
{
    const Button b = buttonAt(e->position());
    QVector3D dir;
    const bool region = b == Button::None && regionAt(e->position(), &dir);
    if (b != m_hoverButton || region != m_hoverRegion || (region && dir != m_hoverDir)) {
        m_hoverButton = b;
        m_hoverRegion = region;
        m_hoverDir = dir;
        update();
    }
    e->accept();
}

void ViewCube::leaveEvent(QEvent*)
{
    m_hoverButton = Button::None;
    m_hoverRegion = false;
    update();
}

void ViewCube::mousePressEvent(QMouseEvent* e)
{
    e->accept();
    if (e->button() != Qt::LeftButton)
        return;
    switch (buttonAt(e->position())) {
        case Button::Home: m_vp->homeView(); return;
        case Button::RollLeft:
        case Button::RollRight: {
            QVector3D axis;
            if (!alignedOnFace(&axis))
                return;
            const float angle = buttonAt(e->position()) == Button::RollRight ? -90.f : 90.f;
            // the camera looks along -axis: roll around that viewing direction
            const QQuaternion roll = QQuaternion::fromAxisAndAngle(-axis, angle);
            m_vp->animateTo((roll * m_vp->camera().rotation).normalized(), {}, {}, kAnimMs);
            return;
        }
        default: break;
    }
    QVector3D dir;
    if (regionAt(e->position(), &dir))
        m_vp->animateTo(orientationLookingFrom(dir), {}, {}, kAnimMs);
}

}  // namespace silex::ui
