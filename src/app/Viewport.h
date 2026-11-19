#pragma once
// 3D viewport on Qt RHI (Direct3D 11/12, Vulkan or Metal — no legacy OpenGL path).
//
// Frame = 3 passes:
//   1. opaque pass   -> offscreen RGBA8 + depth texture (grid, bodies, edges)
//   2. OIT pass      -> weighted blended order-independent transparency
//                       (profiles, operation previews, origin planes), depth-tested
//                       against pass 1 without writing depth
//   3. composite     -> background gradient + FXAA + OIT resolve into the widget

#include <QPointer>
#include <QRhiWidget>
#include <QVariantAnimation>
#include <memory>
#include <optional>
#include <rhi/qrhi.h>
#include <vector>

#include "Camera.h"
#include "Picker.h"
#include "ViewportTool.h"
#include "model/RenderModel.h"

namespace silex::ui {

class Viewport : public QRhiWidget
{
    Q_OBJECT
public:
    explicit Viewport(QWidget* parent = nullptr);
    ~Viewport() override;

    void setClient(ViewportClient* c) { m_client = c; }
    void setSnapshot(std::shared_ptr<const ModelSnapshot> snap);
    void highlightChanged();  ///< call when the client's HighlightState changed

    Camera& camera() { return m_camera; }
    const Camera& camera() const { return m_camera; }
    void animateTo(const QQuaternion& rot, std::optional<QVector3D> target = {}, std::optional<float> distance = {});
    void fitAll(bool animated = true);
    void setStandardView(int which);

    SelItem pickAt(QPointF px, uint32_t filter) const;
    std::optional<Vec3> rayPlane(QPointF px, const Placement& pl) const;
    bool toScreen(const Vec3& p, QPointF* out) const;
    QSize logicalSize() const { return size(); }
    QWidget* overlay() const { return m_overlay; }
    void refreshOverlay();

    /// Debug / CI helper: renders one frame offscreen and saves it.
    bool saveScreenshot(const QString& path);

signals:
    void cameraChanged();

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    struct GpuMesh
    {
        QRhiBuffer* pos = nullptr;
        QRhiBuffer* nrm = nullptr;
        QRhiBuffer* col = nullptr;
        QRhiBuffer* idx = nullptr;
        quint32 indexCount = 0;
        QRhiBuffer* epos = nullptr;
        QRhiBuffer* ecol = nullptr;
        quint32 edgeVertexCount = 0;
        std::vector<int> vertexFace;  ///< face of each vertex
        uint64_t version = 0;
        void release();
    };

    void createStaticResources();
    void createSizedResources(QSize sz);
    void releaseAll();
    QRhiBuffer* makeBuffer(QRhiBuffer::UsageFlags usage, const void* data, quint32 bytes, QRhiResourceUpdateBatch* u);
    void uploadMesh(GpuMesh& g, const RenderMesh& m, QRhiResourceUpdateBatch* u, bool edges);
    void syncGeometry(QRhiResourceUpdateBatch* u);
    void updateColors(QRhiResourceUpdateBatch* u);
    void rebuildGrid(QRhiResourceUpdateBatch* u);
    void rebuildPlanes(QRhiResourceUpdateBatch* u);
    void rebuildPicker();
    void paintOverlayContent(QPainter& p);
    QShader loadShader(const QString& name) const;
    QRhiGraphicsPipeline* makePipeline(const QString& vs, const QString& fs, QRhiRenderPassDescriptor* rpd,
                                       QRhiShaderResourceBindings* layout, bool lines, bool depthWrite,
                                       int colorTargets, int blendMode, bool lineVertexLayout);
    void drawMesh(QRhiCommandBuffer* cb, const GpuMesh& g);
    void drawEdges(QRhiCommandBuffer* cb, const GpuMesh& g);
    void writeUbo(QRhiResourceUpdateBatch* u, QRhiBuffer* ubo, const QMatrix4x4& mvp, float alpha, float bias);

    // --- state
    Camera m_camera;
    ViewportClient* m_client = nullptr;
    std::shared_ptr<const ModelSnapshot> m_snap;
    bool m_geometryDirty = true;
    uint64_t m_colorVersion = ~0ull;
    std::vector<bool> m_pickerProfiles;
    Picker m_picker;
    QWidget* m_overlay = nullptr;
    QVariantAnimation* m_anim = nullptr;
    enum class Nav { None, Orbit, Pan } m_nav = Nav::None;
    QPointF m_lastPos;
    float m_gridStep = 0;
    float m_planeSize = 0;

    // --- RHI
    QRhi* m_rhi = nullptr;
    QSize m_rtSize;
    QRhiTexture* m_colorTex = nullptr;
    QRhiTexture* m_depthTex = nullptr;
    QRhiTexture* m_accumTex = nullptr;
    QRhiTexture* m_revealTex = nullptr;
    QRhiTextureRenderTarget* m_opaqueRT = nullptr;
    QRhiTextureRenderTarget* m_oitRT = nullptr;
    QRhiRenderPassDescriptor* m_opaqueRpd = nullptr;
    QRhiRenderPassDescriptor* m_oitRpd = nullptr;
    QRhiSampler* m_linear = nullptr;
    QRhiSampler* m_nearest = nullptr;

    QRhiBuffer* m_uboBody = nullptr;
    QRhiBuffer* m_uboEdge = nullptr;
    QRhiBuffer* m_uboGrid = nullptr;
    QRhiBuffer* m_uboOit = nullptr;
    QRhiBuffer* m_uboComposite = nullptr;
    QRhiShaderResourceBindings* m_srbBody = nullptr;
    QRhiShaderResourceBindings* m_srbEdge = nullptr;
    QRhiShaderResourceBindings* m_srbGrid = nullptr;
    QRhiShaderResourceBindings* m_srbOit = nullptr;
    QRhiShaderResourceBindings* m_srbComposite = nullptr;

    QRhiGraphicsPipeline* m_psMesh = nullptr;
    QRhiGraphicsPipeline* m_psEdges = nullptr;
    QRhiGraphicsPipeline* m_psGrid = nullptr;
    QRhiGraphicsPipeline* m_psOit = nullptr;
    QRhiGraphicsPipeline* m_psComposite = nullptr;

    std::vector<GpuMesh> m_bodies;
    std::vector<GpuMesh> m_profiles;
    GpuMesh m_preview;
    GpuMesh m_planes;
    GpuMesh m_grid;  ///< uses epos/ecol only
};

}  // namespace silex::ui
