#pragma once
// Quaternion orbit camera (Z-up world, turntable orbit, zoom to cursor, animated alignment).

#include <QMatrix4x4>
#include <QPointF>
#include <QQuaternion>
#include <QSize>
#include <QVector3D>

#include "model/Math.h"

namespace silex::ui {

struct Ray
{
    QVector3D origin;
    QVector3D dir;  ///< normalized
};

class Camera
{
public:
    Camera();

    QVector3D target{0, 0, 0};
    float distance = 160.f;
    QQuaternion rotation;  ///< camera -> world
    float fovY = 35.f;
    bool orthographic = false;
    float sceneRadius = 100.f;

    QVector3D eye() const { return target + rotation.rotatedVector({0, 0, distance}); }
    QVector3D up() const { return rotation.rotatedVector({0, 1, 0}); }
    QVector3D right() const { return rotation.rotatedVector({1, 0, 0}); }
    QVector3D forward() const { return rotation.rotatedVector({0, 0, -1}); }

    QMatrix4x4 view() const;
    QMatrix4x4 projection(float aspect) const;
    QMatrix4x4 viewProjection(QSize size) const { return projection(aspectOf(size)) * view(); }

    void orbit(float dxPixels, float dyPixels);
    void pan(float dxPixels, float dyPixels, QSize size);
    void zoom(float steps, QPointF cursor, QSize size);
    void fit(const BBox& box);

    /// Orientation looking along -normal with xDir to the right (sketch alignment).
    static QQuaternion orientationFor(const Placement& pl);
    static QQuaternion isometric();
    static QQuaternion standardView(int which);  ///< 1 front, 3 right, 7 top, 0 iso

    Ray ray(QPointF pixel, QSize size) const;
    /// Window coordinates (pixels, y down); z = NDC depth. Returns false if behind the eye.
    bool project(const QVector3D& p, QSize size, QPointF* out, float* depth = nullptr) const;
    /// World size of one pixel at the target distance.
    float pixelSize(QSize size) const;

    static float aspectOf(QSize s) { return s.height() > 0 ? float(s.width()) / float(s.height()) : 1.f; }
};

}  // namespace silex::ui
