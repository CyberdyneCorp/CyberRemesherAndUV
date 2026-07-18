#include "cyber/uv/unwrap.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <unordered_map>

#include "cyber/uv/common.hpp"

namespace cyber::uv {
namespace {

// One nonzero of the real least-squares conformal matrix.
struct Triplet {
    std::size_t row = 0;
    std::size_t col = 0;
    double value = 0.0;
};

// A fan triangle of an island face, referenced by island-local vertex index.
struct Tri {
    std::size_t a = 0, b = 0, c = 0;
};

// Collects the island's distinct vertices (island-local order) and the fan
// triangles referencing them.
struct IslandTopology {
    std::vector<VertexId> vertices;
    std::vector<Tri> triangles;
};

[[nodiscard]] IslandTopology buildTopology(const Mesh& mesh, std::span<const FaceId> island) {
    IslandTopology topo;
    std::unordered_map<Index, std::size_t> localOf;

    const auto localIndex = [&](VertexId v) -> std::size_t {
        auto [it, inserted] = localOf.try_emplace(v.value, topo.vertices.size());
        if (inserted) {
            topo.vertices.push_back(v);
        }
        return it->second;
    };

    for (const FaceId face : island) {
        const std::vector<VertexId> verts = mesh.faceVertices(face);
        if (verts.size() < 3) {
            continue;
        }
        const std::size_t v0 = localIndex(verts[0]);
        for (std::size_t i = 1; i + 1 < verts.size(); ++i) {
            const std::size_t v1 = localIndex(verts[i]);
            const std::size_t v2 = localIndex(verts[i + 1]);
            topo.triangles.push_back({v0, v1, v2});
        }
    }
    return topo;
}

// Isometrically embeds a triangle in its own plane: a at the origin, b on the
// +x axis, c at (cx, cy) with cy > 0.
struct LocalTriangle {
    Vec2 p0{}, p1{}, p2{};
};

[[nodiscard]] LocalTriangle embedTriangle(Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 ab = b - a;
    const float baseLen = length(ab);
    const Vec3 xAxis = baseLen > 0.0f ? ab * (1.0f / baseLen) : Vec3{1, 0, 0};
    const Vec3 normal = normalized(cross(ab, c - a));
    const Vec3 yAxis = cross(normal, xAxis);
    const Vec3 ac = c - a;
    LocalTriangle t;
    t.p0 = {0.0f, 0.0f};
    t.p1 = {baseLen, 0.0f};
    t.p2 = {dot(ac, xAxis), dot(ac, yAxis)};
    return t;
}

// Picks two well-separated pin vertices: the vertex farthest from the
// centroid, then the vertex farthest from that one.
[[nodiscard]] std::pair<std::size_t, std::size_t> choosePins(const Mesh& mesh,
                                                             const IslandTopology& topo) {
    const std::size_t n = topo.vertices.size();
    Vec3 centroid{};
    for (const VertexId v : topo.vertices) {
        centroid += mesh.position(v);
    }
    centroid = centroid * (1.0f / static_cast<float>(n));

    const auto farthestFrom = [&](Vec3 origin) {
        std::size_t best = 0;
        float bestDist = -1.0f;
        for (std::size_t i = 0; i < n; ++i) {
            const float d = lengthSquared(mesh.position(topo.vertices[i]) - origin);
            if (d > bestDist) {
                bestDist = d;
                best = i;
            }
        }
        return best;
    };

    const std::size_t a = farthestFrom(centroid);
    const std::size_t b = farthestFrom(mesh.position(topo.vertices[a]));
    return {a, b};
}

// Sparse matrix-vector products used by the CGLS solver.
void matVec(const std::vector<Triplet>& m, const std::vector<double>& x, std::vector<double>& out) {
    std::fill(out.begin(), out.end(), 0.0);
    for (const Triplet& t : m) {
        out[t.row] += t.value * x[t.col];
    }
}
void matVecTranspose(const std::vector<Triplet>& m, const std::vector<double>& x,
                     std::vector<double>& out) {
    std::fill(out.begin(), out.end(), 0.0);
    for (const Triplet& t : m) {
        out[t.col] += t.value * x[t.row];
    }
}
[[nodiscard]] double dotProduct(const std::vector<double>& a, const std::vector<double>& b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

// Solves min ||A x - rhs|| by conjugate gradient on the normal equations
// (CGLS). A never needs to be squared explicitly.
[[nodiscard]] int solveCgls(const std::vector<Triplet>& a, std::size_t rows, std::size_t cols,
                            const std::vector<double>& rhs, std::vector<double>& x, int maxIter,
                            double tolerance) {
    std::vector<double> residual = rhs;          // r = rhs - A x, with x = 0
    std::vector<double> z(cols, 0.0);            // z = A^T r
    std::vector<double> p(cols, 0.0);
    std::vector<double> w(rows, 0.0);
    x.assign(cols, 0.0);

    matVecTranspose(a, residual, z);
    p = z;
    double gamma = dotProduct(z, z);
    const double gamma0 = gamma;
    const double tol2 = tolerance * tolerance * (gamma0 + 1e-30);

    int iter = 0;
    for (; iter < maxIter && gamma > tol2; ++iter) {
        matVec(a, p, w);
        const double wNorm = dotProduct(w, w);
        if (wNorm <= 0.0) {
            break;
        }
        const double alpha = gamma / wNorm;
        for (std::size_t i = 0; i < cols; ++i) {
            x[i] += alpha * p[i];
        }
        for (std::size_t i = 0; i < rows; ++i) {
            residual[i] -= alpha * w[i];
        }
        matVecTranspose(a, residual, z);
        const double gammaNext = dotProduct(z, z);
        const double beta = gammaNext / gamma;
        for (std::size_t i = 0; i < cols; ++i) {
            p[i] = z[i] + beta * p[i];
        }
        gamma = gammaNext;
    }
    return iter;
}

}  // namespace

UnwrapResult lscmUnwrap(const Mesh& mesh, std::span<const FaceId> island,
                        const UnwrapOptions& options) {
    UnwrapResult result;
    const IslandTopology topo = buildTopology(mesh, island);
    const std::size_t n = topo.vertices.size();
    if (topo.triangles.empty() || n < 3) {
        return result;
    }

    // Columns: [0, n) hold u, [n, 2n) hold v.
    const auto [pinA, pinB] = choosePins(mesh, topo);
    const float pinLen = length(mesh.position(topo.vertices[pinB]) - mesh.position(topo.vertices[pinA]));
    std::vector<Vec2> pinnedUv(n);
    std::vector<bool> pinned(2 * n, false);
    pinnedUv[pinA] = {0.0f, 0.0f};
    pinnedUv[pinB] = {pinLen > 0.0f ? pinLen : 1.0f, 0.0f};
    pinned[pinA] = pinned[n + pinA] = true;
    pinned[pinB] = pinned[n + pinB] = true;

    // Free-variable remap: global column -> free column (or npos).
    std::vector<std::size_t> freeCol(2 * n, 0);
    std::size_t freeCount = 0;
    for (std::size_t c = 0; c < 2 * n; ++c) {
        if (!pinned[c]) {
            freeCol[c] = freeCount++;
        }
    }

    // Build the full real conformal matrix, then split pinned columns into the
    // right-hand side. Each triangle contributes one complex equation, i.e.
    // two real rows.
    const std::size_t rows = 2 * topo.triangles.size();
    std::vector<double> rhs(rows, 0.0);
    std::vector<Triplet> matrix;
    matrix.reserve(topo.triangles.size() * 12);

    const auto pinnedValue = [&](std::size_t col) -> double {
        const std::size_t vert = col < n ? col : col - n;
        return col < n ? static_cast<double>(pinnedUv[vert].x)
                       : static_cast<double>(pinnedUv[vert].y);
    };

    const auto emit = [&](std::size_t row, std::size_t col, double val) {
        if (val == 0.0) {
            return;
        }
        if (pinned[col]) {
            rhs[row] -= val * pinnedValue(col);
        } else {
            matrix.push_back({row, freeCol[col], val});
        }
    };

    for (std::size_t t = 0; t < topo.triangles.size(); ++t) {
        const Tri& tri = topo.triangles[t];
        const LocalTriangle lt =
            embedTriangle(mesh.position(topo.vertices[tri.a]), mesh.position(topo.vertices[tri.b]),
                          mesh.position(topo.vertices[tri.c]));
        const double dT = static_cast<double>(lt.p1.x) * static_cast<double>(lt.p2.y) -
                          static_cast<double>(lt.p2.x) * static_cast<double>(lt.p1.y);
        if (dT <= 1e-20) {
            continue;  // degenerate triangle contributes nothing
        }
        const double invSqrt = 1.0 / std::sqrt(dT);

        // Complex per-vertex coefficients W_j = (x_{k}-x_{j}) + i(y_{k}-y_{j}).
        const Vec2 pts[3] = {lt.p0, lt.p1, lt.p2};
        const std::size_t vids[3] = {tri.a, tri.b, tri.c};
        const std::size_t rowRe = 2 * t;
        const std::size_t rowIm = 2 * t + 1;
        for (int j = 0; j < 3; ++j) {
            const Vec2 next = pts[static_cast<std::size_t>((j + 1) % 3)];
            const Vec2 prev = pts[static_cast<std::size_t>((j + 2) % 3)];
            const double wRe = (static_cast<double>(prev.x) - static_cast<double>(next.x)) * invSqrt;
            const double wIm = (static_cast<double>(prev.y) - static_cast<double>(next.y)) * invSqrt;
            const std::size_t uCol = vids[static_cast<std::size_t>(j)];
            const std::size_t vCol = n + vids[static_cast<std::size_t>(j)];
            // Re(W u) = wRe*u - wIm*v ; Im(W u) = wIm*u + wRe*v
            emit(rowRe, uCol, wRe);
            emit(rowRe, vCol, -wIm);
            emit(rowIm, uCol, wIm);
            emit(rowIm, vCol, wRe);
        }
    }

    if (freeCount == 0 || matrix.empty()) {
        return result;
    }

    int maxIter = options.maxIterations;
    if (maxIter <= 0) {
        maxIter = static_cast<int>(std::min<std::size_t>(4 * freeCount + 64, 100000));
    }
    std::vector<double> solution;
    result.iterations = solveCgls(matrix, rows, freeCount, rhs, solution, maxIter,
                                  static_cast<double>(options.tolerance));

    result.vertices = topo.vertices;
    result.uv.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t uCol = i;
        const std::size_t vCol = n + i;
        const float u = pinned[uCol] ? pinnedUv[i].x : static_cast<float>(solution[freeCol[uCol]]);
        const float v = pinned[vCol] ? pinnedUv[i].y : static_cast<float>(solution[freeCol[vCol]]);
        result.uv[i] = {u, v};
    }
    result.ok = true;
    return result;
}

void writeIslandUv(Mesh& mesh, std::span<const FaceId> island, const UnwrapResult& result) {
    if (!result.ok) {
        return;
    }
    std::unordered_map<Index, std::size_t> localOf;
    for (std::size_t i = 0; i < result.vertices.size(); ++i) {
        localOf.emplace(result.vertices[i].value, i);
    }
    std::vector<Vec2>& uv = ensureUvColumn(mesh);
    for (const FaceId face : island) {
        for (const LoopId loop : mesh.faceLoops(face)) {
            const auto it = localOf.find(mesh.loopVertex(loop).value);
            if (it != localOf.end()) {
                uv[static_cast<std::size_t>(loop.value)] = result.uv[it->second];
            }
        }
    }
}

bool unwrapIslandToUv(Mesh& mesh, std::span<const FaceId> island, const UnwrapOptions& options) {
    const UnwrapResult result = lscmUnwrap(mesh, island, options);
    if (!result.ok) {
        return false;
    }
    writeIslandUv(mesh, island, result);
    return true;
}

}  // namespace cyber::uv
