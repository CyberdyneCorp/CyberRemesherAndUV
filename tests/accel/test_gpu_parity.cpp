#include <doctest.h>

#include <algorithm>
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
// On cpu-headless the only backend is CPU, so the comparison loop reduces to a
// self-check of the reference primitives — which is why the last case below
// reports that fact instead of letting the suite claim a parity it never
// measured.
namespace accel = cyber::accel;

namespace {

// Per-primitive absolute tolerance. GPU float ops reassociate, so exact
// equality is not required; 1e-3 comfortably covers f32 rounding at these
// sizes while catching real divergence.
constexpr float kTolerance = 1e-3f;

// Columns are drawn from [0, cols), independently of the row count: a CSR
// matrix is rectangular in general (the seamless solver's reduction operators
// are nUv x W and W x nUv), and a square-only generator cannot catch a backend
// that sizes the x vector by the row count.
accel::SparseMatrix randomCsr(std::size_t rows, std::size_t cols, std::mt19937& rng) {
    std::uniform_int_distribution<int> perRow(1, 5);
    std::uniform_int_distribution<std::size_t> col(0, cols - 1);
    std::uniform_real_distribution<float> val(-1.0f, 1.0f);
    accel::SparseMatrix a;
    a.rows = rows;
    a.cols = cols;
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

std::vector<float> randomVector(std::size_t n, std::mt19937& rng) {
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> v(n);
    for (float& e : v) {
        e = dist(rng);
    }
    return v;
}

// Records the shape every backend is handed, so the column plumbing can be
// asserted on a host-only build. Everything else is the CPU reference.
class RecordingBackend final : public accel::IBackend {
public:
    [[nodiscard]] accel::BackendKind kind() const override { return accel::BackendKind::Cpu; }
    [[nodiscard]] std::string deviceName() const override { return "recording"; }

    void spmvCsr(std::size_t rows, std::size_t cols, const std::size_t* rowStart,
                 const std::size_t* colIndex, const float* value, const float* x,
                 float* y) override {
        lastRows = rows;
        lastCols = cols;
        IBackend::spmvCsr(rows, cols, rowStart, colIndex, value, x, y);
    }

    std::size_t lastRows = 0;
    std::size_t lastCols = 0;
};

}  // namespace

TEST_CASE("every available backend matches the CPU reference (parity)") {
    const auto backends = accel::availableBackends();
    REQUIRE(!backends.empty());
    const auto& cpu = backends.back();
    REQUIRE(cpu->kind() == accel::BackendKind::Cpu);

    std::mt19937 rng(12345);
    constexpr std::size_t n = 4096;

    const std::vector<float> x = randomVector(n, rng);
    const std::vector<float> y = randomVector(n, rng);

    // Square, wide (rows < cols, the Tt shape) and tall (rows > cols, the Tuv
    // shape). A backend that stages `rows` floats of x reads past the device
    // buffer on the wide case and past the CALLER's host buffer on the tall one.
    struct Shape {
        const char* name;
        std::size_t rows;
        std::size_t cols;
    };
    const Shape shapes[] = {{"square", n, n}, {"wide", 8, n}, {"tall", n, 8}};

    // CPU reference results.
    std::vector<float> refAxpy = y;
    cpu->axpy(2.5f, x.data(), refAxpy.data(), n);
    const float refDot = cpu->dot(x.data(), y.data(), n);

    for (const auto& backend : backends) {
        CAPTURE(backend->deviceName());

        std::vector<float> axpyOut = y;
        backend->axpy(2.5f, x.data(), axpyOut.data(), n);
        for (std::size_t i = 0; i < n; ++i) {
            REQUIRE(axpyOut[i] == doctest::Approx(refAxpy[i]).epsilon(kTolerance));
        }

        const float d = backend->dot(x.data(), y.data(), n);
        REQUIRE(d == doctest::Approx(refDot).epsilon(kTolerance));
    }

    for (const Shape& shape : shapes) {
        CAPTURE(std::string(shape.name));
        const accel::SparseMatrix a = randomCsr(shape.rows, shape.cols, rng);
        const std::vector<float> xs = randomVector(shape.cols, rng);
        std::vector<float> ref(shape.rows);
        cpu->spmvCsr(a.rows, a.cols, a.rowStart.data(), a.colIndex.data(), a.value.data(),
                     xs.data(), ref.data());
        for (const auto& backend : backends) {
            CAPTURE(backend->deviceName());
            std::vector<float> out(shape.rows);
            backend->spmvCsr(a.rows, a.cols, a.rowStart.data(), a.colIndex.data(), a.value.data(),
                             xs.data(), out.data());
            for (std::size_t i = 0; i < shape.rows; ++i) {
                REQUIRE(out[i] == doctest::Approx(ref[i]).epsilon(kTolerance));
            }
        }
    }
}

TEST_CASE("spmv hands the backend the column count, not the row count") {
    // Regression: every GPU backend sized and uploaded x by `rows`, so a
    // rectangular matrix — the seamless solver's Tuv (nUv x W) and Tt
    // (W x nUv) — either indexed past the device copy (wide) or over-read the
    // caller's host x (tall). The column count is the only bound available, so
    // the primitive has to carry it.
    RecordingBackend backend;

    accel::SparseMatrix wide;  // 2 x 5
    wide.rows = 2;
    wide.cols = 5;
    wide.rowStart = {0, 2, 3};
    wide.colIndex = {0, 4, 3};
    wide.value = {1.0f, 2.0f, 3.0f};
    accel::Buffer<float> x{1, 2, 3, 4, 5};
    accel::Buffer<float> y;
    accel::spmv(backend, wide, x, y);
    REQUIRE(backend.lastRows == 2);
    REQUIRE(backend.lastCols == 5);
    REQUIRE(y.host() == std::vector<float>{1.0f * 1 + 2.0f * 5, 3.0f * 4});

    accel::SparseMatrix tall;  // 5 x 2
    tall.rows = 5;
    tall.cols = 2;
    tall.rowStart = {0, 1, 2, 3, 4, 5};
    tall.colIndex = {0, 1, 0, 1, 0};
    tall.value = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    accel::Buffer<float> xs{7, 9};
    accel::spmv(backend, tall, xs, y);
    REQUIRE(backend.lastRows == 5);
    // The caller's x is only 2 long: a backend told "5" would read past it.
    REQUIRE(backend.lastCols == 2);
    REQUIRE(y.host() == std::vector<float>{7, 9, 7, 9, 7});
}

TEST_CASE("spmv never lets a backend read past the caller's x buffer") {
    // A matrix left without an explicit column count must not make the backend
    // stage more of x than the caller actually provided.
    RecordingBackend backend;
    accel::SparseMatrix a;
    a.rows = 4;
    a.cols = 0;  // unset
    a.rowStart = {0, 1, 2, 3, 4};
    a.colIndex = {0, 1, 0, 1};
    a.value = {1.0f, 1.0f, 1.0f, 1.0f};
    accel::Buffer<float> x{3, 4};
    accel::Buffer<float> y;
    accel::spmv(backend, a, x, y);
    REQUIRE(backend.lastCols == x.size());

    a.cols = 1000;  // larger than x: still bounded by the buffer
    accel::spmv(backend, a, x, y);
    REQUIRE(backend.lastCols == x.size());
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

TEST_CASE("backend enumeration is built once and shared") {
    // Probing constructs a context and compiles kernel source per GPU backend
    // (~100 ms measured for OpenCL); repeating that per call made selectBackend
    // unusable as the documented per-operation override, and handed out a
    // different instance than defaultBackend().
    const auto first = accel::availableBackends();
    const auto second = accel::availableBackends();
    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        REQUIRE(first[i].get() == second[i].get());
    }
    REQUIRE(accel::selectBackend(accel::BackendKind::Cpu).get() == first.back().get());
}

TEST_CASE("selectBackend honours the request and falls back to CPU") {
    // CPU is always selectable.
    const auto cpu = accel::selectBackend(accel::BackendKind::Cpu);
    REQUIRE(cpu != nullptr);
    REQUIRE(cpu->kind() == accel::BackendKind::Cpu);

    // Selecting any kind never returns null; an absent GPU degrades to CPU.
    for (const auto kind :
         {accel::BackendKind::Metal, accel::BackendKind::Cuda, accel::BackendKind::OpenCl}) {
        const auto backend = accel::selectBackend(kind);
        REQUIRE(backend != nullptr);
        // Either the requested GPU kind (if present) or the CPU fallback.
        REQUIRE((backend->kind() == kind || backend->kind() == accel::BackendKind::Cpu));
    }
}

TEST_CASE("parity actually compared a GPU backend when one was compiled in") {
    // The parity cases above iterate availableBackends(). With no GPU backend
    // compiled in that list holds the CPU backend alone and they compare the
    // reference against itself — a pass that measured nothing. This case makes
    // the two situations distinguishable:
    //   - nothing compiled in (the cpu-headless configuration CI builds): say
    //     so loudly and move on, since there is no kernel to hold to anything.
    //   - compiled in but not detected at runtime: FAIL. That build was
    //     explicitly configured for the device, so a silent pass would ship a
    //     GPU kernel that no test ever ran.
    const auto compiled = accel::compiledBackendKinds();
    if (compiled.empty()) {
        MESSAGE(
            "SKIPPED: no GPU backend compiled in — the parity cases compared the CPU reference "
            "against itself. Configure with CYBER_ENABLE_CUDA/OPENCL/METAL on a machine with the "
            "device to exercise a kernel.");
        return;
    }
    const auto backends = accel::availableBackends();
    for (const accel::BackendKind kind : compiled) {
        CAPTURE(static_cast<int>(kind));
        const bool present =
            std::any_of(backends.begin(), backends.end(),
                        [kind](const auto& backend) { return backend->kind() == kind; });
        REQUIRE(present);
    }
}
