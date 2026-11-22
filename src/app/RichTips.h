#pragma once
// v0.5 lot 3 — "intent cards": a long hover (1.2 s) on a tool of the Model / Sketch bars (or on an
// entry of their drop-down menus) shows a card with the bilingual name, the shortcut, a vector
// "before -> after" thumbnail and one sentence in maker language. They replace the plain tooltips.

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QString>
#include <functional>

class QMenu;
class QTimer;
class QToolBar;
class QWidget;

namespace silex::ui {

class RichTips : public QObject
{
    Q_OBJECT
public:
    /// `french` tells the language of the sentence (the name is always bilingual).
    explicit RichTips(std::function<bool()> french, QObject* parent = nullptr);

    /// Watches the tool buttons of a toolbar (and the menus of its drop-down buttons).
    void watch(QToolBar* bar);
    /// True when a card exists for this tool (English name, e.g. "Extrude").
    static bool hasCard(const QString& tool);
    /// The card widget (for docs / tests); nullptr when unknown.
    QWidget* makeCard(const QString& tool, const QString& shortcut) const;

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void watchMenu(QMenu* menu, const QString& prefix);
    void showFor(QWidget* anchor, const QString& tool, const QString& shortcut, QPoint at);
    void hide();

    std::function<bool()> m_french;
    QTimer* m_timer;
    QPointer<QWidget> m_card;
    QPointer<QWidget> m_pendingAnchor;
    QString m_pendingTool, m_pendingKey;
    QPoint m_pendingAt;
};

}  // namespace silex::ui
