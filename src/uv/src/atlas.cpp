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

// Greedy chart-merge driver: repeatedly merges any adjacent chart pair for which
// `accept(facesA, facesB)` holds, to a fixpoint, then relabels `chartOf`
// compactly. Deterministic — candidate pairs are visited in sorted order.
template <typename AcceptFn>
void greedyMergeCharts(const Mesh& mesh, std::vector<int>& chartOf, AcceptFn accept) {
    int chartCount = 0;
    for (const int c : chartOf) {
        chartCount = std::max(chartCount, c + 1);
    }
    if (chartCount <= 1) {
        return;
    }
    const auto n = static_cast<std::size_t>(chartCount);
    std::vector<int> parent(n);
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
        if (c >= 0) {
            chartFaces[static_cast<std::size_t>(c)].push_back(face);
        }
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

    bool changed = true;
    while (changed) {
        changed = false;
        for (const std::pair<int, int>& pr : pairs) {
            const int ra = findRoot(pr.first);
            const int rb = findRoot(pr.second);
            if (ra == rb) {
                continue;
            }
            if (!accept(chartFaces[static_cast<std::size_t>(ra)],
                        chartFaces[static_cast<std::size_t>(rb)])) {
                continue;
            }
            const std::size_t keep = static_cast<std::size_t>(std::min(ra, rb));
            const std::size_t drop = static_cast<std::size_t>(std::max(ra, rb));
            parent[drop] = static_cast<int>(keep);
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

// Merges adjacent charts whose union still fits within a `maxChartAngleDeg`
// normal cone. Because every face of a merged chart stays within one cone, the
// result is at least as flat as growth guarantees (distortion cannot rise) and
// stays disk-like (a cone under 90 degrees cannot wrap a tube).
void mergeCoplanarCharts(const Mesh& mesh, std::vector<int>& chartOf,
                         const AtlasOptions& options) {
    const float cosBound = std::cos(degreesToRadians(options.maxChartAngleDeg));
    greedyMergeCharts(mesh, chartOf,
                      [&](const std::vector<FaceId>& a, const std::vector<FaceId>& b) {
                          Vec3 sum{0.0f, 0.0f, 0.0f};
                          for (const FaceId face : a) {
                              sum = sum + normalized(mesh.faceNormal(face));
                          }
                          for (const FaceId face : b) {
                              sum = sum + normalized(mesh.faceNormal(face));
                          }
                          const Vec3 axis = normalized(sum);
                          for (const FaceId face : a) {
                              if (dot(normalized(mesh.faceNormal(face)), axis) < cosBound) {
                                  return false;
                              }
                          }
                          for (const FaceId face : b) {
                              if (dot(normalized(mesh.faceNormal(face)), axis) < cosBound) {
                                  return false;
                              }
                          }
                          return true;
                      });
}

// Distortion of an unwrap given its per-vertex UVs: the maximum of the per-face
// conformal (angle) error and the chart-wide AREA-scale spread. Conformal error
// alone misses inter-facet stretch — a chart of flat facets (e.g. three cube
// faces) has ~0 angle error per triangle no matter how the facets splay, because
// no triangle straddles a fold. The area-scale spread (how much the tightest and
// loosest faces differ in UV-area-per-surface-area) catches exactly that. Both
// are in [0,1); 0 = isometric. Also reports whether the layout folds. Mirrors
// the per-triangle Jacobian SVD in distortion.hpp measureDistortion.
float unwrapDistortion(const Mesh& mesh, const std::vector<FaceId>& faces,
                       const UnwrapResult& result, bool& folded) {
    std::vector<Vec2> uvOf(mesh.vertexCapacity(), Vec2{0.0f, 0.0f});
    for (std::size_t i = 0; i < result.vertices.size(); ++i) {
        uvOf[static_cast<std::size_t>(result.vertices[i].value)] = result.uv[i];
    }
    float maxAngle = 0.0f;
    float minRatio = std::numeric_limits<float>::max();
    float maxRatio = 0.0f;
    int positive = 0;
    int negative = 0;
    for (const FaceId face : faces) {
        const std::vector<VertexId> verts = mesh.faceVertices(face);
        if (verts.size() < 3) {
            continue;
        }
        const Vec3 p0 = mesh.position(verts[0]);
        const Vec2 q0 = uvOf[static_cast<std::size_t>(verts[0].value)];
        float faceAngle = 0.0f;
        float faceUvArea = 0.0f;    // signed
        float faceSurfArea = 0.0f;  // positive
        std::size_t tris = 0;
        for (std::size_t i = 1; i + 1 < verts.size(); ++i) {
            const Vec3 p1 = mesh.position(verts[i]);
            const Vec3 p2 = mesh.position(verts[i + 1]);
            const Vec2 q1 = uvOf[static_cast<std::size_t>(verts[i].value)];
            const Vec2 q2 = uvOf[static_cast<std::size_t>(verts[i + 1].value)];
            const Vec3 e1 = p1 - p0;
            const Vec3 e2 = p2 - p0;
            const float baseLen = length(e1);
            const Vec3 xAxis = baseLen > 0.0f ? e1 * (1.0f / baseLen) : Vec3{1.0f, 0.0f, 0.0f};
            const Vec3 nrm = normalized(cross(e1, e2));
            const Vec3 yAxis = cross(nrm, xAxis);
            const Vec2 s1v{baseLen, 0.0f};
            const Vec2 s2v{dot(e2, xAxis), dot(e2, yAxis)};
            const Vec2 du = q1 - q0;
            const Vec2 dv = q2 - q0;
            const float uvArea = 0.5f * (du.x * dv.y - dv.x * du.y);
            faceUvArea += uvArea;
            faceSurfArea += 0.5f * (s1v.x * s2v.y - s2v.x * s1v.y);
            if (uvArea > 0.0f) {
                ++positive;
            } else if (uvArea < 0.0f) {
                ++negative;
            }
            const float detP = s1v.x * s2v.y - s2v.x * s1v.y;
            if (std::fabs(detP) > 1e-20f) {
                const float invDet = 1.0f / detP;
                const float ia = s2v.y * invDet, ib = -s2v.x * invDet;
                const float ic = -s1v.y * invDet, id = s1v.x * invDet;
                const float ja = du.x * ia + dv.x * ic;
                const float jb = du.x * ib + dv.x * id;
                const float jc = du.y * ia + dv.y * ic;
                const float jd = du.y * ib + dv.y * id;
                float sv1 = 0.0f, sv2 = 0.0f;
                detail::singularValues2x2(ja, jb, jc, jd, sv1, sv2);
                const float denom = sv1 + sv2;
                faceAngle += denom > 0.0f ? (sv1 - sv2) / denom : 0.0f;
                ++tris;
            }
        }
        maxAngle = std::fmax(maxAngle, tris > 0 ? faceAngle / static_cast<float>(tris) : 0.0f);
        if (faceSurfArea > 0.0f) {
            const float ratio = std::fabs(faceUvArea) / faceSurfArea;
            minRatio = std::fmin(minRatio, ratio);
            maxRatio = std::fmax(maxRatio, ratio);
        }
    }
    folded = positive > 0 && negative > 0;
    const float areaSpread =
        (maxRatio > minRatio && maxRatio > 0.0f) ? (maxRatio - minRatio) / (maxRatio + minRatio)
                                                 : 0.0f;
    return std::fmax(maxAngle, areaSpread);
}

// Second, looser merge pass: merges adjacent charts when LSCM-unwrapping their
// union keeps the combined (conformal + area-spread) distortion at or below
// `maxChartDistortion` (and does not fold). This spends the distortion headroom
// the cone merge leaves on the table to cut the chart count further. A no-op
// when the cap is <= 0.
void mergeByDistortion(const Mesh& mesh, std::vector<int>& chartOf,
                       const AtlasOptions& options) {
    if (options.maxChartDistortion <= 0.0f) {
        return;
    }
    const float cap = options.maxChartDistortion;
    greedyMergeCharts(mesh, chartOf,
                      [&](const std::vector<FaceId>& a, const std::vector<FaceId>& b) {
                          std::vector<FaceId> merged;
                          merged.reserve(a.size() + b.size());
                          merged.insert(merged.end(), a.begin(), a.end());
                          merged.insert(merged.end(), b.begin(), b.end());
                          const UnwrapResult r = lscmUnwrap(mesh, merged);
                          if (!r.ok) {
                              return false;
                          }
                          bool folded = false;
                          return unwrapDistortion(mesh, merged, r, folded) <= cap && !folded;
                      });
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
        mergeCoplanarCharts(mesh, chartOf, options);  // free: no distortion rise
        mergeByDistortion(mesh, chartOf, options);    // spend headroom up to the cap
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
