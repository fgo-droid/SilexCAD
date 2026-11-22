#pragma once
// v0.3 Lot 3.2 — technical drawing sheet: layout of the 3 HLR views (first-angle projection),
// ISO-like frame and editable title block, 2D dimensions (linear, diameter, radius) and export to
// PDF (QPdfWriter) and SVG. Shared by the interactive DrawingWindow and the MCP export_drawing.
//
// Units: the sheet is expressed in paper millimetres (origin top-left, y down). Views are in model
// millimetres (x right, y up). Dimensions are stored in view coordinates (model mm), so they stay
// meaningful whatever the scale, and are re-snapped to the new geometry when the model changes.

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <optional>
#include <string>
#include <vector>

#include "model/DrawingData.h"
#include "model/Json.h"

class QPainter;

namespace silex::ui {

struct DrawingDim
{
    enum class Type { Linear, Diameter, Radius } type = Type::Linear;
    std::string view;   ///< "front" | "top" | "right"
    char orient = 'a';  ///< linear: 'h' horizontal, 'v' vertical, 'a' aligned
    Vec2 p1, p2;        ///< linear: measured points; diameter/radius: p1 = centre, p2.x = radius
    Vec2 pos;           ///< dimension line / text location (view coordinates)

    double value() const;
    QString text() const;
};

struct DrawingSettings
{
    std::string sheet = "A4";     ///< "A4" | "A3" (landscape)
    std::string scale = "auto";   ///< "auto" | "2:1" | "1:1" | "1:2" | "1:5" | "1:10" | "1:20"
    std::string title = "Pièce";
    std::string designer;
    std::string date;             ///< empty: today
    std::string material;         ///< shown in the title block
    std::vector<DrawingDim> dims;

    static DrawingSettings fromJson(const Json& j);
    Json toJson() const;
};

/// Abstract 2D output (QPainter or SVG).
class SheetCanvas
{
public:
    enum class Pen { Frame, Thin, Visible, Hidden, Axis, Dimension };
    virtual ~SheetCanvas() = default;
    virtual void polyline(const std::vector<QPointF>& pts, Pen pen) = 0;
    virtual void polygon(const std::vector<QPointF>& pts, const QColor& fill) = 0;
    virtual void text(QPointF at, double sizeMm, const QString& s, const QColor& c, bool bold, Qt::Alignment align,
                      double rotationDeg = 0) = 0;
};

class DrawingSheet
{
public:
    struct ViewPlacement
    {
        const DrawingViewData* view = nullptr;
        QRectF paper;    ///< footprint of the view on the sheet (mm)
        QPointF origin;  ///< paper point of the view coordinate (min.x, max.y)
    };

    DrawingSheet(DrawingProjection proj, DrawingSettings settings);

    QSizeF paperSize() const;
    double scale() const { return m_scale; }
    QString scaleText() const;
    const DrawingSettings& settings() const { return m_settings; }
    DrawingSettings& settings() { return m_settings; }
    const DrawingProjection& projection() const { return m_proj; }
    const std::vector<ViewPlacement>& placements() const { return m_views; }
    QRectF titleBlock() const;

    QPointF toPaper(const ViewPlacement& v, Vec2 p) const;
    Vec2 toView(const ViewPlacement& v, QPointF paper) const;
    const ViewPlacement* viewAt(QPointF paper) const;
    const ViewPlacement* viewNamed(const std::string& name) const;

    /// Nearest snap point (vertex or circle centre) within `tolPaper` mm on the sheet.
    std::optional<Vec2> snapPoint(const ViewPlacement& v, QPointF paper, double tolPaper) const;
    /// Circle under the cursor (distance to the circumference within `tolPaper`).
    std::optional<DrawingCircle> snapCircle(const ViewPlacement& v, QPointF paper, double tolPaper) const;
    /// Re-attaches the dimensions to the current projection (after a model change).
    void resnapDimensions();

    void render(SheetCanvas& c, int highlightDim = -1, const DrawingDim* preview = nullptr) const;
    void paint(QPainter& p, int highlightDim = -1, const DrawingDim* preview = nullptr) const;  ///< 1 unit = 1 mm
    /// Dimension whose text / line is near `paper` (index) or -1.
    int dimAt(QPointF paper, double tolPaper) const;

    bool exportPdf(const QString& path, QString* error = nullptr) const;
    bool exportSvg(const QString& path, QString* error = nullptr) const;
    QString toSvg() const;

private:
    void layout();
    void renderDim(SheetCanvas& c, const DrawingDim& d, bool highlight) const;

    DrawingProjection m_proj;
    DrawingSettings m_settings;
    double m_scale = 1.0;
    std::vector<ViewPlacement> m_views;
};

}  // namespace silex::ui
