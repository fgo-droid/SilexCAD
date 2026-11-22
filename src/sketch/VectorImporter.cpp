#include "sketch/VectorImporter.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace silex::vectorimport {
namespace {

constexpr double kTiny = 1e-12;
constexpr int kMaxDepth = 12;  // adaptive subdivision depth (per initial span)

// ------------------------------------------------------------------ small geometry helpers

Vec2 polar(double a) { return {std::cos(a), std::sin(a)}; }
double angleOf(Vec2 v) { return std::atan2(v.y, v.x); }
Vec2 perp(Vec2 v) { return {-v.y, v.x}; }

/// Returns a1 shifted by multiples of 2 pi into (a0, a0 + 2 pi].
double normalizeEnd(double a0, double a1)
{
    while (a1 <= a0)
        a1 += 2 * kPi;
    while (a1 - a0 > 2 * kPi)
        a1 -= 2 * kPi;
    return a1;
}

bool angleInArc(double t, double a0, double a1)
{
    double d = std::fmod(t - a0, 2 * kPi);
    if (d < 0)
        d += 2 * kPi;
    return d <= (a1 - a0) + 1e-12;
}

double distToSegment(Vec2 p, Vec2 a, Vec2 b)
{
    const Vec2 d = b - a;
    const double l2 = d.dot(d);
    if (l2 < 1e-30)
        return (p - a).length();
    const double t = std::clamp((p - a).dot(d) / l2, 0.0, 1.0);
    return (p - (a + d * t)).length();
}

VectorShape mkLine(Vec2 a, Vec2 b)
{
    VectorShape s;
    s.kind = GeoKind::Line;
    s.p1 = a;
    s.p2 = b;
    return s;
}

VectorShape mkArc(Vec2 c, double r, double a0, double a1)
{
    VectorShape s;
    s.kind = GeoKind::Arc;
    s.c = c;
    s.r = r;
    s.a0 = a0;
    s.a1 = normalizeEnd(a0, a1);
    return s;
}

VectorShape mkCircle(Vec2 c, double r)
{
    VectorShape s;
    s.kind = GeoKind::Circle;
    s.c = c;
    s.r = r;
    return s;
}

struct Box
{
    Vec2 lo{std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    Vec2 hi{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};

    bool valid() const { return lo.x <= hi.x && lo.y <= hi.y; }
    void add(Vec2 p)
    {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
    }
    void add(const VectorShape& s)
    {
        switch (s.kind) {
            case GeoKind::Line:
                add(s.p1);
                add(s.p2);
                break;
            case GeoKind::Circle:
                add(s.c - Vec2{s.r, s.r});
                add(s.c + Vec2{s.r, s.r});
                break;
            case GeoKind::Arc:
                add(s.c + polar(s.a0) * s.r);
                add(s.c + polar(s.a1) * s.r);
                for (int k = 0; k < 4; ++k) {
                    const double t = k * 0.5 * kPi;
                    if (angleInArc(t, s.a0, s.a1))
                        add(s.c + polar(t) * s.r);
                }
                break;
            case GeoKind::Point: add(s.p1); break;
        }
    }
};

Box boxOf(const std::vector<VectorShape>& shapes)
{
    Box b;
    for (const VectorShape& s : shapes)
        b.add(s);
    return b;
}

bool finiteShape(const VectorShape& s)
{
    auto f = [](double v) { return std::isfinite(v); };
    return f(s.p1.x) && f(s.p1.y) && f(s.p2.x) && f(s.p2.y) && f(s.c.x) && f(s.c.y) && f(s.r) && f(s.a0) &&
           f(s.a1);
}

void warn(std::vector<std::string>* w, std::string msg)
{
    if (w)
        w->push_back(std::move(msg));
}

// ------------------------------------------------------------------ adaptive discretisation

using Curve = std::function<Vec2(double)>;

void flattenSpan(const Curve& f, double ta, Vec2 pa, double tb, Vec2 pb, double tol, int depth,
                 std::vector<Vec2>& out)
{
    const double tm = 0.5 * (ta + tb);
    const Vec2 pm = f(tm);
    const Vec2 q1 = f(ta + 0.25 * (tb - ta));
    const Vec2 q3 = f(ta + 0.75 * (tb - ta));
    const double dev = std::max({distToSegment(pm, pa, pb), distToSegment(q1, pa, pb), distToSegment(q3, pa, pb)});
    if (dev <= tol || depth >= kMaxDepth) {
        out.push_back(pb);
        return;
    }
    flattenSpan(f, ta, pa, tm, pm, tol, depth + 1, out);
    flattenSpan(f, tm, pm, tb, pb, tol, depth + 1, out);
}

/// Discretises f on [t0, t1] (first split into `initial` equal spans) into lines.
void flatten(const Curve& f, double t0, double t1, int initial, double tol, std::vector<VectorShape>& out)
{
    initial = std::max(initial, 1);
    tol = std::max(tol, 1e-7);
    std::vector<Vec2> pts{f(t0)};
    Vec2 prev = pts.front();
    for (int i = 1; i <= initial; ++i) {
        const double ta = t0 + (t1 - t0) * static_cast<double>(i - 1) / initial;
        const double tb = i == initial ? t1 : t0 + (t1 - t0) * static_cast<double>(i) / initial;
        const Vec2 pb = f(tb);
        flattenSpan(f, ta, prev, tb, pb, tol, 0, pts);
        prev = pb;
    }
    for (size_t i = 1; i < pts.size(); ++i)
        if ((pts[i] - pts[i - 1]).length() > kTiny)
            out.push_back(mkLine(pts[i - 1], pts[i]));
}

int spansFor(double sweep) { return std::max(4, static_cast<int>(std::ceil(std::abs(sweep) / (0.25 * kPi)))); }

/// Locale-independent number parsing (accepts a leading '+'). Returns the characters consumed (0 = none).
size_t parseDouble(std::string_view s, double& v)
{
    size_t i = 0;
    if (i < s.size() && s[i] == '+')
        ++i;
    const char* first = s.data() + i;
    const auto [ptr, ec] = std::from_chars(first, s.data() + s.size(), v);
    if (ec != std::errc())
        return 0;
    return static_cast<size_t>(ptr - s.data());
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

std::string_view trim(std::string_view s)
{
    while (!s.empty() && isSpace(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back()))
        s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s)
{
    std::string r(s);
    for (char& c : r)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

// ================================================================== SVG

/// 2D affine map, SVG layout: x' = a x + c y + e, y' = b x + d y + f.
struct Affine
{
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

    Vec2 apply(Vec2 p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
    /// (this * m)(p) = this(m(p))
    Affine operator*(const Affine& m) const
    {
        return {a * m.a + c * m.b, b * m.a + d * m.b, a * m.c + c * m.d,
                b * m.c + d * m.d, a * m.e + c * m.f + e, b * m.e + d * m.f + f};
    }
    double det() const { return a * d - b * c; }
    /// True when the map is a similarity (uniform scale, rotation, possibly mirrored); `s` = scale.
    bool similarity(double& s) const
    {
        const double n1 = a * a + b * b, n2 = c * c + d * d, dt = a * c + b * d;
        const double ref = std::max(n1, n2);
        if (ref < 1e-30 || std::abs(n1 - n2) > 1e-9 * ref || std::abs(dt) > 1e-9 * ref)
            return false;
        s = std::sqrt(0.5 * (n1 + n2));
        return true;
    }
};

/// Number / flag scanner for path data, points, viewBox and transform arguments.
class Scanner
{
public:
    explicit Scanner(std::string_view s) : m_s(s) {}

    void skipSep()
    {
        while (m_i < m_s.size() && (isSpace(m_s[m_i]) || m_s[m_i] == ','))
            ++m_i;
    }
    bool atEnd()
    {
        skipSep();
        return m_i >= m_s.size();
    }
    char peek() const { return m_i < m_s.size() ? m_s[m_i] : '\0'; }
    void advance() { ++m_i; }
    bool hasNumber()
    {
        skipSep();
        if (m_i >= m_s.size())
            return false;
        const char c = m_s[m_i];
        return isDigit(c) || c == '.' || c == '-' || c == '+';
    }
    /// SVG number grammar: "1.5.5" = 1.5 .5, "-1-2" = -1 -2, exponents.
    double number()
    {
        skipSep();
        const size_t n = m_s.size();
        size_t j = m_i;
        if (j < n && (m_s[j] == '+' || m_s[j] == '-'))
            ++j;
        bool digits = false;
        while (j < n && isDigit(m_s[j])) {
            ++j;
            digits = true;
        }
        if (j < n && m_s[j] == '.') {
            ++j;
            while (j < n && isDigit(m_s[j])) {
                ++j;
                digits = true;
            }
        }
        if (!digits)
            throw std::runtime_error("SVG: number expected");
        if (j < n && (m_s[j] == 'e' || m_s[j] == 'E')) {
            size_t k = j + 1;
            if (k < n && (m_s[k] == '+' || m_s[k] == '-'))
                ++k;
            if (k < n && isDigit(m_s[k])) {
                while (k < n && isDigit(m_s[k]))
                    ++k;
                j = k;
            }
        }
        double v = 0;
        if (parseDouble(m_s.substr(m_i, j - m_i), v) == 0)
            throw std::runtime_error("SVG: invalid number");
        m_i = j;
        return v;
    }
    bool flag()
    {
        skipSep();
        if (m_i < m_s.size() && (m_s[m_i] == '0' || m_s[m_i] == '1'))
            return m_s[m_i++] == '1';
        throw std::runtime_error("SVG: arc flag expected");
    }

private:
    std::string_view m_s;
    size_t m_i = 0;
};

/// Emits pieces in the final (mm, Y up) space through the current transformation matrix.
class Emitter
{
public:
    Emitter(std::vector<VectorShape>& out, double tol) : m_out(out), m_tol(tol) {}

    Affine m;

    void line(Vec2 a, Vec2 b)
    {
        const Vec2 p = m.apply(a), q = m.apply(b);
        if ((q - p).length() > kTiny)
            m_out.push_back(mkLine(p, q));
    }

    void cubic(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3)
    {
        // Béziers are affine invariant: discretise the mapped control polygon in mm.
        const Vec2 q0 = m.apply(p0), q1 = m.apply(p1), q2 = m.apply(p2), q3 = m.apply(p3);
        const Curve f = [q0, q1, q2, q3](double t) {
            const double u = 1 - t;
            return q0 * (u * u * u) + q1 * (3 * u * u * t) + q2 * (3 * u * t * t) + q3 * (t * t * t);
        };
        flatten(f, 0, 1, 4, m_tol, m_out);
    }

    void quad(Vec2 p0, Vec2 p1, Vec2 p2)
    {
        const Vec2 q0 = m.apply(p0), q1 = m.apply(p1), q2 = m.apply(p2);
        const Curve f = [q0, q1, q2](double t) {
            const double u = 1 - t;
            return q0 * (u * u) + q1 * (2 * u * t) + q2 * (t * t);
        };
        flatten(f, 0, 1, 4, m_tol, m_out);
    }

    /// Elliptical arc in user space, from parameter t0 over dt (signed), discretised.
    void ellipseArc(Vec2 c, double rx, double ry, double phi, double t0, double dt)
    {
        const Affine mm = m;
        const double cp = std::cos(phi), sp = std::sin(phi);
        const Curve f = [mm, c, rx, ry, cp, sp](double t) {
            const double x = rx * std::cos(t), y = ry * std::sin(t);
            return mm.apply(c + Vec2{x * cp - y * sp, x * sp + y * cp});
        };
        flatten(f, t0, t0 + dt, spansFor(dt), m_tol, m_out);
    }

    void circle(Vec2 c, double r)
    {
        double s = 0;
        if (m.similarity(s))
            m_out.push_back(mkCircle(m.apply(c), r * s));
        else
            ellipseArc(c, r, r, 0, 0, 2 * kPi);
    }

    void ellipse(Vec2 c, double rx, double ry)
    {
        if (std::abs(rx - ry) <= 1e-9 * std::max(rx, ry))
            circle(c, rx);
        else
            ellipseArc(c, rx, ry, 0, 0, 2 * kPi);
    }

    /// SVG "A" command (endpoint parameterisation, implementation notes F.6.5 / F.6.6).
    void svgArc(Vec2 p0, double rx, double ry, double phiDeg, bool large, bool sweep, Vec2 p1)
    {
        if ((p1 - p0).length() < kTiny)
            return;  // omitted
        rx = std::abs(rx);
        ry = std::abs(ry);
        if (rx < kTiny || ry < kTiny) {
            line(p0, p1);
            return;
        }
        const double phi = phiDeg * kPi / 180.0;
        const double cp = std::cos(phi), sp = std::sin(phi);
        const Vec2 h = (p0 - p1) * 0.5;
        const double x1 = cp * h.x + sp * h.y, y1 = -sp * h.x + cp * h.y;
        const double lambda = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
        if (lambda > 1) {
            const double k = std::sqrt(lambda);
            rx *= k;
            ry *= k;
        }
        const double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
        const double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        double coef = den > 0 ? std::sqrt(std::max(0.0, num / den)) : 0.0;
        if (large == sweep)
            coef = -coef;
        const double cxp = coef * rx * y1 / ry, cyp = -coef * ry * x1 / rx;
        const Vec2 mid = (p0 + p1) * 0.5;
        const Vec2 c{cp * cxp - sp * cyp + mid.x, sp * cxp + cp * cyp + mid.y};
        const Vec2 u{(x1 - cxp) / rx, (y1 - cyp) / ry}, v{(-x1 - cxp) / rx, (-y1 - cyp) / ry};
        const double t0 = angleOf(u);
        double dt = std::atan2(u.cross(v), u.dot(v));
        if (!sweep && dt > 0)
            dt -= 2 * kPi;
        else if (sweep && dt < 0)
            dt += 2 * kPi;
        if (std::abs(dt) < 1e-12)
            return;

        double s = 0;
        if (std::abs(rx - ry) <= 1e-9 * std::max(rx, ry) && m.similarity(s)) {
            // true circular arc; the final orientation flips with a mirroring map (Y flip included)
            const Vec2 C = m.apply(c), S = m.apply(p0), E = m.apply(p1);
            const double R = rx * s;
            const bool ccw = (dt > 0) == (m.det() > 0);
            m_out.push_back(ccw ? mkArc(C, R, angleOf(S - C), angleOf(E - C))
                                : mkArc(C, R, angleOf(E - C), angleOf(S - C)));
        }
        else {
            ellipseArc(c, rx, ry, phi, t0, dt);
        }
    }

private:
    std::vector<VectorShape>& m_out;
    double m_tol;
};

void parsePath(std::string_view d, Emitter& em)
{
    Scanner sc(d);
    Vec2 cur, start, lastCtrl;
    char lastUp = 0;
    while (!sc.atEnd()) {
        const char cmd = sc.peek();
        if (!isAlpha(cmd))
            throw std::runtime_error("SVG path: command expected");
        sc.advance();
        const bool rel = cmd >= 'a' && cmd <= 'z';
        const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));
        auto pt = [&]() {
            const double x = sc.number();
            const double y = sc.number();
            return rel ? cur + Vec2{x, y} : Vec2{x, y};
        };
        if (up == 'Z') {
            em.line(cur, start);
            cur = start;
            lastCtrl = cur;
            lastUp = 'Z';
            continue;
        }
        bool first = true;
        do {
            char done = up;
            switch (up) {
                case 'M': {
                    const Vec2 p = pt();
                    if (first) {
                        cur = start = p;
                    }
                    else {  // implicit lineto
                        em.line(cur, p);
                        cur = p;
                        done = 'L';
                    }
                    break;
                }
                case 'L': {
                    const Vec2 p = pt();
                    em.line(cur, p);
                    cur = p;
                    break;
                }
                case 'H': {
                    const double x = sc.number();
                    const Vec2 p{rel ? cur.x + x : x, cur.y};
                    em.line(cur, p);
                    cur = p;
                    break;
                }
                case 'V': {
                    const double y = sc.number();
                    const Vec2 p{cur.x, rel ? cur.y + y : y};
                    em.line(cur, p);
                    cur = p;
                    break;
                }
                case 'C': {
                    const Vec2 c1 = pt();
                    const Vec2 c2 = pt();
                    const Vec2 p = pt();
                    em.cubic(cur, c1, c2, p);
                    lastCtrl = c2;
                    cur = p;
                    break;
                }
                case 'S': {
                    const Vec2 c1 = (lastUp == 'C' || lastUp == 'S') ? cur * 2.0 - lastCtrl : cur;
                    const Vec2 c2 = pt();
                    const Vec2 p = pt();
                    em.cubic(cur, c1, c2, p);
                    lastCtrl = c2;
                    cur = p;
                    break;
                }
                case 'Q': {
                    const Vec2 q = pt();
                    const Vec2 p = pt();
                    em.quad(cur, q, p);
                    lastCtrl = q;
                    cur = p;
                    break;
                }
                case 'T': {
                    const Vec2 q = (lastUp == 'Q' || lastUp == 'T') ? cur * 2.0 - lastCtrl : cur;
                    const Vec2 p = pt();
                    em.quad(cur, q, p);
                    lastCtrl = q;
                    cur = p;
                    break;
                }
                case 'A': {
                    const double rx = sc.number();
                    const double ry = sc.number();
                    const double rot = sc.number();
                    const bool large = sc.flag();
                    const bool sweep = sc.flag();
                    const Vec2 p = pt();
                    em.svgArc(cur, rx, ry, rot, large, sweep, p);
                    cur = p;
                    break;
                }
                default: throw std::runtime_error(std::string("SVG path: unknown command '") + cmd + "'");
            }
            lastUp = done;
            first = false;
        } while (sc.hasNumber());
    }
}

Affine parseTransform(std::string_view s)
{
    Affine m;
    size_t i = 0;
    while (true) {
        while (i < s.size() && (isSpace(s[i]) || s[i] == ','))
            ++i;
        if (i >= s.size())
            break;
        const size_t n0 = i;
        while (i < s.size() && isAlpha(s[i]))
            ++i;
        const std::string name(s.substr(n0, i - n0));
        while (i < s.size() && isSpace(s[i]))
            ++i;
        if (name.empty() || i >= s.size() || s[i] != '(')
            throw std::runtime_error("SVG: malformed transform");
        const size_t close = s.find(')', i);
        if (close == std::string_view::npos)
            throw std::runtime_error("SVG: malformed transform");
        Scanner sc(s.substr(i + 1, close - i - 1));
        std::vector<double> v;
        while (sc.hasNumber())
            v.push_back(sc.number());
        i = close + 1;

        auto arg = [&](size_t k, double def) { return k < v.size() ? v[k] : def; };
        Affine t;
        if (name == "matrix" && v.size() >= 6) {
            t = {v[0], v[1], v[2], v[3], v[4], v[5]};
        }
        else if (name == "translate" && !v.empty()) {
            t = {1, 0, 0, 1, v[0], arg(1, 0)};
        }
        else if (name == "scale" && !v.empty()) {
            t = {v[0], 0, 0, arg(1, v[0]), 0, 0};
        }
        else if (name == "rotate" && !v.empty()) {
            const double a = v[0] * kPi / 180.0, ca = std::cos(a), sa = std::sin(a);
            t = {ca, sa, -sa, ca, 0, 0};
            if (v.size() >= 3) {
                const Affine to{1, 0, 0, 1, v[1], v[2]}, back{1, 0, 0, 1, -v[1], -v[2]};
                t = to * t * back;
            }
        }
        else if (name == "skewX" && !v.empty()) {
            t = {1, 0, std::tan(v[0] * kPi / 180.0), 1, 0, 0};
        }
        else if (name == "skewY" && !v.empty()) {
            t = {1, std::tan(v[0] * kPi / 180.0), 0, 1, 0, 0};
        }
        else {
            throw std::runtime_error("SVG: unsupported transform '" + name + "'");
        }
        m = m * t;
    }
    return m;
}

// ------------------------------------------------------------------ minimal XML tokenizer

struct XmlTag
{
    std::string name;
    std::map<std::string, std::string> attrs;
    bool closing = false;
    bool selfClosing = false;

    const std::string* attr(const char* key) const
    {
        auto it = attrs.find(key);
        return it == attrs.end() ? nullptr : &it->second;
    }
};

class XmlReader
{
public:
    explicit XmlReader(std::string_view s) : m_s(s) {}

    /// Next start / end tag (comments, PIs, DOCTYPE, CDATA and text are skipped). False at the end.
    bool next(XmlTag& t)
    {
        const size_t n = m_s.size();
        while (true) {
            const size_t lt = m_s.find('<', m_i);
            if (lt == std::string_view::npos)
                return false;
            m_i = lt;
            if (starts("<!--")) {
                m_i = skipPast("-->", m_i + 4);
                continue;
            }
            if (starts("<![CDATA[")) {
                m_i = skipPast("]]>", m_i + 9);
                continue;
            }
            if (starts("<?")) {
                m_i = skipPast("?>", m_i + 2);
                continue;
            }
            if (starts("<!")) {  // DOCTYPE, possibly with an internal subset
                int depth = 0;
                size_t j = m_i + 2;
                for (; j < n; ++j) {
                    if (m_s[j] == '[')
                        ++depth;
                    else if (m_s[j] == ']')
                        --depth;
                    else if (m_s[j] == '>' && depth <= 0)
                        break;
                }
                if (j >= n)
                    throw std::runtime_error("XML: unterminated declaration");
                m_i = j + 1;
                continue;
            }

            t = XmlTag{};
            size_t j = m_i + 1;
            if (j < n && m_s[j] == '/') {
                t.closing = true;
                ++j;
            }
            const size_t n0 = j;
            while (j < n && !isSpace(m_s[j]) && m_s[j] != '>' && m_s[j] != '/')
                ++j;
            t.name = localName(m_s.substr(n0, j - n0));
            if (t.closing) {
                const size_t gt = m_s.find('>', j);
                if (gt == std::string_view::npos)
                    throw std::runtime_error("XML: unterminated tag");
                m_i = gt + 1;
                return true;
            }
            bool ended = false;
            while (j < n) {
                while (j < n && isSpace(m_s[j]))
                    ++j;
                if (j >= n)
                    break;
                if (m_s[j] == '>') {
                    ++j;
                    ended = true;
                    break;
                }
                if (m_s[j] == '/' && j + 1 < n && m_s[j + 1] == '>') {
                    t.selfClosing = true;
                    j += 2;
                    ended = true;
                    break;
                }
                const size_t a0 = j;
                while (j < n && !isSpace(m_s[j]) && m_s[j] != '=' && m_s[j] != '>' && m_s[j] != '/')
                    ++j;
                const std::string key(m_s.substr(a0, j - a0));
                if (key.empty()) {
                    ++j;  // stray character
                    continue;
                }
                while (j < n && isSpace(m_s[j]))
                    ++j;
                std::string value;
                if (j < n && m_s[j] == '=') {
                    ++j;
                    while (j < n && isSpace(m_s[j]))
                        ++j;
                    if (j < n && (m_s[j] == '"' || m_s[j] == '\'')) {
                        const size_t end = m_s.find(m_s[j], j + 1);
                        if (end == std::string_view::npos)
                            throw std::runtime_error("XML: unterminated attribute value");
                        value = decode(m_s.substr(j + 1, end - j - 1));
                        j = end + 1;
                    }
                    else {
                        const size_t v0 = j;
                        while (j < n && !isSpace(m_s[j]) && m_s[j] != '>')
                            ++j;
                        value = std::string(m_s.substr(v0, j - v0));
                    }
                }
                t.attrs[key] = std::move(value);
            }
            if (!ended)
                throw std::runtime_error("XML: unterminated tag");
            m_i = j;
            return true;
        }
    }

private:
    bool starts(std::string_view p) const { return m_s.substr(m_i, p.size()) == p; }
    size_t skipPast(std::string_view end, size_t from) const
    {
        const size_t e = m_s.find(end, from);
        if (e == std::string_view::npos)
            throw std::runtime_error("XML: unterminated comment or section");
        return e + end.size();
    }
    static std::string localName(std::string_view s)
    {
        const size_t colon = s.find(':');
        return std::string(colon == std::string_view::npos ? s : s.substr(colon + 1));
    }
    static std::string decode(std::string_view s)
    {
        std::string r;
        r.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '&') {
                static const std::array<std::pair<std::string_view, char>, 5> kEnt{
                    {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}}};
                bool found = false;
                for (const auto& [ent, ch] : kEnt)
                    if (s.substr(i, ent.size()) == ent) {
                        r += ch;
                        i += ent.size() - 1;
                        found = true;
                        break;
                    }
                if (found)
                    continue;
            }
            r += s[i];
        }
        return r;
    }

    std::string_view m_s;
    size_t m_i = 0;
};

// ------------------------------------------------------------------ SVG document walk

/// mm per unit of a CSS length unit; 0 = unknown ("%" , "em"...).
double unitMm(std::string_view u)
{
    const std::string s = lower(u);
    if (s.empty() || s == "px")
        return 25.4 / 96.0;
    if (s == "mm")
        return 1.0;
    if (s == "cm")
        return 10.0;
    if (s == "in")
        return 25.4;
    if (s == "pt")
        return 25.4 / 72.0;
    if (s == "pc")
        return 25.4 / 6.0;
    if (s == "q")
        return 0.25;
    return 0.0;
}

struct Length
{
    double v = 0;
    std::string unit;
    bool ok = false;
};

Length parseLength(const std::string* attr)
{
    Length l;
    if (!attr)
        return l;
    const std::string_view s = trim(*attr);
    const size_t used = parseDouble(s, l.v);
    if (used == 0)
        return l;
    l.unit = std::string(trim(s.substr(used)));
    l.ok = std::isfinite(l.v);
    return l;
}

/// A length attribute in user units (physical units converted with 96 user units per inch).
double userLength(const XmlTag& t, const char* key, double def)
{
    const Length l = parseLength(t.attr(key));
    if (!l.ok)
        return def;
    const double mm = unitMm(l.unit);
    if (mm <= 0)
        return def;  // percentages / font-relative units: not supported
    return l.v * mm / (25.4 / 96.0);
}

bool hidden(const XmlTag& t)
{
    if (const std::string* d = t.attr("display"); d && trim(*d) == "none")
        return true;
    if (const std::string* st = t.attr("style")) {
        std::string_view s = *st;
        while (!s.empty()) {
            const size_t semi = s.find(';');
            const std::string_view decl = s.substr(0, semi);
            const size_t colon = decl.find(':');
            if (colon != std::string_view::npos && trim(decl.substr(0, colon)) == "display" &&
                trim(decl.substr(colon + 1)) == "none")
                return true;
            if (semi == std::string_view::npos)
                break;
            s.remove_prefix(semi + 1);
        }
    }
    return false;
}

/// Root transform: user units -> mm with the Y axis flipped.
Affine rootTransform(const XmlTag& t)
{
    double k = 25.4 / 96.0;
    Vec2 vbMin{0, 0};
    if (const std::string* vb = t.attr("viewBox")) {
        Scanner sc(*vb);
        std::vector<double> v;
        while (sc.hasNumber() && v.size() < 4)
            v.push_back(sc.number());
        if (v.size() == 4 && v[2] > 0 && v[3] > 0) {
            vbMin = {v[0], v[1]};
            const Length w = parseLength(t.attr("width")), h = parseLength(t.attr("height"));
            const double wu = w.ok ? unitMm(w.unit) : 0, hu = h.ok ? unitMm(h.unit) : 0;
            const double sx = wu > 0 && w.v > 0 ? w.v * wu / v[2] : 0;
            const double sy = hu > 0 && h.v > 0 ? h.v * hu / v[3] : 0;
            if (sx > 0 && sy > 0)
                k = std::min(sx, sy);  // preserveAspectRatio "meet"
            else if (sx > 0)
                k = sx;
            else if (sy > 0)
                k = sy;
            // else: the viewport defaults to the viewBox size in px -> 1 user unit = 1 px
        }
    }
    return Affine{k, 0, 0, -k, -k * vbMin.x, k * vbMin.y};
}

std::vector<Vec2> parsePoints(const std::string* attr)
{
    std::vector<Vec2> pts;
    if (!attr)
        return pts;
    Scanner sc(*attr);
    std::vector<double> v;
    while (sc.hasNumber())
        v.push_back(sc.number());
    for (size_t i = 0; i + 1 < v.size(); i += 2)
        pts.push_back({v[i], v[i + 1]});
    return pts;
}

void emitRect(const XmlTag& t, Emitter& em)
{
    const double x = userLength(t, "x", 0), y = userLength(t, "y", 0);
    const double w = userLength(t, "width", 0), h = userLength(t, "height", 0);
    if (!(w > 0) || !(h > 0))
        return;
    const bool hasRx = t.attr("rx") != nullptr, hasRy = t.attr("ry") != nullptr;
    double rx = std::max(0.0, userLength(t, "rx", 0)), ry = std::max(0.0, userLength(t, "ry", 0));
    if (hasRx && !hasRy)
        ry = rx;
    else if (hasRy && !hasRx)
        rx = ry;
    rx = std::min(rx, w / 2);
    ry = std::min(ry, h / 2);
    if (rx <= kTiny || ry <= kTiny) {
        const Vec2 p[4] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
        for (int i = 0; i < 4; ++i)
            em.line(p[i], p[(i + 1) % 4]);
        return;
    }
    // rounded corners: lines + quarter arcs (true arcs when rx == ry under a similarity)
    em.line({x + rx, y}, {x + w - rx, y});
    em.svgArc({x + w - rx, y}, rx, ry, 0, false, true, {x + w, y + ry});
    em.line({x + w, y + ry}, {x + w, y + h - ry});
    em.svgArc({x + w, y + h - ry}, rx, ry, 0, false, true, {x + w - rx, y + h});
    em.line({x + w - rx, y + h}, {x + rx, y + h});
    em.svgArc({x + rx, y + h}, rx, ry, 0, false, true, {x, y + h - ry});
    em.line({x, y + h - ry}, {x, y + ry});
    em.svgArc({x, y + ry}, rx, ry, 0, false, true, {x + rx, y});
}

/// Emits a shape element (path, rect, circle, ellipse, line, polyline, polygon).
void emitShape(const XmlTag& t, Emitter& em)
{
    const std::string& n = t.name;
    if (n == "path") {
        if (const std::string* d = t.attr("d"))
            parsePath(*d, em);
    }
    else if (n == "rect") {
        emitRect(t, em);
    }
    else if (n == "circle") {
        const double r = userLength(t, "r", 0);
        if (r > 0)
            em.circle({userLength(t, "cx", 0), userLength(t, "cy", 0)}, r);
    }
    else if (n == "ellipse") {
        const double rx = userLength(t, "rx", 0), ry = userLength(t, "ry", 0);
        if (rx > 0 && ry > 0)
            em.ellipse({userLength(t, "cx", 0), userLength(t, "cy", 0)}, rx, ry);
    }
    else if (n == "line") {
        em.line({userLength(t, "x1", 0), userLength(t, "y1", 0)}, {userLength(t, "x2", 0), userLength(t, "y2", 0)});
    }
    else if (n == "polyline" || n == "polygon") {
        const std::vector<Vec2> pts = parsePoints(t.attr("points"));
        for (size_t i = 1; i < pts.size(); ++i)
            em.line(pts[i - 1], pts[i]);
        if (n == "polygon" && pts.size() > 2)
            em.line(pts.back(), pts.front());
    }
}

bool isContainer(const std::string& n) { return n == "svg" || n == "g" || n == "a" || n == "switch"; }
bool isShape(const std::string& n)
{
    return n == "path" || n == "rect" || n == "circle" || n == "ellipse" || n == "line" || n == "polyline" ||
           n == "polygon";
}

// ================================================================== DXF

struct Pair
{
    int code = 0;
    std::string value;
};

double toD(const std::string& s)
{
    double v = 0;
    if (parseDouble(trim(s), v) == 0 || !std::isfinite(v))
        return 0;
    return v;
}

int toI(const std::string& s)
{
    const std::string_view t = trim(s);
    int v = 0;
    const std::from_chars_result r = std::from_chars(t.data(), t.data() + t.size(), v);
    return r.ec == std::errc() ? v : 0;
}

std::vector<Pair> readPairs(const std::string& text)
{
    std::vector<std::string_view> lines;
    std::string_view s = text;
    if (s.substr(0, 3) == "\xEF\xBB\xBF")
        s.remove_prefix(3);
    if (s.substr(0, 18) == "AutoCAD Binary DXF")
        throw std::runtime_error("Binary DXF files are not supported (save as ASCII DXF)");
    while (!s.empty()) {
        const size_t nl = s.find('\n');
        lines.push_back(trim(s.substr(0, nl)));
        if (nl == std::string_view::npos)
            break;
        s.remove_prefix(nl + 1);
    }
    while (!lines.empty() && lines.back().empty())
        lines.pop_back();
    std::vector<Pair> pairs;
    pairs.reserve(lines.size() / 2);
    for (size_t i = 0; i + 1 < lines.size(); i += 2) {
        int code = 0;
        const std::string_view c = lines[i];
        const auto [ptr, ec] = std::from_chars(c.data(), c.data() + c.size(), code);
        if (ec != std::errc() || ptr != c.data() + c.size())
            throw std::runtime_error("DXF: invalid group code at line " + std::to_string(i + 1));
        pairs.push_back({code, std::string(lines[i + 1])});
    }
    return pairs;
}

struct Entity
{
    std::string type;
    std::vector<Pair> g;

    double get(int code, double def = 0) const
    {
        for (const Pair& p : g)
            if (p.code == code)
                return toD(p.value);
        return def;
    }
    int geti(int code, int def = 0) const
    {
        for (const Pair& p : g)
            if (p.code == code)
                return toI(p.value);
        return def;
    }
    bool has(int code) const
    {
        return std::any_of(g.begin(), g.end(), [code](const Pair& p) { return p.code == code; });
    }
    /// OCS with a -Z extrusion: the entity is mirrored in X when seen from +Z.
    bool mirrored() const { return get(230, 1.0) < 0; }
};

double insUnitsMm(int u)
{
    switch (u) {
        case 1: return 25.4;           // inch
        case 2: return 304.8;          // foot
        case 3: return 1609344.0;      // mile
        case 4: return 1.0;            // mm
        case 5: return 10.0;           // cm
        case 6: return 1000.0;         // m
        case 7: return 1.0e6;          // km
        case 8: return 2.54e-5;        // microinch
        case 9: return 0.0254;         // mil
        case 10: return 914.4;         // yard
        case 11: return 1.0e-7;        // angstrom
        case 12: return 1.0e-6;        // nanometre
        case 13: return 1.0e-3;        // micron
        case 14: return 100.0;         // decimetre
        default: return 1.0;           // unitless / unknown: mm
    }
}

void mirrorX(VectorShape& s)
{
    switch (s.kind) {
        case GeoKind::Line:
            s.p1.x = -s.p1.x;
            s.p2.x = -s.p2.x;
            break;
        case GeoKind::Circle: s.c.x = -s.c.x; break;
        case GeoKind::Arc: {
            s.c.x = -s.c.x;
            const double a0 = kPi - s.a1, a1 = kPi - s.a0;
            s.a0 = a0;
            s.a1 = normalizeEnd(a0, a1);
            break;
        }
        case GeoKind::Point: s.p1.x = -s.p1.x; break;
    }
}

/// Polyline segment p -> q with a DXF bulge (tan of a quarter of the included angle, > 0 = CCW).
void bulgeSegment(Vec2 p, Vec2 q, double b, std::vector<VectorShape>& out)
{
    const Vec2 d = q - p;
    if (d.length() < kTiny)
        return;
    if (std::abs(b) < 1e-12) {
        out.push_back(mkLine(p, q));
        return;
    }
    const Vec2 c = (p + q) * 0.5 + perp(d) * ((1 - b * b) / (4 * b));
    const double r = (p - c).length();
    out.push_back(b > 0 ? mkArc(c, r, angleOf(p - c), angleOf(q - c)) : mkArc(c, r, angleOf(q - c), angleOf(p - c)));
}

struct PolyVertex
{
    Vec2 p;
    double bulge = 0;
};

void emitPolyline(const std::vector<PolyVertex>& v, bool closed, std::vector<VectorShape>& out)
{
    if (v.size() < 2)
        return;
    for (size_t i = 0; i + 1 < v.size(); ++i)
        bulgeSegment(v[i].p, v[i + 1].p, v[i].bulge, out);
    if (closed)
        bulgeSegment(v.back().p, v.front().p, v.back().bulge, out);
}

/// Rational de Boor evaluation in the knot span k (U[k] <= t <= U[k+1]).
Vec2 deBoor(size_t p, size_t k, double t, const std::vector<double>& U, const std::vector<Vec2>& P,
            const std::vector<double>& W)
{
    std::vector<std::array<double, 3>> d(p + 1);
    for (size_t j = 0; j <= p; ++j) {
        const size_t idx = j + k - p;
        d[j] = {P[idx].x * W[idx], P[idx].y * W[idx], W[idx]};
    }
    for (size_t r = 1; r <= p; ++r)
        for (size_t j = p; j >= r; --j) {
            const size_t i = j + k - p;
            const double den = U[i + p - r + 1] - U[i];
            const double alpha = std::abs(den) < 1e-300 ? 0.0 : (t - U[i]) / den;
            for (size_t q = 0; q < 3; ++q)
                d[j][q] = (1 - alpha) * d[j - 1][q] + alpha * d[j][q];
        }
    const double w = std::abs(d[p][2]) < 1e-300 ? 1.0 : d[p][2];
    return {d[p][0] / w, d[p][1] / w};
}

class DxfReader
{
public:
    DxfReader(std::vector<std::string>* warnings, double tol) : m_warnings(warnings), m_tol(tol) {}

    std::vector<VectorShape> read(const std::string& text)
    {
        const std::vector<Pair> pairs = readPairs(text);
        std::vector<Entity> entities;
        std::string section;
        bool sawSection = false;
        for (size_t i = 0; i < pairs.size();) {
            const Pair& p = pairs[i];
            if (p.code == 0 && p.value == "SECTION") {
                sawSection = true;
                section = (i + 1 < pairs.size() && pairs[i + 1].code == 2) ? pairs[i + 1].value : std::string();
                i += 2;
                continue;
            }
            if (p.code == 0 && p.value == "ENDSEC") {
                section.clear();
                ++i;
                continue;
            }
            if (p.code == 0 && p.value == "EOF")
                break;
            if (section == "HEADER" && p.code == 9 && p.value == "$INSUNITS") {
                if (i + 1 < pairs.size() && pairs[i + 1].code == 70)
                    m_unit = insUnitsMm(toI(pairs[i + 1].value));
                i += 2;
                continue;
            }
            if (section == "BLOCKS" && p.code == 0 && p.value == "BLOCK")
                ++m_blocks;
            if (section == "ENTITIES" && p.code == 0) {
                Entity e;
                e.type = p.value;
                ++i;
                while (i < pairs.size() && pairs[i].code != 0)
                    e.g.push_back(pairs[i++]);
                entities.push_back(std::move(e));
                continue;
            }
            ++i;
        }
        if (!sawSection)
            throw std::runtime_error("Not a DXF file (no SECTION found)");

        bool inPoly = false;
        Entity poly;
        std::vector<Entity> verts;
        for (const Entity& e : entities) {
            if (inPoly) {
                if (e.type == "VERTEX") {
                    verts.push_back(e);
                    continue;
                }
                finishPolyline(poly, verts);
                inPoly = false;
                if (e.type == "SEQEND")
                    continue;
            }
            if (e.type == "POLYLINE") {
                inPoly = true;
                poly = e;
                verts.clear();
                continue;
            }
            entity(e);
        }
        if (inPoly)
            finishPolyline(poly, verts);

        if (m_inserts > 0)
            warn(m_warnings, std::to_string(m_inserts) +
                                 " block reference(s) (INSERT) ignored: explode the blocks before exporting");
        if (m_blocks > 0 && m_inserts == 0)
            warn(m_warnings, "The BLOCKS section was ignored");
        if (!m_ignored.empty()) {
            std::string msg = "Ignored DXF entities:";
            bool firstItem = true;
            for (const auto& [type, count] : m_ignored) {
                msg += (firstItem ? " " : ", ") + type + " (" + std::to_string(count) + ")";
                firstItem = false;
            }
            warn(m_warnings, msg);
        }
        return std::move(m_out);
    }

private:
    Vec2 pt(const Entity& e, int cx) const { return Vec2{e.get(cx), e.get(cx + 10)} * m_unit; }

    void push(std::vector<VectorShape>& shapes, bool mirror)
    {
        for (VectorShape& s : shapes) {
            if (mirror)
                mirrorX(s);
            m_out.push_back(s);
        }
    }

    void entity(const Entity& e)
    {
        std::vector<VectorShape> s;
        bool ocs = false;
        if (e.type == "LINE") {
            const Vec2 a = pt(e, 10), b = pt(e, 11);
            if ((b - a).length() > kTiny)
                s.push_back(mkLine(a, b));
        }
        else if (e.type == "CIRCLE") {
            ocs = true;
            const double r = e.get(40) * m_unit;
            if (r > kTiny)
                s.push_back(mkCircle(pt(e, 10), r));
        }
        else if (e.type == "ARC") {
            ocs = true;
            const double r = e.get(40) * m_unit;
            if (r > kTiny)
                s.push_back(mkArc(pt(e, 10), r, e.get(50) * kPi / 180.0, e.get(51) * kPi / 180.0));
        }
        else if (e.type == "LWPOLYLINE") {
            ocs = true;
            std::vector<PolyVertex> v;
            for (const Pair& p : e.g) {
                if (p.code == 10)
                    v.push_back({{toD(p.value) * m_unit, 0}, 0});
                else if (p.code == 20 && !v.empty())
                    v.back().p.y = toD(p.value) * m_unit;
                else if (p.code == 42 && !v.empty())
                    v.back().bulge = toD(p.value);
            }
            emitPolyline(v, (e.geti(70) & 1) != 0, s);
        }
        else if (e.type == "SPLINE") {
            spline(e, s);
        }
        else if (e.type == "ELLIPSE") {
            ellipse(e, s);
        }
        else if (e.type == "INSERT") {
            ++m_inserts;
        }
        else {
            ++m_ignored[e.type];
        }
        push(s, ocs && e.mirrored());
    }

    void finishPolyline(const Entity& poly, const std::vector<Entity>& verts)
    {
        const int flags = poly.geti(70);
        if (flags & (16 | 64)) {
            ++m_ignored["POLYLINE (mesh)"];
            return;
        }
        std::vector<PolyVertex> v;
        for (const Entity& e : verts) {
            if (e.geti(70) & 16)
                continue;  // spline frame control point
            v.push_back({pt(e, 10), e.get(42)});
        }
        std::vector<VectorShape> s;
        emitPolyline(v, (flags & 1) != 0, s);
        push(s, (flags & 8) == 0 && poly.mirrored());  // 3D polylines are in WCS
    }

    void spline(const Entity& e, std::vector<VectorShape>& out)
    {
        const int degree = e.geti(71, 3);
        std::vector<double> knots, weights;
        std::vector<Vec2> ctrl, fit;
        for (const Pair& p : e.g) {
            switch (p.code) {
                case 40: knots.push_back(toD(p.value)); break;
                case 41: weights.push_back(toD(p.value)); break;
                case 10: ctrl.push_back({toD(p.value) * m_unit, 0}); break;
                case 20:
                    if (!ctrl.empty())
                        ctrl.back().y = toD(p.value) * m_unit;
                    break;
                case 11: fit.push_back({toD(p.value) * m_unit, 0}); break;
                case 21:
                    if (!fit.empty())
                        fit.back().y = toD(p.value) * m_unit;
                    break;
                default: break;
            }
        }
        if (degree >= 1 && ctrl.size() >= static_cast<size_t>(degree) + 1 &&
            knots.size() == ctrl.size() + static_cast<size_t>(degree) + 1) {
            const size_t p = static_cast<size_t>(degree);
            if (weights.size() != ctrl.size())
                weights.assign(ctrl.size(), 1.0);
            for (size_t k = p; k < ctrl.size(); ++k) {
                if (!(knots[k + 1] - knots[k] > 1e-12))
                    continue;
                const Curve f = [&, p, k](double t) { return deBoor(p, k, t, knots, ctrl, weights); };
                flatten(f, knots[k], knots[k + 1], 2, m_tol, out);
            }
            return;
        }
        if (fit.size() >= 2) {
            warn(m_warnings, "A SPLINE without usable control points was approximated through its fit points");
            for (size_t i = 1; i < fit.size(); ++i)
                if ((fit[i] - fit[i - 1]).length() > kTiny)
                    out.push_back(mkLine(fit[i - 1], fit[i]));
            if ((e.geti(70) & 1) != 0 && (fit.back() - fit.front()).length() > kTiny)
                out.push_back(mkLine(fit.back(), fit.front()));
            return;
        }
        ++m_ignored["SPLINE (invalid)"];
    }

    void ellipse(const Entity& e, std::vector<VectorShape>& out)
    {
        const Vec2 c = pt(e, 10), major = pt(e, 11);
        const double ratio = e.get(40, 1.0);
        double t0 = e.get(41, 0.0), t1 = e.get(42, 2 * kPi);
        if (major.length() < kTiny || !(ratio > 0))
            return;
        Vec2 minor = perp(major) * ratio;
        if (e.mirrored())
            minor = minor * -1.0;  // the parameter runs around the extrusion direction
        const bool full = std::abs(t1 - t0) >= 2 * kPi - 1e-9 || (e.has(41) == false && e.has(42) == false);
        if (full && std::abs(ratio - 1.0) < 1e-9) {
            out.push_back(mkCircle(c, major.length()));
            return;
        }
        if (full)
            t1 = t0 + 2 * kPi;
        while (t1 <= t0)
            t1 += 2 * kPi;
        const Curve f = [c, major, minor](double t) { return c + major * std::cos(t) + minor * std::sin(t); };
        flatten(f, t0, t1, spansFor(t1 - t0), m_tol, out);
    }

    std::vector<std::string>* m_warnings;
    double m_tol;
    double m_unit = 1.0;
    int m_inserts = 0, m_blocks = 0;
    std::map<std::string, int> m_ignored;
    std::vector<VectorShape> m_out;
};

// ================================================================== import helpers

struct UnionFind
{
    std::vector<size_t> parent;
    explicit UnionFind(size_t n) : parent(n)
    {
        for (size_t i = 0; i < n; ++i)
            parent[i] = i;
    }
    size_t find(size_t i)
    {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    }
    void unite(size_t a, size_t b)
    {
        a = find(a);
        b = find(b);
        if (a != b)
            parent[std::max(a, b)] = std::min(a, b);  // smallest index = root: deterministic
    }
};

}  // namespace

// ================================================================== public API

std::vector<VectorShape> parseSvg(const std::string& text, std::vector<std::string>* warnings, double chordTolerance)
{
    std::vector<VectorShape> out;
    Emitter em(out, chordTolerance);
    XmlReader xml(text);

    struct State
    {
        Affine ctm;
        bool skip = false;
    };
    std::vector<State> stack;
    bool sawRoot = false;
    int texts = 0, uses = 0, images = 0, failed = 0;
    std::string firstError;

    XmlTag t;
    while (xml.next(t)) {
        if (t.closing) {
            if (!stack.empty())
                stack.pop_back();
            continue;
        }
        if (!sawRoot) {
            if (t.name != "svg")
                throw std::runtime_error("Not an SVG document (the root element is <" + t.name + ">)");
            sawRoot = true;
            Affine ctm = rootTransform(t);
            if (const std::string* tr = t.attr("transform"))
                ctm = ctm * parseTransform(*tr);
            if (!t.selfClosing)
                stack.push_back({ctm, hidden(t)});
            continue;
        }
        const State parent = stack.empty() ? State{} : stack.back();
        State st{parent.ctm, true};
        if (!parent.skip && !hidden(t)) {
            const std::string& n = t.name;
            if (isContainer(n) || isShape(n)) {
                try {
                    Affine ctm = parent.ctm;
                    if (n == "svg") {  // nested viewport: only its position is honoured
                        ctm = ctm * Affine{1, 0, 0, 1, userLength(t, "x", 0), userLength(t, "y", 0)};
                    }
                    if (const std::string* tr = t.attr("transform"))
                        ctm = ctm * parseTransform(*tr);
                    st.ctm = ctm;
                    st.skip = !isContainer(n);  // children of shapes (title, animate...) are ignored
                    if (isShape(n)) {
                        em.m = ctm;
                        emitShape(t, em);
                    }
                }
                catch (const std::exception& ex) {
                    ++failed;
                    if (firstError.empty())
                        firstError = ex.what();
                    st.skip = true;
                }
            }
            else if (n == "text") {
                ++texts;
            }
            else if (n == "use") {
                ++uses;
            }
            else if (n == "image") {
                ++images;
            }
            // defs, clipPath, mask, symbol, marker, pattern, style, metadata...: subtree skipped
        }
        if (!t.selfClosing)
            stack.push_back(st);
    }
    if (!sawRoot)
        throw std::runtime_error("Not an SVG document (no <svg> element)");
    if (texts > 0)
        warn(warnings, std::to_string(texts) + " <text> element(s) ignored (convert text to paths before importing)");
    if (uses > 0)
        warn(warnings, std::to_string(uses) + " <use> element(s) ignored (unlink clones before importing)");
    if (images > 0)
        warn(warnings, std::to_string(images) + " <image> element(s) ignored");
    if (failed > 0)
        warn(warnings, std::to_string(failed) + " element(s) could not be read: " + firstError);
    return out;
}

std::vector<VectorShape> parseDxf(const std::string& text, std::vector<std::string>* warnings, double chordTolerance)
{
    DxfReader r(warnings, chordTolerance);
    return r.read(text);
}

std::vector<VectorShape> loadVectorFile(const std::filesystem::path& file, std::vector<std::string>* warnings,
                                        double chordTolerance)
{
    const std::string ext = lower(file.extension().string());
    if (ext != ".svg" && ext != ".dxf")
        throw std::runtime_error("Unsupported file type '" + ext + "' (expected .svg or .dxf)");
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot open the file");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (trim(text).empty())
        throw std::runtime_error("The file is empty");
    if (ext == ".svg") {
        if (text.substr(0, 3) == "\xEF\xBB\xBF")
            text.erase(0, 3);
        return parseSvg(text, warnings, chordTolerance);
    }
    return parseDxf(text, warnings, chordTolerance);
}

ImportResult importShapes(Sketch& sk, std::vector<VectorShape> shapes, const ImportOptions& o)
{
    ImportResult res;
    shapes.erase(std::remove_if(shapes.begin(), shapes.end(),
                                [](const VectorShape& s) {
                                    return !finiteShape(s) || s.kind == GeoKind::Point ||
                                           (s.kind != GeoKind::Line && !(s.r > 0));
                                }),
                 shapes.end());
    const Box raw = boxOf(shapes);
    if (shapes.empty() || !raw.valid()) {
        res.error = "Nothing to import: the drawing contains no supported geometry";
        return res;
    }

    // ---- 1) placement: scale (or target width), rotation about the bbox center, offset
    double k = o.scale;
    if (o.targetWidth > 0) {
        const double w = raw.hi.x - raw.lo.x;
        if (w < kTiny) {
            res.error = "The drawing has no width: a target width cannot be applied";
            return res;
        }
        k = o.targetWidth / w;
    }
    if (!(k > 0) || !std::isfinite(k)) {
        res.error = "Invalid import scale";
        return res;
    }
    const Vec2 center = (raw.lo + raw.hi) * 0.5;
    const double ang = o.angleDeg * kPi / 180.0, ca = std::cos(ang), sa = std::sin(ang);
    auto xf = [&](Vec2 p) {
        const Vec2 q = (p - center) * k;
        return Vec2{ca * q.x - sa * q.y, sa * q.x + ca * q.y} + o.offset;
    };
    for (VectorShape& s : shapes) {
        if (s.kind == GeoKind::Line) {
            s.p1 = xf(s.p1);
            s.p2 = xf(s.p2);
        }
        else {
            s.c = xf(s.c);
            s.r *= k;
            s.a0 += ang;
            s.a1 += ang;
        }
    }

    // ---- 2) drop zero-length pieces
    const size_t before = shapes.size();
    shapes.erase(std::remove_if(shapes.begin(), shapes.end(),
                                [](const VectorShape& s) {
                                    switch (s.kind) {
                                        case GeoKind::Line: return (s.p2 - s.p1).length() < 1e-9;
                                        case GeoKind::Circle: return s.r < 1e-9;
                                        case GeoKind::Arc: return s.r < 1e-9 || (s.a1 - s.a0) * s.r < 1e-9;
                                        default: return true;
                                    }
                                }),
                 shapes.end());
    size_t dropped = before - shapes.size();

    // ---- 3) end point clusters (grid hash + union-find)
    struct End
    {
        size_t shape;
        int which;  // 0 = start, 1 = end
        Vec2 p;
    };
    std::vector<End> ends;
    std::vector<size_t> firstEnd(shapes.size(), std::numeric_limits<size_t>::max());
    for (size_t i = 0; i < shapes.size(); ++i) {
        const VectorShape& s = shapes[i];
        if (s.kind == GeoKind::Line) {
            firstEnd[i] = ends.size();
            ends.push_back({i, 0, s.p1});
            ends.push_back({i, 1, s.p2});
        }
        else if (s.kind == GeoKind::Arc) {
            firstEnd[i] = ends.size();
            ends.push_back({i, 0, s.c + polar(s.a0) * s.r});
            ends.push_back({i, 1, s.c + polar(s.a1) * s.r});
        }
    }
    const double tol = std::max(o.mergeTolerance, 1e-9);
    UnionFind uf(ends.size());
    {
        std::map<std::pair<long long, long long>, std::vector<size_t>> grid;
        for (size_t i = 0; i < ends.size(); ++i) {
            const long long gx = static_cast<long long>(std::floor(ends[i].p.x / tol));
            const long long gy = static_cast<long long>(std::floor(ends[i].p.y / tol));
            for (long long dx = -1; dx <= 1; ++dx)
                for (long long dy = -1; dy <= 1; ++dy) {
                    auto it = grid.find({gx + dx, gy + dy});
                    if (it == grid.end())
                        continue;
                    for (size_t j : it->second)
                        if ((ends[j].p - ends[i].p).length() <= tol)
                            uf.unite(i, j);
                }
            grid[{gx, gy}].push_back(i);
        }
    }

    // ---- 4) pieces collapsed by the merge: lines vanish, nearly closed arcs become circles
    std::vector<char> alive(shapes.size(), 1);
    for (size_t i = 0; i < shapes.size(); ++i) {
        if (firstEnd[i] == std::numeric_limits<size_t>::max())
            continue;
        if (uf.find(firstEnd[i]) != uf.find(firstEnd[i] + 1))
            continue;
        VectorShape& s = shapes[i];
        if (s.kind == GeoKind::Arc && s.a1 - s.a0 > kPi) {
            s.kind = GeoKind::Circle;
            firstEnd[i] = std::numeric_limits<size_t>::max();
        }
        else {
            alive[i] = 0;
            ++dropped;
        }
    }
    auto liveEnd = [&](size_t e) {
        const size_t s = ends[e].shape;
        return alive[s] && firstEnd[s] != std::numeric_limits<size_t>::max();
    };

    // ---- 5) cluster positions (mean of their live members) and snapping
    std::map<size_t, std::vector<size_t>> members;  // root -> end indices (ascending)
    for (size_t e = 0; e < ends.size(); ++e)
        if (liveEnd(e))
            members[uf.find(e)].push_back(e);
    std::vector<Vec2> pos(ends.size());
    for (const auto& entry : members) {
        const std::vector<size_t>& list = entry.second;
        Vec2 sum;
        for (size_t e : list)
            sum = sum + ends[e].p;
        const Vec2 mean = sum * (1.0 / static_cast<double>(list.size()));
        for (size_t e : list)
            pos[e] = mean;
    }
    for (size_t i = 0; i < shapes.size(); ++i) {
        if (!alive[i] || firstEnd[i] == std::numeric_limits<size_t>::max())
            continue;
        VectorShape& s = shapes[i];
        const Vec2 S = pos[firstEnd[i]], E = pos[firstEnd[i] + 1];
        if (s.kind == GeoKind::Line) {
            s.p1 = S;
            s.p2 = E;
        }
        else {
            // refit the arc through the snapped end points, center kept on the chord bisector
            const Vec2 chord = E - S;
            if (chord.length() < 1e-12) {
                alive[i] = 0;
                ++dropped;
                continue;
            }
            const Vec2 mid = (S + E) * 0.5, n = perp(chord).normalized();
            s.c = mid + n * n.dot(s.c - mid);
            s.r = (S - s.c).length();
            s.a0 = angleOf(S - s.c);
            s.a1 = normalizeEnd(s.a0, angleOf(E - s.c));
        }
    }
    // an arc killed above may still be listed: rebuild the member lists
    members.clear();
    for (size_t e = 0; e < ends.size(); ++e)
        if (liveEnd(e))
            members[uf.find(e)].push_back(e);

    // ---- 6) sketch geometry
    std::vector<SketchGeo> made;
    std::vector<size_t> geoIndex(shapes.size(), std::numeric_limits<size_t>::max());
    Box fin;
    for (size_t i = 0; i < shapes.size(); ++i) {
        if (!alive[i])
            continue;
        const VectorShape& s = shapes[i];
        SketchGeo g;
        g.id = newUuid();
        g.kind = s.kind;
        g.construction = o.construction;
        switch (s.kind) {
            case GeoKind::Line:
                g.p1 = s.p1;
                g.p2 = s.p2;
                ++res.lines;
                break;
            case GeoKind::Circle:
                g.c = s.c;
                g.r = s.r;
                ++res.circles;
                break;
            case GeoKind::Arc:
                g.c = s.c;
                g.r = s.r;
                g.a0 = s.a0;
                g.a1 = s.a1;
                ++res.arcs;
                break;
            default: continue;
        }
        fin.add(s);
        geoIndex[i] = made.size();
        made.push_back(g);
    }
    if (made.empty()) {
        res.error = "Nothing to import: every piece of the drawing is degenerate";
        return res;
    }
    res.bboxMin = fin.lo;
    res.bboxMax = fin.hi;
    for (const SketchGeo& g : made) {
        sk.geos.push_back(g);
        res.geos.push_back(g.id);
    }

    // ---- 7) constraints
    auto addC = [&](ConstraintKind kind, GeoRef a, GeoRef b, double value) {
        SketchConstraint c;
        c.id = newUuid();
        c.kind = kind;
        c.a = std::move(a);
        c.b = std::move(b);
        c.value = value;
        sk.constraints.push_back(c);
        res.constraints.push_back(c.id);
    };
    auto refOf = [&](size_t e) {
        const End& en = ends[e];
        return GeoRef{made[geoIndex[en.shape]].id, en.which == 0 ? PointPos::Start : PointPos::End};
    };
    auto pointOf = [&](size_t e) {
        const End& en = ends[e];
        return made[geoIndex[en.shape]].point(en.which == 0 ? PointPos::Start : PointPos::End);
    };
    for (const auto& entry : members)
        for (size_t m = 1; m < entry.second.size(); ++m) {
            addC(ConstraintKind::Coincident, refOf(entry.second.front()), refOf(entry.second[m]), 0);
            ++res.merged;
        }

    if (o.rigid) {
        // Rigid block: every point class, circle center (+ radius) and arc (one more scalar) is
        // dimensioned from a construction anchor point -> only the anchor's 2 DOF remain.
        // DistanceX/Y(a, b) = b - a (SketchSolver: addConstraintDifference(a, b, value)).
        SketchGeo anchor;
        anchor.id = newUuid();
        anchor.kind = GeoKind::Point;
        anchor.construction = true;
        anchor.p1 = (fin.lo + fin.hi) * 0.5;
        sk.geos.push_back(anchor);
        res.geos.push_back(anchor.id);
        res.anchor = anchor.id;
        const GeoRef A{anchor.id, PointPos::Start};
        auto pinX = [&](const GeoRef& r, Vec2 p) { addC(ConstraintKind::DistanceX, A, r, p.x - anchor.p1.x); };
        auto pinY = [&](const GeoRef& r, Vec2 p) { addC(ConstraintKind::DistanceY, A, r, p.y - anchor.p1.y); };

        for (const auto& entry : members) {
            const GeoRef r = refOf(entry.second.front());
            const Vec2 p = pointOf(entry.second.front());
            pinX(r, p);
            pinY(r, p);
        }
        for (const SketchGeo& g : made) {
            if (g.kind == GeoKind::Circle) {
                const GeoRef r{g.id, PointPos::Center};
                pinX(r, g.c);
                pinY(r, g.c);
                addC(ConstraintKind::Radius, GeoRef{g.id, PointPos::None}, GeoRef{}, g.r);
            }
            else if (g.kind == GeoKind::Arc) {
                // End points are already held by their point classes: one scalar is left (the
                // center sliding on the chord bisector). Radius fixes it unless the arc is close to
                // a half (or full) turn, where it is singular: then the center coordinate along
                // the bisector is dimensioned instead.
                if (std::abs(std::sin(g.a1 - g.a0)) >= 0.3) {
                    addC(ConstraintKind::Radius, GeoRef{g.id, PointPos::None}, GeoRef{}, g.r);
                }
                else {
                    const Vec2 chord = g.point(PointPos::End) - g.point(PointPos::Start);
                    const GeoRef r{g.id, PointPos::Center};
                    if (std::abs(chord.y) >= std::abs(chord.x))
                        pinX(r, g.c);  // bisector mostly horizontal
                    else
                        pinY(r, g.c);
                }
            }
        }
    }

    if (dropped > 0)
        res.warnings.push_back(std::to_string(dropped) + " degenerate piece(s) (shorter than the merge tolerance) dropped");
    res.ok = true;
    return res;
}

ImportResult importFile(Sketch& sk, const std::filesystem::path& file, const ImportOptions& o)
{
    ImportResult res;
    try {
        std::vector<std::string> warnings;
        double tol = std::max(o.chordTolerance, 1e-6);
        if (o.targetWidth <= 0 && o.scale > 0)
            tol /= o.scale;  // the tolerance applies to the final size
        std::vector<VectorShape> shapes = loadVectorFile(file, &warnings, tol);
        if (o.targetWidth > 0) {
            const Box b = boxOf(shapes);
            const double w = b.valid() ? b.hi.x - b.lo.x : 0;
            if (w > kTiny && o.targetWidth / w > 1.5) {  // enlarged a lot: re-discretise finer
                warnings.clear();
                shapes = loadVectorFile(file, &warnings, tol * w / o.targetWidth);
            }
        }
        res = importShapes(sk, std::move(shapes), o);
        res.warnings.insert(res.warnings.begin(), warnings.begin(), warnings.end());
    }
    catch (const std::exception& e) {
        res = ImportResult{};
        res.error = e.what();
    }
    return res;
}

}  // namespace silex::vectorimport
