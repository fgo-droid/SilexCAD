#pragma once
// Contract between the 3D viewport (rendering, navigation, picking) and whoever drives the
// interaction (the Workspace and its active editing session).

#include <cstdint>
#include <set>
#include <vector>

#include "Selection.h"
#include "model/Math.h"

class QMouseEvent;
class QKeyEvent;
class QPainter;

namespace silex::ui {

struct HighlightState
{
    SelItem hover;
    std::vector<SelItem> selected;
    std::vector<bool> profileVisible;  ///< per snapshot profile
    bool showOriginPlanes = false;
    bool showPreview = true;
    bool dimBodies = false;            ///< sketch editing: bodies drawn slightly transparent-looking
    bool ghostBodies = false;          ///< v0.3: every body translucent (interference X-ray view)
    std::set<Uuid> hiddenBodies;       ///< v0.4: bodies hidden with V / Isolate (display only)
    bool clipOn = false;               ///< v0.3: section view
    Vec3 clipNormal{0, 0, 1};          ///< material where dot(n, p) > clipD is hidden
    double clipD = 0;
    uint64_t version = 0;              ///< bump on any change
};

class ViewportClient
{
public:
    virtual ~ViewportClient() = default;
    virtual void viewportMousePress(QMouseEvent* e) = 0;
    virtual void viewportMouseMove(QMouseEvent* e) = 0;
    virtual void viewportMouseRelease(QMouseEvent* e) = 0;
    virtual void viewportMouseDoubleClick(QMouseEvent* e) = 0;
    virtual bool viewportKeyPress(QKeyEvent* e) = 0;
    virtual void paintOverlay(QPainter& p) = 0;
    virtual const HighlightState& highlight() const = 0;
};

}  // namespace silex::ui
