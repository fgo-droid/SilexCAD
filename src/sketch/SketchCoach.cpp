#include "sketch/SketchCoach.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <numeric>

#include "model/Features.h"
#include "sketch/SketchSolver.h"

namespace silex {

namespace {

std::string num(double v)
{
    char b[32];
    if (std::abs(v - std::round(v)) < 1e-6)
        std::snprintf(b, sizeof b, "%.0f", v);
    else
        std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}

/// DOF by rank, redundancy allowed (used when the whole group is frozen on purpose)
int dofByRank(const Sketch& sk)
{
    Sketch t = sk;
    const auto r = SketchSolver::solve(t);
    return r.converged ? r.dof : -1;
}

int dofOf(const Sketch& sk)
{
    Sketch t = sk;
    const auto r = SketchSolver::solve(t);
    if (!r.redundant.empty() || !r.conflicting.empty() || !r.converged)
        return -1;
    return r.dof;
}

SketchConstraint probe(ConstraintKind k, GeoRef a, GeoRef b = {})
{
    SketchConstraint c;
    c.id = newUuid();
    c.kind = k;
    c.a = a;
    c.b = b;
    return c;
}

GeoRef firstPoint(const SketchGeo& g)
{
    switch (g.kind) {
        case GeoKind::Line:
        case GeoKind::Point: return {g.id, PointPos::Start};
        default: return {g.id, PointPos::Center};
    }
}

}  // namespace

std::vector<CoachItem> explainSketch(const Sketch& sk, bool fr)
{
    std::vector<CoachItem> out;
    Sketch base = sk;
    const SolveResult res = SketchSolver::solve(base);
    if (!res.conflicting.empty() || !res.redundant.empty() || !res.converged) {
        out.push_back({{}, fr ? "Une contrainte en rouge contredit les autres : supprime-la (clic sur son badge puis Suppr) "
                                "ou change la cote qui pose problème."
                              : "A red constraint contradicts the others: delete it (click its badge, then Del) or "
                                "change the conflicting dimension.",
                       -1});
        return out;
    }
    if (res.dof <= 0) {
        out.push_back({{}, fr ? "Esquisse entièrement contrainte : rien ne peut bouger par accident. Bravo !"
                              : "Fully constrained sketch: nothing can move by accident. Well done!",
                       0});
        return out;
    }

    // connected groups of entities (constraints linking two entities, construction included)
    std::map<Uuid, Uuid> parent;
    std::function<Uuid(const Uuid&)> find = [&](const Uuid& x) -> Uuid {
        auto it = parent.find(x);
        if (it == parent.end() || it->second == x)
            return x;
        return it->second = find(it->second);
    };
    for (const auto& g : base.geos)
        parent[g.id] = g.id;
    for (const auto& c : base.constraints) {
        std::vector<Uuid> ids;
        for (const GeoRef* r : {&c.a, &c.b, &c.c})
            if (r->valid() && !isReservedGeo(r->geo) && parent.count(r->geo))
                ids.push_back(r->geo);
        for (size_t i = 1; i < ids.size(); ++i)
            parent[find(ids[i])] = find(ids[0]);
    }
    std::map<Uuid, std::vector<const SketchGeo*>> groups;
    for (const auto& g : base.geos)
        groups[find(g.id)].push_back(&g);

    for (const auto& [root, gs] : groups) {
        // freedom of the group = DOF lost when the whole group is fixed
        Sketch fixedAll = base;
        for (const SketchGeo* g : gs)
            fixedAll.constraints.push_back(probe(ConstraintKind::Fixed, {g->id}));
        const int rest = dofByRank(fixedAll);
        const int groupDof = rest < 0 ? -1 : res.dof - rest;
        if (groupDof <= 0)
            continue;
        auto it0 = std::find_if(gs.begin(), gs.end(), [](const SketchGeo* g) { return !g->construction && g->kind != GeoKind::Point; });
        const SketchGeo& g0 = it0 != gs.end() ? **it0 : *gs.front();
        auto freeWith = [&](SketchConstraint c) {
            Sketch t = base;
            c.value = t.measure(c);
            t.constraints.push_back(c);
            const int d = dofOf(t);
            return d >= 0 && d < res.dof;
        };
        const GeoRef p0 = firstPoint(g0);
        const bool tx = freeWith(probe(ConstraintKind::DistanceX, p0, {kOriginId, PointPos::Start}));
        const bool ty = freeWith(probe(ConstraintKind::DistanceY, p0, {kOriginId, PointPos::Start}));
        bool rot = false;
        for (const SketchGeo* g : gs)
            if (g->kind == GeoKind::Line && !g->construction) {
                SketchConstraint c = probe(ConstraintKind::Angle, {g->id});
                Sketch t = base;
                c.value = std::atan2(g->p2.y - g->p1.y, g->p2.x - g->p1.x);
                t.constraints.push_back(c);
                const int d = dofOf(t);
                rot = d >= 0 && d < res.dof;
                break;
            }
        const int sizeDof = groupDof - int(tx) - int(ty) - int(rot);

        // what is it?
        int lines = 0, circles = 0, arcs = 0;
        for (const SketchGeo* g : gs)
            if (!g->construction) {
                lines += g->kind == GeoKind::Line;
                circles += g->kind == GeoKind::Circle;
                arcs += g->kind == GeoKind::Arc;
            }
        int hv = 0;
        for (const auto& c : base.constraints)
            if ((c.kind == ConstraintKind::Horizontal || c.kind == ConstraintKind::Vertical) && find(c.a.geo) == root)
                ++hv;
        std::vector<double> lens, diams;
        for (const auto& c : base.constraints) {
            if (!c.a.valid() || isReservedGeo(c.a.geo) || find(c.a.geo) != root)
                continue;
            if (c.kind == ConstraintKind::Distance)
                lens.push_back(std::abs(c.value));
            if (c.kind == ConstraintKind::Diameter)
                diams.push_back(c.value);
            if (c.kind == ConstraintKind::Radius)
                diams.push_back(2 * c.value);
        }
        std::string what, whatEn;
        const bool rect = lines == 4 && circles == 0 && arcs == 0 && hv >= 3;
        if (rect) {
            what = "Ce rectangle";
            whatEn = "This rectangle";
        }
        else if (circles == 1 && lines == 0 && arcs == 0) {
            what = "Ce cercle";
            whatEn = "This circle";
        }
        else if (lines == 1 && circles == 0 && arcs == 0) {
            what = "Cette ligne";
            whatEn = "This line";
        }
        else {
            what = "Ce contour";
            whatEn = "This outline";
        }
        std::string sizes;
        if (rect && lens.size() >= 2)
            sizes = num(lens[0]) + " × " + num(lens[1]) + " mm";
        else if (!diams.empty())
            sizes = "Ø" + num(diams[0]);

        std::string text;
        const bool moves = tx || ty;
        const std::string axes = tx && ty ? "X et Y" : tx ? "X" : "Y";
        const std::string axesEn = tx && ty ? "X and Y" : tx ? "X" : "Y";
        const bool lonelyCircle = circles == 1 && lines == 0 && arcs == 0;
        if (sizeDof <= 0 && moves && !lonelyCircle) {
            if (fr)
                text = what + (sizes.empty() ? " a toutes ses dimensions" : " a ses bonnes dimensions (" + sizes + ")") +
                       ", mais il peut encore glisser en " + axes + ". " +
                       (rect ? "Attache son centre à l'origine avec la contrainte Milieu (M) — ou un coin avec "
                               "Coïncidence (I) —, ou cote sa position avec D."
                             : circles == 1 ? "Ajoute une cote D entre son centre et un bord (ou l'origine) pour chaque direction."
                                            : "Attache un de ses points à l'origine (Coïncidence I) ou cote sa position avec D.");
            else
                text = whatEn + (sizes.empty() ? " has all its sizes" : " has the right sizes (" + sizes + ")") +
                       " but can still slide in " + axesEn + ". " +
                       (rect ? "Attach its center to the origin with the Midpoint constraint (M) — or a corner with "
                               "Coincident (I) —, or dimension its position with D."
                             : circles == 1 ? "Add a D dimension between its center and an edge (or the origin) for each direction."
                                            : "Attach one of its points to the origin (Coincident I) or dimension its position with D.");
        }
        else if (circles == 1 && lines == 0 && arcs == 0 && diams.empty()) {
            text = fr ? "Ce cercle n'a pas encore de diamètre : clique-le avec l'outil Cote (D)."
                      : "This circle has no diameter yet: click it with the Dimension tool (D).";
            if (moves)
                text += fr ? " Sa position manque aussi : ajoute une cote D vers un bord ou l'origine."
                           : " Its position is missing too: add a D dimension to an edge or the origin.";
        }
        else if (circles == 1 && !diams.empty() && moves) {
            text = fr ? "Ce cercle a un diamètre fixé (" + sizes + "), mais sa distance au bord manque. Ajoute une cote D."
                      : "This circle has a fixed diameter (" + sizes + ") but its distance to the edge is missing. Add a D dimension.";
        }
        else {
            if (fr) {
                text = what + " n'est pas encore complètement défini : " + std::to_string(groupDof) + " degré(s) de liberté. ";
                if (sizeDof > 0)
                    text += "Il manque " + std::to_string(sizeDof) + " cote(s) de taille (outil D). ";
                if (rot)
                    text += "Il peut tourner : rends un côté horizontal (H) ou vertical (V). ";
                if (moves)
                    text += "Il peut glisser en " + axes + " : attache-le à l'origine (I / M). ";
            }
            else {
                text = whatEn + " is not fully defined yet: " + std::to_string(groupDof) + " degree(s) of freedom. ";
                if (sizeDof > 0)
                    text += std::to_string(sizeDof) + " size dimension(s) missing (tool D). ";
                if (rot)
                    text += "It can turn: make one side horizontal (H) or vertical (V). ";
                if (moves)
                    text += "It can slide in " + axesEn + ": attach it to the origin (I / M). ";
            }
        }
        CoachItem item;
        for (const SketchGeo* g : gs)
            item.geos.push_back(g->id);
        item.text = text;
        item.dof = groupDof;
        out.push_back(std::move(item));
    }
    if (out.empty())
        out.push_back({{}, fr ? "Il reste " + std::to_string(res.dof) + " degré(s) de liberté : fais glisser les points "
                                "bleus pour voir ce qui bouge encore."
                              : std::to_string(res.dof) + " degree(s) of freedom left: drag the blue points to see what still moves.",
                       res.dof});
    return out;
}

namespace {
Json bi(const std::string& fr, const std::string& en) { return Json{{"fr", fr}, {"en", en}}; }

std::string lessonNum(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", std::round(v * 100) / 100);
    return buf;
}

struct TypeText
{
    const char* fr;
    const char* en;
    const char* tool;  ///< toolbar entry pulsed by the Hint button
};

TypeText typeText(const std::string& t)
{
    static const std::map<std::string, TypeText> m{
        {FeatureType::Sketch, {"une esquisse", "a sketch", "Sketch"}},
        {FeatureType::Extrude, {"une extrusion", "an extrusion", "Extrude"}},
        {FeatureType::Revolve, {"une révolution", "a revolve", "Revolve"}},
        {FeatureType::Fillet, {"un congé", "a fillet", "Fillet"}},
        {FeatureType::Chamfer, {"un chanfrein", "a chamfer", "Chamfer"}},
        {FeatureType::Shell, {"une coque", "a shell", "Shell"}},
        {FeatureType::Combine, {"une combinaison", "a combine", "Combine"}},
        {FeatureType::Mirror, {"une symétrie", "a mirror", "Mirror"}},
        {FeatureType::LinearPattern, {"une répétition linéaire", "a linear pattern", "Linear Pattern"}},
        {FeatureType::CircularPattern, {"une répétition circulaire", "a circular pattern", "Circular Pattern"}},
        {FeatureType::Sweep, {"un balayage", "a sweep", "Sweep"}},
        {FeatureType::Loft, {"un lissage", "a loft", "Loft"}},
        {FeatureType::OffsetFace, {"un Pousser / Tirer", "a Press / Pull", "Press/Pull"}},
        {FeatureType::Box, {"une boîte", "a box", "Box"}},
        {FeatureType::Cylinder, {"un cylindre", "a cylinder", "Cylinder"}},
        {FeatureType::DefeatureFace, {"une suppression de faces", "a face deletion", "Delete Face"}},
        {FeatureType::SplitBody, {"une scission", "a split", "Split Body"}},
        {FeatureType::TransformBody, {"un déplacement", "a move", "Move/Copy"}},
        {FeatureType::ConstructionPlane, {"un plan de construction", "a construction plane", "Offset Plane"}},
        {FeatureType::Rib, {"une nervure", "a rib", "Rib"}},
        {FeatureType::Thread, {"un filetage", "a thread", "Thread"}},
        {FeatureType::FastenerHole, {"un perçage normalisé", "a standard hole", "Hole / Insert"}},
        {FeatureType::ImportStep, {"un import STEP", "a STEP import", ""}},
    };
    auto it = m.find(t);
    return it != m.end() ? it->second : TypeText{"une étape", "a step", ""};
}
}  // namespace

Json lessonFromProject(const DocState& doc, const ModelSnapshot& snap, const std::string& id, const std::string& titleFr,
                       const std::string& titleEn)
{
    const ModelFacts facts = collectFacts(doc, snap);
    Json steps = Json::array();
    std::map<std::string, int> counts;
    size_t sketchIndex = 0;
    for (int i = 0; i < doc.marker && i < int(doc.features.size()); ++i) {
        const FeaturePtr& n = doc.features[size_t(i)];
        if (n->suppressed)
            continue;
        TypeText tt = typeText(n->type);
        if (n->type == FeatureType::ImportStep && !n->params.value("component", std::string{}).empty())
            tt = {"un composant de la bibliothèque", "a library component", "Insert Component"};
        const int count = ++counts[n->type];
        Json crit = Json::array();
        crit.push_back({{"type", "feature"}, {"feature", n->type}, {"min", count},
                        {"label", bi(std::string("Étape « ") + n->name + " » (" + tt.fr + ")", "Step '" + n->name + "' (" + tt.en + ")")}});
        std::string goalFr = std::string("Crée ") + tt.fr + " (« " + n->name + " »)";
        std::string goalEn = std::string("Create ") + tt.en + " ('" + n->name + "')";
        const Json& p = n->params;
        auto dim = [&](const char* ptr, const char* fr, const char* en, const char* unit = " mm") {
            const Json* v = nullptr;
            try {
                const Json::json_pointer jp(ptr);
                if (p.contains(jp))
                    v = &p.at(jp);
            }
            catch (...) {
            }
            if (!v || !v->is_number())
                return;
            const double x = v->get<double>();
            crit.push_back({{"type", "feature_param"}, {"feature", n->type}, {"pointer", ptr}, {"value", x},
                            {"tol", std::max(0.01, std::abs(x) * 0.005)},
                            {"label", bi(std::string(fr) + " = " + lessonNum(x) + unit, std::string(en) + " = " + lessonNum(x) + unit)}});
            goalFr += std::string(", ") + fr + " " + lessonNum(x) + unit;
            goalEn += std::string(", ") + en + " " + lessonNum(x) + unit;
        };
        if (n->type == FeatureType::Sketch) {
            const int dof = sketchIndex < facts.sketchDof.size() ? facts.sketchDof[sketchIndex] : -1;
            ++sketchIndex;
            if (dof == 0) {
                crit.push_back({{"type", "sketch_dof"}, {"value", 0},
                                {"label", bi("Esquisse entièrement contrainte (0 DOF)", "Sketch fully constrained (0 DOF)")}});
                goalFr += ", entièrement contrainte (esquisse blanche)";
                goalEn += ", fully constrained (white sketch)";
            }
        }
        else if (n->type == FeatureType::Extrude || n->type == FeatureType::OffsetFace)
            dim("/distance", "distance", "distance");
        else if (n->type == FeatureType::Revolve)
            dim("/angle", "angle", "angle", " deg");
        else if (n->type == FeatureType::Fillet)
            dim("/value", "rayon", "radius");
        else if (n->type == FeatureType::Chamfer)
            dim("/value", "distance", "distance");
        else if (n->type == FeatureType::Shell)
            dim("/thickness", "épaisseur", "thickness");
        else if (n->type == FeatureType::Box) {
            dim("/length", "longueur", "length");
            dim("/height", "hauteur", "height");
        }
        else if (n->type == FeatureType::Cylinder) {
            dim("/diameter", "diamètre", "diameter");
            dim("/height", "hauteur", "height");
        }
        else if (n->type == FeatureType::CircularPattern)
            dim("/count", "nombre", "count", "");
        else if (n->type == FeatureType::LinearPattern)
            dim("/dir1/count", "nombre", "count", "");
        if ((n->type == FeatureType::FastenerHole || n->type == FeatureType::Thread) && p.contains("size")) {
            const std::string sz = p.value("size", std::string{});
            crit.push_back({{"type", "feature_param"}, {"feature", n->type}, {"pointer", "/size"}, {"equals", sz},
                            {"label", bi("Taille " + sz, "Size " + sz)}});
            goalFr += ", taille " + sz;
            goalEn += ", size " + sz;
        }
        goalFr += ".";
        goalEn += ".";
        Json step{{"title", bi(std::to_string(steps.size() + 1) + ". " + n->name, std::to_string(steps.size() + 1) + ". " + n->name)},
                  {"goal", bi(goalFr, goalEn)},
                  {"criteria", crit}};
        if (*tt.tool)
            step["hint"] = {{"tool", tt.tool},
                            {"text", bi(std::string("Utilise l'outil ") + tt.tool + ".", std::string("Use the ") + tt.tool + " tool.")}};
        if (!n->annotation.empty())
            step["annotation"] = bi(n->annotation, n->annotationEn.empty() ? n->annotation : n->annotationEn);
        steps.push_back(step);
    }
    if (!steps.empty()) {
        Json& last = steps.back()["criteria"];
        last.push_back({{"type", "no_errors"}, {"label", bi("Aucune étape en erreur", "No failed step")}});
        double vol = 0;
        for (const auto& b : snap.bodies)
            if (!b.locked())
                vol = std::max(vol, b.volume);
        if (vol > 0)
            last.push_back({{"type", "body_volume"}, {"value", vol}, {"tol", 0.02},
                            {"label", bi("Volume final ≈ " + lessonNum(vol) + " mm³", "Final volume ≈ " + lessonNum(vol) + " mm³")}});
    }
    const int minutes = std::max(5, int(steps.size()) * 2);
    return Json{{"id", id},
                {"category", "course"},
                {"order", 100},
                {"level", bi("Débutant", "Beginner")},
                {"level_rank", 0},
                {"minutes", minutes},
                {"title", bi(titleFr, titleEn)},
                {"intro", bi("Leçon créée par ton formateur à partir de sa pièce : suis les étapes, chacune se valide toute seule.",
                             "Lesson created by your trainer from their part: follow the steps, each one validates itself.")},
                {"example", id},
                {"steps", steps}};
}

ModelFacts collectFacts(const DocState& doc, const ModelSnapshot& snap)
{
    ModelFacts f;
    for (int i = 0; i < doc.marker && i < int(doc.features.size()); ++i) {
        const auto& n = doc.features[size_t(i)];
        if (n->suppressed)
            continue;
        ModelFacts::Feature x;
        x.type = n->type;
        x.name = n->name;
        x.params = n->params;
        if (const FeatureResult* r = snap.result(n->id))
            x.status = r->status;
        f.features.push_back(x);
        if (n->type == FeatureType::Sketch) {
            Sketch sk;
            if (const SketchView* sv = snap.sketch(n->id))
                sk = sv->sketch;
            else
                sk = n->as<SketchParams>().sketch;
            f.sketchDof.push_back(sk.geos.empty() ? -1 : dofOf(sk));
        }
    }
    for (const auto& b : snap.bodies) {
        f.bodies.push_back({b.id, b.name, b.volume});
        if (b.locked())
            continue;  // v0.5: reference components are not part of the exercise
        f.bounds.add(b.mesh.bbox);
        for (const auto& mf : b.mesh.faces)
            if (mf.sig.type == 1)  // cylinder: the centroid lies on the axis
                f.holes.push_back(mf.sig.center);
    }
    for (const auto& p : doc.parameters)
        if (p.error.empty())
            f.parameters[p.name] = p.value;
    return f;
}

}  // namespace silex
