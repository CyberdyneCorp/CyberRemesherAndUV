// OpenCL compute backend (compute-acceleration spec, tier-2). Kernels are
// compiled at runtime from the source string below, so no build-time device
// architecture selection is needed and the same binary runs anywhere an
// OpenCL 1.2+ ICD is present. makeOpenClBackend() returns nullptr when no
// platform/device is available, so the registry degrades gracefully to CPU.
//
// Verified on NVIDIA (RTX 5060, driver 580) via the parity tests in
// tests/accel/test_gpu_parity.cpp when built with -DCYBER_ENABLE_OPENCL=ON.

#define CL_TARGET_OPENCL_VERSION 120
#define CL_HPP_TARGET_OPENCL_VERSION 120
#define CL_HPP_MINIMUM_OPENCL_VERSION 120
#define CL_HPP_ENABLE_EXCEPTIONS

#include <CL/opencl.hpp>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "cyber/accel/backend.hpp"

namespace cyber::accel {

namespace {

// index_t mirrors std::size_t (8 bytes) so the CSR index buffers upload
// verbatim; the host guarantees the platform is 64-bit.
const char* kSource = R"CLC(
typedef ulong index_t;

__kernel void axpy(const float alpha, __global const float* x, __global float* y) {
    const size_t i = get_global_id(0);
    y[i] += alpha * x[i];
}

__kernel void spmv(const index_t rows,
                   __global const index_t* rowStart,
                   __global const index_t* colIndex,
                   __global const float* value,
                   __global const float* x,
                   __global float* y) {
    const size_t row = get_global_id(0);
    if (row >= rows) return;
    float sum = 0.0f;
    for (index_t k = rowStart[row]; k < rowStart[row + 1]; ++k) {
        sum += value[k] * x[colIndex[k]];
    }
    y[row] = sum;
}

// --- Flat-BVH geometry kernels (roadmap 4.6/5.8/11.1) --------------------
// The BVH is uploaded as plain scalar arrays (bounds: 6 floats/node, meta: 2
// uints/node = {leftFirst, triCount}; triangles: 9 floats/tri + 1 uint face) so
// there is no host/device struct-padding ambiguity. One work-item per query,
// iterative fixed-size-stack traversal matching the CPU reference.

inline float3 ld3(__global const float* p, uint i) { return (float3)(p[i], p[i + 1], p[i + 2]); }

float3 closestOnTri(float3 p, float3 a, float3 b, float3 c) {
    float3 ab = b - a; float3 ac = c - a; float3 ap = p - a;
    float d1 = dot(ab, ap); float d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;
    float3 bp = p - b;
    float d3 = dot(ab, bp); float d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return a + ab * (d1 / (d1 - d3));
    float3 cp = p - c;
    float d5 = dot(ab, cp); float d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return a + ac * (d2 / (d2 - d6));
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

float boxDistSq(float3 p, float3 lo, float3 hi) {
    float3 d = fmax(fmax(lo - p, (float3)(0.0f)), p - hi);
    return dot(d, d);
}

bool rayBox(float3 o, float3 inv, float3 lo, float3 hi, float maxT) {
    float3 t1 = (lo - o) * inv;
    float3 t2 = (hi - o) * inv;
    float3 tmn = fmin(t1, t2);
    float3 tmx = fmax(t1, t2);
    float tmin = fmax(fmax(tmn.x, tmn.y), fmax(tmn.z, 0.0f));
    float tmax = fmin(fmin(tmx.x, tmx.y), fmin(tmx.z, maxT));
    return tmin <= tmax;
}

float rayTri(float3 o, float3 dir, float3 a, float3 b, float3 c) {
    const float kEps = 1e-9f;
    float3 ab = b - a; float3 ac = c - a;
    float3 pvec = cross(dir, ac);
    float det = dot(ab, pvec);
    if (fabs(det) < kEps) return -1.0f;
    float invDet = 1.0f / det;
    float3 tvec = o - a;
    float u = dot(tvec, pvec) * invDet;
    if (u < 0.0f || u > 1.0f) return -1.0f;
    float3 qvec = cross(tvec, ab);
    float v = dot(dir, qvec) * invDet;
    if (v < 0.0f || u + v > 1.0f) return -1.0f;
    float t = dot(ac, qvec) * invDet;
    return t < 0.0f ? -1.0f : t;
}

__kernel void closest_bvh(const uint nodeCount,
                          __global const float* bounds,
                          __global const uint* meta,
                          __global const float* triV,
                          __global const float* queries,
                          const uint n,
                          __global float* out) {
    const uint qi = get_global_id(0);
    if (qi >= n) return;
    float3 q = ld3(queries, qi * 3);
    float3 best = (float3)(0.0f);
    float bestD2 = 3.4e38f;
    uint stack[64];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        uint ni = stack[--top];
        float3 lo = ld3(bounds, ni * 6);
        float3 hi = ld3(bounds, ni * 6 + 3);
        if (boxDistSq(q, lo, hi) >= bestD2) continue;
        uint leftFirst = meta[ni * 2];
        uint triCount = meta[ni * 2 + 1];
        if (triCount > 0) {
            for (uint i = 0; i < triCount; ++i) {
                uint base = (leftFirst + i) * 9;
                float3 p = closestOnTri(q, ld3(triV, base), ld3(triV, base + 3), ld3(triV, base + 6));
                float3 diff = p - q;
                float dd = dot(diff, diff);
                if (dd < bestD2) { bestD2 = dd; best = p; }
            }
        } else if (top + 2 <= 64) {
            stack[top++] = leftFirst;
            stack[top++] = leftFirst + 1;
        }
    }
    out[qi * 3] = best.x;
    out[qi * 3 + 1] = best.y;
    out[qi * 3 + 2] = best.z;
}

__kernel void raycast_bvh(const uint nodeCount,
                          __global const float* bounds,
                          __global const uint* meta,
                          __global const float* triV,
                          __global const uint* triFace,
                          __global const float* origins,
                          __global const float* dirs,
                          const uint n,
                          __global float* outHit,
                          __global int* outFace) {
    const uint ri = get_global_id(0);
    if (ri >= n) return;
    float3 o = ld3(origins, ri * 3);
    float3 dir = ld3(dirs, ri * 3);
    float len = sqrt(dot(dir, dir));
    if (len > 0.0f) dir = dir * (1.0f / len);
    float3 inv = (float3)(1.0f / (dir.x != 0.0f ? dir.x : 1e-30f),
                          1.0f / (dir.y != 0.0f ? dir.y : 1e-30f),
                          1.0f / (dir.z != 0.0f ? dir.z : 1e-30f));
    float bestT = 3.4e38f;
    int bestFace = -1;
    float3 bestPoint = (float3)(0.0f);
    uint stack[64];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        uint ni = stack[--top];
        float3 lo = ld3(bounds, ni * 6);
        float3 hi = ld3(bounds, ni * 6 + 3);
        if (!rayBox(o, inv, lo, hi, bestT)) continue;
        uint leftFirst = meta[ni * 2];
        uint triCount = meta[ni * 2 + 1];
        if (triCount > 0) {
            for (uint i = 0; i < triCount; ++i) {
                uint tri = leftFirst + i;
                uint base = tri * 9;
                float t = rayTri(o, dir, ld3(triV, base), ld3(triV, base + 3), ld3(triV, base + 6));
                if (t >= 0.0f && t < bestT) {
                    bestT = t;
                    bestFace = (int)triFace[tri];
                    bestPoint = o + dir * t;
                }
            }
        } else if (top + 2 <= 64) {
            stack[top++] = leftFirst;
            stack[top++] = leftFirst + 1;
        }
    }
    outHit[ri * 3] = bestPoint.x;
    outHit[ri * 3 + 1] = bestPoint.y;
    outHit[ri * 3 + 2] = bestPoint.z;
    outFace[ri] = bestFace;
}
)CLC";

class OpenClBackend final : public IBackend {
public:
    OpenClBackend(cl::Device device, cl::Context context, cl::Program program)
        : m_device(std::move(device)),
          m_context(std::move(context)),
          m_program(std::move(program)),
          m_queue(m_context, m_device) {}

    [[nodiscard]] BackendKind kind() const override { return BackendKind::OpenCl; }

    [[nodiscard]] std::string deviceName() const override {
        return "OpenCL (" + m_device.getInfo<CL_DEVICE_NAME>() + ")";
    }

    // Arbitrary host callables cannot cross to the device; the generic
    // parallelFor stays on the host. Accelerated work goes through the typed
    // kernels below (spec: "irregular work remains on CPU").
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
        const std::size_t bytes = n * sizeof(float);
        cl::Buffer bx(m_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, bytes,
                      const_cast<float*>(x));
        cl::Buffer by(m_context, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, bytes, y);
        cl::Kernel kernel(m_program, "axpy");
        kernel.setArg(0, alpha);
        kernel.setArg(1, bx);
        kernel.setArg(2, by);
        m_queue.enqueueNDRangeKernel(kernel, cl::NullRange, cl::NDRange(n));
        m_queue.enqueueReadBuffer(by, CL_TRUE, 0, bytes, y);
    }

    void spmvCsr(std::size_t rows, const std::size_t* rowStart, const std::size_t* colIndex,
                 const float* value, const float* x, float* y) override {
        if (rows == 0) {
            return;
        }
        const std::size_t nnz = rowStart[rows];
        // Widest dimension referenced by any column index is `rows`.
        cl::Buffer bRowStart(m_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                             (rows + 1) * sizeof(std::size_t), const_cast<std::size_t*>(rowStart));
        cl::Buffer bColIndex(m_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                             nnz * sizeof(std::size_t), const_cast<std::size_t*>(colIndex));
        cl::Buffer bValue(m_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, nnz * sizeof(float),
                          const_cast<float*>(value));
        cl::Buffer bx(m_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, rows * sizeof(float),
                      const_cast<float*>(x));
        cl::Buffer by(m_context, CL_MEM_WRITE_ONLY, rows * sizeof(float));
        cl::Kernel kernel(m_program, "spmv");
        kernel.setArg(0, static_cast<cl_ulong>(rows));
        kernel.setArg(1, bRowStart);
        kernel.setArg(2, bColIndex);
        kernel.setArg(3, bValue);
        kernel.setArg(4, bx);
        kernel.setArg(5, by);
        m_queue.enqueueNDRangeKernel(kernel, cl::NullRange, cl::NDRange(rows));
        m_queue.enqueueReadBuffer(by, CL_TRUE, 0, rows * sizeof(float), y);
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
        std::vector<float> bounds;
        std::vector<cl_uint> meta;
        std::vector<float> triV;
        std::vector<cl_uint> triFace;
        packBvh(nodes, nodeCount, tris, triCount, bounds, meta, triV, triFace);

        cl::Buffer bBounds = readBuffer(bounds);
        cl::Buffer bMeta = readBuffer(meta);
        cl::Buffer bTriV = readBuffer(triV);
        cl::Buffer bQ(m_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, n * 3 * sizeof(float),
                      const_cast<float*>(queriesXYZ));
        cl::Buffer bOut(m_context, CL_MEM_WRITE_ONLY, n * 3 * sizeof(float));

        cl::Kernel kernel(m_program, "closest_bvh");
        kernel.setArg(0, static_cast<cl_uint>(nodeCount));
        kernel.setArg(1, bBounds);
        kernel.setArg(2, bMeta);
        kernel.setArg(3, bTriV);
        kernel.setArg(4, bQ);
        kernel.setArg(5, static_cast<cl_uint>(n));
        kernel.setArg(6, bOut);
        m_queue.enqueueNDRangeKernel(kernel, cl::NullRange, cl::NDRange(n));
        m_queue.enqueueReadBuffer(bOut, CL_TRUE, 0, n * 3 * sizeof(float), outXYZ);
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
        std::vector<float> bounds;
        std::vector<cl_uint> meta;
        std::vector<float> triV;
        std::vector<cl_uint> triFace;
        packBvh(nodes, nodeCount, tris, triCount, bounds, meta, triV, triFace);

        cl::Buffer bBounds = readBuffer(bounds);
        cl::Buffer bMeta = readBuffer(meta);
        cl::Buffer bTriV = readBuffer(triV);
        cl::Buffer bFace = readBuffer(triFace);
        cl::Buffer bO(m_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, n * 3 * sizeof(float),
                      const_cast<float*>(originsXYZ));
        cl::Buffer bD(m_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, n * 3 * sizeof(float),
                      const_cast<float*>(dirsXYZ));
        cl::Buffer bHit(m_context, CL_MEM_WRITE_ONLY, n * 3 * sizeof(float));
        cl::Buffer bOutFace(m_context, CL_MEM_WRITE_ONLY, n * sizeof(int));

        cl::Kernel kernel(m_program, "raycast_bvh");
        kernel.setArg(0, static_cast<cl_uint>(nodeCount));
        kernel.setArg(1, bBounds);
        kernel.setArg(2, bMeta);
        kernel.setArg(3, bTriV);
        kernel.setArg(4, bFace);
        kernel.setArg(5, bO);
        kernel.setArg(6, bD);
        kernel.setArg(7, static_cast<cl_uint>(n));
        kernel.setArg(8, bHit);
        kernel.setArg(9, bOutFace);
        m_queue.enqueueNDRangeKernel(kernel, cl::NullRange, cl::NDRange(n));
        m_queue.enqueueReadBuffer(bHit, CL_TRUE, 0, n * 3 * sizeof(float), outHitXYZ);
        m_queue.enqueueReadBuffer(bOutFace, CL_TRUE, 0, n * sizeof(int), outFace);
    }

private:
    // Repacks the flat BVH into scalar arrays matching the kernel layout.
    static void packBvh(const FlatBvhNode* nodes, std::size_t nodeCount, const FlatBvhTri* tris,
                        std::size_t triCount, std::vector<float>& bounds,
                        std::vector<cl_uint>& meta, std::vector<float>& triV,
                        std::vector<cl_uint>& triFace) {
        bounds.resize(nodeCount * 6);
        meta.resize(nodeCount * 2);
        for (std::size_t i = 0; i < nodeCount; ++i) {
            bounds[i * 6 + 0] = nodes[i].boundsMin[0];
            bounds[i * 6 + 1] = nodes[i].boundsMin[1];
            bounds[i * 6 + 2] = nodes[i].boundsMin[2];
            bounds[i * 6 + 3] = nodes[i].boundsMax[0];
            bounds[i * 6 + 4] = nodes[i].boundsMax[1];
            bounds[i * 6 + 5] = nodes[i].boundsMax[2];
            meta[i * 2 + 0] = nodes[i].leftFirst;
            meta[i * 2 + 1] = nodes[i].triCount;
        }
        // Keep at least one element so cl::Buffer never has size 0.
        triV.resize((triCount > 0 ? triCount : 1) * 9);
        triFace.resize(triCount > 0 ? triCount : 1);
        for (std::size_t i = 0; i < triCount; ++i) {
            const FlatBvhTri& t = tris[i];
            triV[i * 9 + 0] = t.a[0];
            triV[i * 9 + 1] = t.a[1];
            triV[i * 9 + 2] = t.a[2];
            triV[i * 9 + 3] = t.b[0];
            triV[i * 9 + 4] = t.b[1];
            triV[i * 9 + 5] = t.b[2];
            triV[i * 9 + 6] = t.c[0];
            triV[i * 9 + 7] = t.c[1];
            triV[i * 9 + 8] = t.c[2];
            triFace[i] = t.face;
        }
    }

    template <class T>
    cl::Buffer readBuffer(std::vector<T>& host) {
        return cl::Buffer(m_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                          host.size() * sizeof(T), host.data());
    }

    cl::Device m_device;
    cl::Context m_context;
    cl::Program m_program;
    cl::CommandQueue m_queue;
};

}  // namespace

std::shared_ptr<IBackend> makeOpenClBackend() {
    try {
        std::vector<cl::Platform> platforms;
        cl::Platform::get(&platforms);
        for (const cl::Platform& platform : platforms) {
            std::vector<cl::Device> devices;
            platform.getDevices(CL_DEVICE_TYPE_GPU, &devices);
            if (devices.empty()) {
                continue;
            }
            cl::Device device = devices.front();
            cl::Context context(device);
            cl::Program program(context, kSource);
            program.build({device});
            return std::make_shared<OpenClBackend>(std::move(device), std::move(context),
                                                   std::move(program));
        }
    } catch (const cl::Error&) {
        // No usable OpenCL device/driver — fall back to CPU.
    }
    return nullptr;
}

}  // namespace cyber::accel
