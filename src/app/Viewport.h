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
#include <set>
#include <rhi/qrhi.h>
#include <vector>

#include "Camera.h"
#include "Picker.h"
#include "ViewportTool.h"
#include "model/RenderModel.h"

namespace silex::ui {

class ViewCube;

class Viewport : public QRhiWidget
{
    Q_OBJECT
public:
    explicit Viewport(QWidget* parent = nullptr);
    ~Viewport() override;

    void setClient(ViewportClient* c) { m_client = c; }
    void setSnapshot(std::shared_ptr<const ModelSnapshot> snap);

    /// v0.3: extra translucent meshes (interference volumes...) drawn in the OIT pass.
    struct OverlayMesh
    {
        RenderMesh mesh;
        QColor color;
        float alpha = 0.6f;
    };
    void setOverlayMeshes(std::vector<OverlayMesh> meshes);

    /// v0.5 lot 4: printer build plate (centered on the origin, Z = 0 up to `size.z`), drawn with the grid.
    struct BedSpec
    {
        bool on = false;
        QVector3D size{220, 220, 250};
        bool exceeded = false;  ///< a body leaves the build volume: drawn in red
        bool operator==(const BedSpec&) const = default;
    };
    void setBed(const BedSpec& bed);
    void highlightChanged();  ///< call when the client's HighlightState changed

    Camera& camera() { return m_camera; }
    const Camera& camera() const { return m_camera; }
    void animateTo(const QQuaternion& rot, std::optional<QVector3D> target = {}, std::optional<float> distance = {},
                   int durationMs = 320);
    void fitAll(bool animated = true);
    /// Default isometric view, fitted on the model (ViewCube Home button, key 0 + W).
    void homeView();
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
    /// Tab belongs to the sketch editor (switches the floating dimension inputs), not to focus chains.
    bool focusNextPrevChild(bool) override { return false; }
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
    void writeUbo(QRhiResourceUpdateBatch* u, QRhiBuffer* ubo, const QMatrix4x4& mvp, float alpha, float bias,
                  bool clip = true);
    bool bodyTranslucent(size_t i) const;

    // --- state
    Camera m_camera;
    ViewportClient* m_client = nullptr;
    std::shared_ptr<const ModelSnapshot> m_snap;
    bool m_geometryDirty = true;
    uint64_t m_colorVersion = ~0ull;
    std::vector<bool> m_pickerProfiles;
    std::set<Uuid> m_pickerHidden;
    bool bodyHidden(size_t i) const;
    Picker m_picker;
    QWidget* m_overlay = nullptr;
    QVariantAnimation* m_anim = nullptr;
    ViewCube* m_viewCube = nullptr;  ///< ergonomics lot: orientation cube (top-right)
    enum class Nav { None, Orbit, Pan } m_nav = Nav::None;
    QPointF m_lastPos;
    float m_gridStep = 0;
    BedSpec m_bed;
    bool m_bedDirty = false;
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
    std::vector<OverlayMesh> m_overlaySrc;
    std::vector<GpuMesh> m_overlays;
    bool m_overlaysDirty = false;
    std::vector<GpuMesh> m_profiles;
    GpuMesh m_preview;
    GpuMesh m_planes;
    GpuMesh m_grid;  ///< uses epos/ecol only
};

}  // namespace silex::ui
