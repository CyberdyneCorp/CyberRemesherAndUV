#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

#include "cyber/accel/backend.hpp"
#include "cyber/core/bvh.hpp"
#include "cyber/core/detail/parallel_chunks.hpp"
#include "cyber/core/math.hpp"
#include "cyber/core/threading.hpp"

// CPU reference implementations of IBackend's accelerated numeric primitives.
// These live on the base class so every backend inherits a correct version and
// GPU backends only override the ones they accelerate.
namespace cyber::accel {

namespace {

// Shortest range worth handing to worker threads at all. Spawning and joining
// std::threads costs far more than a loop of a few dozen items — the failure
// mode IBackend::spmvCsr already guards against with its nnz threshold. Kept
// deliberately low: parallelFor cannot know the per-item cost, and a caller
// whose items are expensive must not silently lose its workers. Chunking is
// never observable either way — every primitive built on parallelFor writes only
// the indices it was handed, so a serial run is byte-identical to a threaded
// one.
constexpr std::size_t kMinParallelItems = 64;

// True while this thread is executing a parallelFor body. A parallelFor invoked
// from inside another one runs inline instead of fanning out again: nesting
// would multiply the thread count (the AO bake casts its rays from inside the
// texel loop's own parallelFor). It is armed for the inline path too — a short
// outer range still runs a parallelFor body, and leaving it unarmed there let
// every nested call fan out per item.
thread_local bool tInParallelRegion = false;

class ParallelRegion {
public:
    ParallelRegion() : m_previous(tInParallelRegion) { tInParallelRegion = true; }
    ~ParallelRegion() { tInParallelRegion = m_previous; }
    ParallelRegion(const ParallelRegion&) = delete;
    ParallelRegion& operator=(const ParallelRegion&) = delete;

private:
    bool m_previous;
};

std::size_t workerCount(std::size_t total) {
    if (tInParallelRegion || total < kMinParallelItems) {
        return 1;
    }
    return cyber::workerThreadsFor(total);
}

}  // namespace

void IBackend::parallelFor(std::size_t begin, std::size_t end,
                           const std::function<void(std::size_t, std::size_t)>& fn) {
    if (begin >= end) {
        return;
    }
    const std::size_t workers = workerCount(end - begin);
    // A throw from fn arrives here on the calling thread (forEachChunk captures
    // it and joins first), so the C ABI's handlers still convert it to a
    // CyberStatus instead of the process dying on an unhandled worker exception.
    cyber::detail::forEachChunk(begin, end, workers, [&fn](std::size_t lo, std::size_t hi) {
        const ParallelRegion region;
        fn(lo, hi);
    });
}

void IBackend::axpy(float alpha, const float* x, float* y, std::size_t n) {
    parallelFor(0, n, [alpha, x, y](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            y[i] += alpha * x[i];
        }
    });
}

float IBackend::dot(const float* x, const float* y, std::size_t n) {
    double total = 0.0;  // accumulate in double to keep the reference accurate
    std::mutex mutex;
    parallelFor(0, n, [&](std::size_t lo, std::size_t hi) {
        double local = 0.0;
        for (std::size_t i = lo; i < hi; ++i) {
            local += static_cast<double>(x[i]) * static_cast<double>(y[i]);
        }
        const std::lock_guard<std::mutex> lock(mutex);
        total += local;
    });
    return static_cast<float>(total);
}

void IBackend::spmvCsr(std::size_t rows, std::size_t /*cols*/, const std::size_t* rowStart,
                       const std::size_t* colIndex, const float* value, const float* x, float* y) {
    // spmvCsr runs once per CG iteration; on the small operators the native seamless
    // solve produces (~1e3 rows), spawning+joining ~hardware_concurrency std::threads
    // per call costs far more than the row loop (measured: taskset -c 0 matches the
    // 24-core solve time — the solve is thread-launch-bound, not compute-bound). Run
    // small operators serially: byte-IDENTICAL to the parallel path (each row is
    // independent and the inner accumulation order is unchanged), only faster. Genuinely
    // large operators still thread.
    const std::size_t nnz = rows > 0 ? rowStart[rows] : 0;
    constexpr std::size_t kThreadThreshold = 1u << 16;  // ~65k nonzeros
    if (nnz < kThreadThreshold) {
        for (std::size_t row = 0; row < rows; ++row) {
            float sum = 0.0f;
            for (std::size_t k = rowStart[row]; k < rowStart[row + 1]; ++k) {
                sum += value[k] * x[colIndex[k]];
            }
            y[row] = sum;
        }
        return;
    }
    parallelFor(0, rows, [=](std::size_t lo, std::size_t hi) {
        for (std::size_t row = lo; row < hi; ++row) {
            float sum = 0.0f;
            for (std::size_t k = rowStart[row]; k < rowStart[row + 1]; ++k) {
                sum += value[k] * x[colIndex[k]];
            }
            y[row] = sum;
        }
    });
}

// --- Flat-BVH geometry reference (host stack traversal) -------------------
// These mirror the core Bvh::closestPoint / Bvh::raycast traversal but over the
// device-uploadable flat arrays, so a GPU backend's kernels are held to exactly
// this behaviour by the parity tests.
namespace {

constexpr std::size_t kStackSize = 64;

Vec3 nodeMin(const FlatBvhNode& n) { return {n.boundsMin[0], n.boundsMin[1], n.boundsMin[2]}; }
Vec3 nodeMax(const FlatBvhNode& n) { return {n.boundsMax[0], n.boundsMax[1], n.boundsMax[2]}; }

float boxDistanceSquared(Vec3 p, Vec3 lo, Vec3 hi) {
    const float dx = std::fmax(std::fmax(lo.x - p.x, 0.0f), p.x - hi.x);
    const float dy = std::fmax(std::fmax(lo.y - p.y, 0.0f), p.y - hi.y);
    const float dz = std::fmax(std::fmax(lo.z - p.z, 0.0f), p.z - hi.z);
    return dx * dx + dy * dy + dz * dz;
}

bool rayIntersectsBox(Vec3 origin, Vec3 invDir, Vec3 lo, Vec3 hi, float maxT) {
    float tmin = 0.0f;
    float tmax = maxT;
    const float ax[3] = {origin.x, origin.y, origin.z};
    const float ai[3] = {invDir.x, invDir.y, invDir.z};
    const float al[3] = {lo.x, lo.y, lo.z};
    const float ah[3] = {hi.x, hi.y, hi.z};
    for (int k = 0; k < 3; ++k) {
        const float t1 = (al[k] - ax[k]) * ai[k];
        const float t2 = (ah[k] - ax[k]) * ai[k];
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
// origin: positive when the ray passes the edge on one side, negative on the
// other, zero exactly on it.
//
// Evaluated in a canonical vertex order and negated afterwards, so the two
// triangles sharing an edge evaluate the SAME expression on the SAME floats and
// get bitwise identical magnitudes with exactly opposite signs. That is what
// makes the inside test watertight: at most one of the two can reject a ray
// crossing their shared edge. Evaluating dot(dir, cross(p, q)) and
// dot(dir, cross(q, p)) instead leaves the two rounded independently, and both
// can land marginally on the reject side — the leak that shows up as isolated
// unoccluded texels in an AO bake.
float edgeVolume(Vec3 dir, Vec3 p, Vec3 q) {
    const bool swapped = lexLess(q, p);
    const Vec3 lo = swapped ? q : p;
    const Vec3 hi = swapped ? p : q;
    const float volume = dot(dir, cross(lo, hi));
    return swapped ? -volume : volume;
}

// Watertight inside test (canonical edge volumes) with the Moller-Trumbore
// distance; front and back faces both hit. Returns t >= 0 or -1 on miss. The
// inside test is equivalent to Moller-Trumbore's u/v rejections in exact
// arithmetic, so accepted rays keep the same t bit for bit; only rays within
// rounding distance of an edge change verdict.
float rayTriangle(Vec3 origin, Vec3 dir, Vec3 a, Vec3 b, Vec3 c) {
    constexpr float kEpsilon = 1e-9f;
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 pvec = cross(dir, ac);
    const float det = dot(ab, pvec);
    if (std::fabs(det) < kEpsilon) {
        return -1.0f;
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
        return -1.0f;
    }
    const Vec3 tvec = origin - a;
    const Vec3 qvec = cross(tvec, ab);
    const float t = dot(ac, qvec) * (1.0f / det);
    return t < 0.0f ? -1.0f : t;
}

Vec3 triVertex(const float (&v)[3]) { return {v[0], v[1], v[2]}; }

}  // namespace

void IBackend::closestPointsBvh(const FlatBvh& flat, const float* queriesXYZ, std::size_t n,
                                float* outXYZ) {
    const FlatBvhNode* nodes = flat.nodes.data();
    const std::size_t nodeCount = flat.nodes.size();
    const FlatBvhTri* tris = flat.tris.data();
    parallelFor(0, n, [=](std::size_t lo, std::size_t hi) {
        for (std::size_t qi = lo; qi < hi; ++qi) {
            const Vec3 query{queriesXYZ[qi * 3], queriesXYZ[qi * 3 + 1], queriesXYZ[qi * 3 + 2]};
            Vec3 best{};
            float bestD2 = std::numeric_limits<float>::max();
            std::array<std::uint32_t, kStackSize> stack{};
            std::size_t top = 0;
            if (nodeCount > 0) {
                stack[top++] = 0;
            }
            while (top > 0) {
                const FlatBvhNode& node = nodes[stack[--top]];
                if (boxDistanceSquared(query, nodeMin(node), nodeMax(node)) >= bestD2) {
                    continue;
                }
                if (node.triCount > 0) {
                    for (std::uint32_t i = 0; i < node.triCount; ++i) {
                        const FlatBvhTri& tri = tris[node.leftFirst + i];
                        const Vec3 p = closestPointOnTriangle(query, triVertex(tri.a),
                                                              triVertex(tri.b), triVertex(tri.c));
                        const float d2 = lengthSquared(p - query);
                        if (d2 < bestD2) {
                            bestD2 = d2;
                            best = p;
                        }
                    }
                } else if (top + 2 <= kStackSize) {
                    stack[top++] = node.leftFirst;
                    stack[top++] = node.leftFirst + 1;
                }
            }
            outXYZ[qi * 3] = best.x;
            outXYZ[qi * 3 + 1] = best.y;
            outXYZ[qi * 3 + 2] = best.z;
        }
    });
}

void IBackend::raycastBvh(const FlatBvh& flat, const float* originsXYZ, const float* dirsXYZ,
                          std::size_t n, float* outHitXYZ, int* outFace) {
    const FlatBvhNode* nodes = flat.nodes.data();
    const std::size_t nodeCount = flat.nodes.size();
    const FlatBvhTri* tris = flat.tris.data();
    parallelFor(0, n, [=](std::size_t lo, std::size_t hi) {
        for (std::size_t ri = lo; ri < hi; ++ri) {
            const Vec3 origin{originsXYZ[ri * 3], originsXYZ[ri * 3 + 1], originsXYZ[ri * 3 + 2]};
            const Vec3 dir =
                normalized({dirsXYZ[ri * 3], dirsXYZ[ri * 3 + 1], dirsXYZ[ri * 3 + 2]});
            const Vec3 invDir{1.0f / (dir.x != 0.0f ? dir.x : 1e-30f),
                              1.0f / (dir.y != 0.0f ? dir.y : 1e-30f),
                              1.0f / (dir.z != 0.0f ? dir.z : 1e-30f)};
            float bestT = std::numeric_limits<float>::max();
            int bestFace = -1;
            Vec3 bestPoint{};
            std::array<std::uint32_t, kStackSize> stack{};
            std::size_t top = 0;
            if (nodeCount > 0) {
                stack[top++] = 0;
            }
            while (top > 0) {
                const FlatBvhNode& node = nodes[stack[--top]];
                if (!rayIntersectsBox(origin, invDir, nodeMin(node), nodeMax(node), bestT)) {
                    continue;
                }
                if (node.triCount > 0) {
                    for (std::uint32_t i = 0; i < node.triCount; ++i) {
                        const FlatBvhTri& tri = tris[node.leftFirst + i];
                        const float t = rayTriangle(origin, dir, triVertex(tri.a), triVertex(tri.b),
                                                    triVertex(tri.c));
                        if (t >= 0.0f && t < bestT) {
                            bestT = t;
                            bestFace = static_cast<int>(tri.face);
                            bestPoint = origin + dir * t;
                        }
                    }
                } else if (top + 2 <= kStackSize) {
                    stack[top++] = node.leftFirst;
                    stack[top++] = node.leftFirst + 1;
                }
            }
            outHitXYZ[ri * 3] = bestPoint.x;
            outHitXYZ[ri * 3 + 1] = bestPoint.y;
            outHitXYZ[ri * 3 + 2] = bestPoint.z;
            outFace[ri] = bestFace;
        }
    });
}

}  // namespace cyber::accel
