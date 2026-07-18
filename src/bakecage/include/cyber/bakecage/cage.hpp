#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"

// Editable projection cage (task 11.2). A Cage stores a per-vertex cage
// distance over an EditMesh (the low-poly `cyber::Mesh` consumed by the bake).
// Rays for a high-to-low bake start at `position + normal * cageDistance` and
// travel inward, so the cage is what lets an artist push the projection
// envelope out where the low-poly dips inside the high-poly and pull it in to
// avoid catching neighbouring shells.
//
// Editing follows the sculpt-brush model:
//   * tweakDistance  - additive brush stroke with a radial falloff;
//   * setVertexDistance - a "double-tap" per-vertex override that pins a
//     vertex to an exact distance and makes it immune to later falloff tweaks;
//   * relax - Laplacian smoothing of the distance field;
//   * reset - restore the uniform default over a region (or the whole cage).
//
// A Cage is bound to the topology of the Mesh it was built from (a snapshot,
// like Bvh). Its state is a pair of flat per-vertex arrays and is serialisable
// for document persistence via save()/load() and serialize()/deserialize().
namespace cyber::bakecage {

// Radial brush falloff curve, evaluated on the normalised distance t in [0,1]
// from the brush centre (0) to its radius (1). weight(0) == 1, weight(>=1) == 0.
enum class Falloff {
    Constant,  // 1 inside the radius, 0 outside (hard edge)
    Linear,    // 1 - t
    Smooth,    // smoothstep, C1-continuous at both ends
    Sharp,     // (1 - t)^2, concentrated at the centre
    Root,      // sqrt(1 - t), wide plateau
};

[[nodiscard]] float falloffWeight(Falloff curve, float t);

// A spherical brush in object space. Vertices within `radius` of `center` are
// affected; `curve` shapes the per-vertex weight.
struct Brush {
    Vec3 center;
    float radius = 1.0f;
    Falloff curve = Falloff::Smooth;
};

// One projection ray produced from the cage, ready for src/bake to cast: it
// starts on the outside of the surface and travels inward toward the Target.
struct CageRay {
    Vec3 origin;       // position + normal * cageDistance
    Vec3 direction;    // -normal (unit), or {0,0,0} for an isolated vertex
    float maxDistance;  // 2 * cageDistance (full sweep through the surface)
    VertexId vertex;
};

// Flat, POD-array snapshot of a cage's state; the serialisation payload.
struct CageState {
    float defaultDistance = 0.1f;
    std::vector<float> distances;              // per vertex slot (by VertexId)
    std::vector<std::uint8_t> overridden;      // 1 = pinned by setVertexDistance
};

class Cage {
public:
    // Builds a uniform cage of `defaultDistance` over `mesh`. The cage keeps a
    // reference to `mesh` for adjacency/geometry queries and must not outlive it.
    Cage(const Mesh& mesh, float defaultDistance);

    [[nodiscard]] const Mesh& mesh() const { return *m_mesh; }
    [[nodiscard]] float defaultDistance() const { return m_defaultDistance; }
    [[nodiscard]] std::size_t vertexSlots() const { return m_distance.size(); }

    // Current cage distance at a vertex (default for slots never touched).
    [[nodiscard]] float distance(VertexId v) const;
    // True if `v` was pinned by setVertexDistance and is immune to tweaks.
    [[nodiscard]] bool isOverridden(VertexId v) const;

    // --- edit operations ---------------------------------------------------

    // Additive brush stroke: for each vertex in `region`, adds
    // `delta * falloffWeight(brush.curve, dist/brush.radius)` to its cage
    // distance. Pinned (overridden) vertices are skipped entirely, so a
    // per-vertex override survives tweaks of its neighbours. The resulting
    // distance is clamped to be non-negative. Returns the number of vertices
    // actually modified.
    std::size_t tweakDistance(std::span<const VertexId> region, float delta, const Brush& brush);

    // "Double-tap" per-vertex override: pins `v` to exactly `d`, independent of
    // any falloff. Pinned vertices stay put under tweakDistance and relax.
    void setVertexDistance(VertexId v, float d);
    // Clears a per-vertex override without changing the stored distance.
    void clearOverride(VertexId v);

    // Laplacian smoothing of the distance field: each free vertex moves a
    // fraction `strength` (in [0,1]) toward the average of its edge-adjacent
    // vertices, repeated `iterations` times. Pinned vertices act as fixed
    // boundary constraints. Reduces distance variance.
    void relax(int iterations = 1, float strength = 0.5f);

    // Restores the uniform default distance over `region` and clears any
    // overrides there.
    void reset(std::span<const VertexId> region);
    // Restores the whole cage to the uniform default and clears all overrides.
    void reset();

    // --- ray generation for src/bake --------------------------------------

    // Per-vertex projection rays (position + normal*distance, aimed inward).
    // One ray per alive vertex, in ascending VertexId order.
    [[nodiscard]] std::vector<CageRay> projectionRays() const;

    // --- persistence -------------------------------------------------------

    [[nodiscard]] CageState save() const;
    // Loads a state whose arrays must match this cage's vertex-slot count.
    // Returns false (and changes nothing) on a size mismatch.
    bool load(const CageState& state);

    // Binary blob for document persistence.
    [[nodiscard]] std::vector<std::byte> serialize() const;
    // Restores from serialize() output; returns false on a malformed or
    // size-mismatched blob.
    bool deserialize(std::span<const std::byte> bytes);

private:
    const Mesh* m_mesh;
    float m_defaultDistance;
    std::vector<float> m_distance;
    std::vector<std::uint8_t> m_overridden;
};

}  // namespace cyber::bakecage
