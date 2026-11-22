#pragma once
// CPU picking: BVH ray-casting for faces/profiles + screen-space distance for edges.

#include <memory>
#include <set>
#include <vector>

#include <QPointF>
#include <QSize>
#include <QVector3D>

#include "Camera.h"
#include "Selection.h"
#include "model/RenderModel.h"

namespace silex::ui {

class Picker
{
public:
    /// `profileVisible[i]` tells whether snapshot.profiles[i] can be picked.
    void build(std::shared_ptr<const ModelSnapshot> snap, const std::vector<bool>& profileVisible,
               const std::set<Uuid>& hiddenBodies = {});
    SelItem pick(const Camera& cam, QSize size, QPointF px, uint32_t filter) const;
    /// Section view: entities beyond the plane dot(n, p) > d cannot be picked.
    void setClip(bool on, QVector3D n = {}, float d = 0)
    {
        m_clipOn = on;
        m_clipN = n;
        m_clipD = d;
    }
    bool clipped(const QVector3D& p) const { return m_clipOn && QVector3D::dotProduct(m_clipN, p) > m_clipD + 1e-4f; }

private:
    struct Tri
    {
        QVector3D a, b, c;
        int object;  ///< >= 0 body index, < 0 => profile -(i+1)
        int face;
    };
    struct Node
    {
        QVector3D lo, hi;
        int left = -1, right = -1;  ///< children (inner node)
        int first = 0, count = 0;   ///< triangles (leaf)
    };

    int buildNode(int first, int count, int depth);
    bool hitTris(const Ray& r, uint32_t filter, float& tBest, int& triBest) const;

    std::shared_ptr<const ModelSnapshot> m_snap;
    std::vector<bool> m_profileVisible;
    std::set<Uuid> m_hiddenBodies;
    std::vector<Tri> m_tris;
    std::vector<Node> m_nodes;
    bool m_clipOn = false;
    QVector3D m_clipN;
    float m_clipD = 0;
};

}  // namespace silex::ui
