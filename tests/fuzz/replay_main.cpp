// Corpus replay driver: feeds every checked-in seed through its fuzz target.
//
// The round-2/3 fuzz campaigns left no harness and no corpus, so nothing they
// found could ever regress-test anything. This binary is the cheap half of the
// fix — it runs under ctest on every platform, needs no clang and no libFuzzer,
// and is bounded (file-count, per-file size and a wall-clock budget) so a
// regression that hangs or grows without bound FAILS instead of wedging CI.
//
//   cyber_fuzz_replay <corpus-root>
//
// <corpus-root>/png/*   -> cyber::fuzz::png
// <corpus-root>/mesh/*  -> cyber::fuzz::meshIo

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "harness.hpp"

namespace {

// A seed is a minimised reproducer, never a real asset; anything larger is a
// mistake in the corpus rather than an input worth replaying.
constexpr std::uintmax_t kMaxSeedBytes = 1u << 20;  // 1 MiB
constexpr std::size_t kMaxSeeds = 512;
constexpr std::chrono::seconds kBudget{60};

using Target = int (*)(const std::uint8_t*, std::size_t);

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Returns the number of seeds replayed, or -1 on a corpus problem.
int replayDirectory(const std::filesystem::path& dir, Target target, const char* name) {
    if (!std::filesystem::is_directory(dir)) {
        std::fprintf(stderr, "replay: corpus dir %s is missing\n", dir.string().c_str());
        return -1;
    }
    const auto start = std::chrono::steady_clock::now();
    int replayed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.file_size() > kMaxSeedBytes) {
            std::fprintf(stderr, "replay: seed %s exceeds %ju bytes\n",
                         entry.path().string().c_str(), kMaxSeedBytes);
            return -1;
        }
        if (static_cast<std::size_t>(replayed) >= kMaxSeeds) {
            std::fprintf(stderr, "replay: %s corpus exceeds %zu seeds\n", name, kMaxSeeds);
            return -1;
        }
        const auto bytes = readFile(entry.path());
        target(bytes.data(), bytes.size());
        ++replayed;
        if (std::chrono::steady_clock::now() - start > kBudget) {
            std::fprintf(stderr,
                         "replay: %s corpus exceeded its %llds budget after %d seeds "
                         "— a parser is hanging on one of them\n",
                         name, static_cast<long long>(kBudget.count()), replayed);
            return -1;
        }
    }
    std::printf("replay: %s — %d seed(s) OK\n", name, replayed);
    return replayed;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <corpus-root>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path root(argv[1]);
    const int png = replayDirectory(root / "png", &cyber::fuzz::png, "png");
    const int mesh = replayDirectory(root / "mesh", &cyber::fuzz::meshIo, "mesh");
    if (png < 0 || mesh < 0) {
        return 1;
    }
    // An empty corpus would pass vacuously, which is how the campaigns' findings
    // went unguarded in the first place.
    if (png == 0 || mesh == 0) {
        std::fprintf(stderr, "replay: a corpus directory is empty\n");
        return 1;
    }
    std::printf("replay: %d seed(s) replayed, no crash\n", png + mesh);
    return 0;
}
