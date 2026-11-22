#pragma once
// Public, OCCT-free facade of the geometric core.
//
// KernelEngine owns a dedicated worker thread. submit() never blocks: a newer request
// cancels the one in flight (std::stop_token => OCCT progress user-break) so that dragging
// a value recomputes "at the speed of the hand" without queuing stale work.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "model/Document.h"
#include "model/DrawingData.h"
#include "model/Pedagogy.h"
#include "model/RenderModel.h"

namespace silex {

struct SolidMetrics
{
    double volume = 0;
    Vec3 center;
    BBox bbox;
};

struct BodyMetrics
{
    std::vector<SolidMetrics> solidList;  ///< per solid (useful for imported assemblies)
    Uuid id;
    std::string name;
    double volume = 0;
    double area = 0;
    Vec3 centerOfMass;
    BBox bbox;
    bool valid = false;  ///< BRepCheck_Analyzer
    bool locked = false; ///< v0.5: locked reference component
    int solids = 0, faces = 0, edges = 0;
    std::vector<std::string> faceNames, edgeNames;
};

struct Analysis
{
    std::vector<FeatureResult> results;
    std::vector<BodyMetrics> bodies;
    const FeatureResult* result(const Uuid& id) const
    {
        for (const auto& r : results)
            if (r.id == id)
                return &r;
        return nullptr;
    }
};

// ---------------------------------------------------------------- v0.3 analysis services

/// An entity to measure: a face / edge (persistent reference) or a whole body.
struct MeasureRef
{
    enum class Kind { Face, Edge, Body } kind = Kind::Face;
    Uuid body;    ///< owner body (Face/Edge: optional hint) or the body itself
    TopoRef ref;  ///< Face / Edge
};

struct MeasureCircle
{
    Vec3 center;
    Vec3 axis;
    double radius = 0;
};

struct MeasureResult
{
    bool ok = false;
    std::string error;
    // distance (two entities)
    bool hasDistance = false;
    double distance = 0;   ///< minimal distance (mm), BRepExtrema_DistShapeShape
    Vec3 p1, p2;           ///< closest points
    Vec3 delta;            ///< p2 - p1 (projected components dX dY dZ)
    // angle between two planar faces or two straight edges (degrees, 0..90 for lines, 0..180 for faces)
    bool hasAngle = false;
    double angleDeg = 0;
    // single entity properties
    std::vector<std::string> types;          ///< "plane", "cylinder", "line", "circle", "body"...
    std::vector<MeasureCircle> circles;      ///< circular edges / cylindrical faces among the entities
    double length = 0;                       ///< edge length (single edge)
    double area = 0;                         ///< face area (single face)
};

struct InterferencePair
{
    Uuid a, b;
    std::string nameA, nameB;
    double overlapVolume = 0;  ///< mm3 (BRepAlgoAPI_Common)
    double clearance = 0;      ///< minimal distance (mm); 0 when touching or colliding
    std::string status;        ///< "collision" (red) | "tight" (orange, < 0.15 mm) | "ok" (green)
    Vec3 p1, p2;               ///< closest points
    RenderMesh overlap;        ///< overlap volume, for display (when requested)
};

struct InterferenceReport
{
    std::vector<InterferencePair> pairs;
    bool collision = false;
    double minClearance = 0;   ///< over the pairs without collision (0 if none)
    std::string error;
};

/// FDM thresholds (Lot 1.2): collision above 1e-4 mm3, tight fit under 0.15 mm.
inline constexpr double kCollisionVolume = 1e-4;
inline constexpr double kTightClearance = 0.15;

struct ThreeMfOptions
{
    std::vector<Uuid> bodies;        ///< empty = every body
    double linearDeflection = 0.05;  ///< mm
    std::string title = "Silex design";
    std::string designer;
};

struct ExportSummary
{
    std::string error;  ///< empty on success
    int objects = 0;
    size_t triangles = 0;
    size_t bytes = 0;
};

/// Result of KernelEngine::selfTest() (installation diagnostic, `silex --doctor`).
struct KernelSelfTest
{
    bool ok = false;
    std::string detail;  ///< what was checked, or the failure
};

class KernelEngine
{
public:
    /// Builds a 10 x 20 x 30 box (BRepPrimAPI_MakeBox), checks it with BRepCheck_Analyzer and its
    /// volume: proves that the OpenCascade libraries load and work.
    static KernelSelfTest selfTest();
    /// OpenCascade version the kernel was built with ("7.8.1").
    static std::string occtVersion();

    // ---- v0.7 lot A: fonts of the parametric text
    /// Searched before <exe>/resources/fonts and the system font folders.
    static void addFontDirectory(const std::string& dir);
    /// Family names of the installed / bundled fonts (regular style), sorted.
    static std::vector<std::string> fontFamilies();
    struct TextMetrics
    {
        bool ok = false;
        std::string error, fontFile;
        double area = 0;         ///< mm²
        double strokeWidth = 0;  ///< estimated stem width (mm)
        int faces = 0;
    };
    /// Lays out a text without a model (inspector hints, tests).
    static TextMetrics measureText(const std::string& text, const std::string& font, double height);

    using Callback = std::function<void(std::shared_ptr<const ModelSnapshot>)>;

    /// `onResult` is invoked on the worker thread.
    explicit KernelEngine(Callback onResult);
    ~KernelEngine();
    KernelEngine(const KernelEngine&) = delete;
    KernelEngine& operator=(const KernelEngine&) = delete;

    /// Asynchronous recompute. `previewFeature` (may be empty) gets its tool body returned
    /// in ModelSnapshot::preview for transparent display.
    void submit(const DocState& state, const Uuid& previewFeature, uint64_t revision);

    // ---- synchronous services (any thread) ------------------------------------------------
    static std::shared_ptr<const ModelSnapshot> computeSnapshot(const DocState& state,
                                                                const Uuid& previewFeature = {});
    static Analysis analyze(const DocState& state);
    /// Exports every body (or only `bodyId`). Returns an error message on failure.
    static std::optional<std::string> exportStep(const DocState& state, const std::string& path,
                                                 const Uuid& bodyId = {});
    static std::optional<std::string> exportStl(const DocState& state, const std::string& path,
                                                const Uuid& bodyId = {}, double linearDeflection = 0.05);
    /// Reads a STEP file and returns the shape as an embeddable B-Rep string.
    static std::string importStepAsBrep(const std::string& path);  // throws std::runtime_error
    /// v0.5 lot 2: computes the facts the given lesson criteria need beyond collectFacts (collisions,
    /// "robust" what-if recomputes). Only what is referenced is computed.
    static void enrichLessonFacts(ModelFacts& f, const DocState& doc, const std::vector<Json>& criteria);

    // ---- v0.3 services. The static versions recompute the document; the member versions reuse
    //      the geometry of the last result computed by the worker (instantaneous in the UI).
    static MeasureResult measure(const DocState& state, const std::vector<MeasureRef>& refs);
    MeasureResult measureLast(const std::vector<MeasureRef>& refs) const;
    /// Every pair of `bodies` (all bodies when empty). `meshes`: tessellate the overlap volumes.
    static InterferenceReport checkInterference(const DocState& state, const std::vector<Uuid>& bodies,
                                                bool meshes = false);
    InterferenceReport checkInterferenceLast(const std::vector<Uuid>& bodies, bool meshes = true) const;
    /// 3MF (zip/OPC) export: one object per body, names, colours (basematerials), millimetres.
    static ExportSummary export3mf(const DocState& state, const std::string& path, const ThreeMfOptions& o = {});
    /// Hidden-line projection (HLRBRep) of the bodies (all when empty) in 3 orthographic views.
    static DrawingProjection projectDrawing(const DocState& state, const std::vector<Uuid>& bodies = {});

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace silex
