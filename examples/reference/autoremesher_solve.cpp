// In-process QuadCover seamless-UV solve. See autoremesher_solve.hpp.
//
// Mirrors the sequence autoremesher_harness.cpp's main() performs on the
// AutoRemesher public API, minus the OBJ/UV file I/O and the std::_Exit(0)
// shortcut (a library cannot terminate the process). GEO::initialize() is guarded
// so it runs once even across multiple islands/threads, and GEO::terminate() is
// never called — Geogram's teardown is what the harness sidestepped with _Exit,
// so we simply leave the runtime initialized for the life of the process.
#include "autoremesher_solve.hpp"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <new>
#include <streambuf>
#include <string>

#ifndef _WIN32
#include <csignal>
#endif

#include <geogram/basic/common.h>

#include <AutoRemesher/AutoRemesher>
#include <AutoRemesher/Vector2>
#include <AutoRemesher/Vector3>

namespace cyber::remesh::qcsolver {

namespace {

#ifndef _WIN32
// Signal dispositions GEO::initialize() is known to take over: SIGSEGV/SIGILL/
// SIGBUS/SIGFPE through os_install_signal_handlers(), plus SIGINT, which
// Process::initialize() resets to the default through enable_cancel(false) —
// that one is NOT covered by the GEOGRAM_NO_HANDLER flag.
const int kHostSignals[] = {SIGSEGV, SIGILL, SIGBUS, SIGFPE, SIGINT};
const std::size_t kHostSignalCount = sizeof(kHostSignals) / sizeof(kHostSignals[0]);

bool sameDisposition(const struct sigaction& a, const struct sigaction& b) {
    if (a.sa_flags != b.sa_flags) {
        return false;
    }
    return (a.sa_flags & SA_SIGINFO) != 0 ? a.sa_sigaction == b.sa_sigaction
                                          : a.sa_handler == b.sa_handler;
}
#endif

// GEO::initialize() is a process-global mutation: with its default flag it
// replaces the host's crash handlers, and it always resets SIGINT, replaces
// std::terminate/std::new_handler and setenv()s LC_NUMERIC=POSIX. A library
// embedded in someone else's process (DCC plugin, farm worker) must leave all of
// that as it found it — a host whose crash reporter is silently unhooked by a
// remesh writes no minidump for any later fault. So we ask for
// GEOGRAM_NO_HANDLER *and* snapshot/restore everything the flag does not cover;
// the restore is also what keeps the guarantee if a re-vendored Geogram installs
// handlers regardless of the flag.
//
// Not restored: the FP exception mask (Geogram's Process::initialize disables
// FE_DIVBYZERO/OVERFLOW/UNDERFLOW/INVALID traps). Handing the vendored solver
// back a trapping FP environment would turn its benign intermediate infinities
// into a host crash, which is worse than the loss it causes.
void initializeGeogramPreservingHost() {
#ifndef _WIN32
    struct sigaction savedSignals[kHostSignalCount] = {};
    bool savedSignalOk[kHostSignalCount] = {};
    for (std::size_t i = 0; i < kHostSignalCount; ++i) {
        savedSignalOk[i] = ::sigaction(kHostSignals[i], nullptr, &savedSignals[i]) == 0;
    }
    const char* lcNumeric = std::getenv("LC_NUMERIC");
    const bool hadLcNumeric = lcNumeric != nullptr;
    const std::string savedLcNumeric = hadLcNumeric ? std::string(lcNumeric) : std::string();
#endif
    const std::terminate_handler savedTerminate = std::get_terminate();
    const std::new_handler savedNewHandler = std::get_new_handler();

    GEO::initialize(GEO::GEOGRAM_NO_HANDLER);

    std::set_terminate(savedTerminate);
    std::set_new_handler(savedNewHandler);
#ifndef _WIN32
    // Put back only what Geogram actually changed, so a signal the host never
    // touched is left byte-for-byte pristine rather than re-armed through us.
    for (std::size_t i = 0; i < kHostSignalCount; ++i) {
        struct sigaction current = {};
        if (!savedSignalOk[i] || ::sigaction(kHostSignals[i], nullptr, &current) != 0 ||
            sameDisposition(current, savedSignals[i])) {
            continue;
        }
        ::sigaction(kHostSignals[i], &savedSignals[i], nullptr);
    }
    if (hadLcNumeric) {
        ::setenv("LC_NUMERIC", savedLcNumeric.c_str(), 1);
    } else {
        ::unsetenv("LC_NUMERIC");
    }
#endif
}

// GEO::initialize() must run before any Geogram use and exactly once per process.
// std::call_once makes that safe under concurrent island solves; we never pair it
// with GEO::terminate() (that static teardown is the double-free the harness
// avoided via std::_Exit — leaving the runtime up for the process lifetime is the
// library-safe equivalent).
void ensureGeogramInitialized() {
    static std::once_flag once;
    std::call_once(once, [] { initializeGeogramPreservingHost(); });
}

// The vendored AutoRemesher/exploragram code traces its progress straight to
// std::cerr and std::cout (hundreds of lines on a large mesh) with no switch of
// its own, and a library must not write to its host's console uninvited.
// CYBER_QC_VERBOSE brings the traces back for debugging; read once, since the
// solve is reachable from several threads.
bool solverOutputWanted() {
    static const bool wanted = std::getenv("CYBER_QC_VERBOSE") != nullptr;
    return wanted;
}

class NullBuffer : public std::streambuf {
  protected:
    int overflow(int c) override { return c; }
    std::streamsize xsputn(const char*, std::streamsize n) override { return n; }
};

// Deliberately leaked: std::cout/std::cerr must never be left pointing at a
// destroyed sink if the process exits while a solve is running.
std::streambuf& nullSink() {
    static std::streambuf* sink = new NullBuffer();
    return *sink;
}

std::mutex& consoleMutex() {
    static std::mutex mutex;
    return mutex;
}

// Guarded by consoleMutex(). std::cout/std::cerr are process globals, so
// concurrent island solves share ONE redirection: only the outermost guard swaps
// the buffers, and only the last one out puts the host's back.
std::size_t g_consoleDepth = 0;
std::streambuf* g_savedOut = nullptr;
std::streambuf* g_savedErr = nullptr;

// Points std::cout/std::cerr at a null sink for the duration of a solve.
class ConsoleSilencer {
  public:
    ConsoleSilencer() : engaged_(!solverOutputWanted()) {
        if (!engaged_) {
            return;
        }
        const std::lock_guard<std::mutex> lock(consoleMutex());
        if (g_consoleDepth++ == 0) {
            g_savedOut = std::cout.rdbuf(&nullSink());
            g_savedErr = std::cerr.rdbuf(&nullSink());
        }
    }
    ~ConsoleSilencer() {
        if (!engaged_) {
            return;
        }
        const std::lock_guard<std::mutex> lock(consoleMutex());
        if (--g_consoleDepth == 0) {
            std::cout.rdbuf(g_savedOut);
            std::cerr.rdbuf(g_savedErr);
        }
    }
    ConsoleSilencer(const ConsoleSilencer&) = delete;
    ConsoleSilencer& operator=(const ConsoleSilencer&) = delete;

  private:
    const bool engaged_;
};

}  // namespace

SeamlessSolveResult solveSeamlessUv(const std::vector<std::array<double, 3>>& verts,
                                    const std::vector<std::array<std::size_t, 3>>& tris,
                                    long targetQuads, double scaling, double adaptivity) {
    SeamlessSolveResult result;
    if (verts.empty() || tris.empty()) {
        return result;
    }

    // Covers the whole solve INCLUDING Geogram's own start-up logging.
    const ConsoleSilencer silence;
    ensureGeogramInitialized();

    std::vector<AutoRemesher::Vector3> arVerts;
    arVerts.reserve(verts.size());
    for (const auto& v : verts) {
        arVerts.emplace_back(v[0], v[1], v[2]);
    }
    std::vector<std::vector<size_t>> arTris;
    arTris.reserve(tris.size());
    for (const auto& t : tris) {
        arTris.push_back({t[0], t[1], t[2]});
    }

    AutoRemesher::AutoRemesher remesher(arVerts, arTris);
    remesher.setTargetTriangleCount(static_cast<size_t>(targetQuads) * 2);  // GUI mapping
    remesher.setScaling(scaling);
    remesher.setGradientAdaptivity(adaptivity);
    remesher.setModelType(AutoRemesher::ModelType::Organic);
    if (!remesher.remesh()) {
        return result;
    }

    const std::vector<AutoRemesher::Vector3>& isoVerts = remesher.isotropicVertices();
    const std::vector<std::vector<size_t>>& isoTris = remesher.isotropicTriangles();
    const std::vector<std::vector<AutoRemesher::Vector2>>& isoUvs = remesher.isotropicTriangleUvs();
    if (isoVerts.empty() || isoTris.empty() || isoUvs.size() != isoTris.size()) {
        return result;
    }

    result.vertices.reserve(isoVerts.size());
    for (const AutoRemesher::Vector3& v : isoVerts) {
        result.vertices.push_back({v.x(), v.y(), v.z()});
    }
    result.triangles.reserve(isoTris.size());
    result.triangleUvs.reserve(isoTris.size());
    for (size_t t = 0; t < isoTris.size(); ++t) {
        if (isoTris[t].size() != 3 || isoUvs[t].size() != 3) {
            return SeamlessSolveResult{};  // malformed corner count -> fail cleanly
        }
        result.triangles.push_back({isoTris[t][0], isoTris[t][1], isoTris[t][2]});
        result.triangleUvs.push_back({isoUvs[t][0].x(), isoUvs[t][0].y(), isoUvs[t][1].x(),
                                      isoUvs[t][1].y(), isoUvs[t][2].x(), isoUvs[t][2].y()});
    }

    result.ok = true;
    return result;
}

}  // namespace cyber::remesh::qcsolver
