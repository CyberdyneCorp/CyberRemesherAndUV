#include <atomic>
#include <fstream>
#include <system_error>

#include "harness.hpp"

namespace cyber::fuzz {
namespace {

// Monotonic per-process counter: two targets in one replay run must not race for
// the same file name, and libFuzzer's -jobs mode forks, so the pid is in there too.
std::atomic<unsigned long long> g_counter{0};

}  // namespace

TempInput::TempInput(const std::uint8_t* data, std::size_t size, const std::string& extension) {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return;
    }
    // `this` disambiguates concurrently live guards (libFuzzer's -jobs mode
    // forks); the counter disambiguates successive ones in the same process.
    const auto tag = static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(this));
    m_path = dir / ("cyber_fuzz_" + std::to_string(tag) + "_" +
                    std::to_string(g_counter.fetch_add(1)) + extension);

    std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        m_path.clear();
        return;
    }
    if (size > 0) {
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    out.close();
    m_ok = out.good();
}

TempInput::~TempInput() {
    if (!m_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }
}

}  // namespace cyber::fuzz
