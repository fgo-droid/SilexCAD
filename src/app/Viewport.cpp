#include "Viewport.h"

#include <QFile>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <QtMath>
#include <algorithm>
#include <cstring>
#include <functional>
#include <rhi/qshader.h>
#include <set>

#include "Theme.h"
#include "model/Features.h"

namespace silex::ui {

namespace {

constexpr quint32 kUboSize = 112;  // mat4 + 3 x vec4 (std140)

class OverlayWidget final : public QWidget
{
public:
    OverlayWidget(QWidget* parent, std::function<void(QPainter&)> paint) : QWidget(parent), m_paint(std::move(paint))
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        setAutoFillBackground(false);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        m_paint(p);
    }

private:
    std::function<void(QPainter&)> m_paint;
};

void pushColor(std::vector<float>& v, const QColor& c, float alpha = 1.f)
{
    v.push_back(float(c.redF()));
    v.push_back(float(c.greenF()));
    v.push_back(float(c.blueF()));
    v.push_back(alpha);
}

QColor previewColor(BooleanOp op)
{
    switch (op) {
        case BooleanOp::Cut: return Colors::PreviewCut;
        case BooleanOp::NewBody: return Colors::PreviewNew;
        default: return Colors::PreviewJoin;
    }
}

}  // namespace

// ------------------------------------------------------------------ GpuMesh

void Viewport::GpuMesh::release()
{
    for (QRhiBuffer** b : {&pos, &nrm, &col, &idx, &epos, &ecol})
        if (*b) {
            (*b)->deleteLater();
            *b = nullptr;
        }
    indexCount = 0;
    edgeVertexCount = 0;
    vertexFace.clear();
}

// ------------------------------------------------------------------ construction

Viewport::Viewport(QWidget* parent) : QRhiWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(320, 240);
    m_overlay = new OverlayWidget(this, [this](QPainter& p) { paintOverlayContent(p); });
    m_overlay->setGeometry(rect());
    m_overlay->show();
    m_anim = new QVariantAnimation(this);
    m_anim->setDuration(320);
    m_anim->setEasingCurve(QEasingCurve::OutCubic);
}

Viewport::~Viewport() = default;

void Viewport::resizeEvent(QResizeEvent* e)
{
    QRhiWidget::resizeEvent(e);
    m_overlay->setGeometry(rect());
}

void Viewport::setSnapshot(std::shared_ptr<const ModelSnapshot> snap)
{
    m_snap = std::move(snap);
    m_geometryDirty = true;
    if (m_snap && m_snap->bbox.valid())
        m_camera.sceneRadius = std::max(float(m_snap->bbox.diagonal()) * 0.5f, 10.f);
    rebuildPicker();
    update();
    refreshOverlay();
}

void Viewport::highlightChanged()
{
    if (m_client && m_client->highlight().profileVisible != m_pickerProfiles)
        rebuildPicker();
    update();
    refreshOverlay();
}

void Viewport::refreshOverlay() { m_overlay->update(); }

void Viewport::rebuildPicker()
{
    m_pickerProfiles.clear();
    if (m_client)
        m_pickerProfiles = m_client->highlight().profileVisible;
    m_picker.build(m_snap, m_pickerProfiles);
}

// ------------------------------------------------------------------ camera helpers

void Viewport::animateTo(const QQuaternion& rot, std::optional<QVector3D> target, std::optional<float> distance)
{
    m_anim->stop();
    m_anim->disconnect(this);
    const QQuaternion r0 = m_camera.rotation;
    const QVector3D t0 = m_camera.target, t1 = target.value_or(t0);
    const float d0 = m_camera.distance, d1 = distance.value_or(d0);
    connect(m_anim, &QVariantAnimation::valueChanged, this, [=, this](const QVariant& v) {
        const float k = v.toFloat();
        m_camera.rotation = QQuaternion::slerp(r0, rot, k).normalized();
        m_camera.target = t0 + (t1 - t0) * k;
        m_camera.distance = d0 + (d1 - d0) * k;
        update();
        refreshOverlay();
        emit cameraChanged();
    });
    m_anim->setStartValue(0.f);
    m_anim->setEndValue(1.f);
    m_anim->start();
}

void Viewport::fitAll(bool animated)
{
    BBox box;
    if (m_snap)
        box = m_snap->bbox;
    Camera c = m_camera;
    c.fit(box);
    m_camera.sceneRadius = c.sceneRadius;
    if (animated)
        animateTo(m_camera.rotation, c.target, c.distance);
    else {
        m_camera.target = c.target;
        m_camera.distance = c.distance;
        update();
    }
}

void Viewport::setStandardView(int which) { animateTo(Camera::standardView(which)); }

SelItem Viewport::pickAt(QPointF px, uint32_t filter) const { return m_picker.pick(m_camera, size(), px, filter); }

std::optional<Vec3> Viewport::rayPlane(QPointF px, const Placement& pl) const
{
    const Ray r = m_camera.ray(px, size());
    const QVector3D n(float(pl.normal.x), float(pl.normal.y), float(pl.normal.z));
    const QVector3D o(float(pl.origin.x), float(pl.origin.y), float(pl.origin.z));
    const float denom = QVector3D::dotProduct(r.dir, n);
    if (std::abs(denom) < 1e-7f)
        return std::nullopt;
    const float t = QVector3D::dotProduct(o - r.origin, n) / denom;
    const QVector3D h = r.origin + r.dir * t;
    return Vec3{h.x(), h.y(), h.z()};
}

bool Viewport::toScreen(const Vec3& p, QPointF* out) const
{
    return m_camera.project(QVector3D(float(p.x), float(p.y), float(p.z)), size(), out);
}

bool Viewport::saveScreenshot(const QString& path) { return grab().save(path); }

// ------------------------------------------------------------------ RHI resources

QShader Viewport::loadShader(const QString& name) const
{
    QFile f(QStringLiteral(":/shaders/") + name + QStringLiteral(".qsb"));
    if (!f.open(QIODevice::ReadOnly))
        qFatal("Missing shader %s", qPrintable(name));
    return QShader::fromSerialized(f.readAll());
}

QRhiBuffer* Viewport::makeBuffer(QRhiBuffer::UsageFlags usage, const void* data, quint32 bytes,
                                 QRhiResourceUpdateBatch* u)
{
    QRhiBuffer* b = m_rhi->newBuffer(QRhiBuffer::Static, usage, std::max<quint32>(bytes, 16));
    b->create();
    if (data && bytes)
        u->uploadStaticBuffer(b, 0, bytes, data);
    return b;
}

QRhiGraphicsPipeline* Viewport::makePipeline(const QString& vs, const QString& fs, QRhiRenderPassDescriptor* rpd,
                                             QRhiShaderResourceBindings* layout, bool lines, bool depthWrite,
                                             int colorTargets, int blendMode, bool lineLayout)
{
    QRhiGraphicsPipeline* ps = m_rhi->newGraphicsPipeline();
    ps->setShaderStages({{QRhiShaderStage::Vertex, loadShader(vs)}, {QRhiShaderStage::Fragment, loadShader(fs)}});
    QRhiVertexInputLayout il;
    if (lineLayout) {
        il.setBindings({{3 * sizeof(float)}, {4 * sizeof(float)}});
        il.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0}, {1, 1, QRhiVertexInputAttribute::Float4, 0}});
    }
    else {
        il.setBindings({{3 * sizeof(float)}, {3 * sizeof(float)}, {4 * sizeof(float)}});
        il.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0},
                          {1, 1, QRhiVertexInputAttribute::Float3, 0},
                          {2, 2, QRhiVertexInputAttribute::Float4, 0}});
    }
    ps->setVertexInputLayout(il);
    ps->setShaderResourceBindings(layout);
    ps->setRenderPassDescriptor(rpd);
    ps->setTopology(lines ? QRhiGraphicsPipeline::Lines : QRhiGraphicsPipeline::Triangles);
    ps->setCullMode(QRhiGraphicsPipeline::None);
    ps->setDepthTest(true);
    ps->setDepthWrite(depthWrite);
    ps->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);

    QList<QRhiGraphicsPipeline::TargetBlend> blends;
    if (blendMode == 2) {  // OIT: accumulation + coverage
        QRhiGraphicsPipeline::TargetBlend acc;
        acc.enable = true;
        acc.srcColor = acc.dstColor = acc.srcAlpha = acc.dstAlpha = QRhiGraphicsPipeline::One;
        QRhiGraphicsPipeline::TargetBlend cov;
        cov.enable = true;
        cov.srcColor = cov.srcAlpha = QRhiGraphicsPipeline::One;
        cov.dstColor = cov.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        blends = {acc, cov};
    }
    else {
        QRhiGraphicsPipeline::TargetBlend b;
        if (blendMode == 1) {  // premultiplied alpha
            b.enable = true;
            b.srcColor = b.srcAlpha = QRhiGraphicsPipeline::One;
            b.dstColor = b.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        }
        for (int i = 0; i < colorTargets; ++i)
            blends.append(b);
    }
    ps->setTargetBlends(blends.cbegin(), blends.cend());
    ps->create();
    return ps;
}

void Viewport::createStaticResources()
{
    m_linear = m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                 QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    m_linear->create();
    m_nearest = m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                  QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    m_nearest->create();

    auto ubo = [&](quint32 size) {
        QRhiBuffer* b = m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, size);
        b->create();
        return b;
    };
    m_uboBody = ubo(kUboSize);
    m_uboEdge = ubo(kUboSize);
    m_uboGrid = ubo(kUboSize);
    m_uboOit = ubo(kUboSize);
    m_uboComposite = ubo(48);

    const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
    auto srb = [&](QRhiBuffer* b) {
        QRhiShaderResourceBindings* s = m_rhi->newShaderResourceBindings();
        s->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, stages, b)});
        s->create();
        return s;
    };
    m_srbBody = srb(m_uboBody);
    m_srbEdge = srb(m_uboEdge);
    m_srbGrid = srb(m_uboGrid);
    m_srbOit = srb(m_uboOit);
}

void Viewport::createSizedResources(QSize sz)
{
    m_rtSize = sz;
    for (QRhiResource* r : std::initializer_list<QRhiResource*>{m_opaqueRT, m_oitRT, m_colorTex, m_depthTex,
                                                                 m_accumTex, m_revealTex, m_srbComposite})
        if (r)
            r->deleteLater();

    const QRhiTexture::Format depthFmt =
        m_rhi->isTextureFormatSupported(QRhiTexture::D32F) ? QRhiTexture::D32F : QRhiTexture::D24;
    const QRhiTexture::Format revealFmt =
        m_rhi->isTextureFormatSupported(QRhiTexture::R16F) ? QRhiTexture::R16F : QRhiTexture::RGBA16F;

    m_colorTex = m_rhi->newTexture(QRhiTexture::RGBA8, sz, 1, QRhiTexture::RenderTarget);
    m_colorTex->create();
    m_depthTex = m_rhi->newTexture(depthFmt, sz, 1, QRhiTexture::RenderTarget);
    m_depthTex->create();
    m_accumTex = m_rhi->newTexture(QRhiTexture::RGBA16F, sz, 1, QRhiTexture::RenderTarget);
    m_accumTex->create();
    m_revealTex = m_rhi->newTexture(revealFmt, sz, 1, QRhiTexture::RenderTarget);
    m_revealTex->create();

    {
        QRhiTextureRenderTargetDescription d{QRhiColorAttachment(m_colorTex)};
        d.setDepthTexture(m_depthTex);
        m_opaqueRT = m_rhi->newTextureRenderTarget(d);
        if (!m_opaqueRpd)
            m_opaqueRpd = m_opaqueRT->newCompatibleRenderPassDescriptor();
        m_opaqueRT->setRenderPassDescriptor(m_opaqueRpd);
        m_opaqueRT->create();
    }
    {
        QRhiTextureRenderTargetDescription d;
        d.setColorAttachments({QRhiColorAttachment(m_accumTex), QRhiColorAttachment(m_revealTex)});
        d.setDepthTexture(m_depthTex);
        m_oitRT = m_rhi->newTextureRenderTarget(d, QRhiTextureRenderTarget::PreserveDepthStencilContents);
        if (!m_oitRpd)
            m_oitRpd = m_oitRT->newCompatibleRenderPassDescriptor();
        m_oitRT->setRenderPassDescriptor(m_oitRpd);
        m_oitRT->create();
    }

    m_srbComposite = m_rhi->newShaderResourceBindings();
    const auto fs = QRhiShaderResourceBinding::FragmentStage;
    m_srbComposite->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, fs, m_uboComposite),
                                 QRhiShaderResourceBinding::sampledTexture(1, fs, m_colorTex, m_linear),
                                 QRhiShaderResourceBinding::sampledTexture(2, fs, m_accumTex, m_nearest),
                                 QRhiShaderResourceBinding::sampledTexture(3, fs, m_revealTex, m_nearest)});
    m_srbComposite->create();

    if (!m_psMesh) {
        m_psMesh = makePipeline("mesh.vert", "mesh.frag", m_opaqueRpd, m_srbBody, false, true, 1, 0, false);
        m_psEdges = makePipeline("line.vert", "line.frag", m_opaqueRpd, m_srbEdge, true, true, 1, 0, true);
        m_psGrid = makePipeline("line.vert", "line.frag", m_opaqueRpd, m_srbGrid, true, false, 1, 1, true);
        m_psOit = makePipeline("mesh.vert", "oit.frag", m_oitRpd, m_srbOit, false, false, 2, 2, false);
    }
}

void Viewport::initialize(QRhiCommandBuffer*)
{
    if (m_rhi != rhi()) {
        releaseAll();
        m_rhi = rhi();
        createStaticResources();
        m_geometryDirty = true;
        m_gridStep = 0;
        m_planeSize = 0;
    }
    const QSize sz = renderTarget()->pixelSize();
    if (sz != m_rtSize)
        createSizedResources(sz);

    // The widget's own render pass descriptor may change with its texture.
    if (m_psComposite)
        m_psComposite->deleteLater();
    m_psComposite = m_rhi->newGraphicsPipeline();
    m_psComposite->setShaderStages(
        {{QRhiShaderStage::Vertex, loadShader("composite.vert")}, {QRhiShaderStage::Fragment, loadShader("composite.frag")}});
    m_psComposite->setVertexInputLayout({});
    m_psComposite->setShaderResourceBindings(m_srbComposite);
    m_psComposite->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    m_psComposite->setCullMode(QRhiGraphicsPipeline::None);
    m_psComposite->create();
}

void Viewport::releaseAll()
{
    for (auto& g : m_bodies)
        g.release();
    m_bodies.clear();
    for (auto& g : m_profiles)
        g.release();
    m_profiles.clear();
    m_preview.release();
    m_planes.release();
    m_grid.release();
    auto del = [](auto*& r) {
        delete r;
        r = nullptr;
    };
    del(m_psMesh);
    del(m_psEdges);
    del(m_psGrid);
    del(m_psOit);
    del(m_psComposite);
    del(m_srbBody);
    del(m_srbEdge);
    del(m_srbGrid);
    del(m_srbOit);
    del(m_srbComposite);
    del(m_uboBody);
    del(m_uboEdge);
    del(m_uboGrid);
    del(m_uboOit);
    del(m_uboComposite);
    del(m_opaqueRT);
    del(m_oitRT);
    del(m_opaqueRpd);
    del(m_oitRpd);
    del(m_colorTex);
    del(m_depthTex);
    del(m_accumTex);
    del(m_revealTex);
    del(m_linear);
    del(m_nearest);
    m_rtSize = {};
    m_rhi = nullptr;
}

void Viewport::releaseResources() { releaseAll(); }

// ------------------------------------------------------------------ geometry upload

void Viewport::uploadMesh(GpuMesh& g, const RenderMesh& m, QRhiResourceUpdateBatch* u, bool edges)
{
    g.release();
    const quint32 nv = quint32(m.positions.size() / 3);
    g.pos = makeBuffer(QRhiBuffer::VertexBuffer, m.positions.data(), quint32(m.positions.size() * sizeof(float)), u);
    g.nrm = makeBuffer(QRhiBuffer::VertexBuffer, m.normals.data(), quint32(m.normals.size() * sizeof(float)), u);
    g.col = makeBuffer(QRhiBuffer::VertexBuffer, nullptr, nv * 16, u);
    g.idx = makeBuffer(QRhiBuffer::IndexBuffer, m.indices.data(), quint32(m.indices.size() * sizeof(uint32_t)), u);
    g.indexCount = quint32(m.indices.size());
    g.vertexFace.assign(nv, -1);
    for (size_t t = 0; t < m.triFace.size(); ++t)
        for (int k = 0; k < 3; ++k)
            g.vertexFace[m.indices[3 * t + size_t(k)]] = int(m.triFace[t]);
    if (edges && !m.edgePoints.empty()) {
        g.epos = makeBuffer(QRhiBuffer::VertexBuffer, m.edgePoints.data(),
                            quint32(m.edgePoints.size() * sizeof(float)), u);
        g.edgeVertexCount = quint32(m.edgePoints.size() / 3);
        g.ecol = makeBuffer(QRhiBuffer::VertexBuffer, nullptr, g.edgeVertexCount * 16, u);
    }
}

void Viewport::syncGeometry(QRhiResourceUpdateBatch* u)
{
    if (!m_geometryDirty)
        return;
    m_geometryDirty = false;
    m_colorVersion = ~0ull;
    const size_t nb = m_snap ? m_snap->bodies.size() : 0;
    for (size_t i = nb; i < m_bodies.size(); ++i)
        m_bodies[i].release();
    m_bodies.resize(nb);
    for (size_t i = 0; i < nb; ++i) {
        const BodyView& bv = m_snap->bodies[i];
        if (m_bodies[i].version != bv.meshVersion || !m_bodies[i].pos) {
            uploadMesh(m_bodies[i], bv.mesh, u, true);
            m_bodies[i].version = bv.meshVersion;
        }
    }
    for (auto& g : m_profiles)
        g.release();
    m_profiles.clear();
    if (m_snap) {
        m_profiles.resize(m_snap->profiles.size());
        for (size_t i = 0; i < m_snap->profiles.size(); ++i)
            uploadMesh(m_profiles[i], m_snap->profiles[i].mesh, u, false);
    }
    m_preview.release();
    if (m_snap && m_snap->preview)
        uploadMesh(m_preview, *m_snap->preview, u, false);
}

void Viewport::rebuildGrid(QRhiResourceUpdateBatch* u)
{
    const float step = std::pow(10.f, std::floor(std::log10(std::max(m_camera.distance * 0.12f, 0.01f))));
    if (step == m_gridStep && m_grid.epos)
        return;
    m_gridStep = step;
    m_grid.release();
    std::vector<float> pts, cols;
    const int n = 50;
    const float ext = step * n;
    auto line = [&](QVector3D a, QVector3D b, const QColor& c, float alpha) {
        for (const QVector3D& p : {a, b}) {
            pts.insert(pts.end(), {p.x(), p.y(), p.z()});
            pushColor(cols, c, alpha);
        }
    };
    const QColor gridColor(0x9a, 0xa4, 0xb2);
    for (int i = -n; i <= n; ++i) {
        const float v = float(i) * step;
        const float alpha = i == 0 ? 0.f : (i % 10 == 0 ? 0.30f : 0.11f);
        if (alpha > 0) {
            line({v, -ext, 0}, {v, ext, 0}, gridColor, alpha);
            line({-ext, v, 0}, {ext, v, 0}, gridColor, alpha);
        }
    }
    line({-ext, 0, 0}, {ext, 0, 0}, QColor(0xe0, 0x50, 0x50), 0.75f);
    line({0, -ext, 0}, {0, ext, 0}, QColor(0x60, 0xc0, 0x60), 0.75f);
    line({0, 0, 0}, {0, 0, ext * 0.25f}, QColor(0x50, 0x8c, 0xf0), 0.75f);
    m_grid.epos = makeBuffer(QRhiBuffer::VertexBuffer, pts.data(), quint32(pts.size() * sizeof(float)), u);
    m_grid.ecol = makeBuffer(QRhiBuffer::VertexBuffer, cols.data(), quint32(cols.size() * sizeof(float)), u);
    m_grid.edgeVertexCount = quint32(pts.size() / 3);
}

void Viewport::rebuildPlanes(QRhiResourceUpdateBatch* u)
{
    const float s = std::max(m_camera.sceneRadius * 0.6f, 20.f);
    if (s == m_planeSize && m_planes.pos)
        return;
    m_planeSize = s;
    RenderMesh m;
    const char* kinds[3] = {"XY", "XZ", "YZ"};
    for (uint32_t k = 0; k < 3; ++k) {
        const Placement pl = originPlacement(kinds[k]);
        const Vec3 y = pl.yDir();
        const uint32_t base = uint32_t(m.positions.size() / 3);
        for (const auto& [a, b] : {std::pair{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}}) {
            const Vec3 p = pl.xDir * (a * s) + y * (b * s);
            m.positions.insert(m.positions.end(), {float(p.x), float(p.y), float(p.z)});
            m.normals.insert(m.normals.end(), {float(pl.normal.x), float(pl.normal.y), float(pl.normal.z)});
        }
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        m.triFace.insert(m.triFace.end(), {k, k});
    }
    uploadMesh(m_planes, m, u, false);
    m_colorVersion = ~0ull;
}

void Viewport::updateColors(QRhiResourceUpdateBatch* u)
{
    static const HighlightState kEmpty;
    const HighlightState& hs = m_client ? m_client->highlight() : kEmpty;
    if (hs.version == m_colorVersion)
        return;
    m_colorVersion = hs.version;

    std::set<std::pair<int, int>> selFaces, selEdges, selProfiles;
    std::set<int> selBodies;
    int targetBody = -1;  // first selected body = boolean target (orange) when several are picked
    std::string selPlane;
    for (const SelItem& s : hs.selected) {
        if (s.kind == SelKind::Body) {
            if (selBodies.empty())
                targetBody = s.object;
            selBodies.insert(s.object);
        }
        if (s.kind == SelKind::Face) selFaces.insert({s.object, s.index});
        if (s.kind == SelKind::Edge) selEdges.insert({s.object, s.index});
        if (s.kind == SelKind::Profile) selProfiles.insert({s.object, 0});
        if (s.kind == SelKind::OriginPlane) selPlane = s.name;
    }
    const SelItem& hv = hs.hover;
    const float dim = hs.dimBodies ? 0.8f : 1.f;

    std::vector<float> cols;
    for (size_t b = 0; b < m_bodies.size() && m_snap && b < m_snap->bodies.size(); ++b) {
        GpuMesh& g = m_bodies[b];
        cols.clear();
        cols.reserve(g.vertexFace.size() * 4);
        const bool wholeBody = selBodies.count(int(b)) > 0;
        const QColor bodyColor = !wholeBody ? Colors::Body
                                 : (int(b) == targetBody && selBodies.size() > 1)
                                     ? Colors::TargetBody
                                     : Colors::Selected;
        for (int f : g.vertexFace) {
            QColor c = bodyColor;
            if (selFaces.count({int(b), f}))
                c = Colors::Selected;
            else if (hv.kind == SelKind::Face && hv.object == int(b) && hv.index == f)
                c = Colors::Hover;
            if (dim < 1.f)
                c = c.darker(125);
            pushColor(cols, c);
        }
        if (!cols.empty())
            u->uploadStaticBuffer(g.col, 0, quint32(cols.size() * sizeof(float)), cols.data());

        if (g.ecol) {
            cols.assign(size_t(g.edgeVertexCount) * 4, 0.f);
            const auto& edges = m_snap->bodies[b].mesh.edges;
            for (size_t ei = 0; ei < edges.size(); ++ei) {
                QColor c = Colors::Edge;
                if (selEdges.count({int(b), int(ei)}))
                    c = Colors::Selected;
                else if (hv.kind == SelKind::Edge && hv.object == int(b) && hv.index == int(ei))
                    c = Colors::Hover;
                for (uint32_t k = edges[ei].first; k < edges[ei].first + edges[ei].count && k < g.edgeVertexCount; ++k) {
                    cols[4 * k] = float(c.redF());
                    cols[4 * k + 1] = float(c.greenF());
                    cols[4 * k + 2] = float(c.blueF());
                    cols[4 * k + 3] = 1.f;
                }
            }
            u->uploadStaticBuffer(g.ecol, 0, quint32(cols.size() * sizeof(float)), cols.data());
        }
    }

    for (size_t p = 0; p < m_profiles.size(); ++p) {
        GpuMesh& g = m_profiles[p];
        float a = 0.28f;
        QColor c = Colors::Profile;
        if (selProfiles.count({int(p), 0})) {
            c = Colors::Selected;
            a = 0.62f;
        }
        else if (hv.kind == SelKind::Profile && hv.object == int(p)) {
            c = Colors::Hover;
            a = 0.5f;
        }
        cols.clear();
        for (size_t v = 0; v < g.vertexFace.size(); ++v)
            pushColor(cols, c, a);
        if (!cols.empty())
            u->uploadStaticBuffer(g.col, 0, quint32(cols.size() * sizeof(float)), cols.data());
    }

    if (m_preview.col && m_snap) {
        cols.clear();
        const QColor c = previewColor(m_snap->previewOp);
        for (size_t v = 0; v < m_preview.vertexFace.size(); ++v)
            pushColor(cols, c, 0.38f);
        if (!cols.empty())
            u->uploadStaticBuffer(m_preview.col, 0, quint32(cols.size() * sizeof(float)), cols.data());
    }

    if (m_planes.col) {
        cols.clear();
        const QColor base[3] = {QColor(0x4a, 0x8c, 0xff), QColor(0x5c, 0xd6, 0x8a), QColor(0xff, 0x6a, 0x5a)};
        const char* names[3] = {"XY", "XZ", "YZ"};
        for (int f : m_planes.vertexFace) {
            const bool hot = (hv.kind == SelKind::OriginPlane && hv.name == names[f]) || selPlane == names[f];
            pushColor(cols, base[f], hot ? 0.5f : 0.2f);
        }
        u->uploadStaticBuffer(m_planes.col, 0, quint32(cols.size() * sizeof(float)), cols.data());
    }
}

// ------------------------------------------------------------------ frame

void Viewport::writeUbo(QRhiResourceUpdateBatch* u, QRhiBuffer* ubo, const QMatrix4x4& mvp, float alpha, float bias)
{
    float d[28];
    std::memcpy(d, mvp.constData(), 16 * sizeof(float));
    const QVector3D e = m_camera.eye(), f = m_camera.forward();
    d[16] = e.x();
    d[17] = e.y();
    d[18] = e.z();
    d[19] = m_camera.orthographic ? 1.f : 0.f;
    d[20] = alpha;
    d[21] = bias;
    d[22] = 0;
    d[23] = 0;
    d[24] = f.x();
    d[25] = f.y();
    d[26] = f.z();
    d[27] = 0;
    u->updateDynamicBuffer(ubo, 0, kUboSize, d);
}

void Viewport::drawMesh(QRhiCommandBuffer* cb, const GpuMesh& g)
{
    if (!g.indexCount)
        return;
    const QRhiCommandBuffer::VertexInput vin[] = {{g.pos, 0}, {g.nrm, 0}, {g.col, 0}};
    cb->setVertexInput(0, 3, vin, g.idx, 0, QRhiCommandBuffer::IndexUInt32);
    cb->drawIndexed(g.indexCount);
}

void Viewport::drawEdges(QRhiCommandBuffer* cb, const GpuMesh& g)
{
    if (!g.edgeVertexCount)
        return;
    const QRhiCommandBuffer::VertexInput vin[] = {{g.epos, 0}, {g.ecol, 0}};
    cb->setVertexInput(0, 2, vin);
    cb->draw(g.edgeVertexCount);
}

void Viewport::render(QRhiCommandBuffer* cb)
{
    QRhiResourceUpdateBatch* u = m_rhi->nextResourceUpdateBatch();
    const QSize px = renderTarget()->pixelSize();
    const QRhiViewport vp(0, 0, float(px.width()), float(px.height()));

    syncGeometry(u);
    rebuildGrid(u);
    rebuildPlanes(u);
    updateColors(u);

    static const HighlightState kEmpty;
    const HighlightState& hs = m_client ? m_client->highlight() : kEmpty;

    const QMatrix4x4 mvp = m_rhi->clipSpaceCorrMatrix() * m_camera.viewProjection(size());
    writeUbo(u, m_uboBody, mvp, 1.f, 0.f);
    writeUbo(u, m_uboEdge, mvp, 1.f, 2e-5f);
    writeUbo(u, m_uboGrid, mvp, 1.f, 0.f);
    writeUbo(u, m_uboOit, mvp, 1.f, 1e-5f);

    // 1) opaque
    cb->beginPass(m_opaqueRT, QColor::fromRgbF(0, 0, 0, 0), {1.0f, 0}, u);
    cb->setGraphicsPipeline(m_psGrid);
    cb->setViewport(vp);
    cb->setShaderResources(m_srbGrid);
    drawEdges(cb, m_grid);
    cb->setGraphicsPipeline(m_psMesh);
    cb->setViewport(vp);
    cb->setShaderResources(m_srbBody);
    for (const auto& g : m_bodies)
        drawMesh(cb, g);
    cb->setGraphicsPipeline(m_psEdges);
    cb->setViewport(vp);
    cb->setShaderResources(m_srbEdge);
    for (const auto& g : m_bodies)
        drawEdges(cb, g);
    cb->endPass();

    // 2) order-independent transparency
    cb->beginPass(m_oitRT, QColor::fromRgbF(0, 0, 0, 0), {1.0f, 0});
    cb->setGraphicsPipeline(m_psOit);
    cb->setViewport(vp);
    cb->setShaderResources(m_srbOit);
    for (size_t i = 0; i < m_profiles.size(); ++i)
        if (i < hs.profileVisible.size() && hs.profileVisible[i])
            drawMesh(cb, m_profiles[i]);
    if (hs.showPreview)
        drawMesh(cb, m_preview);
    if (hs.showOriginPlanes)
        drawMesh(cb, m_planes);
    cb->endPass();

    // 3) composite
    QRhiResourceUpdateBatch* u2 = m_rhi->nextResourceUpdateBatch();
    const QColor top = Colors::BackgroundTop, bottom = Colors::Background;
    const float cd[12] = {float(top.redF()),    float(top.greenF()),    float(top.blueF()),    1.f,
                          float(bottom.redF()), float(bottom.greenF()), float(bottom.blueF()), 1.f,
                          float(px.width()),    float(px.height()),     m_rhi->isYUpInFramebuffer() ? 1.f : 0.f, 0.f};
    u2->updateDynamicBuffer(m_uboComposite, 0, sizeof(cd), cd);
    cb->beginPass(renderTarget(), Qt::black, {1.0f, 0}, u2);
    cb->setGraphicsPipeline(m_psComposite);
    cb->setViewport(vp);
    cb->setShaderResources(m_srbComposite);
    cb->draw(3);
    cb->endPass();

    m_overlay->update();
}

// ------------------------------------------------------------------ overlay

void Viewport::paintOverlayContent(QPainter& p)
{
    if (m_client)
        m_client->paintOverlay(p);

    // Orientation triad (bottom-left)
    const QPointF o(48, height() - 48);
    struct Axis
    {
        QVector3D d;
        QColor c;
        const char* label;
    };
    Axis axes[3] = {{{1, 0, 0}, QColor(0xe0, 0x50, 0x50), "X"},
                    {{0, 1, 0}, QColor(0x60, 0xc0, 0x60), "Y"},
                    {{0, 0, 1}, QColor(0x50, 0x8c, 0xf0), "Z"}};
    std::sort(std::begin(axes), std::end(axes), [&](const Axis& a, const Axis& b) {
        return QVector3D::dotProduct(a.d, m_camera.forward()) > QVector3D::dotProduct(b.d, m_camera.forward());
    });
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(11);
    p.setFont(f);
    for (const Axis& a : axes) {
        const QPointF v(QVector3D::dotProduct(a.d, m_camera.right()) * 30.0,
                        -QVector3D::dotProduct(a.d, m_camera.up()) * 30.0);
        p.setPen(QPen(a.c, 2.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(o, o + v);
        p.drawText(QRectF(o + v * 1.3 - QPointF(8, 8), QSizeF(16, 16)), Qt::AlignCenter, a.label);
    }
}

// ------------------------------------------------------------------ input

void Viewport::mousePressEvent(QMouseEvent* e)
{
    setFocus(Qt::MouseFocusReason);
    m_lastPos = e->position();
    const bool alt = e->modifiers() & Qt::AltModifier;
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    // Fusion-style navigation: MMB = pan, Shift+MMB = orbit. Laptop: Alt+LMB orbit, Alt+Shift+LMB pan.
    if (e->button() == Qt::MiddleButton) {
        m_nav = shift ? Nav::Orbit : Nav::Pan;
        m_anim->stop();
        return;
    }
    if (e->button() == Qt::LeftButton && alt) {
        m_nav = shift ? Nav::Pan : Nav::Orbit;
        m_anim->stop();
        return;
    }
    if (m_client)
        m_client->viewportMousePress(e);
}

void Viewport::mouseMoveEvent(QMouseEvent* e)
{
    if (m_nav != Nav::None) {
        const QPointF d = e->position() - m_lastPos;
        m_lastPos = e->position();
        if (m_nav == Nav::Orbit)
            m_camera.orbit(float(d.x()), float(d.y()));
        else
            m_camera.pan(float(d.x()), float(d.y()), size());
        update();
        refreshOverlay();
        emit cameraChanged();
        return;
    }
    if (m_client)
        m_client->viewportMouseMove(e);
}

void Viewport::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_nav != Nav::None) {
        m_nav = Nav::None;
        return;
    }
    if (m_client)
        m_client->viewportMouseRelease(e);
}

void Viewport::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() == Qt::MiddleButton) {
        fitAll();
        return;
    }
    if (m_client)
        m_client->viewportMouseDoubleClick(e);
}

void Viewport::wheelEvent(QWheelEvent* e)
{
    m_anim->stop();
    m_camera.zoom(float(e->angleDelta().y()) / 120.f, e->position(), size());
    update();
    refreshOverlay();
    emit cameraChanged();
}

void Viewport::keyPressEvent(QKeyEvent* e)
{
    if (m_client && m_client->viewportKeyPress(e))
        return;
    QRhiWidget::keyPressEvent(e);
}

}  // namespace silex::ui
