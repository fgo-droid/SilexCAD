#pragma once
// Persistent reference to a face or an edge of the model (Topological Naming).
// `name` is the semantic history-based name produced by the kernel (see kernel/TopoNaming.h);
// `sig` is the geometric signature used to disambiguate or re-attach the reference
// when the topology changes upstream.

#include <string>

#include "model/Json.h"
#include "model/Math.h"

namespace silex {

struct GeomSignature
{
    Vec3 center;     ///< centroid (face) or mid-point (edge)
    Vec3 dir;        ///< normal (planar face) / tangent at mid-point (edge)
    double size = 0; ///< area (face) or length (edge)
    int type = 0;    ///< GeomAbs surface / curve type

    double distance(const GeomSignature& o, double modelSize) const
    {
        const double scale = modelSize > 1e-9 ? modelSize : 1.0;
        double d = (center - o.center).length() / scale;
        d += 0.25 * (1.0 - std::abs(dir.dot(o.dir)));
        const double m = std::max(std::abs(size), std::abs(o.size));
        if (m > 1e-12)
            d += 0.25 * std::abs(size - o.size) / m;
        if (type != o.type)
            d += 0.5;
        return d;
    }
};

struct TopoRef
{
    std::string name;
    GeomSignature sig;
};

inline void to_json(Json& j, const GeomSignature& s)
{
    j = Json{{"c", s.center}, {"d", s.dir}, {"s", s.size}, {"t", s.type}};
}
inline void from_json(const Json& j, GeomSignature& s)
{
    s.center = j.value("c", Vec3{});
    s.dir = j.value("d", Vec3{});
    s.size = j.value("s", 0.0);
    s.type = j.value("t", 0);
}
inline void to_json(Json& j, const TopoRef& r) { j = Json{{"name", r.name}, {"sig", r.sig}}; }
inline void from_json(const Json& j, TopoRef& r)
{
    r.name = j.value("name", std::string{});
    r.sig = j.value("sig", GeomSignature{});
}

}  // namespace silex
