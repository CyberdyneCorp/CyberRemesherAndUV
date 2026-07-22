// UNVERIFIED: This Vulkan backend requires the Vulkan SDK and a real GPU +
// windowing system to build and run. It is not exercised in headless Linux CI
// (no VkInstance/loader, no swapchain surface). The code follows the documented
// Vulkan 1.2 API and compiles in an isolated target WITHOUT the project's
// strict warning set (vendor headers do not survive -Werror -Wold-style-cast).
// Treat every call site here as unverified until run on hardware.

#include <vulkan/vulkan.h>

#include <cstring>
#include <stdexcept>
#include <vector>

#include "cyber/render/rhi.hpp"

namespace cyber::render {
namespace {

VkFormat toVkFormat(TextureFormat format) {
    switch (format) {
        case TextureFormat::RGBA8Unorm:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::BGRA8Unorm:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case TextureFormat::RGBA16Float:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case TextureFormat::R32Uint:
            return VK_FORMAT_R32_UINT;
        case TextureFormat::Depth32Float:
            return VK_FORMAT_D32_SFLOAT;
        case TextureFormat::Depth24Stencil8:
            return VK_FORMAT_D24_UNORM_S8_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}

VkBufferUsageFlags toVkBufferUsage(BufferUsage usage) {
    VkBufferUsageFlags flags = 0;
    if (hasUsage(usage, BufferUsage::Vertex)) flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (hasUsage(usage, BufferUsage::Index)) flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (hasUsage(usage, BufferUsage::Uniform)) flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (hasUsage(usage, BufferUsage::Storage)) flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (hasUsage(usage, BufferUsage::Indirect)) flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (hasUsage(usage, BufferUsage::CopySrc)) flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (hasUsage(usage, BufferUsage::CopyDst)) flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return flags;
}

VkPrimitiveTopology toVkTopology(PrimitiveTopology topology) {
    switch (topology) {
        case PrimitiveTopology::TriangleList:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case PrimitiveTopology::TriangleStrip:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case PrimitiveTopology::LineList:
            return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case PrimitiveTopology::PointList:
            return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

class VulkanDevice;

std::uint32_t findMemoryType(VkPhysicalDevice physical, std::uint32_t typeBits,
                             VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physical, &memProps);
    for (std::uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        const bool typeOk = (typeBits & (1u << i)) != 0u;
        const bool propOk = (memProps.memoryTypes[i].propertyFlags & properties) == properties;
        if (typeOk && propOk) {
            return i;
        }
    }
    throw std::runtime_error("vulkan: no compatible memory type");
}

class VulkanBuffer final : public IBuffer {
public:
    VulkanBuffer(VkDevice device, VkPhysicalDevice physical, const BufferDesc& desc)
        : m_device(device), m_size(desc.sizeBytes) {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = desc.sizeBytes;
        info.usage = toVkBufferUsage(desc.usage);
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_device, &info, nullptr, &m_buffer) != VK_SUCCESS) {
            throw std::runtime_error("vulkan: vkCreateBuffer failed");
        }

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(m_device, m_buffer, &req);
        const VkMemoryPropertyFlags props =
            desc.memory == MemoryLocation::HostVisible
                ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        m_hostVisible = desc.memory == MemoryLocation::HostVisible;

        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = findMemoryType(physical, req.memoryTypeBits, props);
        if (vkAllocateMemory(m_device, &alloc, nullptr, &m_memory) != VK_SUCCESS) {
            throw std::runtime_error("vulkan: vkAllocateMemory failed");
        }
        vkBindBufferMemory(m_device, m_buffer, m_memory, 0);
    }

    ~VulkanBuffer() override {
        if (m_buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, m_buffer, nullptr);
        if (m_memory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_memory, nullptr);
    }

    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;

    std::uint64_t sizeBytes() const override { return m_size; }

    void update(std::span<const std::uint8_t> bytes, std::uint64_t offset) override {
        if (!m_hostVisible) {
            throw std::runtime_error("vulkan: update() on non-host-visible buffer");
        }
        void* mapped = nullptr;
        vkMapMemory(m_device, m_memory, offset, bytes.size(), 0, &mapped);
        std::memcpy(mapped, bytes.data(), bytes.size());
        vkUnmapMemory(m_device, m_memory);
    }

    VkBuffer handle() const { return m_buffer; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    std::uint64_t m_size = 0;
    bool m_hostVisible = false;
};

class VulkanTexture final : public ITexture {
public:
    VulkanTexture(VkDevice device, const TextureDesc& desc)
        : m_device(device), m_width(desc.width), m_height(desc.height), m_format(desc.format) {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = toVkFormat(desc.format);
        info.extent = {desc.width, desc.height, 1};
        info.mipLevels = desc.mipLevels;
        info.arrayLayers = 1;
        info.samples = static_cast<VkSampleCountFlagBits>(desc.sampleCount);
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                     (desc.renderTarget ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0u);
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &info, nullptr, &m_image) != VK_SUCCESS) {
            throw std::runtime_error("vulkan: vkCreateImage failed");
        }
    }

    ~VulkanTexture() override {
        if (m_owns && m_image != VK_NULL_HANDLE) vkDestroyImage(m_device, m_image, nullptr);
    }

    VulkanTexture(const VulkanTexture&) = delete;
    VulkanTexture& operator=(const VulkanTexture&) = delete;

    std::uint32_t width() const override { return m_width; }
    std::uint32_t height() const override { return m_height; }
    TextureFormat format() const override { return m_format; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkImage m_image = VK_NULL_HANDLE;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    TextureFormat m_format = TextureFormat::RGBA8Unorm;
    bool m_owns = true;
};

class VulkanPipeline final : public IPipeline {
public:
    VulkanPipeline(VkDevice device, const PipelineDesc& desc)
        : m_device(device), m_compute(!desc.computeShader.empty()) {
        // A full pipeline needs a render pass / dynamic rendering info and a
        // pipeline layout; those are elided here but the shader modules are
        // created from the provided SPIR-V so the structure is real.
        if (!desc.vertexShader.empty()) {
            m_vertexModule = createModule(desc.vertexShader);
        }
        if (!desc.fragmentShader.empty()) {
            m_fragmentModule = createModule(desc.fragmentShader);
        }
        if (!desc.computeShader.empty()) {
            m_computeModule = createModule(desc.computeShader);
        }
        m_topology = toVkTopology(desc.topology);
    }

    ~VulkanPipeline() override {
        for (VkShaderModule m : {m_vertexModule, m_fragmentModule, m_computeModule}) {
            if (m != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m, nullptr);
        }
        if (m_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_pipeline, nullptr);
    }

    VulkanPipeline(const VulkanPipeline&) = delete;
    VulkanPipeline& operator=(const VulkanPipeline&) = delete;

    bool isCompute() const override { return m_compute; }
    VkPipeline handle() const { return m_pipeline; }
    VkPrimitiveTopology topology() const { return m_topology; }

private:
    VkShaderModule createModule(const std::vector<std::uint8_t>& spirv) {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = spirv.size();
        info.pCode = reinterpret_cast<const std::uint32_t*>(spirv.data());
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &info, nullptr, &module) != VK_SUCCESS) {
            throw std::runtime_error("vulkan: vkCreateShaderModule failed");
        }
        return module;
    }

    VkDevice m_device = VK_NULL_HANDLE;
    VkShaderModule m_vertexModule = VK_NULL_HANDLE;
    VkShaderModule m_fragmentModule = VK_NULL_HANDLE;
    VkShaderModule m_computeModule = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPrimitiveTopology m_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    bool m_compute = false;
};

class VulkanSwapchain final : public ISwapchain {
public:
    VulkanSwapchain(VkDevice device, const SwapchainDesc& desc)
        : m_device(device), m_width(desc.width), m_height(desc.height) {
        // Surface creation is platform specific (VK_KHR_*_surface) and needs the
        // native window handle; the actual vkCreateSwapchainKHR call happens once
        // a VkSurfaceKHR is available. Left as configuration here.
    }

    ~VulkanSwapchain() override {
        if (m_swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    }

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    ITexture* acquireNextImage() override { return nullptr; }
    void present() override { /* vkQueuePresentKHR once a surface/queue exist */ }
    void resize(std::uint32_t width, std::uint32_t height) override {
        m_width = width;
        m_height = height;
    }
    std::uint32_t width() const override { return m_width; }
    std::uint32_t height() const override { return m_height; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
};

class VulkanCommandEncoder final : public ICommandEncoder {
public:
    VulkanCommandEncoder(VkDevice device, VkCommandPool pool) : m_device(device), m_pool(pool) {
        VkCommandBufferAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        info.commandPool = pool;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;
        vkAllocateCommandBuffers(m_device, &info, &m_cmd);

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(m_cmd, &begin);
    }

    ~VulkanCommandEncoder() override {
        if (m_cmd != VK_NULL_HANDLE) vkFreeCommandBuffers(m_device, m_pool, 1, &m_cmd);
    }

    VulkanCommandEncoder(const VulkanCommandEncoder&) = delete;
    VulkanCommandEncoder& operator=(const VulkanCommandEncoder&) = delete;

    void beginRenderPass(ITexture*, ITexture*, Vec4) override {
        // vkCmdBeginRendering (dynamic rendering) once attachments are resolved.
    }
    void endRenderPass() override { /* vkCmdEndRendering */ }

    void setViewport(const Viewport& vp) override {
        VkViewport v{vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth};
        vkCmdSetViewport(m_cmd, 0, 1, &v);
    }

    void setPipeline(IPipeline& pipeline) override {
        auto& vk = static_cast<VulkanPipeline&>(pipeline);
        if (vk.handle() != VK_NULL_HANDLE) {
            const VkPipelineBindPoint bind =
                vk.isCompute() ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
            vkCmdBindPipeline(m_cmd, bind, vk.handle());
        }
    }

    void setVertexBuffer(IBuffer& buffer, std::uint32_t slot) override {
        auto& vk = static_cast<VulkanBuffer&>(buffer);
        VkBuffer handle = vk.handle();
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(m_cmd, slot, 1, &handle, &offset);
    }

    void setIndexBuffer(IBuffer& buffer, IndexType type) override {
        auto& vk = static_cast<VulkanBuffer&>(buffer);
        vkCmdBindIndexBuffer(
            m_cmd, vk.handle(), 0,
            type == IndexType::Uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    }

    void setPushConstants(std::span<const std::uint8_t> bytes) override {
        m_pushConstants.assign(bytes.begin(), bytes.end());
        // vkCmdPushConstants needs the pipeline layout; recorded once bound.
    }

    void draw(std::uint32_t vertexCount, std::uint32_t instanceCount) override {
        vkCmdDraw(m_cmd, vertexCount, instanceCount, 0, 0);
    }

    void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount) override {
        vkCmdDrawIndexed(m_cmd, indexCount, instanceCount, 0, 0, 0);
    }

    void dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) override {
        vkCmdDispatch(m_cmd, x, y, z);
    }

    VkCommandBuffer handle() const { return m_cmd; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    std::vector<std::uint8_t> m_pushConstants;
};

class VulkanDevice final : public IRhiDevice {
public:
    explicit VulkanDevice(const RhiDeviceDesc& desc) {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = desc.applicationName.c_str();
        app.apiVersion = VK_API_VERSION_1_2;

        VkInstanceCreateInfo instInfo{};
        instInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instInfo.pApplicationInfo = &app;
        if (vkCreateInstance(&instInfo, nullptr, &m_instance) != VK_SUCCESS) {
            throw std::runtime_error("vulkan: vkCreateInstance failed");
        }

        std::uint32_t count = 0;
        vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
        if (count == 0) {
            throw std::runtime_error("vulkan: no physical device");
        }
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(m_instance, &count, devices.data());
        m_physical = pickAdapter(devices, desc.preferHighPerformanceAdapter);

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(m_physical, &props);
        m_adapterName = props.deviceName;
        m_graphicsQueueFamily = findGraphicsQueueFamily(m_physical);

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = m_graphicsQueueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        if (vkCreateDevice(m_physical, &deviceInfo, nullptr, &m_device) != VK_SUCCESS) {
            throw std::runtime_error("vulkan: vkCreateDevice failed");
        }
        vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_queue);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = m_graphicsQueueFamily;
        vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool);
    }

    ~VulkanDevice() override {
        if (m_device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_device);
            if (m_commandPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(m_device, m_commandPool, nullptr);
            }
            vkDestroyDevice(m_device, nullptr);
        }
        if (m_instance != VK_NULL_HANDLE) vkDestroyInstance(m_instance, nullptr);
    }

    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    RhiBackend backend() const override { return RhiBackend::Vulkan; }
    std::string adapterName() const override { return m_adapterName; }

    std::unique_ptr<IBuffer> createBuffer(const BufferDesc& desc) override {
        return std::make_unique<VulkanBuffer>(m_device, m_physical, desc);
    }
    std::unique_ptr<ITexture> createTexture(const TextureDesc& desc) override {
        return std::make_unique<VulkanTexture>(m_device, desc);
    }
    std::unique_ptr<IPipeline> createPipeline(const PipelineDesc& desc) override {
        return std::make_unique<VulkanPipeline>(m_device, desc);
    }
    std::unique_ptr<ISwapchain> createSwapchain(const SwapchainDesc& desc) override {
        return std::make_unique<VulkanSwapchain>(m_device, desc);
    }
    std::unique_ptr<ICommandEncoder> createCommandEncoder() override {
        return std::make_unique<VulkanCommandEncoder>(m_device, m_commandPool);
    }

    void submit(ICommandEncoder& encoder) override {
        auto& vk = static_cast<VulkanCommandEncoder&>(encoder);
        VkCommandBuffer cmd = vk.handle();
        vkEndCommandBuffer(cmd);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vkQueueSubmit(m_queue, 1, &submit, VK_NULL_HANDLE);
    }

    void waitIdle() override { vkDeviceWaitIdle(m_device); }

private:
    static VkPhysicalDevice pickAdapter(const std::vector<VkPhysicalDevice>& devices,
                                        bool preferDiscrete) {
        VkPhysicalDevice fallback = devices.front();
        for (VkPhysicalDevice d : devices) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(d, &props);
            if (preferDiscrete && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                return d;
            }
        }
        return fallback;
    }

    static std::uint32_t findGraphicsQueueFamily(VkPhysicalDevice physical) {
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        for (std::uint32_t i = 0; i < count; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) {
                return i;
            }
        }
        throw std::runtime_error("vulkan: no graphics queue family");
    }

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    std::uint32_t m_graphicsQueueFamily = 0;
    std::string m_adapterName;
};

}  // namespace

std::unique_ptr<IRhiDevice> makeVulkanDevice(const RhiDeviceDesc& desc) {
    try {
        return std::make_unique<VulkanDevice>(desc);
    } catch (const std::exception&) {
        return nullptr;  // no compatible Vulkan device present
    }
}

}  // namespace cyber::render
