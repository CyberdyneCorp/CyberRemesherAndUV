#include "cyber/core/interface_conformance.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_set>

namespace cyber::remesh {

namespace {

// Bit pattern, never an epsilon. A tolerance cannot distinguish "never touched"
// from "moved and snapped back to within 1e-6", and that distinction IS the
// guarantee — the whole reason the region path exists rather than a solve-then-
// snap. io_obj.cpp writes at 6 significant digits, so a payload round-trip
// cannot carry this proof either; it must run against the live handle.
bool bitwiseEqual(Vec3 a, Vec3 b) {
    return std::memcmp(&a.x, &b.x, sizeof(float)) == 0 &&
           std::memcmp(&a.y, &b.y, sizeof(float)) == 0 &&
           std::memcmp(&a.z, &b.z, sizeof(float)) == 0;
}

std::pair<Index, Index> ordered(VertexId a, VertexId b) {
    return {std::min(a.value, b.value), std::max(a.value, b.value)};
}

bool edgeExists(const Mesh& mesh, Index a, Index b) {
    const VertexId va{a};
    if (!mesh.isAlive(va)) {
        return false;
    }
    for (const EdgeId e : mesh.vertexEdges(va)) {
        if (!mesh.isAlive(e)) {
            continue;
        }
        const auto [x, y] = mesh.edgeVertices(e);
        if ((x.value == a && y.value == b) || (x.value == b && y.value == a)) {
            return true;
        }
    }
    return false;
}

int activeFaceCount(const Mesh& mesh, const RegionSolve& region, VertexId v) {
    int n = 0;
    for (const FaceId f : mesh.vertexFaces(v)) {
        if (!region.frozen(f)) {
            ++n;
        }
    }
    return n;
}

}  // namespace

std::string ConformanceResult::describeFailure() const {
    if (exact()) {
        return {};
    }
    std::string out = "region solve broke exact landing:";
    if (!movedOrDeadPrescribed.empty()) {
        out += " " + std::to_string(movedOrDeadPrescribed.size()) + " prescribed vertices moved "
               "or died (first id " + std::to_string(movedOrDeadPrescribed.front().value) + ");";
    }
    if (!corruptedFrozenFaces.empty()) {
        out += " " + std::to_string(corruptedFrozenFaces.size()) + " frozen face rings changed "
               "(first id " + std::to_string(corruptedFrozenFaces.front().value) + ");";
    }
    if (!lostInterfaceEdges.empty()) {
        out += " " + std::to_string(lostInterfaceEdges.size()) + " interface edges lost;";
    }
    return out;
}

InterfaceSnapshot captureInterface(const Mesh& mesh, const RegionSolve& region) {
    InterfaceSnapshot snap;
    for (Index i = 0; i < mesh.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (mesh.isAlive(v) && region.pinned(v)) {
            snap.prescribedPositions[i] = mesh.position(v);
        }
    }
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        const FaceId f{i};
        if (!mesh.isAlive(f) || !region.frozen(f)) {
            continue;
        }
        std::vector<Index> ring;
        for (const VertexId v : mesh.faceVertices(f)) {
            ring.push_back(v.value);
        }
        snap.frozenRings[i] = std::move(ring);
    }
    for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
        const EdgeId e{i};
        if (!region.isInterfaceEdge(mesh, e)) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(e);
        snap.interfaceEdges.push_back(ordered(a, b));
    }
    std::sort(snap.interfaceEdges.begin(), snap.interfaceEdges.end());
    return snap;
}

ConformanceResult verifyInterfaceConformance(const Mesh& mesh, const RegionSolve& region,
                                             const InterfaceSnapshot& before) {
    ConformanceResult out;

    // --- REFUSE tier ---------------------------------------------------------
    {
        std::vector<Index> moved;
        for (const auto& [idx, pos] : before.prescribedPositions) {
            const VertexId v{idx};
            if (!mesh.isAlive(v) || !bitwiseEqual(mesh.position(v), pos)) {
                moved.push_back(idx);
            }
        }
        std::sort(moved.begin(), moved.end());
        for (const Index idx : moved) {
            out.movedOrDeadPrescribed.push_back(VertexId{idx});
        }
    }
    {
        std::vector<Index> broken;
        for (const auto& [idx, ring] : before.frozenRings) {
            const FaceId f{idx};
            if (!mesh.isAlive(f)) {
                broken.push_back(idx);
                continue;
            }
            std::vector<Index> now;
            for (const VertexId v : mesh.faceVertices(f)) {
                now.push_back(v.value);
            }
            if (now != ring) {
                broken.push_back(idx);
            }
        }
        std::sort(broken.begin(), broken.end());
        for (const Index idx : broken) {
            out.corruptedFrozenFaces.push_back(FaceId{idx});
        }
    }
    for (const auto& [a, b] : before.interfaceEdges) {
        if (!edgeExists(mesh, a, b)) {
            out.lostInterfaceEdges.push_back({a, b});
        }
    }

    // --- REPORT tier ---------------------------------------------------------
    // Per-vertex interface regularity against the cage-derived prescription.
    {
        std::vector<Index> irregular;
        for (const auto& [idx, required] : region.requiredInRegion) {
            const VertexId v{idx};
            if (!mesh.isAlive(v)) {
                continue;  // already named by the refuse tier
            }
            if (activeFaceCount(mesh, region, v) != required) {
                irregular.push_back(idx);
            }
        }
        std::sort(irregular.begin(), irregular.end());
        for (const Index idx : irregular) {
            out.irregularInterface.push_back(VertexId{idx});
        }
    }

    // Discrete index identity over the SOLVED patch:
    //   sum_interior(4 - deg) + sum_boundary(3 - deg) == 4 * chi
    // A quad disk balances at exactly 4 (re-derived: 4F = 2E_int + E_bnd and
    // E_bnd = V_bnd give 4*V_int + 2*V_bnd - 4F, and Euler closes it). Reported
    // as a residual rather than asserted, since a multi-loop or pinched region
    // legitimately does not balance.
    {
        std::unordered_set<Index> rv, re;
        std::size_t rf = 0;
        for (Index i = 0; i < mesh.faceCapacity(); ++i) {
            const FaceId f{i};
            if (!mesh.isAlive(f) || region.frozen(f)) {
                continue;
            }
            ++rf;
            const std::vector<VertexId> ring = mesh.faceVertices(f);
            bool touchesInterface = false;
            for (std::size_t k = 0; k < ring.size(); ++k) {
                rv.insert(ring[k].value);
                if (region.pinned(ring[k])) {
                    touchesInterface = true;
                }
                const VertexId a = ring[k];
                const VertexId b = ring[(k + 1) % ring.size()];
                for (const EdgeId e : mesh.vertexEdges(a)) {
                    const auto [x, y] = mesh.edgeVertices(e);
                    if ((x.value == a.value && y.value == b.value) ||
                        (x.value == b.value && y.value == a.value)) {
                        re.insert(e.value);
                        break;
                    }
                }
            }
            if (touchesInterface && ring.size() != 4) {
                ++out.interfaceTriangles;
            }
        }
        const int chi =
            static_cast<int>(rv.size()) - static_cast<int>(re.size()) + static_cast<int>(rf);
        int sum = 0;
        for (const Index idx : rv) {
            const VertexId v{idx};
            const int deg = activeFaceCount(mesh, region, v);
            sum += region.pinned(v) ? (3 - deg) : (4 - deg);
        }
        out.indexResidual = sum - 4 * chi;
    }
    return out;
}

}  // namespace cyber::remesh
