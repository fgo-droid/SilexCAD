#include "EnclosureWizard.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

#include "kernel/KernelEngine.h"
#include "model/Bindings.h"
#include "model/Expression.h"
#include "model/Features.h"

namespace silex::ui {

namespace {

struct Builder
{
    DocState d;
    std::vector<CommandPtr> cmds;
    int count = 0;

    bool taken(const std::string& name) const
    {
        return std::any_of(d.features.begin(), d.features.end(), [&](const FeaturePtr& f) { return f->name == name; });
    }
    std::string unique(const std::string& name) const
    {
        std::string n = name;
        for (int k = 2; taken(n); ++k)
            n = name + "_" + std::to_string(k);
        return n;
    }
    void run(CommandPtr c)
    {
        d = c->apply(d);
        cmds.push_back(std::move(c));
    }
    /// Adds a feature (expressions bound by JSON pointer) with its FR / EN annotation.
    FeaturePtr add(const std::string& type, const std::string& name, Json params,
                   const std::vector<std::pair<std::string, std::string>>& exprs, const std::string& fr,
                   const std::string& en)
    {
        for (const auto& [ptr, e] : exprs)
            setFieldExpression(params, ptr, e);
        auto n = std::make_shared<FeatureNode>();
        n->id = newUuid();
        n->type = type;
        n->name = unique(name);
        n->params = std::move(params);
        n->annotation = fr;
        n->annotationEn = en;
        run(std::make_shared<AddFeatureCommand>(n));
        ++count;
        return n;
    }
    std::shared_ptr<const ModelSnapshot> snapshot() const { return KernelEngine::computeSnapshot(d); }
    /// The step failed when it was computed (message), empty if OK.
    std::string failure(const Uuid& id) const
    {
        auto s = snapshot();
        for (const auto& r : s->results)
            if (r.id == id && r.status == FeatureStatus::Error)
                return r.message.empty() ? std::string("error") : r.message;
        return {};
    }
};

const BodyView* bodyOf(const ModelSnapshot& s, const Uuid& id)
{
    for (const auto& b : s.bodies)
        if (b.id == id)
            return &b;
    return nullptr;
}

/// Straight vertical edges of a body, optionally filtered (length, position).
std::vector<TopoRef> verticalEdges(const ModelSnapshot& s, const Uuid& body, const std::function<bool(const MeshEdge&)>& keep)
{
    std::vector<TopoRef> out;
    if (const BodyView* b = bodyOf(s, body))
        for (const auto& e : b->mesh.edges)
            if (e.sig.type == 0 && std::abs(e.sig.dir.normalized().z) > 0.999 && keep(e))
                out.push_back(TopoRef{e.name, e.sig});
    return out;
}

PrimitiveParams prim(double cx, double cy, double z, double l, double w, double h, BooleanOp op, const Uuid& target)
{
    PrimitiveParams p;
    p.plane.kind = "XY";
    p.plane.offset = z;
    p.centerX = cx;
    p.centerY = cy;
    p.length = l;
    p.width = w;
    p.diameter = l;
    p.height = h;
    p.op = op;
    p.targetBody = target;
    return p;
}

}  // namespace

std::optional<EnclosureResult> buildEnclosure(const DocState& doc, const EnclosureSpec& sp, std::string* error)
{
    auto fail = [&](const std::string& e) -> std::optional<EnclosureResult> {
        if (error)
            *error = e;
        return std::nullopt;
    };
    const double L = sp.length, W = sp.width, H = sp.height, t = sp.wall, r = std::max(0.0, sp.cornerRadius),
                 c = std::max(0.0, sp.clearance);
    if (L < 10 || W < 10 || H < 5)
        return fail("The inner size must be at least 10 x 10 x 5 mm");
    if (t < 0.8 || t > std::min(L, W) / 4)
        return fail("The wall thickness must be between 0.8 mm and a quarter of the inner width");
    if (sp.closure != "lip" && sp.closure != "screws" && sp.closure != "slide")
        return fail("Unknown closure '" + sp.closure + "' (lip, screws, slide)");
    if (sp.closure == "screws" && (L < 20 || W < 20))
        return fail("Screw posts need an inner size of at least 20 x 20 mm");

    Builder b{doc, {}, 0};
    b.d.marker = int(b.d.features.size());  // append at the end of the timeline

    // ---- parameters (Ctrl+P): the whole enclosure follows them
    std::vector<Parameter> ps = b.d.parameters;
    auto param = [&](const std::string& base, double v, const std::string& comment) {
        std::string n = base;
        for (int k = 2; std::any_of(ps.begin(), ps.end(), [&](const Parameter& p) { return p.name == n; }); ++k)
            n = base + "_" + std::to_string(k);
        Parameter p;
        p.name = n;
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", v);
        p.expression = buf;
        p.unit = "mm";
        p.comment = comment;
        ps.push_back(p);
        return n;
    };
    const std::string pL = param("int_longueur", L, "Longueur intérieure du boîtier");
    const std::string pW = param("int_largeur", W, "Largeur intérieure du boîtier");
    const std::string pH = param("int_hauteur", H, "Hauteur intérieure du boîtier");
    const std::string pT = param("paroi", t, "Épaisseur des parois");
    const std::string pR = param("rayon_coins", r, "Rayon extérieur des coins");
    evaluateParameters(ps);
    b.run(std::make_shared<SetParametersCommand>(ps));

    const bool closed = sp.closure != "slide";
    // ---- 1. solid blank
    const FeaturePtr box = b.add(
        FeatureType::Box, "Boitier_brut", Json(prim(0, 0, 0, L + 2 * t, W + 2 * t, H + 2 * t, BooleanOp::NewBody, {})),
        {{"/length", pL + " + 2 * " + pT}, {"/width", pW + " + 2 * " + pT}, {"/height", pH + " + 2 * " + pT}},
        "Le bloc plein du boîtier : dimensions intérieures + 2 parois (paramètres Ctrl+P).",
        "The solid blank: inner size + 2 walls (parameters, Ctrl+P).");
    const Uuid base = box->id;
    if (auto e = b.failure(base); !e.empty())
        return fail("Box: " + e);

    // ---- 2. rounded corners
    if (r > 0.05) {
        auto edges = verticalEdges(*b.snapshot(), base, [](const MeshEdge&) { return true; });
        if (edges.size() == 4) {
            EdgeBlendParams fp;
            fp.edges = edges;
            fp.value = r;
            const FeaturePtr f = b.add(FeatureType::Fillet, "Coins_arrondis", Json(fp), {{"/value", pR}},
                                       "Les 4 arêtes verticales arrondies : plus agréable en main, et les coins s'impriment mieux.",
                                       "The 4 vertical edges rounded: nicer to hold, and the corners print better.");
            if (auto e = b.failure(f->id); !e.empty())
                return fail("Fillet: " + e);
        }
    }

    // ---- 3. shell
    ShellParams shp;
    shp.body = base;
    shp.thickness = t;
    if (!closed) {
        auto s = b.snapshot();
        if (const BodyView* bv = bodyOf(*s, base))
            for (const auto& f : bv->mesh.faces)
                if (f.planar && f.plane.normal.normalized().z > 0.999 && f.sig.center.z > H + 2 * t - 1e-3)
                    shp.removedFaces.push_back(TopoRef{f.name, f.sig});
    }
    const FeaturePtr shell = b.add(
        FeatureType::Shell, "Coque", Json(shp), {{"/thickness", pT}},
        closed ? "Coque fermée (aucune face retirée) : le bloc devient creux, avec une paroi constante partout."
               : "Coque ouverte sur le dessus : le couvercle coulissant fermera l'ouverture.",
        closed ? "Closed shell (no face removed): the blank becomes hollow, with a constant wall everywhere."
               : "Shell open at the top: the sliding lid will close the opening.");
    if (auto e = b.failure(shell->id); !e.empty())
        return fail("Shell: " + e);

    EnclosureResult res;
    res.baseBody = base;
    const double top = H + 2 * t;
    if (closed) {
        // ---- 4. split: box (below) + lid (the top wall)
        SplitBodyParams spl;
        spl.body = base;
        spl.plane.kind = "XY";
        spl.plane.offset = H + t;
        const FeaturePtr split = b.add(FeatureType::SplitBody, "Separation_couvercle", Json(spl), {{"/plane/offset", pH + " + " + pT}},
                                       "Le plan de coupe au ras de la cavité sépare la boîte (corps _1) et le couvercle (corps _2).",
                                       "The cutting plane flush with the cavity separates the box (body _1) and the lid (body _2).");
        if (auto e = b.failure(split->id); !e.empty())
            return fail("Split: " + e);
        res.lidBody = split->id + "#2";
    }

    const Uuid lid = res.lidBody;
    if (sp.closure == "lip") {
        // ---- 5. snap-fit lip under the lid: ring 1.2 mm thick, 3 mm deep, with the clearance all around
        const double lipT = 1.2, lipH = 3;
        const FeaturePtr lip = b.add(
            FeatureType::Box, "Levre", Json(prim(0, 0, H + t, L - 2 * c, W - 2 * c, -lipH, BooleanOp::Join, lid)),
            {{"/length", pL + " - " + std::to_string(2 * c)}, {"/width", pW + " - " + std::to_string(2 * c)}, {"/plane/offset", pH + " + " + pT}},
            "La lèvre sous le couvercle, plus petite que la cavité du jeu de montage : elle centre le couvercle et le tient par friction.",
            "The lip under the lid, smaller than the cavity by the fit clearance: it centers the lid and holds it by friction.");
        if (auto e = b.failure(lip->id); !e.empty())
            return fail("Lip: " + e);
        const double inner = std::max(0.0, r - t);  // corner radius of the cavity
        if (inner - c > 0.1) {
            const double x = (L - 2 * c) / 2;
            auto edges = verticalEdges(*b.snapshot(), lid, [&](const MeshEdge& e) {
                return std::abs(e.sig.size - lipH) < 0.05 && std::abs(std::abs(e.sig.center.x) - x) < 0.05;
            });
            if (edges.size() == 4) {
                EdgeBlendParams fp;
                fp.edges = edges;
                fp.value = inner - c;
                b.add(FeatureType::Fillet, "Levre_coins", Json(fp), {},
                      "Les coins de la lèvre suivent l'arrondi intérieur de la boîte.",
                      "The lip corners follow the inner rounding of the box.");
            }
        }
        b.add(FeatureType::Box, "Levre_evidement",
              Json(prim(0, 0, H + t, L - 2 * c - 2 * lipT, W - 2 * c - 2 * lipT, -(lipH + 0.5), BooleanOp::Cut, lid)),
              {{"/length", pL + " - " + std::to_string(2 * c + 2 * lipT)},
               {"/width", pW + " - " + std::to_string(2 * c + 2 * lipT)},
               {"/plane/offset", pH + " + " + pT}},
              "On évide la lèvre : il ne reste qu'un anneau de 1,2 mm, souple et léger.",
              "The lip is hollowed: only a 1.2 mm ring remains, flexible and light.");
    }
    else if (sp.closure == "screws") {
        // ---- 5. four M3 posts in the corners (heat-set inserts), countersunk holes in the lid
        const double d = 7, off = 3.0;  // posts overlap the walls by 0.5 mm: a robust boolean
        const double px = L / 2 - off, py = W / 2 - off;
        std::vector<Vec2> centers{{-px, -py}, {px, -py}, {px, py}, {-px, py}};
        const char* sx[] = {"-", "", "", "-"};
        const char* sy[] = {"-", "-", "", ""};
        for (int k = 0; k < 4; ++k) {
            const auto& cc = centers[size_t(k)];
            PrimitiveParams cp = prim(cc.x, cc.y, t, d, d, H, BooleanOp::Join, base);
            cp.diameter = d;
            b.add(FeatureType::Cylinder, "Colonnette_" + std::to_string(k + 1), Json(cp),
                  {{"/centerX", std::string(sx[k]) + "(" + pL + " / 2 - 3)"},
                   {"/centerY", std::string(sy[k]) + "(" + pW + " / 2 - 3)"},
                   {"/plane/offset", pT},
                   {"/height", pH}},
                  k == 0 ? "Colonnette Ø7 dans chaque coin, du fond jusqu'au plan de coupe : elle recevra un insert M3."
                         : "",
                  k == 0 ? "A Ø7 post in each corner, from the floor up to the cutting plane: it will take an M3 insert."
                         : "");
        }
        FastenerHoleParams ins;
        ins.plane.kind = "XY";
        ins.plane.offset = H + t;
        ins.centers = centers;
        ins.kind = "insert";
        ins.size = "M3";
        ins.targetBody = base;
        std::vector<std::pair<std::string, std::string>> cx;
        for (int k = 0; k < 4; ++k) {
            cx.push_back({"/centers/" + std::to_string(k) + "/0", std::string(sx[k]) + "(" + pL + " / 2 - 3)"});
            cx.push_back({"/centers/" + std::to_string(k) + "/1", std::string(sy[k]) + "(" + pW + " / 2 - 3)"});
        }
        auto withOffset = cx;
        withOffset.push_back({"/plane/offset", pH + " + " + pT});
        b.add(FeatureType::FastenerHole, "Inserts_M3", Json(ins), withOffset,
              "Logements d'inserts laiton M3 (Ø4 × 5,7) en tête des colonnettes : on les pose au fer à souder.",
              "M3 brass insert pockets (Ø4 × 5.7) on top of the posts: set them with a soldering iron.");
        FastenerHoleParams sc = ins;
        sc.plane.offset = top;
        sc.kind = t >= 1.6 ? "countersink" : "clearance";
        sc.targetBody = lid;
        auto lidExpr = cx;
        lidExpr.push_back({"/plane/offset", pH + " + 2 * " + pT});
        b.add(FeatureType::FastenerHole, "Vis_couvercle", Json(sc), lidExpr,
              "Les 4 trous de vis du couvercle, alignés sur les inserts (mêmes expressions de position).",
              "The 4 screw holes of the lid, aligned with the inserts (same position expressions).");
    }
    else {
        // ---- sliding lid: grooves in the two long walls, the +X end wall opened above the groove
        const double lidT = std::max(1.2, std::min(2.0, t)), g = lidT + 0.4, keep = 0.8;
        const double z0 = top - keep - g;
        b.add(FeatureType::Box, "Rainures",
              Json(prim((t + 1) / 2, 0, z0, L + t + 1, W + t, g, BooleanOp::Cut, base)),
              {{"/length", pL + " + " + pT + " + 1"}, {"/width", pW + " + " + pT}},
              "Deux rainures (à mi-épaisseur des parois) guident le couvercle ; une bande de 0,8 mm le retient au-dessus.",
              "Two grooves (half-way into the walls) guide the lid; a 0.8 mm band holds it from above.");
        b.add(FeatureType::Box, "Entree_couvercle",
              Json(prim(L / 2 + (t + 1) / 2, 0, z0, t + 1.2, W + t, top - z0 + 1, BooleanOp::Cut, base)),
              {{"/width", pW + " + " + pT}},
              "Le bout de la boîte est ouvert au-dessus de la rainure : le couvercle entre par là.",
              "The end of the box is opened above the groove: the lid slides in there.");
        PrimitiveParams lp = prim((t + c) / 2, 0, z0 + 0.2, L + t - c, W + t - 2 * c, lidT, BooleanOp::NewBody, {});
        const FeaturePtr lidF = b.add(FeatureType::Box, "Couvercle_coulissant", Json(lp),
                                      {{"/length", pL + " + " + pT + " - " + std::to_string(c)},
                                       {"/width", pW + " + " + pT + " - " + std::to_string(2 * c)}},
                                      "Le couvercle coulissant, en place : plus étroit que les rainures du jeu de montage.",
                                      "The sliding lid, in place: narrower than the grooves by the fit clearance.");
        res.lidBody = lidF->id;
    }

    // final check: every step computes
    auto s = b.snapshot();
    for (const auto& rr : s->results)
        if (rr.status == FeatureStatus::Error)
            if (const auto f = b.d.find(rr.id))
                return fail(f->name + ": " + rr.message);
    res.featureCount = b.count;
    res.command = std::make_shared<BatchCommand>("Enclosure wizard", b.cmds);
    return res;
}

std::optional<EnclosureSpec> askEnclosureSpec(QWidget* parent, bool french)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QObject::tr("Enclosure Wizard"));
    auto* lay = new QVBoxLayout(&dlg);
    auto* intro = new QLabel(french ? QString::fromUtf8("Un boîtier en 3 clics. L'assistant écrit une timeline commentée "
                                                        "(Box → Fillet → Shell → Split → fermeture) que tu peux modifier "
                                                        "étape par étape, et des paramètres (Ctrl+P).")
                                    : QObject::tr("An enclosure in 3 clicks. The wizard writes a commented timeline (Box → Fillet "
                                                  "→ Shell → Split → closure) you can edit step by step, and parameters (Ctrl+P)."),
                             &dlg);
    intro->setWordWrap(true);
    lay->addWidget(intro);
    auto* form = new QFormLayout();
    QSettings st;
    auto spin = [&](const char* key, double def, double lo, double hi) {
        auto* b = new QDoubleSpinBox(&dlg);
        b->setRange(lo, hi);
        b->setDecimals(1);
        b->setSuffix(" mm");
        b->setValue(st.value(QString("enclosure/") + key, def).toDouble());
        return b;
    };
    auto* L = spin("length", 80, 10, 1000);
    auto* W = spin("width", 50, 10, 1000);
    auto* H = spin("height", 30, 5, 1000);
    auto* T = spin("wall", 2.0, 0.8, 10);
    auto* R = spin("radius", 3.0, 0, 50);
    form->addRow(QObject::tr("Inner length"), L);
    form->addRow(QObject::tr("Inner width"), W);
    form->addRow(QObject::tr("Inner height"), H);
    form->addRow(QObject::tr("Wall thickness"), T);
    form->addRow(QObject::tr("Corner radius"), R);
    auto* closure = new QComboBox(&dlg);
    closure->addItem(QObject::tr("Snap-fit lip"), "lip");
    closure->addItem(QObject::tr("4 M3 screw posts (heat-set inserts)"), "screws");
    closure->addItem(QObject::tr("Sliding lid"), "slide");
    closure->setCurrentIndex(std::max(0, closure->findData(st.value("enclosure/closure", "lip"))));
    form->addRow(QObject::tr("Closure"), closure);
    lay->addLayout(form);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    bb->button(QDialogButtonBox::Ok)->setText(QObject::tr("Create"));
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted)
        return std::nullopt;
    EnclosureSpec s;
    s.length = L->value();
    s.width = W->value();
    s.height = H->value();
    s.wall = T->value();
    s.cornerRadius = R->value();
    s.closure = closure->currentData().toString().toStdString();
    st.setValue("enclosure/length", s.length);
    st.setValue("enclosure/width", s.width);
    st.setValue("enclosure/height", s.height);
    st.setValue("enclosure/wall", s.wall);
    st.setValue("enclosure/radius", s.cornerRadius);
    st.setValue("enclosure/closure", QString::fromStdString(s.closure));
    return s;
}

}  // namespace silex::ui
