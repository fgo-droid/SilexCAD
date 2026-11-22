#include "DrawingSheet.h"

#include <QDate>
#include <QFile>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QTextStream>
#include <algorithm>
#include <cmath>

#include "model/Math.h"

namespace silex::ui {

namespace {

const QColor kBlack(0x10, 0x12, 0x14);
const QColor kAxis(0xc8, 0x20, 0x20);
const QColor kDim(0x1f, 0x5f, 0xbf);
const QColor kHighlight(0xff, 0x7a, 0x00);
const QColor kGrey(0x6a, 0x70, 0x78);

QString fmt(double v)
{
    QString s = QString::number(v, 'f', 2);
    while (s.contains('.') && (s.endsWith('0') || s.endsWith('.')))
        s.chop(1);
    return s;
}

struct PenStyle
{
    double width;
    QColor color;
    std::vector<double> dash;  ///< mm (empty = solid)
};

PenStyle styleOf(SheetCanvas::Pen p)
{
    switch (p) {
        case SheetCanvas::Pen::Frame: return {0.7, kBlack, {}};
        case SheetCanvas::Pen::Thin: return {0.25, kBlack, {}};
        case SheetCanvas::Pen::Visible: return {0.5, kBlack, {}};
        case SheetCanvas::Pen::Hidden: return {0.25, kBlack, {3.0, 1.5}};
        case SheetCanvas::Pen::Axis: return {0.18, kAxis, {8.0, 1.5, 1.0, 1.5}};
        case SheetCanvas::Pen::Dimension: return {0.25, kDim, {}};
    }
    return {0.25, kBlack, {}};
}

double vAlignOffset(Qt::Alignment a, double size)
{
    if (a & Qt::AlignTop)
        return 0.8 * size;
    if (a & Qt::AlignBottom)
        return -0.2 * size;
    return 0.35 * size;  // vertical centre
}

// ---------------------------------------------------------------- QPainter backend

class PainterCanvas final : public SheetCanvas
{
public:
    explicit PainterCanvas(QPainter& p, std::optional<QColor> force = std::nullopt) : m_p(p), m_force(force) {}

    void polyline(const std::vector<QPointF>& pts, Pen pen) override
    {
        if (pts.size() < 2)
            return;
        const PenStyle st = styleOf(pen);
        QPen qp(m_force.value_or(st.color), st.width, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin);
        qp.setCosmetic(false);
        if (!st.dash.empty()) {
            QList<qreal> pattern;
            for (double d : st.dash)
                pattern << d / st.width;  // Qt dash patterns are in pen widths
            qp.setDashPattern(pattern);
        }
        m_p.setPen(qp);
        m_p.setBrush(Qt::NoBrush);
        m_p.drawPolyline(pts.data(), int(pts.size()));
    }

    void polygon(const std::vector<QPointF>& pts, const QColor& fill) override
    {
        m_p.setPen(Qt::NoPen);
        m_p.setBrush(m_force.value_or(fill));
        m_p.drawPolygon(pts.data(), int(pts.size()));
        m_p.setBrush(Qt::NoBrush);
    }

    void text(QPointF at, double sizeMm, const QString& s, const QColor& c, bool bold, Qt::Alignment align,
              double rot) override
    {
        m_p.save();
        m_p.translate(at);
        m_p.rotate(rot);
        const double k = sizeMm / 100.0;
        m_p.scale(k, k);
        QFont f("Arial");
        f.setPixelSize(100);
        f.setBold(bold);
        m_p.setFont(f);
        m_p.setPen(m_force.value_or(c));
        const double w = QFontMetricsF(f).horizontalAdvance(s);
        double x = 0;
        if (align & Qt::AlignHCenter)
            x = -w / 2;
        else if (align & Qt::AlignRight)
            x = -w;
        m_p.drawText(QPointF(x, vAlignOffset(align, 100.0)), s);
        m_p.restore();
    }

private:
    QPainter& m_p;
    std::optional<QColor> m_force;
};

// ---------------------------------------------------------------- SVG backend

QString xmlEsc(QString s)
{
    s.replace('&', "&amp;");
    s.replace('<', "&lt;");
    s.replace('>', "&gt;");
    s.replace('"', "&quot;");
    return s;
}

class SvgCanvas final : public SheetCanvas
{
public:
    QString out;

    void polyline(const std::vector<QPointF>& pts, Pen pen) override
    {
        if (pts.size() < 2)
            return;
        const PenStyle st = styleOf(pen);
        QString d;
        for (size_t i = 0; i < pts.size(); ++i)
            d += QString("%1%2,%3 ").arg(i ? "L" : "M").arg(pts[i].x(), 0, 'f', 3).arg(pts[i].y(), 0, 'f', 3);
        QString dash;
        if (!st.dash.empty()) {
            QStringList l;
            for (double x : st.dash)
                l << QString::number(x);
            dash = QString(" stroke-dasharray=\"%1\"").arg(l.join(","));
        }
        out += QString("<path d=\"%1\" fill=\"none\" stroke=\"%2\" stroke-width=\"%3\" stroke-linejoin=\"round\"%4/>\n")
                   .arg(d.trimmed(), st.color.name())
                   .arg(st.width)
                   .arg(dash);
    }

    void polygon(const std::vector<QPointF>& pts, const QColor& fill) override
    {
        QString d;
        for (const QPointF& p : pts)
            d += QString("%1,%2 ").arg(p.x(), 0, 'f', 3).arg(p.y(), 0, 'f', 3);
        out += QString("<polygon points=\"%1\" fill=\"%2\"/>\n").arg(d.trimmed(), fill.name());
    }

    void text(QPointF at, double size, const QString& s, const QColor& c, bool bold, Qt::Alignment align,
              double rot) override
    {
        const char* anchor = (align & Qt::AlignHCenter) ? "middle" : (align & Qt::AlignRight) ? "end" : "start";
        out += QString("<text transform=\"translate(%1,%2) rotate(%3)\" x=\"0\" y=\"%4\" font-family=\"Arial, "
                       "Helvetica, sans-serif\" font-size=\"%5\" fill=\"%6\" text-anchor=\"%7\"%8>%9</text>\n")
                   .arg(at.x(), 0, 'f', 3)
                   .arg(at.y(), 0, 'f', 3)
                   .arg(rot)
                   .arg(vAlignOffset(align, size), 0, 'f', 3)
                   .arg(size)
                   .arg(c.name(), anchor, bold ? " font-weight=\"bold\"" : "", xmlEsc(s));
    }
};

QPointF qp(Vec2 v) { return {v.x, v.y}; }

void arrow(SheetCanvas& c, QPointF tip, QPointF from, const QColor& col)
{
    QPointF d = tip - from;
    const double l = std::hypot(d.x(), d.y());
    if (l < 1e-9)
        return;
    d /= l;
    const QPointF n(-d.y(), d.x());
    const double len = 3.0, half = 0.55;
    c.polygon({tip, tip - d * len + n * half, tip - d * len - n * half}, col);
}

}  // namespace

// ================================================================== DrawingDim

double DrawingDim::value() const
{
    switch (type) {
        case Type::Diameter: return 2 * p2.x;
        case Type::Radius: return p2.x;
        case Type::Linear:
            if (orient == 'h')
                return std::abs(p2.x - p1.x);
            if (orient == 'v')
                return std::abs(p2.y - p1.y);
            return (p2 - p1).length();
    }
    return 0;
}

QString DrawingDim::text() const
{
    switch (type) {
        case Type::Diameter: return QString(QChar(0x2300)) + fmt(value());
        case Type::Radius: return "R" + fmt(value());
        default: return fmt(value());
    }
}

// ================================================================== settings

DrawingSettings DrawingSettings::fromJson(const Json& j)
{
    DrawingSettings s;
    if (!j.is_object())
        return s;
    s.sheet = j.value("sheet", s.sheet);
    s.scale = j.value("scale", s.scale);
    s.title = j.value("title", s.title);
    s.designer = j.value("designer", s.designer);
    s.date = j.value("date", s.date);
    s.material = j.value("material", s.material);
    for (const auto& jd : j.value("dims", Json::array())) {
        DrawingDim d;
        const std::string t = jd.value("type", std::string("linear"));
        d.type = t == "diameter" ? DrawingDim::Type::Diameter : t == "radius" ? DrawingDim::Type::Radius : DrawingDim::Type::Linear;
        d.view = jd.value("view", std::string("front"));
        const std::string o = jd.value("orient", std::string("a"));
        d.orient = o.empty() ? 'a' : o[0];
        d.p1 = jd.value("p1", Vec2{});
        d.p2 = jd.value("p2", Vec2{});
        d.pos = jd.value("pos", Vec2{});
        s.dims.push_back(d);
    }
    return s;
}

Json DrawingSettings::toJson() const
{
    Json jd = Json::array();
    for (const auto& d : dims) {
        const char* t = d.type == DrawingDim::Type::Diameter ? "diameter" : d.type == DrawingDim::Type::Radius ? "radius" : "linear";
        jd.push_back(Json{{"type", t}, {"view", d.view}, {"orient", std::string(1, d.orient)},
                          {"p1", d.p1}, {"p2", d.p2}, {"pos", d.pos}});
    }
    Json j{{"sheet", sheet}, {"scale", scale}, {"title", title}, {"dims", jd}};
    if (!designer.empty())
        j["designer"] = designer;
    if (!date.empty())
        j["date"] = date;
    if (!material.empty())
        j["material"] = material;
    return j;
}

// ================================================================== DrawingSheet

DrawingSheet::DrawingSheet(DrawingProjection proj, DrawingSettings settings)
    : m_proj(std::move(proj)), m_settings(std::move(settings))
{
    layout();
}

QSizeF DrawingSheet::paperSize() const { return m_settings.sheet == "A3" ? QSizeF(420, 297) : QSizeF(297, 210); }

QString DrawingSheet::scaleText() const
{
    if (m_scale >= 1)
        return QString("%1:1").arg(fmt(m_scale));
    return QString("1:%1").arg(fmt(1.0 / m_scale));
}

QRectF DrawingSheet::titleBlock() const
{
    const QSizeF ps = paperSize();
    const QRectF frame(20, 10, ps.width() - 30, ps.height() - 20);
    return QRectF(frame.right() - 180, frame.bottom() - 36, 180, 36);
}

const DrawingSheet::ViewPlacement* DrawingSheet::viewNamed(const std::string& name) const
{
    for (const auto& v : m_views)
        if (v.view && v.view->name == name)
            return &v;
    return nullptr;
}

const DrawingSheet::ViewPlacement* DrawingSheet::viewAt(QPointF paper) const
{
    for (const auto& v : m_views)
        if (v.paper.adjusted(-15, -15, 15, 15).contains(paper))
            return &v;
    return nullptr;
}

QPointF DrawingSheet::toPaper(const ViewPlacement& v, Vec2 p) const
{
    return {v.origin.x() + (p.x - v.view->min.x) * m_scale, v.origin.y() + (v.view->max.y - p.y) * m_scale};
}

Vec2 DrawingSheet::toView(const ViewPlacement& v, QPointF paper) const
{
    return {v.view->min.x + (paper.x() - v.origin.x()) / m_scale, v.view->max.y - (paper.y() - v.origin.y()) / m_scale};
}

void DrawingSheet::layout()
{
    m_views.clear();
    const DrawingViewData *front = nullptr, *top = nullptr, *right = nullptr;
    for (const auto& v : m_proj.views) {
        if (v.name == "front") front = &v;
        if (v.name == "top") top = &v;
        if (v.name == "right") right = &v;
    }
    if (!front || !top || !right)
        return;
    const QSizeF ps = paperSize();
    const QRectF frame(20, 10, ps.width() - 30, ps.height() - 20);
    const QRectF zone(frame.left() + 8, frame.top() + 8, frame.width() - 16, frame.height() - 16 - 36);
    const double gap = 24;
    const double W = right->width() + front->width(), H = front->height() + top->height();

    static const std::vector<std::pair<std::string, double>> scales{
        {"10:1", 10}, {"5:1", 5}, {"2:1", 2}, {"1:1", 1}, {"1:2", 0.5}, {"1:5", 0.2}, {"1:10", 0.1}, {"1:20", 0.05}, {"1:50", 0.02}, {"1:100", 0.01}};
    m_scale = 0;
    for (const auto& [name, s] : scales)
        if (m_settings.scale == name)
            m_scale = s;
    if (m_scale == 0) {
        m_scale = scales.back().second;
        for (const auto& [name, s] : scales)
            if (W * s + gap <= zone.width() && H * s + gap <= zone.height()) {
                m_scale = s;
                break;
            }
    }
    const double s = m_scale;
    const double left = zone.center().x() - (W * s + gap) / 2;
    const double topY = zone.center().y() - (H * s + gap) / 2;
    // first-angle (European): right view on the LEFT of the front view, top view BELOW it
    const QRectF rR(left, topY, right->width() * s, right->height() * s);
    const QRectF rF(rR.right() + gap, topY, front->width() * s, front->height() * s);
    const QRectF rT(rF.left(), rF.bottom() + gap, top->width() * s, top->height() * s);
    m_views.push_back({front, rF, rF.topLeft()});
    m_views.push_back({top, rT, rT.topLeft()});
    m_views.push_back({right, rR, rR.topLeft()});
}

std::optional<Vec2> DrawingSheet::snapPoint(const ViewPlacement& v, QPointF paper, double tol) const
{
    std::optional<Vec2> best;
    double bestD = tol;
    auto consider = [&](Vec2 q) {
        const QPointF pq = toPaper(v, q);
        const double d = std::hypot(pq.x() - paper.x(), pq.y() - paper.y());
        if (d < bestD) {
            bestD = d;
            best = q;
        }
    };
    for (const Vec2& q : v.view->vertices)
        consider(q);
    for (const auto& c : v.view->circles)
        consider(c.center);
    return best;
}

std::optional<DrawingCircle> DrawingSheet::snapCircle(const ViewPlacement& v, QPointF paper, double tol) const
{
    std::optional<DrawingCircle> best;
    double bestD = tol;
    for (const auto& c : v.view->circles) {
        const QPointF pc = toPaper(v, c.center);
        const double d = std::abs(std::hypot(pc.x() - paper.x(), pc.y() - paper.y()) - c.radius * m_scale);
        if (d < bestD) {
            bestD = d;
            best = c;
        }
    }
    return best;
}

void DrawingSheet::resnapDimensions()
{
    const double tol = 2.0;  // model mm
    for (DrawingDim& d : m_settings.dims) {
        const ViewPlacement* v = viewNamed(d.view);
        if (!v)
            continue;
        if (d.type == DrawingDim::Type::Linear) {
            for (Vec2* p : {&d.p1, &d.p2}) {
                double bestD = tol;
                Vec2 best = *p;
                for (const Vec2& q : v->view->vertices)
                    if ((q - *p).length() < bestD) {
                        bestD = (q - *p).length();
                        best = q;
                    }
                for (const auto& c : v->view->circles)
                    if ((c.center - *p).length() < bestD) {
                        bestD = (c.center - *p).length();
                        best = c.center;
                    }
                *p = best;
            }
        }
        else {
            double bestD = tol;
            for (const auto& c : v->view->circles)
                if ((c.center - d.p1).length() < bestD) {
                    bestD = (c.center - d.p1).length();
                    d.p1 = c.center;
                    d.p2 = {c.radius, 0};
                }
        }
    }
}

int DrawingSheet::dimAt(QPointF paper, double tol) const
{
    for (size_t i = 0; i < m_settings.dims.size(); ++i) {
        const DrawingDim& d = m_settings.dims[i];
        const ViewPlacement* v = viewNamed(d.view);
        if (!v)
            continue;
        const QPointF p = toPaper(*v, d.pos);
        if (std::hypot(p.x() - paper.x(), p.y() - paper.y()) < tol + 2)
            return int(i);
    }
    return -1;
}

void DrawingSheet::renderDim(SheetCanvas& c, const DrawingDim& d, bool hl) const
{
    const ViewPlacement* v = viewNamed(d.view);
    if (!v)
        return;
    const QColor col = hl ? kHighlight : kDim;
    const QPointF pos = toPaper(*v, d.pos);
    const double ts = 3.5;
    if (d.type == DrawingDim::Type::Linear) {
        const QPointF P1 = toPaper(*v, d.p1), P2 = toPaper(*v, d.p2);
        QPointF A, B;
        QPointF u;
        if (d.orient == 'h') {
            A = {P1.x(), pos.y()};
            B = {P2.x(), pos.y()};
        }
        else if (d.orient == 'v') {
            A = {pos.x(), P1.y()};
            B = {pos.x(), P2.y()};
        }
        else {
            QPointF dd = P2 - P1;
            const double l = std::max(std::hypot(dd.x(), dd.y()), 1e-9);
            dd /= l;
            const QPointF n(-dd.y(), dd.x());
            const double t = QPointF::dotProduct(pos - P1, n);
            A = P1 + n * t;
            B = P2 + n * t;
        }
        // extension lines (1 mm gap at the part, 2 mm overshoot)
        auto ext = [&](QPointF from, QPointF to) {
            QPointF dd = to - from;
            const double l = std::hypot(dd.x(), dd.y());
            if (l < 1e-6)
                return;
            dd /= l;
            c.polyline({from + dd * 1.0, to + dd * 2.0}, SheetCanvas::Pen::Dimension);
        };
        ext(P1, A);
        ext(P2, B);
        c.polyline({A, B}, SheetCanvas::Pen::Dimension);
        arrow(c, A, B, col);
        arrow(c, B, A, col);
        QPointF m = (A + B) / 2;
        double ang = std::atan2(B.y() - A.y(), B.x() - A.x()) * 180.0 / kPi;
        if (ang > 90.01) ang -= 180;
        if (ang < -90.01) ang += 180;
        const double rad = ang * kPi / 180.0;
        const QPointF up(std::sin(rad), -std::cos(rad));  // normal pointing "up" in reading direction
        c.text(m + up * 1.0, ts, d.text(), col, false, Qt::AlignHCenter | Qt::AlignBottom, ang);
        return;
    }
    const QPointF C = toPaper(*v, d.p1);
    const double R = d.p2.x * m_scale;
    QPointF dir = pos - C;
    double l = std::hypot(dir.x(), dir.y());
    if (l < 1e-6) {
        dir = {1, 0};
        l = 1;
    }
    dir /= l;
    const QPointF onCircle = C + dir * R;
    if (d.type == DrawingDim::Type::Diameter) {
        const QPointF opposite = C - dir * R;
        const QPointF end = l > R ? pos : onCircle;
        c.polyline({opposite, end}, SheetCanvas::Pen::Dimension);
        arrow(c, onCircle, C, col);
        arrow(c, opposite, C, col);
    }
    else {
        const QPointF end = l > R ? pos : onCircle;
        c.polyline({C, end}, SheetCanvas::Pen::Dimension);
        arrow(c, onCircle, C, col);
    }
    const bool rightSide = dir.x() >= 0;
    if (l > R)
        c.polyline({pos, pos + QPointF(rightSide ? 6 : -6, 0)}, SheetCanvas::Pen::Dimension);
    c.text(pos + QPointF(rightSide ? 1 : -1, -0.8), ts, d.text(), col, false,
           (rightSide ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignBottom);
}

void DrawingSheet::render(SheetCanvas& c, int highlightDim, const DrawingDim* preview) const
{
    const QSizeF ps = paperSize();
    c.polygon({{0, 0}, {ps.width(), 0}, {ps.width(), ps.height()}, {0, ps.height()}}, Qt::white);
    const QRectF frame(20, 10, ps.width() - 30, ps.height() - 20);
    auto rect = [&](const QRectF& r, SheetCanvas::Pen pen) {
        c.polyline({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft(), r.topLeft()}, pen);
    };
    rect(frame, SheetCanvas::Pen::Frame);
    // centring marks
    c.polyline({{frame.center().x(), 5}, {frame.center().x(), frame.top()}}, SheetCanvas::Pen::Frame);
    c.polyline({{frame.center().x(), ps.height() - 5}, {frame.center().x(), frame.bottom()}}, SheetCanvas::Pen::Frame);

    // ---- views
    for (const ViewPlacement& vp : m_views) {
        for (const DrawingCurve& cv : vp.view->curves) {
            std::vector<QPointF> pts;
            pts.reserve(cv.pts.size());
            for (const Vec2& q : cv.pts)
                pts.push_back(toPaper(vp, q));
            c.polyline(pts, cv.kind == DrawLineKind::Visible  ? SheetCanvas::Pen::Visible
                            : cv.kind == DrawLineKind::Hidden ? SheetCanvas::Pen::Hidden
                                                              : SheetCanvas::Pen::Axis);
        }
        c.text({vp.paper.center().x(), vp.paper.bottom() + 7}, 3.0, QString::fromStdString(vp.view->label), kGrey, false,
               Qt::AlignHCenter | Qt::AlignTop);
    }

    // ---- dimensions
    for (size_t i = 0; i < m_settings.dims.size(); ++i)
        renderDim(c, m_settings.dims[i], int(i) == highlightDim);
    if (preview)
        renderDim(c, *preview, true);

    // ---- title block
    const QRectF tb = titleBlock();
    rect(tb, SheetCanvas::Pen::Frame);
    auto cell = [&](double x, double y, double w, double h, const QString& label, const QString& value, double size,
                    bool bold = false) {
        const QRectF r(tb.left() + x, tb.top() + y, w, h);
        rect(r, SheetCanvas::Pen::Thin);
        c.text({r.left() + 1.5, r.top() + 1.2}, 2.0, label, kGrey, false, Qt::AlignLeft | Qt::AlignTop);
        c.text({r.left() + 2, r.bottom() - 1.8}, size, value, kBlack, bold, Qt::AlignLeft | Qt::AlignBottom);
    };
    const QString date = m_settings.date.empty() ? QDate::currentDate().toString("dd/MM/yyyy")
                                                 : QString::fromStdString(m_settings.date);
    cell(0, 0, 110, 14, QObject::tr("Titre"), QString::fromStdString(m_settings.title), 5.0, true);
    cell(110, 0, 70, 14, QObject::tr("Logiciel / projection"), "Silex v0.3", 3.0);
    cell(0, 14, 40, 11, QObject::tr("Échelle"), scaleText(), 3.5, true);
    cell(40, 14, 25, 11, QObject::tr("Unité"), "mm", 3.5);
    cell(65, 14, 45, 11, QObject::tr("Date"), date, 3.5);
    cell(110, 14, 70, 11, QObject::tr("Dessinateur"), QString::fromStdString(m_settings.designer), 3.5);
    cell(0, 25, 55, 11, QObject::tr("Volume"), QString("%1 mm³").arg(m_proj.volume, 0, 'f', 1), 3.5);
    cell(55, 25, 55, 11, QObject::tr("Masse estimée"), QString("%1 g").arg(m_proj.mass, 0, 'f', 2), 3.5);
    cell(110, 25, 70, 11, QObject::tr("Matériau"), QString::fromStdString(m_settings.material), 3.5);
    // first-angle projection symbol (ISO 128): truncated cone, end view on the right
    const QPointF o(tb.left() + 153, tb.top() + 8.5);
    c.polyline({o + QPointF(-6, -1.6), o + QPointF(0, -3.2), o + QPointF(0, 3.2), o + QPointF(-6, 1.6), o + QPointF(-6, -1.6)},
               SheetCanvas::Pen::Thin);
    auto circle = [&](QPointF ctr, double r) {
        std::vector<QPointF> pts;
        for (int k = 0; k <= 36; ++k)
            pts.push_back(ctr + QPointF(r * std::cos(k * kPi / 18), r * std::sin(k * kPi / 18)));
        c.polyline(pts, SheetCanvas::Pen::Thin);
    };
    circle(o + QPointF(9, 0), 3.2);
    circle(o + QPointF(9, 0), 1.6);
    c.text({tb.left() + 166, tb.top() + 8.5}, 2.0, QObject::tr("1er dièdre"), kGrey, false,
           Qt::AlignLeft | Qt::AlignVCenter);
}

void DrawingSheet::paint(QPainter& p, int highlightDim, const DrawingDim* preview) const
{
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    PainterCanvas c(p);
    render(c, highlightDim, preview);
}

bool DrawingSheet::exportPdf(const QString& path, QString* error) const
{
    QPdfWriter w(path);
    w.setPageSize(QPageSize(m_settings.sheet == "A3" ? QPageSize::A3 : QPageSize::A4));
    w.setPageOrientation(QPageLayout::Landscape);
    w.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Millimeter);
    w.setResolution(1200);
    w.setTitle(QString::fromStdString(m_settings.title));
    w.setCreator("Silex v0.3");
    QPainter p;
    if (!p.begin(&w)) {
        if (error)
            *error = QObject::tr("Cannot write %1").arg(path);
        return false;
    }
    const double k = w.resolution() / 25.4;
    p.scale(k, k);
    paint(p);
    p.end();
    return QFile::exists(path);
}

QString DrawingSheet::toSvg() const
{
    SvgCanvas c;
    render(c);
    const QSizeF ps = paperSize();
    return QString("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%1mm\" height=\"%2mm\" viewBox=\"0 0 %1 %2\">\n"
                   "<title>%3</title>\n<desc>Generated by Silex v0.3</desc>\n%4</svg>\n")
        .arg(ps.width())
        .arg(ps.height())
        .arg(xmlEsc(QString::fromStdString(m_settings.title)), c.out);
}

bool DrawingSheet::exportSvg(const QString& path, QString* error) const
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    const QByteArray data = toSvg().toUtf8();
    return f.write(data) == data.size();
}

}  // namespace silex::ui
