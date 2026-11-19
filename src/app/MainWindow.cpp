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
#include <QToolBar>
#include <QVBoxLayout>

#include "Icons.h"
#include "MarkingMenu.h"
#include "ModelController.h"
#include "SketchEditor.h"
#include "Theme.h"
#include "Timeline.h"
#include "Viewport.h"
#include "Workspace.h"
#include "kernel/KernelEngine.h"

namespace silex::ui {

MainWindow::MainWindow()
{
    m_model = new ModelController(this);
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

    m_statusRight = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusRight);

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
        QString s = tr("%1 body(ies) · %2 ms").arg(snap->bodies.size()).arg(snap->computeMs, 0, 'f', 0);
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
    add(file, IconId::ExportStep, tr("Export STEP..."), QKeySequence(tr("Ctrl+E")), [this] { doExport(); });
    add(file, IconId::ExportStl, tr("Export STL..."), QKeySequence(tr("Ctrl+Shift+E")), [this] { doExportStl(); });
    file->addSeparator();
    m_undo = add(file, IconId::Undo, tr("Undo"), QKeySequence::Undo, [this] { m_ws->undo(); });
    m_redo = add(file, IconId::Redo, tr("Redo"), QKeySequence(tr("Ctrl+Y")), [this] { m_ws->redo(); });
    m_undo->setEnabled(false);
    m_redo->setEnabled(false);

    auto* model = addToolBar(tr("Model"));
    model->setObjectName("modelBar");
    model->setIconSize(QSize(26, 26));
    model->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    auto tool = [&](IconId ic, const QString& text, const QString& key, auto fn) {
        QAction* a = new QAction(icon(ic), text, this);
        a->setToolTip(QString("%1 (%2)").arg(text, key));
        connect(a, &QAction::triggered, this, fn);
        model->addAction(a);
    };
    tool(IconId::Sketch, tr("Sketch"), "S", [this] { m_ws->startSketch(); });
    tool(IconId::Extrude, tr("Extrude"), "E", [this] { m_ws->startFeature(FeatureType::Extrude); });
    tool(IconId::Revolve, tr("Revolve"), "R", [this] { m_ws->startFeature(FeatureType::Revolve); });
    tool(IconId::Fillet, tr("Fillet"), "F", [this] { m_ws->startFeature(FeatureType::Fillet); });
    tool(IconId::Chamfer, tr("Chamfer"), "C", [this] { m_ws->startFeature(FeatureType::Chamfer); });
    tool(IconId::Shell, tr("Shell"), "H", [this] { m_ws->startFeature(FeatureType::Shell); });
    tool(IconId::Combine, tr("Combine"), "B", [this] { m_ws->startFeature(FeatureType::Combine); });
    tool(IconId::Mirror, tr("Mirror"), "M", [this] { m_ws->startFeature(FeatureType::Mirror); });
    tool(IconId::LinearPattern, tr("Linear Pattern"), "Shift+P", [this] { m_ws->startFeature(FeatureType::LinearPattern); });
    tool(IconId::CircularPattern, tr("Circular Pattern"), "Shift+C", [this] { m_ws->startFeature(FeatureType::CircularPattern); });
    model->addSeparator();
    tool(IconId::Fit, tr("Fit"), "W", [this] { m_vp->fitAll(); });

    // Contextual sketch bar: shown only while sketching (no workbench switch).
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
    };
    sk(IconId::Line, tr("Line"), "L", [](SketchEditor* e) { e->setTool(SketchEditor::Tool::Line); });
    sk(IconId::Rectangle, tr("Rectangle"), "R", [](SketchEditor* e) { e->setTool(SketchEditor::Tool::Rectangle); });
    sk(IconId::Circle, tr("Circle"), "C", [](SketchEditor* e) { e->setTool(SketchEditor::Tool::Circle); });
    sk(IconId::Arc, tr("Arc"), "A", [](SketchEditor* e) { e->setTool(SketchEditor::Tool::Arc); });
    sk(IconId::Dimension, tr("Dimension"), "D", [](SketchEditor* e) { e->setTool(SketchEditor::Tool::Dimension); });
    sk(IconId::Construction, tr("Construction"), "X", [](SketchEditor* e) { e->toggleConstruction(); });
    m_sketchBar->addSeparator();
    QAction* finish = new QAction(icon(IconId::Finish), tr("Finish sketch"), this);
    connect(finish, &QAction::triggered, this, [this] { m_ws->finishSession(); });
    m_sketchBar->addAction(finish);
    m_sketchBar->setVisible(false);
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
            if (checked && pos == selBodies.begin() && selBodies.size() > 1)
                label += tr("  — target");
            auto* cb = new QCheckBox(label, bodiesBox);
            cb->setChecked(checked);
            if (checked && pos == selBodies.begin() && selBodies.size() > 1)
                cb->setStyleSheet(QString("color: %1;").arg(Colors::TargetBody.name()));
            connect(cb, &QCheckBox::toggled, this, [this, id = bv.id](bool) { m_ws->toggleBody(id); });
            bl->addWidget(cb);
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
           "Double-click a body: select it (Ctrl: add) · B: combine"),
        w);
    help->setWordWrap(true);
    help->setStyleSheet("color: #9aa3ad;");
    lay->addWidget(help);
    lay->addStretch(1);
    setPanel(w);
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

void MainWindow::scheduleScreenshot(const QString& path)
{
    auto shoot = [this, path] {
        QTimer::singleShot(900, this, [this, path] {
            grab().save(path);
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
