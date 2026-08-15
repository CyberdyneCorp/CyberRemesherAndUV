#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

// Fuzz targets over the untrusted-input surface (the parsers the three hardening
// rounds touched). Each has the libFuzzer signature — take a byte buffer, return
// 0 — so it can be driven two ways from the same source:
//
//   * `cyber_fuzz_replay` (built with the test suite, run by ctest) replays the
//     checked-in seed corpus under tests/fuzz/corpus. Cheap, deterministic and
//     available on every platform, so a crash a campaign once found stays found.
//   * `-DCYBER_BUILD_FUZZERS=ON` with clang's `-fsanitize=fuzzer` builds one
//     libFuzzer binary per target for an actual campaign (see tests/fuzz/README.md).
//
// Both parsers under test take a path, not a buffer, so the harness writes the
// input to a temp file. That caps throughput, but it is the interface a caller
// actually uses and keeps the harness honest about it.
namespace cyber::fuzz {

// PNG decoder (cyber::imageio::loadPng).
int png(const std::uint8_t* data, std::size_t size);

// Mesh importers (cyber::io::importMesh); the first input byte picks the
// extension, so one corpus covers OBJ/PLY/STL/glTF/GLB.
int meshIo(const std::uint8_t* data, std::size_t size);

// Writes `data` to a uniquely named temp file with the given extension and
// removes it when the returned guard dies. Declared here so both targets share
// one implementation of the awkward part.
class TempInput {
public:
    TempInput(const std::uint8_t* data, std::size_t size, const std::string& extension);
    ~TempInput();
    TempInput(const TempInput&) = delete;
    TempInput& operator=(const TempInput&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }
    [[nodiscard]] bool ok() const { return m_ok; }

private:
    std::filesystem::path m_path;
    bool m_ok = false;
};

}  // namespace cyber::fuzz
