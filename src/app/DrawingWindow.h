#pragma once
// v0.3 Lot 3.2 — DrawingView (Ctrl+D): A4 / A3 landscape sheet in a QGraphicsView with the three
// HLR views, interactive 2D dimensions snapped on projected vertices and arcs, editable title
// block, automatic update when the 3D model changes, PDF / SVG export.

#include <QMainWindow>
#include <memory>
#include <optional>
#include <string>

#include "DrawingSheet.h"

class QComboBox;
class QGraphicsScene;
class QLineEdit;
class QTimer;
class QLabel;

namespace silex::ui {

class ModelController;
class SheetView;
class SheetItem;

class DrawingWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit DrawingWindow(ModelController* model, QWidget* parent = nullptr);
    ~DrawingWindow() override;

    enum class Tool { Select, Linear, Diameter, Radius };

    // called by the sheet view (scene coordinates = paper mm)
    void sheetPress(QPointF paper, Qt::KeyboardModifiers mods);
    void sheetMove(QPointF paper, Qt::KeyboardModifiers mods);
    void sheetKey(int key);
    const DrawingSheet* sheet() const { return m_sheet.get(); }
    int highlighted() const { return m_selected; }
    const DrawingDim* previewDim() const { return m_preview ? &*m_preview : nullptr; }
    std::optional<QPointF> snapMarker() const { return m_snapMarker; }

protected:
    void showEvent(QShowEvent* e) override;

private:
    void buildUi();
    void setTool(Tool t);
    void reproject(bool force = false);
    void rebuildSheet();
    void commitSettings();
    void loadSettingsFromDoc();
    void exportAs(const QString& format);
    void fitSheet();
    void updateStatus(const QString& msg = {});

    ModelController* m_model;
    DrawingSettings m_settings;
    DrawingProjection m_proj;
    std::string m_projectedKey;  ///< what the current projection was computed from
    std::unique_ptr<DrawingSheet> m_sheet;

    QGraphicsScene* m_scene = nullptr;
    SheetView* m_view = nullptr;
    SheetItem* m_item = nullptr;
    QComboBox* m_sheetBox = nullptr;
    QComboBox* m_scaleBox = nullptr;
    QLineEdit* m_title = nullptr;
    QLineEdit* m_designer = nullptr;
    QLineEdit* m_date = nullptr;
    QLineEdit* m_material = nullptr;
    QLabel* m_status = nullptr;
    QTimer* m_refresh = nullptr;

    Tool m_tool = Tool::Select;
    int m_step = 0;
    DrawingDim m_pending;
    std::optional<DrawingDim> m_preview;
    std::optional<QPointF> m_snapMarker;
    int m_selected = -1;
    bool m_loading = false;
};

}  // namespace silex::ui
