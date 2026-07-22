#include "cyber/render/rhi.hpp"

#include <algorithm>

namespace cyber::render {

// Backend makers are defined in their own best-effort translation units and
// only declared/linked when the matching SDK is compiled in. Each returns null
// when no compatible device exists, so the interface never forces a GPU.
#ifdef CYBER_RENDER_HAS_METAL
std::unique_ptr<IRhiDevice> makeMetalDevice(const RhiDeviceDesc& desc);
#endif
#ifdef CYBER_RENDER_HAS_VULKAN
std::unique_ptr<IRhiDevice> makeVulkanDevice(const RhiDeviceDesc& desc);
#endif

std::string toString(RhiBackend backend) {
    switch (backend) {
        case RhiBackend::Metal:
            return "Metal";
        case RhiBackend::Vulkan:
            return "Vulkan";
    }
    return "Unknown";
}

std::vector<RhiBackend> availableBackends() {
    std::vector<RhiBackend> backends;
#ifdef CYBER_RENDER_HAS_METAL
    backends.push_back(RhiBackend::Metal);
#endif
#ifdef CYBER_RENDER_HAS_VULKAN
    backends.push_back(RhiBackend::Vulkan);
#endif
    return backends;
}

bool isBackendAvailable(RhiBackend backend) {
    for (const RhiBackend b : availableBackends()) {
        if (b == backend) {
            return true;
        }
    }
    return false;
}

std::unique_ptr<IRhiDevice> createRhiDevice([[maybe_unused]] RhiBackend backend,
                                            [[maybe_unused]] const RhiDeviceDesc& desc) {
    switch (backend) {
        case RhiBackend::Metal:
#ifdef CYBER_RENDER_HAS_METAL
            return makeMetalDevice(desc);
#else
            return nullptr;
#endif
        case RhiBackend::Vulkan:
#ifdef CYBER_RENDER_HAS_VULKAN
            return makeVulkanDevice(desc);
#else
            return nullptr;
#endif
    }
    return nullptr;
}

}  // namespace cyber::render
