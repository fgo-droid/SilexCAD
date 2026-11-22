#pragma once
// v0.7 lot A — "Text (Emboss / Engrave)" tool (Create ▾, Shift+T outside a sketch, right click on a face).
// Click a planar face (or keep the selected one): the text is centered where you clicked, snapped to the
// face center. Text with {parameters}, font, height, depth (> 0 relief, < 0 engraving), rotation,
// alignment and position in the panel, live preview. An orange warning appears when the letter strokes
// are thinner than the FDM nozzle.

#include <QPointer>
#include <map>
#include <optional>
#include <string>

#include "Workspace.h"
#include "model/Features.h"

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QToolButton;

namespace silex::ui {

class ExprSpinBox;

class TextEditor final : public EditSession
{
public:
    TextEditor(Workspace& ws, FeaturePtr existing);
    ~TextEditor() override;

    QString title() const override { return QString::fromStdString(m_name); }
    QWidget* createPanel(QWidget* parent) override;
    uint32_t pickFilter() const override;
    bool mousePress(QMouseEvent* e) override;
    void paintOverlay(QPainter& p) override;
    void onSnapshot() override;
    bool commit() override;
    void cancel() override {}
    std::vector<MenuAction> menuActions() override;
    Uuid editedFeature() const override { return m_existing ? m_id : Uuid{}; }

private:
    std::optional<Placement> plane() const;
    FeaturePtr buildNode() const;
    void pushPreview();
    void syncSelection();
    void updatePanel();
    void updateHints();
    bool ready() const { return m_tp.plane.kind != "Face" || !m_tp.plane.face.name.empty(); }

    FeaturePtr m_existing;
    Uuid m_id;
    std::string m_name;
    EmbossTextParams m_tp;
    bool m_placed = false;
    std::map<std::string, std::string> m_exprs;  ///< json pointer -> expression (height, depth...)

    QPointer<QPlainTextEdit> m_text;
    QPointer<QComboBox> m_fontBox, m_alignBox;
    QPointer<ExprSpinBox> m_heightBox, m_depthBox, m_angleBox, m_xBox, m_yBox;
    QPointer<QLabel> m_resolved, m_fdm, m_message, m_where;
    QPointer<QToolButton> m_paramButton;
    bool m_updating = false;
};

}  // namespace silex::ui
