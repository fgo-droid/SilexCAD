#pragma once
// Persistent topological naming (answer to the Topological Naming Problem).
//
// Naming grammar (all names are built from stable UUIDs, never from OCCT indices):
//   sketch curve       <sketchId>/<geoId>
//   sketch region      <sketchId>:region(<geoId,geoId,...>)
//   generated face     <featId>:<kind>(<source edge name>)   kind = side | fillet | chamfer
//   cap faces          <featId>:start   <featId>:end
//   imported face      <featId>:f<n>
//   leftover face      <featId>:new<n>     (sorted geometrically, deterministic)
//   edge               E[<face name>|<face name>]   (names of the two adjacent faces)
//
// Names are propagated through every OCCT operation using its history
// (Modified / Generated / IsDeleted). When a reference is resolved, candidates sharing the
// name are disambiguated with a geometric signature (centroid, normal/tangent, area/length);
// if the name vanished, the closest signature is used as a fallback and a warning is raised.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <BRepBuilderAPI_MakeShape.hxx>
#include <BRepTools_History.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

#include "kernel/NamedShape.h"
#include "model/Math.h"
#include "model/TopoRef.h"

namespace silex::kernel {

GeomSignature faceSignature(const TopoDS_Face& f);
GeomSignature edgeSignature(const TopoDS_Edge& e);
/// Plane frame of a planar face, normal oriented like the face (outward on solids).
std::optional<Placement> facePlacement(const TopoDS_Face& f);

/// Uniform view over the history of an OCCT algorithm.
struct ShapeHistory
{
    std::function<const TopTools_ListOfShape&(const TopoDS_Shape&)> modified;
    std::function<const TopTools_ListOfShape&(const TopoDS_Shape&)> generated;
    std::function<bool(const TopoDS_Shape&)> isDeleted;
};
ShapeHistory historyOf(BRepBuilderAPI_MakeShape& op);
ShapeHistory historyOf(const Handle(BRepTools_History)& h);

/// Names the faces of `result` from the faces of `inputs` (Modified / unchanged), then
/// faces generated from input edges as "<featId>:<generatedKind>(<edge name>)", then
/// leftovers as "<featId>:new<n>", and finally every edge from its adjacent faces.
NamedShape propagateNames(const TopoDS_Shape& result, const std::vector<const NamedShape*>& inputs,
                          const ShapeHistory& history, const std::string& featId,
                          const std::string& generatedKind = "gen");

/// Names every edge "E[a|b]" from the primary names of its adjacent faces.
void nameEdgesFromFaces(NamedShape& ns);

struct ResolvedRef
{
    TopoDS_Shape shape;
    std::string owner;       ///< body id
    bool byName = true;      ///< false when re-attached by geometry only
};

/// Finds the sub-shape (TopAbs_FACE or TopAbs_EDGE) designated by `ref` among `bodies`.
template<typename BodyRange>
std::optional<ResolvedRef> resolveRef(const BodyRange& bodies, const TopoRef& ref, TopAbs_ShapeEnum type,
                                      double modelSize);

}  // namespace silex::kernel

#include "kernel/TopoNaming.inl"
