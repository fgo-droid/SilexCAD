#pragma once
// v0.4 lot 3 — SketchCoach: "why is my sketch still blue?" in plain language.
//
// The sketch is split into connected groups of entities; for each group the solver is probed with
// temporary constraints (X / Y position, angle, whole group fixed) to find out exactly what is
// still free: sliding in X / Y, turning, or missing sizes. Then one sentence per group tells the
// student which tool to use (M, I, D, H / V) without doing it for them.

#include <string>
#include <vector>

#include "model/Document.h"
#include "model/Pedagogy.h"
#include "model/Sketch.h"

namespace silex {

struct CoachItem
{
    std::vector<Uuid> geos;  ///< entities of the group (to highlight them)
    std::string text;
    int dof = 0;
};

std::vector<CoachItem> explainSketch(const Sketch& sk, bool fr);

/// Facts for the tutorial engine: features (with status), bodies, DOF of every sketch, parameters.
ModelFacts collectFacts(const DocState& doc, const ModelSnapshot& snap);

/// v0.5 lot 5 — Lesson recorder: a lesson JSON (bilingual, valid for validateLessonJson) built from the
/// timeline of a finished part. One step per active feature with automatic criteria: count of steps of
/// that type, sketch fully constrained, key dimension (distance, radius, thickness, size...), and on the
/// last step the body count and final volume (±2 %). Texts are drafts the trainer then edits.
Json lessonFromProject(const DocState& doc, const ModelSnapshot& snap, const std::string& id, const std::string& titleFr,
                       const std::string& titleEn);

}  // namespace silex
