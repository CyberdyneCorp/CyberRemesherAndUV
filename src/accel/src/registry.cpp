#include <mutex>

#include "cyber/accel/backend.hpp"

namespace cyber::accel {

std::shared_ptr<IBackend> makeCpuBackend();

std::vector<std::shared_ptr<IBackend>> availableBackends() {
    std::vector<std::shared_ptr<IBackend>> backends;
    // GPU backends register themselves here as task group 4 lands
    // (Metal 4.3, CUDA 4.4, OpenCL 4.5), ordered by the documented
    // priority. CPU is always last and always present.
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

}  // namespace cyber::accel
