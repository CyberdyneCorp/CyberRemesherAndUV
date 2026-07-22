// cyberremesh — headless batch remeshing (cli-headless spec).
//
// Exit codes (spec: "Exit codes reflect outcomes"):
//   0 success | 2 argument error | 3 input load failure | 4 pipeline failure
//   or empty result | 5 partial success (failed islands) | 6 output/report
//   write failure | 130 cancelled (SIGINT).
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define CYBER_ISATTY _isatty
#define CYBER_FILENO _fileno
#else
#include <unistd.h>
#define CYBER_ISATTY isatty
#define CYBER_FILENO fileno
#endif

#include <json.hpp>

#include "cyber/accel/backend.hpp"
#include "cyber/core/io.hpp"
#include "cyber/core/pipeline.hpp"
#include "cyber/core/progress.hpp"
#include "cyber/core/version.hpp"
#include "cyber/quadrangulate/field_quadrangulator.hpp"

namespace {

using cyber::CancelToken;
namespace remesh = cyber::remesh;

constexpr int kExitOk = 0;
constexpr int kExitArgs = 2;
constexpr int kExitLoad = 3;
constexpr int kExitPipeline = 4;
constexpr int kExitPartial = 5;
constexpr int kExitWrite = 6;
constexpr int kExitCancelled = 130;

// SIGINT -> cooperative cancellation.
std::atomic<bool> g_interrupted{false};
void onSigint(int) { g_interrupted.store(true); }

struct CliOptions {
    std::string input;
    std::string output;
    std::string report;
    remesh::Parameters params;
    bool verbose = false;
    bool quiet = false;
};

void printUsage() {
    std::fprintf(stderr,
                 "usage: cyberremesh --input <mesh> --output <mesh> [options]\n"
                 "  --target-quads <int>     target quad count (default 50000)\n"
                 "  --edge-scale <float>     density scale (default 1.0)\n"
                 "  --sharp-edge <deg>       feature angle (default 90)\n"
                 "  --smooth-normal <deg>    smooth projection angle (default 0)\n"
                 "  --adaptivity <float>     0..1 curvature adaptivity (default 1)\n"
                 "  --pure-quads             quads-only output\n"
                 "  --hole-fill <int>        max hole boundary to fill (default 64)\n"
                 "  --patch-policy <p>       keep-largest | keep-all | min-faces:<N>\n"
                 "  --report <path.json>     machine-readable run report\n"
                 "  --verbose | --quiet      diagnostic detail / errors only\n"
                 "  --list-backends          print compute backends and exit\n"
                 "  --version                print version and exit\n");
}

template <typename T>
std::optional<T> parseNumber(const std::string& text) {
    T value{};
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

// Returns exit code (kExitOk to continue) and fills options.
int parseArgs(int argc, char** argv, CliOptions& options, bool& exitEarly) {
    exitEarly = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s requires a value\n", flag);
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };
        auto numeric = [&](const char* flag, auto& target) -> bool {
            const auto text = next(flag);
            if (!text) {
                return false;
            }
            const auto value = parseNumber<std::decay_t<decltype(target)>>(*text);
            if (!value) {
                // Non-numeric values are rejected, never silently zero
                // (AutoRemesher converted "abc" to 0 silently — spec'd away).
                std::fprintf(stderr, "error: %s expects a number, got '%s'\n", flag, text->c_str());
                return false;
            }
            target = *value;
            return true;
        };

        if (arg == "--version") {
            std::printf("cyberremesh %.*s\n", static_cast<int>(cyber::version().size()),
                        cyber::version().data());
            exitEarly = true;
            return kExitOk;
        }
        if (arg == "--list-backends") {
            for (const auto& backend : cyber::accel::availableBackends()) {
                std::printf("%s\n", backend->deviceName().c_str());
            }
            exitEarly = true;
            return kExitOk;
        }
        if (arg == "--help" || arg == "-h") {
            printUsage();
            exitEarly = true;
            return kExitOk;
        }
        if (arg == "--input" || arg == "-i") {
            const auto v = next("--input");
            if (!v) {
                return kExitArgs;
            }
            options.input = *v;
        } else if (arg == "--output" || arg == "-o") {
            const auto v = next("--output");
            if (!v) {
                return kExitArgs;
            }
            options.output = *v;
        } else if (arg == "--report") {
            const auto v = next("--report");
            if (!v) {
                return kExitArgs;
            }
            options.report = *v;
        } else if (arg == "--target-quads") {
            if (!numeric("--target-quads", options.params.targetQuadCount)) {
                return kExitArgs;
            }
        } else if (arg == "--edge-scale") {
            if (!numeric("--edge-scale", options.params.edgeScale)) {
                return kExitArgs;
            }
        } else if (arg == "--sharp-edge") {
            if (!numeric("--sharp-edge", options.params.sharpEdgeDegrees)) {
                return kExitArgs;
            }
        } else if (arg == "--smooth-normal") {
            if (!numeric("--smooth-normal", options.params.smoothNormalDegrees)) {
                return kExitArgs;
            }
        } else if (arg == "--adaptivity") {
            if (!numeric("--adaptivity", options.params.adaptivity)) {
                return kExitArgs;
            }
        } else if (arg == "--hole-fill") {
            if (!numeric("--hole-fill", options.params.holeFillMaxBoundary)) {
                return kExitArgs;
            }
        } else if (arg == "--pure-quads") {
            options.params.pureQuads = true;
        } else if (arg == "--patch-policy") {
            const auto v = next("--patch-policy");
            if (!v) {
                return kExitArgs;
            }
            if (*v == "keep-largest") {
                options.params.smallPatchPolicy = remesh::SmallPatchPolicy::KeepLargest;
            } else if (*v == "keep-all") {
                options.params.smallPatchPolicy = remesh::SmallPatchPolicy::KeepAll;
            } else if (v->rfind("min-faces:", 0) == 0) {
                const auto n = parseNumber<int>(v->substr(10));
                if (!n) {
                    std::fprintf(stderr, "error: --patch-policy min-faces:<N> needs a number\n");
                    return kExitArgs;
                }
                options.params.smallPatchPolicy = remesh::SmallPatchPolicy::MinFaces;
                options.params.smallPatchMinFaces = *n;
            } else {
                std::fprintf(stderr, "error: unknown patch policy '%s'\n", v->c_str());
                return kExitArgs;
            }
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--quiet") {
            options.quiet = true;
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            printUsage();
            return kExitArgs;
        }
    }
    if (options.input.empty() || options.output.empty()) {
        std::fprintf(stderr, "error: --input and --output are required\n");
        printUsage();
        return kExitArgs;
    }
    return kExitOk;
}

const char* statusName(remesh::RunStatus status) {
    switch (status) {
        case remesh::RunStatus::Success:
            return "success";
        case remesh::RunStatus::Partial:
            return "partial";
        case remesh::RunStatus::Cancelled:
            return "cancelled";
        case remesh::RunStatus::Error:
            return "error";
    }
    return "unknown";
}

// Writing the report is a hard requirement when requested: an unwritable
// path is exit 6, never silently skipped (spec).
int writeReport(const CliOptions& options, const remesh::PipelineResult& result,
                double elapsedSeconds) {
    nlohmann::json report;
    report["tool"] = "cyberremesh";
    report["version"] = std::string(cyber::version());
    report["input"] = options.input;
    report["output"] = options.output;
    report["status"] = statusName(result.status);
    report["elapsedSeconds"] = elapsedSeconds;
    report["parameters"] = {
        {"targetQuadCount", options.params.targetQuadCount},
        {"edgeScale", options.params.edgeScale},
        {"sharpEdgeDegrees", options.params.sharpEdgeDegrees},
        {"smoothNormalDegrees", options.params.smoothNormalDegrees},
        {"adaptivity", options.params.adaptivity},
        {"pureQuads", options.params.pureQuads},
        {"holeFillMaxBoundary", options.params.holeFillMaxBoundary},
    };
    report["statistics"] = {
        {"vertices", result.stats.vertexCount},
        {"quads", result.stats.quadCount},
        {"triangles", result.stats.triangleCount},
        {"otherPolygons", result.stats.otherPolygonCount},
        {"islands", result.stats.islandCount},
        {"islandsFailed", result.stats.islandsFailed},
        {"targetEdgeLength", result.stats.targetEdgeLength},
    };
    report["warnings"] = nlohmann::json::array();
    for (const auto& issue : result.parameterIssues) {
        report["warnings"].push_back(issue.message);
    }
    report["failedIslands"] = nlohmann::json::array();
    for (const auto& diag : result.failedIslands) {
        report["failedIslands"].push_back({{"island", diag.islandIndex},
                                           {"inputFaces", diag.inputFaces},
                                           {"stage", diag.stage},
                                           {"reason", diag.reason}});
    }

    std::ofstream file(options.report, std::ios::trunc);
    if (!file) {
        std::fprintf(stderr, "error: cannot write report to '%s'\n", options.report.c_str());
        return kExitWrite;
    }
    file << report.dump(2) << '\n';
    if (!file.flush()) {
        std::fprintf(stderr, "error: write to '%s' failed\n", options.report.c_str());
        return kExitWrite;
    }
    return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
    CliOptions options;
    bool exitEarly = false;
    const int argStatus = parseArgs(argc, argv, options, exitEarly);
    if (argStatus != kExitOk || exitEarly) {
        return argStatus;
    }
    std::signal(SIGINT, onSigint);

    auto imported = cyber::io::importMesh(options.input);
    if (!imported.ok()) {
        std::fprintf(stderr, "error: %s\n", imported.error().message.c_str());
        return kExitLoad;
    }
    if (!options.quiet) {
        for (const auto& warning : imported.value().warnings) {
            std::fprintf(stderr, "warning: %s\n", warning.c_str());
        }
    }

    const CancelToken cancel;
    const bool showProgress = !options.quiet && CYBER_ISATTY(CYBER_FILENO(stdout)) != 0;
    cyber::ProgressSink sink([&](float p, std::string_view stage) {
        if (g_interrupted.load()) {
            cancel.requestCancel();
        }
        if (showProgress) {
            std::printf("\r%3d%% %.*s   ", static_cast<int>(p * 100.0f),
                        static_cast<int>(stage.size()), stage.data());
            std::fflush(stdout);
        }
    });

    const auto start = std::chrono::steady_clock::now();
    // The CLI uses the field-aligned quadrangulator (QuadCover-lite) for
    // better edge flow than the greedy default.
    const remesh::PipelineResult result =
        remesh::remesh(imported.value().mesh, options.params, &sink, &cancel,
                       [] { return remesh::makeFieldAlignedQuadrangulator(); });
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (showProgress) {
        std::printf("\r");
    }

    if (!options.quiet) {
        for (const auto& issue : result.parameterIssues) {
            std::fprintf(stderr, "warning: %s\n", issue.message.c_str());
        }
        for (const auto& diag : result.failedIslands) {
            std::fprintf(stderr, "warning: island %zu failed at %s: %s\n", diag.islandIndex,
                         diag.stage.c_str(), diag.reason.c_str());
        }
    }

    if (result.status == remesh::RunStatus::Cancelled) {
        std::fprintf(stderr, "cancelled\n");
        return kExitCancelled;
    }
    if (result.status == remesh::RunStatus::Error) {
        std::fprintf(stderr, "error: %s\n", result.error.c_str());
        return kExitPipeline;
    }

    const auto exportStatus = cyber::io::exportMesh(result.mesh, options.output);
    if (!exportStatus.ok()) {
        std::fprintf(stderr, "error: %s\n", exportStatus.error().message.c_str());
        return kExitWrite;
    }

    if (!options.report.empty()) {
        const int reportStatus = writeReport(options, result, elapsed);
        if (reportStatus != kExitOk) {
            return reportStatus;
        }
    }

    if (!options.quiet) {
        std::printf("=== cyberremesh report ===\n");
        std::printf("input:     %s\n", options.input.c_str());
        std::printf("output:    %s\n", options.output.c_str());
        std::printf("status:    %s\n", statusName(result.status));
        std::printf("vertices:  %zu\n", result.stats.vertexCount);
        std::printf("quads:     %zu\n", result.stats.quadCount);
        std::printf("non-quads: %zu\n",
                    result.stats.triangleCount + result.stats.otherPolygonCount);
        std::printf("islands:   %zu (%zu failed)\n", result.stats.islandCount,
                    result.stats.islandsFailed);
        std::printf("time:      %.2fs\n", elapsed);
    }
    return result.status == remesh::RunStatus::Partial ? kExitPartial : kExitOk;
}
