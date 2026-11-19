#include "Camera.h"

#include <QtMath>
#include <algorithm>

namespace silex::ui {

Camera::Camera() { rotation = isometric(); }

QQuaternion Camera::isometric()
{
    // Looking from (+1,-1,+1) towards the origin, Z up.
    const QVector3D back = QVector3D(1, -1, 0.9f).normalized();
    const QVector3D right = QVector3D::crossProduct(QVector3D(0, 0, 1), back).normalized();
    const QVector3D up = QVector3D::crossProduct(back, right).normalized();
    return QQuaternion::fromAxes(right, up, back);
}

QQuaternion Camera::standardView(int which)
{
    switch (which) {
        case 1: return QQuaternion::fromAxes({1, 0, 0}, {0, 0, 1}, {0, -1, 0});  // front (-Y)
        case 3: return QQuaternion::fromAxes({0, 1, 0}, {0, 0, 1}, {1, 0, 0});   // right (+X)
        case 7: return QQuaternion::fromAxes({1, 0, 0}, {0, 1, 0}, {0, 0, 1});   // top (+Z)
        default: return isometric();
    }
}

QQuaternion Camera::orientationFor(const Placement& pl)
{
    const QVector3D x(float(pl.xDir.x), float(pl.xDir.y), float(pl.xDir.z));
    const QVector3D n(float(pl.normal.x), float(pl.normal.y), float(pl.normal.z));
    const QVector3D y = QVector3D::crossProduct(n, x);
    return QQuaternion::fromAxes(x.normalized(), y.normalized(), n.normalized());
}

QMatrix4x4 Camera::view() const
{
    QMatrix4x4 v;
    v.lookAt(eye(), target, up());
    return v;
}

QMatrix4x4 Camera::projection(float aspect) const
{
    QMatrix4x4 p;
    const float nearP = std::max(distance * 0.002f, 0.01f);
    const float farP = distance * 4.f + sceneRadius * 6.f + 1000.f;
    if (orthographic) {
        const float h = distance * std::tan(qDegreesToRadians(fovY) * 0.5f);
        p.ortho(-h * aspect, h * aspect, -h, h, -farP, farP);
    }
    else {
        p.perspective(fovY, aspect, nearP, farP);
    }
    return p;
}

void Camera::orbit(float dx, float dy)
{
    const QQuaternion yaw = QQuaternion::fromAxisAndAngle(0, 0, 1, -dx * 0.35f);
    const QQuaternion pitch = QQuaternion::fromAxisAndAngle(1, 0, 0, -dy * 0.35f);
    rotation = (yaw * rotation * pitch).normalized();
}

float Camera::pixelSize(QSize size) const
{
    const float h = 2.f * distance * std::tan(qDegreesToRadians(fovY) * 0.5f);
    return h / std::max(1, size.height());
}

void Camera::pan(float dx, float dy, QSize size)
{
    const float s = pixelSize(size);
    target += right() * (-dx * s) + up() * (dy * s);
}

void Camera::zoom(float steps, QPointF cursor, QSize size)
{
    const float factor = std::pow(0.85f, steps);
    // Zoom towards the point under the cursor (on the plane through the target).
    const Ray r = ray(cursor, size);
    const QVector3D n = forward();
    const float denom = QVector3D::dotProduct(r.dir, n);
    QVector3D anchor = target;
    if (std::abs(denom) > 1e-6f) {
        const float t = QVector3D::dotProduct(target - r.origin, n) / denom;
        if (t > 0)
            anchor = r.origin + r.dir * t;
    }
    const float newDistance = std::clamp(distance * factor, 0.01f, 1e6f);
    const float k = 1.f - newDistance / distance;
    target += (anchor - target) * k;
    distance = newDistance;
}

void Camera::fit(const BBox& box)
{
    if (!box.valid()) {
        target = {0, 0, 0};
        distance = 160.f;
        sceneRadius = 100.f;
        return;
    }
    const Vec3 c = box.center();
    target = QVector3D(float(c.x), float(c.y), float(c.z));
    const float r = std::max(float(box.diagonal()) * 0.5f, 1.f);
    sceneRadius = r;
    distance = r / std::sin(qDegreesToRadians(fovY) * 0.5f) * 1.1f;
}

Ray Camera::ray(QPointF pixel, QSize size) const
{
    const QMatrix4x4 inv = viewProjection(size).inverted();
    const float x = float(2.0 * pixel.x() / std::max(1, size.width()) - 1.0);
    const float y = float(1.0 - 2.0 * pixel.y() / std::max(1, size.height()));
    const QVector3D a = inv.map(QVector3D(x, y, -1.f));
    const QVector3D b = inv.map(QVector3D(x, y, 1.f));
    return {a, (b - a).normalized()};
}

bool Camera::project(const QVector3D& p, QSize size, QPointF* out, float* depth) const
{
    const QVector4D c = viewProjection(size) * QVector4D(p, 1.f);
    if (c.w() <= 1e-6f)
        return false;
    const float x = c.x() / c.w(), y = c.y() / c.w();
    if (out)
        *out = QPointF((x + 1.0) * 0.5 * size.width(), (1.0 - y) * 0.5 * size.height());
    if (depth)
        *depth = c.z() / c.w();
    return true;
}

}  // namespace silex::ui
