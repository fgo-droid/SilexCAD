#pragma once
// Radial marking menu: press the right button, flick towards a command, release.
// A plain click opens it in "click mode" instead.

#include <QWidget>
#include <vector>

#include "Workspace.h"

namespace silex::ui {

class MarkingMenu : public QWidget
{
    Q_OBJECT
public:
    static void popup(QWidget* parent, QPoint center, std::vector<MenuAction> items);

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    MarkingMenu(QWidget* parent, QPoint center, std::vector<MenuAction> items);
    int itemAt(QPointF p) const;
    QPointF slot(int i) const;
    QRectF itemRect(int i) const;
    void trigger(int i);

    QPointF m_center;
    std::vector<MenuAction> m_items;
    int m_hot = -1;
    bool m_moved = false;
};

}  // namespace silex::ui
