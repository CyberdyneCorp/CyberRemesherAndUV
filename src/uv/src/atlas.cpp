#include "cyber/uv/atlas.hpp"

#include <cmath>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/uv/common.hpp"
#include "cyber/uv/distortion.hpp"

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

}  // namespace

SeamSet autoSeams(const Mesh& mesh, const AtlasOptions& options) {
    const std::vector<int> chartOf = computeCharts(mesh, options);
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

    const PackResult pack = packIslands(mesh, islands, options.pack);
    result.packedArea = pack.usedArea;
    result.texelDensity = pack.texelDensity;
    result.ok = pack.ok;
    return result;
}

}  // namespace cyber::uv
