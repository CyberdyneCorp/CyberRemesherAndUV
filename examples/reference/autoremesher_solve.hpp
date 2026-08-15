// In-process QuadCover seamless-UV solver (Milestone M4c).
//
// Exposes AutoRemesher's Geogram quad_cover pipeline (isotropic remesh ->
// seamless-UV parameterize) as a plain-C++ function so cyber_quadrangulate can
// obtain a seamless integer-grid UV WITHOUT spawning the autoremesher_cli
// harness, writing OBJ/UV temp files, or reading CYBER_QUADCOVER_CLI.
//
// The signature deliberately uses only std types (no Geogram/AutoRemesher types)
// so the caller — compiled at C++20 with strict warnings — never has to include,
// or even see, the C++14 Geogram headers. The implementation
// (autoremesher_solve.cpp) is built into the isolated `cyber_quadcover_solver`
// static library with relaxed flags.
#ifndef CYBER_REMESH_AUTOREMESHER_SOLVE_HPP
#define CYBER_REMESH_AUTOREMESHER_SOLVE_HPP

#include <array>
#include <cstddef>
#include <vector>

namespace cyber::remesh::qcsolver {

struct SeamlessSolveResult {
    std::vector<std::array<double, 3>> vertices;
    std::vector<std::array<std::size_t, 3>> triangles;
    // Per triangle, the 3 corner UVs flattened as u0,v0,u1,v1,u2,v2.
    std::vector<std::array<double, 6>> triangleUvs;
    bool ok = false;
};

// Run the isotropic-remesh + seamless-UV (Geogram quad_cover) solve in-process.
// `targetQuads` maps to AutoRemesher::setTargetTriangleCount(targetQuads * 2)
// exactly as the harness did; `scaling`/`adaptivity` map to setScaling /
// setGradientAdaptivity. GEO::initialize() runs exactly once per process
// (thread-safe) and GEO::terminate() is never called. Returns ok == false on any
// failure (remesh() false, empty output, or a mismatched UV/triangle count).
//
// Host-process contract: the call leaves the process's signal dispositions,
// std::terminate handler, std::new_handler and LC_NUMERIC exactly as it found
// them, and writes nothing to std::cout/std::cerr. Silencing the vendored solver
// does not silence the host: a write the host makes to those streams from its own
// thread while a solve runs still reaches the buffer the host installed. (The one
// exception is a host thread that is itself a task-pool or OpenMP worker, which
// cannot be told apart from the solver's own.) Set CYBER_QC_VERBOSE to let the
// vendored solver's progress traces through.
SeamlessSolveResult solveSeamlessUv(const std::vector<std::array<double, 3>>& verts,
                                    const std::vector<std::array<std::size_t, 3>>& tris,
                                    long targetQuads, double scaling, double adaptivity);

}  // namespace cyber::remesh::qcsolver

#endif  // CYBER_REMESH_AUTOREMESHER_SOLVE_HPP
