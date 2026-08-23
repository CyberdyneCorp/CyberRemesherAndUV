#include <cstdlib>
#include <cstring>
#include <mutex>

#include "cyber/accel/backend.hpp"
#include "cyber/accel/detail/fallback_report.hpp"

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

namespace {

std::vector<std::shared_ptr<IBackend>> probeBackends() {
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

std::mutex g_defaultMutex;
std::shared_ptr<IBackend> g_default;

// Probing is expensive — makeOpenClBackend creates a context and compiles its
// kernel source (~100 ms measured) — and the backends are stateless handles, so
// the list is built once and shared. Callers then also see the SAME instance
// from availableBackends(), selectBackend() and defaultBackend().
const std::vector<std::shared_ptr<IBackend>>& backendList() {
    static const std::vector<std::shared_ptr<IBackend>> backends = probeBackends();
    return backends;
}

// CYBER_BACKEND=cpu|cuda|metal|opencl: the support escape hatch for a consumer
// of a GPU build that cannot rebuild the library (the installed artifact
// exposes no C++ selection API). An unset or unrecognized value keeps the
// automatic best-first choice.
std::shared_ptr<IBackend> backendFromEnvironment() {
    const char* name = std::getenv("CYBER_BACKEND");
    if (name == nullptr) {
        return nullptr;
    }
    struct Named {
        const char* name;
        BackendKind kind;
    };
    for (const Named& entry :
         {Named{"cpu", BackendKind::Cpu}, Named{"metal", BackendKind::Metal},
          Named{"cuda", BackendKind::Cuda}, Named{"opencl", BackendKind::OpenCl}}) {
        if (std::strcmp(name, entry.name) == 0) {
            return selectBackend(entry.kind);
        }
    }
    return nullptr;
}

}  // namespace

std::vector<std::shared_ptr<IBackend>> availableBackends() { return backendList(); }

std::size_t gpuFallbackCount() { return detail::fallbackEventCount(); }

std::vector<BackendKind> compiledBackendKinds() {
    std::vector<BackendKind> kinds;
#if defined(CYBER_HAS_METAL)
    kinds.push_back(BackendKind::Metal);
#endif
#if defined(CYBER_HAS_CUDA)
    kinds.push_back(BackendKind::Cuda);
#endif
#if defined(CYBER_HAS_OPENCL)
    kinds.push_back(BackendKind::OpenCl);
#endif
    return kinds;
}

std::shared_ptr<IBackend> defaultBackend() {
    const std::lock_guard<std::mutex> lock(g_defaultMutex);
    if (!g_default) {
        g_default = backendFromEnvironment();
    }
    if (!g_default) {
        g_default = backendList().front();
    }
    return g_default;
}

void setDefaultBackend(std::shared_ptr<IBackend> backend) {
    const std::lock_guard<std::mutex> lock(g_defaultMutex);
    g_default = std::move(backend);
}

std::shared_ptr<IBackend> selectBackend(BackendKind kind) {
    const auto& backends = backendList();
    for (const auto& backend : backends) {
        if (backend->kind() == kind) {
            return backend;
        }
    }
    return backends.back();  // CPU is always present and always last
}

}  // namespace cyber::accel
