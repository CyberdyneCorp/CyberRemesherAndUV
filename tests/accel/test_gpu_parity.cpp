#include <doctest.h>

#include <cstddef>
#include <random>
#include <vector>

#include "cyber/accel/backend.hpp"
#include "cyber/accel/primitives.hpp"

// Backend parity harness (compute-acceleration spec: "Every primitive SHALL
// have automated parity tests running the CPU backend against each available
// GPU backend on randomized inputs, asserting agreement within documented
// per-primitive tolerances").
//
// On cpu-headless the only backend is CPU, so this reduces to a self-check of
// the reference primitives. On a GPU preset (CYBER_ENABLE_CUDA/OPENCL) the
// enabled backend is compared element-for-element against the CPU reference.
namespace accel = cyber::accel;

namespace {

// Per-primitive absolute tolerance. GPU float ops reassociate, so exact
// equality is not required; 1e-3 comfortably covers f32 rounding at these
// sizes while catching real divergence.
constexpr float kTolerance = 1e-3f;

accel::SparseMatrix randomCsr(std::size_t rows, std::mt19937& rng) {
    std::uniform_int_distribution<int> perRow(1, 5);
    std::uniform_int_distribution<std::size_t> col(0, rows - 1);
    std::uniform_real_distribution<float> val(-1.0f, 1.0f);
    accel::SparseMatrix a;
    a.rows = rows;
    a.rowStart.push_back(0);
    for (std::size_t r = 0; r < rows; ++r) {
        const int nnz = perRow(rng);
        for (int k = 0; k < nnz; ++k) {
            a.colIndex.push_back(col(rng));
            a.value.push_back(val(rng));
        }
        a.rowStart.push_back(a.colIndex.size());
    }
    return a;
}

}  // namespace

TEST_CASE("every available backend matches the CPU reference (parity)") {
    const auto backends = accel::availableBackends();
    REQUIRE(!backends.empty());
    const auto& cpu = backends.back();
    REQUIRE(cpu->kind() == accel::BackendKind::Cpu);

    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    constexpr std::size_t n = 4096;

    std::vector<float> x(n), y(n);
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = dist(rng);
        y[i] = dist(rng);
    }
    const accel::SparseMatrix a = randomCsr(n, rng);
    std::vector<float> xs(n);
    for (std::size_t i = 0; i < n; ++i) {
        xs[i] = dist(rng);
    }

    // CPU reference results.
    std::vector<float> refAxpy = y;
    cpu->axpy(2.5f, x.data(), refAxpy.data(), n);
    const float refDot = cpu->dot(x.data(), y.data(), n);
    std::vector<float> refSpmv(n);
    cpu->spmvCsr(a.rows, a.rowStart.data(), a.colIndex.data(), a.value.data(), xs.data(),
                 refSpmv.data());

    for (const auto& backend : backends) {
        CAPTURE(backend->deviceName());

        std::vector<float> axpyOut = y;
        backend->axpy(2.5f, x.data(), axpyOut.data(), n);
        for (std::size_t i = 0; i < n; ++i) {
            REQUIRE(axpyOut[i] == doctest::Approx(refAxpy[i]).epsilon(kTolerance));
        }

        const float d = backend->dot(x.data(), y.data(), n);
        REQUIRE(d == doctest::Approx(refDot).epsilon(kTolerance));

        std::vector<float> spmvOut(n);
        backend->spmvCsr(a.rows, a.rowStart.data(), a.colIndex.data(), a.value.data(), xs.data(),
                         spmvOut.data());
        for (std::size_t i = 0; i < n; ++i) {
            REQUIRE(spmvOut[i] == doctest::Approx(refSpmv[i]).epsilon(kTolerance));
        }
    }
}

TEST_CASE("backend enumeration is CPU-terminated and non-empty") {
    const auto backends = accel::availableBackends();
    REQUIRE(!backends.empty());
    REQUIRE(backends.back()->kind() == accel::BackendKind::Cpu);
    // Every backend reports a non-empty device name for the selection UI.
    for (const auto& backend : backends) {
        REQUIRE(!backend->deviceName().empty());
    }
}

TEST_CASE("selectBackend honours the request and falls back to CPU") {
    // CPU is always selectable.
    const auto cpu = accel::selectBackend(accel::BackendKind::Cpu);
    REQUIRE(cpu != nullptr);
    REQUIRE(cpu->kind() == accel::BackendKind::Cpu);

    // Selecting any kind never returns null; an absent GPU degrades to CPU.
    for (const auto kind : {accel::BackendKind::Metal, accel::BackendKind::Cuda,
                            accel::BackendKind::OpenCl}) {
        const auto backend = accel::selectBackend(kind);
        REQUIRE(backend != nullptr);
        // Either the requested GPU kind (if present) or the CPU fallback.
        REQUIRE((backend->kind() == kind || backend->kind() == accel::BackendKind::Cpu));
    }
}
