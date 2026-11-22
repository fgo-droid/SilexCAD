#pragma once
// v0.4 lot 2 — FDM printability assistant: overhangs (orange 45-60°, red > 60°) and walls thinner
// than the nozzle allows (magenta), coloured on the bodies, with the areas in the inspector.

#include <QPointer>
#include <vector>

#include "Workspace.h"
#include "model/Printability.h"

class QLabel;

namespace silex::ui {

class PrintabilityTool final : public EditSession
{
public:
    explicit PrintabilityTool(Workspace& ws);
    ~PrintabilityTool() override;
    QString title() const override { return QObject::tr("Printability"); }
    QWidget* createPanel(QWidget* parent) override;
    uint32_t pickFilter() const override { return SelFilter::Faces; }
    void onPicked(const SelItem& item, bool additive) override;
    void onSnapshot() override { compute(); }
    void paintOverlay(QPainter& p) override;
    bool commit() override { return true; }
    void cancel() override {}

private:
    void compute();
    void updatePanel();

    std::vector<Uuid> m_bodies;  ///< empty = every visible body
    PrintabilityOptions m_opt;
    PrintabilityResult m_total;
    int m_dirIndex = 0;
    QPointer<QLabel> m_label;
};

}  // namespace silex::ui
