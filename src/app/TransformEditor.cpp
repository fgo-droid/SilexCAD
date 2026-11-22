#include "TransformEditor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "ExprSpinBox.h"
#include "Icons.h"
#include "model/Bindings.h"
#include "ModelController.h"
#include "Theme.h"
#include "Viewport.h"

namespace silex::ui {

TransformEditor::TransformEditor(Workspace& ws, std::string type, FeaturePtr existing)
    : EditSession(ws), m_type(std::move(type)), m_existing(std::move(existing))
{
    if (m_existing) {
        m_id = m_existing->id;
        m_name = m_existing->name;
        if (m_existing->params.contains("expr") && m_existing->params["expr"].is_object())
            for (const auto& [ptr, e] : m_existing->params["expr"].items())
                if (e.is_string())
                    m_exprs[ptr] = e.get<std::string>();
        if (isMirror()) {
            const auto p = m_existing->as<MirrorParams>();
            m_targets = p.targets;
            m_ref1 = p.plane;
            m_merge = p.merge;
        }
        else if (isLinear()) {
            const auto p = m_existing->as<LinearPatternParams>();
            m_targets = p.targets;
            m_d1 = p.dir1;
            m_d2 = p.dir2;
            m_ref1 = m_d1.ref;
            m_ref2 = m_d2.ref;
        }
        else {
            const auto p = m_existing->as<CircularPatternParams>();
            m_targets = p.targets;
            m_ref1 = p.axis;
            m_count = p.count;
            m_angle = p.totalAngle;
            m_equal = p.equalSpacing;
            m_reverse = p.reverse;
        }
    }
    else {
        m_id = newUuid();
        m_name = ws.model()->doc().nextName(m_type == FeatureType::LinearPattern     ? "LinearPattern"
                                            : m_type == FeatureType::CircularPattern ? "CircularPattern"
                                                                                     : "Mirror");
        m_ref1 = isMirror() ? GeomRef{"YZ", {}} : isLinear() ? GeomRef{"X", {}} : GeomRef{"Z", {}};
        // Seed the targets with the bodies already selected (selection first, then tool).
        for (const Uuid& b : ws.selectedBodies())
            m_targets.ids.push_back(b);
    }
    syncSelection();
    pushPreview();
}

int TransformEditor::insertIndex() const
{
    const DocState& d = m_ws.model()->doc();
    return m_existing ? d.indexOf(m_id) : d.marker;
}

// ------------------------------------------------------------------ document

FeaturePtr TransformEditor::buildNode() const
{
    auto n = m_existing ? std::make_shared<FeatureNode>(*m_existing) : std::make_shared<FeatureNode>();
    n->id = m_id;
    n->type = m_type;
    n->name = m_name;
    if (isMirror()) {
        MirrorParams p;
        p.targets = m_targets;
        p.plane = m_ref1;
        p.merge = m_merge;
        n->params = Json(p);
    }
    else if (isLinear()) {
        LinearPatternParams p;
        p.targets = m_targets;
        p.dir1 = m_d1;
        p.dir1.ref = m_ref1;
        p.dir2 = m_d2;
        p.dir2.ref = m_ref2;
        n->params = Json(p);
    }
    else {
        CircularPatternParams p;
        p.targets = m_targets;
        p.axis = m_ref1;
        p.count = m_count;
        p.totalAngle = m_angle;
        p.equalSpacing = m_equal;
        p.reverse = m_reverse;
        n->params = Json(p);
    }
    for (const auto& [ptr, e] : m_exprs)
        setFieldExpression(n->params, ptr, e);
    return n;
}

DocState TransformEditor::previewDoc() const
{
    DocState d = m_ws.model()->doc();
    if (m_existing) {
        d = UpdateFeatureCommand(buildNode()).apply(d);
        d.marker = d.indexOf(m_id) + 1;  // later features rolled back while editing
    }
    else {
        d = AddFeatureCommand(buildNode()).apply(d);
    }
    return d;
}

void TransformEditor::pushPreview()
{
    if (m_targets.ids.empty()) {
        DocState d = m_ws.model()->doc();
        if (m_existing)
            d.marker = d.indexOf(m_id);
        m_ws.model()->preview(d, {});
        return;
    }
    m_ws.model()->preview(previewDoc(), m_id);
}

bool TransformEditor::commit()
{
    if (m_targets.ids.empty()) {
        m_ws.status(m_targets.type == "body" ? QObject::tr("Click the bodies to copy")
                                             : QObject::tr("Tick the features to copy"));
        return false;
    }
    if (m_existing)
        m_ws.model()->execute(UpdateFeatureCommand(buildNode()));
    else
        m_ws.model()->execute(AddFeatureCommand(buildNode()));
    return true;
}

// ------------------------------------------------------------------ picking & highlight

void TransformEditor::onPicked(const SelItem& item, bool)
{
    if (!item.valid())
        return;
    if (m_slot != Slot::Targets) {
        GeomRef r;
        r.kind = item.kind == SelKind::Edge ? "Edge" : "Face";
        r.ref = item.ref();
        if (isMirror() && (r.kind != "Face" || !item.planar)) {
            m_ws.status(QObject::tr("The mirror plane must be a planar face"));
            return;
        }
        (m_slot == Slot::Ref1 ? m_ref1 : m_ref2) = r;
        m_slot = Slot::Targets;
    }
    else if (m_targets.type == "body" && (item.kind == SelKind::Face || item.kind == SelKind::Edge)) {
        auto it = std::find(m_targets.ids.begin(), m_targets.ids.end(), item.owner);
        if (it != m_targets.ids.end())
            m_targets.ids.erase(it);
        else
            m_targets.ids.push_back(item.owner);
    }
    else {
        return;
    }
    syncSelection();
    pushPreview();
    updatePanel();
}

void TransformEditor::syncSelection()
{
    std::vector<SelItem> sel;
    if (m_targets.type == "body")
        for (const Uuid& b : m_targets.ids)
            if (SelItem s = m_ws.bodyItem(b); s.valid())
                sel.push_back(s);
    for (const GeomRef* r : {&m_ref1, &m_ref2}) {
        if (r->kind == "Edge")
            if (SelItem s = m_ws.findEdge(r->ref); s.valid())
                sel.push_back(s);
        if (r->kind == "Face")
            if (auto snap = m_ws.model()->snapshot())
                for (size_t b = 0; b < snap->bodies.size(); ++b) {
                    const auto& faces = snap->bodies[b].mesh.faces;
                    for (size_t f = 0; f < faces.size(); ++f)
                        if (faces[f].name == r->ref.name) {
                            SelItem s;
                            s.kind = SelKind::Face;
                            s.owner = snap->bodies[b].id;
                            s.object = int(b);
                            s.index = int(f);
                            s.name = faces[f].name;
                            s.sig = faces[f].sig;
                            sel.push_back(s);
                        }
                }
    }
    m_ws.setSelection(std::move(sel));
}

void TransformEditor::onSnapshot()
{
    syncSelection();
    updatePanel();
}

std::vector<MenuAction> TransformEditor::menuActions()
{
    return {
        {QObject::tr("OK"), icon(IconId::Finish), [ws = &m_ws] { ws->finishSession(); }},
        {QObject::tr("Cancel"), icon(IconId::Cancel), [ws = &m_ws] { ws->cancelSession(); }},
    };
}

void TransformEditor::paintOverlay(QPainter& p)
{
    QString hint;
    if (m_slot != Slot::Targets)
        hint = isMirror() ? QObject::tr("Click a planar face to use as mirror plane")
                          : QObject::tr("Click an edge or a face to use as %1").arg(isCircular() ? "axis" : "direction");
    else if (m_targets.ids.empty())
        hint = m_targets.type == "body" ? QObject::tr("Click the bodies to copy")
                                        : QObject::tr("Tick the features to copy in the inspector");
    else
        hint = QObject::tr("%n target(s) — preview in transparency", nullptr, int(m_targets.ids.size()));
    QFont f = p.font();
    f.setPixelSize(12);
    f.setBold(true);
    p.setFont(f);
    const QRectF box(12, 12, 380, 52);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(20, 24, 30, 200));
    p.drawRoundedRect(box, 6, 6);
    p.setPen(QColor(230, 233, 237));
    p.drawText(box.adjusted(10, 6, -10, -26), Qt::AlignLeft | Qt::AlignVCenter, QString::fromStdString(m_name));
    f.setBold(false);
    f.setPixelSize(11);
    p.setFont(f);
    p.setPen(m_slot != Slot::Targets ? QColor(0xff, 0xd0, 0x40) : QColor(170, 178, 188));
    p.drawText(box.adjusted(10, 28, -10, -4), Qt::AlignLeft | Qt::AlignVCenter, hint);
    p.setBrush(Qt::NoBrush);
}

// ------------------------------------------------------------------ panel

QString TransformEditor::refText(const GeomRef& r) const
{
    if (r.kind == "Edge")
        return QObject::tr("Picked edge");
    if (r.kind == "Face")
        return QObject::tr("Picked face");
    return QString::fromStdString(r.kind);
}

QComboBox* TransformEditor::makeRefCombo(QWidget* parent, bool planes)
{
    auto* box = new QComboBox(parent);
    const QStringList base = planes ? QStringList{"XY", "XZ", "YZ"} : QStringList{"X", "Y", "Z"};
    for (const QString& k : base)
        box->addItem(planes ? QObject::tr("Plane %1").arg(k) : QObject::tr("Axis %1").arg(k), k);
    return box;
}

void TransformEditor::syncRefCombo(QComboBox* box, const GeomRef& ref, bool planes)
{
    if (!box)
        return;
    const int picked = box->findData("picked");
    if (ref.kind == "Edge" || ref.kind == "Face") {
        if (picked < 0)
            box->addItem(refText(ref), "picked");
        else
            box->setItemText(picked, refText(ref));
        box->setCurrentIndex(box->findData("picked"));
    }
    else {
        if (picked >= 0)
            box->removeItem(picked);
        box->setCurrentIndex(std::max(0, box->findData(QString::fromStdString(ref.kind))));
    }
    (void)planes;
}

void TransformEditor::onRefCombo(QComboBox* box, GeomRef& ref)
{
    if (m_updating)
        return;
    const QString k = box->currentData().toString();
    if (k == "picked")
        return;
    ref = GeomRef{k.toStdString(), {}};
    syncSelection();
    pushPreview();
}

void TransformEditor::fillFeatureList()
{
    if (!m_featureList)
        return;
    m_updating = true;
    m_featureList->clear();
    const DocState& d = m_ws.model()->doc();
    const int limit = insertIndex();
    for (int i = 0; i < limit && i < int(d.features.size()); ++i) {
        const FeatureNode& f = *d.features[size_t(i)];
        if (f.type != FeatureType::Extrude && f.type != FeatureType::Revolve)
            continue;
        auto* item = new QListWidgetItem(QString::fromStdString(f.name), m_featureList);
        item->setData(Qt::UserRole, QString::fromStdString(f.id));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        const bool on = std::find(m_targets.ids.begin(), m_targets.ids.end(), f.id) != m_targets.ids.end();
        item->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    }
    m_updating = false;
}

QWidget* TransformEditor::createPanel(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* lay = new QVBoxLayout(w);
    auto* title = new QLabel(QString("<b>%1</b>").arg(QString::fromStdString(m_name)), w);
    title->setStyleSheet("font-size: 15px;");
    lay->addWidget(title);

    // ---- targets
    auto* tgt = new QGroupBox(QObject::tr("Targets"), w);
    auto* tf = new QFormLayout(tgt);
    m_typeBox = new QComboBox(tgt);
    m_typeBox->addItem(QObject::tr("Bodies"), "body");
    m_typeBox->addItem(QObject::tr("Features"), "feature");
    tf->addRow(QObject::tr("Type"), m_typeBox);
    auto* row = new QHBoxLayout();
    m_targetsLabel = new QLabel(tgt);
    auto* clear = new QPushButton(QObject::tr("Clear"), tgt);
    row->addWidget(m_targetsLabel, 1);
    row->addWidget(clear);
    tf->addRow(QObject::tr("Selected"), row);
    m_featureList = new QListWidget(tgt);
    m_featureList->setMaximumHeight(120);
    tf->addRow(m_featureList);
    lay->addWidget(tgt);
    QObject::connect(m_typeBox, &QComboBox::currentIndexChanged, m_typeBox, [this](int) {
        if (m_updating)
            return;
        m_targets.type = m_typeBox->currentData().toString().toStdString();
        m_targets.ids.clear();
        fillFeatureList();
        syncSelection();
        pushPreview();
        updatePanel();
    });
    QObject::connect(clear, &QPushButton::clicked, clear, [this] {
        m_targets.ids.clear();
        fillFeatureList();
        syncSelection();
        pushPreview();
        updatePanel();
    });
    QObject::connect(m_featureList, &QListWidget::itemChanged, m_featureList, [this](QListWidgetItem* it) {
        if (m_updating)
            return;
        const Uuid id = it->data(Qt::UserRole).toString().toStdString();
        std::erase(m_targets.ids, id);
        if (it->checkState() == Qt::Checked)
            m_targets.ids.push_back(id);
        pushPreview();
        updatePanel();
    });

    auto pickButton = [&](QWidget* p, Slot slot) {
        auto* b = new QPushButton(QObject::tr("Pick"), p);
        b->setToolTip(QObject::tr("Then click an edge or a face in the 3D view"));
        b->setCheckable(true);
        QObject::connect(b, &QPushButton::toggled, b, [this, slot](bool on) {
            m_slot = on ? slot : Slot::Targets;
            m_ws.viewport()->refreshOverlay();
        });
        return b;
    };
    auto refRow = [&](QWidget* p, QFormLayout* f, const QString& label, QPointer<QComboBox>& box,
                      QPointer<QPushButton>& pick, Slot slot, GeomRef& ref, bool planes) {
        auto* r = new QHBoxLayout();
        box = makeRefCombo(p, planes);
        pick = pickButton(p, slot);
        r->addWidget(box, 1);
        r->addWidget(pick);
        f->addRow(label, r);
        QComboBox* b = box;
        QObject::connect(b, &QComboBox::currentIndexChanged, b, [this, b, &ref](int) { onRefCombo(b, ref); });
    };
    auto bindExpr = [&](ExprSpinBox* s, const std::string& ptr, double v) {
        auto it = m_exprs.find(ptr);
        s->setValueAndExpression(v, it != m_exprs.end() ? it->second : std::string{});
        QObject::connect(s, &ExprSpinBox::exprChanged, s, [this, ptr](const QString& e) {
            if (e.isEmpty())
                m_exprs.erase(ptr);
            else
                m_exprs[ptr] = e.toStdString();
        });
    };
    auto spinI = [&](QWidget* p, int lo, int hi, int& v, const std::string& ptr) {
        auto* s = new ExprSpinBox(p);
        s->setDecimals(0);
        s->setRange(lo, hi);
        bindExpr(s, ptr, v);
        QObject::connect(s, &QDoubleSpinBox::valueChanged, s, [this, &v](double x) {
            v = int(std::lround(x));
            pushPreview();
        });
        return s;
    };
    auto spinD = [&](QWidget* p, double lo, double hi, double& v, const QString& suffix, const std::string& ptr) {
        auto* s = new ExprSpinBox(p);
        s->setRange(lo, hi);
        s->setDecimals(2);
        s->setSuffix(suffix);
        bindExpr(s, ptr, v);
        QObject::connect(s, &QDoubleSpinBox::valueChanged, s, [this, &v](double x) {
            v = x;
            pushPreview();
        });
        return s;
    };
    auto check = [&](QWidget* p, const QString& text, bool& v) {
        auto* c = new QCheckBox(text, p);
        c->setChecked(v);
        QObject::connect(c, &QCheckBox::toggled, c, [this, &v](bool on) {
            v = on;
            pushPreview();
        });
        return c;
    };

    // ---- type-specific parameters
    if (isMirror()) {
        auto* g = new QGroupBox(QObject::tr("Mirror plane"), w);
        auto* f = new QFormLayout(g);
        refRow(g, f, QObject::tr("Plane"), m_ref1Box, m_pick1, Slot::Ref1, m_ref1, true);
        f->addRow("", check(g, QObject::tr("Join if touching"), m_merge));
        lay->addWidget(g);
    }
    else if (isLinear()) {
        auto* g1 = new QGroupBox(QObject::tr("Direction 1"), w);
        auto* f1 = new QFormLayout(g1);
        refRow(g1, f1, QObject::tr("Direction"), m_ref1Box, m_pick1, Slot::Ref1, m_ref1, false);
        f1->addRow(QObject::tr("Count"), spinI(g1, 2, 2000, m_d1.count, "/dir1/count"));
        f1->addRow(QObject::tr("Spacing"), spinD(g1, 0.01, 1e5, m_d1.spacing, " mm", "/dir1/spacing"));
        f1->addRow("", check(g1, QObject::tr("Reverse"), m_d1.reverse));
        f1->addRow("", check(g1, QObject::tr("Symmetric"), m_d1.symmetric));
        lay->addWidget(g1);
        auto* g2 = new QGroupBox(QObject::tr("Direction 2 (optional)"), w);
        auto* f2 = new QFormLayout(g2);
        refRow(g2, f2, QObject::tr("Direction"), m_ref2Box, m_pick2, Slot::Ref2, m_ref2, false);
        f2->addRow(QObject::tr("Count"), spinI(g2, 1, 2000, m_d2.count, "/dir2/count"));
        f2->addRow(QObject::tr("Spacing"), spinD(g2, 0.01, 1e5, m_d2.spacing, " mm", "/dir2/spacing"));
        f2->addRow("", check(g2, QObject::tr("Reverse"), m_d2.reverse));
        f2->addRow("", check(g2, QObject::tr("Symmetric"), m_d2.symmetric));
        lay->addWidget(g2);
    }
    else {
        auto* g = new QGroupBox(QObject::tr("Axis and occurrences"), w);
        auto* f = new QFormLayout(g);
        refRow(g, f, QObject::tr("Axis"), m_ref1Box, m_pick1, Slot::Ref1, m_ref1, false);
        f->addRow(QObject::tr("Count"), spinI(g, 2, 2000, m_count, "/count"));
        f->addRow(QObject::tr("Total angle"), spinD(g, -360, 360, m_angle, QString(QChar(0xB0)), "/totalAngle"));
        f->addRow("", check(g, QObject::tr("Equal spacing"), m_equal));
        f->addRow("", check(g, QObject::tr("Reverse"), m_reverse));
        lay->addWidget(g);
    }

    m_message = new QLabel(w);
    m_message->setWordWrap(true);
    lay->addWidget(m_message);
    lay->addStretch(1);
    auto* ok = new QPushButton(icon(IconId::Finish), QObject::tr("OK"), w);
    ok->setDefault(true);
    auto* cancel = new QPushButton(icon(IconId::Cancel), QObject::tr("Cancel"), w);
    QObject::connect(ok, &QPushButton::clicked, ok, [ws = &m_ws] { ws->finishSession(); });
    QObject::connect(cancel, &QPushButton::clicked, cancel, [ws = &m_ws] { ws->cancelSession(); });
    lay->addWidget(ok);
    lay->addWidget(cancel);

    fillFeatureList();
    updatePanel();
    return w;
}

void TransformEditor::updatePanel()
{
    m_updating = true;
    if (m_typeBox)
        m_typeBox->setCurrentIndex(m_targets.type == "feature" ? 1 : 0);
    if (m_featureList)
        m_featureList->setVisible(m_targets.type == "feature");
    if (m_targetsLabel)
        m_targetsLabel->setText(m_targets.type == "body"
                                    ? QObject::tr("%n body(ies)", nullptr, int(m_targets.ids.size()))
                                    : QObject::tr("%n feature(s)", nullptr, int(m_targets.ids.size())));
    syncRefCombo(m_ref1Box, m_ref1, isMirror());
    syncRefCombo(m_ref2Box, m_ref2, false);
    for (QPushButton* b : {m_pick1.data(), m_pick2.data()})
        if (b) {
            b->blockSignals(true);
            b->setChecked((b == m_pick1 && m_slot == Slot::Ref1) || (b == m_pick2 && m_slot == Slot::Ref2));
            b->blockSignals(false);
        }
    if (m_message) {
        QString msg;
        QColor c = Colors::Warning;
        if (auto snap = m_ws.model()->snapshot())
            if (const FeatureResult* r = snap->result(m_id)) {
                if (r->status == FeatureStatus::Error) {
                    msg = QString::fromStdString(r->message);
                    c = Colors::Error;
                }
                else if (r->status == FeatureStatus::Warning)
                    msg = QString::fromStdString(r->message);
            }
        m_message->setText(msg.isEmpty() ? QString()
                                         : QString("<span style='color:%1'>%2</span>").arg(c.name(), msg.toHtmlEscaped()));
    }
    m_updating = false;
}

}  // namespace silex::ui
