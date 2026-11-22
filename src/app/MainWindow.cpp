#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QCheckBox>
#include <QDockWidget>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QScrollArea>
#include <QStatusBar>
#include <QTimer>
#include <QPainter>
#include <QToolBar>
#include <QToolButton>
#include <QActionGroup>
#include <cmath>
#include <QLocale>
#include <functional>
#include <tuple>
#include <QMenu>
#include <QVBoxLayout>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QSlider>
#include <QDialog>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QLineEdit>
#include <QSettings>
#include <QDesktopServices>
#include <QDir>
#include <QUrl>

#include "AboutDialog.h"
#include "AutosaveManager.h"
#include "ComponentLibrary.h"
#include "RichTips.h"
#include "LessonRecorder.h"
#include "model/Expression.h"
#include "DrawingWindow.h"
#include "ExprSpinBox.h"
#include "ParametersDialog.h"
#include "SectionPanel.h"

#include "Icons.h"
#include "MarkingMenu.h"
#include "ModelController.h"
#include "SketchEditor.h"
#include "LearnPanel.h"
#include "Theme.h"
#include "Timeline.h"
#include "Viewport.h"
#include "Workspace.h"
#include "kernel/KernelEngine.h"

namespace silex::ui {

MainWindow::MainWindow()
{
    m_model = new ModelController(this);
    m_autosave = new AutosaveManager(m_model, this);
    // numeric fields evaluate expressions against the document's global parameters
    ExprSpinBox::setParameterSource([this] {
        auto values = std::make_shared<std::map<std::string, double>>(parameterValues(m_model->doc().parameters));
        return ParameterLookup([values](const std::string& n) -> std::optional<double> {
            auto it = values->find(n);
            if (it == values->end())
                return std::nullopt;
            return it->second;
        });
    });
    buildUi();
    buildActions();
    refreshTitle();
    rebuildDefaultInspector();
}

MainWindow::~MainWindow()
{
    // The workspace owns editing sessions that reference the viewport.
    delete m_ws;
    m_ws = nullptr;
}

void MainWindow::buildUi()
{
    m_vp = new Viewport(this);
    setCentralWidget(m_vp);
    m_ws = new Workspace(m_model, m_vp, this);

    // Timeline (bottom, always visible)
    m_timeline = new Timeline(m_model, this);
    auto* tdock = new QDockWidget(this);
    tdock->setObjectName("timeline");
    tdock->setTitleBarWidget(new QWidget(tdock));
    tdock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    tdock->setWidget(m_timeline);
    addDockWidget(Qt::BottomDockWidgetArea, tdock);

    // Inspector (right)
    m_inspector = new QDockWidget(tr("Inspector"), this);
    m_inspector->setObjectName("inspector");
    m_inspector->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    m_inspectorArea = new QScrollArea(m_inspector);
    m_inspectorArea->setWidgetResizable(true);
    m_inspectorArea->setFrameShape(QFrame::NoFrame);
    m_inspectorArea->setMinimumWidth(290);
    m_inspector->setWidget(m_inspectorArea);
    addDockWidget(Qt::RightDockWidgetArea, m_inspector);

    // ---- status bar (ergonomics lot): MCP | bodies & errors | triangles | compute time | snap
    m_mcpLabel = new QLabel(this);
    m_mcpLabel->setTextFormat(Qt::RichText);
    statusBar()->addPermanentWidget(m_mcpLabel);
    m_statusRight = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusRight);
    m_triLabel = new QLabel(this);
    m_triLabel->setToolTip(tr("Triangles of the current tessellation (density before an STL / 3MF export)"));
    statusBar()->addPermanentWidget(m_triLabel);
    m_msLabel = new QLabel(this);
    m_msLabel->setToolTip(tr("Duration of the last model recompute"));
    statusBar()->addPermanentWidget(m_msLabel);

    // magnetic grid: click toggles it, the arrow chooses Off / 0.5 / 1 / 2 / 5 mm (Alt bypasses it)
    m_snapButton = new QToolButton(this);
    m_snapButton->setPopupMode(QToolButton::MenuButtonPopup);
    m_snapButton->setAutoRaise(true);
    m_snapButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_snapButton->setMinimumWidth(m_snapButton->fontMetrics().horizontalAdvance(tr("Snap: %1 mm").arg(QLocale().toString(0.5))) + 30);
    m_snapButton->setToolTip(tr("Magnetic grid of the sketcher — click to toggle, arrow to choose the step "
                                "(hold Alt while drawing to bypass it)"));
    {
        auto* menu = new QMenu(m_snapButton);
        auto* group = new QActionGroup(menu);
        auto addStep = [&](const QString& text, double step) {
            QAction* act = menu->addAction(text);
            act->setCheckable(true);
            act->setData(step);
            group->addAction(act);
            connect(act, &QAction::triggered, this, [this, step] {
                if (step > 0) {
                    m_lastSnapStep = step;
                    m_ws->setGridStep(step);
                }
                m_ws->setGridSnap(step > 0);
                updateSnapButton();
                m_vp->refreshOverlay();
            });
        };
        addStep(tr("Off"), 0);
        for (double v : {0.5, 1.0, 2.0, 5.0})
            addStep(QString("%1 mm").arg(QLocale().toString(v)), v);
        m_snapButton->setMenu(menu);
        connect(m_snapButton, &QToolButton::clicked, this, [this] {
            const bool on = !m_ws->gridSnap();
            m_ws->setGridStep(m_lastSnapStep);
            m_ws->setGridSnap(on);
            updateSnapButton();
            m_vp->refreshOverlay();
        });
    }
    statusBar()->addPermanentWidget(m_snapButton);
    updateSnapButton();

    // v0.5 lot 4: build plate (click toggles it, the arrow chooses the printer)
    {
        auto* bed = new QToolButton(this);
        bed->setPopupMode(QToolButton::MenuButtonPopup);
        bed->setAutoRaise(true);
        bed->setCheckable(true);
        bed->setIcon(icon(IconId::Bed));
        bed->setText(tr("Bed"));
        bed->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        QSettings st;
        Workspace::Bed b;
        b.on = st.value("bed/on", false).toBool();
        b.name = st.value("bed/name", b.name).toString();
        b.size = {st.value("bed/x", 220.0).toDouble(), st.value("bed/y", 220.0).toDouble(), st.value("bed/z", 250.0).toDouble()};
        m_ws->setBed(b);
        auto refresh = [this, bed] {
            const auto& cur = m_ws->bed();
            bed->setChecked(cur.on);
            bed->setToolTip(tr("Build plate %1 (%2 × %3 × %4 mm) — click to show / hide, arrow to choose the printer")
                                .arg(cur.name).arg(cur.size.x).arg(cur.size.y).arg(cur.size.z));
            QSettings s;
            s.setValue("bed/on", cur.on);
            s.setValue("bed/name", cur.name);
            s.setValue("bed/x", cur.size.x);
            s.setValue("bed/y", cur.size.y);
            s.setValue("bed/z", cur.size.z);
        };
        auto* menu = new QMenu(bed);
        auto preset = [this, menu, refresh](const QString& name, double x, double y, double z) {
            connect(menu->addAction(QString("%1  (%2 × %3 × %4)").arg(name).arg(x).arg(y).arg(z)), &QAction::triggered, this,
                    [this, refresh, name, x, y, z] {
                        Workspace::Bed b = m_ws->bed();
                        b.on = true;
                        b.name = name;
                        b.size = {x, y, z};
                        m_ws->setBed(b);
                        refresh();
                    });
        };
        preset("Prusa MK4 / Ender 3", 220, 220, 250);
        preset("Bambu Lab P1 / X1", 256, 256, 256);
        preset("Prusa Mini", 180, 180, 180);
        menu->addSeparator();
        connect(menu->addAction(tr("Custom size...")), &QAction::triggered, this, [this, refresh] {
            Workspace::Bed b = m_ws->bed();
            bool ok = false;
            const QString txt = QInputDialog::getText(this, tr("Build volume"), tr("Width × depth × height (mm):"),
                                                      QLineEdit::Normal,
                                                      QString("%1 x %2 x %3").arg(b.size.x).arg(b.size.y).arg(b.size.z), &ok);
            if (!ok)
                return;
            const QStringList parts = QString(txt).replace(QChar(0xD7), 'x').split('x', Qt::SkipEmptyParts);
            if (parts.size() != 3)
                return;
            b.size = {parts[0].trimmed().toDouble(), parts[1].trimmed().toDouble(), parts[2].trimmed().toDouble()};
            if (b.size.x <= 0 || b.size.y <= 0 || b.size.z <= 0)
                return;
            b.on = true;
            b.name = tr("Custom");
            m_ws->setBed(b);
            refresh();
        });
        bed->setMenu(menu);
        connect(bed, &QToolButton::clicked, this, [this, refresh] {
            Workspace::Bed b = m_ws->bed();
            b.on = !b.on;
            m_ws->setBed(b);
            refresh();
        });
        connect(m_ws, &Workspace::bedChanged, this, [this, refresh] {
            m_ws->setBed(m_ws->bed());
            refresh();
        });
        refresh();
        statusBar()->addPermanentWidget(bed);
    }
    setMcpState(McpState::Off);
    connect(m_model, &ModelController::tutorModeChanged, this, [this] { setMcpState(m_mcpState, m_mcpDetail); });

    // v0.4 lot 3: "Learn / Guided mode" panel (left, toggled by the Learn button / F1)
    m_learnDock = new QDockWidget(tr("Learn / Guided mode"), this);
    m_learnDock->setObjectName("learn");
    m_learnDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    m_learn = new LearnPanel(m_model, m_ws, m_learnDock);
    auto* learnScroll = new QScrollArea(m_learnDock);
    learnScroll->setWidgetResizable(true);
    learnScroll->setFrameShape(QFrame::NoFrame);
    learnScroll->setWidget(m_learn);
    learnScroll->setMinimumWidth(300);
    m_learnDock->setWidget(learnScroll);
    addDockWidget(Qt::LeftDockWidgetArea, m_learnDock);
    m_learnDock->hide();
    connect(m_learn, &LearnPanel::hintRequested, this, &MainWindow::pulseTool);
    connect(m_learn, &LearnPanel::openRepairRequested, this, [this](const QString& path) {
        if (maybeSave())
            openFile(path);
    });
    connect(m_learn, &LearnPanel::openExampleRequested, this, [this](const QString& path) {
        if (maybeSave() && openFile(path))
            m_learn->startTour();
    });

    // v0.3 section view panel (hidden until Alt+S)
    m_sectionDock = new QDockWidget(tr("Section view"), this);
    m_sectionDock->setObjectName("section");
    m_sectionDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    m_section = new SectionPanel(m_ws, m_model, m_sectionDock);
    m_sectionDock->setWidget(m_section);
    addDockWidget(Qt::RightDockWidgetArea, m_sectionDock);
    m_sectionDock->hide();
    connect(m_ws, &Workspace::sectionToggleRequested, this, [this] { toggleSection(); });
    connect(m_ws, &Workspace::visibilityChanged, this, [this] {
        if (!m_ws->hasSession())
            rebuildDefaultInspector();
    });

    connect(m_timeline, &Timeline::editRequested, m_ws, &Workspace::editFeature);
    connect(m_ws, &Workspace::sessionChanged, this, &MainWindow::onSessionChanged);
    connect(m_ws, &Workspace::selectionChanged, this, [this] {
        if (!m_ws->hasSession())
            rebuildDefaultInspector();
    });
    connect(m_ws, &Workspace::statusMessage, this,
            [this](const QString& m, int t) { statusBar()->showMessage(m, t); });
    connect(m_ws, &Workspace::markingMenuRequested, this,
            [this](QPoint pos) { MarkingMenu::popup(m_vp, pos, m_ws->contextActions()); });
    connect(m_model, &ModelController::documentChanged, this, &MainWindow::refreshTitle);
    connect(m_model, &ModelController::busyChanged, this, [this](bool busy) {
        if (busy)
            m_statusRight->setText(tr("Computing..."));
    });
    connect(m_model, &ModelController::snapshotChanged, this, [this] {
        auto snap = m_model->snapshot();
        int errors = 0, warnings = 0;
        for (const auto& r : snap->results) {
            errors += r.status == FeatureStatus::Error;
            warnings += r.status == FeatureStatus::Warning;
        }
        QString s = tr("%n body(ies)", nullptr, int(snap->bodies.size()));
        size_t triangles = 0;
        for (const auto& b : snap->bodies)
            triangles += b.mesh.indices.size() / 3;
        m_triLabel->setText(tr("%1 triangles").arg(QLocale().toString(qulonglong(triangles))));
        m_msLabel->setText(tr("compute %1 ms").arg(snap->computeMs, 0, 'f', 0));
        if (errors)
            s += QString("  <span style='color:%1'>%2 error(s)</span>").arg(Colors::Error.name()).arg(errors);
        if (warnings)
            s += QString("  <span style='color:%1'>%2 warning(s)</span>").arg(Colors::Warning.name()).arg(warnings);
        m_statusRight->setText(s);
        if (m_firstFit && snap->bbox.valid()) {
            m_firstFit = false;
            m_vp->fitAll(false);
        }
        if (!m_ws->hasSession())
            rebuildDefaultInspector();
        m_undo->setEnabled(m_ws->hasSession() || m_model->history().canUndo());
        m_redo->setEnabled(m_ws->hasSession() || m_model->history().canRedo());
    });
}

void MainWindow::buildActions()
{
    auto* file = addToolBar(tr("File"));
    file->setObjectName("fileBar");
    file->setIconSize(QSize(22, 22));
    auto add = [&](QToolBar* bar, IconId ic, const QString& text, const QKeySequence& ks, auto fn) {
        QAction* a = new QAction(icon(ic), text, this);
        if (!ks.isEmpty()) {
            a->setShortcut(ks);
            a->setShortcutContext(Qt::WindowShortcut);
            a->setToolTip(QString("%1 (%2)").arg(text, ks.toString(QKeySequence::NativeText)));
        }
        connect(a, &QAction::triggered, this, fn);
        if (bar)
            bar->addAction(a);
        return a;
    };
    add(file, IconId::New, tr("New"), QKeySequence::New, [this] { doNew(); });
    add(file, IconId::Open, tr("Open..."), QKeySequence::Open, [this] { doOpen(); });
    add(file, IconId::Save, tr("Save"), QKeySequence::Save, [this] { doSave(false); });
    QAction* saveAs = add(nullptr, IconId::Save, tr("Save as..."), QKeySequence::SaveAs, [this] { doSave(true); });
    addAction(saveAs);
    file->addSeparator();
    add(file, IconId::ImportStep, tr("Import STEP..."), QKeySequence(tr("Ctrl+I")), [this] { doImport(); });
    {
        // Export ▾ (STEP / STL / 3MF): keeps the file bar short on 1366 px screens
        auto* exp = new QToolButton(file);
        exp->setIcon(icon(IconId::ExportStep));
        exp->setToolTip(tr("Export (STEP Ctrl+E · STL Ctrl+Shift+E · 3MF Ctrl+3)"));
        exp->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(exp);
        menu->setToolTipsVisible(true);
        menu->addAction(add(nullptr, IconId::ExportStep, tr("Export STEP..."), QKeySequence(tr("Ctrl+E")), [this] { doExport(); }));
        menu->addAction(add(nullptr, IconId::ExportStl, tr("Export STL..."), QKeySequence(tr("Ctrl+Shift+E")), [this] { doExportStl(); }));
        menu->addAction(add(nullptr, IconId::Export3mf, tr("Export 3MF..."), QKeySequence(tr("Ctrl+3")), [this] { doExport3mf(); }));
        for (QAction* a : menu->actions())
            addAction(a);  // shortcuts stay active although the actions live in a menu
        exp->setMenu(menu);
        file->addWidget(exp);
    }
    file->addSeparator();
    m_undo = add(file, IconId::Undo, tr("Undo"), QKeySequence::Undo, [this] { m_ws->undo(); });
    m_redo = add(file, IconId::Redo, tr("Redo"), QKeySequence(tr("Ctrl+Y")), [this] { m_ws->redo(); });
    m_undo->setEnabled(false);
    m_redo->setEnabled(false);
    file->addSeparator();
    file->addWidget(buildHelpButton(file));

    auto* model = addToolBar(tr("Model"));
    model->setObjectName("modelBar");
    model->setIconSize(QSize(24, 24));
    model->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    model->setStyleSheet("QToolButton { padding: 1px 2px; } QToolButton::menu-indicator { image: none; width: 0; }");
    auto tool = [&](IconId ic, const QString& text, const QString& key, auto fn) {
        QAction* a = new QAction(icon(ic), text, this);
        a->setToolTip(key.isEmpty() ? text : QString("%1 (%2)").arg(text, key));
        connect(a, &QAction::triggered, this, fn);
        model->addAction(a);
        return a;
    };
    using Item = std::tuple<IconId, QString, QString, std::function<void()>>;
    // Grouped drop-downs (no overflow on 1366 px): "Create ▾" etc. Entries show their shortcut,
    // which keeps working directly from the keyboard.
    auto group = [&](IconId ic, const QString& text, std::vector<Item> items) {
        auto* b = new QToolButton(model);
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setPopupMode(QToolButton::InstantPopup);
        b->setIcon(icon(ic));
        b->setText(text + QString::fromUtf8(" \u25BE"));
        QStringList tips;
        auto* menu = new QMenu(b);
        menu->setToolTipsVisible(true);
        for (const auto& [eic, etext, key, fn] : items) {
            QAction* a = menu->addAction(icon(eic), key.isEmpty() ? etext : etext + "\t" + key);
            a->setToolTip(key.isEmpty() ? etext : QString("%1 (%2)").arg(etext, key));
            a->setData(etext);  // plain name (Hint pulse, tutorials)
            connect(a, &QAction::triggered, this, [fn = fn] { fn(); });
            tips << a->toolTip();
        }
        b->setToolTip(QString("<b>%1</b><br>%2").arg(text, tips.join("<br>")));
        b->setMenu(menu);
        b->setProperty("richTool", text);
        return model->addWidget(b);
    };
    // v0.5 lot 3: "Essential" profile = 7 fundamental commands; "Complete" = everything
    QList<QAction*> pro, essentialOnly;
    tool(IconId::Sketch, tr("Sketch"), "S", [this] { m_ws->startSketch(); });
    pro << group(IconId::Box, tr("Create"),
          {{IconId::Box, tr("Box"), "Shift+B", [this] { m_ws->startFeature(FeatureType::Box); }},
           {IconId::Cylinder, tr("Cylinder"), "Shift+Y", [this] { m_ws->startFeature(FeatureType::Cylinder); }},
           {IconId::Rib, tr("Rib"), "", [this] { m_ws->startFeature(FeatureType::Rib); }},
           {IconId::Hole, tr("Hole / Insert"), "Shift+H", [this] { m_ws->startFeature(FeatureType::FastenerHole); }},
           {IconId::Text, tr("Text (Emboss / Engrave)"), "Shift+T", [this] { m_ws->startFeature(FeatureType::EmbossText); }},
           {IconId::Component, tr("Insert Component"), "", [this] { insertComponent(); }},
           {IconId::Enclosure, tr("Enclosure Wizard"), "", [this] { m_ws->startEnclosureWizard(); }}});
    tool(IconId::Extrude, tr("Extrude"), "E", [this] { m_ws->startFeature(FeatureType::Extrude); });
    tool(IconId::PressPull, tr("Press/Pull"), "Q", [this] { m_ws->startPressPull(); });
    // Essential-only entries (the Complete profile has them in its drop-downs)
    essentialOnly << group(IconId::Fillet, tr("Fillet / Chamfer"),
                           {{IconId::Fillet, tr("Fillet"), "F", [this] { m_ws->startFeature(FeatureType::Fillet); }},
                            {IconId::Chamfer, tr("Chamfer"), "C", [this] { m_ws->startFeature(FeatureType::Chamfer); }}});
    essentialOnly << tool(IconId::Hole, tr("Hole / Insert"), "Shift+H", [this] { m_ws->startFeature(FeatureType::FastenerHole); });
    essentialOnly << tool(IconId::Measure, tr("Measure"), "I", [this] { m_ws->startMeasure(); });
    essentialOnly << group(IconId::ExportStl, tr("Export STL/3MF"),
                           {{IconId::ExportStl, tr("Export STL..."), "Ctrl+Shift+E", [this] { doExportStl(); }},
                            {IconId::Export3mf, tr("Export 3MF..."), "Ctrl+3", [this] { doExport3mf(); }}});
    pro << tool(IconId::Revolve, tr("Revolve"), "R", [this] { m_ws->startFeature(FeatureType::Revolve); });
    pro << tool(IconId::Sweep, tr("Sweep"), "Shift+W", [this] { m_ws->startFeature(FeatureType::Sweep); });
    pro << tool(IconId::Loft, tr("Loft"), "Shift+L", [this] { m_ws->startFeature(FeatureType::Loft); });
    pro << model->addSeparator();
    pro << group(IconId::Fillet, tr("Modify"),
          {{IconId::Fillet, tr("Fillet"), "F", [this] { m_ws->startFeature(FeatureType::Fillet); }},
           {IconId::Chamfer, tr("Chamfer"), "C", [this] { m_ws->startFeature(FeatureType::Chamfer); }},
           {IconId::Shell, tr("Shell"), "H", [this] { m_ws->startFeature(FeatureType::Shell); }},
           {IconId::SplitBody, tr("Split Body"), "", [this] { m_ws->startFeature(FeatureType::SplitBody); }},
           {IconId::DeleteFace, tr("Delete Face"), tr("Del"), [this] { m_ws->startDeleteFace(); }},
           {IconId::MoveBody, tr("Move/Copy"), "", [this] { m_ws->startFeature(FeatureType::TransformBody); }},
           {IconId::Align, tr("Align"), "", [this] { m_ws->startAlign("face"); }},
           {IconId::LayFlat, tr("Lay Flat on Bed"), "", [this] { m_ws->startLayFlat(); }}});
    pro << group(IconId::LinearPattern, tr("Pattern"),
          {{IconId::Mirror, tr("Mirror"), "M", [this] { m_ws->startFeature(FeatureType::Mirror); }},
           {IconId::LinearPattern, tr("Linear Pattern"), "Shift+P", [this] { m_ws->startFeature(FeatureType::LinearPattern); }},
           {IconId::CircularPattern, tr("Circular Pattern"), "Shift+C", [this] { m_ws->startFeature(FeatureType::CircularPattern); }}});
    pro << tool(IconId::Combine, tr("Combine"), "B", [this] { m_ws->startFeature(FeatureType::Combine); });
    pro << tool(IconId::Thread, tr("Thread"), "", [this] { m_ws->startFeature(FeatureType::Thread); });
    pro << group(IconId::Plane, tr("Construct"),
          {{IconId::Plane, tr("Offset Plane"), "", [this] { m_ws->startPlane("offset"); }},
           {IconId::Plane, tr("Angled Plane"), "", [this] { m_ws->startPlane("angle"); }},
           {IconId::Plane, tr("Midplane"), "", [this] { m_ws->startPlane("mid"); }}});
    pro << group(IconId::Measure, tr("Inspect"),
          {{IconId::Measure, tr("Measure"), "I", [this] { m_ws->startMeasure(); }},
           {IconId::Interference, tr("Interference"), "Shift+I", [this] { m_ws->startInterference(); }},
           {IconId::Section, tr("Section"), "Alt+S", [this] { toggleSection(); }},
           {IconId::Printability, tr("Printability"), "", [this] { m_ws->startPrintability(); }}});
    {
        // Alt+S works from anywhere in the window (the viewport ignores Alt shortcuts)
        auto* sec = new QAction(this);
        sec->setShortcut(QKeySequence(tr("Alt+S")));
        sec->setShortcutContext(Qt::WindowShortcut);
        connect(sec, &QAction::triggered, this, [this] { toggleSection(); });
        addAction(sec);
    }
    model->addSeparator();
    auto shortcutTool = [&](IconId ic, const QString& text, const QKeySequence& ks, auto fn) {
        QAction* a = tool(ic, text, ks.toString(QKeySequence::NativeText), fn);
        a->setShortcut(ks);
        a->setShortcutContext(Qt::WindowShortcut);
        return a;
    };
    pro << shortcutTool(IconId::Drawing, tr("Drawing"), QKeySequence(tr("Ctrl+D")), [this] { showDrawing(); });
    pro << shortcutTool(IconId::Parameters, tr("Parameters"), QKeySequence(tr("Ctrl+P")), [this] { showParameters(); });
    tool(IconId::Fit, tr("Fit"), "W", [this] { m_vp->fitAll(); });
    shortcutTool(IconId::Learn, tr("Learn"), QKeySequence(tr("F1")), [this] { m_learnDock->setVisible(!m_learnDock->isVisible()); });
    {
        // profile switch, at the right end of the bar
        auto* spacer = new QWidget(model);
        spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        model->addWidget(spacer);
        m_profileButton = new QToolButton(model);
        m_profileButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        m_profileButton->setPopupMode(QToolButton::InstantPopup);
        m_profileButton->setIcon(icon(IconId::Profile));
        auto* menu = new QMenu(m_profileButton);
        auto* ess = menu->addAction(icon(IconId::Learn), tr("Essential (beginner / workshop): the 7 fundamental commands"));
        auto* full = menu->addAction(icon(IconId::Body), tr("Complete (pro): every tool"));
        connect(ess, &QAction::triggered, this, [this] { setProfile(true); });
        connect(full, &QAction::triggered, this, [this] { setProfile(false); });
        m_profileButton->setMenu(menu);
        model->addWidget(m_profileButton);
    }
    m_proActions = pro;
    m_essentialActions = essentialOnly;

    // Contextual sketch bar: shown only while sketching (no workbench switch).
    addToolBarBreak();  // own row: the sketch bar is long and only shown while sketching
    m_sketchBar = addToolBar(tr("Sketch"));
    m_sketchBar->setObjectName("sketchBar");
    m_sketchBar->setIconSize(QSize(24, 24));
    m_sketchBar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    auto sk = [&](IconId ic, const QString& text, const QString& key, auto fn) {
        QAction* a = new QAction(icon(ic), text, this);
        a->setToolTip(QString("%1 (%2)").arg(text, key));
        connect(a, &QAction::triggered, this, [this, fn] {
            if (auto* e = dynamic_cast<SketchEditor*>(m_ws->session()))
                fn(e);
        });
        m_sketchBar->addAction(a);
        return a;
    };
    using SkTool = SketchEditor::Tool;
    /// tool button with a drop-down of variants (the button keeps the last variant chosen)
    auto skDrop = [&](std::vector<std::tuple<IconId, QString, QString, std::function<void(SketchEditor*)>>> variants) {
        auto* b = new QToolButton(m_sketchBar);
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setPopupMode(QToolButton::MenuButtonPopup);
        auto* menu = new QMenu(b);
        QAction* first = nullptr;
        for (const auto& [ic, text, key, fn] : variants) {
            QAction* a = new QAction(icon(ic), text, b);
            a->setToolTip(key.isEmpty() ? text : QString("%1 (%2)").arg(text, key));
            connect(a, &QAction::triggered, this, [this, b, a, fn = fn] {
                b->setDefaultAction(a);
                if (auto* e = dynamic_cast<SketchEditor*>(m_ws->session()))
                    fn(e);
            });
            menu->addAction(a);
            if (!first)
                first = a;
        }
        b->setMenu(menu);
        b->setDefaultAction(first);
        int textW = 0;
        for (const auto& v : variants)
            textW = std::max(textW, b->fontMetrics().horizontalAdvance(std::get<1>(v)));
        b->setMinimumWidth(textW + 30);  // room for the drop-down arrow
        return m_sketchBar->addWidget(b);
    };
    sk(IconId::Line, tr("Line"), "L", [](SketchEditor* e) { e->setTool(SketchEditor::Tool::Line); });
    skDrop({{IconId::Rectangle, tr("Rectangle"), "R", [](SketchEditor* e) { e->setTool(SkTool::Rectangle); }},
            {IconId::CenterRectangle, tr("Center Rectangle"), "Shift+R", [](SketchEditor* e) { e->setTool(SkTool::CenterRectangle); }},
            {IconId::Polygon, tr("Polygon"), "", [](SketchEditor* e) { e->setTool(SkTool::Polygon); }}});
    sk(IconId::Circle, tr("Circle"), "C", [](SketchEditor* e) { e->setTool(SketchEditor::Tool::Circle); });
    sk(IconId::Arc, tr("Arc"), "A", [](SketchEditor* e) { e->setTool(SketchEditor::Tool::Arc); });
    m_proActions << sk(IconId::Slot, tr("Slot"), "U", [](SketchEditor* e) { e->setTool(SkTool::Slot); });
    sk(IconId::Dimension, tr("Dimension"), "D", [](SketchEditor* e) { e->setTool(SketchEditor::Tool::Dimension); });
    m_proActions << sk(IconId::Project, tr("Project"), "E", [](SketchEditor* e) { e->setTool(SketchEditor::Tool::Project); });
    m_proActions << sk(IconId::Construction, tr("Construction"), "X", [](SketchEditor* e) { e->toggleConstruction(); });
    m_sketchBar->addSeparator();
    skDrop({{IconId::Trim, tr("Trim"), "T", [](SketchEditor* e) { e->setTool(SkTool::Trim); }},
            {IconId::Extend, tr("Extend"), "", [](SketchEditor* e) { e->setTool(SkTool::Extend); }}});
    m_proActions << sk(IconId::SketchFillet, tr("Fillet"), "Shift+F", [](SketchEditor* e) { e->setTool(SkTool::Fillet); });
    m_proActions << sk(IconId::Offset, tr("Offset"), "Shift+O", [](SketchEditor* e) { e->runOffset(); });
    m_proActions << sk(IconId::SketchMirror, tr("Mirror"), tr("entities, then the line"), [](SketchEditor* e) { e->runMirror(); });
    m_proActions << sk(IconId::SketchPattern, tr("Pattern"), tr("+ center point = circular"), [](SketchEditor* e) { e->runPattern(); });
    m_sketchBar->addSeparator();
    // v0.7: vector import and calibrated reference image
    sk(IconId::ImportVector, tr("Import SVG/DXF"), "Ctrl+Shift+I", [](SketchEditor* e) { e->importVector(); });
    m_proActions << sk(IconId::Canvas, tr("Image"), tr("insert a photo, then right-click > Calibrate"),
                       [](SketchEditor* e) { e->hasCanvas() ? e->startCalibration() : e->insertCanvas(); });
    m_sketchBar->addSeparator();
    QAction* finish = new QAction(icon(IconId::Finish), tr("Finish sketch"), this);
    connect(finish, &QAction::triggered, this, [this] { m_ws->finishSession(); });
    m_sketchBar->addAction(finish);
    m_sketchBar->setVisible(false);

    // v0.5 lot 3: intent cards (1.2 s hover) on the Model and Sketch bars, then the saved profile
    m_richTips = new RichTips([this] { return m_model->pedagogyFrench(); }, this);
    m_richTips->watch(model);
    m_richTips->watch(m_sketchBar);
    setProfile(QSettings().value("ui/profile", "complete").toString() == "essential");
}

void MainWindow::setProfile(bool essential, bool persist)
{
    m_essential = essential;
    for (QAction* a : m_proActions)
        a->setVisible(!essential);
    for (QAction* a : m_essentialActions)
        a->setVisible(essential);
    if (m_profileButton) {
        m_profileButton->setText((essential ? tr("Essential") : tr("Pro")) + QString::fromUtf8(" \u25BE"));
        m_profileButton->setToolTip(essential ? tr("Essential profile: the 7 fundamental commands (click for the Complete profile)")
                                              : tr("Complete profile: every tool (click for the Essential profile)"));
        // icon only in the Complete profile: its bar is full on 1366 px screens
        m_profileButton->setToolButtonStyle(essential ? Qt::ToolButtonTextUnderIcon : Qt::ToolButtonIconOnly);
    }
    setEssentialUi(essential);
    if (persist)
        QSettings().setValue("ui/profile", essential ? "essential" : "complete");
}

void MainWindow::pulseTool(const QString& tool, const QString& text)
{
    if (!text.isEmpty())
        statusBar()->showMessage(QString::fromUtf8("\xF0\x9F\x92\xA1 ") + text, 12000);
    if (tool.isEmpty())
        return;
    // find the tool button (direct action, drop-down default action or one of its menu entries)
    QWidget* target = nullptr;
    for (QToolBar* bar : findChildren<QToolBar*>()) {
        for (QAction* a : bar->actions())
            if (a->text().compare(tool, Qt::CaseInsensitive) == 0)
                target = bar->widgetForAction(a);
        for (QToolButton* b : bar->findChildren<QToolButton*>()) {
            if (b->defaultAction() && b->defaultAction()->text().compare(tool, Qt::CaseInsensitive) == 0)
                target = b;
            if (b->menu())
                for (QAction* a : b->menu()->actions())
                    if (a->text().section('\t', 0, 0).compare(tool, Qt::CaseInsensitive) == 0 ||
                        a->data().toString().compare(tool, Qt::CaseInsensitive) == 0)
                        target = b;
        }
        if (target)
            break;
    }
    if (!target)
        return;
    // pulse: orange frame blinking for ~2.5 s (the student still does the work)
    auto* timer = new QTimer(target);
    auto* count = new int(0);
    const QString base = target->styleSheet();
    connect(timer, &QTimer::timeout, target, [target, timer, count, base] {
        ++*count;
        target->setStyleSheet(*count % 2 ? base + "QToolButton { border: 2px solid #ffb020; border-radius: 4px; background: #4a3a10; }" : base);
        if (*count >= 10) {
            target->setStyleSheet(base);
            timer->stop();
            timer->deleteLater();
            delete count;
        }
    });
    timer->start(250);
}

void MainWindow::updateSnapButton()
{
    const bool on = m_ws->gridSnap();
    m_snapButton->setText(on ? tr("Snap: %1 mm").arg(QLocale().toString(m_ws->gridStep())) : tr("Snap: off"));
    for (QAction* a : m_snapButton->menu()->actions())
        a->setChecked(on ? std::abs(a->data().toDouble() - m_ws->gridStep()) < 1e-9 : a->data().toDouble() == 0);
}

void MainWindow::setMcpState(McpState st, const QString& detail)
{
    QColor c;
    QString text, tip;
    switch (st) {
        case McpState::Off:
            c = QColor(0x6a, 0x72, 0x7c);
            text = tr("MCP off");
            tip = tr("Started with --no-ipc: no AI agent can drive this window");
            break;
        case McpState::Ready:
            c = QColor(0x9a, 0xa3, 0xad);
            text = tr("MCP ready");
            tip = tr("Listening on %1 — no agent connected").arg(detail);
            break;
        case McpState::Connected:
            c = QColor(0x5c, 0xd6, 0x8a);
            text = tr("MCP agent connected");
            tip = tr("An AI agent (Claude Code, Gemini CLI...) is driving Silex through the IPC channel");
            break;
        case McpState::Disabled:
            c = QColor(0xf0, 0xa0, 0x40);
            text = tr("IPC disabled");
            tip = tr("The IPC channel is already used by another Silex (%1)").arg(detail);
            break;
    }
    m_mcpState = st;
    m_mcpDetail = detail;
    if (m_model->tutorMode()) {  // v0.5: Socratic tutor, modelling commands locked
        text += tr(" · tutor");
        tip += "\n" + tr("Tutor mode: the agent can only observe and guide, Silex refuses its modelling commands.");
    }
    m_mcpLabel->setText(QString("<span style='color:%1; font-size:14px'>&#9679;</span> %2").arg(c.name(), text));
    m_mcpLabel->setToolTip(tip);
}

void MainWindow::refreshTitle()
{
    const QString name = m_model->filePath().isEmpty() ? tr("Untitled") : QFileInfo(m_model->filePath()).fileName();
    setWindowTitle(QString("%1%2 — Silex").arg(name, m_model->isDirty() ? "*" : ""));
    if (m_undo) {
        m_undo->setEnabled(m_ws->hasSession() || m_model->history().canUndo());
        m_redo->setEnabled(m_ws->hasSession() || m_model->history().canRedo());
        m_undo->setToolTip(tr("Undo %1").arg(QString::fromStdString(m_model->history().undoLabel())));
    }
}

void MainWindow::setPanel(QWidget* w)
{
    if (m_panel)
        m_panel->deleteLater();
    m_panel = w;
    if (QWidget* old = m_inspectorArea->takeWidget(); old && old != w)
        old->deleteLater();
    m_inspectorArea->setWidget(w);
}

void MainWindow::onSessionChanged()
{
    EditSession* s = m_ws->session();
    m_sketchBar->setVisible(s && s->isSketch());
    m_timeline->setEditing(s ? s->editedFeature() : Uuid{});
    if (s) {
        m_inspector->setWindowTitle(s->title());
        setPanel(s->createPanel(nullptr));
    }
    else {
        m_inspector->setWindowTitle(tr("Inspector"));
        rebuildDefaultInspector();
    }
    refreshTitle();
}

void MainWindow::insertComponent()
{
    const bool fr = m_model->pedagogyFrench();
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Insert Component"));
    dlg.resize(560, 380);
    auto* lay = new QHBoxLayout(&dlg);
    auto* list = new QListWidget(&dlg);
    list->setIconSize(QSize(24, 24));
    for (const auto& c : ComponentLibrary::all()) {
        auto* it = new QListWidgetItem(icon(IconId::Component), QString::fromStdString(c.name(fr)), list);
        it->setData(Qt::UserRole, QString::fromStdString(c.id));
    }
    lay->addWidget(list, 1);
    auto* right = new QVBoxLayout();
    auto* info = new QLabel(&dlg);
    info->setWordWrap(true);
    info->setMinimumWidth(240);
    info->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    right->addWidget(info, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Insert"));
    right->addWidget(buttons);
    lay->addLayout(right, 1);
    auto show = [&](int row) {
        if (row < 0 || row >= int(ComponentLibrary::all().size()))
            return;
        const ComponentInfo& c = ComponentLibrary::all()[size_t(row)];
        QString holes;
        for (const auto& h : c.holes)
            holes += QString("(%1 ; %2) %3%4<br>").arg(h.x).arg(h.y).arg(QString::fromUtf8("\u00D8")).arg(h.d);
        info->setText(QString("<b>%1</b><p>%2</p>").arg(QString::fromStdString(c.name(fr)).toHtmlEscaped(),
                                                       QString::fromStdString(c.description(fr)).toHtmlEscaped()) +
                      (c.screw.empty() ? QString()
                                       : tr("<p>Screws: <b>%1</b>, standoffs %2 mm</p>").arg(QString::fromStdString(c.screw)).arg(c.standoff)) +
                      (holes.isEmpty() ? QString() : tr("<p>Mounting holes (component frame):<br>%1</p>").arg(holes)) +
                      tr("<p style='color:#a8b0ba'>Inserted as a locked reference body: place it with Move/Copy or Align, "
                         "project its edges in a sketch, drill with Hole / Insert (snaps to its holes).</p>"));
    };
    connect(list, &QListWidget::currentRowChanged, &dlg, show);
    connect(list, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    list->setCurrentRow(0);
    if (dlg.exec() != QDialog::Accepted || !list->currentItem())
        return;
    m_ws->insertComponent(list->currentItem()->data(Qt::UserRole).toString().toStdString());
}

void MainWindow::rebuildDefaultInspector()
{
    auto* w = new QWidget;
    auto* lay = new QVBoxLayout(w);
    auto snap = m_model->snapshot();

    // Bodies: tick to select whole bodies (order of ticking = order of selection, first = target)
    auto* bodiesBox = new QGroupBox(tr("Bodies"), w);
    auto* bl = new QVBoxLayout(bodiesBox);
    const std::vector<Uuid> selBodies = m_ws->selectedBodies();
    if (!snap || snap->bodies.empty()) {
        auto* none = new QLabel(tr("<span style='color:#80868f'>none</span>"), bodiesBox);
        bl->addWidget(none);
    }
    else {
        for (const auto& bv : snap->bodies) {
            const auto pos = std::find(selBodies.begin(), selBodies.end(), bv.id);
            const bool checked = pos != selBodies.end();
            QString label = QString("%1  (%2 faces)").arg(QString::fromStdString(bv.name)).arg(bv.mesh.faces.size());
            if (bv.locked())
                label = QString::fromUtf8("\U0001F512 ") + QString::fromStdString(bv.name) + tr("  (reference)");
            if (checked && pos == selBodies.begin() && selBodies.size() > 1)
                label += tr("  — target");
            // v0.4: eye = visibility (V), hidden bodies greyed out
            const bool hidden = m_ws->bodyHidden(bv.id);
            auto* row = new QHBoxLayout();
            auto* eye = new QToolButton(bodiesBox);
            eye->setAutoRaise(true);
            eye->setIcon(icon(hidden ? IconId::EyeOff : IconId::Eye));
            eye->setToolTip(hidden ? tr("Show (V)") : tr("Hide (V)"));
            connect(eye, &QToolButton::clicked, this, [this, id = bv.id, hidden] { m_ws->setBodyHidden(id, !hidden); });
            row->addWidget(eye);
            auto* cb = new QCheckBox(label, bodiesBox);
            cb->setChecked(checked);
            cb->setEnabled(!hidden);
            if (bv.locked())
                cb->setToolTip(tr("Locked library component: move it (Move/Copy, Align), project its edges in a sketch, "
                                  "check interferences. It is never modified, printed or exported with the parts."));
            if (hidden)
                cb->setStyleSheet("color: #6a727c;");
            else if (checked && pos == selBodies.begin() && selBodies.size() > 1)
                cb->setStyleSheet(QString("color: %1;").arg(Colors::TargetBody.name()));
            connect(cb, &QCheckBox::toggled, this, [this, id = bv.id](bool) { m_ws->toggleBody(id); });
            row->addWidget(cb, 1);
            bl->addLayout(row);
        }
    }
    if (selBodies.size() >= 2) {
        auto* row = new QHBoxLayout();
        auto button = [&](const QString& text, BooleanOp op) {
            auto* pb = new QPushButton(icon(IconId::Combine), text, bodiesBox);
            connect(pb, &QPushButton::clicked, this, [this, op] { m_ws->combineSelected(op); });
            row->addWidget(pb);
        };
        button(tr("Merge"), BooleanOp::Join);
        button(tr("Subtract"), BooleanOp::Cut);
        button(tr("Intersect"), BooleanOp::Intersect);
        bl->addLayout(row);
        auto* hint = new QLabel(tr("<small>Subtract / intersect use the first selected body (orange) as target.</small>"),
                                bodiesBox);
        hint->setWordWrap(true);
        hint->setStyleSheet("color: #9aa3ad;");
        bl->addWidget(hint);
    }
    lay->addWidget(bodiesBox);

    if (selBodies.size() == 1)
        lay->addWidget(bodyPropertiesPanel(w, selBodies.front()));

    // v0.4: "Construction" folder (construction planes): sketch on them in one click
    if (snap && !snap->planes.empty()) {
        auto* cons = new QGroupBox(tr("Construction"), w);
        auto* cl = new QVBoxLayout(cons);
        for (const auto& pv : snap->planes) {
            auto* row = new QHBoxLayout();
            auto* lbl = new QLabel(QString("%1").arg(QString::fromStdString(pv.name)), cons);
            row->addWidget(lbl, 1);
            auto* skb = new QPushButton(icon(IconId::Sketch), tr("Sketch"), cons);
            skb->setToolTip(tr("Start a sketch on this plane"));
            connect(skb, &QPushButton::clicked, this, [this, pv] {
                SelItem s;
                s.kind = SelKind::OriginPlane;
                s.name = "plane:" + pv.id;
                s.owner = pv.id;
                s.planar = true;
                s.plane = pv.plane;
                m_ws->startSketchOn(s);
            });
            row->addWidget(skb);
            auto* ed = new QPushButton(tr("Edit"), cons);
            connect(ed, &QPushButton::clicked, this, [this, id = pv.id] { m_ws->editFeature(id); });
            row->addWidget(ed);
            cl->addLayout(row);
        }
        lay->addWidget(cons);
    }

    if (snap) {
        QString issues;
        for (const auto& r : snap->results) {
            if (r.status != FeatureStatus::Error && r.status != FeatureStatus::Warning)
                continue;
            FeaturePtr f = m_model->doc().find(r.id);
            const QColor c = r.status == FeatureStatus::Error ? Colors::Error : Colors::Warning;
            issues += QString("<span style='color:%1'>&#9679; %2</span>: %3<br>")
                          .arg(c.name(), f ? QString::fromStdString(f->name).toHtmlEscaped() : QString("?"),
                               QString::fromStdString(r.message).toHtmlEscaped());
            if (const QString why = m_ws->explainStep(r.id); !why.isEmpty())  // v0.4 lot 3
                issues += QString("<span style='color:#cfe8d2'>&#128161; %1</span><br>").arg(why.toHtmlEscaped());
        }
        if (!issues.isEmpty()) {
            auto* l = new QLabel(tr("<b>Problems</b><br>") + issues, w);
            l->setWordWrap(true);
            lay->addWidget(l);
        }
    }

    const auto& sel = m_ws->selection();
    if (!sel.empty()) {
        QString s = tr("<b>Selection</b> (%1)<br>").arg(sel.size());
        int n = 0;
        for (const SelItem& it : sel) {
            if (++n > 8) {
                s += "...";
                break;
            }
            const char* kind = it.kind == SelKind::Face ? "Face" : it.kind == SelKind::Edge ? "Edge" : it.kind == SelKind::Body ? "Body" : "Profile";
            QString name = QString::fromStdString(it.name);
            if (name.size() > 60)
                name = name.left(57) + "...";
            s += QString("%1 <span style='color:#80868f'>%2</span><br>").arg(kind, name.toHtmlEscaped());
        }
        auto* l = new QLabel(s, w);
        l->setWordWrap(true);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        lay->addWidget(l);
    }

    auto* help = new QLabel(
        tr("<b>Shortcuts</b><br>"
           "S sketch · E extrude · R revolve · F fillet · C chamfer<br>"
           "Right-drag: marking menu<br>"
           "Middle-drag: pan · Shift+middle: orbit · Wheel: zoom<br>"
           "Alt+left: orbit · W / Home: fit · 0/1/3/7: views<br>"
           "Double-click a profile: extrude · a timeline item: edit<br>"
           "Double-click a body: select it (Ctrl: add) · B: combine<br>"
           "<b>v0.3</b> I measure · Shift+I interference · Alt+S section<br>"
           "Shift+W sweep · Shift+L loft · Ctrl+P parameters<br>"
           "Ctrl+D drawing · Ctrl+3 export 3MF<br>"
           "<b>v0.4</b> Del delete body / face · V hide / show · F1 guided mode<br>"
           "Drag in the void: box select (→ window · ← crossing)"),
        w);
    help->setWordWrap(true);
    help->setStyleSheet("color: #9aa3ad;");
    lay->addWidget(help);
    lay->addStretch(1);
    setPanel(w);
}

// ------------------------------------------------------------------ help, autosave, recovery

QToolButton* MainWindow::buildHelpButton(QWidget* parent)
{
    auto* help = new QToolButton(parent);
    help->setText(tr("?"));
    help->setToolTip(tr("Help: About Silex / Licenses, autosave"));
    help->setPopupMode(QToolButton::InstantPopup);
    help->setStyleSheet("QToolButton { font-weight: bold; font-size: 15px; padding: 0 6px; }");
    auto* menu = new QMenu(help);
    menu->addAction(tr("About Silex / Licenses..."), this, [this] { AboutDialog(this).exec(); });
    menu->addSeparator();
    QMenu* autosave = menu->addMenu(tr("Autosave"));
    QAction* enabled = autosave->addAction(tr("Enabled"));
    enabled->setCheckable(true);
    enabled->setChecked(m_autosave->enabled());
    connect(enabled, &QAction::toggled, this, [this](bool on) { m_autosave->setEnabled(on); });
    autosave->addSeparator();
    auto* every = new QActionGroup(autosave);
    for (int minutes : {1, 2, 5, 10, 15}) {
        QAction* a = autosave->addAction(tr("Every %n minute(s)", nullptr, minutes));
        a->setCheckable(true);
        a->setChecked(m_autosave->intervalMinutes() == minutes);
        every->addAction(a);
        connect(a, &QAction::triggered, this, [this, minutes] { m_autosave->setIntervalMinutes(minutes); });
    }
    autosave->addSeparator();
    autosave->addAction(tr("Open the recovery folder"), this, [] {
        QDir().mkpath(AutosaveManager::recoveryDir());
        QDesktopServices::openUrl(QUrl::fromLocalFile(AutosaveManager::recoveryDir()));
    });
    help->setMenu(menu);
    return help;
}

void MainWindow::offerRecovery()
{
    if (m_autosave->offerRecovery(this)) {
        m_firstFit = true;
        statusBar()->showMessage(tr("Autosave restored: save the design to keep it (Ctrl+S)"), 8000);
    }
}

bool MainWindow::recoverFrom(const QString& autosaveFile)
{
    QString err;
    if (!m_autosave->restoreFile(autosaveFile, &err)) {
        statusBar()->showMessage(tr("Recovery failed: %1").arg(err), 8000);
        return false;
    }
    m_firstFit = true;
    statusBar()->showMessage(tr("Autosave restored: save the design to keep it (Ctrl+S)"), 8000);
    return true;
}

// ------------------------------------------------------------------ files

bool MainWindow::maybeSave()
{
    if (!m_model->isDirty())
        return true;
    const auto r = QMessageBox::question(this, tr("Silex"), tr("Save changes to the current design?"),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel)
        return false;
    if (r == QMessageBox::Save)
        return doSave(false);
    return true;
}

bool MainWindow::doSave(bool saveAs)
{
    if (m_ws->hasSession())
        m_ws->finishSession();
    QString path = m_model->filePath();
    if (saveAs || path.isEmpty()) {
        path = QFileDialog::getSaveFileName(this, tr("Save design"), path, tr("Silex design (*.silex)"));
        if (path.isEmpty())
            return false;
        if (!path.endsWith(".silex"))
            path += ".silex";
    }
    QString err;
    if (!m_model->save(path, &err)) {
        QMessageBox::critical(this, tr("Save failed"), err);
        return false;
    }
    statusBar()->showMessage(tr("Saved %1").arg(path), 4000);
    return true;
}

bool MainWindow::openFile(const QString& path)
{
    if (m_ws->hasSession())
        m_ws->cancelSession();
    QString err;
    if (!m_model->load(path, &err)) {
        QMessageBox::critical(this, tr("Open failed"), err);
        return false;
    }
    m_firstFit = true;
    return true;
}

void MainWindow::doOpen()
{
    if (!maybeSave())
        return;
    const QString path = QFileDialog::getOpenFileName(this, tr("Open design"), {}, tr("Silex design (*.silex)"));
    if (!path.isEmpty())
        openFile(path);
}

void MainWindow::doNew()
{
    if (!maybeSave())
        return;
    if (m_ws->hasSession())
        m_ws->cancelSession();
    m_model->newDocument();
    m_firstFit = true;
}

void MainWindow::doImport()
{
    const QString path =
        QFileDialog::getOpenFileName(this, tr("Import STEP"), {}, tr("STEP files (*.step *.stp *.STEP *.STP)"));
    if (path.isEmpty())
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        ImportParams p;
        p.sourcePath = QFileInfo(path).fileName().toStdString();
        p.brep = KernelEngine::importStepAsBrep(path.toStdString());
        if (m_ws->hasSession())
            m_ws->finishSession();
        const std::string name = QFileInfo(path).completeBaseName().toStdString();
        m_model->execute(AddFeatureCommand(makeFeature(FeatureType::ImportStep, name, Json(p))));
        m_firstFit = true;
    }
    catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Import failed"), QString::fromUtf8(e.what()));
    }
    QApplication::restoreOverrideCursor();
}

void MainWindow::doExport()
{
    QString path = QFileDialog::getSaveFileName(this, tr("Export STEP"), {}, tr("STEP files (*.step)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(".step", Qt::CaseInsensitive) && !path.endsWith(".stp", Qt::CaseInsensitive))
        path += ".step";
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto err = KernelEngine::exportStep(m_model->doc(), path.toStdString());
    QApplication::restoreOverrideCursor();
    if (err)
        QMessageBox::critical(this, tr("Export failed"), QString::fromStdString(*err));
    else
        statusBar()->showMessage(tr("Exported %1").arg(path), 4000);
}

void MainWindow::doExportStl()
{
    QString path = QFileDialog::getSaveFileName(this, tr("Export STL"), {}, tr("STL files (*.stl)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(".stl", Qt::CaseInsensitive))
        path += ".stl";
    bool ok = false;
    const double deflection =
        QInputDialog::getDouble(this, tr("Export STL"),
                                tr("Chordal deflection (mm) — smaller is finer and heavier:"), 0.05, 0.001, 5.0, 3, &ok);
    if (!ok)
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto err = KernelEngine::exportStl(m_model->doc(), path.toStdString(), {}, deflection);
    QApplication::restoreOverrideCursor();
    if (err)
        QMessageBox::critical(this, tr("Export failed"), QString::fromStdString(*err));
    else
        statusBar()->showMessage(tr("Exported %1 (%2 KB)").arg(path).arg(QFileInfo(path).size() / 1024), 5000);
}

QWidget* MainWindow::bodyPropertiesPanel(QWidget* parent, const Uuid& bodyId)
{
    auto* box = new QGroupBox(tr("Body properties"), parent);
    auto* form = new QFormLayout(box);
    auto snap = m_model->snapshot();
    const BodyView* bv = nullptr;
    if (snap)
        for (const auto& b : snap->bodies)
            if (b.id == bodyId)
                bv = &b;
    if (!bv) {
        form->addRow(new QLabel(tr("computing..."), box));
        return box;
    }
    const BBox& bb = bv->mesh.bbox;
    const Vec3 size = bb.valid() ? bb.max - bb.min : Vec3{};
    auto value = [&](const QString& label, const QString& v) {
        auto* l = new QLabel(v, box);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(label, l);
    };
    value(tr("Volume"), QString("%1 mm³").arg(bv->volume, 0, 'f', 2));
    value(tr("Area"), QString("%1 mm²").arg(bv->area, 0, 'f', 2));
    value(tr("Bounding box"), QString("%1 × %2 × %3 mm").arg(size.x, 0, 'f', 2).arg(size.y, 0, 'f', 2).arg(size.z, 0, 'f', 2));
    value(tr("Estimated mass"), QString("<b>%1 g</b>").arg(bv->mass(), 0, 'f', 2));

    // ---- material (Lot 4.1)
    const BodyMaterial current = m_model->doc().materialOf(bodyId);
    auto* preset = new QComboBox(box);
    preset->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    preset->setMinimumContentsLength(12);
    int selected = -1;
    for (size_t i = 0; i < materialPresets().size(); ++i) {
        const BodyMaterial& m = materialPresets()[i];
        QPixmap pm(14, 14);
        pm.fill(QColor::fromRgbF(m.r, m.g, m.b));
        preset->addItem(QIcon(pm), QString("%1 (%2 g/cm³)").arg(QString::fromStdString(m.name)).arg(m.density));
        if (m == current)
            selected = int(i);
    }
    preset->addItem(tr("Custom"));
    preset->setCurrentIndex(selected >= 0 ? selected : preset->count() - 1);
    form->addRow(tr("Material"), preset);
    connect(preset, &QComboBox::activated, this, [this, bodyId](int i) {
        if (i >= 0 && i < int(materialPresets().size()))
            m_model->execute(SetBodyMaterialCommand(bodyId, materialPresets()[size_t(i)]));
    });

    auto* colorBtn = new QPushButton(box);
    QPixmap sw(40, 14);
    sw.fill(QColor::fromRgbF(current.r, current.g, current.b));
    colorBtn->setIcon(QIcon(sw));
    colorBtn->setIconSize(QSize(40, 14));
    colorBtn->setText(tr("Colour..."));
    form->addRow(tr("Colour"), colorBtn);
    connect(colorBtn, &QPushButton::clicked, this, [this, bodyId, current] {
        const QColor c = QColorDialog::getColor(QColor::fromRgbF(current.r, current.g, current.b, current.a), this,
                                                tr("Body colour"), QColorDialog::ShowAlphaChannel);
        if (!c.isValid())
            return;
        BodyMaterial m = current;
        m.name = tr("Custom").toStdString();
        m.r = float(c.redF());
        m.g = float(c.greenF());
        m.b = float(c.blueF());
        m.a = float(c.alphaF());
        m_model->execute(SetBodyMaterialCommand(bodyId, m));
    });

    auto* opacity = new QSlider(Qt::Horizontal, box);
    opacity->setRange(5, 100);
    opacity->setValue(int(std::lround(current.a * 100)));
    opacity->setToolTip(tr("Opacity: lower it to see the parts inside an enclosure"));
    form->addRow(tr("Opacity"), opacity);
    connect(opacity, &QSlider::sliderReleased, this, [this, bodyId, current, opacity] {
        BodyMaterial m = current;
        m.a = float(opacity->value()) / 100.f;
        if (std::abs(m.a - current.a) > 1e-3)
            m_model->execute(SetBodyMaterialCommand(bodyId, m));
    });

    auto* density = new QDoubleSpinBox(box);
    density->setRange(0.01, 25.0);
    density->setDecimals(3);
    density->setSuffix(" g/cm³");
    density->setValue(current.density);
    form->addRow(tr("Density"), density);
    connect(density, &QDoubleSpinBox::editingFinished, this, [this, bodyId, current, density] {
        if (std::abs(density->value() - current.density) < 1e-9)
            return;
        BodyMaterial m = current;
        m.density = density->value();
        m_model->execute(SetBodyMaterialCommand(bodyId, m));
    });
    return box;
}

void MainWindow::showParameters()
{
    if (m_ws->hasSession())
        m_ws->finishSession();
    ParametersDialog dlg(m_model, this);
    m_ws->holdGhost(true);  // v0.5: the geometry before stays visible while the dialog is open
    dlg.exec();
    m_ws->holdGhost(false);
}

void MainWindow::showDrawing()
{
    if (m_ws->hasSession())
        m_ws->finishSession();
    if (!m_drawing) {
        m_drawing = new DrawingWindow(m_model, this);
        m_drawing->setWindowFlag(Qt::Window);
        m_drawing->setAttribute(Qt::WA_DeleteOnClose, false);
    }
    m_drawing->show();
    m_drawing->raise();
    m_drawing->activateWindow();
}

void MainWindow::toggleSection()
{
    m_section->activate(!m_section->isActive());
}

void MainWindow::doExport3mf()
{
    QString base = m_model->filePath().isEmpty() ? QString() : QFileInfo(m_model->filePath()).completeBaseName();
    QString path = QFileDialog::getSaveFileName(this, tr("Export 3MF"), base, tr("3MF files (*.3mf)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(".3mf", Qt::CaseInsensitive))
        path += ".3mf";
    bool ok = false;
    const double deflection =
        QInputDialog::getDouble(this, tr("Export 3MF"),
                                tr("Chordal deflection (mm) — smaller is finer and heavier:"), 0.05, 0.001, 5.0, 3, &ok);
    if (!ok)
        return;
    ThreeMfOptions o;
    o.linearDeflection = deflection;
    o.title = base.isEmpty() ? "Silex design" : base.toStdString();
    // export only the selected bodies when there is a selection
    o.bodies = m_ws->selectedBodies();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const ExportSummary s = KernelEngine::export3mf(m_model->doc(), path.toStdString(), o);
    QApplication::restoreOverrideCursor();
    if (!s.error.empty())
        QMessageBox::critical(this, tr("Export failed"), QString::fromStdString(s.error));
    else
        m_model->noteEvent("3mf_exported");  // lesson criterion (Parcours 9)
    if (s.error.empty())
        statusBar()->showMessage(tr("Exported %1: %2 body(ies), %3 triangles, %4 KB")
                                     .arg(path)
                                     .arg(s.objects)
                                     .arg(s.triangles)
                                     .arg(s.bytes / 1024),
                                 6000);
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (maybeSave())
        e->accept();
    else
        e->ignore();
}

// ------------------------------------------------------------------ demo & screenshot

namespace {
SketchGeo line(Vec2 a, Vec2 b)
{
    SketchGeo g;
    g.id = newUuid();
    g.kind = GeoKind::Line;
    g.p1 = a;
    g.p2 = b;
    return g;
}
SketchConstraint cons(ConstraintKind k, GeoRef a, GeoRef b = {}, double v = 0)
{
    SketchConstraint c;
    c.id = newUuid();
    c.kind = k;
    c.a = a;
    c.b = b;
    c.value = v;
    return c;
}
}  // namespace

void MainWindow::loadDemo()
{
    // Plate 60 x 40 x 15, filleted vertical edges, a through hole sketched on the top face.
    Sketch s1;
    const double w = 60, h = 40;
    const Vec2 p[4] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
    Uuid l[4];
    for (int i = 0; i < 4; ++i) {
        auto g = line(p[i], p[(i + 1) % 4]);
        l[i] = g.id;
        s1.geos.push_back(g);
    }
    for (int i = 0; i < 4; ++i)
        s1.constraints.push_back(cons(ConstraintKind::Coincident, {l[i], PointPos::End}, {l[(i + 1) % 4], PointPos::Start}));
    s1.constraints.push_back(cons(ConstraintKind::Horizontal, {l[0]}));
    s1.constraints.push_back(cons(ConstraintKind::Horizontal, {l[2]}));
    s1.constraints.push_back(cons(ConstraintKind::Vertical, {l[1]}));
    s1.constraints.push_back(cons(ConstraintKind::Vertical, {l[3]}));
    s1.constraints.push_back(cons(ConstraintKind::Coincident, {l[0], PointPos::Start}, {kOriginId, PointPos::Start}));
    s1.constraints.push_back(cons(ConstraintKind::Distance, {l[0]}, {}, w));
    s1.constraints.push_back(cons(ConstraintKind::Distance, {l[3]}, {}, h));
    SketchParams sp1;
    sp1.sketch = s1;
    auto sk1 = makeFeature(FeatureType::Sketch, "Sketch1", Json(sp1));

    ExtrudeParams ep;
    ep.profile.sketch = sk1->id;
    ep.distance = 15;
    auto ex1 = makeFeature(FeatureType::Extrude, "Extrude1", Json(ep));

    // Fillets on the four vertical edges, referenced by persistent names.
    auto side = [&](int i) { return ex1->id + ":side(" + sk1->id + "/" + l[i] + ")"; };
    EdgeBlendParams fp;
    fp.value = 6;
    for (int i = 0; i < 4; ++i) {
        std::string a = side(i), b = side((i + 1) % 4);
        if (b < a)
            std::swap(a, b);
        TopoRef r;
        r.name = "E[" + a + "|" + b + "]";
        r.sig.center = {p[(i + 1) % 4].x, p[(i + 1) % 4].y, 7.5};
        r.sig.dir = {0, 0, 1};
        r.sig.size = 15;
        r.sig.type = 0;
        fp.edges.push_back(r);
    }
    auto fil = makeFeature(FeatureType::Fillet, "Fillet1", Json(fp));

    Sketch s2;
    SketchGeo c;
    c.id = newUuid();
    c.kind = GeoKind::Circle;
    c.c = {w / 2, h / 2};
    c.r = 9;
    s2.geos.push_back(c);
    s2.constraints.push_back(cons(ConstraintKind::Diameter, {c.id}, {}, 18));
    SketchParams sp2;
    sp2.plane.kind = "Face";
    sp2.plane.face.name = ex1->id + ":end";
    sp2.plane.face.sig.center = {w / 2, h / 2, 15};
    sp2.plane.face.sig.dir = {0, 0, 1};
    sp2.plane.face.sig.size = w * h;
    sp2.sketch = s2;
    auto sk2 = makeFeature(FeatureType::Sketch, "Sketch2", Json(sp2));

    ExtrudeParams cp;
    cp.profile.sketch = sk2->id;
    cp.distance = 15;
    cp.reversed = true;
    cp.op = BooleanOp::Cut;
    auto cut = makeFeature(FeatureType::Extrude, "Hole", Json(cp));

    if (m_ws->hasSession())
        m_ws->cancelSession();
    m_model->newDocument();
    for (const auto& f : {sk1, ex1, fil, sk2, cut})
        m_model->execute(AddFeatureCommand(f));
    m_firstFit = true;
}

void MainWindow::scheduleEdit(const QString& name)
{
    auto* conn = new QMetaObject::Connection;
    *conn = connect(m_model, &ModelController::snapshotChanged, this, [this, name, conn] {
        if (m_model->busy())
            return;
        disconnect(*conn);
        delete conn;
        for (const auto& f : m_model->doc().features)
            if (QString::fromStdString(f->name) == name) {
                m_ws->editFeature(f->id);
                return;
            }
        statusBar()->showMessage(tr("No feature named %1").arg(name), 5000);
    });
}

void MainWindow::scheduleTool(const QString& tool)
{
    auto* conn = new QMetaObject::Connection;
    *conn = connect(m_model, &ModelController::snapshotChanged, this, [this, tool, conn] {
        if (m_model->busy())
            return;
        disconnect(*conn);
        delete conn;
        auto snap = m_model->snapshot();
        if (tool == "interference" || tool == "measure") {
            // select every body (measure: the first two)
            std::vector<SelItem> sel;
            if (snap)
                for (const auto& b : snap->bodies)
                    if (tool == "interference" || sel.size() < 2)
                        sel.push_back(m_ws->bodyItem(b.id));
            m_ws->setSelection(sel);
            if (tool == "interference")
                m_ws->startInterference();
            else
                m_ws->startMeasure();
        }
        else if (tool == "section")
            toggleSection();
        else if (tool == "printability")
            m_ws->startPrintability();
        else if (tool == "learn")
            m_learnDock->show();
        else if (tool.startsWith("learn:")) {
            m_learnDock->show();
            m_learn->selectLesson(tool.mid(6).toInt());
        }
        else if (tool == "learn-list") {
            m_learnDock->show();
            QTimer::singleShot(300, this, [this] { m_learn->showLessonList(); });
        }
        else if (tool == "sketch-select") {
            if (auto* e = dynamic_cast<SketchEditor*>(m_ws->session()))
                e->selectAllCurves();
        }
        else if (tool == "tour") {
            m_learnDock->show();
            m_learn->startTour();
        }
        else if (tool == "drawing")
            showDrawing();
        else if (tool == "parameters")
            QTimer::singleShot(100, this, [this] { showParameters(); });
        else if (tool == "properties" && snap && !snap->bodies.empty())
            m_ws->setSelection({m_ws->bodyItem(snap->bodies.front().id)});
        // ---- v0.5 QA / docs hooks (nothing is saved in the settings)
        else if (tool == "essential")
            setProfile(true, false);
        else if (tool.startsWith("card:")) {
            const QString name = tool.mid(5);
            if (QWidget* card = m_richTips->makeCard(name, name == "Hole / Insert" ? "Shift+H" : name == "Shell" ? "H" : ""))
                QTimer::singleShot(200, this, [this, card] {
                    card->move(mapToGlobal(QPoint(width() / 2 - card->width() / 2, 90)));
                    card->show();
                });
        }
        else if (tool == "bed" || tool == "bed-small") {
            Workspace::Bed b = m_ws->bed();
            b.on = true;
            if (tool == "bed-small") {
                b.name = "Prusa Mini";
                b.size = {180, 180, 180};
            }
            m_ws->setBed(b);
            m_vp->fitAll(false);
        }
        else if (tool == "hole" && snap) {
            // the largest upward planar face, then the Hole / Insert tool on it
            const MeshFace* best = nullptr;
            Uuid owner;
            int bi = -1, fi = -1;
            for (size_t b = 0; b < snap->bodies.size(); ++b)
                for (size_t f = 0; f < snap->bodies[b].mesh.faces.size(); ++f) {
                    const MeshFace& mf = snap->bodies[b].mesh.faces[f];
                    if (mf.planar && mf.plane.normal.z > 0.99 && (!best || mf.sig.size > best->sig.size)) {
                        best = &mf;
                        owner = snap->bodies[b].id;
                        bi = int(b);
                        fi = int(f);
                    }
                }
            if (best) {
                SelItem s;
                s.kind = SelKind::Face;
                s.owner = owner;
                s.name = best->name;
                s.sig = best->sig;
                s.planar = true;
                s.plane = best->plane;
                s.object = bi;
                s.index = fi;
                m_ws->setSelection({s});
                m_ws->startFeature(FeatureType::FastenerHole);
            }
        }
        else if (tool == "components")
            QTimer::singleShot(100, this, [this] { insertComponent(); });
        else if (tool == "enclosure")
            QTimer::singleShot(100, this, [this] { m_ws->startEnclosureWizard(); });
        else if (tool == "recorder") {
            m_learnDock->show();
            QTimer::singleShot(100, this, [this] {
                LessonRecorder dlg(m_model, this);
                dlg.exec();
            });
        }
        else if (tool == "tutor") {
            m_learnDock->show();
            m_model->setTutorMode(true);
        }
        else if (tool == "ghost" && !m_model->doc().parameters.empty()) {
            // the first parameter grows by 30 %: the previous state stays as a ghost
            m_ws->holdGhost(true);
            std::vector<Parameter> ps = m_model->doc().parameters;
            ps.front().expression = std::to_string(ps.front().value * 1.3);
            evaluateParameters(ps);
            m_model->execute(SetParametersCommand(ps));
        }
        else
            statusBar()->showMessage(tr("Unknown tool %1").arg(tool), 5000);
    });
}

void MainWindow::scheduleScreenshot(const QString& path)
{
    auto shoot = [this, path] {
        QTimer::singleShot(1500, this, [this, path] {
            if (QWidget* modal = QApplication::activeModalWidget())
                modal->grab().save(path);
            else if (m_drawing && m_drawing->isVisible())
                m_drawing->grab().save(path);
            else {
                QPixmap pm = grab();
                QPainter p(&pm);
                if (QWidget* pop = QApplication::activePopupWidget())  // open drop-down (QA of menus)
                    p.drawPixmap(mapFromGlobal(pop->mapToGlobal(QPoint(0, 0))), pop->grab());
                for (QWidget* w : QApplication::topLevelWidgets())  // v0.5: intent cards
                    if (w->isVisible() && (w->windowFlags() & Qt::ToolTip) == Qt::ToolTip && w != this)
                        p.drawPixmap(mapFromGlobal(w->mapToGlobal(QPoint(0, 0))), w->grab());
                p.end();
                pm.save(path);
            }
            qApp->quit();
        });
    };
    if (m_model->snapshot() && m_model->snapshot()->bbox.valid() && !m_model->busy())
        shoot();
    else {
        auto* conn = new QMetaObject::Connection;
        *conn = connect(m_model, &ModelController::snapshotChanged, this, [this, shoot, conn] {
            if (m_model->busy())
                return;
            disconnect(*conn);
            delete conn;
            shoot();
        });
    }
    QTimer::singleShot(60000, this, [] { qApp->quit(); });  // safety net
}

}  // namespace silex::ui
