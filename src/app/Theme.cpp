#include "Theme.h"

#include <QPainter>

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace silex::ui {

void applyDarkTheme(QApplication& app)
{
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window, Colors::Panel);
    p.setColor(QPalette::WindowText, QColor(0xe6, 0xe8, 0xeb));
    p.setColor(QPalette::Base, QColor(0x22, 0x25, 0x29));
    p.setColor(QPalette::AlternateBase, Colors::Panel);
    p.setColor(QPalette::ToolTipBase, QColor(0x1d, 0x1f, 0x23));
    p.setColor(QPalette::ToolTipText, QColor(0xe6, 0xe8, 0xeb));
    p.setColor(QPalette::Text, QColor(0xe6, 0xe8, 0xeb));
    p.setColor(QPalette::Button, QColor(0x35, 0x39, 0x40));
    p.setColor(QPalette::ButtonText, QColor(0xe6, 0xe8, 0xeb));
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, Colors::Accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, QColor(0x80, 0x86, 0x90));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x70, 0x75, 0x7d));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x70, 0x75, 0x7d));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x70, 0x75, 0x7d));
    app.setPalette(p);
    app.setStyleSheet(R"(
        QToolBar { background: #2d3036; border: none; spacing: 2px; padding: 3px; }
        QToolBar::separator { background: #454a52; width: 1px; margin: 4px 6px; }
        QToolButton { border: 1px solid transparent; border-radius: 4px; padding: 3px; color: #e6e8eb; }
        QToolButton:hover { background: #3c4149; border-color: #4a5059; }
        QToolButton:checked { background: #1f5f99; border-color: #2f9bff; }
        QToolButton:disabled { color: #6a7078; }
        QDockWidget { color: #c9ced6; }
        QDockWidget::title { background: #2a2d33; padding: 5px; }
        QStatusBar { background: #24272c; color: #aab1bb; }
        QGroupBox { border: 1px solid #40454d; border-radius: 4px; margin-top: 12px; padding-top: 6px; }
        QGroupBox::title { subcontrol-origin: margin; left: 8px; color: #9aa3ad; }
        QPushButton { background: #3a3f47; border: 1px solid #4a5059; border-radius: 4px; padding: 5px 12px; }
        QPushButton:hover { background: #444a53; }
        QPushButton:default { background: #1f6fb8; border-color: #2f9bff; }
        QLineEdit, QDoubleSpinBox, QComboBox { background: #1f2226; border: 1px solid #454a52; border-radius: 3px; padding: 3px; }
        QMenu { background: #2b2e34; border: 1px solid #454a52; }
        QMenu::item:selected { background: #1f5f99; }
    )");
}

namespace {
bool g_essential = false;
}
bool essentialUi() { return g_essential; }
void setEssentialUi(bool on) { g_essential = on; }

void paintSelectionBox(QPainter& p, const QPointF& from, const QPointF& to)
{
    const bool crossing = to.x() < from.x();
    const QColor c = crossing ? QColor(0x4c, 0xd9, 0x7b) : QColor(0x2f, 0x9b, 0xff);
    QColor fill = c;
    fill.setAlpha(45);
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(c, 1.2, crossing ? Qt::DashLine : Qt::SolidLine));
    p.setBrush(fill);
    p.drawRect(QRectF(from, to).normalized());
    p.restore();
}

}  // namespace silex::ui
