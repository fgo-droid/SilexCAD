#include "model/Printability.h"

#include <algorithm>
#include <cmath>

namespace silex {

namespace {

Vec3 vtx(const RenderMesh& m, uint32_t i) { return {m.positions[3 * i], m.positions[3 * i + 1], m.positions[3 * i + 2]}; }
Vec3 nrm(const RenderMesh& m, uint32_t i)
{
    if (m.normals.size() < 3 * size_t(i) + 3)
        return {};
    return {m.normals[3 * i], m.normals[3 * i + 1], m.normals[3 * i + 2]};
}

struct Tri
{
    Vec3 a, b, c, n;  ///< n: unit outward normal
    double area;
    Vec3 lo, hi;
};

/// Möller–Trumbore; returns t > 0 or -1.
double rayTri(const Vec3& o, const Vec3& d, const Tri& t)
{
    const Vec3 e1 = t.b - t.a, e2 = t.c - t.a;
    const Vec3 p = d.cross(e2);
    const double det = e1.dot(p);
    if (std::abs(det) < 1e-14)
        return -1;
    const double inv = 1.0 / det;
    const Vec3 s = o - t.a;
    const double u = s.dot(p) * inv;
    if (u < -1e-9 || u > 1 + 1e-9)
        return -1;
    const Vec3 q = s.cross(e1);
    const double v = d.dot(q) * inv;
    if (v < -1e-9 || u + v > 1 + 1e-9)
        return -1;
    const double tt = e2.dot(q) * inv;
    return tt > 0 ? tt : -1;
}

bool rayHitsBox(const Vec3& o, const Vec3& d, const Vec3& lo, const Vec3& hi, double tMax)
{
    double t0 = 0, t1 = tMax;
    const double oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z}, l[3] = {lo.x, lo.y, lo.z}, h[3] = {hi.x, hi.y, hi.z};
    for (int k = 0; k < 3; ++k) {
        if (std::abs(dd[k]) < 1e-15) {
            if (oo[k] < l[k] - 1e-9 || oo[k] > h[k] + 1e-9)
                return false;
            continue;
        }
        double a = (l[k] - oo[k]) / dd[k], b = (h[k] - oo[k]) / dd[k];
        if (a > b)
            std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1 + 1e-12)
            return false;
    }
    return true;
}

std::vector<Tri> triangles(const RenderMesh& m)
{
    std::vector<Tri> out;
    out.reserve(m.indices.size() / 3);
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        Tri t;
        t.a = vtx(m, m.indices[i]);
        t.b = vtx(m, m.indices[i + 1]);
        t.c = vtx(m, m.indices[i + 2]);
        Vec3 n = (t.b - t.a).cross(t.c - t.a);
        t.area = n.length() * 0.5;
        n = n.normalized();
        // orientation from the shading normals (outward)
        const Vec3 vn = nrm(m, m.indices[i]) + nrm(m, m.indices[i + 1]) + nrm(m, m.indices[i + 2]);
        if (vn.length() > 1e-12 && vn.dot(n) < 0)
            n = n * -1.0;
        t.n = n;
        t.lo = {std::min({t.a.x, t.b.x, t.c.x}), std::min({t.a.y, t.b.y, t.c.y}), std::min({t.a.z, t.b.z, t.c.z})};
        t.hi = {std::max({t.a.x, t.b.x, t.c.x}), std::max({t.a.y, t.b.y, t.c.y}), std::max({t.a.z, t.b.z, t.c.z})};
        out.push_back(t);
    }
    return out;
}

}  // namespace

PrintabilityResult analyzePrintability(const RenderMesh& mesh, const PrintabilityOptions& o)
{
    PrintabilityResult r;
    const auto tris = triangles(mesh);
    const Vec3 up = o.buildDir.normalized();
    r.overhang.assign(tris.size(), TriClass::Ok);
    r.thin.assign(tris.size(), 0);
    if (tris.empty())
        return r;
    double plate = 1e300;
    for (const Tri& t : tris)
        plate = std::min({plate, t.a.dot(up), t.b.dot(up), t.c.dot(up)});

    const double kDeg = 180.0 / 3.14159265358979323846;
    for (size_t i = 0; i < tris.size(); ++i) {
        const Tri& t = tris[i];
        r.area += t.area;
        const double down = -t.n.dot(up);  // > 0: facing the plate
        if (down <= 1e-9)
            continue;
        const double angle = std::asin(std::min(1.0, down)) * kDeg;  // from the vertical
        const bool onPlate = std::max({t.a.dot(up), t.b.dot(up), t.c.dot(up)}) - plate < 1e-4;
        if (onPlate)
            continue;
        if (angle > o.failAngle + 1e-9) {
            r.overhang[i] = TriClass::Fail;
            r.failArea += t.area;
        }
        else if (angle >= o.warnAngle - 1e-9) {
            r.overhang[i] = TriClass::Warn;
            r.warnArea += t.area;
        }
    }

    if (o.checkWalls) {
        // inward ray from each triangle centroid: first hit on the other side = local thickness
        const double maxRay = std::max(o.minWall * 4, 1.0);
        for (size_t i = 0; i < tris.size(); ++i) {
            const Tri& t = tris[i];
            const Vec3 c = (t.a + t.b + t.c) * (1.0 / 3.0);
            const Vec3 d = t.n * -1.0;
            const Vec3 o0 = c + d * 1e-6;
            double best = -1;
            for (size_t j = 0; j < tris.size(); ++j) {
                if (j == i || !rayHitsBox(o0, d, tris[j].lo, tris[j].hi, maxRay))
                    continue;
                if (tris[j].n.dot(d) <= 0)
                    continue;  // only the inner side of the opposite wall (exit face)
                const double h = rayTri(o0, d, tris[j]);
                if (h > 1e-7 && (best < 0 || h < best))
                    best = h;
            }
            if (best < 0)
                continue;
            if (r.minThickness < 0 || best < r.minThickness)
                r.minThickness = best;
            if (best < o.minWall - 1e-9) {
                r.thin[i] = 1;
                r.thinArea += t.area;
            }
        }
    }
    return r;
}

RenderMesh extractTriangles(const RenderMesh& mesh, const std::vector<bool>& keep, double lift)
{
    RenderMesh out;
    const auto tris = triangles(mesh);
    for (size_t i = 0; i < tris.size() && i < keep.size(); ++i) {
        if (!keep[i])
            continue;
        const Tri& t = tris[i];
        const uint32_t base = uint32_t(out.positions.size() / 3);
        for (const Vec3& p : {t.a, t.b, t.c}) {
            const Vec3 q = p + t.n * lift;
            out.positions.insert(out.positions.end(), {float(q.x), float(q.y), float(q.z)});
            out.normals.insert(out.normals.end(), {float(t.n.x), float(t.n.y), float(t.n.z)});
            out.bbox.add(q);
        }
        out.indices.insert(out.indices.end(), {base, base + 1, base + 2});
        out.triFace.push_back(0);
    }
    if (!out.indices.empty())
        out.faces.push_back(MeshFace{});
    return out;
}

}  // namespace silex
