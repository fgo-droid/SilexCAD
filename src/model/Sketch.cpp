#include "model/Sketch.h"

#include <cmath>
#include <stdexcept>

#include <algorithm>
#include <array>

namespace silex {

Vec2 SketchGeo::point(PointPos pos) const
{
    switch (kind) {
        case GeoKind::Point: return p1;
        case GeoKind::Line: return pos == PointPos::End ? p2 : p1;
        case GeoKind::Circle: return c;
        case GeoKind::Arc:
            if (pos == PointPos::Start)
                return c + Vec2{std::cos(a0), std::sin(a0)} * r;
            if (pos == PointPos::End)
                return c + Vec2{std::cos(a1), std::sin(a1)} * r;
            return c;
    }
    return {};
}

bool SketchGeo::hasPoint(PointPos pos) const
{
    switch (kind) {
        case GeoKind::Point: return pos == PointPos::Start;
        case GeoKind::Line: return pos == PointPos::Start || pos == PointPos::End;
        case GeoKind::Circle: return pos == PointPos::Center;
        case GeoKind::Arc: return pos != PointPos::None;
    }
    return false;
}

namespace {
constexpr std::array<std::pair<ConstraintKind, const char*>, 19> kKindNames{{
    {ConstraintKind::Coincident, "Coincident"},
    {ConstraintKind::Horizontal, "Horizontal"},
    {ConstraintKind::Vertical, "Vertical"},
    {ConstraintKind::Parallel, "Parallel"},
    {ConstraintKind::Perpendicular, "Perpendicular"},
    {ConstraintKind::Tangent, "Tangent"},
    {ConstraintKind::Equal, "Equal"},
    {ConstraintKind::Fixed, "Fixed"},
    {ConstraintKind::PointOnObject, "PointOnObject"},
    {ConstraintKind::Distance, "Distance"},
    {ConstraintKind::DistanceX, "DistanceX"},
    {ConstraintKind::DistanceY, "DistanceY"},
    {ConstraintKind::Radius, "Radius"},
    {ConstraintKind::Diameter, "Diameter"},
    {ConstraintKind::Angle, "Angle"},
    {ConstraintKind::Midpoint, "Midpoint"},
    {ConstraintKind::Symmetric, "Symmetric"},
    {ConstraintKind::Concentric, "Concentric"},
    {ConstraintKind::Collinear, "Collinear"},
}};

const char* geoKindName(GeoKind k)
{
    switch (k) {
        case GeoKind::Line: return "Line";
        case GeoKind::Circle: return "Circle";
        case GeoKind::Arc: return "Arc";
        case GeoKind::Point: return "Point";
    }
    return "Line";
}
GeoKind geoKindFrom(const std::string& s)
{
    if (s == "Circle") return GeoKind::Circle;
    if (s == "Arc") return GeoKind::Arc;
    if (s == "Point") return GeoKind::Point;
    return GeoKind::Line;
}
}  // namespace

const char* toString(ConstraintKind k)
{
    for (auto& [kk, n] : kKindNames)
        if (kk == k)
            return n;
    return "?";
}

std::optional<ConstraintKind> constraintKindFromString(const std::string& s)
{
    for (auto& [kk, n] : kKindNames)
        if (s == n)
            return kk;
    return std::nullopt;
}

bool isDimensional(ConstraintKind k)
{
    switch (k) {
        case ConstraintKind::Distance:
        case ConstraintKind::DistanceX:
        case ConstraintKind::DistanceY:
        case ConstraintKind::Radius:
        case ConstraintKind::Diameter:
        case ConstraintKind::Angle: return true;
        default: return false;
    }
}

const SketchGeo* Sketch::find(const Uuid& id) const
{
    auto it = std::find_if(geos.begin(), geos.end(), [&](const SketchGeo& g) { return g.id == id; });
    return it == geos.end() ? nullptr : &*it;
}

SketchGeo* Sketch::find(const Uuid& id)
{
    auto it = std::find_if(geos.begin(), geos.end(), [&](const SketchGeo& g) { return g.id == id; });
    return it == geos.end() ? nullptr : &*it;
}

std::optional<SketchGeo> Sketch::geoOrReserved(const Uuid& id) const
{
    if (const SketchGeo* g = find(id))
        return *g;
    SketchGeo g;
    g.id = id;
    g.construction = true;
    if (id == kOriginId) {
        g.kind = GeoKind::Point;
        return g;
    }
    if (id == kXAxisId) {
        g.kind = GeoKind::Line;
        g.p2 = {1, 0};
        return g;
    }
    if (id == kYAxisId) {
        g.kind = GeoKind::Line;
        g.p2 = {0, 1};
        return g;
    }
    return std::nullopt;
}

std::optional<Vec2> Sketch::pointOf(const GeoRef& ref) const
{
    auto g = geoOrReserved(ref.geo);
    if (!g)
        return std::nullopt;
    return g->point(ref.pos);
}

void Sketch::removeGeo(const Uuid& id)
{
    geos.erase(std::remove_if(geos.begin(), geos.end(), [&](const SketchGeo& g) { return g.id == id; }),
               geos.end());
    constraints.erase(std::remove_if(constraints.begin(), constraints.end(),
                                     [&](const SketchConstraint& c) { return c.references(id); }),
                      constraints.end());
}

void Sketch::removeConstraint(const Uuid& id)
{
    constraints.erase(std::remove_if(constraints.begin(), constraints.end(),
                                     [&](const SketchConstraint& c) { return c.id == id; }),
                      constraints.end());
}

double Sketch::measure(const SketchConstraint& c) const
{
    auto ga = geoOrReserved(c.a.geo);
    auto gb = c.b.valid() ? geoOrReserved(c.b.geo) : std::nullopt;
    if (!ga)
        return 0;
    switch (c.kind) {
        case ConstraintKind::Distance:
            if (!gb) {
                if (ga->kind == GeoKind::Line)
                    return (ga->p2 - ga->p1).length();
                return 0;
            }
            if (c.b.pos == PointPos::None && gb->kind == GeoKind::Line) {
                const Vec2 p = ga->point(c.a.pos);
                const Vec2 d = (gb->p2 - gb->p1).normalized();
                return std::abs(d.cross(p - gb->p1));
            }
            return (gb->point(c.b.pos) - ga->point(c.a.pos)).length();
        case ConstraintKind::DistanceX:
            if (!gb)
                return ga->p2.x - ga->p1.x;
            return gb->point(c.b.pos).x - ga->point(c.a.pos).x;
        case ConstraintKind::DistanceY:
            if (!gb)
                return ga->p2.y - ga->p1.y;
            return gb->point(c.b.pos).y - ga->point(c.a.pos).y;
        case ConstraintKind::Radius: return ga->r;
        case ConstraintKind::Diameter: return 2 * ga->r;
        case ConstraintKind::Angle:
            if (gb && ga->kind == GeoKind::Line && gb->kind == GeoKind::Line) {
                const Vec2 d1 = ga->p2 - ga->p1, d2 = gb->p2 - gb->p1;
                return std::atan2(d1.cross(d2), d1.dot(d2));
            }
            return 0;
        default: return 0;
    }
}

// ---------------------------------------------------------------- JSON

void to_json(Json& j, const SketchGeo& g)
{
    j = Json{{"id", g.id}, {"kind", geoKindName(g.kind)}};
    if (g.construction)
        j["construction"] = true;
    if (g.external())
        j["ref"] = g.ext;
    switch (g.kind) {
        case GeoKind::Point: j["p"] = g.p1; break;
        case GeoKind::Line:
            j["p1"] = g.p1;
            j["p2"] = g.p2;
            break;
        case GeoKind::Circle:
            j["c"] = g.c;
            j["r"] = g.r;
            break;
        case GeoKind::Arc:
            j["c"] = g.c;
            j["r"] = g.r;
            j["a0"] = g.a0;
            j["a1"] = g.a1;
            break;
    }
}

void from_json(const Json& j, SketchGeo& g)
{
    g.id = j.at("id").get<std::string>();
    g.kind = geoKindFrom(j.at("kind").get<std::string>());
    g.construction = j.value("construction", false);
    g.ext = j.value("ref", TopoRef{});
    if (g.external())
        g.construction = true;
    switch (g.kind) {
        case GeoKind::Point: g.p1 = j.at("p").get<Vec2>(); break;
        case GeoKind::Line:
            g.p1 = j.at("p1").get<Vec2>();
            g.p2 = j.at("p2").get<Vec2>();
            break;
        case GeoKind::Circle:
            g.c = j.at("c").get<Vec2>();
            g.r = j.at("r").get<double>();
            break;
        case GeoKind::Arc:
            g.c = j.at("c").get<Vec2>();
            g.r = j.at("r").get<double>();
            g.a0 = j.at("a0").get<double>();
            g.a1 = j.at("a1").get<double>();
            break;
    }
}

static Json refToJson(const GeoRef& r) { return Json::array({r.geo, static_cast<int>(r.pos)}); }
static GeoRef refFromJson(const Json& j)
{
    if (!j.is_array() || j.size() != 2)
        return {};
    return {j[0].get<std::string>(), static_cast<PointPos>(j[1].get<int>())};
}

void to_json(Json& j, const SketchConstraint& c)
{
    j = Json{{"id", c.id}, {"kind", toString(c.kind)}, {"a", refToJson(c.a)}};
    if (c.b.valid())
        j["b"] = refToJson(c.b);
    if (c.c.valid())
        j["c"] = refToJson(c.c);
    if (isDimensional(c.kind) || c.kind == ConstraintKind::Fixed || (c.kind == ConstraintKind::Symmetric && c.value != 0))
        j["value"] = c.value;
    if (c.kind == ConstraintKind::Fixed)
        j["value2"] = c.value2;
    if (c.labelOffset.x != 0 || c.labelOffset.y != 0)
        j["label"] = c.labelOffset;
    if (!c.expr.empty())
        j["expr"] = c.expr;
    if (!c.group.empty())
        j["group"] = c.group;
}

void from_json(const Json& j, SketchConstraint& c)
{
    c.id = j.at("id").get<std::string>();
    c.kind = constraintKindFromString(j.at("kind").get<std::string>()).value_or(ConstraintKind::Coincident);
    c.a = refFromJson(j.at("a"));
    if (j.contains("b"))
        c.b = refFromJson(j["b"]);
    if (j.contains("c"))
        c.c = refFromJson(j["c"]);
    c.value = j.value("value", 0.0);
    c.value2 = j.value("value2", 0.0);
    c.labelOffset = j.value("label", Vec2{});
    c.expr = j.value("expr", std::string{});
    c.group = j.value("group", std::string{});
}

void to_json(Json& j, const Sketch& s) { j = Json{{"geos", s.geos}, {"constraints", s.constraints}}; }

void from_json(const Json& j, Sketch& s)
{
    s.geos = j.value("geos", std::vector<SketchGeo>{});
    s.constraints = j.value("constraints", std::vector<SketchConstraint>{});
}

// ------------------------------------------------------------------ v0.7 lot C: calibrated image

SketchCanvas makeCanvas(int pxWidth, int pxHeight, double widthMm, Vec2 center)
{
    SketchCanvas c;
    c.pxWidth = pxWidth;
    c.pxHeight = pxHeight;
    c.widthMm = widthMm;
    c.heightMm = pxWidth > 0 ? widthMm * double(pxHeight) / double(pxWidth) : widthMm;
    c.center = center;
    return c;
}

Vec2 canvasPixelToSketch(const SketchCanvas& c, Vec2 px)
{
    if (c.pxWidth <= 0 || c.pxHeight <= 0)
        throw std::invalid_argument("The image has no pixel size");
    double u = (px.x / c.pxWidth - 0.5) * c.widthMm;
    double v = (0.5 - px.y / c.pxHeight) * c.heightMm;
    if (c.flipH)
        u = -u;
    if (c.flipV)
        v = -v;
    const double a = c.angleDeg * kPi / 180.0;
    return {c.center.x + u * std::cos(a) - v * std::sin(a), c.center.y + u * std::sin(a) + v * std::cos(a)};
}

Vec2 canvasSketchToPixel(const SketchCanvas& c, Vec2 p)
{
    if (c.pxWidth <= 0 || c.pxHeight <= 0 || c.widthMm == 0 || c.heightMm == 0)
        throw std::invalid_argument("The image has no size");
    const double a = c.angleDeg * kPi / 180.0;
    const Vec2 d{p.x - c.center.x, p.y - c.center.y};
    double u = d.x * std::cos(a) + d.y * std::sin(a);
    double v = -d.x * std::sin(a) + d.y * std::cos(a);
    if (c.flipH)
        u = -u;
    if (c.flipV)
        v = -v;
    return {(u / c.widthMm + 0.5) * c.pxWidth, (0.5 - v / c.heightMm) * c.pxHeight};
}

bool calibrateCanvas(SketchCanvas& c, Vec2 px1, Vec2 px2, double distanceMm, std::string* error)
{
    auto fail = [&](const char* m) {
        if (error)
            *error = m;
        return false;
    };
    if (c.pxWidth <= 0 || c.pxHeight <= 0)
        return fail("The image has no pixel size");
    if (!(distanceMm > 0))
        return fail("The real distance must be positive");
    const double pixels = std::hypot(px2.x - px1.x, px2.y - px1.y);
    if (pixels < 1e-9)
        return fail("Pick two different points on the image");
    const Vec2 keep = canvasPixelToSketch(c, px1);
    const double mmPerPixel = distanceMm / pixels;
    c.widthMm = c.pxWidth * mmPerPixel;
    c.heightMm = c.pxHeight * mmPerPixel;
    // the first picked point stays where it was: the photo does not jump away
    const Vec2 moved = canvasPixelToSketch(c, px1);
    c.center = {c.center.x + keep.x - moved.x, c.center.y + keep.y - moved.y};
    return true;
}

void to_json(Json& j, const SketchCanvas& c)
{
    j = Json{{"image_path", c.imagePath}, {"mime", c.mime},          {"image_px", {c.pxWidth, c.pxHeight}},
             {"opacity", c.opacity},      {"width_mm", c.widthMm},   {"height_mm", c.heightMm},
             {"center_xy", {c.center.x, c.center.y}}, {"angle_deg", c.angleDeg}, {"flip_h", c.flipH},
             {"flip_v", c.flipV},         {"visible", c.visible},    {"image_data", c.imageData}};
}

void from_json(const Json& j, SketchCanvas& c)
{
    c.imagePath = j.value("image_path", std::string{});
    c.imageData = j.value("image_data", std::string{});
    c.mime = j.value("mime", std::string("image/png"));
    if (const Json px = j.value("image_px", Json::array()); px.is_array() && px.size() == 2) {
        c.pxWidth = px[0].get<int>();
        c.pxHeight = px[1].get<int>();
    }
    c.opacity = j.value("opacity", 0.5);
    c.widthMm = j.value("width_mm", 100.0);
    c.heightMm = j.value("height_mm", 100.0);
    if (const Json ce = j.value("center_xy", Json::array()); ce.is_array() && ce.size() == 2)
        c.center = {ce[0].get<double>(), ce[1].get<double>()};
    c.angleDeg = j.value("angle_deg", 0.0);
    c.flipH = j.value("flip_h", false);
    c.flipV = j.value("flip_v", false);
    c.visible = j.value("visible", true);
}
}  // namespace silex
