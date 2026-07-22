#pragma once

#include "cyber/core/math.hpp"

// Bake-preview descriptors (task 11.4, best-effort). These are pure CPU data
// structures: they describe *what* the viewport should show when previewing a
// bake, so that a GPU/renderer layer (not part of this headless module) can
// consume them. No rendering happens here; only the movable preview light and
// the viewport preview state live in this header.
namespace cyber::bakecage {

// A single movable light used to preview a baked normal / lit result in the
// viewport. `directional` reinterprets `position` as a direction (a distant
// sun); otherwise it is a point light in object space.
struct PreviewLight {
    Vec3 position{0.0f, 0.0f, 1.0f};
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    bool directional = true;

    // Absolute / relative moves, so a UI gizmo can drag the light around.
    void moveTo(Vec3 p) { position = p; }
    void moveBy(Vec3 delta) { position = position + delta; }

    // For a directional light, the unit direction the light travels along
    // (from the light toward the surface). For a point light, returns the
    // stored position unchanged.
    [[nodiscard]] Vec3 direction() const { return directional ? normalized(position) : position; }
};

// How the viewport shades the preview.
enum class PreviewShading {
    Flat,       // flat low-poly shading, no baked map applied
    Lit,        // low-poly lit by the preview light
    NormalMap,  // baked tangent-space normals applied, lit by the preview light
    Baked,      // show the baked texture unlit (albedo / AO / etc.)
};

// The full viewport bake-preview state: which overlays are shown, how the
// surface is shaded, and the light driving it. A renderer reads this; toggling
// a field changes what the artist sees without rebaking.
struct BakePreview {
    PreviewShading shading = PreviewShading::NormalMap;
    PreviewLight light;

    // Overlays.
    bool showCage = false;   // draw the projection cage envelope
    bool showRays = false;   // draw per-vertex projection rays
    bool showLinks = false;  // draw component-link connections
    bool showWireframe = false;

    // Blend of the cage overlay when shown, in [0,1].
    float cageOpacity = 0.35f;

    // Convenience presets a UI can bind to buttons.
    [[nodiscard]] static BakePreview cageInspection() {
        BakePreview p;
        p.shading = PreviewShading::Flat;
        p.showCage = true;
        p.showRays = true;
        p.showWireframe = true;
        return p;
    }
    [[nodiscard]] static BakePreview resultInspection() {
        BakePreview p;
        p.shading = PreviewShading::NormalMap;
        return p;
    }
};

}  // namespace cyber::bakecage
