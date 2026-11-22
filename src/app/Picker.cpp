#include "Picker.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "model/Features.h"

namespace silex::ui {

namespace {
QVector3D V(const float* p) { return {p[0], p[1], p[2]}; }

bool rayTri(const Ray& r, const QVector3D& a, const QVector3D& b, const QVector3D& c, float& t)
{
    const QVector3D e1 = b - a, e2 = c - a;
    const QVector3D p = QVector3D::crossProduct(r.dir, e2);
    const float det = QVector3D::dotProduct(e1, p);
    if (std::abs(det) < 1e-12f)
        return false;
    const float inv = 1.f / det;
    const QVector3D s = r.origin - a;
    const float u = QVector3D::dotProduct(s, p) * inv;
    if (u < 0.f || u > 1.f)
        return false;
    const QVector3D q = QVector3D::crossProduct(s, e1);
    const float v = QVector3D::dotProduct(r.dir, q) * inv;
    if (v < 0.f || u + v > 1.f)
        return false;
    t = QVector3D::dotProduct(e2, q) * inv;
    return t > 0.f;
}

bool rayBox(const Ray& r, const QVector3D& lo, const QVector3D& hi, float tMax)
{
    float t0 = 0.f, t1 = tMax;
    for (int i = 0; i < 3; ++i) {
        const float inv = 1.f / (std::abs(r.dir[i]) > 1e-12f ? r.dir[i] : 1e-12f);
        float a = (lo[i] - r.origin[i]) * inv, b = (hi[i] - r.origin[i]) * inv;
        if (a > b)
            std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1)
            return false;
    }
    return true;
}

double segDist(QPointF p, QPointF a, QPointF b, double* tOut)
{
    const QPointF d = b - a;
    const double l2 = d.x() * d.x() + d.y() * d.y();
    double t = l2 > 1e-12 ? ((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / l2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    if (tOut)
        *tOut = t;
    const QPointF f = a + d * t;
    return std::hypot(p.x() - f.x(), p.y() - f.y());
}
}  // namespace

void Picker::build(std::shared_ptr<const ModelSnapshot> snap, const std::vector<bool>& profileVisible,
                   const std::set<Uuid>& hiddenBodies)
{
    m_snap = std::move(snap);
    m_profileVisible = profileVisible;
    m_hiddenBodies = hiddenBodies;
    m_tris.clear();
    m_nodes.clear();
    if (!m_snap)
        return;
    auto addMesh = [&](const RenderMesh& m, int object) {
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            const float* p = m.positions.data();
            m_tris.push_back({V(p + 3 * m.indices[i]), V(p + 3 * m.indices[i + 1]), V(p + 3 * m.indices[i + 2]), object,
                              int(m.triFace[i / 3])});
        }
    };
    for (size_t b = 0; b < m_snap->bodies.size(); ++b)
        if (!m_hiddenBodies.count(m_snap->bodies[b].id))  // v0.4: hidden bodies cannot be picked
            addMesh(m_snap->bodies[b].mesh, int(b));
    for (size_t p = 0; p < m_snap->profiles.size(); ++p)
        if (p < m_profileVisible.size() && m_profileVisible[p])
            addMesh(m_snap->profiles[p].mesh, -int(p) - 1);
    if (!m_tris.empty())
        buildNode(0, int(m_tris.size()), 0);
}

int Picker::buildNode(int first, int count, int depth)
{
    Node n;
    n.lo = QVector3D(1e30f, 1e30f, 1e30f);
    n.hi = -n.lo;
    for (int i = first; i < first + count; ++i)
        for (const QVector3D& v : {m_tris[size_t(i)].a, m_tris[size_t(i)].b, m_tris[size_t(i)].c}) {
            n.lo = QVector3D(std::min(n.lo.x(), v.x()), std::min(n.lo.y(), v.y()), std::min(n.lo.z(), v.z()));
            n.hi = QVector3D(std::max(n.hi.x(), v.x()), std::max(n.hi.y(), v.y()), std::max(n.hi.z(), v.z()));
        }
    const int idx = int(m_nodes.size());
    m_nodes.push_back(n);
    if (count <= 4 || depth > 40) {
        m_nodes[size_t(idx)].first = first;
        m_nodes[size_t(idx)].count = count;
        return idx;
    }
    const QVector3D ext = n.hi - n.lo;
    const int axis = ext.x() > ext.y() ? (ext.x() > ext.z() ? 0 : 2) : (ext.y() > ext.z() ? 1 : 2);
    const int mid = first + count / 2;
    std::nth_element(m_tris.begin() + first, m_tris.begin() + mid, m_tris.begin() + first + count,
                     [axis](const Tri& x, const Tri& y) {
                         return (x.a[axis] + x.b[axis] + x.c[axis]) < (y.a[axis] + y.b[axis] + y.c[axis]);
                     });
    const int l = buildNode(first, mid - first, depth + 1);
    const int r = buildNode(mid, first + count - mid, depth + 1);
    m_nodes[size_t(idx)].left = l;
    m_nodes[size_t(idx)].right = r;
    return idx;
}

bool Picker::hitTris(const Ray& r, uint32_t filter, float& tBest, int& triBest) const
{
    if (m_nodes.empty())
        return false;
    std::vector<int> stack{0};
    bool hit = false;
    while (!stack.empty()) {
        const Node& n = m_nodes[size_t(stack.back())];
        stack.pop_back();
        if (!rayBox(r, n.lo, n.hi, tBest))
            continue;
        if (n.left >= 0) {
            stack.push_back(n.left);
            stack.push_back(n.right);
            continue;
        }
        for (int i = n.first; i < n.first + n.count; ++i) {
            const Tri& t = m_tris[size_t(i)];
            const bool isProfile = t.object < 0;
            if (isProfile && !(filter & SelFilter::Profiles))
                continue;
            float tt;
            if (!rayTri(r, t.a, t.b, t.c, tt))
                continue;
            if (clipped(r.origin + r.dir * tt))
                continue;
            // Profiles lie on body faces: give them priority at (almost) equal depth.
            const float bias = isProfile ? 1e-3f * std::max(1.f, tt) : 0.f;
            if (tt - bias < tBest) {
                tBest = tt - bias;
                triBest = i;
                hit = true;
            }
        }
    }
    return hit;
}

SelItem Picker::pick(const Camera& cam, QSize size, QPointF px, uint32_t filter) const
{
    SelItem none;
    if (!m_snap)
        return none;
    const Ray r = cam.ray(px, size);

    float tFace = std::numeric_limits<float>::max();
    int tri = -1;
    hitTris(r, filter, tFace, tri);

    // Origin planes (only when asked for, e.g. "pick a sketch plane")
    SelItem planeHit;
    float tPlane = std::numeric_limits<float>::max();
    if (filter & SelFilter::OriginPlanes) {
        const float s = std::max(cam.sceneRadius * 0.6f, 20.f);
        for (const char* k : {"XY", "XZ", "YZ"}) {
            const Placement pl = originPlacement(k);
            const QVector3D x(float(pl.xDir.x), float(pl.xDir.y), float(pl.xDir.z));
            const QVector3D n(float(pl.normal.x), float(pl.normal.y), float(pl.normal.z));
            const QVector3D y = QVector3D::crossProduct(n, x);
            const float denom = QVector3D::dotProduct(r.dir, n);
            if (std::abs(denom) < 1e-6f)
                continue;
            const float t = -QVector3D::dotProduct(r.origin, n) / denom;
            if (t <= 0 || t >= tPlane)
                continue;
            const QVector3D h = r.origin + r.dir * t;
            const float u = QVector3D::dotProduct(h, x), v = QVector3D::dotProduct(h, y);
            if (u < 0 || v < 0 || u > s || v > s)
                continue;
            tPlane = t;
            planeHit.kind = SelKind::OriginPlane;
            planeHit.name = k;
            planeHit.planar = true;
            planeHit.plane = pl;
        }
    }

    // Edges: closest projected segment within 7 px that is not hidden behind a face.
    if (filter & SelFilter::Edges) {
        double bestPx = 7.0;
        SelItem edgeHit;
        for (size_t b = 0; b < m_snap->bodies.size(); ++b) {
            if (m_hiddenBodies.count(m_snap->bodies[b].id))
                continue;
            const RenderMesh& m = m_snap->bodies[b].mesh;
            for (size_t ei = 0; ei < m.edges.size(); ++ei) {
                const MeshEdge& e = m.edges[ei];
                for (uint32_t k = e.first; k + 1 < e.first + e.count; k += 2) {
                    const QVector3D a = V(&m.edgePoints[3 * k]), c = V(&m.edgePoints[3 * (k + 1)]);
                    if (clipped((a + c) * 0.5f))
                        continue;
                    QPointF pa, pc;
                    if (!cam.project(a, size, &pa) || !cam.project(c, size, &pc))
                        continue;
                    double t;
                    const double d = segDist(px, pa, pc, &t);
                    if (d >= bestPx)
                        continue;
                    const QVector3D w = a + (c - a) * float(t);
                    const float tw = QVector3D::dotProduct(w - r.origin, r.dir);
                    if (tri >= 0 && tw > tFace + 1e-3f * std::max(1.f, cam.distance))
                        continue;  // occluded
                    bestPx = d;
                    edgeHit.kind = SelKind::Edge;
                    edgeHit.owner = m_snap->bodies[b].id;
                    edgeHit.object = int(b);
                    edgeHit.index = int(ei);
                    edgeHit.name = e.name;
                    edgeHit.sig = e.sig;
                }
            }
        }
        if (edgeHit.valid())
            return edgeHit;
    }

    if (tri >= 0 && tFace < tPlane) {
        const Tri& t = m_tris[size_t(tri)];
        SelItem s;
        if (t.object < 0) {
            const int p = -t.object - 1;
            const ProfileView& pv = m_snap->profiles[size_t(p)];
            s.kind = SelKind::Profile;
            s.owner = pv.sketchId;
            s.name = pv.regionKey;
            s.object = p;
            s.index = 0;
            if (!pv.mesh.faces.empty()) {
                s.sig = pv.mesh.faces.front().sig;
                s.planar = true;
                s.plane = pv.mesh.faces.front().plane;
            }
            return s;
        }
        if (!(filter & SelFilter::Faces))
            return planeHit;
        const BodyView& bv = m_snap->bodies[size_t(t.object)];
        const MeshFace& f = bv.mesh.faces[size_t(t.face)];
        if ((filter & SelFilter::PlanarOnly) && !f.planar)
            return planeHit;
        s.kind = SelKind::Face;
        s.owner = bv.id;
        s.object = t.object;
        s.index = t.face;
        s.name = f.name;
        s.sig = f.sig;
        s.planar = f.planar;
        s.plane = f.plane;
        return s;
    }
    return planeHit;
}

}  // namespace silex::ui
