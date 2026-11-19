#include "model/Sketch.h"

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
constexpr std::array<std::pair<ConstraintKind, const char*>, 15> kKindNames{{
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
                                     [&](const SketchConstraint& c) {
                                         return c.a.geo == id || c.b.geo == id;
                                     }),
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
    if (isDimensional(c.kind) || c.kind == ConstraintKind::Fixed)
        j["value"] = c.value;
    if (c.kind == ConstraintKind::Fixed)
        j["value2"] = c.value2;
    if (c.labelOffset.x != 0 || c.labelOffset.y != 0)
        j["label"] = c.labelOffset;
}

void from_json(const Json& j, SketchConstraint& c)
{
    c.id = j.at("id").get<std::string>();
    c.kind = constraintKindFromString(j.at("kind").get<std::string>()).value_or(ConstraintKind::Coincident);
    c.a = refFromJson(j.at("a"));
    if (j.contains("b"))
        c.b = refFromJson(j["b"]);
    c.value = j.value("value", 0.0);
    c.value2 = j.value("value2", 0.0);
    c.labelOffset = j.value("label", Vec2{});
}

void to_json(Json& j, const Sketch& s) { j = Json{{"geos", s.geos}, {"constraints", s.constraints}}; }

void from_json(const Json& j, Sketch& s)
{
    s.geos = j.value("geos", std::vector<SketchGeo>{});
    s.constraints = j.value("constraints", std::vector<SketchConstraint>{});
}

}  // namespace silex
