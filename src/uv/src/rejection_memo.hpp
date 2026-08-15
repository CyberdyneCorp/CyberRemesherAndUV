#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace cyber::uv::detail {

// Remembers the chart pairs the merge fixpoint's `accept` predicate turned down.
// `accept` is a pure function of the ORDERED pair of face sets, so a rejected
// pair stays rejected until one of the two charts grows — yet the fixpoint
// revisits every surviving pair on every round, and for the distortion pass each
// visit costs a full trial LSCM unwrap of the union. Each rejection is stamped
// with the two charts' versions, which are bumped only when a chart absorbs
// another, so the stamp expires exactly when the answer could change. Skipping a
// stamped pair therefore changes no decision and no ordering: only calls whose
// answer is already known are skipped.
//
// The order matters and the key must keep it. mergeByDistortion unwraps the
// concatenation `a ++ b`, so the LSCM system — and with it the measured
// distortion — differs between (a,b) and (b,a). A key folded to min/max would
// answer one order with the other order's rejection and suppress a merge that is
// legal as requested, silently changing the chart partition. The stamp is folded,
// because chart versions genuinely are symmetric.
class RejectionMemo {
public:
    explicit RejectionMemo(std::size_t chartCount) : m_version(chartCount, 0) {}

    [[nodiscard]] bool isKnownRejected(int a, int b) const {
        const auto it = m_entries.find(key(a, b));
        return it != m_entries.end() && it->second == stamp(a, b);
    }
    void reject(int a, int b) { m_entries[key(a, b)] = stamp(a, b); }
    void bumpVersion(std::size_t chart) { ++m_version[chart]; }

private:
    static std::uint64_t key(int a, int b) {
        return pack(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b));
    }
    [[nodiscard]] std::uint64_t stamp(int a, int b) const {
        return pack(m_version[static_cast<std::size_t>(std::min(a, b))],
                    m_version[static_cast<std::size_t>(std::max(a, b))]);
    }
    static std::uint64_t pack(std::uint32_t lo, std::uint32_t hi) {
        return (static_cast<std::uint64_t>(lo) << 32) | hi;
    }

    std::vector<std::uint32_t> m_version;
    std::unordered_map<std::uint64_t, std::uint64_t> m_entries;
};

}  // namespace cyber::uv::detail
