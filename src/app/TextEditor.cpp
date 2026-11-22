#include "TextEditor.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

#include "ExprSpinBox.h"
#include "Icons.h"
#include "ModelController.h"
#include "Theme.h"
#include "Viewport.h"
#include "kernel/KernelEngine.h"
#include "model/Bindings.h"

namespace silex::ui {

namespace {
const char* const kFields[] = {"/height", "/depth", "/angle", "/x", "/y"};
}

TextEditor::TextEditor(Workspace& ws, FeaturePtr existing) : EditSession(ws), m_existing(std::move(existing))
{
    if (m_existing) {
        m_id = m_existing->id;
        m_name = m_existing->name;
        m_tp = m_existing->as<EmbossTextParams>();
        for (const char* ptr : kFields)
            if (auto e = fieldExpression(m_existing->params, ptr); !e.empty())
                m_exprs[ptr] = e;
        m_placed = true;
    }
    else {
        m_id = newUuid();
        m_name = ws.model()->doc().nextName("Text");
        m_tp.plane.kind = "Face";
        for (const SelItem& s : ws.selection())
            if (s.kind == SelKind::Face && s.planar) {
                m_tp.plane.face = s.ref();
                m_placed = true;
                break;
            }
    }
    syncSelection();
    pushPreview();
}

TextEditor::~TextEditor() = default;

uint32_t TextEditor::pickFilter() const { return SelFilter::Faces | SelFilter::PlanarOnly; }

std::optional<Placement> TextEditor::plane() const
{
    const SketchPlaneRef& r = m_tp.plane;
    if (r.kind == "XY" || r.kind == "XZ" || r.kind == "YZ")
        return offsetPlacement(originPlacement(r.kind), r.offset);
    auto snap = m_ws.model()->snapshot();
    if (!snap)
        return std::nullopt;
    if (r.kind == "Plane") {
        for (const auto& pv : snap->planes)
            if (pv.id == r.planeId)
                return offsetPlacement(pv.plane, r.offset);
        return std::nullopt;
    }
    for (const auto& b : snap->bodies)
        for (const auto& f : b.mesh.faces)
            if (f.name == r.face.name && f.planar) {
                // same frame as the kernel: face centroid, X = world X projected on the face
                Placement pl = f.plane;
                pl.origin = f.sig.center;
                const Vec3 n = pl.normal.normalized();
                Vec3 x = Vec3{1, 0, 0} - n * n.x;
                if (x.length() < 1e-6)
                    x = Vec3{0, 1, 0} - n * n.y;
                pl.xDir = x.normalized();
                return offsetPlacement(pl, r.offset);
            }
    return std::nullopt;
}

bool TextEditor::mousePress(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return false;
    const SelItem hit = m_ws.viewport()->pickAt(e->position(), SelFilter::Faces | SelFilter::PlanarOnly);
    if (hit.kind != SelKind::Face)
        return false;  // empty space: orbit / pan as usual
    if (!hit.planar) {
        m_ws.status(QObject::tr("The text needs a planar face"));
        return true;
    }
    if (m_tp.plane.kind != "Face" || m_tp.plane.face.name != hit.name) {
        m_tp.plane = SketchPlaneRef{};
        m_tp.plane.kind = "Face";
        m_tp.plane.face = hit.ref();
        m_tp.x = m_tp.y = 0;
        m_exprs.erase("/x");
        m_exprs.erase("/y");
        syncSelection();
    }
    else if (const auto pl = plane()) {
        // same face: the click moves the text (snapped to the face center within 12 px)
        if (const auto w = m_ws.viewport()->rayPlane(e->position(), *pl)) {
            Vec2 local = pl->toLocal(*w);
            QPointF c;
            if (m_ws.viewport()->toScreen(pl->origin, &c) &&
                std::hypot(c.x() - e->position().x(), c.y() - e->position().y()) < 12)
                local = {0, 0};
            else if (m_ws.gridSnap()) {
                const double s = m_ws.gridStep();
                local = {std::round(local.x / s) * s, std::round(local.y / s) * s};
            }
            m_tp.x = local.x;
            m_tp.y = local.y;
            m_exprs.erase("/x");
            m_exprs.erase("/y");
        }
    }
    m_placed = true;
    updatePanel();
    pushPreview();
    return true;
}

void TextEditor::paintOverlay(QPainter& p)
{
    const auto pl = plane();
    if (!pl || !m_placed)
        return;
    QPointF s;
    if (!m_ws.viewport()->toScreen(pl->toWorld({m_tp.x, m_tp.y}), &s))
        return;
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Colors::SketchSelected, 2));
    p.drawLine(s + QPointF(-8, 0), s + QPointF(8, 0));
    p.drawLine(s + QPointF(0, -8), s + QPointF(0, 8));
}

FeaturePtr TextEditor::buildNode() const
{
    auto n = m_existing ? std::make_shared<FeatureNode>(*m_existing) : std::make_shared<FeatureNode>();
    n->id = m_id;
    n->type = FeatureType::EmbossText;
    n->name = m_name;
    EmbossTextParams tp = m_tp;
    try {
        tp.resolved = interpolateText(tp.text, ExprSpinBox::parameterLookup());
    }
    catch (const std::exception&) {
        tp.resolved = tp.text;
    }
    n->params = Json(tp);
    for (const auto& [ptr, e] : m_exprs)
        setFieldExpression(n->params, ptr, e);
    return n;
}

void TextEditor::pushPreview()
{
    DocState d = m_ws.model()->doc();
    if (!ready() || !m_placed) {
        if (m_existing)
            d.marker = d.indexOf(m_id);
        m_ws.model()->preview(d, {});
        return;
    }
    FeaturePtr node = buildNode();
    if (m_existing) {
        d = UpdateFeatureCommand(node).apply(d);
        d.marker = d.indexOf(m_id) + 1;
    }
    else
        d = AddFeatureCommand(node).apply(d);
    m_ws.model()->preview(d, m_id);
}

void TextEditor::syncSelection()
{
    std::vector<SelItem> sel;
    if (m_tp.plane.kind == "Face")
        if (auto snap = m_ws.model()->snapshot())
            for (size_t b = 0; b < snap->bodies.size(); ++b) {
                const auto& faces = snap->bodies[b].mesh.faces;
                for (size_t f = 0; f < faces.size(); ++f)
                    if (faces[f].name == m_tp.plane.face.name) {
                        SelItem s;
                        s.kind = SelKind::Face;
                        s.owner = snap->bodies[b].id;
                        s.object = int(b);
                        s.index = int(f);
                        s.name = faces[f].name;
                        s.sig = faces[f].sig;
                        s.planar = true;
                        s.plane = faces[f].plane;
                        sel.push_back(s);
                    }
            }
    m_ws.setSelection(std::move(sel));
}

void TextEditor::onSnapshot()
{
    updateHints();
}

bool TextEditor::commit()
{
    if (!ready() || !m_placed) {
        m_ws.status(QObject::tr("Click a planar face to place the text"));
        return false;
    }
    if (QString::fromStdString(m_tp.text).trimmed().isEmpty()) {
        m_ws.status(QObject::tr("Type a text first"));
        return false;
    }
    FeaturePtr node = buildNode();
    if (m_existing)
        m_ws.model()->execute(UpdateFeatureCommand(node));
    else
        m_ws.model()->execute(AddFeatureCommand(node));
    return true;
}

std::vector<MenuAction> TextEditor::menuActions()
{
    return {
        {QObject::tr("OK"), icon(IconId::Finish), [ws = &m_ws] { ws->finishSession(); }},
        {QObject::tr("Cancel"), icon(IconId::Cancel), [ws = &m_ws] { ws->cancelSession(); }},
        {QObject::tr("Center on the face"), icon(IconId::Fit), [this] {
             m_tp.x = m_tp.y = 0;
             m_exprs.erase("/x");
             m_exprs.erase("/y");
             updatePanel();
             pushPreview();
         }},
    };
}

QWidget* TextEditor::createPanel(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* lay = new QVBoxLayout(w);
    auto* title = new QLabel(QString("<b>%1</b>").arg(QString::fromStdString(m_name)), w);
    title->setStyleSheet("font-size: 15px;");
    lay->addWidget(title);

    // text + quick parameter insertion
    auto* textRow = new QHBoxLayout();
    textRow->addWidget(new QLabel(QObject::tr("Text"), w));
    textRow->addStretch(1);
    m_paramButton = new QToolButton(w);
    m_paramButton->setText(QObject::tr("{x} Parameter"));
    m_paramButton->setToolTip(QObject::tr("Insert a global parameter (Ctrl+P): its value is written in the text "
                                          "and follows every change"));
    m_paramButton->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(m_paramButton);
    QObject::connect(menu, &QMenu::aboutToShow, menu, [this, menu] {
        menu->clear();
        const auto& params = m_ws.model()->doc().parameters;
        if (params.empty())
            menu->addAction(QObject::tr("(no parameter: Ctrl+P to create one)"))->setEnabled(false);
        for (const Parameter& prm : params) {
            const QString name = QString::fromStdString(prm.name);
            menu->addAction(QString("%1 = %2").arg(name, QString::fromStdString(formatNumber(prm.value))), m_text,
                            [this, name] { m_text->insertPlainText("{" + name + "}"); });
        }
    });
    m_paramButton->setMenu(menu);
    textRow->addWidget(m_paramButton);
    lay->addLayout(textRow);
    m_text = new QPlainTextEdit(w);
    m_text->setPlaceholderText(QObject::tr("e.g. Box v{version} - {largeur}x{hauteur} mm"));
    m_text->setMaximumHeight(70);
    m_text->setTabChangesFocus(true);
    lay->addWidget(m_text);
    m_resolved = new QLabel(w);
    m_resolved->setWordWrap(true);
    m_resolved->setStyleSheet("color: #9fd3ff;");
    lay->addWidget(m_resolved);

    auto* form = new QFormLayout();
    m_fontBox = new QComboBox(w);
    m_fontBox->setEditable(true);  // a .ttf / .otf file name or path is accepted too
    m_fontBox->setToolTip(QObject::tr("Bundled: Liberation Sans. System fonts and .ttf / .otf files work too, but a "
                                      "design using them only rebuilds identically where the font is installed."));
    QStringList fams;
    for (const auto& f : KernelEngine::fontFamilies())
        fams << QString::fromStdString(f);
    if (!fams.contains("Liberation Sans"))
        fams.prepend("Liberation Sans");
    m_fontBox->addItems(fams);
    form->addRow(QObject::tr("Font"), m_fontBox);
    auto spin = [&](double lo, double hi, const QString& suffix) {
        auto* b = new ExprSpinBox(w);
        b->setRange(lo, hi);
        b->setDecimals(2);
        b->setSuffix(suffix);
        b->setKeyboardTracking(true);
        return b;
    };
    m_heightBox = spin(0.1, 1000, " mm");
    m_heightBox->setToolTip(QObject::tr("Height of the capital letters"));
    form->addRow(QObject::tr("Height"), m_heightBox);
    m_depthBox = spin(-100, 100, " mm");
    m_depthBox->setToolTip(QObject::tr("Positive: relief (adds material). Negative: engraving (removes material)."));
    form->addRow(QObject::tr("Depth"), m_depthBox);
    m_angleBox = spin(-360, 360, QString::fromUtf8("°"));
    form->addRow(QObject::tr("Rotation"), m_angleBox);
    m_alignBox = new QComboBox(w);
    m_alignBox->addItem(QObject::tr("Left"), "left");
    m_alignBox->addItem(QObject::tr("Center"), "center");
    m_alignBox->addItem(QObject::tr("Right"), "right");
    form->addRow(QObject::tr("Alignment"), m_alignBox);
    m_xBox = spin(-1e5, 1e5, " mm");
    m_yBox = spin(-1e5, 1e5, " mm");
    form->addRow(QObject::tr("Position X"), m_xBox);
    form->addRow(QObject::tr("Position Y"), m_yBox);
    lay->addLayout(form);
    auto* snapRow = new QHBoxLayout();
    auto* center = new QPushButton(QObject::tr("Face center"), w);
    center->setToolTip(QObject::tr("Position 0, 0 = center of the face (or origin of the plane)"));
    snapRow->addWidget(center);
    snapRow->addStretch(1);
    lay->addLayout(snapRow);

    m_fdm = new QLabel(w);
    m_fdm->setWordWrap(true);
    lay->addWidget(m_fdm);
    m_where = new QLabel(w);
    m_where->setWordWrap(true);
    m_where->setStyleSheet("color: #a8b0ba;");
    lay->addWidget(m_where);
    m_message = new QLabel(w);
    m_message->setWordWrap(true);
    lay->addWidget(m_message);
    lay->addStretch(1);

    auto changed = [this] {
        if (m_updating)
            return;
        m_tp.text = m_text->toPlainText().toStdString();
        m_tp.font = m_fontBox->currentText().trimmed().toStdString();
        m_tp.align = m_alignBox->currentData().toString().toStdString();
        const std::pair<ExprSpinBox*, std::pair<const char*, double*>> fields[] = {
            {m_heightBox, {"/height", &m_tp.height}}, {m_depthBox, {"/depth", &m_tp.depth}},
            {m_angleBox, {"/angle", &m_tp.angle}},    {m_xBox, {"/x", &m_tp.x}},
            {m_yBox, {"/y", &m_tp.y}}};
        for (const auto& [box, f] : fields) {
            *f.second = box->value();
            if (box->expression().empty())
                m_exprs.erase(f.first);
            else
                m_exprs[f.first] = box->expression();
        }
        updateHints();
        pushPreview();
    };
    QObject::connect(m_text, &QPlainTextEdit::textChanged, w, changed);
    QObject::connect(m_fontBox, &QComboBox::currentTextChanged, w, changed);
    QObject::connect(m_alignBox, &QComboBox::currentIndexChanged, w, changed);
    for (ExprSpinBox* b : {m_heightBox.data(), m_depthBox.data(), m_angleBox.data(), m_xBox.data(), m_yBox.data()}) {
        QObject::connect(b, &QDoubleSpinBox::valueChanged, w, changed);
        QObject::connect(b, &ExprSpinBox::exprChanged, w, changed);
    }
    QObject::connect(center, &QPushButton::clicked, w, [this] {
        m_tp.x = m_tp.y = 0;
        m_exprs.erase("/x");
        m_exprs.erase("/y");
        updatePanel();
        pushPreview();
    });
    updatePanel();
    return w;
}

void TextEditor::updatePanel()
{
    if (!m_text)
        return;
    m_updating = true;
    if (m_text->toPlainText().toStdString() != m_tp.text)
        m_text->setPlainText(QString::fromStdString(m_tp.text));
    m_fontBox->setCurrentText(QString::fromStdString(m_tp.font));
    m_alignBox->setCurrentIndex(std::max(0, m_alignBox->findData(QString::fromStdString(m_tp.align))));
    auto expr = [&](const char* ptr) {
        auto it = m_exprs.find(ptr);
        return it == m_exprs.end() ? std::string() : it->second;
    };
    m_heightBox->setValueAndExpression(m_tp.height, expr("/height"));
    m_depthBox->setValueAndExpression(m_tp.depth, expr("/depth"));
    m_angleBox->setValueAndExpression(m_tp.angle, expr("/angle"));
    m_xBox->setValueAndExpression(m_tp.x, expr("/x"));
    m_yBox->setValueAndExpression(m_tp.y, expr("/y"));
    m_updating = false;
    updateHints();
}

void TextEditor::updateHints()
{
    if (!m_resolved)
        return;
    std::string resolved = m_tp.text;
    QString error;
    try {
        resolved = interpolateText(m_tp.text, ExprSpinBox::parameterLookup());
    }
    catch (const std::exception& e) {
        error = QString::fromUtf8(e.what());
    }
    if (!error.isEmpty())
        m_resolved->setText(QString("<span style='color:#ff5252'>%1</span>").arg(error.toHtmlEscaped()));
    else if (m_tp.text.find('{') != std::string::npos)
        m_resolved->setText(QObject::tr("Shown as: <b>%1</b>").arg(QString::fromStdString(resolved).toHtmlEscaped()));
    else
        m_resolved->clear();

    // FDM guard: letter strokes thinner than the nozzle cannot be printed
    const auto m = KernelEngine::measureText(resolved.empty() ? std::string("l") : resolved, m_tp.font, m_tp.height);
    if (!m.ok)
        m_fdm->setText(QString("<span style='color:#ff5252'>%1</span>").arg(QString::fromStdString(m.error).toHtmlEscaped()));
    else if (m.strokeWidth > 0 && m.strokeWidth < m_tp.nozzle - 1e-9)
        m_fdm->setText(QObject::tr("<span style='color:#ff9800'>&#9888; Strokes about %1 mm wide: thinner than the "
                                   "%2 mm nozzle. Use at least %3 mm of height, or a bolder font.</span>")
                           .arg(m.strokeWidth, 0, 'f', 2)
                           .arg(m_tp.nozzle, 0, 'f', 1)
                           .arg(m_tp.height * m_tp.nozzle / m.strokeWidth, 0, 'f', 1));
    else
        m_fdm->setText(QObject::tr("<span style='color:#7bd88f'>Strokes about %1 mm wide: printable with a %2 mm nozzle."
                                   "</span>")
                           .arg(m.strokeWidth, 0, 'f', 2)
                           .arg(m_tp.nozzle, 0, 'f', 1));

    m_where->setText(!m_placed ? QObject::tr("Click a planar face to place the text.")
                               : QObject::tr("Click the face again to move the text (snaps to its center)."));
    QString msg;
    if (auto snap = m_ws.model()->snapshot())
        for (const auto& r : snap->results)
            if (r.id == m_id && !r.message.empty())
                msg = QString::fromStdString(r.message);
    m_message->setText(msg.isEmpty() ? QString() : QString("<span style='color:#ffb454'>%1</span>").arg(msg.toHtmlEscaped()));
}

}  // namespace silex::ui
