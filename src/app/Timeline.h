#pragma once
// Horizontal parametric timeline (Fusion-style): one icon per feature, a draggable rollback
// marker, drag-to-reorder (DAG-checked), status colours, context menu.

#include <QWidget>
#include <vector>

#include "model/Document.h"

namespace silex::ui {

class ModelController;

class Timeline : public QWidget
{
    Q_OBJECT
public:
    explicit Timeline(ModelController* model, QWidget* parent = nullptr);
    void setEditing(const Uuid& id);
    QSize sizeHint() const override { return {800, 62}; }
    QSize minimumSizeHint() const override { return {200, 62}; }

signals:
    void editRequested(const Uuid& id);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    bool event(QEvent* e) override;

private:
    QRectF itemRect(int i) const;
    QRectF buttonRect(int i) const;
    double markerX(int marker) const;
    int itemAt(QPointF p) const;
    int gapAt(double x) const;
    void moveMarker(int m);

    ModelController* m_model;
    Uuid m_editing;
    int m_hover = -1;
    int m_pressItem = -1;
    QPointF m_pressPos;
    bool m_draggingMarker = false;
    bool m_draggingItem = false;
    int m_dropGap = -1;
};

}  // namespace silex::ui
