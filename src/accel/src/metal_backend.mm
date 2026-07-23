// Metal compute backend (compute-acceleration spec, tier-1 on Apple).
//
// UNVERIFIED: written best-effort on non-Apple hardware and NOT compiled or
// run — no macOS/iOS toolchain is available in the development environment.
// The structure mirrors the CUDA/OpenCL backends (which ARE verified against
// the CPU reference) so the parity tests (task 4.6) validate it as soon as it
// builds on real Apple hardware. Treat every line here as needing a first
// real compile + parity run before it can be trusted.
//
// makeMetalBackend() returns nullptr when no Metal device is present so the
// registry degrades gracefully to CPU.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <memory>
#include <string>

#include "cyber/accel/backend.hpp"

namespace cyber::accel {

namespace {

// Runtime-compiled MSL, mirroring the CUDA/OpenCL kernels. size_t maps to
// 64-bit `ulong` on Apple silicon so the CSR index buffers upload verbatim.
constexpr const char* kSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

kernel void axpy(constant float& alpha [[buffer(0)]],
                 device const float* x [[buffer(1)]],
                 device float* y [[buffer(2)]],
                 uint i [[thread_position_in_grid]]) {
    y[i] += alpha * x[i];
}

kernel void spmv(constant ulong& rows [[buffer(0)]],
                 device const ulong* rowStart [[buffer(1)]],
                 device const ulong* colIndex [[buffer(2)]],
                 device const float* value [[buffer(3)]],
                 device const float* x [[buffer(4)]],
                 device float* y [[buffer(5)]],
                 uint row [[thread_position_in_grid]]) {
    if (row >= rows) return;
    float sum = 0.0f;
    for (ulong k = rowStart[row]; k < rowStart[row + 1]; ++k) {
        sum += value[k] * x[colIndex[k]];
    }
    y[row] = sum;
}
)MSL";

class MetalBackend final : public IBackend {
public:
    MetalBackend(id<MTLDevice> device, id<MTLLibrary> library)
        : m_device(device), m_queue([device newCommandQueue]), m_library(library) {}

    BackendKind kind() const override { return BackendKind::Metal; }
    std::string deviceName() const override {
        return "Metal (" + std::string([[m_device name] UTF8String]) + ")";
    }

    void parallelFor(std::size_t begin, std::size_t end,
                     const std::function<void(std::size_t, std::size_t)>& fn) override {
        if (begin < end) {
            fn(begin, end);
        }
    }

    void axpy(float alpha, const float* x, float* y, std::size_t n) override {
        if (n == 0) return;
        const std::size_t bytes = n * sizeof(float);
        id<MTLBuffer> bx = [m_device newBufferWithBytes:x length:bytes options:0];
        id<MTLBuffer> by = [m_device newBufferWithBytes:y length:bytes options:0];
        id<MTLComputePipelineState> pipeline = pipelineFor(@"axpy");
        id<MTLCommandBuffer> cmd = [m_queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:pipeline];
        [enc setBytes:&alpha length:sizeof(float) atIndex:0];
        [enc setBuffer:bx offset:0 atIndex:1];
        [enc setBuffer:by offset:0 atIndex:2];
        dispatch(enc, n);
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];
        std::memcpy(y, [by contents], bytes);
    }

    void spmvCsr(std::size_t rows, const std::size_t* rowStart, const std::size_t* colIndex,
                 const float* value, const float* x, float* y) override {
        if (rows == 0) return;
        const std::size_t nnz = rowStart[rows];
        id<MTLBuffer> bRowStart = [m_device newBufferWithBytes:rowStart
                                                        length:(rows + 1) * sizeof(std::size_t)
                                                       options:0];
        id<MTLBuffer> bColIndex = [m_device newBufferWithBytes:colIndex
                                                        length:nnz * sizeof(std::size_t)
                                                       options:0];
        id<MTLBuffer> bValue = [m_device newBufferWithBytes:value length:nnz * sizeof(float) options:0];
        id<MTLBuffer> bx = [m_device newBufferWithBytes:x length:rows * sizeof(float) options:0];
        id<MTLBuffer> by = [m_device newBufferWithLength:rows * sizeof(float) options:0];
        id<MTLComputePipelineState> pipeline = pipelineFor(@"spmv");
        id<MTLCommandBuffer> cmd = [m_queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:pipeline];
        uint64_t rows64 = rows;
        [enc setBytes:&rows64 length:sizeof(uint64_t) atIndex:0];
        [enc setBuffer:bRowStart offset:0 atIndex:1];
        [enc setBuffer:bColIndex offset:0 atIndex:2];
        [enc setBuffer:bValue offset:0 atIndex:3];
        [enc setBuffer:bx offset:0 atIndex:4];
        [enc setBuffer:by offset:0 atIndex:5];
        dispatch(enc, rows);
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];
        std::memcpy(y, [by contents], rows * sizeof(float));
    }

private:
    id<MTLComputePipelineState> pipelineFor(NSString* name) {
        id<MTLFunction> fn = [m_library newFunctionWithName:name];
        return [m_device newComputePipelineStateWithFunction:fn error:nil];
    }

    void dispatch(id<MTLComputeCommandEncoder> enc, std::size_t n) {
        MTLSize grid = MTLSizeMake(n, 1, 1);
        MTLSize group = MTLSizeMake(std::min<std::size_t>(n, 256), 1, 1);
        [enc dispatchThreads:grid threadsPerThreadgroup:group];
    }

    id<MTLDevice> m_device;
    id<MTLCommandQueue> m_queue;
    id<MTLLibrary> m_library;
};

}  // namespace

std::shared_ptr<IBackend> makeMetalBackend() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            return nullptr;
        }
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:kSource]
                                                      options:nil
                                                        error:&error];
        if (library == nil) {
            return nullptr;
        }
        return std::make_shared<MetalBackend>(device, library);
    }
}

}  // namespace cyber::accel
