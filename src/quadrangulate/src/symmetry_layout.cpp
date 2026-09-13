#include "cyber/quadrangulate/symmetry_layout.hpp"

#include "cyber/core/bvh.hpp"

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

struct Bounds {
    Vec3 low{kInf, kInf, kInf};
    Vec3 high{-kInf, -kInf, -kInf};
    std::size_t vertices = 0;
};

Bounds boundsOf(const Mesh& mesh) {
    Bounds bounds;
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const VertexId id{v};
        if (!mesh.isAlive(id)) {
            continue;
        }
        const Vec3 p = mesh.position(id);
        bounds.low = Vec3{std::min(bounds.low.x, p.x), std::min(bounds.low.y, p.y),
                          std::min(bounds.low.z, p.z)};
        bounds.high = Vec3{std::max(bounds.high.x, p.x), std::max(bounds.high.y, p.y),
                           std::max(bounds.high.z, p.z)};
        ++bounds.vertices;
    }
    return bounds;
}

float detectionTolerance(const Mesh& mesh, const Bounds& bounds) {
    const float diagonal = length(bounds.high - bounds.low);
    float edgeTotal = 0.0f;
    std::size_t edges = 0;
    for (Index e = 0; e < mesh.edgeCapacity(); ++e) {
        const EdgeId id{e};
        if (!mesh.isAlive(id)) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(id);
        edgeTotal += length(mesh.position(a) - mesh.position(b));
        ++edges;
    }
    const float scaleTolerance = std::max(diagonal, 1.0f) * 1e-4f;
    const float samplingTolerance = edges == 0 ? scaleTolerance : edgeTotal / edges * 1e-3f;
    return std::max(1e-6f, std::min(scaleTolerance, samplingTolerance));
}

Vec3 reflectedNormal(const Plane& plane, Vec3 normal) {
    const Vec3 unitNormal = normalized(plane.normal);
    return normalized(normal - unitNormal * (2.0f * dot(normal, unitNormal)));
}

void scoreSurface(const Mesh& mesh, const Plane& plane, float tolerance,
                  SymmetryDetectionReport& report) {
    const Bvh surface(mesh);
    if (surface.empty()) {
        return;
    }

    float totalError = 0.0f;
    float totalNormalAgreement = 0.0f;
    std::vector<std::size_t> componentByFace(mesh.faceCapacity(), kInvalidIndex);
    const std::vector<std::vector<FaceId>> components = mesh.islands();
    for (std::size_t component = 0; component < components.size(); ++component) {
        for (const FaceId face : components[component]) {
            componentByFace[face.value] = component;
        }
    }
    const auto* groups = mesh.faceAttributes().find<std::int32_t>("group_id");
    const auto* materials = mesh.faceAttributes().find<std::int32_t>("material_id");
    std::map<std::size_t, std::size_t> reflectedComponents;
    std::set<std::size_t> claimedComponents;
    for (Index f = 0; f < mesh.faceCapacity(); ++f) {
        const FaceId face{f};
        if (!mesh.isAlive(face)) {
            continue;
        }
        const std::vector<VertexId> vertices = mesh.faceVertices(face);
        if (vertices.size() < 3) {
            continue;
        }
        const Vec3 a = mesh.position(vertices.front());
        for (std::size_t i = 1; i + 1 < vertices.size(); ++i) {
            const Vec3 b = mesh.position(vertices[i]);
            const Vec3 c = mesh.position(vertices[i + 1]);
            ++report.sampledSurfacePoints;
            const Vec3 sample = (a + b + c) / 3.0f;
            const Bvh::ClosestHit hit = surface.closestPoint(mirrorAcrossPlane(plane, sample));
            const float error = std::sqrt(std::max(0.0f, hit.distanceSquared));
            if (error > tolerance) {
                ++report.unmatchedSurfacePoints;
                continue;
            }
            ++report.matchedSurfacePoints;
            totalError += error;
            report.maxSurfaceError = std::max(report.maxSurfaceError, error);
            const Vec3 sourceNormal = normalized(cross(b - a, c - a));
            const float agreement = std::clamp(
                dot(reflectedNormal(plane, sourceNormal), mesh.faceNormal(hit.face)), -1.0f, 1.0f);
            totalNormalAgreement += agreement;
            if (agreement >= 0.9f) {
                ++report.normalConsistentSurfacePoints;
            }
            const std::size_t sourceComponent = componentByFace[face.value];
            const std::size_t targetComponent = componentByFace[hit.face.value];
            const auto [mapping, inserted] = reflectedComponents.emplace(sourceComponent, targetComponent);
            if (inserted) {
                if (!claimedComponents.insert(targetComponent).second) {
                    mapping->second = kInvalidIndex;
                }
            }
            if (mapping->second == targetComponent) {
                ++report.componentConsistentSurfacePoints;
            }
            if (groups != nullptr || materials != nullptr) {
                ++report.sampledSemanticSurfacePoints;
                const bool groupMatches = groups == nullptr || (*groups)[face.value] == (*groups)[hit.face.value];
                const bool materialMatches =
                    materials == nullptr || (*materials)[face.value] == (*materials)[hit.face.value];
                if (groupMatches && materialMatches) {
                    ++report.semanticConsistentSurfacePoints;
                }
            }
        }
    }
    if (report.matchedSurfacePoints != 0) {
        report.meanSurfaceError = totalError / static_cast<float>(report.matchedSurfacePoints);
        report.meanNormalAgreement =
            totalNormalAgreement / static_cast<float>(report.matchedSurfacePoints);
    }
}

SymmetryDetectionReport scorePlane(const Mesh& mesh, const Plane& plane, SymmetryAxis axis,
                                   float tolerance) {
    SymmetryDetectionReport report;
    report.axis = axis;
    report.plane = plane;
    report.sampledVertices = mesh.vertexCount();
    report.matchTolerance = tolerance;
    const VertexGrid grid(mesh, tolerance);
    float totalError = 0.0f;
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const VertexId id{v};
        if (!mesh.isAlive(id)) {
            continue;
        }
        const Vec3 reflected = mirrorAcrossPlane(report.plane, mesh.position(id));
        const Index partner = grid.nearest(mesh, reflected, tolerance);
        if (partner == kInvalidIndex) {
            ++report.unmatchedVertices;
            continue;
        }
        const float error = length(mesh.position(VertexId{partner}) - reflected);
        ++report.matchedVertices;
        totalError += error;
        report.maxMatchError = std::max(report.maxMatchError, error);
    }
    if (report.matchedVertices != 0) {
        report.meanMatchError = totalError / static_cast<float>(report.matchedVertices);
    }
    scoreSurface(mesh, report.plane, tolerance, report);
    const float surfaceCoverage = report.sampledSurfacePoints == 0
                                      ? 0.0f
                                      : static_cast<float>(report.matchedSurfacePoints) /
                                            static_cast<float>(report.sampledSurfacePoints);
    const float surfaceAccuracy = tolerance == 0.0f
                                      ? 0.0f
                                      : 1.0f - report.meanSurfaceError / tolerance;
    // Surface agreement decides the advisory result. Vertex correspondence is
    // retained as diagnostics, but making it a gate would reject the common
    // case where two geometric halves use different tessellation densities.
    report.confidence = std::clamp(surfaceCoverage * std::max(0.0f, surfaceAccuracy), 0.0f, 1.0f);
    // Detection stays deliberately strict until corpus calibration establishes
    // an application threshold.  A caller can inspect weaker hypotheses, but
    // no hypothesis changes the mesh.
    report.detected = report.unmatchedSurfacePoints == 0 && report.confidence >= 0.995f;
    return report;
}

Vec3 centroidOf(const Mesh& mesh, const Bounds& bounds) {
    Vec3 centroid{};
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const VertexId id{v};
        if (mesh.isAlive(id)) {
            centroid += mesh.position(id);
        }
    }
    return centroid / static_cast<float>(bounds.vertices);
}

struct PrincipalAxes {
    std::array<Vec3, 3> vectors{};
    std::array<float, 3> values{};
};

PrincipalAxes principalAxes(const Mesh& mesh, const Bounds& bounds) {
    const Vec3 centroid = centroidOf(mesh, bounds);
    float covariance[3][3]{};
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const VertexId id{v};
        if (!mesh.isAlive(id)) {
            continue;
        }
        const Vec3 p = mesh.position(id) - centroid;
        const float values[3] = {p.x, p.y, p.z};
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                covariance[row][column] += values[row] * values[column];
            }
        }
    }
    float vectors[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    for (int iteration = 0; iteration < 12; ++iteration) {
        int row = 0;
        int column = 1;
        for (int candidateRow = 0; candidateRow < 3; ++candidateRow) {
            for (int candidateColumn = candidateRow + 1; candidateColumn < 3; ++candidateColumn) {
                if (std::abs(covariance[candidateRow][candidateColumn]) >
                    std::abs(covariance[row][column])) {
                    row = candidateRow;
                    column = candidateColumn;
                }
            }
        }
        if (std::abs(covariance[row][column]) <= 1e-7f) {
            break;
        }
        const float angle = 0.5f * std::atan2(2.0f * covariance[row][column],
                                              covariance[column][column] - covariance[row][row]);
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        for (int i = 0; i < 3; ++i) {
            const float left = covariance[i][row];
            const float right = covariance[i][column];
            covariance[i][row] = cosine * left - sine * right;
            covariance[i][column] = sine * left + cosine * right;
        }
        for (int i = 0; i < 3; ++i) {
            const float left = covariance[row][i];
            const float right = covariance[column][i];
            covariance[row][i] = cosine * left - sine * right;
            covariance[column][i] = sine * left + cosine * right;
            const float vectorLeft = vectors[i][row];
            const float vectorRight = vectors[i][column];
            vectors[i][row] = cosine * vectorLeft - sine * vectorRight;
            vectors[i][column] = sine * vectorLeft + cosine * vectorRight;
        }
    }
    PrincipalAxes result;
    for (std::size_t i = 0; i < 3; ++i) {
        result.values[i] = covariance[i][i];
        result.vectors[i] = normalized(Vec3{vectors[0][i], vectors[1][i], vectors[2][i]});
    }
    return result;
}

bool duplicatesPlane(const std::vector<Plane>& candidates, const Plane& candidate) {
    for (const Plane& existing : candidates) {
        if (std::abs(dot(normalized(existing.normal), normalized(candidate.normal))) > 0.9999f) {
            return true;
        }
    }
    return false;
}

}  // namespace

SymmetryDetectionReport detectSymmetry(const Mesh& mesh) {
    const Bounds bounds = boundsOf(mesh);
    if (bounds.vertices < 3) {
        return {};
    }
    const float tolerance = detectionTolerance(mesh, bounds);
    std::vector<Plane> planes{symmetryPlane(mesh, SymmetryAxis::X),
                              symmetryPlane(mesh, SymmetryAxis::Y),
                              symmetryPlane(mesh, SymmetryAxis::Z)};
    std::vector<SymmetryAxis> axes{SymmetryAxis::X, SymmetryAxis::Y, SymmetryAxis::Z};
    const PrincipalAxes pca = principalAxes(mesh, bounds);
    const float magnitude = std::max({std::abs(pca.values[0]), std::abs(pca.values[1]),
                                      std::abs(pca.values[2]), 1e-8f});
    for (std::size_t i = 0; i < 3; ++i) {
        bool unique = true;
        for (std::size_t j = 0; j < 3; ++j) {
            unique = unique && (i == j || std::abs(pca.values[i] - pca.values[j]) > magnitude * 1e-4f);
        }
        const Plane candidate{centroidOf(mesh, bounds), pca.vectors[i]};
        if (unique && !duplicatesPlane(planes, candidate)) {
            planes.push_back(candidate);
            axes.push_back(SymmetryAxis::None);
        }
    }
    std::vector<SymmetryDetectionReport> candidates;
    candidates.reserve(planes.size());
    for (std::size_t i = 0; i < planes.size(); ++i) {
        candidates.push_back(scorePlane(mesh, planes[i], axes[i], tolerance));
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.confidence > b.confidence; });
    SymmetryDetectionReport result = candidates.front();
    result.ambiguous =
        candidates[1].detected && std::abs(candidates[1].confidence - result.confidence) < 1e-4f;
    if (result.ambiguous) {
        result.detected = false;
        result.axis = SymmetryAxis::None;
    }
    return result;
}

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
        report.onPlane.push_back(vid);
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

namespace {

// The shared body. `onPlane` has already been decided by the caller and every
// vertex in it has already been projected; everything else gets a twin.
MirrorReport mirrorWithCenterline(Mesh& mesh, const Plane& plane, const std::vector<char>& onPlane,
                                  MirrorReport report) {
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
        if (vid.value < onPlane.size() && onPlane[vid.value] != 0) {
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

// "Exactly on the plane" as a geometric tolerance, scaled to the mesh so it does
// not depend on modelling units. Matches splitAtPlane's own 1e-5f default in
// spirit: it admits floating-point noise from projection and nothing else.
float planeEpsilon(const Mesh& mesh) {
    Vec3 lo{kInf, kInf, kInf};
    Vec3 hi{-kInf, -kInf, -kInf};
    bool any = false;
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const VertexId vid{v};
        if (!mesh.isAlive(vid)) {
            continue;
        }
        const Vec3 p = mesh.position(vid);
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        any = true;
    }
    if (!any) {
        return 1e-5f;
    }
    return std::max(1e-6f, 1e-5f * length(hi - lo));
}

// Mark every vertex within `tolerance` of the plane, projecting it exactly onto
// the plane first so membership is decidable rather than a float coincidence.
std::vector<char> centerlineByDistance(Mesh& mesh, const Plane& plane, float tolerance,
                                       std::size_t& welded) {
    std::vector<char> onPlane(mesh.vertexCapacity(), 0);
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const VertexId vid{v};
        if (!mesh.isAlive(vid)) {
            continue;
        }
        const Vec3 p = mesh.position(vid);
        if (std::abs(signedDistance(plane, p)) <= tolerance) {
            mesh.setPosition(vid, projectToPlane(plane, p));
            onPlane[v] = 1;
            ++welded;
        }
    }
    return onPlane;
}

}  // namespace

MirrorReport mirrorAcross(Mesh& mesh, const Plane& plane, float tolerance) {
    MirrorReport report;
    if (length(plane.normal) < 1e-12f || mesh.faceCapacity() == 0) {
        return report;
    }
    const std::vector<char> onPlane =
        centerlineByDistance(mesh, plane, tolerance, report.weldedCenterline);
    return mirrorWithCenterline(mesh, plane, onPlane, report);
}

MirrorReport mirrorAcross(Mesh& mesh, const Plane& plane, const std::vector<VertexId>& onPlane) {
    MirrorReport report;
    if (length(plane.normal) < 1e-12f || mesh.faceCapacity() == 0) {
        return report;
    }
    std::vector<char> mask(mesh.vertexCapacity(), 0);
    for (const VertexId vid : onPlane) {
        if (vid.value < mask.size() && mesh.isAlive(vid)) {
            mask[vid.value] = 1;
            ++report.weldedCenterline;
        }
    }
    // The caller's set is the vertices it SNAPPED, which is the border. A vertex
    // can also sit on the plane without being on the border — the extraction is
    // free to place one there — and duplicating that one instead of welding it
    // leaves its twin unpaired (measured: 4 unmatched faces on a procedural
    // sphere when the set was used alone).
    //
    // So the membership test stays, but at a GEOMETRIC epsilon rather than the
    // caller's weld tolerance. That distinction is the whole point: a third of
    // an edge is the right radius for MATCHING a vertex to its reflection and
    // far too coarse for deciding what the seam is, and conflating the two is
    // what flattened interior surface onto the midplane.
    const float eps = planeEpsilon(mesh);
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        const VertexId vid{v};
        if (!mesh.isAlive(vid) || mask[v] != 0) {
            continue;
        }
        const Vec3 p = mesh.position(vid);
        if (std::abs(signedDistance(plane, p)) <= eps) {
            mesh.setPosition(vid, projectToPlane(plane, p));
            mask[v] = 1;
            ++report.weldedCenterline;
        }
    }
    return mirrorWithCenterline(mesh, plane, mask, report);
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

// ---------------------------------------------------------------------------
// Forced-symmetry orchestration (shared by the CLI and the C ABI).
// ---------------------------------------------------------------------------

namespace {

// Mean edge length of `mesh`, or 0 when it has no edges.
float meanEdgeLength(const Mesh& mesh) {
    float total = 0.0f;
    std::size_t edges = 0;
    for (Index e = 0; e < mesh.edgeCapacity(); ++e) {
        const EdgeId id{e};
        if (!mesh.isAlive(id)) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(id);
        total += length(mesh.position(b) - mesh.position(a));
        ++edges;
    }
    return edges != 0 ? total / static_cast<float>(edges) : 0.0f;
}

// Recount face/vertex statistics from the mesh. After mirroring, the counts the
// pipeline returned describe the HALF it solved, which is half of what the
// caller is holding.
void recountStats(const Mesh& mesh, Statistics& stats) {
    stats.vertexCount = 0;
    stats.quadCount = 0;
    stats.triangleCount = 0;
    stats.otherPolygonCount = 0;
    for (Index v = 0; v < mesh.vertexCapacity(); ++v) {
        stats.vertexCount += mesh.isAlive(VertexId{v}) ? 1u : 0u;
    }
    for (Index f = 0; f < mesh.faceCapacity(); ++f) {
        const FaceId id{f};
        if (!mesh.isAlive(id)) {
            continue;
        }
        const std::size_t n = mesh.faceSize(id);
        if (n == 4) {
            ++stats.quadCount;
        } else if (n == 3) {
            ++stats.triangleCount;
        } else {
            ++stats.otherPolygonCount;
        }
    }
}

}  // namespace

int symmetricHalfTarget(int wholeTargetQuads) { return std::max(100, wholeTargetQuads / 2); }

PipelineResult remeshSymmetric(const Mesh& input, const Parameters& rawParams, SymmetryAxis axis,
                               SymmetryRunReport* report, ProgressSink* progress,
                               const CancelToken* cancel,
                               const QuadrangulatorFactory& quadrangulator,
                               const QuadrangulatorFactory& fallbackQuadrangulator,
                               const Guidance* guidance, const ResourceLimits* limits) {
    if (axis == SymmetryAxis::None) {
        return remesh(input, rawParams, progress, cancel, quadrangulator, fallbackQuadrangulator,
                      guidance, nullptr, limits);
    }

    const Plane plane = symmetryPlane(input, axis);
    const SymmetrySplit split = splitAtPlane(input, plane, /*positiveSide=*/true);
    if (!split.valid) {
        PipelineResult failed;
        failed.status = RunStatus::Error;
        failed.parameterIssues.push_back(
            {"symmetry", "could not split the input at its midplane", /*fatal=*/true});
        return failed;
    }

    // The whole model is what the caller asked for, so the half is solved for
    // half of it.
    Parameters halfParams = rawParams;
    halfParams.targetQuadCount = symmetricHalfTarget(rawParams.targetQuadCount);
    if (report != nullptr) {
        report->applied = true;
        report->requestedQuads = rawParams.targetQuadCount;
        report->halfQuads = halfParams.targetQuadCount;
    }

    PipelineResult result = remesh(split.half, halfParams, progress, cancel, quadrangulator,
                                   fallbackQuadrangulator, guidance, nullptr, limits);
    if (result.mesh.faceCount() == 0) {
        return result;
    }

    // Weld tolerance from the ACHIEVED edge length: the remesh moves the border
    // by a fraction of an edge, and anything inside that fraction is the
    // centerline rather than real geometry.
    const float edge = meanEdgeLength(result.mesh);
    const float tolerance = edge > 0.0f ? 0.35f * edge : 1e-4f;

    // The remesh's border does not land on the cut — the isoline extraction
    // ends where the isolines end — so it is projected back before mirroring.
    // Safe here because the half was split from the caller's input, so its only
    // border IS the cut; the drift is reported so a wandering remesh stays
    // visible rather than absorbed.
    const BorderSnapReport snap = snapBorderToPlane(result.mesh, plane);
    // The centerline is the set snapBorderToPlane just projected, NOT everything
    // within the weld tolerance. `tolerance` is a third of an edge — the right
    // radius for MATCHING a vertex to its reflection below, and far too coarse
    // for deciding what the seam is. Passing it here flattened interior vertices
    // that merely came near the midplane and welded them instead of mirroring
    // them, which is what left the seam residue.
    const MirrorReport mirror = mirrorAcross(result.mesh, plane, snap.onPlane);
    recountStats(result.mesh, result.stats);
    if (report != nullptr) {
        report->borderSnapped = snap.snapped;
        report->maxBorderDrift = snap.maxDistance;
        report->membranesRemoved = snap.membranesRemoved;
        report->mirroredVertices = mirror.mirroredVertices;
        report->mirroredFaces = mirror.mirroredFaces;
        report->topologicallySymmetric = isTopologicallySymmetric(result.mesh, plane, tolerance);
    }
    return result;
}

}  // namespace cyber::remesh
