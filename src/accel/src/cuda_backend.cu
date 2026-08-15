// CUDA compute backend (compute-acceleration spec, tier-1 on NVIDIA).
// makeCudaBackend() returns nullptr when no CUDA device is present, so the
// registry degrades gracefully to CPU. Compiled to PTX (virtual arch) so the
// driver JITs for the actual GPU at load time — one build runs across NVIDIA
// generations, including those newer than the build-time toolkit.
//
// Verified on an RTX 5060 (compute cap 12.0, driver 580) with the CUDA
// toolkit's forward PTX JIT, via tests/accel/test_gpu_parity.cpp built with
// -DCYBER_ENABLE_CUDA=ON.

#include <cuda_runtime.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "cyber/accel/backend.hpp"
#include "cyber/accel/detail/bvh_residency.hpp"

namespace cyber::accel {

namespace {

// A device failure must degrade, never go unnoticed: an unchecked cudaMalloc or
// launch failure returns normally with the caller's output buffer UNTOUCHED and
// leaves the context in a sticky error state, so every later CUDA call in the
// process fails too. Each primitive checks every status, reports once, clears
// the sticky error and completes on the CPU reference (compute-acceleration
// spec: "fall back to CPU automatically when a GPU backend fails at runtime ...
// never crashing or producing partial results").
void reportFallback(const char* primitive, const char* what) {
    static std::once_flag once;
    std::call_once(once, [primitive, what] {
        std::fprintf(stderr, "[accel] CUDA %s failed (%s); falling back to the CPU reference\n",
                     primitive, what);
    });
}

bool cudaOk(cudaError_t status, const char* primitive) {
    if (status == cudaSuccess) {
        return true;
    }
    reportFallback(primitive, cudaGetErrorString(status));
    cudaGetLastError();  // clear the sticky error so later calls can succeed
    return false;
}

__global__ void axpyKernel(float alpha, const float* x, float* y, std::size_t n) {
    const std::size_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        y[i] += alpha * x[i];
    }
}

__global__ void spmvKernel(std::size_t rows, const std::size_t* rowStart,
                           const std::size_t* colIndex, const float* value, const float* x,
                           float* y) {
    const std::size_t row = blockIdx.x * blockDim.x + threadIdx.x;
    if (row >= rows) {
        return;
    }
    float sum = 0.0f;
    for (std::size_t k = rowStart[row]; k < rowStart[row + 1]; ++k) {
        sum += value[k] * x[colIndex[k]];
    }
    y[row] = sum;
}

// --- Flat-BVH geometry kernels (roadmap 4.6/5.8/11.1) --------------------
// One thread per query, iterative fixed-size-stack traversal over the flattened
// core BVH. FlatBvhNode/FlatBvhTri are POD (device-usable) and match the CPU
// reference in backend_primitives.cpp element-for-element within f32 tolerance.

constexpr int kCudaStackSize = 64;

__device__ inline float3 v3(const float* p) { return make_float3(p[0], p[1], p[2]); }
__device__ inline float3 operator-(float3 a, float3 b) {
    return make_float3(a.x - b.x, a.y - b.y, a.z - b.z);
}
__device__ inline float3 operator+(float3 a, float3 b) {
    return make_float3(a.x + b.x, a.y + b.y, a.z + b.z);
}
__device__ inline float3 operator*(float3 a, float s) {
    return make_float3(a.x * s, a.y * s, a.z * s);
}
__device__ inline float d3(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
__device__ inline float3 cross3(float3 a, float3 b) {
    return make_float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

// Ericson closest point on triangle, ported verbatim from the core routine.
__device__ inline float3 closestOnTri(float3 p, float3 a, float3 b, float3 c) {
    const float3 ab = b - a;
    const float3 ac = c - a;
    const float3 ap = p - a;
    const float d1 = d3(ab, ap);
    const float d2 = d3(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return a;
    }
    const float3 bp = p - b;
    const float d3v = d3(ab, bp);
    const float d4 = d3(ac, bp);
    if (d3v >= 0.0f && d4 <= d3v) {
        return b;
    }
    const float vc = d1 * d4 - d3v * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3v <= 0.0f) {
        return a + ab * (d1 / (d1 - d3v));
    }
    const float3 cp = p - c;
    const float d5 = d3(ab, cp);
    const float d6 = d3(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        return c;
    }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        return a + ac * (d2 / (d2 - d6));
    }
    const float va = d3v * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3v) >= 0.0f && (d5 - d6) >= 0.0f) {
        return b + (c - b) * ((d4 - d3v) / ((d4 - d3v) + (d5 - d6)));
    }
    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

__device__ inline float boxDistSq(float3 p, float3 lo, float3 hi) {
    const float dx = fmaxf(fmaxf(lo.x - p.x, 0.0f), p.x - hi.x);
    const float dy = fmaxf(fmaxf(lo.y - p.y, 0.0f), p.y - hi.y);
    const float dz = fmaxf(fmaxf(lo.z - p.z, 0.0f), p.z - hi.z);
    return dx * dx + dy * dy + dz * dz;
}

__device__ inline bool rayBox(float3 o, float3 inv, float3 lo, float3 hi, float maxT) {
    float tmin = 0.0f;
    float tmax = maxT;
    const float t1x = (lo.x - o.x) * inv.x, t2x = (hi.x - o.x) * inv.x;
    tmin = fmaxf(tmin, fminf(t1x, t2x));
    tmax = fminf(tmax, fmaxf(t1x, t2x));
    const float t1y = (lo.y - o.y) * inv.y, t2y = (hi.y - o.y) * inv.y;
    tmin = fmaxf(tmin, fminf(t1y, t2y));
    tmax = fminf(tmax, fmaxf(t1y, t2y));
    const float t1z = (lo.z - o.z) * inv.z, t2z = (hi.z - o.z) * inv.z;
    tmin = fmaxf(tmin, fminf(t1z, t2z));
    tmax = fminf(tmax, fmaxf(t1z, t2z));
    return tmin <= tmax;
}

// Watertight edge test, mirroring the CPU reference: evaluated in a canonical
// vertex order so the two triangles sharing an edge see bitwise opposite values
// and at most one of them can reject a ray crossing it.
__device__ inline bool lexLess3(float3 p, float3 q) {
    if (p.x != q.x) {
        return p.x < q.x;
    }
    if (p.y != q.y) {
        return p.y < q.y;
    }
    return p.z < q.z;
}

__device__ inline float edgeVolume(float3 dir, float3 p, float3 q) {
    const bool swapped = lexLess3(q, p);
    const float3 lo = swapped ? q : p;
    const float3 hi = swapped ? p : q;
    const float volume = d3(dir, cross3(lo, hi));
    return swapped ? -volume : volume;
}

__device__ inline float rayTri(float3 o, float3 dir, float3 a, float3 b, float3 c) {
    const float kEpsilon = 1e-9f;
    const float3 ab = b - a;
    const float3 ac = c - a;
    const float3 pvec = cross3(dir, ac);
    const float det = d3(ab, pvec);
    if (fabsf(det) < kEpsilon) {
        return -1.0f;
    }
    const float3 oa = a - o;
    const float3 ob = b - o;
    const float3 oc = c - o;
    const float eab = edgeVolume(dir, oa, ob);
    const float ebc = edgeVolume(dir, ob, oc);
    const float eca = edgeVolume(dir, oc, oa);
    const bool inside = (eab >= 0.0f && ebc >= 0.0f && eca >= 0.0f) ||
                        (eab <= 0.0f && ebc <= 0.0f && eca <= 0.0f);
    if (!inside) {
        return -1.0f;
    }
    const float3 tvec = o - a;
    const float3 qvec = cross3(tvec, ab);
    const float t = d3(ac, qvec) * (1.0f / det);
    return t < 0.0f ? -1.0f : t;
}

__global__ void closestKernel(const FlatBvhNode* nodes, const FlatBvhTri* tris,
                              const float* queries, std::size_t n, float* out) {
    const std::size_t qi = blockIdx.x * blockDim.x + threadIdx.x;
    if (qi >= n) {
        return;
    }
    const float3 q = v3(queries + qi * 3);
    float3 best = make_float3(0.0f, 0.0f, 0.0f);
    float bestD2 = 3.4e38f;
    unsigned stack[kCudaStackSize];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const FlatBvhNode node = nodes[stack[--top]];
        if (boxDistSq(q, v3(node.boundsMin), v3(node.boundsMax)) >= bestD2) {
            continue;
        }
        if (node.triCount > 0) {
            for (unsigned i = 0; i < node.triCount; ++i) {
                const FlatBvhTri tri = tris[node.leftFirst + i];
                const float3 p = closestOnTri(q, v3(tri.a), v3(tri.b), v3(tri.c));
                const float3 diff = p - q;
                const float dd = d3(diff, diff);
                if (dd < bestD2) {
                    bestD2 = dd;
                    best = p;
                }
            }
        } else if (top + 2 <= kCudaStackSize) {
            stack[top++] = node.leftFirst;
            stack[top++] = node.leftFirst + 1;
        }
    }
    out[qi * 3] = best.x;
    out[qi * 3 + 1] = best.y;
    out[qi * 3 + 2] = best.z;
}

__global__ void raycastKernel(const FlatBvhNode* nodes, const FlatBvhTri* tris,
                              const float* origins, const float* dirs, std::size_t n,
                              float* outHit, int* outFace) {
    const std::size_t ri = blockIdx.x * blockDim.x + threadIdx.x;
    if (ri >= n) {
        return;
    }
    const float3 o = v3(origins + ri * 3);
    float3 dir = v3(dirs + ri * 3);
    const float len = sqrtf(d3(dir, dir));
    if (len > 0.0f) {
        dir = dir * (1.0f / len);
    }
    const float3 inv = make_float3(1.0f / (dir.x != 0.0f ? dir.x : 1e-30f),
                                   1.0f / (dir.y != 0.0f ? dir.y : 1e-30f),
                                   1.0f / (dir.z != 0.0f ? dir.z : 1e-30f));
    float bestT = 3.4e38f;
    int bestFace = -1;
    float3 bestPoint = make_float3(0.0f, 0.0f, 0.0f);
    unsigned stack[kCudaStackSize];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const FlatBvhNode node = nodes[stack[--top]];
        if (!rayBox(o, inv, v3(node.boundsMin), v3(node.boundsMax), bestT)) {
            continue;
        }
        if (node.triCount > 0) {
            for (unsigned i = 0; i < node.triCount; ++i) {
                const FlatBvhTri tri = tris[node.leftFirst + i];
                const float t = rayTri(o, dir, v3(tri.a), v3(tri.b), v3(tri.c));
                if (t >= 0.0f && t < bestT) {
                    bestT = t;
                    bestFace = static_cast<int>(tri.face);
                    bestPoint = o + dir * t;
                }
            }
        } else if (top + 2 <= kCudaStackSize) {
            stack[top++] = node.leftFirst;
            stack[top++] = node.leftFirst + 1;
        }
    }
    outHit[ri * 3] = bestPoint.x;
    outHit[ri * 3 + 1] = bestPoint.y;
    outHit[ri * 3 + 2] = bestPoint.z;
    outFace[ri] = bestFace;
}

// Minimal owning device-pointer helper so kernel launches stay exception-safe.
// It records the allocation status instead of discarding it: a failed cudaMalloc
// otherwise leaves a null pointer that the following memcpy and launch happily
// use, producing a silently untouched output buffer.
template <class T>
class DevicePtr {
public:
    DevicePtr() = default;
    explicit DevicePtr(std::size_t count) {
        if (count > 0) {
            m_status = cudaMalloc(&m_ptr, count * sizeof(T));
        }
    }
    ~DevicePtr() { cudaFree(m_ptr); }
    DevicePtr(const DevicePtr&) = delete;
    DevicePtr& operator=(const DevicePtr&) = delete;
    DevicePtr(DevicePtr&& other) noexcept
        : m_ptr(std::exchange(other.m_ptr, nullptr)),
          m_status(std::exchange(other.m_status, cudaSuccess)) {}
    DevicePtr& operator=(DevicePtr&& other) noexcept {
        if (this != &other) {
            cudaFree(m_ptr);
            m_ptr = std::exchange(other.m_ptr, nullptr);
            m_status = std::exchange(other.m_status, cudaSuccess);
        }
        return *this;
    }
    [[nodiscard]] T* get() const { return m_ptr; }
    [[nodiscard]] bool ok(const char* primitive) const { return cudaOk(m_status, primitive); }

private:
    T* m_ptr = nullptr;
    cudaError_t m_status = cudaSuccess;
};

class CudaBackend final : public IBackend {
public:
    explicit CudaBackend(std::string name) : m_name(std::move(name)) {}

    [[nodiscard]] BackendKind kind() const override { return BackendKind::Cuda; }
    [[nodiscard]] std::string deviceName() const override { return "CUDA (" + m_name + ")"; }

    // parallelFor is the base class's host worker pool and is not overridden:
    // host callables cannot cross to the device, and running the range inline
    // would single-thread every CPU-side loop in the library whenever this
    // backend is selected. Accelerated work runs through the typed kernels
    // below, which are reachable from several host worker threads at once (the
    // AO bake queries from inside its texel loop) and therefore serialize on
    // m_deviceMutex: one resident BVH, one launch at a time.

    void axpy(float alpha, const float* x, float* y, std::size_t n) override {
        if (n == 0) {
            return;
        }
        if (!deviceAxpy(alpha, x, y, n)) {
            IBackend::axpy(alpha, x, y, n);
        }
    }

    void spmvCsr(std::size_t rows, std::size_t cols, const std::size_t* rowStart,
                 const std::size_t* colIndex, const float* value, const float* x,
                 float* y) override {
        if (rows == 0) {
            return;
        }
        if (!deviceSpmv(rows, cols, rowStart, colIndex, value, x, y)) {
            IBackend::spmvCsr(rows, cols, rowStart, colIndex, value, x, y);
        }
    }

    void closestPointsBvh(const FlatBvhNode* nodes, std::size_t nodeCount, const FlatBvhTri* tris,
                          std::size_t triCount, const float* queriesXYZ, std::size_t n,
                          float* outXYZ) override {
        if (n == 0) {
            return;
        }
        if (nodeCount == 0) {
            std::fill(outXYZ, outXYZ + n * 3, 0.0f);
            return;
        }
        if (!deviceClosest(nodes, nodeCount, tris, triCount, queriesXYZ, n, outXYZ)) {
            IBackend::closestPointsBvh(nodes, nodeCount, tris, triCount, queriesXYZ, n, outXYZ);
        }
    }

    void raycastBvh(const FlatBvhNode* nodes, std::size_t nodeCount, const FlatBvhTri* tris,
                    std::size_t triCount, const float* originsXYZ, const float* dirsXYZ,
                    std::size_t n, float* outHitXYZ, int* outFace) override {
        if (n == 0) {
            return;
        }
        if (nodeCount == 0) {
            std::fill(outHitXYZ, outHitXYZ + n * 3, 0.0f);
            std::fill(outFace, outFace + n, -1);
            return;
        }
        if (!deviceRaycast(nodes, nodeCount, tris, triCount, originsXYZ, dirsXYZ, n, outHitXYZ,
                           outFace)) {
            IBackend::raycastBvh(nodes, nodeCount, tris, triCount, originsXYZ, dirsXYZ, n,
                                 outHitXYZ, outFace);
        }
    }

private:
    // Each device* returns false the moment any status is bad, leaving the
    // caller's output untouched for the CPU reference to fill in.
    bool deviceAxpy(float alpha, const float* x, float* y, std::size_t n) {
        static constexpr const char* kName = "axpy";
        const std::lock_guard<std::mutex> lock(m_deviceMutex);
        DevicePtr<float> dx(n), dy(n);
        if (!dx.ok(kName) || !dy.ok(kName)) {
            return false;
        }
        if (!cudaOk(cudaMemcpy(dx.get(), x, n * sizeof(float), cudaMemcpyHostToDevice), kName) ||
            !cudaOk(cudaMemcpy(dy.get(), y, n * sizeof(float), cudaMemcpyHostToDevice), kName)) {
            return false;
        }
        const unsigned block = 256;
        const unsigned grid = static_cast<unsigned>((n + block - 1) / block);
        axpyKernel<<<grid, block>>>(alpha, dx.get(), dy.get(), n);
        if (!cudaOk(cudaGetLastError(), kName)) {
            return false;
        }
        return cudaOk(cudaMemcpy(y, dy.get(), n * sizeof(float), cudaMemcpyDeviceToHost), kName);
    }

    bool deviceSpmv(std::size_t rows, std::size_t cols, const std::size_t* rowStart,
                    const std::size_t* colIndex, const float* value, const float* x, float* y) {
        static constexpr const char* kName = "spmvCsr";
        const std::lock_guard<std::mutex> lock(m_deviceMutex);
        const std::size_t nnz = rowStart[rows];
        DevicePtr<std::size_t> dRowStart(rows + 1), dColIndex(nnz);
        // x is indexed by column, so it is `cols` long — NOT `rows`: the
        // seamless solver's reduction operators are rectangular, and sizing
        // this by the row count either truncates the device copy (wide matrices
        // read past the buffer) or over-reads the caller's host vector (tall
        // matrices).
        DevicePtr<float> dValue(nnz), dx(cols), dy(rows);
        if (!dRowStart.ok(kName) || !dColIndex.ok(kName) || !dValue.ok(kName) || !dx.ok(kName) ||
            !dy.ok(kName)) {
            return false;
        }
        if (!cudaOk(cudaMemcpy(dRowStart.get(), rowStart, (rows + 1) * sizeof(std::size_t),
                               cudaMemcpyHostToDevice),
                    kName) ||
            !cudaOk(cudaMemcpy(dColIndex.get(), colIndex, nnz * sizeof(std::size_t),
                               cudaMemcpyHostToDevice),
                    kName) ||
            !cudaOk(cudaMemcpy(dValue.get(), value, nnz * sizeof(float), cudaMemcpyHostToDevice),
                    kName) ||
            !cudaOk(cudaMemcpy(dx.get(), x, cols * sizeof(float), cudaMemcpyHostToDevice), kName)) {
            return false;
        }
        const unsigned block = 256;
        const unsigned grid = static_cast<unsigned>((rows + block - 1) / block);
        spmvKernel<<<grid, block>>>(rows, dRowStart.get(), dColIndex.get(), dValue.get(), dx.get(),
                                    dy.get());
        if (!cudaOk(cudaGetLastError(), kName)) {
            return false;
        }
        return cudaOk(cudaMemcpy(y, dy.get(), rows * sizeof(float), cudaMemcpyDeviceToHost), kName);
    }

    bool deviceClosest(const FlatBvhNode* nodes, std::size_t nodeCount, const FlatBvhTri* tris,
                       std::size_t triCount, const float* queriesXYZ, std::size_t n,
                       float* outXYZ) {
        static constexpr const char* kName = "closestPointsBvh";
        const std::lock_guard<std::mutex> lock(m_deviceMutex);
        if (!ensureBvhResident(nodes, nodeCount, tris, triCount, kName)) {
            return false;
        }
        DevicePtr<float> dQ(n * 3), dOut(n * 3);
        if (!dQ.ok(kName) || !dOut.ok(kName)) {
            return false;
        }
        if (!cudaOk(cudaMemcpy(dQ.get(), queriesXYZ, n * 3 * sizeof(float), cudaMemcpyHostToDevice),
                    kName)) {
            return false;
        }
        const unsigned block = 128;
        const unsigned grid = static_cast<unsigned>((n + block - 1) / block);
        closestKernel<<<grid, block>>>(m_dNodes.get(), m_dTris.get(), dQ.get(), n, dOut.get());
        if (!cudaOk(cudaGetLastError(), kName)) {
            return false;
        }
        return cudaOk(
            cudaMemcpy(outXYZ, dOut.get(), n * 3 * sizeof(float), cudaMemcpyDeviceToHost), kName);
    }

    bool deviceRaycast(const FlatBvhNode* nodes, std::size_t nodeCount, const FlatBvhTri* tris,
                       std::size_t triCount, const float* originsXYZ, const float* dirsXYZ,
                       std::size_t n, float* outHitXYZ, int* outFace) {
        static constexpr const char* kName = "raycastBvh";
        const std::lock_guard<std::mutex> lock(m_deviceMutex);
        if (!ensureBvhResident(nodes, nodeCount, tris, triCount, kName)) {
            return false;
        }
        DevicePtr<float> dO(n * 3), dD(n * 3), dHit(n * 3);
        DevicePtr<int> dFace(n);
        if (!dO.ok(kName) || !dD.ok(kName) || !dHit.ok(kName) || !dFace.ok(kName)) {
            return false;
        }
        if (!cudaOk(cudaMemcpy(dO.get(), originsXYZ, n * 3 * sizeof(float), cudaMemcpyHostToDevice),
                    kName) ||
            !cudaOk(cudaMemcpy(dD.get(), dirsXYZ, n * 3 * sizeof(float), cudaMemcpyHostToDevice),
                    kName)) {
            return false;
        }
        const unsigned block = 128;
        const unsigned grid = static_cast<unsigned>((n + block - 1) / block);
        raycastKernel<<<grid, block>>>(m_dNodes.get(), m_dTris.get(), dO.get(), dD.get(), n,
                                       dHit.get(), dFace.get());
        if (!cudaOk(cudaGetLastError(), kName)) {
            return false;
        }
        return cudaOk(cudaMemcpy(outHitXYZ, dHit.get(), n * 3 * sizeof(float),
                                 cudaMemcpyDeviceToHost),
                      kName) &&
               cudaOk(cudaMemcpy(outFace, dFace.get(), n * sizeof(int), cudaMemcpyDeviceToHost),
                      kName);
    }

    // Keeps the last-queried BVH on the device. The AO bake issues one query
    // per texel against the same hierarchy, so re-uploading here made bake cost
    // O(texels x target triangles) — the host side already hoists its flatten()
    // out of the loop for the same reason.
    bool ensureBvhResident(const FlatBvhNode* nodes, std::size_t nodeCount, const FlatBvhTri* tris,
                           std::size_t triCount, const char* primitive) {
        const detail::BvhResidencyKey key =
            detail::makeBvhResidencyKey(nodes, nodeCount, tris, triCount);
        if (m_bvhResident && key == m_bvhKey) {
            return true;
        }
        m_bvhResident = false;
        DevicePtr<FlatBvhNode> dNodes(nodeCount);
        DevicePtr<FlatBvhTri> dTris(triCount > 0 ? triCount : 1);
        if (!dNodes.ok(primitive) || !dTris.ok(primitive)) {
            return false;
        }
        if (!cudaOk(cudaMemcpy(dNodes.get(), nodes, nodeCount * sizeof(FlatBvhNode),
                               cudaMemcpyHostToDevice),
                    primitive)) {
            return false;
        }
        if (triCount > 0 && !cudaOk(cudaMemcpy(dTris.get(), tris, triCount * sizeof(FlatBvhTri),
                                               cudaMemcpyHostToDevice),
                                    primitive)) {
            return false;
        }
        m_dNodes = std::move(dNodes);
        m_dTris = std::move(dTris);
        m_bvhKey = key;
        m_bvhResident = true;
        return true;
    }

    std::string m_name;
    std::mutex m_deviceMutex;
    detail::BvhResidencyKey m_bvhKey;
    bool m_bvhResident = false;
    DevicePtr<FlatBvhNode> m_dNodes;
    DevicePtr<FlatBvhTri> m_dTris;
};

}  // namespace

std::shared_ptr<IBackend> makeCudaBackend() {
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0) {
        return nullptr;
    }
    cudaDeviceProp props{};
    if (cudaGetDeviceProperties(&props, 0) != cudaSuccess) {
        return nullptr;
    }
    return std::make_shared<CudaBackend>(props.name);
}

}  // namespace cyber::accel
