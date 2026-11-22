#include "kernel/ops/TextOutline.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <Geom_BezierCurve.hxx>
#include <ShapeFix_Face.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>

#ifdef _WIN32
#include <windows.h>
#endif

namespace silex::kernel {

namespace fs = std::filesystem;

namespace {

// ------------------------------------------------------------------ fonts

std::mutex& ftMutex()
{
    static std::mutex m;
    return m;
}

FT_Library library()
{
    static FT_Library lib = [] {
        FT_Library l = nullptr;
        if (FT_Init_FreeType(&l) != 0)
            throw std::runtime_error("FreeType could not be initialised");
        return l;
    }();
    return lib;
}

std::string envVar(const char* name)
{
#ifdef _WIN32
    char* v = nullptr;
    size_t len = 0;
    if (_dupenv_s(&v, &len, name) != 0 || !v)
        return {};
    std::string s(v);
    std::free(v);
    return s;
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

fs::path exeDir()
{
#ifdef _WIN32
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return fs::path(std::wstring(buf, n)).parent_path();
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}

std::vector<std::string>& extraDirs()
{
    static std::vector<std::string> dirs;
    return dirs;
}

std::vector<fs::path> fontDirs()
{
    std::vector<fs::path> dirs;
    for (const auto& d : extraDirs())
        dirs.emplace_back(std::u8string(d.begin(), d.end()));
    if (const std::string r = envVar("SILEX_RESOURCE_DIR"); !r.empty())
        dirs.emplace_back(fs::path(r) / "fonts");
    dirs.push_back(exeDir() / "resources" / "fonts");
#ifdef _WIN32
    const std::string win = envVar("WINDIR");
    dirs.emplace_back(fs::path(win.empty() ? "C:\\Windows" : win) / "Fonts");
    if (const std::string la = envVar("LOCALAPPDATA"); !la.empty())
        dirs.emplace_back(fs::path(la) / "Microsoft" / "Windows" / "Fonts");
#else
    dirs.emplace_back("/usr/share/fonts");
    dirs.emplace_back("/usr/local/share/fonts");
    if (const std::string h = envVar("HOME"); !h.empty())
        dirs.emplace_back(fs::path(h) / ".fonts");
#endif
    return dirs;
}

std::string toUtf8(const fs::path& p)
{
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool isFontFile(const fs::path& p)
{
    const std::string e = lower(toUtf8(p.extension()));
    return e == ".ttf" || e == ".otf" || e == ".ttc";
}

struct FontEntry
{
    std::string family, style, file;
};

/// family (lower case) -> best file (regular style first). Built once (needs ftMutex()).
const std::map<std::string, FontEntry>& familyIndex()
{
    static std::map<std::string, FontEntry> index;
    static size_t builtFor = size_t(-1);
    if (builtFor == extraDirs().size())
        return index;
    builtFor = extraDirs().size();
    index.clear();
    auto rank = [](const std::string& style) {
        const std::string s = lower(style);
        return s == "regular" ? 0 : s == "book" || s == "normal" || s == "roman" ? 1 : s == "medium" ? 2 : 10;
    };
    std::error_code ec;
    for (const fs::path& dir : fontDirs()) {
        if (!fs::is_directory(dir, ec))
            continue;
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec || !it->is_regular_file(ec) || !isFontFile(it->path()))
                continue;
            FT_Face face = nullptr;
            const std::string file = toUtf8(it->path());
            if (FT_New_Face(library(), file.c_str(), 0, &face) != 0 || !face)
                continue;
            if (face->family_name && FT_IS_SCALABLE(face)) {
                FontEntry e{face->family_name, face->style_name ? face->style_name : "", file};
                auto& slot = index[lower(e.family)];
                if (slot.file.empty() || rank(e.style) < rank(slot.style))
                    slot = e;  // earlier folders win at equal rank (bundled fonts first)
            }
            FT_Done_Face(face);
        }
    }
    return index;
}

std::string resolveLocked(const std::string& font)
{
    std::error_code ec;
    const fs::path asPath(std::u8string(font.begin(), font.end()));
    if (asPath.has_parent_path() && fs::is_regular_file(asPath, ec))
        return font;
    if (isFontFile(asPath)) {
        for (const fs::path& dir : fontDirs())
            for (const fs::path& cand : {dir / asPath.filename()})
                if (fs::is_regular_file(cand, ec))
                    return toUtf8(cand);
        // case-insensitive file name (Windows font folder: ARIAL.TTF)
        const std::string want = lower(toUtf8(asPath.filename()));
        for (const fs::path& dir : fontDirs()) {
            if (!fs::is_directory(dir, ec))
                continue;
            for (const auto& e : fs::directory_iterator(dir, ec))
                if (lower(toUtf8(e.path().filename())) == want)
                    return toUtf8(e.path());
        }
        return {};
    }
    const auto& idx = familyIndex();
    if (auto it = idx.find(lower(font)); it != idx.end())
        return it->second.file;
    return {};
}

struct FaceHolder
{
    FT_Face face = nullptr;
    ~FaceHolder()
    {
        if (face)
            FT_Done_Face(face);
    }
};

// ------------------------------------------------------------------ outlines

enum class SegKind { Line, Quad, Cubic };
struct Segment
{
    SegKind kind = SegKind::Line;
    std::vector<Vec2> pts;  ///< including the start point
};
struct Contour
{
    std::vector<Segment> segs;
    std::vector<Vec2> poly;  ///< sampled polygon (containment / area)
};

struct Decomposer
{
    std::vector<Contour> contours;
    Vec2 last;

    static Vec2 v(const FT_Vector* p) { return {double(p->x), double(p->y)}; }
    static int moveTo(const FT_Vector* to, void* user)
    {
        auto* d = static_cast<Decomposer*>(user);
        d->contours.emplace_back();
        d->last = v(to);
        return 0;
    }
    static int lineTo(const FT_Vector* to, void* user)
    {
        auto* d = static_cast<Decomposer*>(user);
        d->contours.back().segs.push_back({SegKind::Line, {d->last, v(to)}});
        d->last = v(to);
        return 0;
    }
    static int conicTo(const FT_Vector* c, const FT_Vector* to, void* user)
    {
        auto* d = static_cast<Decomposer*>(user);
        d->contours.back().segs.push_back({SegKind::Quad, {d->last, v(c), v(to)}});
        d->last = v(to);
        return 0;
    }
    static int cubicTo(const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to, void* user)
    {
        auto* d = static_cast<Decomposer*>(user);
        d->contours.back().segs.push_back({SegKind::Cubic, {d->last, v(c1), v(c2), v(to)}});
        d->last = v(to);
        return 0;
    }
};

Vec2 bezier(const std::vector<Vec2>& p, double t)
{
    std::vector<Vec2> q = p;
    for (size_t k = q.size() - 1; k > 0; --k)
        for (size_t i = 0; i < k; ++i)
            q[i] = q[i] * (1 - t) + q[i + 1] * t;
    return q[0];
}

double signedArea(const std::vector<Vec2>& poly)
{
    double a = 0;
    for (size_t i = 0, n = poly.size(); i < n; ++i)
        a += poly[i].cross(poly[(i + 1) % n]);
    return a / 2;
}

bool inside(const std::vector<Vec2>& poly, Vec2 p)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Vec2 a = poly[i], b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}

/// UTF-8 -> code points (invalid bytes become U+FFFD).
std::vector<char32_t> decodeUtf8(const std::string& s)
{
    std::vector<char32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        char32_t cp = n == 0 ? c : n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F) : (c & 0x07);
        bool ok = true;
        for (int k = 1; k <= n; ++k) {
            if (i + size_t(k) >= s.size() || (static_cast<unsigned char>(s[i + size_t(k)]) >> 6) != 2) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + size_t(k)]) & 0x3F);
        }
        if (!ok) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += size_t(n) + 1;
    }
    return out;
}

}  // namespace

void addFontDirectory(const std::string& dir)
{
    std::lock_guard lock(ftMutex());
    auto& d = extraDirs();
    if (std::find(d.begin(), d.end(), dir) == d.end())
        d.insert(d.begin(), dir);
}

std::string resolveFontFile(const std::string& font)
{
    std::lock_guard lock(ftMutex());
    library();
    return resolveLocked(font);
}

std::vector<std::string> fontFamilies()
{
    std::lock_guard lock(ftMutex());
    library();
    std::vector<std::string> out;
    for (const auto& [key, e] : familyIndex())
        out.push_back(e.family);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    return out;
}

TextOutline buildTextOutline(const TextLayout& L, const Placement& pl)
{
    if (L.height <= 0)
        throw std::runtime_error("The text height must be positive");
    std::lock_guard lock(ftMutex());
    library();
    TextOutline out;
    out.fontFile = resolveLocked(L.font.empty() ? std::string("Liberation Sans") : L.font);
    if (out.fontFile.empty())
        out.fontFile = resolveLocked("Liberation Sans");  // bundled fallback
    if (out.fontFile.empty())
        throw std::runtime_error("Font '" + L.font + "' not found (and the bundled Liberation Sans is missing)");

    FaceHolder fh;
    if (FT_New_Face(library(), out.fontFile.c_str(), 0, &fh.face) != 0 || !fh.face)
        throw std::runtime_error("Cannot read the font file " + out.fontFile);
    FT_Face face = fh.face;
    if (!FT_IS_SCALABLE(face))
        throw std::runtime_error("The font " + out.fontFile + " has no vector outlines");

    auto load = [&](FT_UInt gi) {
        return FT_Load_Glyph(face, gi, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) == 0;
    };
    // capital height: OS/2 table, else the bounding box of 'H'
    double cap = 0;
    if (auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2)); os2 && os2->version >= 2)
        cap = os2->sCapHeight;
    if (cap <= 0) {
        if (FT_UInt gi = FT_Get_Char_Index(face, 'H'); gi && load(gi)) {
            FT_BBox bb;
            FT_Outline_Get_CBox(&face->glyph->outline, &bb);
            cap = double(bb.yMax);
        }
    }
    if (cap <= 0)
        cap = 0.7 * face->units_per_EM;
    const double s = L.height / cap;

    // stroke width estimate: the stem of 'l' (or 'I')
    for (char32_t ch : {U'l', U'I'})
        if (FT_UInt gi = FT_Get_Char_Index(face, ch); gi && load(gi)) {
            FT_BBox bb;
            FT_Outline_Get_CBox(&face->glyph->outline, &bb);
            out.strokeWidth = double(bb.xMax - bb.xMin) * s;
            break;
        }

    // split into lines
    std::vector<std::vector<char32_t>> lines(1);
    for (char32_t c : decodeUtf8(L.text)) {
        if (c == U'\r')
            continue;
        if (c == U'\n')
            lines.emplace_back();
        else
            lines.back().push_back(c);
    }
    const double pitch = L.lineSpacing * L.height;
    const double top = L.height, bottom = -double(lines.size() - 1) * pitch;
    const double yCenter = (top + bottom) / 2;
    const double a = L.angleDeg * kPi / 180.0, ca = std::cos(a), sa = std::sin(a);
    auto place = [&](Vec2 p) {  // layout -> plane coordinates
        return Vec2{L.anchor.x + p.x * ca - p.y * sa, L.anchor.y + p.x * sa + p.y * ca};
    };

    FT_Outline_Funcs funcs{};
    funcs.move_to = &Decomposer::moveTo;
    funcs.line_to = &Decomposer::lineTo;
    funcs.conic_to = &Decomposer::conicTo;
    funcs.cubic_to = &Decomposer::cubicTo;

    BRep_Builder bb;
    TopoDS_Compound all;
    bb.MakeCompound(all);
    std::set<std::string> missing;
    const bool kerning = FT_HAS_KERNING(face);

    for (size_t li = 0; li < lines.size(); ++li) {
        const auto& line = lines[li];
        // advances (font units) for alignment
        std::vector<FT_UInt> glyphs;
        std::vector<double> penX;
        double pen = 0;
        FT_UInt prev = 0;
        for (char32_t c : line) {
            FT_UInt gi = FT_Get_Char_Index(face, FT_ULong(c));
            if (!gi && c != U' ' && c != U'\t') {
                std::string u8;  // for the warning
                if (c < 0x80)
                    u8 = char(c);
                else
                    u8 = "U+" + std::to_string(unsigned(c));
                missing.insert(u8);
            }
            if (kerning && prev && gi) {
                FT_Vector k;
                if (FT_Get_Kerning(face, prev, gi, FT_KERNING_UNSCALED, &k) == 0)
                    pen += double(k.x);
            }
            glyphs.push_back(gi);
            penX.push_back(pen);
            if (gi && load(gi))
                pen += double(face->glyph->advance.x);
            else if (c == U'\t')
                pen += 4 * 0.25 * face->units_per_EM;
            prev = gi;
        }
        const double width = pen * s;
        const double x0 = L.align == "left" ? 0 : L.align == "right" ? -width : -width / 2;
        const double baseline = -double(li) * pitch - yCenter;

        for (size_t gi = 0; gi < glyphs.size(); ++gi) {
            if (!glyphs[gi] || !load(glyphs[gi]) || face->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
                continue;
            Decomposer dec;
            if (FT_Outline_Decompose(&face->glyph->outline, &funcs, &dec) != 0)
                continue;
            auto tr = [&](Vec2 p) { return place(Vec2{x0 + (penX[gi] + p.x) * s, baseline + p.y * s}); };
            std::vector<Contour> cs;
            for (Contour& c : dec.contours) {
                Contour t;
                for (const Segment& sg : c.segs) {
                    Segment ts{sg.kind, {}};
                    for (const Vec2& p : sg.pts)
                        ts.pts.push_back(tr(p));
                    if (ts.kind == SegKind::Line && (ts.pts[1] - ts.pts[0]).length() < 1e-7)
                        continue;
                    const int n = ts.kind == SegKind::Line ? 1 : 8;
                    for (int k = 0; k < n; ++k)
                        t.poly.push_back(ts.kind == SegKind::Line ? ts.pts[0] : bezier(ts.pts, double(k) / n));
                    t.segs.push_back(std::move(ts));
                }
                if (t.segs.empty() || t.poly.size() < 3 || std::abs(signedArea(t.poly)) < 1e-9)
                    continue;
                // a contour must be closed: FreeType closes implicitly
                const Vec2 first = t.segs.front().pts.front(), lastPt = t.segs.back().pts.back();
                if ((first - lastPt).length() > 1e-7)
                    t.segs.push_back({SegKind::Line, {lastPt, first}});
                cs.push_back(std::move(t));
            }
            // nesting depth: even = outer boundary, odd = hole
            std::vector<int> depth(cs.size(), 0);
            for (size_t i = 0; i < cs.size(); ++i)
                for (size_t j = 0; j < cs.size(); ++j)
                    if (i != j && inside(cs[j].poly, cs[i].poly[0]))
                        ++depth[i];
            auto wireOf = [&](const Contour& c) {
                BRepBuilderAPI_MakeWire mw;
                for (const Segment& sg : c.segs) {
                    if (sg.kind == SegKind::Line) {
                        const Vec3 p0 = pl.toWorld(sg.pts[0]), p1 = pl.toWorld(sg.pts[1]);
                        mw.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(p0.x, p0.y, p0.z), gp_Pnt(p1.x, p1.y, p1.z)).Edge());
                    }
                    else {
                        TColgp_Array1OfPnt poles(1, int(sg.pts.size()));
                        for (size_t k = 0; k < sg.pts.size(); ++k) {
                            const Vec3 w = pl.toWorld(sg.pts[k]);
                            poles.SetValue(int(k) + 1, gp_Pnt(w.x, w.y, w.z));
                        }
                        mw.Add(BRepBuilderAPI_MakeEdge(new Geom_BezierCurve(poles)).Edge());
                    }
                }
                if (!mw.IsDone())
                    throw std::runtime_error("A letter outline could not be closed");
                return mw.Wire();
            };
            const gp_Pln plane(gp_Pnt(pl.origin.x, pl.origin.y, pl.origin.z),
                               gp_Dir(pl.normal.x, pl.normal.y, pl.normal.z));
            for (size_t i = 0; i < cs.size(); ++i) {
                if (depth[i] % 2 != 0)
                    continue;
                BRepBuilderAPI_MakeFace mf(plane, wireOf(cs[i]));
                for (size_t j = 0; j < cs.size(); ++j) {
                    if (depth[j] != depth[i] + 1 || !inside(cs[i].poly, cs[j].poly[0]))
                        continue;
                    // the hole belongs to the innermost outer contour containing it
                    bool nearer = false;
                    for (size_t k = 0; k < cs.size(); ++k)
                        if (k != i && depth[k] % 2 == 0 && depth[k] > depth[i] && inside(cs[k].poly, cs[j].poly[0]))
                            nearer = true;
                    if (!nearer)
                        mf.Add(wireOf(cs[j]));
                }
                if (!mf.IsDone())
                    continue;
                Handle(ShapeFix_Face) fix = new ShapeFix_Face(mf.Face());
                fix->FixOrientation();
                fix->Perform();
                const TopoDS_Face f = fix->Face();
                GProp_GProps gp;
                BRepGProp::SurfaceProperties(f, gp);
                if (std::abs(gp.Mass()) < 1e-10)
                    continue;
                out.area += std::abs(gp.Mass());
                bb.Add(all, f);
                ++out.faceCount;
            }
        }
    }
    for (const auto& m : missing)
        out.warnings.push_back("No glyph for '" + m + "' in " + fs::path(std::u8string(out.fontFile.begin(), out.fontFile.end())).filename().string());
    if (out.faceCount == 0)
        throw std::runtime_error("The text is empty (nothing to draw)");
    out.faces = all;
    return out;
}

}  // namespace silex::kernel
