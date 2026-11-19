#pragma once
#include <QMainWindow>
#include <QPointer>

class QLabel;
class QScrollArea;
class QDockWidget;
class QToolBar;
class QAction;

namespace silex::ui {

class ModelController;
class Viewport;
class Workspace;
class Timeline;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    ModelController* model() const { return m_model; }
    bool openFile(const QString& path);
    void loadDemo();
    /// Saves a screenshot of the window once the model is displayed, then quits (CI / docs).
    void scheduleScreenshot(const QString& path);
    /// Opens the editor of the feature named `name` once the model is displayed (docs / demos).
    void scheduleEdit(const QString& name);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void buildActions();
    void buildUi();
    void refreshTitle();
    void onSessionChanged();
    void rebuildDefaultInspector();
    void setPanel(QWidget* w);
    bool maybeSave();
    bool doSave(bool saveAs);
    void doOpen();
    void doNew();
    void doImport();
    void doExport();
    void doExportStl();

    ModelController* m_model = nullptr;
    Viewport* m_vp = nullptr;
    Workspace* m_ws = nullptr;
    Timeline* m_timeline = nullptr;
    QDockWidget* m_inspector = nullptr;
    QScrollArea* m_inspectorArea = nullptr;
    QPointer<QWidget> m_panel;
    QLabel* m_statusRight = nullptr;
    QToolBar* m_sketchBar = nullptr;
    QAction* m_undo = nullptr;
    QAction* m_redo = nullptr;
    bool m_firstFit = true;
};

}  // namespace silex::ui
