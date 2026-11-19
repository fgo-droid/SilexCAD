#pragma once
// What the kernel hands to the UI after a recompute: display meshes tagged with persistent
// names and per-feature status. The UI never sees a TopoDS_Shape.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "model/Features.h"
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
