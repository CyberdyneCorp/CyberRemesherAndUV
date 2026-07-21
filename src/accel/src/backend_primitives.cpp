#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>

#include "cyber/accel/backend.hpp"
#include "cyber/core/bvh.hpp"
#include "cyber/core/math.hpp"

// CPU reference implementations of IBackend's accelerated numeric primitives.
// These live on the base class so every backend inherits a correct version and
// GPU backends only override the ones they accelerate.
namespace cyber::accel {

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

void IBackend::spmvCsr(std::size_t rows, const std::size_t* rowStart, const std::size_t* colIndex,
                       const float* value, const float* x, float* y) {
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

// Moller-Trumbore, front and back faces both hit; returns t >= 0 or -1 on miss.
float rayTriangle(Vec3 origin, Vec3 dir, Vec3 a, Vec3 b, Vec3 c) {
    constexpr float kEpsilon = 1e-9f;
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 pvec = cross(dir, ac);
    const float det = dot(ab, pvec);
    if (std::fabs(det) < kEpsilon) {
        return -1.0f;
    }
    const float invDet = 1.0f / det;
    const Vec3 tvec = origin - a;
    const float u = dot(tvec, pvec) * invDet;
    if (u < 0.0f || u > 1.0f) {
        return -1.0f;
    }
    const Vec3 qvec = cross(tvec, ab);
    const float v = dot(dir, qvec) * invDet;
    if (v < 0.0f || u + v > 1.0f) {
        return -1.0f;
    }
    const float t = dot(ac, qvec) * invDet;
    return t < 0.0f ? -1.0f : t;
}

Vec3 triVertex(const float (&v)[3]) { return {v[0], v[1], v[2]}; }

}  // namespace

void IBackend::closestPointsBvh(const FlatBvhNode* nodes, std::size_t nodeCount,
                                const FlatBvhTri* tris, std::size_t /*triCount*/,
                                const float* queriesXYZ, std::size_t n, float* outXYZ) {
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

void IBackend::raycastBvh(const FlatBvhNode* nodes, std::size_t nodeCount, const FlatBvhTri* tris,
                          std::size_t /*triCount*/, const float* originsXYZ, const float* dirsXYZ,
                          std::size_t n, float* outHitXYZ, int* outFace) {
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
                        const float t = rayTriangle(origin, dir, triVertex(tri.a),
                                                    triVertex(tri.b), triVertex(tri.c));
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
