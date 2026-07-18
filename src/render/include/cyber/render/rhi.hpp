#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "cyber/core/math.hpp"

// Render Hardware Interface (viewport-rendering spec, task 7.1). A thin,
// backend-agnostic abstraction over a modern explicit graphics API. The
// interfaces are pure C++ and compile everywhere; the concrete Metal and
// Vulkan devices live in isolated best-effort translation units that are only
// linked when their SDK/hardware is present. On a host without a compiled-in
// backend, createRhiDevice() returns null and the caller falls back to the
// headless CPU preview path.
namespace cyber::render {

// ---- backends ---------------------------------------------------------------

enum class RhiBackend : std::uint8_t {
    Metal,
    Vulkan,
};

[[nodiscard]] std::string toString(RhiBackend backend);

// Backends that were compiled into this build (a subset of the enum). Empty in
// a headless CI build with no graphics SDK.
[[nodiscard]] std::vector<RhiBackend> availableBackends();
[[nodiscard]] bool isBackendAvailable(RhiBackend backend);

// ---- resource description ---------------------------------------------------

enum class TextureFormat : std::uint8_t {
    RGBA8Unorm,
    BGRA8Unorm,
    RGBA16Float,
    R32Uint,
    Depth32Float,
    Depth24Stencil8,
};

enum class BufferUsage : std::uint32_t {
    None = 0,
    Vertex = 1u << 0,
    Index = 1u << 1,
    Uniform = 1u << 2,
    Storage = 1u << 3,
    Indirect = 1u << 4,
    CopySrc = 1u << 5,
    CopyDst = 1u << 6,
};

[[nodiscard]] constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr bool hasUsage(BufferUsage set, BufferUsage flag) {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(flag)) != 0u;
}

enum class MemoryLocation : std::uint8_t {
    DeviceLocal,  // fastest for the GPU, not host-visible
    HostVisible,  // upload heap, mappable
};

enum class PrimitiveTopology : std::uint8_t {
    TriangleList,
    TriangleStrip,
    LineList,
    PointList,
};

enum class IndexType : std::uint8_t {
    Uint16,
    Uint32,
};

struct BufferDesc {
    std::uint64_t sizeBytes = 0;
    BufferUsage usage = BufferUsage::None;
    MemoryLocation memory = MemoryLocation::DeviceLocal;
    std::string debugName;
};

struct TextureDesc {
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t mipLevels = 1;
    std::uint32_t sampleCount = 1;
    TextureFormat format = TextureFormat::RGBA8Unorm;
    bool renderTarget = false;
    std::string debugName;
};

struct SwapchainDesc {
    // Opaque, platform-native surface handle (NSWindow*/CAMetalLayer* on Metal,
    // VkSurfaceKHR on Vulkan). Left null for offscreen headless rendering.
    void* nativeWindowHandle = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TextureFormat colorFormat = TextureFormat::BGRA8Unorm;
    std::uint32_t imageCount = 3;
    bool vsync = true;
};

struct PipelineDesc {
    // Backend-native shader blobs (Metal library / SPIR-V). Kept as bytes so the
    // interface stays API-neutral; the loader is a caller concern.
    std::vector<std::uint8_t> vertexShader;
    std::vector<std::uint8_t> fragmentShader;
    std::vector<std::uint8_t> computeShader;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    TextureFormat colorFormat = TextureFormat::BGRA8Unorm;
    TextureFormat depthFormat = TextureFormat::Depth32Float;
    bool depthTest = true;
    bool depthWrite = true;
    bool wireframe = false;
    std::string debugName;
};

struct Viewport {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float minDepth = 0.0f;
    float maxDepth = 1.0f;
};

// ---- resource interfaces ----------------------------------------------------

class IBuffer {
public:
    virtual ~IBuffer() = default;
    [[nodiscard]] virtual std::uint64_t sizeBytes() const = 0;
    // Uploads `bytes` at `offset`; only valid for HostVisible buffers.
    virtual void update(std::span<const std::uint8_t> bytes, std::uint64_t offset = 0) = 0;
};

class ITexture {
public:
    virtual ~ITexture() = default;
    [[nodiscard]] virtual std::uint32_t width() const = 0;
    [[nodiscard]] virtual std::uint32_t height() const = 0;
    [[nodiscard]] virtual TextureFormat format() const = 0;
};

class IPipeline {
public:
    virtual ~IPipeline() = default;
    [[nodiscard]] virtual bool isCompute() const = 0;
};

class ISwapchain {
public:
    virtual ~ISwapchain() = default;
    // Acquires the next backbuffer; may be null if the surface is lost/resizing.
    [[nodiscard]] virtual ITexture* acquireNextImage() = 0;
    virtual void present() = 0;
    virtual void resize(std::uint32_t width, std::uint32_t height) = 0;
    [[nodiscard]] virtual std::uint32_t width() const = 0;
    [[nodiscard]] virtual std::uint32_t height() const = 0;
};

// Records GPU work for one submission. Obtained per-frame from the device and
// submitted via IRhiDevice::submit().
class ICommandEncoder {
public:
    virtual ~ICommandEncoder() = default;

    // ---- render pass ----
    virtual void beginRenderPass(ITexture* colorTarget, ITexture* depthTarget, Vec4 clearColor) = 0;
    virtual void endRenderPass() = 0;
    virtual void setViewport(const Viewport& viewport) = 0;
    virtual void setPipeline(IPipeline& pipeline) = 0;
    virtual void setVertexBuffer(IBuffer& buffer, std::uint32_t slot = 0) = 0;
    virtual void setIndexBuffer(IBuffer& buffer, IndexType type) = 0;
    // 64 bytes of inline constants (view/projection, tint, brush radius, ...).
    virtual void setPushConstants(std::span<const std::uint8_t> bytes) = 0;
    virtual void draw(std::uint32_t vertexCount, std::uint32_t instanceCount = 1) = 0;
    virtual void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount = 1) = 0;

    // ---- compute pass ----
    virtual void dispatch(std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ) = 0;
};

class IRhiDevice {
public:
    virtual ~IRhiDevice() = default;

    [[nodiscard]] virtual RhiBackend backend() const = 0;
    [[nodiscard]] virtual std::string adapterName() const = 0;

    [[nodiscard]] virtual std::unique_ptr<IBuffer> createBuffer(const BufferDesc& desc) = 0;
    [[nodiscard]] virtual std::unique_ptr<ITexture> createTexture(const TextureDesc& desc) = 0;
    [[nodiscard]] virtual std::unique_ptr<IPipeline> createPipeline(const PipelineDesc& desc) = 0;
    [[nodiscard]] virtual std::unique_ptr<ISwapchain> createSwapchain(const SwapchainDesc& desc) = 0;

    // A fresh command encoder for the current frame.
    [[nodiscard]] virtual std::unique_ptr<ICommandEncoder> createCommandEncoder() = 0;
    // Submits recorded work and blocks callers that need CPU/GPU sync.
    virtual void submit(ICommandEncoder& encoder) = 0;
    virtual void waitIdle() = 0;
};

struct RhiDeviceDesc {
    bool enableValidation = false;  // API debug/validation layers
    bool preferHighPerformanceAdapter = true;
    std::string applicationName = "CyberRemesher";
};

// Creates a device for `backend`, or null when that backend was not compiled
// into this build or no compatible adapter exists. Never throws.
[[nodiscard]] std::unique_ptr<IRhiDevice> createRhiDevice(RhiBackend backend,
                                                          const RhiDeviceDesc& desc = {});

}  // namespace cyber::render
