#pragma once
// ParametricFeature: the plugin interface every modelling operation implements.

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressRange.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

#include "kernel/NamedShape.h"
#include "model/Document.h"
#include "model/Features.h"
#include "model/Sketch.h"

namespace silex::kernel {

struct SketchRegion
{
    std::string key;    ///< sorted ids of the sketch curves bounding the outer loop
    NamedShape named;   ///< planar face, edges named "<sketchId>/<geoId>"
};

struct SketchOutput
{
    Placement plane;
    Sketch sketch;
    std::vector<SketchRegion> regions;
};

/// The material a feature added or removed (extrusion / revolution tool), so that mirror and
/// pattern features can replicate a *feature* (a hole, a boss) and not only whole bodies.
struct FeatureTool
{
    NamedShape tool;
    BooleanOp op = BooleanOp::Join;  ///< NewBody is stored as Join
    Uuid body;                        ///< body the tool was applied to
};

/// Geometric state flowing through the timeline.
struct ModelState
{
    std::map<Uuid, NamedShape> bodies;
    std::vector<Uuid> bodyOrder;
    std::map<Uuid, std::string> bodyNames;
    std::map<Uuid, SketchOutput> sketches;
    std::map<Uuid, FeatureTool> featureTools;

    double modelSize() const;
};

/// Error raised by a feature; the feature is flagged in the timeline and the last valid
/// geometry is kept (the application never crashes because of a failing operation).
class FeatureError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// OCCT progress indicator bridging std::stop_token => user break (Cancellation Token).
class StopTokenProgress : public Message_ProgressIndicator
{
public:
    explicit StopTokenProgress(std::stop_token t) : m_token(std::move(t)) {}
    Standard_Boolean UserBreak() override { return m_token.stop_requested(); }
    void Show(const Message_ProgressScope&, const Standard_Boolean) override {}

private:
    std::stop_token m_token;
};

struct ExecContext
{
    ExecContext(ModelState& s, const FeatureNode& n, std::stop_token t, bool preview)
        : state(s), node(n), stop(std::move(t)), wantPreview(preview)
    {
    }

    ModelState& state;
    const FeatureNode& node;
    std::stop_token stop;
    bool wantPreview = false;

    std::optional<TopoDS_Shape> preview;
    BooleanOp previewOp = BooleanOp::NewBody;
    /// When true (fillet/chamfer editing) the preview shows the result as a ghost while
    /// the model keeps the *input* geometry, so the user keeps picking original edges.
    bool previewKeepsInput = false;
    std::vector<std::string> warnings;

    Message_ProgressRange progress();
    void throwIfCancelled() const;

private:
    Handle(StopTokenProgress) m_progress;
};

class ParametricFeature
{
public:
    explicit ParametricFeature(const FeatureNode& n) : m_node(n) {}
    virtual ~ParametricFeature() = default;
    virtual void execute(ExecContext& ctx) const = 0;
    const FeatureNode& node() const { return m_node; }

protected:
    const FeatureNode& m_node;
};

using FeatureFactory = std::function<std::unique_ptr<ParametricFeature>(const FeatureNode&)>;

class FeatureRegistry
{
public:
    static FeatureRegistry& instance();
    void add(const std::string& type, FeatureFactory f) { m_factories[type] = std::move(f); }
    std::unique_ptr<ParametricFeature> create(const FeatureNode& n) const;

private:
    std::map<std::string, FeatureFactory> m_factories;
};

/// Registers the built-in features (explicit: static-library-safe).
void registerBuiltinFeatures();

// ---- helpers shared by operations ------------------------------------------------------

/// Profile faces selected by a ProfileRef (all regions when the list is empty).
std::vector<const SketchRegion*> selectRegions(const ExecContext& ctx, const ProfileRef& ref);

/// Combines `tool` with the target body (or creates a new body) and propagates names.
/// Returns the id of the body that received the result.
Uuid applyBoolean(ExecContext& ctx, const NamedShape& tool, BooleanOp op, const Uuid& targetBody);

/// Fuses several named tools into one (names propagated).
NamedShape fuseAll(ExecContext& ctx, const std::vector<NamedShape>& tools);

/// Throws FeatureError if the shape is not a valid, non-empty solid.
void checkSolid(const TopoDS_Shape& s, const std::string& what);

}  // namespace silex::kernel
