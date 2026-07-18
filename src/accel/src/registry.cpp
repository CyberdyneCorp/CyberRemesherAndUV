#include <mutex>

#include "cyber/accel/backend.hpp"

namespace cyber::accel {

std::shared_ptr<IBackend> makeCpuBackend();
#if defined(CYBER_HAS_METAL)
std::shared_ptr<IBackend> makeMetalBackend();
#endif
#if defined(CYBER_HAS_CUDA)
std::shared_ptr<IBackend> makeCudaBackend();
#endif
#if defined(CYBER_HAS_OPENCL)
std::shared_ptr<IBackend> makeOpenClBackend();
#endif

std::vector<std::shared_ptr<IBackend>> availableBackends() {
    std::vector<std::shared_ptr<IBackend>> backends;
    // Documented priority: tier-1 Metal/CUDA first, tier-2 OpenCL next, CPU
    // always last and always present. Each maker returns null when its device
    // is absent, so an enabled-but-unavailable backend is simply skipped
    // (graceful degradation — compute-acceleration spec).
    [[maybe_unused]] auto add = [&backends](const std::shared_ptr<IBackend>& backend) {
        if (backend) {
            backends.push_back(backend);
        }
    };
#if defined(CYBER_HAS_METAL)
    add(makeMetalBackend());
#endif
#if defined(CYBER_HAS_CUDA)
    add(makeCudaBackend());
#endif
#if defined(CYBER_HAS_OPENCL)
    add(makeOpenClBackend());
#endif
    backends.push_back(makeCpuBackend());
    return backends;
}

namespace {
std::mutex g_defaultMutex;
std::shared_ptr<IBackend> g_default;
}  // namespace

std::shared_ptr<IBackend> defaultBackend() {
    const std::lock_guard<std::mutex> lock(g_defaultMutex);
    if (!g_default) {
        g_default = availableBackends().front();
    }
    return g_default;
}

void setDefaultBackend(std::shared_ptr<IBackend> backend) {
    const std::lock_guard<std::mutex> lock(g_defaultMutex);
    g_default = std::move(backend);
}

std::shared_ptr<IBackend> selectBackend(BackendKind kind) {
    const auto backends = availableBackends();
    for (const auto& backend : backends) {
        if (backend->kind() == kind) {
            return backend;
        }
    }
    return backends.back();  // CPU is always present and always last
}

}  // namespace cyber::accel
