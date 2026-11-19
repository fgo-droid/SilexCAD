#include "Timeline.h"

#include <QContextMenuEvent>
#include <QHelpEvent>
#include <QInputDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

#include "Icons.h"
#include "ModelController.h"
#include "Theme.h"

namespace silex::ui {

namespace {
constexpr double kLeft = 150;  // room for the transport buttons
constexpr double kStep = 42;
constexpr double kSize = 34;

IconId iconFor(const std::string& type)
{
    if (type == FeatureType::Sketch) return IconId::Sketch;
    if (type == FeatureType::Extrude) return IconId::Extrude;
    if (type == FeatureType::Revolve) return IconId::Revolve;
    if (type == FeatureType::Fillet) return IconId::Fillet;
    if (type == FeatureType::Chamfer) return IconId::Chamfer;
    if (type == FeatureType::ImportStep) return IconId::ImportStep;
    if (type == FeatureType::Combine) return IconId::Combine;
    if (type == FeatureType::Shell) return IconId::Shell;
    if (type == FeatureType::Mirror) return IconId::Mirror;
    if (type == FeatureType::LinearPattern) return IconId::LinearPattern;
    if (type == FeatureType::CircularPattern) return IconId::CircularPattern;
    return IconId::Body;
}
}  // namespace

Timeline::Timeline(ModelController* model, QWidget* parent) : QWidget(parent), m_model(model)
{
    setMouseTracking(true);
    setMinimumHeight(62);
    connect(model, &ModelController::documentChanged, this, qOverload<>(&QWidget::update));
    connect(model, &ModelController::snapshotChanged, this, qOverload<>(&QWidget::update));
}

void Timeline::setEditing(const Uuid& id)
{
    m_editing = id;
    update();
}

QRectF Timeline::itemRect(int i) const { return {kLeft + i * kStep, (height() - kSize) / 2.0, kSize, kSize}; }

QRectF Timeline::buttonRect(int i) const { return {10.0 + i * 32.0, (height() - 26) / 2.0, 26.0, 26.0}; }

double Timeline::markerX(int marker) const { return kLeft + marker * kStep - (kStep - kSize) / 2.0; }

int Timeline::itemAt(QPointF p) const
{
    const auto& f = m_model->doc().features;
    for (int i = 0; i < int(f.size()); ++i)
        if (itemRect(i).adjusted(-3, -6, 3, 6).contains(p))
            return i;
    return -1;
}

int Timeline::gapAt(double x) const
{
    const int n = int(m_model->doc().features.size());
    const int g = int(std::lround((x - kLeft + (kStep - kSize) / 2.0) / kStep));
    return std::clamp(g, 0, n);
}

void Timeline::moveMarker(int m)
{
    const DocState& d = m_model->doc();
    m = std::clamp(m, 0, int(d.features.size()));
    if (m != d.marker)
        m_model->execute(MoveMarkerCommand(m));
}

void Timeline::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(0x24, 0x27, 0x2c));
    p.setPen(QColor(0x3a, 0x3f, 0x47));
    p.drawLine(0, 0, width(), 0);

    // transport buttons |<  <  >  >|
    const char* labels[4] = {"|<", "<", ">", ">|"};
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(11);
    p.setFont(f);
    for (int i = 0; i < 4; ++i) {
        const QRectF r = buttonRect(i);
        p.setPen(QColor(0x4a, 0x50, 0x59));
        p.setBrush(QColor(0x30, 0x34, 0x3a));
        p.drawRoundedRect(r, 4, 4);
        p.setPen(QColor(0xc8, 0xcd, 0xd4));
        p.drawText(r, Qt::AlignCenter, labels[i]);
    }

    const DocState& d = m_model->doc();
    auto snap = m_model->snapshot();
    // track
    p.setPen(QPen(QColor(0x3c, 0x41, 0x49), 4, Qt::SolidLine, Qt::RoundCap));
    if (!d.features.empty())
        p.drawLine(QPointF(kLeft - 6, height() / 2.0), QPointF(itemRect(int(d.features.size()) - 1).right() + 6, height() / 2.0));

    for (int i = 0; i < int(d.features.size()); ++i) {
        const FeatureNode& fn = *d.features[size_t(i)];
        QRectF r = itemRect(i);
        if (m_draggingItem && i == m_pressItem)
            continue;
        const bool rolled = i >= d.marker;
        const FeatureResult* res = snap ? snap->result(fn.id) : nullptr;
        QColor bg(0x33, 0x38, 0x40);
        if (fn.id == m_editing)
            bg = QColor(0x1f, 0x5f, 0x99);
        else if (i == m_hover)
            bg = QColor(0x3e, 0x45, 0x4f);
        p.setOpacity(rolled ? 0.35 : 1.0);
        p.setPen(QPen(fn.id == m_editing ? Colors::Accent : QColor(0x4a, 0x50, 0x59), 1.2));
        p.setBrush(bg);
        p.drawRoundedRect(r, 5, 5);
        icon(iconFor(fn.type)).paint(&p, r.adjusted(5, 5, -5, -5).toRect());
        if (fn.suppressed) {
            p.setPen(QPen(QColor(0xff, 0xff, 0xff, 150), 2));
            p.drawLine(r.bottomLeft() + QPointF(4, -4), r.topRight() + QPointF(-4, 4));
        }
        if (res && !rolled && (res->status == FeatureStatus::Error || res->status == FeatureStatus::Warning)) {
            const QColor c = res->status == FeatureStatus::Error ? Colors::Error : Colors::Warning;
            p.setPen(QPen(c, 3, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(r.bottomLeft() + QPointF(3, 3), r.bottomRight() + QPointF(-3, 3));
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawEllipse(r.topRight() + QPointF(-3, 3), 4.5, 4.5);
        }
        p.setOpacity(1.0);
    }

    // drop indicator while reordering
    if (m_draggingItem && m_dropGap >= 0) {
        p.setPen(QPen(Colors::Accent, 2));
        const double x = markerX(m_dropGap);
        p.drawLine(QPointF(x, 6), QPointF(x, height() - 6));
    }

    // rollback marker
    const double mx = markerX(d.marker);
    p.setPen(QPen(QColor(0xff, 0xb0, 0x20), 3, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(mx, 6), QPointF(mx, height() - 6));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xff, 0xb0, 0x20));
    p.drawPolygon(QPolygonF{{mx - 6, 3}, {mx + 6, 3}, {mx, 11}});

    if (d.features.empty()) {
        p.setPen(QColor(0x80, 0x88, 0x92));
        p.drawText(QRectF(kLeft, 0, width() - kLeft, height()), Qt::AlignVCenter | Qt::AlignLeft,
                   tr("Timeline — start with a sketch (S) or import a STEP file"));
    }
}

void Timeline::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    const DocState& d = m_model->doc();
    for (int i = 0; i < 4; ++i)
        if (buttonRect(i).contains(e->position())) {
            const int n = int(d.features.size());
            const int targets[4] = {0, d.marker - 1, d.marker + 1, n};
            moveMarker(targets[i]);
            return;
        }
    if (std::abs(e->position().x() - markerX(d.marker)) < 7) {
        m_draggingMarker = true;
        return;
    }
    m_pressItem = itemAt(e->position());
    m_pressPos = e->position();
}

void Timeline::mouseMoveEvent(QMouseEvent* e)
{
    if (m_draggingMarker) {
        moveMarker(gapAt(e->position().x()));
        return;
    }
    if (m_pressItem >= 0 && (e->buttons() & Qt::LeftButton)) {
        if (!m_draggingItem && std::abs(e->position().x() - m_pressPos.x()) > 8)
            m_draggingItem = true;
        if (m_draggingItem) {
            m_dropGap = gapAt(e->position().x());
            update();
        }
        return;
    }
    const int h = itemAt(e->position());
    if (h != m_hover) {
        m_hover = h;
        update();
    }
    const bool overMarker = std::abs(e->position().x() - markerX(m_model->doc().marker)) < 7;
    setCursor(overMarker ? Qt::SizeHorCursor : Qt::ArrowCursor);
}

void Timeline::mouseReleaseEvent(QMouseEvent*)
{
    if (m_draggingItem && m_pressItem >= 0 && m_dropGap >= 0) {
        const DocState& d = m_model->doc();
        const Uuid id = d.features[size_t(m_pressItem)]->id;
        const int to = m_dropGap > m_pressItem ? m_dropGap - 1 : m_dropGap;
        if (to != m_pressItem) {
            DocState after = ReorderFeatureCommand(id, to).apply(d);
            if (after.indexOf(id) == d.indexOf(id))
                QToolTip::showText(QCursor::pos(), tr("Cannot move: it would break a dependency"), this);
            else
                m_model->execute(ReorderFeatureCommand(id, to));
        }
    }
    m_draggingMarker = false;
    m_draggingItem = false;
    m_pressItem = -1;
    m_dropGap = -1;
    update();
}

void Timeline::mouseDoubleClickEvent(QMouseEvent* e)
{
    const int i = itemAt(e->position());
    if (i >= 0)
        emit editRequested(m_model->doc().features[size_t(i)]->id);
}

void Timeline::contextMenuEvent(QContextMenuEvent* e)
{
    const int i = itemAt(e->pos());
    if (i < 0)
        return;
    const DocState& d = m_model->doc();
    const FeaturePtr f = d.features[size_t(i)];
    QMenu menu(this);
    menu.addAction(tr("Edit feature"), this, [this, id = f->id] { emit editRequested(id); });
    menu.addAction(tr("Rename..."), this, [this, f] {
        bool ok = false;
        const QString n = QInputDialog::getText(this, tr("Rename"), tr("Name:"), QLineEdit::Normal,
                                                QString::fromStdString(f->name), &ok);
        if (ok && !n.trimmed().isEmpty())
            m_model->execute(RenameFeatureCommand(f->id, n.trimmed().toStdString()));
    });
    menu.addAction(f->suppressed ? tr("Unsuppress") : tr("Suppress"), this,
                   [this, f] { m_model->execute(SetSuppressedCommand(f->id, !f->suppressed)); });
    menu.addAction(tr("Roll marker here"), this, [this, i] { moveMarker(i + 1); });
    menu.addSeparator();
    const auto dependents = dependentsOf(d, f->id);
    QAction* del = menu.addAction(icon(IconId::Cancel), tr("Delete"), this,
                                  [this, id = f->id] { m_model->execute(DeleteFeatureCommand(id)); });
    if (!dependents.empty())
        del->setText(tr("Delete (%n dependent feature(s) will fail)", nullptr, int(dependents.size())));
    menu.exec(e->globalPos());
}

bool Timeline::event(QEvent* e)
{
    if (e->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(e);
        const int i = itemAt(he->pos());
        if (i >= 0) {
            const FeatureNode& f = *m_model->doc().features[size_t(i)];
            QString text = QString("<b>%1</b> <span style='color:#9aa3ad'>(%2)</span>")
                               .arg(QString::fromStdString(f.name).toHtmlEscaped(), QString::fromStdString(f.type));
            if (auto snap = m_model->snapshot())
                if (const FeatureResult* r = snap->result(f.id)) {
                    if (!r->message.empty())
                        text += "<br>" + QString::fromStdString(r->message).toHtmlEscaped();
                    if (r->status == FeatureStatus::Ok || r->status == FeatureStatus::Warning)
                        text += QString("<br><span style='color:#9aa3ad'>%1 ms</span>").arg(r->milliseconds, 0, 'f', 1);
                }
            QToolTip::showText(he->globalPos(), text, this);
        }
        else
            QToolTip::hideText();
        return true;
    }
    return QWidget::event(e);
}

}  // namespace silex::ui
