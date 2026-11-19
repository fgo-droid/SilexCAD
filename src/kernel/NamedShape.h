#pragma once
// A B-Rep shape whose faces and edges carry persistent, history-based names.

#include <string>
#include <vector>

#include <TopLoc_Location.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Shape.hxx>

namespace silex::kernel {

class NamedShape
{
public:
    NamedShape() = default;
    explicit NamedShape(const TopoDS_Shape& s) { reset(s); }

    /// Sets the shape and (re)indexes its faces and edges; all names are cleared.
    void reset(const TopoDS_Shape& s);

    const TopoDS_Shape& shape() const { return m_shape; }
    bool isNull() const { return m_shape.IsNull(); }

    int faceCount() const { return m_faces.Extent(); }
    int edgeCount() const { return m_edges.Extent(); }
    /// 1-based, like OCCT maps.
    const TopoDS_Shape& face(int i) const { return m_faces(i); }
    const TopoDS_Shape& edge(int i) const { return m_edges(i); }
    int faceIndex(const TopoDS_Shape& f) const { return m_faces.FindIndex(f); }
    int edgeIndex(const TopoDS_Shape& e) const { return m_edges.FindIndex(e); }

    /// Primary (deterministic) name of a face; empty when unnamed.
    const std::string& faceName(int i) const;
    const std::vector<std::string>& faceAliases(int i) const { return m_faceNames[static_cast<size_t>(i - 1)]; }
    bool faceHasName(int i, const std::string& n) const;
    void addFaceName(int i, const std::string& n);
    void setFaceName(int i, const std::string& n);  ///< replaces every alias

    const std::string& edgeName(int i) const { return m_edgeNames[static_cast<size_t>(i - 1)]; }
    void setEdgeName(int i, const std::string& n) { m_edgeNames[static_cast<size_t>(i - 1)] = n; }

    /// Same names, shape moved by `loc` (used by symmetric extrusions).
    NamedShape moved(const TopLoc_Location& loc) const;

private:
    TopoDS_Shape m_shape;
    TopTools_IndexedMapOfShape m_faces, m_edges;
    std::vector<std::vector<std::string>> m_faceNames;
    std::vector<std::string> m_edgeNames;
};

}  // namespace silex::kernel
