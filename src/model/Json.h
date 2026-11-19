#pragma once
#include <nlohmann/json.hpp>

#include "model/Math.h"

namespace silex {

using Json = nlohmann::json;

inline void to_json(Json& j, const Vec2& v) { j = Json::array({v.x, v.y}); }
inline void from_json(const Json& j, Vec2& v) { v = {j.at(0).get<double>(), j.at(1).get<double>()}; }
inline void to_json(Json& j, const Vec3& v) { j = Json::array({v.x, v.y, v.z}); }
inline void from_json(const Json& j, Vec3& v)
{
    v = {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()};
}
inline void to_json(Json& j, const Placement& p)
{
    j = Json{{"origin", p.origin}, {"xDir", p.xDir}, {"normal", p.normal}};
}
inline void from_json(const Json& j, Placement& p)
{
    p.origin = j.at("origin").get<Vec3>();
    p.xDir = j.at("xDir").get<Vec3>();
    p.normal = j.at("normal").get<Vec3>();
}

}  // namespace silex
