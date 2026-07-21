#include "cyber/uv/atlas.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/uv/common.hpp"
#include "cyber/uv/distortion.hpp"
#include "cyber/uv/transforms.hpp"

namespace cyber::uv {

namespace {

// Chart index per face (-1 = unassigned / dead). Faces are grown greedily from
// ascending-id seeds: a neighbour joins the seed's chart while its normal stays
// within `maxChartAngleDeg` of the seed normal. Deterministic (seed order and
// the DFS stack are both id-ordered by construction).
std::vector<int> computeCharts(const Mesh& mesh, const AtlasOptions& options) {
    const auto capacity = mesh.faceCapacity();
    std::vector<int> chartOf(capacity, -1);
    const float cosThresh = std::cos(degreesToRadians(options.maxChartAngleDeg));

    int nextChart = 0;
    for (Index start = 0; start < capacity; ++start) {
        const FaceId seed{start};
        if (!mesh.isAlive(seed) || chartOf[static_cast<std::size_t>(start)] != -1) {
            continue;
        }
        const Vec3 seedNormal = normalized(mesh.faceNormal(seed));
        const int chart = nextChart++;
        chartOf[static_cast<std::size_t>(start)] = chart;

        std::vector<FaceId> stack{seed};
        while (!stack.empty()) {
            const FaceId face = stack.back();
            stack.pop_back();

            const std::vector<VertexId> verts = mesh.faceVertices(face);
            const std::size_t n = verts.size();
            for (std::size_t i = 0; i < n; ++i) {
                const EdgeId edge = mesh.edgeBetween(verts[i], verts[(i + 1) % n]);
                if (!edge.valid()) {
                    continue;
                }
                for (const FaceId nb : mesh.edgeFaces(edge)) {
                    if (nb == face || chartOf[static_cast<std::size_t>(nb.value)] != -1) {
                        continue;
                    }
                    if (dot(normalized(mesh.faceNormal(nb)), seedNormal) >= cosThresh) {
                        chartOf[static_cast<std::size_t>(nb.value)] = chart;
                        stack.push_back(nb);
                    }
                }
            }
        }
    }
    return chartOf;
}

// Merges adjacent charts whose union still fits within a `maxChartAngleDeg`
// normal cone, then relabels `chartOf` compactly. Greedy seed growth splits a
// bumpy surface into many small charts that share a compatible orientation;
// this recombines them. Because every face of a merged chart stays within one
// cone, the result is at least as flat as growth guarantees (distortion cannot
// rise) and stays disk-like (a cone under 90-degrees cannot wrap a tube).
// Deterministic: candidate pairs are processed in sorted order to a fixpoint.
void mergeCoplanarCharts(const Mesh& mesh, std::vector<int>& chartOf,
                         const AtlasOptions& options) {
    int chartCount = 0;
    for (const int c : chartOf) {
        chartCount = std::max(chartCount, c + 1);
    }
    if (chartCount <= 1) {
        return;
    }
    const auto n = static_cast<std::size_t>(chartCount);
    const float cosBound = std::cos(degreesToRadians(options.maxChartAngleDeg));

    std::vector<int> parent(n);
    std::vector<Vec3> normalSum(n, Vec3{0.0f, 0.0f, 0.0f});
    std::vector<std::vector<FaceId>> chartFaces(n);
    for (std::size_t i = 0; i < n; ++i) {
        parent[i] = static_cast<int>(i);
    }

    const auto capacity = mesh.faceCapacity();
    for (Index f = 0; f < capacity; ++f) {
        const FaceId face{f};
        if (!mesh.isAlive(face)) {
            continue;
        }
        const int c = chartOf[static_cast<std::size_t>(f)];
        if (c < 0) {
            continue;
        }
        chartFaces[static_cast<std::size_t>(c)].push_back(face);
        normalSum[static_cast<std::size_t>(c)] =
            normalSum[static_cast<std::size_t>(c)] + normalized(mesh.faceNormal(face));
    }

    // Adjacent chart pairs (deduplicated, sorted for determinism).
    std::vector<std::pair<int, int>> pairs;
    for (Index f = 0; f < capacity; ++f) {
        const FaceId face{f};
        if (!mesh.isAlive(face)) {
            continue;
        }
        const int cf = chartOf[static_cast<std::size_t>(f)];
        const std::vector<VertexId> verts = mesh.faceVertices(face);
        const std::size_t vn = verts.size();
        for (std::size_t i = 0; i < vn; ++i) {
            const EdgeId edge = mesh.edgeBetween(verts[i], verts[(i + 1) % vn]);
            if (!edge.valid()) {
                continue;
            }
            for (const FaceId nb : mesh.edgeFaces(edge)) {
                const int cn = chartOf[static_cast<std::size_t>(nb.value)];
                if (cn != cf) {
                    pairs.emplace_back(std::min(cf, cn), std::max(cf, cn));
                }
            }
        }
    }
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());

    const auto findRoot = [&](int x) {
        while (parent[static_cast<std::size_t>(x)] != x) {
            parent[static_cast<std::size_t>(x)] =
                parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(x)])];
            x = parent[static_cast<std::size_t>(x)];
        }
        return x;
    };
    const auto withinCone = [&](const std::vector<FaceId>& faces, Vec3 axis) {
        for (const FaceId face : faces) {
            if (dot(normalized(mesh.faceNormal(face)), axis) < cosBound) {
                return false;
            }
        }
        return true;
    };

    bool changed = true;
    while (changed) {
        changed = false;
        for (const std::pair<int, int>& pr : pairs) {
            const int ra = findRoot(pr.first);
            const int rb = findRoot(pr.second);
            if (ra == rb) {
                continue;
            }
            const std::size_t ia = static_cast<std::size_t>(ra);
            const std::size_t ib = static_cast<std::size_t>(rb);
            const Vec3 axis = normalized(normalSum[ia] + normalSum[ib]);
            if (!withinCone(chartFaces[ia], axis) || !withinCone(chartFaces[ib], axis)) {
                continue;
            }
            const std::size_t keep = static_cast<std::size_t>(std::min(ra, rb));
            const std::size_t drop = static_cast<std::size_t>(std::max(ra, rb));
            parent[drop] = static_cast<int>(keep);
            normalSum[keep] = normalSum[ia] + normalSum[ib];
            chartFaces[keep].insert(chartFaces[keep].end(), chartFaces[drop].begin(),
                                    chartFaces[drop].end());
            chartFaces[drop].clear();
            changed = true;
        }
    }

    std::vector<int> relabel(n, -1);
    int next = 0;
    for (Index f = 0; f < capacity; ++f) {
        const int c = chartOf[static_cast<std::size_t>(f)];
        if (c < 0) {
            continue;
        }
        const int root = findRoot(c);
        if (relabel[static_cast<std::size_t>(root)] < 0) {
            relabel[static_cast<std::size_t>(root)] = next++;
        }
        chartOf[static_cast<std::size_t>(f)] = relabel[static_cast<std::size_t>(root)];
    }
}

// Fallback UVs for a chart LSCM could not solve: orthographic projection onto
// the plane of the chart's average normal. Always yields a non-degenerate,
// unflipped layout for a roughly-planar chart.
UnwrapResult planarProject(const Mesh& mesh, std::span<const FaceId> island) {
    UnwrapResult out;
    Vec3 normal{0.0f, 0.0f, 0.0f};
    for (const FaceId face : island) {
        normal = normal + mesh.faceNormal(face);
    }
    normal = normalized(normal);
    if (lengthSquared(normal) < 0.5f) {
        normal = {0.0f, 0.0f, 1.0f};
    }
    // A tangent basis (t, b) orthogonal to the plane normal.
    const Vec3 ref = std::fabs(normal.z) < 0.9f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 t = normalized(cross(ref, normal));
    const Vec3 b = cross(normal, t);

    std::vector<bool> seen(mesh.vertexCapacity(), false);
    for (const FaceId face : island) {
        for (const VertexId v : mesh.faceVertices(face)) {
            if (seen[static_cast<std::size_t>(v.value)]) {
                continue;
            }
            seen[static_cast<std::size_t>(v.value)] = true;
            const Vec3 p = mesh.position(v);
            out.vertices.push_back(v);
            out.uv.push_back({dot(p, t), dot(p, b)});
        }
    }
    out.ok = out.vertices.size() >= 3;
    return out;
}

// 2D cross product of (b - a) and (c - a); >0 means a->b->c turns left.
float cross2(Vec2 a, Vec2 b, Vec2 c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Convex hull (Andrew's monotone chain), counter-clockwise, no repeated last
// point. Returns the input as-is when it has fewer than three points.
std::vector<Vec2> convexHull(std::vector<Vec2> pts) {
    std::sort(pts.begin(), pts.end(),
              [](Vec2 a, Vec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    pts.erase(std::unique(pts.begin(), pts.end(),
                          [](Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }),
              pts.end());
    const std::size_t n = pts.size();
    if (n < 3) {
        return pts;
    }
    std::vector<Vec2> hull(2 * n);
    std::size_t k = 0;
    for (std::size_t i = 0; i < n; ++i) {  // lower hull
        while (k >= 2 && cross2(hull[k - 2], hull[k - 1], pts[i]) <= 0.0f) {
            --k;
        }
        hull[k++] = pts[i];
    }
    const std::size_t lower = k + 1;
    for (std::size_t i = n - 1; i-- > 0;) {  // upper hull
        while (k >= lower && cross2(hull[k - 2], hull[k - 1], pts[i]) <= 0.0f) {
            --k;
        }
        hull[k++] = pts[i];
    }
    hull.resize(k - 1);
    return hull;
}

// Rotation (radians, about the centroid) that aligns the chart's minimum-area
// bounding rectangle with the UV axes. The optimal rectangle always shares an
// edge with the convex hull, so it suffices to test every hull-edge direction.
// The longer box side is left horizontal for a consistent, shelf-friendly
// landscape orientation. Returns 0 for a degenerate (sub-triangle) chart.
float minAreaBoxRotation(const std::vector<Vec2>& points) {
    const std::vector<Vec2> hull = convexHull(points);
    if (hull.size() < 3) {
        return 0.0f;
    }
    float bestArea = std::numeric_limits<float>::max();
    float bestAngle = 0.0f;
    float bestW = 0.0f;
    float bestH = 0.0f;
    for (std::size_t i = 0; i < hull.size(); ++i) {
        const Vec2 edge = hull[(i + 1) % hull.size()] - hull[i];
        if (std::fabs(edge.x) < 1e-12f && std::fabs(edge.y) < 1e-12f) {
            continue;
        }
        const float angle = std::atan2(edge.y, edge.x);
        const float c = std::cos(-angle);
        const float s = std::sin(-angle);
        float minX = std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float maxY = std::numeric_limits<float>::lowest();
        for (const Vec2 p : hull) {
            const float x = p.x * c - p.y * s;
            const float y = p.x * s + p.y * c;
            minX = std::fmin(minX, x);
            minY = std::fmin(minY, y);
            maxX = std::fmax(maxX, x);
            maxY = std::fmax(maxY, y);
        }
        const float w = maxX - minX;
        const float h = maxY - minY;
        const float area = w * h;
        if (area < bestArea) {
            bestArea = area;
            bestAngle = angle;
            bestW = w;
            bestH = h;
        }
    }
    // Rotating the chart by -bestAngle makes the optimal box axis-aligned; a
    // further quarter turn puts the longer side horizontal.
    float rotation = -bestAngle;
    if (bestH > bestW) {
        rotation -= kPi * 0.5f;
    }
    return rotation;
}

// Collects the island's corner UVs (with the current parameterization).
std::vector<Vec2> islandUvPoints(const Mesh& mesh, std::span<const FaceId> island) {
    std::vector<Vec2> points;
    const std::vector<Vec2>* uv = uvColumn(mesh);
    if (uv == nullptr) {
        return points;
    }
    for (const FaceId face : island) {
        for (const LoopId loop : mesh.faceLoops(face)) {
            points.push_back((*uv)[static_cast<std::size_t>(loop.value)]);
        }
    }
    return points;
}

}  // namespace

SeamSet autoSeams(const Mesh& mesh, const AtlasOptions& options) {
    std::vector<int> chartOf = computeCharts(mesh, options);
    if (options.mergeCharts) {
        mergeCoplanarCharts(mesh, chartOf, options);
    }
    SeamSet seams;
    const auto capacity = mesh.faceCapacity();
    for (Index f = 0; f < capacity; ++f) {
        const FaceId face{f};
        if (!mesh.isAlive(face)) {
            continue;
        }
        const int chart = chartOf[static_cast<std::size_t>(f)];
        const std::vector<VertexId> verts = mesh.faceVertices(face);
        const std::size_t n = verts.size();
        for (std::size_t i = 0; i < n; ++i) {
            const EdgeId edge = mesh.edgeBetween(verts[i], verts[(i + 1) % n]);
            if (!edge.valid()) {
                continue;
            }
            for (const FaceId nb : mesh.edgeFaces(edge)) {
                if (nb != face && chartOf[static_cast<std::size_t>(nb.value)] != chart) {
                    seams.mark(edge);
                    break;
                }
            }
        }
    }
    return seams;
}

AtlasResult unwrapAtlas(Mesh& mesh, const AtlasOptions& options) {
    AtlasResult result;
    const SeamSet seams = autoSeams(mesh, options);
    result.seamEdges = seams.size();

    const std::vector<std::vector<FaceId>> islands = computeIslands(mesh, seams);
    result.chartCount = static_cast<int>(islands.size());
    if (islands.empty()) {
        return result;
    }

    static_cast<void>(ensureUvColumn(mesh));
    for (const std::vector<FaceId>& island : islands) {
        UnwrapResult unwrap = lscmUnwrap(mesh, island, options.unwrap);
        if (!unwrap.ok) {
            unwrap = planarProject(mesh, island);
            if (unwrap.ok) {
                ++result.fallbackCharts;
            }
        }
        if (unwrap.ok) {
            writeIslandUv(mesh, island, unwrap);
        }
    }

    // Rotate each chart to its minimum-area bounding rectangle. This is a
    // similarity, so it leaves conformal distortion and flips unchanged while
    // giving the axis-aligned shelf packer far less wasted space.
    if (options.reorientCharts) {
        for (const std::vector<FaceId>& island : islands) {
            const float rotation = minAreaBoxRotation(islandUvPoints(mesh, island));
            if (std::fabs(rotation) > 1e-4f) {
                rotateIslandUv(mesh, island, rotation);
            }
        }
    }

    // Measure conformal distortion per chart before packing (packing applies a
    // per-chart similarity transform, which leaves angles unchanged).
    float sumSq = 0.0f;
    int measured = 0;
    for (const std::vector<FaceId>& island : islands) {
        const IslandDistortion d = measureDistortion(mesh, island);
        if (d.faces.empty()) {
            continue;
        }
        ++measured;
        result.maxAngleDistortion = std::fmax(result.maxAngleDistortion, d.maxAngle);
        sumSq += d.rmsAngle * d.rmsAngle;
        if (d.flipped) {
            ++result.flippedCharts;
        }
    }
    if (measured > 0) {
        result.rmsAngleDistortion = std::sqrt(sumSq / static_cast<float>(measured));
    }

    PackParams pack = options.pack;
    pack.strategy = PackStrategy::Skyline;
    const PackResult packResult = packIslands(mesh, islands, pack);
    result.packedArea = packResult.usedArea;
    result.texelDensity = packResult.texelDensity;
    result.ok = packResult.ok;
    return result;
}

}  // namespace cyber::uv
