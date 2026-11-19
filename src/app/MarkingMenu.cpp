#include "MarkingMenu.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QtMath>

#include "Theme.h"

namespace silex::ui {

namespace {
constexpr double kRadius = 105;
constexpr double kRadiusLarge = 165;  // more than 8 entries
constexpr double kDeadZone = 22;
}

void MarkingMenu::popup(QWidget* parent, QPoint center, std::vector<MenuAction> items)
{
    if (items.empty())
        return;
    auto* m = new MarkingMenu(parent, center, std::move(items));
    m->show();
    m->raise();
    m->setFocus();
    m->grabMouse();
}

MarkingMenu::MarkingMenu(QWidget* parent, QPoint center, std::vector<MenuAction> items)
    : QWidget(parent), m_center(center), m_items(std::move(items))
{
    if (m_items.size() > 12)
        m_items.resize(12);
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setGeometry(parent->rect());
}

QPointF MarkingMenu::slot(int i) const
{
    const int n = int(m_items.size());
    double deg;
    if (n <= 8) {
        // N, E, S, W first, then the diagonals (classic marking-menu layout)
        static const double angles[8] = {-90, 0, 90, 180, -45, 45, 135, -135};
        deg = angles[i];
    }
    else {
        deg = -90.0 + i * 360.0 / n;  // clockwise from the top, evenly spaced
    }
    const double r = n <= 8 ? kRadius : kRadiusLarge;
    const double a = qDegreesToRadians(deg);
    return m_center + QPointF(std::cos(a) * r, std::sin(a) * r);
}

QRectF MarkingMenu::itemRect(int i) const
{
    const QPointF c = slot(i);
    const QFontMetricsF fm(font());
    const double w = std::max(96.0, fm.horizontalAdvance(m_items[size_t(i)].label) + 44);
    return {c.x() - w / 2, c.y() - 15, w, 30};
}

int MarkingMenu::itemAt(QPointF p) const
{
    for (int i = 0; i < int(m_items.size()); ++i)
        if (itemRect(i).contains(p))
            return i;
    const QPointF d = p - m_center;
    const double dist = std::hypot(d.x(), d.y());
    if (dist < kDeadZone)
        return -1;
    // angular sector
    int best = -1;
    double bestAng = 1e9;
    const double a = std::atan2(d.y(), d.x());
    for (int i = 0; i < int(m_items.size()); ++i) {
        const QPointF s = slot(i) - m_center;
        double diff = std::abs(std::remainder(a - std::atan2(s.y(), s.x()), 2 * M_PI));
        if (diff < bestAng) {
            bestAng = diff;
            best = i;
        }
    }
    return bestAng < qDegreesToRadians(std::min(25.0, 170.0 / m_items.size())) ? best : -1;
}

void MarkingMenu::trigger(int i)
{
    releaseMouse();
    close();
    if (i >= 0 && i < int(m_items.size()) && m_items[size_t(i)].enabled && m_items[size_t(i)].run) {
        auto run = m_items[size_t(i)].run;
        run();
    }
}

void MarkingMenu::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // center
    p.setPen(QPen(QColor(255, 255, 255, 90), 1.5));
    p.setBrush(QColor(20, 24, 30, 180));
    p.drawEllipse(m_center, kDeadZone, kDeadZone);
    if (m_hot >= 0) {
        p.setPen(QPen(Colors::Accent, 2));
        p.drawLine(m_center, slot(m_hot));
    }
    QFont f = font();
    f.setPixelSize(12);
    p.setFont(f);
    for (int i = 0; i < int(m_items.size()); ++i) {
        const MenuAction& a = m_items[size_t(i)];
        const QRectF r = itemRect(i);
        const bool hot = i == m_hot && a.enabled;
        p.setPen(QPen(hot ? Colors::Accent : QColor(0x55, 0x5b, 0x65), 1.2));
        p.setBrush(hot ? QColor(0x1f, 0x5f, 0x99) : QColor(0x2b, 0x2f, 0x35, 240));
        p.drawRoundedRect(r, 15, 15);
        a.icon.paint(&p, QRect(int(r.left() + 8), int(r.center().y() - 9), 18, 18), Qt::AlignCenter,
                     a.enabled ? QIcon::Normal : QIcon::Disabled);
        p.setPen(a.enabled ? QColor(0xe6, 0xe8, 0xeb) : QColor(0x70, 0x75, 0x7d));
        p.drawText(r.adjusted(32, 0, -10, 0), Qt::AlignVCenter | Qt::AlignLeft, a.label);
    }
}

void MarkingMenu::mouseMoveEvent(QMouseEvent* e)
{
    const QPointF d = e->position() - m_center;
    if (std::hypot(d.x(), d.y()) > kDeadZone)
        m_moved = true;
    const int h = itemAt(e->position());
    if (h != m_hot) {
        m_hot = h;
        update();
    }
}

void MarkingMenu::mouseReleaseEvent(QMouseEvent* e)
{
    // Gesture mode: release over a sector triggers it. Click mode: stay open.
    if (e->button() == Qt::RightButton && !m_moved)
        return;
    const int i = itemAt(e->position());
    if (i >= 0)
        trigger(i);
    else if (m_moved || e->button() == Qt::LeftButton) {
        releaseMouse();
        close();
    }
}

void MarkingMenu::mousePressEvent(QMouseEvent* e)
{
    const int i = itemAt(e->position());
    if (i < 0) {
        releaseMouse();
        close();
        return;
    }
    m_hot = i;
}

void MarkingMenu::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape) {
        releaseMouse();
        close();
    }
}

}  // namespace silex::ui
