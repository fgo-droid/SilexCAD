#include "DrawingWindow.h"

#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QWheelEvent>
#include <cmath>

#include "Icons.h"
#include "ModelController.h"
#include "kernel/KernelEngine.h"

namespace silex::ui {

// ------------------------------------------------------------------ graphics item & view

class SheetItem final : public QGraphicsItem
{
public:
    explicit SheetItem(DrawingWindow* w) : m_w(w) {}
    QRectF boundingRect() const override
    {
        const DrawingSheet* s = m_w->sheet();
        const QSizeF sz = s ? s->paperSize() : QSizeF(297, 210);
        return QRectF(QPointF(0, 0), sz).adjusted(-2, -2, 2, 2);
    }
    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override
    {
        const DrawingSheet* s = m_w->sheet();
        if (!s)
            return;
        // drop shadow
        p->fillRect(QRectF(QPointF(1.5, 1.5), s->paperSize()), QColor(0, 0, 0, 90));
        s->paint(*p, m_w->highlighted(), m_w->previewDim());
        if (auto m = m_w->snapMarker()) {
            p->setPen(QPen(QColor(0xff, 0x7a, 0x00), 0.35));
            p->setBrush(Qt::NoBrush);
            p->drawRect(QRectF(*m - QPointF(1.2, 1.2), QSizeF(2.4, 2.4)));
        }
    }
    void refresh() { prepareGeometryChange(); update(); }

private:
    DrawingWindow* m_w;
};

class SheetView final : public QGraphicsView
{
public:
    SheetView(QGraphicsScene* s, DrawingWindow* w) : QGraphicsView(s), m_w(w)
    {
        setRenderHint(QPainter::Antialiasing);
        setMouseTracking(true);
        setBackgroundBrush(QColor(0x3a, 0x3f, 0x47));
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setFocusPolicy(Qt::StrongFocus);
    }

protected:
    void wheelEvent(QWheelEvent* e) override
    {
        const double k = std::pow(1.0015, e->angleDelta().y());
        scale(k, k);
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::MiddleButton) {
            m_pan = true;
            m_last = e->position();
            setCursor(Qt::ClosedHandCursor);
            return;
        }
        if (e->button() == Qt::LeftButton)
            m_w->sheetPress(mapToScene(e->position().toPoint()), e->modifiers());
        if (e->button() == Qt::RightButton)
            m_w->sheetKey(Qt::Key_Escape);
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (m_pan) {
            const QPointF d = e->position() - m_last;
            m_last = e->position();
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() - int(d.x()));
            verticalScrollBar()->setValue(verticalScrollBar()->value() - int(d.y()));
            return;
        }
        m_w->sheetMove(mapToScene(e->position().toPoint()), e->modifiers());
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::MiddleButton) {
            m_pan = false;
            unsetCursor();
        }
    }
    void keyPressEvent(QKeyEvent* e) override { m_w->sheetKey(e->key()); }

private:
    DrawingWindow* m_w;
    bool m_pan = false;
    QPointF m_last;
};

// ------------------------------------------------------------------ window

DrawingWindow::DrawingWindow(ModelController* model, QWidget* parent) : QMainWindow(parent), m_model(model)
{
    setWindowTitle(tr("Drawing — Silex"));
    resize(1200, 820);
    buildUi();
    loadSettingsFromDoc();
    m_refresh = new QTimer(this);
    m_refresh->setSingleShot(true);
    m_refresh->setInterval(250);
    connect(m_refresh, &QTimer::timeout, this, [this] { reproject(); });
    connect(m_model, &ModelController::snapshotChanged, this, [this] {
        if (isVisible() && !m_model->busy())
            m_refresh->start();
    });
    connect(m_model, &ModelController::documentChanged, this, [this] {
        if (isVisible())
            loadSettingsFromDoc();
    });
}

DrawingWindow::~DrawingWindow() = default;

void DrawingWindow::buildUi()
{
    m_scene = new QGraphicsScene(this);
    m_item = new SheetItem(this);
    m_scene->addItem(m_item);
    m_view = new SheetView(m_scene, this);
    setCentralWidget(m_view);

    auto* bar = addToolBar(tr("Drawing"));
    bar->setIconSize(QSize(22, 22));
    bar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    m_sheetBox = new QComboBox(this);
    m_sheetBox->addItem(tr("A4 landscape"), "A4");
    m_sheetBox->addItem(tr("A3 landscape"), "A3");
    bar->addWidget(new QLabel(tr(" Sheet "), this));
    bar->addWidget(m_sheetBox);
    m_scaleBox = new QComboBox(this);
    for (const char* s : {"auto", "5:1", "2:1", "1:1", "1:2", "1:5", "1:10", "1:20"})
        m_scaleBox->addItem(QString(s) == "auto" ? tr("Auto") : QString(s), s);
    bar->addWidget(new QLabel(tr("  Scale "), this));
    bar->addWidget(m_scaleBox);
    bar->addSeparator();
    connect(m_sheetBox, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_loading)
            return;
        m_settings.sheet = m_sheetBox->currentData().toString().toStdString();
        commitSettings();
        fitSheet();
    });
    connect(m_scaleBox, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_loading)
            return;
        m_settings.scale = m_scaleBox->currentData().toString().toStdString();
        commitSettings();
    });

    auto* group = new QActionGroup(this);
    auto tool = [&](IconId ic, const QString& text, Tool t, const QString& key) {
        QAction* a = bar->addAction(icon(ic), text);
        a->setCheckable(true);
        a->setToolTip(QString("%1 (%2)").arg(text, key));
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, t] { setTool(t); });
        if (t == Tool::Select)
            a->setChecked(true);
        return a;
    };
    tool(IconId::Fit, tr("Select"), Tool::Select, "Esc");
    tool(IconId::Measure, tr("Linear"), Tool::Linear, "L");
    tool(IconId::Circle, tr("Diameter"), Tool::Diameter, "D");
    tool(IconId::Arc, tr("Radius"), Tool::Radius, "R");
    bar->addSeparator();
    QAction* del = bar->addAction(icon(IconId::Cancel), tr("Delete dim."));
    connect(del, &QAction::triggered, this, [this] { sheetKey(Qt::Key_Delete); });
    QAction* fit = bar->addAction(icon(IconId::Fit), tr("Fit"));
    connect(fit, &QAction::triggered, this, [this] { fitSheet(); });
    QAction* upd = bar->addAction(icon(IconId::Redo), tr("Update"));
    connect(upd, &QAction::triggered, this, [this] { reproject(true); });
    bar->addSeparator();
    QAction* pdf = bar->addAction(icon(IconId::Drawing), tr("Export PDF"));
    connect(pdf, &QAction::triggered, this, [this] { exportAs("pdf"); });
    QAction* svg = bar->addAction(icon(IconId::Drawing), tr("Export SVG"));
    connect(svg, &QAction::triggered, this, [this] { exportAs("svg"); });

    // title block editor
    auto* dock = new QDockWidget(tr("Title block"), this);
    dock->setFeatures(QDockWidget::DockWidgetMovable);
    auto* w = new QWidget(dock);
    auto* form = new QFormLayout(w);
    m_title = new QLineEdit(w);
    m_designer = new QLineEdit(w);
    m_date = new QLineEdit(w);
    m_date->setPlaceholderText(tr("today"));
    m_material = new QLineEdit(w);
    form->addRow(tr("Title"), m_title);
    form->addRow(tr("Designer"), m_designer);
    form->addRow(tr("Date"), m_date);
    form->addRow(tr("Material"), m_material);
    auto* help = new QLabel(tr("<small>Wheel: zoom · middle button: pan<br>"
                               "Linear: click 2 points then the dimension position (Shift: aligned)<br>"
                               "Diameter / radius: click a circle then the text position<br>"
                               "Click a dimension then Delete to remove it</small>"),
                            w);
    help->setWordWrap(true);
    help->setStyleSheet("color: #9aa3ad;");
    form->addRow(help);
    dock->setWidget(w);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    for (QLineEdit* e : {m_title, m_designer, m_date, m_material})
        connect(e, &QLineEdit::editingFinished, this, [this] {
            if (m_loading)
                return;
            m_settings.title = m_title->text().toStdString();
            m_settings.designer = m_designer->text().toStdString();
            m_settings.date = m_date->text().toStdString();
            m_settings.material = m_material->text().toStdString();
            commitSettings();
        });

    m_status = new QLabel(this);
    statusBar()->addWidget(m_status, 1);
}

void DrawingWindow::showEvent(QShowEvent* e)
{
    QMainWindow::showEvent(e);
    loadSettingsFromDoc();
    reproject(true);
    QTimer::singleShot(0, this, [this] { fitSheet(); });
}

void DrawingWindow::loadSettingsFromDoc()
{
    const DocState& d = m_model->doc();
    DrawingSettings s;
    if (d.drawing.is_object()) {
        s = DrawingSettings::fromJson(d.drawing);
    }
    else {
        s = m_settings.dims.empty() && m_settings.title == "Pièce" ? DrawingSettings{} : m_settings;
        if (!m_model->filePath().isEmpty())
            s.title = QFileInfo(m_model->filePath()).completeBaseName().toStdString();
        if (auto snap = m_model->snapshot(); snap && !snap->bodies.empty())
            s.material = snap->bodies.front().material.name;
    }
    m_loading = true;
    m_settings = s;
    m_sheetBox->setCurrentIndex(std::max(0, m_sheetBox->findData(QString::fromStdString(s.sheet))));
    m_scaleBox->setCurrentIndex(std::max(0, m_scaleBox->findData(QString::fromStdString(s.scale))));
    m_title->setText(QString::fromStdString(s.title));
    m_designer->setText(QString::fromStdString(s.designer));
    m_date->setText(QString::fromStdString(s.date));
    m_material->setText(QString::fromStdString(s.material));
    m_loading = false;
    rebuildSheet();
}

void DrawingWindow::commitSettings()
{
    rebuildSheet();
    m_loading = true;
    m_model->execute(SetDrawingCommand(m_settings.toJson()));
    m_loading = false;
}

void DrawingWindow::reproject(bool force)
{
    // re-project only when the geometry (features, marker) changed — not for drawing edits
    DocState geo = m_model->doc();
    geo.drawing = Json();
    const std::string key = docToJson(geo).dump();
    if (!force && key == m_projectedKey)
        return;
    m_projectedKey = key;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_proj = KernelEngine::projectDrawing(m_model->doc());
    QApplication::restoreOverrideCursor();
    rebuildSheet();
    if (m_sheet && !m_settings.dims.empty()) {
        m_sheet->resnapDimensions();  // dimensions follow the modified geometry
        m_settings.dims = m_sheet->settings().dims;
    }
    updateStatus(m_proj.error.empty() ? tr("Projection updated") : QString::fromStdString(m_proj.error));
}

void DrawingWindow::rebuildSheet()
{
    m_sheet = std::make_unique<DrawingSheet>(m_proj, m_settings);
    m_item->refresh();
    m_scene->setSceneRect(QRectF(QPointF(-20, -20), m_sheet->paperSize() + QSizeF(40, 40)));
    updateStatus();
}

void DrawingWindow::fitSheet()
{
    if (m_sheet)
        m_view->fitInView(QRectF(QPointF(-5, -5), m_sheet->paperSize() + QSizeF(10, 10)), Qt::KeepAspectRatio);
}

void DrawingWindow::updateStatus(const QString& msg)
{
    if (!m_sheet)
        return;
    QString s = tr("Scale %1 · %n dimension(s)", nullptr, int(m_settings.dims.size())).arg(m_sheet->scaleText());
    if (!msg.isEmpty())
        s += " · " + msg;
    m_status->setText(s);
}

void DrawingWindow::setTool(Tool t)
{
    m_tool = t;
    m_step = 0;
    m_preview.reset();
    m_snapMarker.reset();
    m_item->update();
    switch (t) {
        case Tool::Linear: updateStatus(tr("Linear dimension: click the first point")); break;
        case Tool::Diameter: updateStatus(tr("Diameter: click a circle")); break;
        case Tool::Radius: updateStatus(tr("Radius: click an arc or a circle")); break;
        default: updateStatus(); break;
    }
}

void DrawingWindow::sheetKey(int key)
{
    if (key == Qt::Key_Escape) {
        if (m_tool != Tool::Select && m_step > 0)
            setTool(m_tool);
        else
            setTool(Tool::Select);
        m_selected = -1;
        m_item->update();
    }
    else if ((key == Qt::Key_Delete || key == Qt::Key_Backspace) && m_selected >= 0 &&
             m_selected < int(m_settings.dims.size())) {
        m_settings.dims.erase(m_settings.dims.begin() + m_selected);
        m_selected = -1;
        commitSettings();
    }
    else if (key == Qt::Key_L)
        setTool(Tool::Linear);
    else if (key == Qt::Key_D)
        setTool(Tool::Diameter);
    else if (key == Qt::Key_R)
        setTool(Tool::Radius);
}

void DrawingWindow::sheetMove(QPointF paper, Qt::KeyboardModifiers mods)
{
    if (!m_sheet)
        return;
    m_snapMarker.reset();
    const double tol = 3.0;
    if (m_tool == Tool::Linear && m_step < 2) {
        const DrawingSheet::ViewPlacement* v = m_step == 0 ? m_sheet->viewAt(paper) : m_sheet->viewNamed(m_pending.view);
        if (v)
            if (auto q = m_sheet->snapPoint(*v, paper, tol))
                m_snapMarker = m_sheet->toPaper(*v, *q);
    }
    else if ((m_tool == Tool::Diameter || m_tool == Tool::Radius) && m_step == 0) {
        if (const auto* v = m_sheet->viewAt(paper))
            if (auto c = m_sheet->snapCircle(*v, paper, tol))
                m_snapMarker = m_sheet->toPaper(*v, c->center);
    }
    else if (m_step >= 1 && m_tool != Tool::Select) {
        const auto* v = m_sheet->viewNamed(m_pending.view);
        if (v) {
            DrawingDim d = m_pending;
            d.pos = m_sheet->toView(*v, paper);
            if (d.type == DrawingDim::Type::Linear) {
                const QPointF a = m_sheet->toPaper(*v, d.p1), b = m_sheet->toPaper(*v, d.p2);
                const QPointF mid = (a + b) / 2;
                if (mods & Qt::ShiftModifier)
                    d.orient = 'a';
                else if (std::abs(a.x() - b.x()) < 1e-6)
                    d.orient = 'v';
                else if (std::abs(a.y() - b.y()) < 1e-6)
                    d.orient = 'h';
                else
                    d.orient = std::abs(paper.y() - mid.y()) > std::abs(paper.x() - mid.x()) ? 'h' : 'v';
            }
            m_preview = d;
        }
    }
    m_item->update();
}

void DrawingWindow::sheetPress(QPointF paper, Qt::KeyboardModifiers mods)
{
    if (!m_sheet)
        return;
    const double tol = 3.0;
    if (m_tool == Tool::Select) {
        m_selected = m_sheet->dimAt(paper, tol);
        m_item->update();
        updateStatus(m_selected >= 0 ? tr("Dimension selected — Delete to remove") : QString());
        return;
    }
    if (m_tool == Tool::Linear && m_step < 2) {
        const DrawingSheet::ViewPlacement* v = m_step == 0 ? m_sheet->viewAt(paper) : m_sheet->viewNamed(m_pending.view);
        if (!v)
            return;
        const Vec2 q = m_sheet->snapPoint(*v, paper, tol).value_or(m_sheet->toView(*v, paper));
        if (m_step == 0) {
            m_pending = DrawingDim{};
            m_pending.type = DrawingDim::Type::Linear;
            m_pending.view = v->view->name;
            m_pending.p1 = q;
            m_step = 1;
            updateStatus(tr("Click the second point"));
        }
        else {
            if ((q - m_pending.p1).length() < 1e-9)
                return;
            m_pending.p2 = q;
            m_step = 2;
            updateStatus(tr("Click to place the dimension (Shift: aligned)"));
            sheetMove(paper, mods);
        }
        return;
    }
    if ((m_tool == Tool::Diameter || m_tool == Tool::Radius) && m_step == 0) {
        const auto* v = m_sheet->viewAt(paper);
        if (!v)
            return;
        auto c = m_sheet->snapCircle(*v, paper, tol);
        if (!c) {
            updateStatus(tr("No circle here"));
            return;
        }
        m_pending = DrawingDim{};
        m_pending.type = m_tool == Tool::Diameter ? DrawingDim::Type::Diameter : DrawingDim::Type::Radius;
        m_pending.view = v->view->name;
        m_pending.p1 = c->center;
        m_pending.p2 = {c->radius, 0};
        m_step = 1;
        updateStatus(tr("Click to place the text"));
        sheetMove(paper, mods);
        return;
    }
    // final click: place the dimension
    if (m_preview) {
        m_settings.dims.push_back(*m_preview);
        m_preview.reset();
        m_step = 0;
        commitSettings();
        updateStatus(tr("Dimension added"));
    }
}

void DrawingWindow::exportAs(const QString& format)
{
    if (!m_sheet)
        return;
    QString base = QString::fromStdString(m_settings.title);
    if (!m_model->filePath().isEmpty())
        base = QFileInfo(m_model->filePath()).absolutePath() + "/" + base;
    const QString filter = format == "pdf" ? tr("PDF (*.pdf)") : tr("SVG (*.svg)");
    QString path = QFileDialog::getSaveFileName(this, tr("Export drawing"), base + "." + format, filter);
    if (path.isEmpty())
        return;
    if (!path.endsWith("." + format, Qt::CaseInsensitive))
        path += "." + format;
    QString err;
    const bool ok = format == "pdf" ? m_sheet->exportPdf(path, &err) : m_sheet->exportSvg(path, &err);
    if (!ok)
        QMessageBox::critical(this, tr("Export failed"), err.isEmpty() ? path : err);
    else {
        updateStatus(tr("Exported %1").arg(path));
        m_model->noteEvent("drawing_exported");  // v0.4 lot 3: lesson criterion
    }
}

}  // namespace silex::ui
