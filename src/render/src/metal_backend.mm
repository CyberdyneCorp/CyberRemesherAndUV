// UNVERIFIED: This Metal backend is Objective-C++ (.mm) and requires Apple's
// Metal.framework + a real Metal-capable GPU to build and run. It cannot be
// compiled or exercised in headless Linux CI (no Metal SDK, no clang Obj-C++
// toolchain, no CAMetalLayer surface). It compiles in an isolated target
// WITHOUT the project's strict warning set. Every call site is unverified until
// run on Apple hardware.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstring>
#include <stdexcept>

#include "cyber/render/rhi.hpp"

namespace cyber::render {
namespace {

MTLPixelFormat toMtlFormat(TextureFormat format) {
    switch (format) {
        case TextureFormat::RGBA8Unorm:
            return MTLPixelFormatRGBA8Unorm;
        case TextureFormat::BGRA8Unorm:
            return MTLPixelFormatBGRA8Unorm;
        case TextureFormat::RGBA16Float:
            return MTLPixelFormatRGBA16Float;
        case TextureFormat::R32Uint:
            return MTLPixelFormatR32Uint;
        case TextureFormat::Depth32Float:
            return MTLPixelFormatDepth32Float;
        case TextureFormat::Depth24Stencil8:
            return MTLPixelFormatDepth24Unorm_Stencil8;
    }
    return MTLPixelFormatInvalid;
}

MTLPrimitiveType toMtlPrimitive(PrimitiveTopology topology) {
    switch (topology) {
        case PrimitiveTopology::TriangleList:
            return MTLPrimitiveTypeTriangle;
        case PrimitiveTopology::TriangleStrip:
            return MTLPrimitiveTypeTriangleStrip;
        case PrimitiveTopology::LineList:
            return MTLPrimitiveTypeLine;
        case PrimitiveTopology::PointList:
            return MTLPrimitiveTypePoint;
    }
    return MTLPrimitiveTypeTriangle;
}

class MetalBuffer final : public IBuffer {
public:
    MetalBuffer(id<MTLDevice> device, const BufferDesc& desc) : m_size(desc.sizeBytes) {
        const MTLResourceOptions options = desc.memory == MemoryLocation::HostVisible
                                               ? MTLResourceStorageModeShared
                                               : MTLResourceStorageModePrivate;
        m_buffer = [device newBufferWithLength:desc.sizeBytes options:options];
        if (m_buffer == nil) {
            throw std::runtime_error("metal: newBufferWithLength failed");
        }
    }

    std::uint64_t sizeBytes() const override { return m_size; }

    void update(std::span<const std::uint8_t> bytes, std::uint64_t offset) override {
        if (m_buffer.storageMode != MTLStorageModeShared) {
            throw std::runtime_error("metal: update() on private buffer");
        }
        auto* dst = static_cast<std::uint8_t*>(m_buffer.contents) + offset;
        std::memcpy(dst, bytes.data(), bytes.size());
    }

    id<MTLBuffer> handle() const { return m_buffer; }

private:
    id<MTLBuffer> m_buffer = nil;
    std::uint64_t m_size = 0;
};

class MetalTexture final : public ITexture {
public:
    MetalTexture(id<MTLDevice> device, const TextureDesc& desc)
        : m_width(desc.width), m_height(desc.height), m_format(desc.format) {
        MTLTextureDescriptor* d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:toMtlFormat(desc.format)
                                                               width:desc.width
                                                              height:desc.height
                                                           mipmapped:desc.mipLevels > 1];
        d.usage = MTLTextureUsageShaderRead |
                  (desc.renderTarget ? MTLTextureUsageRenderTarget : 0);
        m_texture = [device newTextureWithDescriptor:d];
        if (m_texture == nil) {
            throw std::runtime_error("metal: newTextureWithDescriptor failed");
        }
    }

    explicit MetalTexture(id<MTLTexture> existing)
        : m_texture(existing),
          m_width(static_cast<std::uint32_t>(existing.width)),
          m_height(static_cast<std::uint32_t>(existing.height)),
          m_format(TextureFormat::BGRA8Unorm) {}

    std::uint32_t width() const override { return m_width; }
    std::uint32_t height() const override { return m_height; }
    TextureFormat format() const override { return m_format; }

    id<MTLTexture> handle() const { return m_texture; }

private:
    id<MTLTexture> m_texture = nil;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    TextureFormat m_format = TextureFormat::RGBA8Unorm;
};

class MetalPipeline final : public IPipeline {
public:
    MetalPipeline(id<MTLDevice> device, const PipelineDesc& desc)
        : m_compute(!desc.computeShader.empty()) {
        NSError* error = nil;
        if (m_compute) {
            id<MTLLibrary> lib = makeLibrary(device, desc.computeShader);
            id<MTLFunction> fn = [lib newFunctionWithName:@"cs_main"];
            m_computeState = [device newComputePipelineStateWithFunction:fn error:&error];
        } else {
            id<MTLLibrary> vlib = makeLibrary(device, desc.vertexShader);
            id<MTLLibrary> flib = makeLibrary(device, desc.fragmentShader);
            MTLRenderPipelineDescriptor* rp = [[MTLRenderPipelineDescriptor alloc] init];
            rp.vertexFunction = [vlib newFunctionWithName:@"vs_main"];
            rp.fragmentFunction = [flib newFunctionWithName:@"fs_main"];
            rp.colorAttachments[0].pixelFormat = toMtlFormat(desc.colorFormat);
            rp.depthAttachmentPixelFormat = toMtlFormat(desc.depthFormat);
            m_renderState = [device newRenderPipelineStateWithDescriptor:rp error:&error];
        }
        m_topology = toMtlPrimitive(desc.topology);
        if (error != nil) {
            throw std::runtime_error("metal: pipeline creation failed");
        }
    }

    bool isCompute() const override { return m_compute; }
    id<MTLRenderPipelineState> renderState() const { return m_renderState; }
    id<MTLComputePipelineState> computeState() const { return m_computeState; }
    MTLPrimitiveType topology() const { return m_topology; }

private:
    static id<MTLLibrary> makeLibrary(id<MTLDevice> device,
                                      const std::vector<std::uint8_t>& blob) {
        dispatch_data_t data = dispatch_data_create(blob.data(), blob.size(),
                                                    dispatch_get_main_queue(),
                                                    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError* error = nil;
        return [device newLibraryWithData:data error:&error];
    }

    id<MTLRenderPipelineState> m_renderState = nil;
    id<MTLComputePipelineState> m_computeState = nil;
    MTLPrimitiveType m_topology = MTLPrimitiveTypeTriangle;
    bool m_compute = false;
};

class MetalSwapchain final : public ISwapchain {
public:
    MetalSwapchain(id<MTLDevice> device, const SwapchainDesc& desc)
        : m_width(desc.width), m_height(desc.height) {
        if (desc.nativeWindowHandle != nullptr) {
            m_layer = (__bridge CAMetalLayer*)desc.nativeWindowHandle;
            m_layer.device = device;
            m_layer.pixelFormat = toMtlFormat(desc.colorFormat);
            m_layer.drawableSize = CGSizeMake(desc.width, desc.height);
        }
    }

    ITexture* acquireNextImage() override {
        if (m_layer == nil) {
            return nullptr;
        }
        m_drawable = [m_layer nextDrawable];
        if (m_drawable == nil) {
            return nullptr;
        }
        m_backbuffer = std::make_unique<MetalTexture>(m_drawable.texture);
        return m_backbuffer.get();
    }

    void present() override {
        if (m_drawable != nil) {
            [m_drawable present];
            m_drawable = nil;
        }
    }

    void resize(std::uint32_t width, std::uint32_t height) override {
        m_width = width;
        m_height = height;
        if (m_layer != nil) {
            m_layer.drawableSize = CGSizeMake(width, height);
        }
    }

    std::uint32_t width() const override { return m_width; }
    std::uint32_t height() const override { return m_height; }

private:
    CAMetalLayer* m_layer = nil;
    id<CAMetalDrawable> m_drawable = nil;
    std::unique_ptr<MetalTexture> m_backbuffer;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
};

class MetalCommandEncoder final : public ICommandEncoder {
public:
    explicit MetalCommandEncoder(id<MTLCommandQueue> queue) {
        m_commandBuffer = [queue commandBuffer];
    }

    void beginRenderPass(ITexture* colorTarget, ITexture* depthTarget, Vec4 clearColor) override {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        if (colorTarget != nullptr) {
            auto* mt = static_cast<MetalTexture*>(colorTarget);
            pass.colorAttachments[0].texture = mt->handle();
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor =
                MTLClearColorMake(clearColor.x, clearColor.y, clearColor.z, clearColor.w);
        }
        if (depthTarget != nullptr) {
            auto* dt = static_cast<MetalTexture*>(depthTarget);
            pass.depthAttachment.texture = dt->handle();
            pass.depthAttachment.loadAction = MTLLoadActionClear;
            pass.depthAttachment.clearDepth = 1.0;
        }
        m_renderEncoder = [m_commandBuffer renderCommandEncoderWithDescriptor:pass];
    }

    void endRenderPass() override {
        if (m_renderEncoder != nil) {
            [m_renderEncoder endEncoding];
            m_renderEncoder = nil;
        }
    }

    void setViewport(const Viewport& vp) override {
        MTLViewport v{vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth};
        [m_renderEncoder setViewport:v];
    }

    void setPipeline(IPipeline& pipeline) override {
        auto& mp = static_cast<MetalPipeline&>(pipeline);
        m_topology = mp.topology();
        if (mp.isCompute()) {
            m_computeEncoder = [m_commandBuffer computeCommandEncoder];
            [m_computeEncoder setComputePipelineState:mp.computeState()];
        } else if (m_renderEncoder != nil) {
            [m_renderEncoder setRenderPipelineState:mp.renderState()];
        }
    }

    void setVertexBuffer(IBuffer& buffer, std::uint32_t slot) override {
        auto& mb = static_cast<MetalBuffer&>(buffer);
        [m_renderEncoder setVertexBuffer:mb.handle() offset:0 atIndex:slot];
    }

    void setIndexBuffer(IBuffer& buffer, IndexType type) override {
        m_indexBuffer = static_cast<MetalBuffer&>(buffer).handle();
        m_indexType = type == IndexType::Uint16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
    }

    void setPushConstants(std::span<const std::uint8_t> bytes) override {
        [m_renderEncoder setVertexBytes:bytes.data() length:bytes.size() atIndex:16];
        [m_renderEncoder setFragmentBytes:bytes.data() length:bytes.size() atIndex:16];
    }

    void draw(std::uint32_t vertexCount, std::uint32_t instanceCount) override {
        [m_renderEncoder drawPrimitives:m_topology
                            vertexStart:0
                            vertexCount:vertexCount
                          instanceCount:instanceCount];
    }

    void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount) override {
        [m_renderEncoder drawIndexedPrimitives:m_topology
                                    indexCount:indexCount
                                     indexType:m_indexType
                                   indexBuffer:m_indexBuffer
                             indexBufferOffset:0
                                 instanceCount:instanceCount];
    }

    void dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) override {
        const MTLSize groups = MTLSizeMake(x, y, z);
        const MTLSize threads = MTLSizeMake(8, 8, 1);
        [m_computeEncoder dispatchThreadgroups:groups threadsPerThreadgroup:threads];
    }

    id<MTLCommandBuffer> commandBuffer() const { return m_commandBuffer; }
    void finalizeEncoding() {
        if (m_computeEncoder != nil) {
            [m_computeEncoder endEncoding];
            m_computeEncoder = nil;
        }
        endRenderPass();
    }

private:
    id<MTLCommandBuffer> m_commandBuffer = nil;
    id<MTLRenderCommandEncoder> m_renderEncoder = nil;
    id<MTLComputeCommandEncoder> m_computeEncoder = nil;
    id<MTLBuffer> m_indexBuffer = nil;
    MTLIndexType m_indexType = MTLIndexTypeUInt32;
    MTLPrimitiveType m_topology = MTLPrimitiveTypeTriangle;
};

class MetalDevice final : public IRhiDevice {
public:
    explicit MetalDevice(const RhiDeviceDesc&) {
        m_device = MTLCreateSystemDefaultDevice();
        if (m_device == nil) {
            throw std::runtime_error("metal: no default device");
        }
        m_queue = [m_device newCommandQueue];
        m_adapterName = std::string([m_device.name UTF8String]);
    }

    RhiBackend backend() const override { return RhiBackend::Metal; }
    std::string adapterName() const override { return m_adapterName; }

    std::unique_ptr<IBuffer> createBuffer(const BufferDesc& desc) override {
        return std::make_unique<MetalBuffer>(m_device, desc);
    }
    std::unique_ptr<ITexture> createTexture(const TextureDesc& desc) override {
        return std::make_unique<MetalTexture>(m_device, desc);
    }
    std::unique_ptr<IPipeline> createPipeline(const PipelineDesc& desc) override {
        return std::make_unique<MetalPipeline>(m_device, desc);
    }
    std::unique_ptr<ISwapchain> createSwapchain(const SwapchainDesc& desc) override {
        return std::make_unique<MetalSwapchain>(m_device, desc);
    }
    std::unique_ptr<ICommandEncoder> createCommandEncoder() override {
        return std::make_unique<MetalCommandEncoder>(m_queue);
    }

    void submit(ICommandEncoder& encoder) override {
        auto& me = static_cast<MetalCommandEncoder&>(encoder);
        me.finalizeEncoding();
        [me.commandBuffer() commit];
    }

    void waitIdle() override {
        id<MTLCommandBuffer> cb = [m_queue commandBuffer];
        [cb commit];
        [cb waitUntilCompleted];
    }

private:
    id<MTLDevice> m_device = nil;
    id<MTLCommandQueue> m_queue = nil;
    std::string m_adapterName;
};

}  // namespace

std::unique_ptr<IRhiDevice> makeMetalDevice(const RhiDeviceDesc& desc) {
    @autoreleasepool {
        try {
            return std::make_unique<MetalDevice>(desc);
        } catch (const std::exception&) {
            return nullptr;  // no Metal device present
        }
    }
}

}  // namespace cyber::render
