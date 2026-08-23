// In-process QuadCover seamless-UV solve. See autoremesher_solve.hpp.
//
// Mirrors the sequence autoremesher_harness.cpp's main() performs on the
// AutoRemesher public API, minus the OBJ/UV file I/O and the std::_Exit(0)
// shortcut (a library cannot terminate the process). GEO::initialize() is guarded
// so it runs once even across multiple islands/threads, and GEO::terminate() is
// never called — Geogram's teardown is what the harness sidestepped with _Exit,
// so we simply leave the runtime initialized for the life of the process.
#include "autoremesher_solve.hpp"

#include <atomic>
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

#ifdef _OPENMP
#include <omp.h>
#endif

// oneAPI TBB (2021+) moved its headers under <oneapi/tbb/>; the vendored solver
// does the same dance for the headers it needs.
#if defined(__has_include)
#if __has_include(<oneapi/tbb/task_arena.h>)
#include <oneapi/tbb/task_arena.h>
#else
#include <tbb/task_arena.h>
#endif
#else
#include <tbb/task_arena.h>
#endif

#include <geogram/basic/common.h>
#include <geogram/bibliography/bibliography.h>

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

// Non-zero on a thread that is inside solveSeamlessUv.
thread_local std::size_t t_solveDepth = 0;

// std::cout/std::cerr are the HOST's streams: a solve may drop what the vendored
// solver writes to them, never what the host writes. The solver runs on the
// thread that called in plus the pool workers it farms the islands out to, so
// those are the threads whose writes are dropped; a write from any other thread
// is the host's own logging and is passed through untouched.
//
// Arena slot 0 belongs to whichever external thread owns the arena and stays
// claimed after that thread's last TBB call, so only a worker slot (> 0)
// identifies a thread the solver is running on.
bool writingFromSolve() {
    if (t_solveDepth != 0) {
        return true;
    }
    if (tbb::this_task_arena::current_thread_index() > 0) {
        return true;
    }
#ifdef _OPENMP
    return omp_get_level() > 0;
#else
    return false;
#endif
}

// Forwards the host's writes to the buffer the stream had; swallows the solver's.
class SolveFilterBuffer : public std::streambuf {
  public:
    void setTarget(std::streambuf* target) { target_.store(target, std::memory_order_release); }

  protected:
    int overflow(int c) override {
        std::streambuf* const target = passThroughTarget();
        if (target == nullptr || c == traits_type::eof()) {
            return traits_type::not_eof(c);
        }
        return target->sputc(static_cast<char>(c));
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::streambuf* const target = passThroughTarget();
        return target == nullptr ? n : target->sputn(s, n);
    }
    int sync() override {
        std::streambuf* const target = passThroughTarget();
        return target == nullptr ? 0 : target->pubsync();
    }

  private:
    // The host's buffer for a host write, nullptr (drop) for a solver write.
    std::streambuf* passThroughTarget() const {
        return writingFromSolve() ? nullptr : target_.load(std::memory_order_acquire);
    }

    std::atomic<std::streambuf*> target_{nullptr};
};

// Deliberately leaked: std::cout/std::cerr must never be left pointing at a
// destroyed filter if the process exits while a solve is running.
SolveFilterBuffer& outFilter() {
    static SolveFilterBuffer* filter = new SolveFilterBuffer();
    return *filter;
}

SolveFilterBuffer& errFilter() {
    static SolveFilterBuffer* filter = new SolveFilterBuffer();
    return *filter;
}

std::mutex& consoleMutex() {
    static std::mutex mutex;
    return mutex;
}

// Guarded by consoleMutex(). std::cout/std::cerr are process globals, so
// concurrent island solves share ONE interposition: only the outermost guard
// installs the filters, and only the last one out takes them back off.
std::size_t g_consoleDepth = 0;
std::streambuf* g_savedOut = nullptr;
std::streambuf* g_savedErr = nullptr;

// Interposes a thread-aware filter on std::cout/std::cerr for the duration of a
// solve, so the solver's chatter is dropped while the host keeps its console.
class ConsoleSilencer {
  public:
    ConsoleSilencer() : engaged_(!solverOutputWanted()) {
        ++t_solveDepth;
        if (!engaged_) {
            return;
        }
        const std::lock_guard<std::mutex> lock(consoleMutex());
        if (g_consoleDepth++ == 0) {
            g_savedOut = std::cout.rdbuf();
            g_savedErr = std::cerr.rdbuf();
            outFilter().setTarget(g_savedOut);
            errFilter().setTarget(g_savedErr);
            std::cout.rdbuf(&outFilter());
            std::cerr.rdbuf(&errFilter());
        }
    }
    ~ConsoleSilencer() {
        --t_solveDepth;
        if (!engaged_) {
            return;
        }
        const std::lock_guard<std::mutex> lock(consoleMutex());
        if (--g_consoleDepth != 0) {
            return;
        }
        // Only un-interpose what is still ours: a host that redirected its own
        // stream mid-solve owns it now, and writing our snapshot back would
        // silently undo that.
        if (std::cout.rdbuf() == &outFilter()) {
            std::cout.rdbuf(g_savedOut);
        }
        if (std::cerr.rdbuf() == &errFilter()) {
            std::cerr.rdbuf(g_savedErr);
        }
    }
    ConsoleSilencer(const ConsoleSilencer&) = delete;
    ConsoleSilencer& operator=(const ConsoleSilencer&) = delete;

  private:
    const bool engaged_;
};

// Geogram's citation registry is a process-global vector<CitationRecord> that
// geo_cite() appends to with no dedup and no cap — once per spatial sort inside
// the solve, so the record count tracks the solver's work, not the call count.
// The list is consumed only by Biblio::terminate() under the `biblio` CmdLine
// argument, which this build never declares, and GEO::terminate() is never
// called here (see the header note), so every record a solve appends would stay
// resident for the life of the host process: a farm worker or DCC plugin doing
// thousands of remeshes grows monotonically, and no consumer can reclaim it
// (the shared library exports no GEO symbol). So we drop the records ourselves.
//
// cite() and reset_citations() both touch that vector unsynchronized, so the
// clear must happen when NO solve is in flight: same outermost-only discipline
// as the console filters, with its own depth because that one is skipped
// entirely under CYBER_QC_VERBOSE.
std::mutex& citationMutex() {
    static std::mutex mutex;
    return mutex;
}

std::size_t g_citationDepth = 0;  // guarded by citationMutex()

class CitationReclaimer {
  public:
    CitationReclaimer() {
        const std::lock_guard<std::mutex> lock(citationMutex());
        ++g_citationDepth;
    }
    ~CitationReclaimer() {
        const std::lock_guard<std::mutex> lock(citationMutex());
        if (--g_citationDepth == 0) {
            GEO::Biblio::reset_citations();
        }
    }
    CitationReclaimer(const CitationReclaimer&) = delete;
    CitationReclaimer& operator=(const CitationReclaimer&) = delete;
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
    // After initialize(): Biblio::initialize() cites too, and this must outlive
    // every geo_cite the solve below reaches.
    const CitationReclaimer citations;

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
