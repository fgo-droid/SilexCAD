#include "kernel/Tessellator.h"

#include <algorithm>

#include <BRepAdaptor_Curve.hxx>
#include <BRepLib_ToolTriangulatedShape.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <Poly_Triangulation.hxx>
#include <TopoDS.hxx>

#include "kernel/TopoNaming.h"

namespace silex::kernel {

TessellationParams tessellationFor(double modelSize)
{
    TessellationParams p;
    p.linearDeflection = std::clamp(modelSize * 0.0008, 0.002, 2.0);
    p.angularDeflection = 0.3;
    return p;
}

RenderMesh tessellate(const TopoDS_Shape& s, const TessellationParams& p)
{
    return tessellate(NamedShape(s), p);
}

RenderMesh tessellate(const NamedShape& ns, const TessellationParams& p)
{
    RenderMesh m;
    if (ns.isNull())
        return m;
    BRepMesh_IncrementalMesh mesher(ns.shape(), p.linearDeflection, Standard_False, p.angularDeflection,
                                    Standard_True);

    for (int fi = 1; fi <= ns.faceCount(); ++fi) {
        const TopoDS_Face face = TopoDS::Face(ns.face(fi));
        MeshFace mf;
        mf.name = ns.faceName(fi);
        mf.sig = faceSignature(face);
        if (auto pl = facePlacement(face)) {
            mf.planar = true;
            mf.plane = *pl;
        }
        const uint32_t faceIndex = static_cast<uint32_t>(m.faces.size());
        m.faces.push_back(mf);

        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull() || tri->NbTriangles() == 0)
            continue;
        if (!tri->HasNormals())
            BRepLib_ToolTriangulatedShape::ComputeNormals(face, tri);
        const gp_Trsf trsf = loc.Transformation();
        const bool reversed = face.Orientation() == TopAbs_REVERSED;
        const uint32_t base = static_cast<uint32_t>(m.positions.size() / 3);

        for (int i = 1; i <= tri->NbNodes(); ++i) {
            gp_Pnt pt = tri->Node(i).Transformed(trsf);
            m.positions.insert(m.positions.end(), {static_cast<float>(pt.X()), static_cast<float>(pt.Y()),
                                                   static_cast<float>(pt.Z())});
            m.bbox.add(Vec3{pt.X(), pt.Y(), pt.Z()});
            gp_Dir n = tri->HasNormals() ? tri->Normal(i) : gp_Dir(0, 0, 1);
            n.Transform(trsf);
            if (reversed)
                n.Reverse();
            m.normals.insert(m.normals.end(),
                             {static_cast<float>(n.X()), static_cast<float>(n.Y()), static_cast<float>(n.Z())});
        }
        for (int i = 1; i <= tri->NbTriangles(); ++i) {
            int a, b, c;
            tri->Triangle(i).Get(a, b, c);
            if (reversed)
                std::swap(b, c);
            m.indices.insert(m.indices.end(), {base + static_cast<uint32_t>(a - 1), base + static_cast<uint32_t>(b - 1),
                                               base + static_cast<uint32_t>(c - 1)});
            m.triFace.push_back(faceIndex);
        }
    }

    if (p.withEdges) {
        for (int ei = 1; ei <= ns.edgeCount(); ++ei) {
            const TopoDS_Edge edge = TopoDS::Edge(ns.edge(ei));
            if (BRep_Tool::Degenerated(edge))
                continue;
            MeshEdge me;
            me.name = ns.edgeName(ei);
            me.sig = edgeSignature(edge);
            me.first = static_cast<uint32_t>(m.edgePoints.size() / 3);
            try {
                BRepAdaptor_Curve curve(edge);
                GCPnts_TangentialDeflection disc(curve, p.angularDeflection * 0.5, p.linearDeflection);
                for (int i = 1; i < disc.NbPoints(); ++i) {
                    for (const gp_Pnt& q : {disc.Value(i), disc.Value(i + 1)})
                        m.edgePoints.insert(m.edgePoints.end(), {static_cast<float>(q.X()), static_cast<float>(q.Y()),
                                                                 static_cast<float>(q.Z())});
                }
            }
            catch (const Standard_Failure&) {
                continue;
            }
            me.count = static_cast<uint32_t>(m.edgePoints.size() / 3) - me.first;
            if (me.count > 0)
                m.edges.push_back(me);
        }
    }
    return m;
}

}  // namespace silex::kernel
