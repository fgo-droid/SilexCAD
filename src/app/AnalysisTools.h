#pragma once
// v0.3 Lot 1 — analysis sessions (no feature is created, the model is never modified):
//   MeasureTool       (I)        distance / ΔX ΔY ΔZ / angle / Ø R centre between 1 or 2 entities
//   InterferenceTool  (Shift+I)  collisions (red volumes) and functional clearance (FDM colour code)

#include <QPointer>
#include <vector>

#include "Workspace.h"
#include "kernel/KernelEngine.h"

class QCheckBox;
class QLabel;
class QListWidget;

namespace silex::ui {

class MeasureTool final : public EditSession
{
public:
    explicit MeasureTool(Workspace& ws);
    QString title() const override { return QObject::tr("Measure"); }
    QWidget* createPanel(QWidget* parent) override;
    uint32_t pickFilter() const override { return SelFilter::Faces | SelFilter::Edges; }
    void onPicked(const SelItem& item, bool additive) override;
    void onSnapshot() override;
    void paintOverlay(QPainter& p) override;
    bool commit() override { return true; }
    void cancel() override {}
    std::vector<MenuAction> menuActions() override;

private:
    void compute();
    void updatePanel();
    void syncSelection();

    std::vector<SelItem> m_items;
    bool m_wholeBodies = false;
    MeasureResult m_result;
    QPointer<QLabel> m_label;
};

class InterferenceTool final : public EditSession
{
public:
    explicit InterferenceTool(Workspace& ws);
    ~InterferenceTool() override;
    QString title() const override { return QObject::tr("Interference check"); }
    QWidget* createPanel(QWidget* parent) override;
    uint32_t pickFilter() const override { return SelFilter::Faces | SelFilter::Edges; }
    void onPicked(const SelItem& item, bool additive) override;
    void onSnapshot() override;
    void paintOverlay(QPainter& p) override;
    bool commit() override { return true; }
    void cancel() override {}
    std::vector<MenuAction> menuActions() override;

private:
    void compute();
    void updatePanel();
    void fillBodyList();

    std::vector<Uuid> m_bodies;  ///< empty = every body
    bool m_xray = true;
    InterferenceReport m_report;
    QPointer<QListWidget> m_bodyList;
    QPointer<QLabel> m_label;
    bool m_updating = false;
};

}  // namespace silex::ui
