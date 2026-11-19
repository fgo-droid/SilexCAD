#include "kernel/NamedShape.h"

#include <algorithm>

#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>

namespace silex::kernel {

namespace {
const std::string kEmpty;
}

void NamedShape::reset(const TopoDS_Shape& s)
{
    m_shape = s;
    m_faces.Clear();
    m_edges.Clear();
    if (!s.IsNull()) {
        TopExp::MapShapes(s, TopAbs_FACE, m_faces);
        TopExp::MapShapes(s, TopAbs_EDGE, m_edges);
    }
    m_faceNames.assign(static_cast<size_t>(m_faces.Extent()), {});
    m_edgeNames.assign(static_cast<size_t>(m_edges.Extent()), {});
}

const std::string& NamedShape::faceName(int i) const
{
    const auto& v = m_faceNames[static_cast<size_t>(i - 1)];
    return v.empty() ? kEmpty : v.front();
}

bool NamedShape::faceHasName(int i, const std::string& n) const
{
    const auto& v = m_faceNames[static_cast<size_t>(i - 1)];
    return std::find(v.begin(), v.end(), n) != v.end();
}

void NamedShape::addFaceName(int i, const std::string& n)
{
    if (n.empty())
        return;
    auto& v = m_faceNames[static_cast<size_t>(i - 1)];
    if (std::find(v.begin(), v.end(), n) != v.end())
        return;
    v.push_back(n);
    std::sort(v.begin(), v.end());  // deterministic primary name
}

void NamedShape::setFaceName(int i, const std::string& n)
{
    m_faceNames[static_cast<size_t>(i - 1)] = {n};
}

NamedShape NamedShape::moved(const TopLoc_Location& loc) const
{
    NamedShape out(m_shape.Moved(loc));
    for (int i = 1; i <= m_faces.Extent(); ++i) {
        const int j = out.faceIndex(m_faces(i).Moved(loc));
        if (j > 0)
            for (const auto& n : faceAliases(i))
                out.addFaceName(j, n);
    }
    for (int i = 1; i <= m_edges.Extent(); ++i) {
        const int j = out.edgeIndex(m_edges(i).Moved(loc));
        if (j > 0)
            out.setEdgeName(j, edgeName(i));
    }
    return out;
}

}  // namespace silex::kernel
