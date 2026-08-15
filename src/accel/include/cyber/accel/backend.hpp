#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cyber/core/bvh.hpp"  // FlatBvhNode / FlatBvhTri (accel depends on core)

namespace cyber::accel {

enum class BackendKind { Cpu, Metal, Cuda, OpenCl };

// Compute backend contract. The CPU backend is always present and defines
// correct results; GPU backends are optional accelerators (see the
// compute-acceleration spec). The primitive set grows with task group 4;
// parallel_for is the seed primitive the isotropic remesher needs first.
class IBackend {
public:
    virtual ~IBackend() = default;

    [[nodiscard]] virtual BackendKind kind() const = 0;
    [[nodiscard]] virtual std::string deviceName() const = 0;

    // Invokes fn(begin..end) partitioned across host worker threads. Blocks
    // until done. The partition is an implementation detail — fn must write only
    // the indices it is handed — and the range runs on the calling thread when
    // it is too short to pay for a fan-out, or when the call is nested inside
    // another parallelFor. Callers therefore have to place the parallelism where
    // the work is: a range of a few dozen cheap items is faster serial than
    // threaded.
    //
    // The base class implements this as a host worker pool and every backend —
    // GPU included — inherits it: fn is an arbitrary host callable that cannot
    // cross to a device, so this is the library's only host loop primitive. A
    // device backend accelerates by overriding the typed kernels below, NEVER by
    // replacing this; each GPU backend that ran the range inline single-threaded
    // the AO texel loop, the solver's per-variable loop and the base-class BVH
    // traversal for the whole process. Overriding is reserved for decorators
    // that delegate (the bake tests observe ranges that way).
    //
    // Consequence for device backends: the typed kernels below are reachable
    // from several host worker threads at once, so they must be thread-safe.
    virtual void parallelFor(std::size_t begin, std::size_t end,
                             const std::function<void(std::size_t, std::size_t)>& fn);

    // Accelerated numeric primitives (compute-acceleration spec "hot spots":
    // solver matrix-vector products and the dense vector ops around them).
    // The base class provides the CPU reference in terms of parallelFor and
    // defines correct results; GPU backends override with device kernels and
    // are held to these by the parity tests (task 4.6). They take host
    // pointers — a GPU backend stages its own device copies — so the call
    // sites never change when a GPU backend is selected.

    // y[i] += alpha * x[i], for i in [0, n).
    virtual void axpy(float alpha, const float* x, float* y, std::size_t n);
    // Sum of x[i] * y[i].
    [[nodiscard]] virtual float dot(const float* x, const float* y, std::size_t n);
    // y = A * x for a CSR matrix A with `rows` rows and `cols` columns
    // (rowStart has rows + 1 entries; colIndex/value have nnz entries; x has
    // `cols` entries and y has `rows`). A is NOT assumed square — the seamless
    // solver's reduction operators are genuinely rectangular — so `cols` is the
    // only bound a backend may use when staging a device copy of x. Every
    // colIndex must be < cols.
    virtual void spmvCsr(std::size_t rows, std::size_t cols, const std::size_t* rowStart,
                         const std::size_t* colIndex, const float* value, const float* x, float* y);

    // Batched geometry queries over a flattened BVH (roadmap 4.6/5.8/11.1), so
    // baking and surface projection run on the GPU. Inputs are host pointers to
    // the flat arrays from Bvh::flatten() plus packed xyz query streams; a GPU
    // backend stages its own device copies, one thread per query doing an
    // iterative fixed-stack traversal. The base class provides the CPU
    // reference (host stack traversal), which defines correct results and is
    // what the parity tests hold GPU overrides to.
    //
    // A device backend MAY keep the last-queried BVH resident across calls,
    // identified by the array addresses, their sizes and a content fingerprint
    // (detail::BvhResidencyKey) — otherwise a per-texel bake re-uploads the
    // whole mesh per texel. Callers must therefore not mutate node/triangle
    // arrays in place between queries; hand over a rebuilt snapshot instead.

    // outXYZ[i] = closest point on the BVH surface to queriesXYZ[i]
    // (queriesXYZ / outXYZ are 3*n contiguous floats). Empty BVH -> {0,0,0}.
    virtual void closestPointsBvh(const FlatBvhNode* nodes, std::size_t nodeCount,
                                  const FlatBvhTri* tris, std::size_t triCount,
                                  const float* queriesXYZ, std::size_t n, float* outXYZ);

    // Casts n rays; outHitXYZ[i] holds the nearest hit point and outFace[i] the
    // owning FaceId::value, or outFace[i] = -1 on a miss (directions need not be
    // normalized; nearest positive t along the ray is kept).
    virtual void raycastBvh(const FlatBvhNode* nodes, std::size_t nodeCount, const FlatBvhTri* tris,
                            std::size_t triCount, const float* originsXYZ, const float* dirsXYZ,
                            std::size_t n, float* outHitXYZ, int* outFace);
};

// Enumerates available backends, best first (Metal/CUDA > OpenCL > CPU).
// The CPU backend is always the last entry. Probing a device is expensive
// (the OpenCL maker compiles its kernel source), so the list is built once and
// the same instances are handed out on every call.
[[nodiscard]] std::vector<std::shared_ptr<IBackend>> availableBackends();

// The GPU backend kinds this build was compiled with, whether or not their
// device is present at runtime. Lets a caller — the parity tests above all —
// tell "no GPU compiled in" apart from "GPU compiled in but not detected",
// which are the same empty result in availableBackends().
[[nodiscard]] std::vector<BackendKind> compiledBackendKinds();

// The process-wide default backend (first of availableBackends unless
// overridden by setDefaultBackend, or by the CYBER_BACKEND environment
// variable — "cpu", "cuda", "metal" or "opencl" — which is the support escape
// hatch for a consumer that cannot rebuild the library).
[[nodiscard]] std::shared_ptr<IBackend> defaultBackend();
void setDefaultBackend(std::shared_ptr<IBackend> backend);

// User selection/override (compute-acceleration spec, "Runtime detection,
// selection, and fallback"): return the first available backend of `kind`,
// or the CPU backend when that kind is absent — never null. Passing the
// result to setDefaultBackend makes it process-wide.
[[nodiscard]] std::shared_ptr<IBackend> selectBackend(BackendKind kind);

}  // namespace cyber::accel
