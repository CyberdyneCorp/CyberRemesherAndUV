#include "cyber/core/bvh.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

#include "cyber/core/detail/parallel_chunks.hpp"

namespace cyber {

Vec3 closestPointOnTriangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 ap = p - a;
    const float d1 = dot(ab, ap);
    const float d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return a;
    }

    const Vec3 bp = p - b;
    const float d3 = dot(ab, bp);
    const float d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        return b;
    }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        return a + ab * (d1 / (d1 - d3));
    }

    const Vec3 cp = p - c;
    const float d5 = dot(ab, cp);
    const float d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        return c;
    }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        return a + ac * (d2 / (d2 - d6));
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }

    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

namespace {

// Depth of the fixed traversal stacks, and so the depth the build must stay
// within (see Bvh::Builder::kMaxDepth).
constexpr std::size_t kTraversalStack = 64;

// Half the surface area of the box (the constant factor cancels in every ratio
// this is used in). Empty boxes — an axis with hi < lo — report 0.
float surfaceArea(Vec3 lo, Vec3 hi) {
    const Vec3 d = hi - lo;
    if (d.x < 0.0f || d.y < 0.0f || d.z < 0.0f) {
        return 0.0f;
    }
    return d.x * d.y + d.y * d.z + d.z * d.x;
}

// Axis-aligned box accumulator for the build. Default-constructs EMPTY (an
// inverted box), so growing by an empty box is a no-op and its area is 0.
struct Bounds {
    Vec3 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max()};
    Vec3 hi{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max()};

    void grow(Vec3 a, Vec3 b) {
        lo = min(lo, a);
        hi = max(hi, b);
    }
    void grow(const Bounds& other) { grow(other.lo, other.hi); }
    [[nodiscard]] float area() const { return surfaceArea(lo, hi); }
};

float distanceSquaredToBox(Vec3 p, Vec3 lo, Vec3 hi) {
    const float dx = std::fmax(std::fmax(lo.x - p.x, 0.0f), p.x - hi.x);
    const float dy = std::fmax(std::fmax(lo.y - p.y, 0.0f), p.y - hi.y);
    const float dz = std::fmax(std::fmax(lo.z - p.z, 0.0f), p.z - hi.z);
    return dx * dx + dy * dy + dz * dz;
}

bool rayIntersectsBox(Vec3 origin, Vec3 invDir, Vec3 lo, Vec3 hi, float maxT) {
    float tmin = 0.0f, tmax = maxT;
    for (const auto& [o, inv, l, h] :
         {std::tuple{origin.x, invDir.x, lo.x, hi.x}, std::tuple{origin.y, invDir.y, lo.y, hi.y},
          std::tuple{origin.z, invDir.z, lo.z, hi.z}}) {
        const float t1 = (l - o) * inv;
        const float t2 = (h - o) * inv;
        tmin = std::fmax(tmin, std::fmin(t1, t2));
        tmax = std::fmin(tmax, std::fmax(t1, t2));
    }
    return tmin <= tmax;
}

// Lexicographic order on raw coordinates — a total order used only to pick a
// canonical evaluation order for a shared edge, never for geometry.
bool lexLess(Vec3 p, Vec3 q) {
    if (p.x != q.x) {
        return p.x < q.x;
    }
    if (p.y != q.y) {
        return p.y < q.y;
    }
    return p.z < q.z;
}

// Signed volume of (ray, edge p->q) with p and q already translated by the ray
// origin. Evaluated in a canonical vertex order and negated afterwards, so the
// two triangles sharing an edge evaluate the SAME expression on the SAME floats
// and get bitwise identical magnitudes with exactly opposite signs — at most one
// of them can reject a ray crossing their shared edge. Mirrors the accel
// backends' copy (backend_primitives.cpp and the CUDA/OpenCL kernels), which
// the parity tests pin to this one.
float edgeVolume(Vec3 dir, Vec3 p, Vec3 q) {
    const bool swapped = lexLess(q, p);
    const Vec3 lo = swapped ? q : p;
    const Vec3 hi = swapped ? p : q;
    const float volume = dot(dir, cross(lo, hi));
    return swapped ? -volume : volume;
}

// Watertight inside test with the Moller-Trumbore distance; front and back faces
// both hit. The inside test is equivalent to Moller-Trumbore's u/v rejections in
// exact arithmetic, so accepted rays keep the same t bit for bit; only rays
// within rounding distance of an edge change verdict — where the strict u/v
// rejections could make BOTH adjacent triangles miss and a ray leak through a
// closed surface.
std::optional<float> rayTriangle(Vec3 origin, Vec3 dir, Vec3 a, Vec3 b, Vec3 c) {
    constexpr float kEpsilon = 1e-9f;
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 pvec = cross(dir, ac);
    const float det = dot(ab, pvec);
    if (std::fabs(det) < kEpsilon) {
        return std::nullopt;
    }
    const Vec3 oa = a - origin;
    const Vec3 ob = b - origin;
    const Vec3 oc = c - origin;
    const float eab = edgeVolume(dir, oa, ob);
    const float ebc = edgeVolume(dir, ob, oc);
    const float eca = edgeVolume(dir, oc, oa);
    const bool inside = (eab >= 0.0f && ebc >= 0.0f && eca >= 0.0f) ||
                        (eab <= 0.0f && ebc <= 0.0f && eca <= 0.0f);
    if (!inside) {
        return std::nullopt;
    }
    const Vec3 tvec = origin - a;
    const Vec3 qvec = cross(tvec, ab);
    const float t = dot(ac, qvec) * (1.0f / det);
    if (t < 0.0f) {
        return std::nullopt;
    }
    return t;
}

}  // namespace

// Hierarchy construction. Split quality is decided by a binned surface-area
// heuristic rather than the object median: on a REGULAR grid — a subdivision
// surface or a CAD tessellation, the shape of most Target imports — the median
// split repeatedly cuts a slab into two slabs of nearly the same surface area,
// so a subtree's cost stops falling with depth and query cost stops being a
// function of triangle count at all. The SAH picks the plane that actually
// shrinks the children's area, which is what pruning needs.
struct Bvh::Builder {
    // One triangle as the build sees it: its bounds and where it came from. The
    // build permutes THESE (28 bytes) and writes the triangles once at the end,
    // instead of shuffling 40-byte triangles at every level.
    struct Ref {
        Vec3 lo, hi;
        std::uint32_t triangle;
    };
    // A subtree the top-level pass deferred so a worker can build it.
    struct Task {
        std::uint32_t node;  // index into the shared node array
        std::uint32_t begin, end;
        std::uint32_t depth;
    };

    static constexpr std::uint32_t kLeafSize = 4;
    static constexpr std::size_t kBins = 16;
    // The query traversals use fixed-size stacks (kTraversalStack), so no node
    // may sit deeper than that. A SAH split can be arbitrarily unbalanced, so
    // past kMedianDepth the median split takes over — it halves the range, so
    // what remains is logarithmic — and at kMaxDepth whatever is left becomes a
    // leaf. Reaching either needs upwards of a billion triangles.
    static constexpr std::uint32_t kMedianDepth = 32;
    static constexpr std::uint32_t kMaxDepth = 58;
    // Ranges at least this large poll the cancel token before descending, which
    // bounds cancel latency by the build cost of that many triangles.
    static constexpr std::uint32_t kCancelPollSpan = 4096;
    // Below this the worker fan-out costs more than the build it saves.
    static constexpr std::size_t kParallelThreshold = 64 * 1024;
    // Levels built breadth-first before the rest is handed out as whole
    // subtrees: 2^8 = up to 256 of them, enough to keep a wide machine busy
    // without spending more levels on the breadth-first pass than it saves.
    static constexpr std::uint32_t kTaskDepth = 8;

    std::vector<Ref> refs;
    std::vector<Task> tasks;
    const CancelToken* cancel = nullptr;
    std::atomic<bool> aborted{false};

    [[nodiscard]] bool stop() {
        if (aborted.load(std::memory_order_relaxed)) {
            return true;
        }
        if (cancel != nullptr && cancel->isCancelled()) {
            aborted.store(true, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    static float axisOf(Vec3 v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }

    // Both boxes a split needs, in ONE pass over the range: the geometric
    // bounds that become the node, and the spread of the centroids that the
    // split plane is chosen within.
    struct RangeBounds {
        Bounds node;
        Bounds centroids;
    };

    [[nodiscard]] RangeBounds boundsOf(std::uint32_t begin, std::uint32_t end) const {
        RangeBounds out;
        for (std::uint32_t i = begin; i < end; ++i) {
            out.node.grow(refs[i].lo, refs[i].hi);
            const Vec3 mid = centroid(refs[i]);
            out.centroids.grow(mid, mid);
        }
        return out;
    }

    // Widest axis of the centroid spread, or -1 when every centroid coincides.
    static int widestAxis(const Bounds& centroids) {
        const Vec3 extent = centroids.hi - centroids.lo;
        int axis = 0;
        if (extent.y > extent.x) {
            axis = 1;
        }
        if (extent.z > axisOf(extent, axis)) {
            axis = 2;
        }
        return axisOf(extent, axis) > 0.0f ? axis : -1;
    }

    static Vec3 centroid(const Ref& ref) { return (ref.lo + ref.hi) * 0.5f; }

    // Balanced fallback: the object median on `axis`, ties broken by the
    // triangle's original index so the order does not depend on the input.
    std::uint32_t medianSplit(std::uint32_t begin, std::uint32_t end, int axis) {
        const std::uint32_t mid = begin + (end - begin) / 2;
        const int splitAxis = axis >= 0 ? axis : 0;
        std::nth_element(refs.begin() + begin, refs.begin() + mid, refs.begin() + end,
                         [splitAxis](const Ref& x, const Ref& y) {
                             const float cx = axisOf(x.lo, splitAxis) + axisOf(x.hi, splitAxis);
                             const float cy = axisOf(y.lo, splitAxis) + axisOf(y.hi, splitAxis);
                             if (cx != cy) {
                                 return cx < cy;
                             }
                             return x.triangle < y.triangle;
                         });
        return mid;
    }

    // A chosen split plane: everything binned below `rightBin` goes left.
    // axis < 0 means no plane separates the range.
    struct BinSplit {
        int axis = -1;
        std::size_t rightBin = 0;
        double cost = std::numeric_limits<double>::max();
    };

    // Where a centroid axis maps into the bin array. A zero scale (no spread on
    // that axis) sends everything to bin 0, which no plane can separate.
    struct BinAxis {
        float lo = 0.0f;
        float scale = 0.0f;
    };

    static BinAxis binAxis(const Bounds& centroids, int axis) {
        const float lo = axisOf(centroids.lo, axis);
        const float span = axisOf(centroids.hi, axis) - lo;
        return {lo, span > 0.0f ? static_cast<float>(kBins) / span : 0.0f};
    }

    static std::size_t binOf(const Ref& ref, int axis, const BinAxis& map) {
        const auto bin = static_cast<int>((axisOf(centroid(ref), axis) - map.lo) * map.scale);
        return static_cast<std::size_t>(std::clamp(bin, 0, static_cast<int>(kBins) - 1));
    }

    // Best of the kBins - 1 planes on `axis`. Only the axis with the widest
    // centroid spread is binned: sweeping all three costs about twice the build
    // (one binning pass per axis is the build's dominant loop) and measured at
    // most 4% off the resulting tree's SAH cost on the tori and the sample
    // models — not a trade an interactive Target import should take.
    [[nodiscard]] BinSplit bestBinSplit(std::uint32_t begin, std::uint32_t end, int axis,
                                        const RangeBounds& bounds) const {
        BinSplit best;
        const float nodeArea = bounds.node.area();
        const BinAxis map = binAxis(bounds.centroids, axis);
        if (nodeArea <= 0.0f || map.scale <= 0.0f) {
            return best;
        }
        std::array<Bounds, kBins> binBounds{};
        std::array<std::uint32_t, kBins> binCount{};
        for (std::uint32_t i = begin; i < end; ++i) {
            const std::size_t bin = binOf(refs[i], axis, map);
            binBounds[bin].grow(refs[i].lo, refs[i].hi);
            ++binCount[bin];
        }
        // Suffix sweep first, then a prefix sweep that reads it: one pass each
        // over the bins instead of a plane-by-plane rescan.
        std::array<float, kBins> rightArea{};
        std::array<std::uint32_t, kBins> rightCount{};
        Bounds acc;
        std::uint32_t count = 0;
        for (std::size_t b = kBins - 1; b >= 1; --b) {
            acc.grow(binBounds[b]);
            count += binCount[b];
            rightArea[b] = acc.area();
            rightCount[b] = count;
        }
        acc = Bounds{};
        count = 0;
        for (std::size_t b = 0; b + 1 < kBins; ++b) {
            acc.grow(binBounds[b]);
            count += binCount[b];
            const std::uint32_t right = rightCount[b + 1];
            if (count == 0 || right == 0) {
                continue;
            }
            const double cost = 1.0 + (static_cast<double>(acc.area()) * count +
                                       static_cast<double>(rightArea[b + 1]) * right) /
                                          static_cast<double>(nodeArea);
            if (cost < best.cost) {
                best = {axis, b + 1, cost};
            }
        }
        return best;
    }

    // In-place two-pointer partition (own code, so the resulting order does not
    // depend on the standard library's choice of algorithm).
    std::uint32_t partitionByBin(std::uint32_t begin, std::uint32_t end, const BinSplit& split,
                                 const Bounds& centroids) {
        const BinAxis map = binAxis(centroids, split.axis);
        std::uint32_t left = begin;
        std::uint32_t right = end;
        while (left < right) {
            if (binOf(refs[left], split.axis, map) < split.rightBin) {
                ++left;
            } else {
                std::swap(refs[left], refs[--right]);
            }
        }
        return left;
    }

    // Bounds nodes[node], then either makes it a leaf or partitions its refs.
    // Returns the split point, or `end` when the node became a leaf. This is the
    // whole O(range) cost of a node, and it touches nothing outside nodes[node]
    // and refs[begin, end) — so sibling nodes can run it concurrently.
    std::uint32_t splitNode(std::vector<Node>& nodes, std::uint32_t node, std::uint32_t begin,
                            std::uint32_t end, std::uint32_t depth) {
        const RangeBounds bounds = boundsOf(begin, end);
        nodes[node].boundsMin = bounds.node.lo;
        nodes[node].boundsMax = bounds.node.hi;

        const std::uint32_t count = end - begin;
        const bool poll = count >= kCancelPollSpan;
        if (count <= kLeafSize || depth >= kMaxDepth || (poll && stop())) {
            nodes[node].firstTriangle = begin;
            nodes[node].triangleCount = count;
            return end;
        }

        const int axis = widestAxis(bounds.centroids);
        std::uint32_t mid = 0;
        if (axis < 0) {
            mid = medianSplit(begin, end, axis);
        } else {
            const BinSplit split =
                depth >= kMedianDepth ? BinSplit{} : bestBinSplit(begin, end, axis, bounds);
            // The leaf alternative costs one triangle test per triangle; when no
            // plane beats it, the median split keeps the tree balanced instead
            // of parking a fat leaf here.
            mid = split.axis >= 0 && split.cost < static_cast<double>(count)
                      ? partitionByBin(begin, end, split, bounds.centroids)
                      : medianSplit(begin, end, axis);
            if (mid <= begin || mid >= end) {
                mid = medianSplit(begin, end, axis);
            }
        }

        return mid;
    }

    // Allocates the two children of a node that split, in `nodes`, and returns
    // the index of the left one. Serial by contract: it grows the array.
    static std::uint32_t addChildren(std::vector<Node>& nodes, std::uint32_t node) {
        const auto left = static_cast<std::uint32_t>(nodes.size());
        nodes[node].leftChild = left;
        nodes.emplace_back();
        nodes.emplace_back();
        return left;
    }

    // Builds the whole subtree rooted at nodes[node] on this thread.
    void buildSubtree(std::vector<Node>& nodes, std::uint32_t node, std::uint32_t begin,
                      std::uint32_t end, std::uint32_t depth) {
        const std::uint32_t mid = splitNode(nodes, node, begin, end, depth);
        if (mid == end) {
            return;
        }
        const std::uint32_t left = addChildren(nodes, node);
        buildSubtree(nodes, left, begin, mid, depth + 1);
        buildSubtree(nodes, left + 1, mid, end, depth + 1);
    }
};

void Bvh::gatherTriangles(const Mesh& mesh) {
    m_triangles.reserve(mesh.faceCount());
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f)) {
            continue;
        }
        const std::vector<VertexId> verts = mesh.faceVertices(f);
        for (std::size_t i = 2; i < verts.size(); ++i) {
            m_triangles.push_back(
                {mesh.position(verts[0]), mesh.position(verts[i - 1]), mesh.position(verts[i]), f});
        }
    }
}

Bvh::Bvh(const Mesh& mesh) : Bvh(mesh, nullptr, nullptr) {}

Bvh::Bvh(const Mesh& mesh, ProgressSink* progress, const CancelToken* cancel) {
    gatherTriangles(mesh);
    if (m_triangles.empty() || (cancel != nullptr && cancel->isCancelled())) {
        m_cancelled = !m_triangles.empty();
        m_triangles.clear();
        return;
    }
    if (progress != nullptr) {
        progress->report(0.05f, "bvh");
    }

    Builder builder;
    builder.cancel = cancel;
    builder.refs.resize(m_triangles.size());
    for (std::size_t i = 0; i < m_triangles.size(); ++i) {
        const Triangle& tri = m_triangles[i];
        Bounds b;
        b.grow(tri.a, tri.a);
        b.grow(tri.b, tri.b);
        b.grow(tri.c, tri.c);
        builder.refs[i] = {b.lo, b.hi, static_cast<std::uint32_t>(i)};
    }

    // The build runs in two parallel phases.
    //
    // Phase 1 walks the top kTaskDepth levels breadth-first: every node of a
    // level is bounded and partitioned CONCURRENTLY (they touch disjoint ref
    // ranges), and the children are then allocated on this thread in level
    // order. The alternative — a serial descent to the same depth — was 65% of
    // the whole build on a 1.7M-triangle Target, because each of those levels
    // sweeps every triangle.
    //
    // Phase 2 hands each surviving range to a worker as one subtree, built into
    // a private node array and folded back in TASK order.
    //
    // kTaskDepth is a constant, NOT a function of the core count, so the
    // hierarchy — and every query answered from it — is the same on every
    // machine and whether or not any worker actually started.
    const bool parallel = builder.refs.size() >= Builder::kParallelThreshold;
    const std::size_t workers =
        parallel ? std::max<std::size_t>(1, std::thread::hardware_concurrency()) : 1;
    const std::uint32_t taskDepth = parallel ? Builder::kTaskDepth : 0;

    m_nodes.reserve(m_triangles.size() * 2);
    m_nodes.emplace_back();
    builder.tasks.push_back({0, 0, static_cast<std::uint32_t>(builder.refs.size()), 0});

    std::vector<std::uint32_t> splits;
    for (std::uint32_t depth = 0; depth < taskDepth && !builder.tasks.empty(); ++depth) {
        const std::vector<Builder::Task> level = builder.tasks;
        splits.assign(level.size(), 0);
        detail::forEachChunk(0, level.size(), workers, [&](std::size_t lo, std::size_t hi) {
            for (std::size_t i = lo; i < hi; ++i) {
                splits[i] = builder.splitNode(m_nodes, level[i].node, level[i].begin, level[i].end,
                                              depth);
            }
        });
        builder.tasks.clear();
        for (std::size_t i = 0; i < level.size(); ++i) {
            if (splits[i] == level[i].end) {
                continue;  // became a leaf
            }
            const std::uint32_t left = Builder::addChildren(m_nodes, level[i].node);
            builder.tasks.push_back({left, level[i].begin, splits[i], depth + 1});
            builder.tasks.push_back({left + 1, splits[i], level[i].end, depth + 1});
        }
        if (progress != nullptr) {
            progress->report(0.05f + 0.10f * static_cast<float>(depth + 1) /
                                         static_cast<float>(taskDepth),
                             "bvh");
        }
    }

    std::vector<std::vector<Node>> subtrees(builder.tasks.size());
    {
        std::atomic<std::size_t> nextTask{0};
        detail::forEachChunk(0, workers, workers, [&](std::size_t, std::size_t) {
            for (std::size_t t = nextTask.fetch_add(1, std::memory_order_relaxed);
                 t < builder.tasks.size(); t = nextTask.fetch_add(1, std::memory_order_relaxed)) {
                const Builder::Task& task = builder.tasks[t];
                subtrees[t].emplace_back();
                builder.buildSubtree(subtrees[t], 0, task.begin, task.end, task.depth);
            }
        });
    }

    if (builder.aborted.load(std::memory_order_relaxed)) {
        m_cancelled = true;
        m_triangles.clear();
        m_nodes.clear();
        return;
    }

    for (std::size_t t = 0; t < subtrees.size(); ++t) {
        const std::vector<Node>& local = subtrees[t];
        // Local index 0 IS the placeholder the top pass already allocated; local
        // 1.. are appended, so a local child index shifts by size() - 1.
        const auto shift = static_cast<std::uint32_t>(m_nodes.size()) - 1;
        auto relink = [shift](Node n) {
            if (!n.isLeaf()) {
                n.leftChild += shift;
            }
            return n;
        };
        m_nodes[builder.tasks[t].node] = relink(local[0]);
        for (std::size_t k = 1; k < local.size(); ++k) {
            m_nodes.push_back(relink(local[k]));
        }
        if (progress != nullptr) {
            progress->report(0.15f + 0.85f * static_cast<float>(t + 1) /
                                         static_cast<float>(subtrees.size()),
                             "bvh");
        }
    }

    // The refs now sit in leaf order; write the triangles to match.
    std::vector<Triangle> ordered;
    ordered.reserve(m_triangles.size());
    for (const Builder::Ref& ref : builder.refs) {
        ordered.push_back(m_triangles[ref.triangle]);
    }
    m_triangles.swap(ordered);
    if (progress != nullptr) {
        progress->report(1.0f, "bvh");
    }
}

double Bvh::sahCost() const {
    if (m_nodes.empty()) {
        return 0.0;
    }
    const double rootArea = surfaceArea(m_nodes[0].boundsMin, m_nodes[0].boundsMax);
    if (rootArea <= 0.0f) {
        return 0.0;
    }
    double cost = 0.0;
    for (const Node& node : m_nodes) {
        const double area = surfaceArea(node.boundsMin, node.boundsMax) / rootArea;
        cost += area * (node.isLeaf() ? static_cast<double>(node.triangleCount) : 1.0);
    }
    return cost;
}

Bvh::ClosestHit Bvh::closestPoint(Vec3 query) const {
    ClosestHit best;
    best.distanceSquared = std::numeric_limits<float>::max();
    if (m_nodes.empty()) {
        return best;
    }
    // Best-first descent: visit the nearer child first and remember the
    // farther one with its box distance, so a tight bound is established
    // immediately and far subtrees are pruned wholesale. Same hits as the
    // naive traversal — pruning only skips subtrees that cannot beat the
    // current best — but orders of magnitude fewer node visits when the query
    // is close to the surface (the isotropic passes' common case).
    struct Entry {
        std::uint32_t node;
        float distanceSquared;
    };
    std::array<Entry, kTraversalStack> stack{};
    std::size_t top = 0;
    stack[top++] = {0, distanceSquaredToBox(query, m_nodes[0].boundsMin, m_nodes[0].boundsMax)};
    while (top > 0) {
        const Entry entry = stack[--top];
        // `>` not `>=`: a subtree sitting exactly at the incumbent distance can
        // still hold a lower-id face at that distance, and the tie-break below
        // only sees candidates the traversal actually visits. Exact equality
        // between a box distance and a surface distance is rare, so this widens
        // the frontier only on the tie-heavy geometry it exists to make stable.
        if (entry.distanceSquared > best.distanceSquared) {
            continue;
        }
        const Node& node = m_nodes[entry.node];
        if (node.isLeaf()) {
            for (std::uint32_t i = 0; i < node.triangleCount; ++i) {
                const Triangle& tri = m_triangles[node.firstTriangle + i];
                const Vec3 p = closestPointOnTriangle(query, tri.a, tri.b, tri.c);
                const float d2 = lengthSquared(p - query);
                // Break an exact tie on the lower face id, never on which
                // triangle the traversal reached first. A query on a shared
                // edge or vertex is equidistant from every incident triangle,
                // and each returns a `p` that can differ in the last bit — so
                // without this the answer is a function of the tree's build
                // order, and any change to the builder silently moves it.
                if (d2 < best.distanceSquared ||
                    (d2 == best.distanceSquared && tri.face.value < best.face.value)) {
                    best = {p, d2, tri.face};
                }
            }
            continue;
        }
        Entry nearChild{node.leftChild,
                        distanceSquaredToBox(query, m_nodes[node.leftChild].boundsMin,
                                             m_nodes[node.leftChild].boundsMax)};
        Entry farChild{node.leftChild + 1,
                       distanceSquaredToBox(query, m_nodes[node.leftChild + 1].boundsMin,
                                            m_nodes[node.leftChild + 1].boundsMax)};
        if (farChild.distanceSquared < nearChild.distanceSquared) {
            std::swap(nearChild, farChild);
        }
        if (top + 2 <= stack.size()) {
            if (farChild.distanceSquared < best.distanceSquared) {
                stack[top++] = farChild;
            }
            if (nearChild.distanceSquared < best.distanceSquared) {
                stack[top++] = nearChild;
            }
        }
    }
    return best;
}

std::optional<Bvh::RayHit> Bvh::raycast(Vec3 origin, Vec3 direction, float maxDistance) const {
    if (m_nodes.empty()) {
        return std::nullopt;
    }
    const Vec3 dir = normalized(direction);
    const Vec3 invDir{1.0f / (dir.x != 0.0f ? dir.x : 1e-30f),
                      1.0f / (dir.y != 0.0f ? dir.y : 1e-30f),
                      1.0f / (dir.z != 0.0f ? dir.z : 1e-30f)};
    std::optional<RayHit> best;
    float bestT = maxDistance;

    std::array<std::uint32_t, kTraversalStack> stack{};
    std::size_t top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const Node& node = m_nodes[stack[--top]];
        if (!rayIntersectsBox(origin, invDir, node.boundsMin, node.boundsMax, bestT)) {
            continue;
        }
        if (node.isLeaf()) {
            for (std::uint32_t i = 0; i < node.triangleCount; ++i) {
                const Triangle& tri = m_triangles[node.firstTriangle + i];
                if (const auto t = rayTriangle(origin, dir, tri.a, tri.b, tri.c)) {
                    if (*t < bestT) {
                        bestT = *t;
                        best = RayHit{origin + dir * *t, *t, tri.face};
                    }
                }
            }
            continue;
        }
        if (top + 2 <= stack.size()) {
            stack[top++] = node.leftChild;
            stack[top++] = node.leftChild + 1;
        }
    }
    return best;
}

FlatBvh Bvh::flatten() const {
    // Process-wide, never reused: consumers key device residency on it, so two
    // snapshots must never share a serial even when the allocator hands the
    // second one the first one's freed storage. Starts at 1 — 0 means
    // "not produced by flatten()".
    static std::atomic<std::uint64_t> nextSerial{1};

    FlatBvh flat;
    flat.serial = nextSerial.fetch_add(1, std::memory_order_relaxed);
    flat.nodes.reserve(m_nodes.size());
    for (const Node& node : m_nodes) {
        FlatBvhNode out{};
        out.boundsMin[0] = node.boundsMin.x;
        out.boundsMin[1] = node.boundsMin.y;
        out.boundsMin[2] = node.boundsMin.z;
        out.boundsMax[0] = node.boundsMax.x;
        out.boundsMax[1] = node.boundsMax.y;
        out.boundsMax[2] = node.boundsMax.z;
        if (node.isLeaf()) {
            out.leftFirst = node.firstTriangle;
            out.triCount = node.triangleCount;
        } else {
            out.leftFirst = node.leftChild;  // children at leftChild and leftChild + 1
            out.triCount = 0;
        }
        flat.nodes.push_back(out);
    }
    flat.tris.reserve(m_triangles.size());
    for (const Triangle& tri : m_triangles) {
        FlatBvhTri out{};
        out.a[0] = tri.a.x;
        out.a[1] = tri.a.y;
        out.a[2] = tri.a.z;
        out.b[0] = tri.b.x;
        out.b[1] = tri.b.y;
        out.b[2] = tri.b.z;
        out.c[0] = tri.c.x;
        out.c[1] = tri.c.y;
        out.c[2] = tri.c.z;
        out.face = tri.face.value;
        flat.tris.push_back(out);
    }
    return flat;
}

}  // namespace cyber
