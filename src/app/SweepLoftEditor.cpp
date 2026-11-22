#include "SweepLoftEditor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

#include "Icons.h"
#include "ModelController.h"
#include "Theme.h"
#include "Viewport.h"

namespace silex::ui {

namespace {
int opIndex(BooleanOp op) { return int(op); }
BooleanOp opFromIndex(int i) { return BooleanOp(std::clamp(i, 0, 3)); }
}  // namespace

SweepLoftEditor::SweepLoftEditor(Workspace& ws, std::string type, FeaturePtr existing)
    : EditSession(ws), m_type(std::move(type)), m_existing(std::move(existing))
{
    const DocState& d = ws.model()->doc();
    if (m_existing) {
        m_id = m_existing->id;
        m_name = m_existing->name;
        if (isSweep()) {
            const auto p = m_existing->as<SweepParams>();
            for (const auto& k : p.profile.regions)
                m_profiles.emplace_back(p.profile.sketch, k);
            if (p.profile.regions.empty() && !p.profile.sketch.empty())
                m_profiles.emplace_back(p.profile.sketch, "");
            m_path = p.path;
            m_orientation = p.orientation;
            m_op = p.op;
        }
        else {
            const auto p = m_existing->as<LoftParams>();
            for (const auto& s : p.sections)
                m_profiles.emplace_back(s.sketch, s.regions.empty() ? "" : s.regions.front());
            m_solid = p.solid;
            m_ruled = p.ruled;
            m_op = p.op;
        }
    }
    else {
        m_id = newUuid();
        m_name = d.nextName(isSweep() ? "Sweep" : "Loft");
        for (const SelItem& s : ws.selection())
            if (s.kind == SelKind::Profile)
                m_profiles.emplace_back(s.owner, s.name);
        if (isSweep() && m_profiles.size() > 1)
            m_profiles.resize(1);
        if (isSweep()) {
            // default path: the most recent other sketch before the insertion point
            const Uuid profileSketch = m_profiles.empty() ? Uuid{} : m_profiles.front().first;
            for (int i = std::min(d.marker, int(d.features.size())) - 1; i >= 0; --i) {
                const auto& f = d.features[size_t(i)];
                if (f->type == FeatureType::Sketch && f->id != profileSketch && !f->suppressed) {
                    m_path.sketch = f->id;
                    break;
                }
            }
        }
        if (!ws.model()->snapshot() || ws.model()->snapshot()->bodies.empty())
            m_op = BooleanOp::NewBody;
    }
    syncSelection();
    pushPreview();
}

uint32_t SweepLoftEditor::pickFilter() const
{
    if (m_pickPathEdges)
        return SelFilter::Edges;
    return SelFilter::Profiles;
}

std::set<Uuid> SweepLoftEditor::sketchesInUse() const
{
    std::set<Uuid> s;
    for (const auto& p : m_profiles)
        s.insert(p.first);
    if (!m_path.sketch.empty())
        s.insert(m_path.sketch);
    // every sketch stays pickable while building a loft
    if (!isSweep())
        for (const auto& f : m_ws.model()->doc().features)
            if (f->type == FeatureType::Sketch)
                s.insert(f->id);
    return s;
}

bool SweepLoftEditor::ready() const
{
    if (isSweep())
        return !m_profiles.empty() && (!m_path.sketch.empty() || !m_path.edges.empty());
    return m_profiles.size() >= 2;
}

FeaturePtr SweepLoftEditor::buildNode() const
{
    auto n = m_existing ? std::make_shared<FeatureNode>(*m_existing) : std::make_shared<FeatureNode>();
    n->id = m_id;
    n->type = m_type;
    n->name = m_name;
    if (isSweep()) {
        SweepParams p;
        if (!m_profiles.empty()) {
            p.profile.sketch = m_profiles.front().first;
            if (!m_profiles.front().second.empty())
                p.profile.regions = {m_profiles.front().second};
        }
        p.path = m_path;
        p.orientation = m_orientation;
        p.op = m_op;
        n->params = Json(p);
    }
    else {
        LoftParams p;
        for (const auto& [sk, key] : m_profiles)
            p.sections.push_back(ProfileRef{sk, key.empty() ? std::vector<std::string>{} : std::vector<std::string>{key}});
        p.solid = m_solid;
        p.ruled = m_ruled;
        p.op = m_op;
        n->params = Json(p);
    }
    return n;
}

void SweepLoftEditor::pushPreview()
{
    DocState d = m_ws.model()->doc();
    if (!ready()) {
        if (m_existing)
            d.marker = d.indexOf(m_id);
        m_ws.model()->preview(d, {});
        return;
    }
    if (m_existing) {
        d = UpdateFeatureCommand(buildNode()).apply(d);
        d.marker = d.indexOf(m_id) + 1;
    }
    else {
        d = AddFeatureCommand(buildNode()).apply(d);
    }
    m_ws.model()->preview(d, m_id);
}

bool SweepLoftEditor::commit()
{
    if (!ready()) {
        m_ws.status(isSweep() ? QObject::tr("Pick a closed profile and choose a path")
                              : QObject::tr("Pick at least two profiles on different planes"));
        return false;
    }
    if (m_existing)
        m_ws.model()->execute(UpdateFeatureCommand(buildNode()));
    else
        m_ws.model()->execute(AddFeatureCommand(buildNode()));
    return true;
}

void SweepLoftEditor::onPicked(const SelItem& item, bool)
{
    if (!item.valid())
        return;
    if (m_pickPathEdges && item.kind == SelKind::Edge) {
        m_path.sketch.clear();
        m_path.curves.clear();
        auto it = std::find_if(m_path.edges.begin(), m_path.edges.end(),
                               [&](const TopoRef& r) { return r.name == item.name; });
        if (it != m_path.edges.end())
            m_path.edges.erase(it);
        else
            m_path.edges.push_back(item.ref());
    }
    else if (item.kind == SelKind::Profile) {
        const Profile pr{item.owner, item.name};
        auto it = std::find(m_profiles.begin(), m_profiles.end(), pr);
        if (it != m_profiles.end())
            m_profiles.erase(it);
        else if (isSweep())
            m_profiles = {pr};
        else
            m_profiles.push_back(pr);
        if (isSweep() && m_path.sketch == item.owner)
            m_path.sketch.clear();
    }
    else {
        return;
    }
    syncSelection();
    pushPreview();
    fillPathCombo();
    fillSectionList();
    updatePanel();
}

void SweepLoftEditor::syncSelection()
{
    std::vector<SelItem> sel;
    for (const auto& [sk, key] : m_profiles) {
        if (!key.empty()) {
            if (SelItem s = m_ws.findProfile(sk, key); s.valid())
                sel.push_back(s);
        }
        else if (auto snap = m_ws.model()->snapshot()) {
            for (const auto& pv : snap->profiles)
                if (pv.sketchId == sk)
                    if (SelItem s = m_ws.findProfile(sk, pv.regionKey); s.valid())
                        sel.push_back(s);
        }
    }
    for (const auto& e : m_path.edges)
        if (SelItem s = m_ws.findEdge(e); s.valid())
            sel.push_back(s);
    m_ws.setSelection(std::move(sel));
}

void SweepLoftEditor::onSnapshot()
{
    syncSelection();
    updatePanel();
}

std::vector<MenuAction> SweepLoftEditor::menuActions()
{
    return {
        {QObject::tr("OK"), icon(IconId::Finish), [ws = &m_ws] { ws->finishSession(); }},
        {QObject::tr("Cancel"), icon(IconId::Cancel), [ws = &m_ws] { ws->cancelSession(); }},
    };
}

QString SweepLoftEditor::sketchName(const Uuid& id) const
{
    if (FeaturePtr f = m_ws.model()->doc().find(id))
        return QString::fromStdString(f->name);
    return "?";
}

void SweepLoftEditor::paintOverlay(QPainter& p)
{
    QString hint;
    if (m_pickPathEdges)
        hint = QObject::tr("Click the model edges of the path (in order), then untick 'Pick edges'");
    else if (isSweep())
        hint = m_profiles.empty() ? QObject::tr("Click the closed profile to sweep")
               : !ready()         ? QObject::tr("Choose the path sketch in the inspector")
                                  : QObject::tr("Preview in transparency — Enter to validate");
    else
        hint = m_profiles.size() < 2 ? QObject::tr("Click the profiles in order (%1 selected)").arg(m_profiles.size())
                                     : QObject::tr("%1 sections — Enter to validate").arg(m_profiles.size());
    QFont f = p.font();
    f.setPixelSize(12);
    f.setBold(true);
    p.setFont(f);
    const QRectF box(12, 12, 420, 52);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(20, 24, 30, 200));
    p.drawRoundedRect(box, 6, 6);
    p.setPen(QColor(230, 233, 237));
    p.drawText(box.adjusted(10, 6, -10, -26), Qt::AlignLeft | Qt::AlignVCenter, QString::fromStdString(m_name));
    f.setBold(false);
    f.setPixelSize(11);
    p.setFont(f);
    p.setPen(m_pickPathEdges ? QColor(0xff, 0xd0, 0x40) : QColor(170, 178, 188));
    p.drawText(box.adjusted(10, 28, -10, -4), Qt::AlignLeft | Qt::AlignVCenter, hint);
    p.setBrush(Qt::NoBrush);

    // loft: section numbers at the profile centroids
    if (!isSweep())
        if (auto snap = m_ws.model()->snapshot())
            for (size_t i = 0; i < m_profiles.size(); ++i)
                for (const auto& pv : snap->profiles)
                    if (pv.sketchId == m_profiles[i].first && (m_profiles[i].second.empty() || pv.regionKey == m_profiles[i].second)) {
                        QPointF at;
                        if (!m_ws.viewport()->toScreen(pv.centroid, &at))
                            break;
                        p.setPen(Qt::NoPen);
                        p.setBrush(Colors::Accent);
                        p.drawEllipse(at, 10, 10);
                        p.setPen(Qt::white);
                        p.drawText(QRectF(at - QPointF(10, 10), QSizeF(20, 20)), Qt::AlignCenter, QString::number(i + 1));
                        p.setBrush(Qt::NoBrush);
                        break;
                    }
}

void SweepLoftEditor::fillPathCombo()
{
    if (!m_pathBox)
        return;
    m_updating = true;
    m_pathBox->clear();
    m_pathBox->addItem(QObject::tr("(none)"), "");
    const DocState& d = m_ws.model()->doc();
    const int limit = m_existing ? d.indexOf(m_id) : d.marker;
    const Uuid profileSketch = m_profiles.empty() ? Uuid{} : m_profiles.front().first;
    for (int i = 0; i < limit && i < int(d.features.size()); ++i) {
        const auto& f = d.features[size_t(i)];
        if (f->type == FeatureType::Sketch && f->id != profileSketch)
            m_pathBox->addItem(QString::fromStdString(f->name), QString::fromStdString(f->id));
    }
    if (!m_path.edges.empty())
        m_pathBox->addItem(QObject::tr("%n model edge(s)", nullptr, int(m_path.edges.size())), "edges");
    const int idx = !m_path.edges.empty() ? m_pathBox->findData("edges")
                                          : m_pathBox->findData(QString::fromStdString(m_path.sketch));
    m_pathBox->setCurrentIndex(std::max(0, idx));
    m_updating = false;
}

void SweepLoftEditor::fillSectionList()
{
    if (!m_sectionList)
        return;
    m_updating = true;
    const int row = m_sectionList->currentRow();
    m_sectionList->clear();
    for (size_t i = 0; i < m_profiles.size(); ++i)
        m_sectionList->addItem(QString("%1. %2").arg(i + 1).arg(sketchName(m_profiles[i].first)));
    m_sectionList->setCurrentRow(std::min(row, m_sectionList->count() - 1));
    m_updating = false;
}

QWidget* SweepLoftEditor::createPanel(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* lay = new QVBoxLayout(w);
    auto* title = new QLabel(QString("<b>%1</b>").arg(QString::fromStdString(m_name)), w);
    title->setStyleSheet("font-size: 15px;");
    lay->addWidget(title);
    auto* form = new QFormLayout();

    if (isSweep()) {
        m_profileLabel = new QLabel(w);
        form->addRow(QObject::tr("Profile"), m_profileLabel);
        auto* row = new QHBoxLayout();
        m_pathBox = new QComboBox(w);
        m_pathPick = new QPushButton(QObject::tr("Pick edges"), w);
        m_pathPick->setCheckable(true);
        m_pathPick->setToolTip(QObject::tr("Use a chain of model edges as path (3D trajectory)"));
        row->addWidget(m_pathBox, 1);
        row->addWidget(m_pathPick);
        form->addRow(QObject::tr("Path"), row);
        QObject::connect(m_pathBox, &QComboBox::currentIndexChanged, m_pathBox, [this](int) {
            if (m_updating)
                return;
            const QString v = m_pathBox->currentData().toString();
            if (v == "edges")
                return;
            m_path = PathRef{};
            m_path.sketch = v.toStdString();
            syncSelection();
            pushPreview();
            updatePanel();
        });
        QObject::connect(m_pathPick, &QPushButton::toggled, m_pathPick, [this](bool on) {
            m_pickPathEdges = on;
            m_ws.viewport()->refreshOverlay();
            m_ws.touchHighlight();
        });
        auto* orient = new QComboBox(w);
        orient->addItem(QObject::tr("Corrected Frenet (no twist)"), "CorrectedFrenet");
        orient->addItem(QObject::tr("Frenet"), "Frenet");
        orient->addItem(QObject::tr("Fixed"), "Fixed");
        orient->setCurrentIndex(std::max(0, orient->findData(QString::fromStdString(m_orientation))));
        form->addRow(QObject::tr("Orientation"), orient);
        QObject::connect(orient, &QComboBox::currentIndexChanged, orient, [this, orient](int) {
            m_orientation = orient->currentData().toString().toStdString();
            pushPreview();
        });
    }
    else {
        m_sectionList = new QListWidget(w);
        m_sectionList->setMaximumHeight(150);
        form->addRow(QObject::tr("Profiles (in order)"), m_sectionList);
        auto* row = new QHBoxLayout();
        auto* up = new QPushButton(QObject::tr("Up"), w);
        auto* down = new QPushButton(QObject::tr("Down"), w);
        auto* rem = new QPushButton(QObject::tr("Remove"), w);
        row->addWidget(up);
        row->addWidget(down);
        row->addWidget(rem);
        form->addRow("", row);
        auto move = [this](int delta) {
            const int r = m_sectionList->currentRow();
            const int t = r + delta;
            if (r < 0 || t < 0 || t >= int(m_profiles.size()))
                return;
            std::swap(m_profiles[size_t(r)], m_profiles[size_t(t)]);
            fillSectionList();
            m_sectionList->setCurrentRow(t);
            pushPreview();
            m_ws.viewport()->refreshOverlay();
        };
        QObject::connect(up, &QPushButton::clicked, up, [move] { move(-1); });
        QObject::connect(down, &QPushButton::clicked, down, [move] { move(+1); });
        QObject::connect(rem, &QPushButton::clicked, rem, [this] {
            const int r = m_sectionList->currentRow();
            if (r < 0 || r >= int(m_profiles.size()))
                return;
            m_profiles.erase(m_profiles.begin() + r);
            fillSectionList();
            syncSelection();
            pushPreview();
            updatePanel();
        });
        auto* solid = new QCheckBox(QObject::tr("Solid"), w);
        solid->setChecked(m_solid);
        auto* ruled = new QCheckBox(QObject::tr("Ruled (straight surfaces)"), w);
        ruled->setChecked(m_ruled);
        form->addRow("", solid);
        form->addRow("", ruled);
        QObject::connect(solid, &QCheckBox::toggled, solid, [this](bool on) {
            m_solid = on;
            pushPreview();
        });
        QObject::connect(ruled, &QCheckBox::toggled, ruled, [this](bool on) {
            m_ruled = on;
            pushPreview();
        });
    }

    m_opBox = new QComboBox(w);
    m_opBox->addItems({QObject::tr("New body"), QObject::tr("Join"), QObject::tr("Cut"), QObject::tr("Intersect")});
    m_opBox->setCurrentIndex(opIndex(m_op));
    form->addRow(QObject::tr("Operation"), m_opBox);
    QObject::connect(m_opBox, &QComboBox::currentIndexChanged, m_opBox, [this](int i) {
        if (m_updating)
            return;
        m_op = opFromIndex(i);
        pushPreview();
    });
    lay->addLayout(form);

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

    fillPathCombo();
    fillSectionList();
    updatePanel();
    return w;
}

void SweepLoftEditor::updatePanel()
{
    m_updating = true;
    if (m_profileLabel)
        m_profileLabel->setText(m_profiles.empty() ? QObject::tr("— click a profile —")
                                                   : sketchName(m_profiles.front().first));
    if (m_opBox)
        m_opBox->setCurrentIndex(opIndex(m_op));
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
