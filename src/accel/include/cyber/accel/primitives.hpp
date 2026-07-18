#pragma once

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

#include "cyber/accel/backend.hpp"
#include "cyber/accel/buffer.hpp"
#include "cyber/core/bvh.hpp"
#include "cyber/core/math.hpp"

// Fixed primitive set of the backend abstraction (compute-acceleration spec:
// "a fixed primitive set — parallel map, reduce, scan, sort, BVH build/
// traverse, sparse matrix-vector multiply, closest-point projection, ray
// casting"). These are the CPU reference implementations: they define correct
// results and dispatch their data-parallel loops through IBackend::parallelFor,
// so any backend that overrides parallelFor accelerates them for free. GPU
// backends may additionally override specific primitives (tasks 4.3-4.5);
// parity against these references is enforced in task 4.6.
namespace cyber::accel {

// out[i] = fn(in[i]) — data-parallel map over a typed buffer.
template <class T, class U, class F>
void map(IBackend& backend, const Buffer<T>& in, Buffer<U>& out, F fn) {
    out.resize(in.size());
    const T* src = in.data();
    U* dst = out.data();
    backend.parallelFor(0, in.size(), [src, dst, &fn](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            dst[i] = fn(src[i]);
        }
    });
}

// Fold `in` with an associative, commutative `op` seeded by `init`. Each
// worker reduces its chunk locally, then the partials are combined under a
// lock — order-independent because `op` is commutative.
template <class T, class Op>
T reduce(IBackend& backend, const Buffer<T>& in, T init, Op op) {
    const T* src = in.data();
    T result = init;
    std::mutex mutex;
    backend.parallelFor(0, in.size(), [&](std::size_t lo, std::size_t hi) {
        if (lo >= hi) {
            return;
        }
        T local = src[lo];
        for (std::size_t i = lo + 1; i < hi; ++i) {
            local = op(local, src[i]);
        }
        const std::lock_guard<std::mutex> lock(mutex);
        result = op(result, local);
    });
    return result;
}

// Inclusive prefix scan: out[i] = in[0] op ... op in[i]. Reference form is
// sequential (a correct scan is inherently ordered); GPU backends replace it
// with a work-efficient parallel scan.
template <class T, class Op>
void inclusiveScan(IBackend& /*backend*/, const Buffer<T>& in, Buffer<T>& out, Op op) {
    out.resize(in.size());
    if (in.empty()) {
        return;
    }
    out[0] = in[0];
    for (std::size_t i = 1; i < in.size(); ++i) {
        out[i] = op(out[i - 1], in[i]);
    }
}

// Exclusive prefix scan: out[i] = identity op in[0] op ... op in[i-1].
template <class T, class Op>
void exclusiveScan(IBackend& /*backend*/, const Buffer<T>& in, Buffer<T>& out, T identity, Op op) {
    out.resize(in.size());
    T running = identity;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const T current = in[i];
        out[i] = running;
        running = op(running, current);
    }
}

// In-place sort by `less`. Reference is std::sort; GPU backends use a device
// radix/merge sort.
template <class T, class Less>
void sort(IBackend& /*backend*/, Buffer<T>& buffer, Less less) {
    std::sort(buffer.data(), buffer.data() + buffer.size(), less);
}

// Compressed sparse row matrix, the shape the frame-field and Laplacian
// solvers hand to spmv (task 5.8).
struct SparseMatrix {
    std::size_t rows = 0;
    std::vector<std::size_t> rowStart;  // size rows + 1
    std::vector<std::size_t> colIndex;  // size nnz
    std::vector<float> value;           // size nnz
};

// Sparse matrix-vector product y = A * x, one row per parallel task.
inline void spmv(IBackend& backend, const SparseMatrix& a, const Buffer<float>& x,
                 Buffer<float>& y) {
    y.resize(a.rows);
    const float* xd = x.data();
    float* yd = y.data();
    backend.parallelFor(0, a.rows, [&a, xd, yd](std::size_t lo, std::size_t hi) {
        for (std::size_t row = lo; row < hi; ++row) {
            float sum = 0.0f;
            for (std::size_t k = a.rowStart[row]; k < a.rowStart[row + 1]; ++k) {
                sum += a.value[k] * xd[a.colIndex[k]];
            }
            yd[row] = sum;
        }
    });
}

// Batched closest-point projection: out[i] = closest point on `bvh` to
// queries[i]. Wraps the core BVH's per-query traversal across the backend.
inline void closestPoints(IBackend& backend, const Bvh& bvh, const Buffer<Vec3>& queries,
                          Buffer<Vec3>& out) {
    out.resize(queries.size());
    const Vec3* q = queries.data();
    Vec3* dst = out.data();
    backend.parallelFor(0, queries.size(), [&bvh, q, dst](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            dst[i] = bvh.closestPoint(q[i]).point;
        }
    });
}

// Batched ray cast: out[i] holds the hit for ray i, or nullopt on a miss.
inline void raycast(IBackend& backend, const Bvh& bvh, const Buffer<Vec3>& origins,
                    const Buffer<Vec3>& directions, Buffer<std::optional<Bvh::RayHit>>& out) {
    out.resize(origins.size());
    const Vec3* o = origins.data();
    const Vec3* d = directions.data();
    std::optional<Bvh::RayHit>* dst = out.data();
    backend.parallelFor(0, origins.size(), [&bvh, o, d, dst](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            dst[i] = bvh.raycast(o[i], d[i]);
        }
    });
}

}  // namespace cyber::accel
