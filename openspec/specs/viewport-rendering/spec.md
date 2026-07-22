# viewport-rendering Specification

## Purpose
TBD - created by archiving change bootstrap-v1-platform. Update Purpose after archive.
## Requirements
### Requirement: Render hardware interface with Metal and Vulkan backends
The viewport SHALL render through a thin render-hardware-interface with exactly two backends: Metal (macOS, iPadOS/iOS) and Vulkan (Windows, Linux, Android). No OpenGL backend SHALL exist. All rendering features SHALL work identically on both backends; techniques unavailable on mobile GPUs (e.g. geometry shaders) SHALL NOT be used.

#### Scenario: Feature parity across backends
- **WHEN** the same document is viewed on Metal and Vulkan builds
- **THEN** all overlays, wireframes, and previews SHALL be available and visually equivalent

### Requirement: Remesh stage previews
For automatic remeshing the viewport SHALL provide selectable previews of the pipeline stages — Source, Isotropic, Parameterization, Result — each enabled once its data exists, defaulting to Result after a successful run. The Parameterization preview SHALL show the UV cross-pattern, singular-vertex markers, and extracted-connection overlay. Preview meshes SHALL be cached; switching previews SHALL NOT recompute anything.

#### Scenario: Inspect singularities
- **WHEN** the Parameterization preview is selected after a remesh
- **THEN** singular vertices SHALL be visibly marked and connection traces overlaid on the surface

### Requirement: Retopology rendering
In retopo stages the viewport SHALL render the Target smooth-shaded with vertex colors, the EditMesh as a snapped overlay with configurable opacity and occlusion depth, wireframe on the EditMesh, landmark loop colors, pin markers, symmetry-plane indicator, and visible brush radius for brush-based actions (Relax/Move/Erase).

#### Scenario: Brush radius is visible
- **WHEN** the user activates Relax
- **THEN** the current brush footprint SHALL be displayed on the surface before and during the stroke (CozyBlanket's invisible radius was a known complaint)

### Requirement: Wireframe on every platform
Wireframe overlays SHALL render with consistent screen-space width on all backends including mobile, SHALL be toggleable per mesh, and SHALL NOT rely on geometry shaders.

#### Scenario: Wireframe toggle
- **WHEN** the user disables the EditMesh wireframe
- **THEN** the overlay SHALL disappear without affecting shading (AutoRemesher's wireframe could not be turned off)

### Requirement: Camera and navigation
The viewport SHALL support orbit, pan, and zoom via mouse (desktop) and one/two-finger gestures (touch), double-tap to frame the model, and clamped zoom limits. Camera gestures SHALL require their exact defined contact counts; additional contacts (palm) SHALL be rejected rather than interpreted as zoom.

#### Scenario: Palm contact does not zoom
- **WHEN** a palm rests on the screen while two fingers orbit
- **THEN** the camera SHALL orbit normally without erratic zooming (a top CozyBlanket complaint)

### Requirement: Performance floor and scaling options
The viewport SHALL sustain 60 fps with a 5 M-triangle Target plus a 100 k-vertex EditMesh on reference hardware (Apple M1 iPad Pro class; mid-range desktop GPU), and SHALL offer a resolution-downscale option for battery/thermal headroom. Frame pacing SHALL support 120 Hz displays where available.

#### Scenario: Heavy scene stays smooth
- **WHEN** a 5 M-triangle Target is orbited on reference hardware
- **THEN** the frame rate SHALL NOT drop below 60 fps

### Requirement: Multi-viewport
The system SHALL support side-by-side viewports (e.g. shaded preview next to wireframe retopo view; high-poly next to low-poly in bake stage; 3D next to UV 2D with edge-swipe to maximize either).

#### Scenario: Bake split view
- **WHEN** the bake stage is entered
- **THEN** Target and EditMesh SHALL be viewable side by side with linked cameras

