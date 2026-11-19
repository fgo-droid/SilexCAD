#pragma once
// UI-side selection: identifiers only (body/sketch UUID + persistent name), never shapes.

#include <cstdint>
#include <string>
#include <vector>

#include "model/Math.h"
#include "model/TopoRef.h"
#include "model/Uuid.h"

namespace silex::ui {

enum class SelKind { None, Face, Edge, Profile, OriginPlane, Body };

struct SelItem
{
    SelKind kind = SelKind::None;
    Uuid owner;          ///< body id (Face/Edge) or sketch id (Profile)
    std::string name;    ///< persistent name / region key / "XY" "XZ" "YZ"
    GeomSignature sig;
    int object = -1;     ///< index of the body / profile in the snapshot
    int index = -1;      ///< face / edge index in the mesh
    bool planar = false;
    Placement plane;

    bool valid() const { return kind != SelKind::None; }
    bool sameAs(const SelItem& o) const { return kind == o.kind && owner == o.owner && name == o.name && index == o.index; }
    TopoRef ref() const { return TopoRef{name, sig}; }
};

namespace SelFilter {
enum : uint32_t {
    Faces = 1,
    Edges = 2,
    Profiles = 4,
    OriginPlanes = 8,
    PlanarOnly = 16,
    Model = Faces | Edges,
    All = Faces | Edges | Profiles,
};
}

}  // namespace silex::ui
