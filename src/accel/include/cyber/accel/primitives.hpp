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

// Sparse matrix-vector product y = A * x. Dispatches to IBackend::spmvCsr so a
// GPU backend's device kernel runs when one is selected; the CPU reference is
// the base-class implementation.
inline void spmv(IBackend& backend, const SparseMatrix& a, const Buffer<float>& x,
                 Buffer<float>& y) {
    y.resize(a.rows);
    backend.spmvCsr(a.rows, a.rowStart.data(), a.colIndex.data(), a.value.data(), x.data(),
                    y.data());
}

// Batched closest-point projection: out[i] = closest point on `bvh` to
// queries[i]. Flattens the BVH once and dispatches through the backend's
// closestPointsBvh, so a GPU backend runs the traversal on-device; the CPU
// reference produces identical results. Vec3 is three contiguous floats, so the
// query/out buffers upload without a repack.
inline void closestPoints(IBackend& backend, const Bvh& bvh, const Buffer<Vec3>& queries,
                          Buffer<Vec3>& out) {
    out.resize(queries.size());
    const FlatBvh flat = bvh.flatten();
    backend.closestPointsBvh(flat.nodes.data(), flat.nodes.size(), flat.tris.data(),
                             flat.tris.size(), reinterpret_cast<const float*>(queries.data()),
                             queries.size(), reinterpret_cast<float*>(out.data()));
}

// Batched ray cast: out[i] holds the hit for ray i, or nullopt on a miss.
// Dispatches through the backend's raycastBvh and rebuilds the RayHit records
// (t is the distance from the origin to the returned hit point).
inline void raycast(IBackend& backend, const Bvh& bvh, const Buffer<Vec3>& origins,
                    const Buffer<Vec3>& directions, Buffer<std::optional<Bvh::RayHit>>& out) {
    const std::size_t n = origins.size();
    out.resize(n);
    if (n == 0) {
        return;
    }
    const FlatBvh flat = bvh.flatten();
    std::vector<float> hitXYZ(n * 3);
    std::vector<int> faces(n);
    backend.raycastBvh(flat.nodes.data(), flat.nodes.size(), flat.tris.data(), flat.tris.size(),
                       reinterpret_cast<const float*>(origins.data()),
                       reinterpret_cast<const float*>(directions.data()), n, hitXYZ.data(),
                       faces.data());
    for (std::size_t i = 0; i < n; ++i) {
        if (faces[i] < 0) {
            out[i] = std::nullopt;
            continue;
        }
        const Vec3 point{hitXYZ[i * 3], hitXYZ[i * 3 + 1], hitXYZ[i * 3 + 2]};
        const float t = length(point - origins[i]);
        out[i] = Bvh::RayHit{point, t, FaceId{static_cast<Index>(faces[i])}};
    }
}

}  // namespace cyber::accel
