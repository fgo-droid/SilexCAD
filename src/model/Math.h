#pragma once
// Small OCCT-free math types shared by every layer (UI included).

#include <algorithm>
#include <cmath>
#include <limits>

namespace silex {

constexpr double kPi = 3.14159265358979323846;

struct Vec2
{
    double x = 0, y = 0;

    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(double s) const { return {x * s, y * s}; }
    double dot(const Vec2& o) const { return x * o.x + y * o.y; }
    double cross(const Vec2& o) const { return x * o.y - y * o.x; }
    double length() const { return std::sqrt(x * x + y * y); }
    Vec2 normalized() const
    {
        const double l = length();
        return l > 1e-15 ? Vec2{x / l, y / l} : Vec2{};
    }
    bool operator==(const Vec2&) const = default;
};

struct Vec3
{
    double x = 0, y = 0, z = 0;

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const
    {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    double length() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3 normalized() const
    {
        const double l = length();
        return l > 1e-15 ? Vec3{x / l, y / l, z / l} : Vec3{};
    }
    bool operator==(const Vec3&) const = default;
};

/// Local frame of a sketch plane / planar face. yDir = normal x xDir.
struct Placement
{
    Vec3 origin{0, 0, 0};
    Vec3 xDir{1, 0, 0};
    Vec3 normal{0, 0, 1};

    Vec3 yDir() const { return normal.cross(xDir); }
    Vec3 toWorld(const Vec2& p) const { return origin + xDir * p.x + yDir() * p.y; }
    Vec2 toLocal(const Vec3& p) const
    {
        const Vec3 d = p - origin;
        return {d.dot(xDir), d.dot(yDir())};
    }
    bool operator==(const Placement&) const = default;
};

struct BBox
{
    Vec3 min{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
             std::numeric_limits<double>::max()};
    Vec3 max{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
             std::numeric_limits<double>::lowest()};

    bool valid() const { return min.x <= max.x; }
    void add(const Vec3& p)
    {
        min = {std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z)};
        max = {std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z)};
    }
    void add(const BBox& b)
    {
        if (b.valid()) {
            add(b.min);
            add(b.max);
        }
    }
    Vec3 center() const { return (min + max) * 0.5; }
    double diagonal() const { return valid() ? (max - min).length() : 0.0; }
};

}  // namespace silex
