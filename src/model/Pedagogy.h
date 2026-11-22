#pragma once
// v0.4 lot 3 — guided mode & plain-language diagnostics (OCCT-free, Qt-free, unit-testable).
//
//  * explainFeatureError: turns a kernel failure of a timeline step into an actionable sentence;
//  * Lesson / TutorialEngine: JSON lessons whose steps are validated by inspecting the model
//    (never by imposing one way of drawing): features present, sketch fully constrained, volume...

#include <map>
#include <set>
#include <string>
#include <vector>

#include "model/Json.h"
#include "model/Math.h"
#include "model/RenderModel.h"

namespace silex {

// ------------------------------------------------------------------ feature errors

struct ErrorContext
{
    double smallestBodyExtent = 0;  ///< mm, smallest side of the bounding box of the bodies
    double largestBodyExtent = 0;
};

/// Plain-language explanation (French when `fr`) of a failed / warned step; empty when unknown.
std::string explainFeatureError(const std::string& type, const std::string& message, const Json& params,
                                const ErrorContext& ctx, bool fr);

// ------------------------------------------------------------------ tutorials

/// What a lesson can observe of the model (filled by the application or the tests).
struct ModelFacts
{
    struct Feature
    {
        std::string type, name;
        FeatureStatus status = FeatureStatus::Ok;
        Json params;
    };
    struct Body
    {
        std::string id, name;
        double volume = 0;
    };
    std::vector<Feature> features;  ///< active steps (before the marker), timeline order
    std::vector<Body> bodies;
    std::vector<int> sketchDof;     ///< per sketch step, timeline order (-1 = unknown / failed)
    std::map<std::string, double> parameters;
    std::set<std::string> events;   ///< "interference_ok", "drawing_exported", ...
    // v0.5 lot 2 (repair challenges)
    std::vector<Vec3> holes;        ///< centers of the cylindrical faces of the printable bodies
    BBox bounds;                    ///< bounding box of the printable bodies
    int collisions = -1;            ///< colliding body pairs (-1 = not computed)
    std::map<std::string, bool> robust;  ///< "robust" criteria (key = criterion JSON) computed on a what-if model
};

struct LessonStep
{
    std::string title, goal, hintTool, hintText;
    std::vector<Json> criteria;  ///< see evaluateCriterion
};

struct Lesson
{
    std::string id, title, level;
    int order = 0;               ///< "Parcours N"
    int levelRank = 0;           ///< 0 beginner, 1 intermediate, 2 advanced (sorting / grouping)
    int minutes = 0;
    std::string intro;
    std::string example;         ///< annotated example (examples/<file>.silex) for the guided tour
    std::string category = "course";  ///< v0.5: "course" (Parcours) | "repair" (debug & modify challenge)
    std::string start;           ///< v0.5: broken part opened by a repair challenge (tutorials/<start>.silex)
    std::string baseDir;         ///< v0.5: folder of a user lesson pack (empty = built-in lesson)
    bool french = true;          ///< language the texts were read in
    std::vector<LessonStep> steps;
};

/// Texts are either plain strings or {"fr": "...", "en": "..."} objects (bilingual lessons);
/// `french` picks the language (falls back to the other one).
Lesson lessonFromJson(const Json& j, bool french = true);
std::string localizedText(const Json& v, bool french);

/// Structural check of a lesson file: known criterion types and their required keys, FR + EN texts
/// for every title / goal / hint / label, at least one step and one criterion per step.
/// Returns the problems found (empty = valid).
std::vector<std::string> validateLessonJson(const Json& j);

/// Criterion types:
///  {"type":"feature","feature":"Extrude","min":1,"op":"Cut"}   steps of a type (OK / warning)
///  {"type":"sketch_dof","value":0}                             the last sketch has this many DOF
///  {"type":"body_count","min":1,"max":2}
///  {"type":"body_volume","value":34000,"tol":0.02}             a body within the relative tolerance
///  {"type":"body_volume","min":7000,"max":8000}                 a body within a volume range
///  {"type":"parameter","name":"jeu_montage"}                   a global parameter exists
///  {"type":"feature_param","feature":"Extrude","pointer":"/distance","value":50,"tol":1e-6,"which":"first"}
///        (an array counts its elements: "pointer":"/edges","min":12; a string: "equals":"M8")
///  {"type":"sketch_constraint","kind":"Concentric","min":1}  a sketch uses this constraint
///  {"type":"no_errors"}                                        no red step in the timeline
///  {"type":"all_sketches_dof","value":0}                        every sketch has this many DOF
///  {"type":"parameter","name":"jeu_montage","min":0.1}          (also with min / max)
///  {"type":"holes_symmetric","axis":"x"}                        holes mirrored about the middle of the part
///  {"type":"hole_at","x":20,"y":10,"tol":0.5} / "no_hole_at"    a hole axis (not) at this X / Y
///  {"type":"no_collision"}                                     no colliding bodies (computed on demand)
///  {"type":"robust","parameter":"largeur","value":80,"check":[criteria...]}
///        robustness test: Silex sets the parameter on a copy of the model, recomputes it and checks

///  {"type":"event","name":"drawing_exported"}
///  {"type":"any_of","of":[criteria...]}
bool evaluateCriterion(const Json& c, const ModelFacts& f);
std::string criterionLabel(const Json& c, bool french = true);

/// Progress of a lesson: steps are validated in order; `done` latches the steps already validated
/// (an earlier step is not "unchecked" when the model evolves). Returns the index of the active
/// step (== steps.size() when the lesson is complete).
int updateLessonProgress(const Lesson& l, const ModelFacts& f, std::vector<bool>& done);

}  // namespace silex
