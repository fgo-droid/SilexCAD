#pragma once
// ViewCube: 3D orientation cube in the top-right corner of the viewport, synchronised with the
// camera. Clickable faces (standard views), edges (45° views) and corners (isometric views) with
// hover highlight, a Home button (default isometric view, fitted) and two 90° roll arrows when
// the view is aligned on a face. Every click animates the camera (SLERP, ~200 ms).

#include <QQuaternion>
#include <QVector3D>
#include <QWidget>
#include <vector>

namespace silex::ui {

class Viewport;

class ViewCube final : public QWidget
{
public:
    explicit ViewCube(Viewport* vp);

    /// Keeps the cube in the top-right corner of the viewport.
    void reposition();
    /// Called by the viewport each frame: repaints only when the camera turned.
    void syncCamera(const QQuaternion& rotation);

    /// View direction (target -> eye) of the region under `pos`, if any (tests / tooling).
    static QQuaternion orientationLookingFrom(const QVector3D& dirToEye);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    struct Cell
    {
        QPolygonF poly;   ///< screen polygon
        QVector3D dir;    ///< region direction (face / edge / corner), unnormalised integer vector
        int face = 0;
    };
    enum class Button { None, Home, RollLeft, RollRight };

    std::vector<Cell> visibleCells() const;
    QPointF project(const QVector3D& p, float* depth = nullptr) const;
    bool alignedOnFace(QVector3D* axis) const;
    QRectF buttonRect(Button b) const;
    Button buttonAt(QPointF pos) const;
    bool regionAt(QPointF pos, QVector3D* dir) const;

    Viewport* m_vp;
    QQuaternion m_rot;
    QVector3D m_hoverDir;
    bool m_hoverRegion = false;
    Button m_hoverButton = Button::None;
};

}  // namespace silex::ui
