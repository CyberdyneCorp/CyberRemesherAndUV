#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "cyber/core/mesh.hpp"
#include "cyber/retopo/pins.hpp"
#include "cyber/retopo/relax.hpp"
#include "cyber/retopo/symmetry.hpp"

// Retopology commands (manual-retopology spec, roadmap 9.4): Auto Relax honoring
// pins, whole-mesh commands (subdivide-all / relax-all / mirror-all) and
// landmark loop tagging. Builds on the existing PinSet, relax() and
// applySymmetry(); nothing is duplicated. Header-only and inline.
namespace cyber::retopo {

// Auto Relax: relax the whole mesh with sensible defaults, honoring explicit
// pins and auto-pinning grid corners so regular patterns survive.
inline void autoRelax(Mesh& mesh, const PinSet& pins, int iterations = 4,
                      const SurfaceSnapper* snap = nullptr) {
    RelaxParams params;
    params.iterations = iterations;
    params.brushRadius = 0.0f;  // whole mesh (no visible-radius mask)
    params.autoPinCorners = true;
    relax(mesh, params, &pins, snap);
}

// Whole-mesh relax-all: one tangential smoothing sweep over the whole mesh,
// honoring optional pins.
inline void relaxAll(Mesh& mesh, const PinSet* pins = nullptr, int iterations = 1,
                     const SurfaceSnapper* snap = nullptr) {
    RelaxParams params;
    params.iterations = iterations;
    params.brushRadius = 0.0f;
    relax(mesh, params, pins, snap);
}

// Whole-mesh subdivide-all: linear (Catmull-Clark topology) subdivision to
// quads, replacing the mesh in place.
inline void subdivideAll(Mesh& mesh) { mesh = mesh.linearSubdivide(); }

// Whole-mesh mirror-all: bake the symmetry plane into real geometry. Returns
// the number of faces added.
inline std::size_t mirrorAll(Mesh& mesh, const Symmetry& sym,
                             const SurfaceSnapper* snap = nullptr) {
    return applySymmetry(mesh, sym, snap);
}

// Landmark loop tagging (manual-retopology spec, "landmark loops"): name edge
// loops (ordered vertex lists) so they can be recalled, pinned or measured.
// std::map keeps names in deterministic order.
class LandmarkLoops {
public:
    void tag(const std::string& name, std::vector<VertexId> loop) {
        m_loops[name] = std::move(loop);
    }
    [[nodiscard]] bool has(const std::string& name) const {
        return m_loops.find(name) != m_loops.end();
    }
    [[nodiscard]] const std::vector<VertexId>* find(const std::string& name) const {
        const auto it = m_loops.find(name);
        return it == m_loops.end() ? nullptr : &it->second;
    }
    void remove(const std::string& name) { m_loops.erase(name); }
    [[nodiscard]] std::size_t count() const { return m_loops.size(); }

    // Tagged names in sorted (deterministic) order.
    [[nodiscard]] std::vector<std::string> names() const {
        std::vector<std::string> out;
        out.reserve(m_loops.size());
        for (const auto& entry : m_loops) {
            out.push_back(entry.first);
        }
        return out;
    }

    // Pins every vertex of a named loop into `pins`; no-op if the name is
    // unknown. Reuses PinSet::pinLoop.
    void pinInto(const std::string& name, PinSet& pins) const {
        if (const std::vector<VertexId>* loop = find(name)) {
            pins.pinLoop(*loop);
        }
    }

private:
    std::map<std::string, std::vector<VertexId>> m_loops;
};

}  // namespace cyber::retopo
