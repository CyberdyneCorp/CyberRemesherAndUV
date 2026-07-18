#pragma once

#include <unordered_set>
#include <vector>

#include "cyber/core/mesh.hpp"

// Pinning (manual-retopology spec, "Pinning"): vertices are pinnable
// individually and per edge loop; pinned vertices are immune to Relax and Move
// but still movable by explicit Tweak, and can be cleared en masse.
namespace cyber::retopo {

class PinSet {
public:
    void pin(VertexId v) { m_pinned.insert(v.value); }
    void unpin(VertexId v) { m_pinned.erase(v.value); }

    // Pins every vertex of a loop given as an ordered vertex list.
    void pinLoop(const std::vector<VertexId>& loop) {
        for (const VertexId v : loop) {
            pin(v);
        }
    }

    [[nodiscard]] bool isPinned(VertexId v) const {
        return m_pinned.find(v.value) != m_pinned.end();
    }
    [[nodiscard]] std::size_t count() const { return m_pinned.size(); }

    // Clears all pins en masse.
    void clear() { m_pinned.clear(); }

private:
    std::unordered_set<Index> m_pinned;
};

}  // namespace cyber::retopo
