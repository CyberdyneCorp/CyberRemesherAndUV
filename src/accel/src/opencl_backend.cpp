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

private:
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
