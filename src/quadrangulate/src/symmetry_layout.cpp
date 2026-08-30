#include "cyber/quadrangulate/symmetry_layout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace cyber::remesh {
namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

// Which side of the plane a point is on: -1, 0 (on it) or +1.
int sideOf(const Plane& plane, Vec3 p, float tolerance) {
    const float d = signedDistance(plane, p);
    if (std::abs(d) <= tolerance) {
        return 0;
    }
    return d > 0.0f ? 1 : -1;
}

// A quantized key for a position, so a mirrored vertex can be matched back to
// its twin without an O(n^2) search. The grid is coarse relative to float noise
// but fine relative to any real edge length.
std::array<long long, 3> positionKey(Vec3 p, float tolerance) {
    const double q = std::max(static_cast<double>(tolerance), 1e-9);
    return {static_cast<long long>(std::llround(static_cast<double>(p.x) / q)),
            static_cast<long long>(std::llround(static_cast<double>(p.y) / q)),
            static_cast<long long>(std::llround(static_cast<double>(p.z) / q))};
}

// "Which vertex is at this position, within tolerance."
//
// The cell size is the tolerance and the lookup probes the 27 surrounding
// cells, then checks the real distance. Using the tolerance as a cell size
// WITHOUT the neighbourhood probe is the obvious mistake here, and it fails two
// opposite ways at once: two distinct vertices inside one cell collide onto a
// single entry, and a partner sitting just across a cell boundary is missed.
// Either one reports a perfectly symmetric mesh as asymmetric.
class VertexGrid {
public:
    VertexGrid(const Mesh& mesh, float tolerance) : m_quantum(std::max(tolerance, 1e-9f)) {
        for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
            const VertexId vid{v};
            if (mesh.isAlive(vid)) {
                m_cells[positionKey(mesh.position(vid), m_quantum)].push_back(v);
            }
        }
    }

    // The nearest vertex to `p` within `tolerance`, or kInvalidIndex. Ties break
    // on the lower id, so the answer does not depend on iteration order.
    [[nodiscard]] Index nearest(const Mesh& mesh, Vec3 p, float tolerance) const {
        const std::array<long long, 3> centre = positionKey(p, m_quantum);
        Index best = kInvalidIndex;
        float bestDistance = tolerance;
        for (long long dx = -1; dx <= 1; ++dx) {
            for (long long dy = -1; dy <= 1; ++dy) {
                for (long long dz = -1; dz <= 1; ++dz) {
                    const auto it = m_cells.find({centre[0] + dx, centre[1] + dy, centre[2] + dz});
                    if (it == m_cells.end()) {
                        continue;
                    }
                    for (const Index candidate : it->second) {
                        const float d = length(mesh.position(VertexId{candidate}) - p);
                        if (d < bestDistance || (d == bestDistance && candidate < best)) {
                            bestDistance = d;
                            best = candidate;
                        }
                    }
                }
            }
        }
        return best;
    }

private:
    float m_quantum;
    std::map<std::array<long long, 3>, std::vector<Index>> m_cells;
};

}  // namespace

Plane symmetryPlane(const Mesh& mesh, SymmetryAxis axis) {
    Plane plane;
    if (axis == SymmetryAxis::None) {
        plane.normal = Vec3{0.0f, 0.0f, 0.0f};
        return plane;
    }
    Vec3 lo{kInf, kInf, kInf};
    Vec3 hi{-kInf, -kInf, -kInf};
    bool any = false;
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const VertexId vid{v};
        if (!mesh.isAlive(vid)) {
            continue;
        }
        const Vec3 p = mesh.position(vid);
        lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        any = true;
    }
    plane.point = any ? (lo + hi) * 0.5f : Vec3{};
    switch (axis) {
        case SymmetryAxis::X:
            plane.normal = Vec3{1.0f, 0.0f, 0.0f};
            break;
        case SymmetryAxis::Y:
            plane.normal = Vec3{0.0f, 1.0f, 0.0f};
            break;
        case SymmetryAxis::Z:
            plane.normal = Vec3{0.0f, 0.0f, 1.0f};
            break;
        case SymmetryAxis::None:
            break;
    }
    return plane;
}

SymmetrySplit splitAtPlane(const Mesh& mesh, const Plane& plane, bool positiveSide,
                           float tolerance) {
    SymmetrySplit out;
    if (length(plane.normal) < 1e-12f || mesh.faceCapacity() == 0) {
        return out;
    }
    const int keepSign = positiveSide ? 1 : -1;

    // Build the half by re-emitting faces rather than deleting from a copy: a
    // face that crosses the plane contributes only the part on the working side,
    // and that part needs new vertices at the crossings.
    std::vector<Vec3> positions;
    std::vector<std::vector<Index>> faces;
    std::map<std::array<long long, 3>, Index> vertexOf;  // ordered: deterministic ids
    std::vector<char> onPlane;

    const auto emit = [&](Vec3 p, bool center) {
        if (center) {
            p = projectToPlane(plane, p);  // exactly on, not nearly
        }
        const auto key = positionKey(p, tolerance);
        const auto it = vertexOf.find(key);
        if (it != vertexOf.end()) {
            onPlane[it->second] = onPlane[it->second] != 0 || center ? 1 : 0;
            return it->second;
        }
        const auto id = static_cast<Index>(positions.size());
        positions.push_back(p);
        onPlane.push_back(center ? 1 : 0);
        vertexOf.emplace(key, id);
        return id;
    };

    // Sutherland-Hodgman against the half-space: walk the face loop, keep the
    // vertices on the working side, and insert one wherever an edge crosses.
    // Clipping to a half-space is exact whatever shape the face is, which is
    // why this needs no triangulation first.
    const auto clipFace = [&](const std::vector<VertexId>& vs, std::vector<Index>& poly) {
        const std::size_t n = vs.size();
        bool anyKept = false;
        for (std::size_t k = 0; k < n; ++k) {
            const Vec3 a = mesh.position(vs[k]);
            const Vec3 b = mesh.position(vs[(k + 1) % n]);
            const int sa = sideOf(plane, a, tolerance);
            const int sb = sideOf(plane, b, tolerance);
            if (sa == keepSign || sa == 0) {
                poly.push_back(emit(a, sa == 0));
                anyKept = anyKept || sa == keepSign;
            }
            if (sa != 0 && sb != 0 && sa != sb) {  // strictly crossing
                const float da = signedDistance(plane, a);
                const float db = signedDistance(plane, b);
                const float t = std::clamp(da / (da - db), 0.0f, 1.0f);
                poly.push_back(emit(a + (b - a) * t, true));
            }
        }
        // Drop the leftovers a clip produces when a face only touched the plane
        // at one vertex or along one edge.
        poly.erase(std::unique(poly.begin(), poly.end()), poly.end());
        if (poly.size() > 1 && poly.front() == poly.back()) {
            poly.pop_back();
        }
        return anyKept && poly.size() >= 3;
    };

    for (Index f = 0; f < mesh.faceCapacity(); ++f) {
        const FaceId face{f};
        if (!mesh.isAlive(face)) {
            continue;
        }
        const std::vector<VertexId> vs = mesh.faceVertices(face);
        if (vs.size() < 3) {
            continue;
        }
        std::vector<Index> poly;
        if (clipFace(vs, poly)) {
            faces.push_back(std::move(poly));
        }
    }
    if (faces.empty()) {
        return out;
    }
    out.half = Mesh::fromIndexed(positions, faces);
    for (Index v = 0; v < out.half.vertexCapacity() && v < onPlane.size(); ++v) {
        if (onPlane[v] != 0 && out.half.isAlive(VertexId{v})) {
            out.centerline.push_back(VertexId{v});
        }
    }
    out.valid = true;
    return out;
}

BorderSnapReport snapBorderToPlane(Mesh& mesh, const Plane& plane) {
    BorderSnapReport report;
    if (length(plane.normal) < 1e-12f) {
        return report;
    }
    std::vector<char> onBorder(mesh.vertexCapacity(), 0);
    for (Index e = 0; e < mesh.edgeCapacity(); ++e) {
        const EdgeId edge{e};
        if (!mesh.isAlive(edge) || mesh.edgeFaceCount(edge) != 1) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(edge);
        onBorder[a.value] = 1;
        onBorder[b.value] = 1;
    }
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const VertexId vid{v};
        if (!mesh.isAlive(vid) || onBorder[v] == 0) {
            continue;
        }
        const Vec3 p = mesh.position(vid);
        report.maxDistance = std::max(report.maxDistance, std::abs(signedDistance(plane, p)));
        mesh.setPosition(vid, projectToPlane(plane, p));
        ++report.snapped;
    }
    // Snapping can pull a face ENTIRELY into the plane when all of its corners
    // were on the border. Such a face is a membrane, not surface: in the
    // mirrored result it would sit inside the model as an internal wall, and it
    // mirrors onto itself so it cannot be paired. Remove it here, where it is
    // still obviously a snapping artifact, rather than leaving the mirror to
    // discover an unpairable face later.
    std::vector<FaceId> membranes;
    for (Index f = 0; f < mesh.faceCapacity(); ++f) {
        const FaceId face{f};
        if (!mesh.isAlive(face)) {
            continue;
        }
        bool allOnPlane = true;
        for (const VertexId v : mesh.faceVertices(face)) {
            if (std::abs(signedDistance(plane, mesh.position(v))) > 1e-6f) {
                allOnPlane = false;
                break;
            }
        }
        if (allOnPlane) {
            membranes.push_back(face);
        }
    }
    for (const FaceId f : membranes) {
        mesh.removeFace(f);
        ++report.membranesRemoved;
    }
    return report;
}

MirrorReport mirrorAcross(Mesh& mesh, const Plane& plane, float tolerance) {
    MirrorReport report;
    if (length(plane.normal) < 1e-12f || mesh.faceCapacity() == 0) {
        return report;
    }
    // Snap first, so "on the plane" is a decidable property rather than a
    // floating-point coincidence. Everything below depends on it.
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const VertexId vid{v};
        if (!mesh.isAlive(vid)) {
            continue;
        }
        const Vec3 p = mesh.position(vid);
        if (std::abs(signedDistance(plane, p)) <= tolerance) {
            mesh.setPosition(vid, projectToPlane(plane, p));
            ++report.weldedCenterline;
        }
    }

    // A twin for every off-plane vertex; on-plane vertices are their own twin,
    // which is exactly what welds the seam.
    std::unordered_map<Index, VertexId> twin;
    std::vector<VertexId> original;
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const VertexId vid{v};
        if (mesh.isAlive(vid)) {
            original.push_back(vid);
        }
    }
    for (const VertexId vid : original) {
        const Vec3 p = mesh.position(vid);
        if (std::abs(signedDistance(plane, p)) <= tolerance) {
            twin.emplace(vid.value, vid);
            continue;
        }
        const VertexId t = mesh.addVertex(mirrorAcrossPlane(plane, p));
        twin.emplace(vid.value, t);
        ++report.mirroredVertices;
    }

    // Collect the faces before adding any, so the mirrored faces are not
    // themselves mirrored.
    std::vector<std::vector<VertexId>> sourceFaces;
    for (Index f = 0; f < mesh.faceCapacity(); ++f) {
        const FaceId face{f};
        if (mesh.isAlive(face)) {
            sourceFaces.push_back(mesh.faceVertices(face));
        }
    }
    for (const std::vector<VertexId>& vs : sourceFaces) {
        std::vector<VertexId> mirrored;
        mirrored.reserve(vs.size());
        // Reversed winding: a reflection flips orientation, so keeping the order
        // would leave the mirrored half inside-out.
        for (auto it = vs.rbegin(); it != vs.rend(); ++it) {
            const auto found = twin.find(it->value);
            if (found == twin.end()) {
                return report;  // a vertex went missing: refuse rather than half-mirror
            }
            mirrored.push_back(found->second);
        }
        // A face lying entirely ON the plane would mirror onto itself; adding it
        // twice would make every one of its edges non-manifold.
        const bool degenerate =
            std::equal(mirrored.begin(), mirrored.end(), vs.rbegin(), vs.rend());
        if (degenerate) {
            continue;
        }
        if (mesh.addFace(mirrored).valid()) {
            ++report.mirroredFaces;
        }
    }
    report.valid = true;
    return report;
}

bool isTopologicallySymmetric(const Mesh& mesh, const Plane& plane, float tolerance) {
    if (length(plane.normal) < 1e-12f) {
        return false;
    }
    // Spatial hash for "the vertex at this position, within tolerance".
    //
    // The grid cell is the tolerance and the lookup probes the 27 surrounding
    // cells, then checks the real distance. Using the tolerance as the cell size
    // WITHOUT the neighbourhood probe is the obvious mistake here and it fails
    // in two opposite ways at once: two distinct vertices inside one cell
    // collide onto a single entry, and a partner sitting just across a cell
    // boundary is missed. Both report a perfectly symmetric mesh as asymmetric.
    const VertexGrid grid(mesh, tolerance);
    const auto findNear = [&](Vec3 p) { return grid.nearest(mesh, p, tolerance); };

    // Every vertex must have a mirror partner...
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const VertexId vid{v};
        if (!mesh.isAlive(vid)) {
            continue;
        }
        if (findNear(mirrorAcrossPlane(plane, mesh.position(vid))) == kInvalidIndex) {
            return false;
        }
    }
    // ...and every face's mirror image must also be a face. Compared as a
    // sorted vertex set, because the mirror reverses winding.
    std::set<std::vector<Index>> faceSets;
    for (Index f = 0; f < mesh.faceCapacity(); ++f) {
        const FaceId face{f};
        if (!mesh.isAlive(face)) {
            continue;
        }
        std::vector<Index> key;
        for (const VertexId v : mesh.faceVertices(face)) {
            key.push_back(v.value);
        }
        std::sort(key.begin(), key.end());
        faceSets.insert(std::move(key));
    }
    for (Index f = 0; f < mesh.faceCapacity(); ++f) {
        const FaceId face{f};
        if (!mesh.isAlive(face)) {
            continue;
        }
        std::vector<Index> key;
        for (const VertexId v : mesh.faceVertices(face)) {
            const Index partner = findNear(mirrorAcrossPlane(plane, mesh.position(v)));
            if (partner == kInvalidIndex) {
                return false;
            }
            key.push_back(partner);
        }
        std::sort(key.begin(), key.end());
        if (faceSets.find(key) == faceSets.end()) {
            return false;
        }
    }
    return true;
}

}  // namespace cyber::remesh
