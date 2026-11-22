// v0.3 Lot 4.2 — native 3MF export (3MF Core Specification 1.x).
//
// One <object> per body with its name and its material colour (core <basematerials>, alpha kept
// for translucent bodies), an explicit unit="millimeter", project metadata, and watertight
// indexed meshes (vertices shared between adjacent faces), packed in an OPC zip archive.

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include "kernel/KernelEngine.h"
#include "kernel/Recompute.h"
#include "kernel/Zip.h"

namespace silex {

using namespace kernel;

namespace {

std::string xmlEscape(const std::string& s)
{
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&apos;"; break;
            default: o.push_back(c);
        }
    }
    return o;
}

std::string hexColor(const BodyMaterial& m)
{
    char buf[16];
    auto c = [](float v) { return int(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", c(m.r), c(m.g), c(m.b), c(m.a));
    return buf;
}

std::string num(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", std::abs(v) < 5e-7 ? 0.0 : v);
    return buf;
}

struct Mesh
{
    std::vector<gp_Pnt> vertices;
    std::vector<std::array<int, 3>> triangles;
};

struct KeyHash
{
    size_t operator()(const std::array<long long, 3>& k) const
    {
        size_t h = std::hash<long long>{}(k[0]);
        h ^= std::hash<long long>{}(k[1]) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h ^= std::hash<long long>{}(k[2]) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h;
    }
};

Mesh meshOf(const TopoDS_Shape& shape, double deflection)
{
    BRepMesh_IncrementalMesh mesher(shape, std::max(deflection, 1e-4), Standard_False, 0.3, Standard_True);
    Mesh m;
    std::unordered_map<std::array<long long, 3>, int, KeyHash> index;
    auto key = [](const gp_Pnt& p) {
        return std::array<long long, 3>{std::llround(p.X() * 1e5), std::llround(p.Y() * 1e5), std::llround(p.Z() * 1e5)};
    };
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face face = TopoDS::Face(ex.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull())
            continue;
        const gp_Trsf trsf = loc.Transformation();
        std::vector<int> map(size_t(tri->NbNodes()) + 1, -1);
        for (int i = 1; i <= tri->NbNodes(); ++i) {
            const gp_Pnt p = tri->Node(i).Transformed(trsf);
            auto [it, inserted] = index.try_emplace(key(p), int(m.vertices.size()));
            if (inserted)
                m.vertices.push_back(p);
            map[size_t(i)] = it->second;
        }
        const bool reversed = face.Orientation() == TopAbs_REVERSED;
        for (int i = 1; i <= tri->NbTriangles(); ++i) {
            int a, b, c;
            tri->Triangle(i).Get(a, b, c);
            if (reversed)
                std::swap(b, c);
            const int va = map[size_t(a)], vb = map[size_t(b)], vc = map[size_t(c)];
            if (va == vb || vb == vc || va == vc)
                continue;  // degenerated after vertex welding
            m.triangles.push_back({va, vb, vc});
        }
    }
    return m;
}

std::string today()
{
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

}  // namespace

ExportSummary KernelEngine::export3mf(const DocState& state, const std::string& path, const ThreeMfOptions& o)
{
    ExportSummary sum;
    try {
        Recompute r;
        ComputeOutput out = r.run(state, {}, {});
        std::vector<Uuid> ids;
        if (o.bodies.empty()) {
            for (const Uuid& id : out.state.bodyOrder)
                if (!out.state.lockedBodies.count(id))  // reference components are not printed
                    ids.push_back(id);
        }
        else
            for (const Uuid& id : o.bodies) {
                if (!out.state.bodies.count(id))
                    throw std::runtime_error("Unknown body id: " + id);
                ids.push_back(id);
            }
        if (ids.empty())
            throw std::runtime_error("There is no body to export");

        std::ostringstream x;
        x << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          << "<model unit=\"millimeter\" xml:lang=\"fr-FR\" "
             "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">\n"
          << " <metadata name=\"Title\">" << xmlEscape(o.title) << "</metadata>\n";
        if (!o.designer.empty())
            x << " <metadata name=\"Designer\">" << xmlEscape(o.designer) << "</metadata>\n";
        x << " <metadata name=\"Application\">Silex v0.3</metadata>\n"
          << " <metadata name=\"Description\">Generated by Silex v0.3</metadata>\n"
          << " <metadata name=\"CreationDate\">" << today() << "</metadata>\n"
          << " <resources>\n  <basematerials id=\"1\">\n";
        for (const Uuid& id : ids) {
            const BodyMaterial m = state.materialOf(id);
            x << "   <base name=\"" << xmlEscape(m.name) << "\" displaycolor=\"" << hexColor(m) << "\"/>\n";
        }
        x << "  </basematerials>\n";

        int objectId = 2;
        for (size_t k = 0; k < ids.size(); ++k) {
            const Uuid& id = ids[k];
            const auto nm = out.state.bodyNames.find(id);
            const std::string name = nm != out.state.bodyNames.end() ? nm->second : "Body";
            const Mesh mesh = meshOf(out.state.bodies.at(id).shape(), o.linearDeflection);
            if (mesh.triangles.empty())
                throw std::runtime_error("Body '" + name + "' produced an empty mesh");
            x << "  <object id=\"" << objectId++ << "\" type=\"model\" name=\"" << xmlEscape(name)
              << "\" pid=\"1\" pindex=\"" << k << "\">\n   <mesh>\n    <vertices>\n";
            for (const gp_Pnt& p : mesh.vertices)
                x << "     <vertex x=\"" << num(p.X()) << "\" y=\"" << num(p.Y()) << "\" z=\"" << num(p.Z()) << "\"/>\n";
            x << "    </vertices>\n    <triangles>\n";
            for (const auto& t : mesh.triangles)
                x << "     <triangle v1=\"" << t[0] << "\" v2=\"" << t[1] << "\" v3=\"" << t[2] << "\"/>\n";
            x << "    </triangles>\n   </mesh>\n  </object>\n";
            sum.triangles += mesh.triangles.size();
            ++sum.objects;
        }
        x << " </resources>\n <build>\n";
        for (int id = 2; id < objectId; ++id)
            x << "  <item objectid=\"" << id << "\"/>\n";
        x << " </build>\n</model>\n";

        ZipWriter zip;
        zip.add("[Content_Types].xml",
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
                " <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
                " <Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/>\n"
                "</Types>\n");
        zip.add("_rels/.rels",
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
                " <Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" "
                "Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>\n"
                "</Relationships>\n");
        zip.add("3D/3dmodel.model", x.str());
        const std::string bytes = zip.finish();

        std::ofstream f(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
        if (!f)
            throw std::runtime_error("Cannot write file: " + path);
        f.write(bytes.data(), std::streamsize(bytes.size()));
        if (!f)
            throw std::runtime_error("Cannot write file: " + path);
        sum.bytes = bytes.size();
    }
    catch (const Standard_Failure& e) {
        sum.error = std::string("Geometry kernel error: ") + (e.GetMessageString() ? e.GetMessageString() : "unknown");
    }
    catch (const std::exception& e) {
        sum.error = e.what();
    }
    return sum;
}

}  // namespace silex
