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
#include <memory>
#include <string>
#include <vector>

#include "cyber/accel/backend.hpp"

namespace cyber::accel {

namespace {

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

__device__ inline float rayTri(float3 o, float3 dir, float3 a, float3 b, float3 c) {
    const float kEpsilon = 1e-9f;
    const float3 ab = b - a;
    const float3 ac = c - a;
    const float3 pvec = cross3(dir, ac);
    const float det = d3(ab, pvec);
    if (fabsf(det) < kEpsilon) {
        return -1.0f;
    }
    const float invDet = 1.0f / det;
    const float3 tvec = o - a;
    const float u = d3(tvec, pvec) * invDet;
    if (u < 0.0f || u > 1.0f) {
        return -1.0f;
    }
    const float3 qvec = cross3(tvec, ab);
    const float v = d3(dir, qvec) * invDet;
    if (v < 0.0f || u + v > 1.0f) {
        return -1.0f;
    }
    const float t = d3(ac, qvec) * invDet;
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
template <class T>
class DevicePtr {
public:
    explicit DevicePtr(std::size_t count) { cudaMalloc(&m_ptr, count * sizeof(T)); }
    ~DevicePtr() { cudaFree(m_ptr); }
    DevicePtr(const DevicePtr&) = delete;
    DevicePtr& operator=(const DevicePtr&) = delete;
    [[nodiscard]] T* get() const { return m_ptr; }

private:
    T* m_ptr = nullptr;
};

class CudaBackend final : public IBackend {
public:
    explicit CudaBackend(std::string name) : m_name(std::move(name)) {}

    [[nodiscard]] BackendKind kind() const override { return BackendKind::Cuda; }
    [[nodiscard]] std::string deviceName() const override { return "CUDA (" + m_name + ")"; }

    // Host callables cannot cross to the device; generic parallelFor stays on
    // the host. Accelerated work runs through the typed kernels below.
    void parallelFor(std::size_t begin, std::size_t end,
                     const std::function<void(std::size_t, std::size_t)>& fn) override {
        if (begin < end) {
            fn(begin, end);
        }
    }

    void axpy(float alpha, const float* x, float* y, std::size_t n) override {
        if (n == 0) {
            return;
        }
        DevicePtr<float> dx(n), dy(n);
        cudaMemcpy(dx.get(), x, n * sizeof(float), cudaMemcpyHostToDevice);
        cudaMemcpy(dy.get(), y, n * sizeof(float), cudaMemcpyHostToDevice);
        const unsigned block = 256;
        const unsigned grid = static_cast<unsigned>((n + block - 1) / block);
        axpyKernel<<<grid, block>>>(alpha, dx.get(), dy.get(), n);
        cudaMemcpy(y, dy.get(), n * sizeof(float), cudaMemcpyDeviceToHost);
    }

    void spmvCsr(std::size_t rows, const std::size_t* rowStart, const std::size_t* colIndex,
                 const float* value, const float* x, float* y) override {
        if (rows == 0) {
            return;
        }
        const std::size_t nnz = rowStart[rows];
        DevicePtr<std::size_t> dRowStart(rows + 1), dColIndex(nnz);
        DevicePtr<float> dValue(nnz), dx(rows), dy(rows);
        cudaMemcpy(dRowStart.get(), rowStart, (rows + 1) * sizeof(std::size_t),
                   cudaMemcpyHostToDevice);
        cudaMemcpy(dColIndex.get(), colIndex, nnz * sizeof(std::size_t), cudaMemcpyHostToDevice);
        cudaMemcpy(dValue.get(), value, nnz * sizeof(float), cudaMemcpyHostToDevice);
        cudaMemcpy(dx.get(), x, rows * sizeof(float), cudaMemcpyHostToDevice);
        const unsigned block = 256;
        const unsigned grid = static_cast<unsigned>((rows + block - 1) / block);
        spmvKernel<<<grid, block>>>(rows, dRowStart.get(), dColIndex.get(), dValue.get(), dx.get(),
                                    dy.get());
        cudaMemcpy(y, dy.get(), rows * sizeof(float), cudaMemcpyDeviceToHost);
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
        DevicePtr<FlatBvhNode> dNodes(nodeCount);
        DevicePtr<FlatBvhTri> dTris(triCount > 0 ? triCount : 1);
        DevicePtr<float> dQ(n * 3), dOut(n * 3);
        cudaMemcpy(dNodes.get(), nodes, nodeCount * sizeof(FlatBvhNode), cudaMemcpyHostToDevice);
        if (triCount > 0) {
            cudaMemcpy(dTris.get(), tris, triCount * sizeof(FlatBvhTri), cudaMemcpyHostToDevice);
        }
        cudaMemcpy(dQ.get(), queriesXYZ, n * 3 * sizeof(float), cudaMemcpyHostToDevice);
        const unsigned block = 128;
        const unsigned grid = static_cast<unsigned>((n + block - 1) / block);
        closestKernel<<<grid, block>>>(dNodes.get(), dTris.get(), dQ.get(), n, dOut.get());
        cudaMemcpy(outXYZ, dOut.get(), n * 3 * sizeof(float), cudaMemcpyDeviceToHost);
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
        DevicePtr<FlatBvhNode> dNodes(nodeCount);
        DevicePtr<FlatBvhTri> dTris(triCount > 0 ? triCount : 1);
        DevicePtr<float> dO(n * 3), dD(n * 3), dHit(n * 3);
        DevicePtr<int> dFace(n);
        cudaMemcpy(dNodes.get(), nodes, nodeCount * sizeof(FlatBvhNode), cudaMemcpyHostToDevice);
        if (triCount > 0) {
            cudaMemcpy(dTris.get(), tris, triCount * sizeof(FlatBvhTri), cudaMemcpyHostToDevice);
        }
        cudaMemcpy(dO.get(), originsXYZ, n * 3 * sizeof(float), cudaMemcpyHostToDevice);
        cudaMemcpy(dD.get(), dirsXYZ, n * 3 * sizeof(float), cudaMemcpyHostToDevice);
        const unsigned block = 128;
        const unsigned grid = static_cast<unsigned>((n + block - 1) / block);
        raycastKernel<<<grid, block>>>(dNodes.get(), dTris.get(), dO.get(), dD.get(), n, dHit.get(),
                                       dFace.get());
        cudaMemcpy(outHitXYZ, dHit.get(), n * 3 * sizeof(float), cudaMemcpyDeviceToHost);
        cudaMemcpy(outFace, dFace.get(), n * sizeof(int), cudaMemcpyDeviceToHost);
    }

private:
    std::string m_name;
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
