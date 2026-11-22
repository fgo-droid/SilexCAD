#pragma once
// What the kernel hands to the UI after a recompute: display meshes tagged with persistent
// names and per-feature status. The UI never sees a TopoDS_Shape.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "model/Features.h"
#include "model/Material.h"
#include "model/Math.h"
#include "model/TopoRef.h"
#include "model/Uuid.h"

namespace silex {

struct MeshFace
{
    std::string name;     ///< persistent topological name
    GeomSignature sig;
    bool planar = false;
    Placement plane;      ///< valid when planar (oriented outward)
};

struct MeshEdge
{
    std::string name;
    GeomSignature sig;
    uint32_t first = 0;   ///< first vertex in RenderMesh::edgePoints (line list)
    uint32_t count = 0;   ///< number of vertices (even)
};

struct RenderMesh
{
    std::vector<float> positions;     ///< xyz
    std::vector<float> normals;       ///< xyz
    std::vector<uint32_t> indices;    ///< triangles
    std::vector<uint32_t> triFace;    ///< face index of each triangle
    std::vector<MeshFace> faces;
    std::vector<float> edgePoints;    ///< xyz, line list (pairs)
    std::vector<MeshEdge> edges;
    BBox bbox;

    bool empty() const { return indices.empty() && edgePoints.empty(); }
};

struct BodyView
{
    Uuid id;
    std::string name;
    RenderMesh mesh;
    uint64_t meshVersion = 0;  ///< changes only when the tessellation changes
    // v0.3 physical properties (BRepGProp) and appearance
    double volume = 0;         ///< mm3
    double area = 0;           ///< mm2
    BodyMaterial material;
    std::string component;  ///< v0.5 lot 1: catalog id of a locked reference component ("" = part)
    bool locked() const { return !component.empty(); }
    double mass() const { return massGrams(volume, material.density); }
};

struct ProfileView
{
    Uuid sketchId;
    std::string regionKey;
    RenderMesh mesh;
    Vec3 centroid;
};

struct SketchView
{
    Uuid id;
    Placement plane;
    bool resolved = true;
    Sketch sketch;  ///< v0.3: as computed (projected edges re-attached to the model, re-solved)
};

/// v0.4 lot 1: construction plane (drawn as a translucent square, usable as a sketch / split plane).
struct PlaneView
{
    Uuid id;
    std::string name;
    Placement plane;
    double size = 50;  ///< half side of the displayed square (mm)
};

/// v0.4 lot 2: cosmetic thread (annotation only), drawn as dashed rings with its label.
struct ThreadView
{
    Uuid id;
    std::string label;  ///< "M6x1"
    Vec3 a, b;          ///< axis segment of the thread
    double radius = 0;
};

enum class FeatureStatus { Ok, Warning, Error, Suppressed, RolledBack };

struct FeatureResult
{
    Uuid id;
    FeatureStatus status = FeatureStatus::Ok;
    std::string message;
    double milliseconds = 0;
};

struct ModelSnapshot
{
    uint64_t revision = 0;
    std::vector<BodyView> bodies;
    std::vector<ProfileView> profiles;
    std::vector<SketchView> sketches;
    std::vector<PlaneView> planes;  ///< v0.4
    std::vector<ThreadView> threads;  ///< v0.4 cosmetic threads
    std::vector<FeatureResult> results;
    std::optional<RenderMesh> preview;  ///< tool body of the feature being edited
    BooleanOp previewOp = BooleanOp::NewBody;
    BBox bbox;
    double computeMs = 0;

    const FeatureResult* result(const Uuid& id) const
    {
        for (const auto& r : results)
            if (r.id == id)
                return &r;
        return nullptr;
    }
    const SketchView* sketch(const Uuid& id) const
    {
        for (const auto& s : sketches)
            if (s.id == id)
                return &s;
        return nullptr;
    }
};

}  // namespace silex
