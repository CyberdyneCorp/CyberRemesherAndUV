#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/uv/common.hpp"
#include "cyber/uv/seams.hpp"
#include "cyber/uv/transforms.hpp"

// 2D layout tools (uv-editing spec, "2D layout tools"): grid straightening,
// overlap distribution, and island merge/stitch. Partial symmetrization lives
// in symmetrize.hpp.
namespace cyber::uv {

namespace detail {

// Collapses near-equal coordinate values into evenly spaced grid lines and
// returns, for each input, the straightened value. `tolerance` is relative to
// the value range.
inline std::vector<float> straightenAxis(const std::vector<float>& values, float tolerance) {
    std::vector<float> result(values.size(), 0.0f);
    if (values.empty()) {
        return result;
    }
    std::vector<std::size_t> order(values.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) { return values[a] < values[b]; });

    const float lo = values[order.front()];
    const float hi = values[order.back()];
    const float span = hi - lo;
    const float eps = std::fmax(span * tolerance, 1e-6f);

    // Cluster sorted values; each cluster becomes one grid line.
    std::vector<float> clusterCenter;
    std::vector<std::size_t> clusterOf(values.size(), 0);
    float sum = values[order.front()];
    std::size_t clusterStart = 0;
    for (std::size_t k = 1; k <= order.size(); ++k) {
        const bool boundary = k == order.size() || values[order[k]] - values[order[k - 1]] > eps;
        if (boundary) {
            const std::size_t n = k - clusterStart;
            clusterCenter.push_back(sum / static_cast<float>(n));
            for (std::size_t m = clusterStart; m < k; ++m) {
                clusterOf[order[m]] = clusterCenter.size() - 1;
            }
            clusterStart = k;
            if (k < order.size()) {
                sum = values[order[k]];
            }
        } else {
            sum += values[order[k]];
        }
    }

    // Respace clusters evenly across [lo, hi] so the grid is regular.
    const std::size_t lines = clusterCenter.size();
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::size_t c = clusterOf[i];
        result[i] = lines <= 1 ? clusterCenter[c]
                               : lo + span * static_cast<float>(c) / static_cast<float>(lines - 1);
    }
    return result;
}

}  // namespace detail

// Snaps a grid-topology island's UVs onto an axis-aligned regular grid
// (spec scenario "Grid straightening"). Corners whose U (resp. V) values fall
// in the same cluster are aligned to a shared, evenly spaced grid line.
inline void gridStraightenIslandUv(Mesh& mesh, std::span<const FaceId> island,
                                   float tolerance = 0.25f) {
    std::vector<Vec2>* uv = uvColumn(mesh);
    if (uv == nullptr) {
        return;
    }
    std::vector<std::size_t> loops;
    std::vector<float> us, vs;
    for (const FaceId face : island) {
        for (const LoopId loop : mesh.faceLoops(face)) {
            const std::size_t idx = static_cast<std::size_t>(loop.value);
            loops.push_back(idx);
            us.push_back((*uv)[idx].x);
            vs.push_back((*uv)[idx].y);
        }
    }
    const std::vector<float> nu = detail::straightenAxis(us, tolerance);
    const std::vector<float> nv = detail::straightenAxis(vs, tolerance);
    for (std::size_t i = 0; i < loops.size(); ++i) {
        (*uv)[loops[i]] = {nu[i], nv[i]};
    }
}

// Distributes overlapping islands so their bounding boxes no longer overlap,
// preserving each island's size and orientation (spec: double-tap
// distribution). Islands are laid out on shelves next to their original
// centroid region; only translations are applied.
inline void distributeIslandsUv(Mesh& mesh, std::span<const std::vector<FaceId>> islands,
                                float margin = 0.01f) {
    std::vector<Bounds2> boxes;
    boxes.reserve(islands.size());
    float maxWidth = 0.0f;
    double area = 0.0;
    for (const std::vector<FaceId>& island : islands) {
        const Bounds2 box = islandUvBounds(mesh, island);
        boxes.push_back(box);
        if (box.valid()) {
            const Vec2 s = box.size();
            maxWidth = std::fmax(maxWidth, s.x + margin);
            area += static_cast<double>(s.x + margin) * static_cast<double>(s.y + margin);
        }
    }
    const float shelfWidth = std::fmax(maxWidth, static_cast<float>(std::sqrt(area)));

    float cursorX = 0.0f;
    float shelfY = 0.0f;
    float shelfHeight = 0.0f;
    for (std::size_t i = 0; i < islands.size(); ++i) {
        if (!boxes[i].valid()) {
            continue;
        }
        const Vec2 s = boxes[i].size();
        const float w = s.x + margin;
        if (cursorX > 0.0f && cursorX + w > shelfWidth) {
            shelfY += shelfHeight;
            cursorX = 0.0f;
            shelfHeight = 0.0f;
        }
        const Vec2 target = {cursorX + margin * 0.5f, shelfY + margin * 0.5f};
        translateIslandUv(mesh, islands[i], target - boxes[i].mn);
        cursorX += w;
        shelfHeight = std::fmax(shelfHeight, s.y + margin);
    }
}

// Merges two islands by sewing the seams that separate them: the given edges
// are removed from the seam set (so they stop cutting islands) and, across
// each edge, the two adjacent corners at each shared endpoint are welded to
// the average of their UVs so the boundary fits together (spec: island
// merge/stitch by drawing one over another with boundary fitting).
inline void stitchAlongSeams(Mesh& mesh, SeamSet& seams, std::span<const EdgeId> edges) {
    std::vector<Vec2>* uv = uvColumn(mesh);
    for (const EdgeId edge : edges) {
        seams.sew(edge);
        if (uv == nullptr) {
            continue;
        }
        const std::vector<FaceId> faces = mesh.edgeFaces(edge);
        if (faces.size() != 2) {
            continue;
        }
        const auto [va, vb] = mesh.edgeVertices(edge);
        // Weld the two corners at each endpoint to their shared average.
        for (const VertexId endpoint : {va, vb}) {
            Vec2 sum{};
            std::size_t count = 0;
            std::vector<std::size_t> touched;
            for (const FaceId face : faces) {
                for (const LoopId loop : mesh.faceLoops(face)) {
                    if (mesh.loopVertex(loop) == endpoint) {
                        const std::size_t idx = static_cast<std::size_t>(loop.value);
                        sum = sum + (*uv)[idx];
                        touched.push_back(idx);
                        ++count;
                    }
                }
            }
            if (count == 0) {
                continue;
            }
            const Vec2 avg = sum * (1.0f / static_cast<float>(count));
            for (const std::size_t idx : touched) {
                (*uv)[idx] = avg;
            }
        }
    }
}

}  // namespace cyber::uv
