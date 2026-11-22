#pragma once
#include <QMainWindow>
#include <QPointer>

#include "model/Uuid.h"

class QLabel;
class QScrollArea;
class QDockWidget;
class QToolBar;
class QAction;
class QToolButton;
namespace silex::ui { class LearnPanel; }

namespace silex::ui {

class ModelController;
class AutosaveManager;
class Viewport;
class Workspace;
class Timeline;
class SectionPanel;
class DrawingWindow;

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
    /// v0.3 docs / CI: opens a tool once the model is displayed: measure, interference, section,
    /// drawing, parameters (a screenshot then captures the drawing window for "drawing").
    void scheduleTool(const QString& tool);

    /// Status bar AI indicator: grey = listening without client, green = an agent is connected,
    /// orange = channel already taken (IPC disabled), Off = started with --no-ipc.
    enum class McpState { Off, Ready, Connected, Disabled };
    void setMcpState(McpState s, const QString& detail = {});
    /// Start-up: offers to restore the autosaves of sessions that ended unexpectedly.
    void offerRecovery();
    /// `silex --recover <file.autosave.json>`: restores that autosave without asking.
    bool recoverFrom(const QString& autosaveFile);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void buildActions();
    void buildUi();
    void refreshTitle();
    void onSessionChanged();
    void rebuildDefaultInspector();
    void insertComponent();  ///< v0.5 lot 1: library dialog
    void setProfile(bool essential, bool persist = true);  ///< v0.5 lot 3: Essential / Complete interface
    void setPanel(QWidget* w);
    bool maybeSave();
    bool doSave(bool saveAs);
    void doOpen();
    void doNew();
    void doImport();
    void doExport();
    void doExportStl();
    void doExport3mf();
    void showParameters();
    void showDrawing();
    void toggleSection();
    QWidget* bodyPropertiesPanel(QWidget* parent, const Uuid& bodyId);

    ModelController* m_model = nullptr;
    AutosaveManager* m_autosave = nullptr;
    QToolButton* buildHelpButton(QWidget* parent);
    Viewport* m_vp = nullptr;
    Workspace* m_ws = nullptr;
    Timeline* m_timeline = nullptr;
    QDockWidget* m_inspector = nullptr;
    QScrollArea* m_inspectorArea = nullptr;
    QPointer<QWidget> m_panel;
    QLabel* m_statusRight = nullptr;
    QToolBar* m_sketchBar = nullptr;
    // ergonomics lot: live status bar
    QLabel* m_mcpLabel = nullptr;
    QLabel* m_triLabel = nullptr;
    QLabel* m_msLabel = nullptr;
    QToolButton* m_snapButton = nullptr;
    McpState m_mcpState = McpState::Off;
    QString m_mcpDetail;
    // v0.5 lot 3: interface profile & intent cards
    QToolButton* m_profileButton = nullptr;
    QList<QAction*> m_proActions, m_essentialActions;
    class RichTips* m_richTips = nullptr;
    bool m_essential = false;
    double m_lastSnapStep = 1.0;
    void updateSnapButton();
    // v0.4 lot 3: guided mode
    QDockWidget* m_learnDock = nullptr;
    LearnPanel* m_learn = nullptr;
    void pulseTool(const QString& tool, const QString& text);
    QAction* m_undo = nullptr;
    QAction* m_redo = nullptr;
    bool m_firstFit = true;
    QDockWidget* m_sectionDock = nullptr;
    SectionPanel* m_section = nullptr;
    QPointer<DrawingWindow> m_drawing;
};

}  // namespace silex::ui
