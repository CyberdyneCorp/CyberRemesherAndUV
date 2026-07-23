#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/retopo/snapping.hpp"

// Advanced build tools (manual-retopology spec, "Advanced build tools", roadmap
// 9.3): concrete deterministic edits over cyber::Mesh reusing its structural
// operators (addFace, splitEdge, splitFace). New vertices snap to the Target
// surface when a SurfaceSnapper is supplied. Each function stays focused; the
// heavier SurfaceCut is decomposed into two passes. Header-only and inline.
namespace cyber::retopo {

// ---- affine transform (TransformVertices) ---------------------------------
// Column-major 3x3 linear part + translation; enough for translate/scale/rotate
// without pulling in a matrix type.
struct Affine {
    Vec3 col0{1.0f, 0.0f, 0.0f};
    Vec3 col1{0.0f, 1.0f, 0.0f};
    Vec3 col2{0.0f, 0.0f, 1.0f};
    Vec3 translation{};

    [[nodiscard]] Vec3 apply(Vec3 p) const {
        return col0 * p.x + col1 * p.y + col2 * p.z + translation;
    }

    [[nodiscard]] static Affine translating(Vec3 t) {
        Affine a;
        a.translation = t;
        return a;
    }
    [[nodiscard]] static Affine scaling(Vec3 s) {
        Affine a;
        a.col0 = {s.x, 0.0f, 0.0f};
        a.col1 = {0.0f, s.y, 0.0f};
        a.col2 = {0.0f, 0.0f, s.z};
        return a;
    }
    // Rodrigues rotation about a (normalized) axis by `radians`.
    [[nodiscard]] static Affine rotating(Vec3 axis, float radians) {
        const Vec3 u = normalized(axis);
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        const float t = 1.0f - c;
        Affine a;
        a.col0 = {c + u.x * u.x * t, u.y * u.x * t + u.z * s, u.z * u.x * t - u.y * s};
        a.col1 = {u.x * u.y * t - u.z * s, c + u.y * u.y * t, u.z * u.y * t + u.x * s};
        a.col2 = {u.x * u.z * t + u.y * s, u.y * u.z * t - u.x * s, c + u.z * u.z * t};
        return a;
    }
};

namespace detail {

// Adds a vertex, snapping to the Target surface first when `snap` is usable.
inline VertexId snapAdd(Mesh& mesh, Vec3 p, const SurfaceSnapper* snap) {
    const Vec3 q = (snap != nullptr && !snap->empty()) ? snap->snapToSurface(p).point : p;
    return mesh.addVertex(q);
}

// Bilinear grid of (nu+1) x (nv+1) vertices over the four corners, row-major.
inline std::vector<VertexId> bilinearGridVertices(Mesh& mesh, Vec3 c00, Vec3 c10, Vec3 c11,
                                                  Vec3 c01, int nu, int nv,
                                                  const SurfaceSnapper* snap) {
    std::vector<VertexId> verts;
    verts.reserve(static_cast<std::size_t>((nu + 1) * (nv + 1)));
    for (int j = 0; j <= nv; ++j) {
        const float v = static_cast<float>(j) / static_cast<float>(nv);
        for (int i = 0; i <= nu; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(nu);
            const Vec3 top = lerp(c00, c10, u);
            const Vec3 bot = lerp(c01, c11, u);
            verts.push_back(snapAdd(mesh, lerp(top, bot, v), snap));
        }
    }
    return verts;
}

}  // namespace detail

// A generated patch: its vertices (row-major, `cols` per row) and its new faces.
struct GridPatch {
    std::vector<VertexId> vertices;
    std::vector<FaceId> faces;
    int cols = 0;  // vertices per row = nu + 1
    int rows = 0;  // vertex rows    = nv + 1
};

// BuildQuad: a grid of nu x nv quads bilinearly spanning the four corner
// positions (c00->c10 is +u, c00->c01 is +v). Corners wind consistently.
[[nodiscard]] inline GridPatch buildQuad(Mesh& mesh, Vec3 c00, Vec3 c10, Vec3 c11, Vec3 c01, int nu,
                                         int nv, const SurfaceSnapper* snap = nullptr) {
    GridPatch patch;
    if (nu < 1 || nv < 1) {
        return patch;
    }
    patch.cols = nu + 1;
    patch.rows = nv + 1;
    patch.vertices = detail::bilinearGridVertices(mesh, c00, c10, c11, c01, nu, nv, snap);
    const auto at = [&](int i, int j) {
        return patch.vertices[static_cast<std::size_t>(j * patch.cols + i)];
    };
    for (int j = 0; j < nv; ++j) {
        for (int i = 0; i < nu; ++i) {
            const std::array<VertexId, 4> quad = {at(i, j), at(i + 1, j), at(i + 1, j + 1),
                                                  at(i, j + 1)};
            const FaceId f = mesh.addFace(quad);
            if (f.valid()) {
                patch.faces.push_back(f);
            }
        }
    }
    return patch;
}

// BuildTri: same bilinear grid, each cell split into two triangles.
[[nodiscard]] inline GridPatch buildTri(Mesh& mesh, Vec3 c00, Vec3 c10, Vec3 c11, Vec3 c01, int nu,
                                        int nv, const SurfaceSnapper* snap = nullptr) {
    GridPatch patch;
    if (nu < 1 || nv < 1) {
        return patch;
    }
    patch.cols = nu + 1;
    patch.rows = nv + 1;
    patch.vertices = detail::bilinearGridVertices(mesh, c00, c10, c11, c01, nu, nv, snap);
    const auto at = [&](int i, int j) {
        return patch.vertices[static_cast<std::size_t>(j * patch.cols + i)];
    };
    const auto emit = [&](const std::array<VertexId, 3>& tri) {
        const FaceId f = mesh.addFace(tri);
        if (f.valid()) {
            patch.faces.push_back(f);
        }
    };
    for (int j = 0; j < nv; ++j) {
        for (int i = 0; i < nu; ++i) {
            emit({at(i, j), at(i + 1, j), at(i + 1, j + 1)});
            emit({at(i, j), at(i + 1, j + 1), at(i, j + 1)});
        }
    }
    return patch;
}

// DrawStrip: a quad strip whose centerline follows `path`; each sample spawns a
// left/right pair offset by +/- side/2, and consecutive pairs bridge into quads.
[[nodiscard]] inline std::vector<FaceId> drawStrip(Mesh& mesh, std::span<const Vec3> path,
                                                   Vec3 side,
                                                   const SurfaceSnapper* snap = nullptr) {
    std::vector<FaceId> faces;
    if (path.size() < 2) {
        return faces;
    }
    std::vector<VertexId> left;
    std::vector<VertexId> right;
    left.reserve(path.size());
    right.reserve(path.size());
    for (const Vec3 p : path) {
        left.push_back(detail::snapAdd(mesh, p - side * 0.5f, snap));
        right.push_back(detail::snapAdd(mesh, p + side * 0.5f, snap));
    }
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        const std::array<VertexId, 4> quad = {left[i], right[i], right[i + 1], left[i + 1]};
        const FaceId f = mesh.addFace(quad);
        if (f.valid()) {
            faces.push_back(f);
        }
    }
    return faces;
}

// ExtendBoundary (grid fill): extrude an ordered boundary vertex chain outward
// by `offset` in `rings` successive rows of quads.
[[nodiscard]] inline std::vector<FaceId> extendBoundaryGrid(Mesh& mesh,
                                                            std::span<const VertexId> chain,
                                                            Vec3 offset, int rings,
                                                            const SurfaceSnapper* snap = nullptr) {
    std::vector<FaceId> faces;
    if (chain.size() < 2 || rings < 1) {
        return faces;
    }
    std::vector<VertexId> prev(chain.begin(), chain.end());
    for (int r = 1; r <= rings; ++r) {
        std::vector<VertexId> cur;
        cur.reserve(chain.size());
        for (const VertexId v : chain) {
            cur.push_back(
                detail::snapAdd(mesh, mesh.position(v) + offset * static_cast<float>(r), snap));
        }
        for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
            const std::array<VertexId, 4> quad = {prev[i], prev[i + 1], cur[i + 1], cur[i]};
            const FaceId f = mesh.addFace(quad);
            if (f.valid()) {
                faces.push_back(f);
            }
        }
        prev = std::move(cur);
    }
    return faces;
}

// ExtendBoundary (fan fill): close an ordered boundary chain to a single apex
// (centroid + apexOffset) with a fan of triangles.
[[nodiscard]] inline std::vector<FaceId> extendBoundaryFan(Mesh& mesh,
                                                           std::span<const VertexId> chain,
                                                           Vec3 apexOffset,
                                                           const SurfaceSnapper* snap = nullptr) {
    std::vector<FaceId> faces;
    if (chain.size() < 2) {
        return faces;
    }
    Vec3 centroid{};
    for (const VertexId v : chain) {
        centroid += mesh.position(v);
    }
    centroid = centroid * (1.0f / static_cast<float>(chain.size()));
    const VertexId apex = detail::snapAdd(mesh, centroid + apexOffset, snap);
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        const std::array<VertexId, 3> tri = {chain[i], chain[i + 1], apex};
        const FaceId f = mesh.addFace(tri);
        if (f.valid()) {
            faces.push_back(f);
        }
    }
    return faces;
}

namespace detail {

// True when the single face on the live edge a-b traverses a -> b in its
// stored ring order (cyclically). New faces welded onto that edge must then
// traverse b -> a to keep normals coherent. False when no such edge/face
// exists or the edge is interior (2 faces: orientation is ambiguous, keep
// the caller's order).
inline bool faceTraverses(const Mesh& mesh, VertexId a, VertexId b) {
    const EdgeId e = mesh.edgeBetween(a, b);
    if (!e.valid()) {
        return false;
    }
    const std::vector<FaceId> faces = mesh.edgeFaces(e);
    if (faces.size() != 1) {
        return false;
    }
    const std::vector<VertexId> ring = mesh.faceVertices(faces.front());
    for (std::size_t i = 0; i < ring.size(); ++i) {
        if (ring[i].value == a.value) {
            return ring[(i + 1) % ring.size()].value == b.value;
        }
    }
    return false;
}

}  // namespace detail

// ExtendBoundary rings (task 4.2): like extendBoundaryGrid, but supporting
// CLOSED chains (a wrap-around quad joins the last and first columns each
// ring), correcting the quad winding against the existing face on the
// chain's first edge (coherent normals), and reporting the OUTERMOST ring's
// vertex chain so camera-driven sessions can stack per-step calls with
// different offsets (automatic mode journals ONCE with sequential rings).
// Each ring offsets from the PREVIOUS ring's (possibly snapped) positions.
struct BoundaryExtension {
    std::vector<FaceId> faces;
    std::vector<VertexId> outerChain;
};

[[nodiscard]] inline BoundaryExtension extendBoundaryRings(Mesh& mesh,
                                                           std::span<const VertexId> chain,
                                                           bool closed, Vec3 offset, int rings,
                                                           const SurfaceSnapper* snap = nullptr) {
    BoundaryExtension out;
    out.outerChain.assign(chain.begin(), chain.end());
    if (chain.size() < 2 || rings < 1) {
        return out;
    }
    // New quads traverse prev[i] -> prev[i+1]; flip when the existing face
    // already traverses the chain in that direction.
    const bool flip = detail::faceTraverses(mesh, chain[0], chain[1]);
    std::vector<VertexId> prev = out.outerChain;
    for (int r = 1; r <= rings; ++r) {
        std::vector<VertexId> cur;
        cur.reserve(prev.size());
        for (const VertexId v : prev) {
            cur.push_back(detail::snapAdd(mesh, mesh.position(v) + offset, snap));
        }
        const auto emit = [&](std::size_t i, std::size_t j) {
            const std::array<VertexId, 4> quad =
                flip ? std::array<VertexId, 4>{prev[j], prev[i], cur[i], cur[j]}
                     : std::array<VertexId, 4>{prev[i], prev[j], cur[j], cur[i]};
            const FaceId f = mesh.addFace(quad);
            if (f.valid()) {
                out.faces.push_back(f);
            }
        };
        for (std::size_t i = 0; i + 1 < prev.size(); ++i) {
            emit(i, i + 1);
        }
        if (closed && prev.size() >= 3) {
            emit(prev.size() - 1, 0);
        }
        prev = std::move(cur);
    }
    out.outerChain = std::move(prev);
    return out;
}

// DrawStrip along a stroke path (task 4.2): a quad strip welded onto the
// boundary edge startA-startB, whose stations follow `path` (world-space
// stroke samples, typically resampled at quad-size arc length). Unlike the
// constant-side drawStrip above, each station's rail pair spans the strip
// width along cross(viewDir, tangent) — the screen-perpendicular direction
// the stroke was drawn with — with sign continuity so curved strokes never
// flip rails. Rail vertices snap to the Target when `snap` is given; quad
// winding is corrected against the start edge's existing face.
struct StripResult {
    std::vector<FaceId> faces;
    std::vector<VertexId> newVertices;
};

[[nodiscard]] inline StripResult drawStripPath(Mesh& mesh, std::span<const Vec3> path, float width,
                                               Vec3 viewDir, VertexId startA, VertexId startB,
                                               const SurfaceSnapper* snap = nullptr) {
    StripResult out;
    if (path.empty() || !(width > 0.0f) || !mesh.isAlive(startA) || !mesh.isAlive(startB)) {
        return out;
    }
    const Vec3 a = mesh.position(startA);
    const Vec3 b = mesh.position(startB);
    Vec3 sideRef = b - a;
    if (lengthSquared(sideRef) <= 0.0f || lengthSquared(viewDir) <= 0.0f) {
        return out;
    }
    sideRef = normalized(sideRef);
    const bool flip = detail::faceTraverses(mesh, startA, startB);

    std::vector<VertexId> left{startA};
    std::vector<VertexId> right{startB};
    const Vec3 start = (a + b) * 0.5f;
    for (std::size_t i = 0; i < path.size(); ++i) {
        const Vec3 previous = i == 0 ? start : path[i - 1];
        const Vec3 next = i + 1 < path.size() ? path[i + 1] : path[i];
        Vec3 tangent = next - previous;
        if (lengthSquared(tangent) <= 1e-20f) {
            tangent = path[i] - start;
        }
        Vec3 side = cross(viewDir, tangent);
        if (lengthSquared(side) <= 1e-20f) {
            side = sideRef;  // stroke along the view axis: keep the last rail
        }
        side = normalized(side);
        if (dot(side, sideRef) < 0.0f) {
            side = side * -1.0f;
        }
        sideRef = side;
        const Vec3 half = side * (width * 0.5f);
        const VertexId l = detail::snapAdd(mesh, path[i] - half, snap);
        const VertexId r = detail::snapAdd(mesh, path[i] + half, snap);
        out.newVertices.push_back(l);
        out.newVertices.push_back(r);
        left.push_back(l);
        right.push_back(r);
    }
    for (std::size_t i = 0; i + 1 < left.size(); ++i) {
        const std::array<VertexId, 4> quad =
            flip ? std::array<VertexId, 4>{right[i], left[i], left[i + 1], right[i + 1]}
                 : std::array<VertexId, 4>{left[i], right[i], right[i + 1], left[i + 1]};
        const FaceId f = mesh.addFace(quad);
        if (f.valid()) {
            out.faces.push_back(f);
        }
    }
    return out;
}

// PatchClone: duplicate a set of faces (shared vertices cloned once) transformed
// by `xf`. The clone is a fresh disconnected copy. Returns the new faces.
// `reverseWinding` reverses every cloned face's ring (the Patch Clone FLIP
// option, task 4.2: a mirroring transform inverts orientation — reversing the
// winding keeps the pasted patch's normals coherent).
[[nodiscard]] inline std::vector<FaceId> patchClone(Mesh& mesh, std::span<const FaceId> faces,
                                                    const Affine& xf,
                                                    const SurfaceSnapper* snap = nullptr,
                                                    bool reverseWinding = false) {
    std::vector<FaceId> out;
    std::unordered_map<Index, VertexId> clone;
    const auto cloneVertex = [&](VertexId v) {
        const auto it = clone.find(v.value);
        if (it != clone.end()) {
            return it->second;
        }
        const VertexId nv = detail::snapAdd(mesh, xf.apply(mesh.position(v)), snap);
        clone.emplace(v.value, nv);
        return nv;
    };
    for (const FaceId f : faces) {
        if (!mesh.isAlive(f)) {
            continue;
        }
        const std::vector<VertexId> verts = mesh.faceVertices(f);
        std::vector<VertexId> mapped;
        mapped.reserve(verts.size());
        for (const VertexId v : verts) {
            mapped.push_back(cloneVertex(v));
        }
        if (reverseWinding) {
            std::reverse(mapped.begin(), mapped.end());
        }
        const FaceId nf = mesh.addFace(mapped);
        if (nf.valid()) {
            out.push_back(nf);
        }
    }
    return out;
}

// PatchClone convenience: pure translation.
[[nodiscard]] inline std::vector<FaceId> patchClone(Mesh& mesh, std::span<const FaceId> faces,
                                                    Vec3 offset,
                                                    const SurfaceSnapper* snap = nullptr) {
    return patchClone(mesh, faces, Affine::translating(offset), snap);
}

// TransformVertices: in-place affine transform of a vertex set.
inline void transformVertices(Mesh& mesh, std::span<const VertexId> verts, const Affine& xf) {
    for (const VertexId v : verts) {
        if (mesh.isAlive(v)) {
            mesh.setPosition(v, xf.apply(mesh.position(v)));
        }
    }
}

// PathDistribute: `count` points spaced evenly by arc length along `path`
// (endpoints inclusive). Pure geometry; no mesh mutation.
[[nodiscard]] inline std::vector<Vec3> pathDistribute(std::span<const Vec3> path, int count) {
    std::vector<Vec3> out;
    if (path.size() < 2 || count < 1) {
        return out;
    }
    out.reserve(static_cast<std::size_t>(count));
    if (count == 1) {
        out.push_back(path.front());
        return out;
    }
    std::vector<float> cum(path.size(), 0.0f);
    for (std::size_t i = 1; i < path.size(); ++i) {
        cum[i] = cum[i - 1] + length(path[i] - path[i - 1]);
    }
    const float total = cum.back();
    if (total <= 0.0f) {
        for (int k = 0; k < count; ++k) {
            out.push_back(path.front());
        }
        return out;
    }
    std::size_t seg = 0;
    for (int k = 0; k < count; ++k) {
        const float target = total * static_cast<float>(k) / static_cast<float>(count - 1);
        while (seg + 2 < path.size() && cum[seg + 1] < target) {
            ++seg;
        }
        const float segLen = cum[seg + 1] - cum[seg];
        const float t = segLen > 0.0f ? (target - cum[seg]) / segLen : 0.0f;
        out.push_back(lerp(path[seg], path[seg + 1], t));
    }
    return out;
}

namespace detail {

// Splits every edge that strictly crosses the plane, returning the set of new
// on-plane vertices (by index).
inline std::unordered_set<Index> splitCrossingEdges(Mesh& mesh, const Plane& plane, float eps) {
    std::vector<EdgeId> crossing;
    for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
        const EdgeId e{i};
        if (!mesh.isAlive(e)) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(e);
        const float d0 = signedDistance(plane, mesh.position(a));
        const float d1 = signedDistance(plane, mesh.position(b));
        if ((d0 > eps && d1 < -eps) || (d0 < -eps && d1 > eps)) {
            crossing.push_back(e);
        }
    }
    std::unordered_set<Index> cutVerts;
    for (const EdgeId e : crossing) {
        const auto [a, b] = mesh.edgeVertices(e);
        const float d0 = signedDistance(plane, mesh.position(a));
        const float d1 = signedDistance(plane, mesh.position(b));
        const VertexId nv = mesh.splitEdge(e, d0 / (d0 - d1));
        if (nv.valid()) {
            cutVerts.insert(nv.value);
        }
    }
    return cutVerts;
}

}  // namespace detail

// SurfaceCut: split faces along a plane. Every edge that crosses the plane is
// split at the crossing; every face that then carries two (non-adjacent) cut
// vertices is split between them. Returns the number of faces split.
inline std::size_t surfaceCut(Mesh& mesh, const Plane& plane, float eps = 1e-6f) {
    const std::unordered_set<Index> cutVerts = detail::splitCrossingEdges(mesh, plane, eps);
    if (cutVerts.empty()) {
        return 0;
    }
    std::vector<FaceId> faces;
    for (Index i = 0; i < mesh.faceCapacity(); ++i) {
        const FaceId f{i};
        if (mesh.isAlive(f)) {
            faces.push_back(f);
        }
    }
    std::size_t splits = 0;
    for (const FaceId f : faces) {
        if (!mesh.isAlive(f)) {
            continue;
        }
        const std::vector<VertexId> vs = mesh.faceVertices(f);
        std::vector<std::size_t> hits;
        for (std::size_t k = 0; k < vs.size(); ++k) {
            if (cutVerts.count(vs[k].value) != 0) {
                hits.push_back(k);
            }
        }
        if (hits.size() != 2) {
            continue;
        }
        const std::size_t gap = hits[1] - hits[0];
        if (gap <= 1 || gap + 1 >= vs.size()) {
            continue;  // adjacent corners: nothing to split between
        }
        if (mesh.splitFace(f, vs[hits[0]], vs[hits[1]]).valid()) {
            ++splits;
        }
    }
    return splits;
}

// SurfaceCut (knife segment): cut counts for one knife segment.
struct SurfaceCutResult {
    std::size_t splitEdges = 0;
    std::size_t splitFaces = 0;
    std::size_t triangulatedFaces = 0;
};

// SurfaceCut restricted to one knife SEGMENT (the app's straight knife
// stroke, task 4.1): the cut plane spans the segment a->b and the view
// direction, and only edges whose plane crossing lies within the segment's
// extent (between the two planes perpendicular to a->b at its endpoints) are
// split — a global plane cut would slice topology far outside the stroke.
// After the split pass every face that carries two non-adjacent cut vertices
// is split between them, and with `triangulateNGons` every touched face left
// with more than four sides is fan-triangulated (the spec's auto-triangulated
// n-gons). New vertices snap to the Target when `snap` is supplied.
inline SurfaceCutResult surfaceCutSegment(Mesh& mesh, Vec3 a, Vec3 b, Vec3 viewDir,
                                          bool triangulateNGons,
                                          const SurfaceSnapper* snap = nullptr, float eps = 1e-6f) {
    SurfaceCutResult result;
    const Vec3 ab = b - a;
    const Vec3 normal = cross(ab, viewDir);
    if (lengthSquared(ab) <= eps * eps || lengthSquared(normal) <= 1e-20f) {
        return result;
    }
    const Plane plane{a, normalized(normal)};
    const float abLen2 = lengthSquared(ab);

    // Pass 1: split edges crossing the plane within the segment slab.
    std::vector<EdgeId> crossing;
    for (Index i = 0; i < mesh.edgeCapacity(); ++i) {
        const EdgeId e{i};
        if (!mesh.isAlive(e)) {
            continue;
        }
        const auto [v0, v1] = mesh.edgeVertices(e);
        const float d0 = signedDistance(plane, mesh.position(v0));
        const float d1 = signedDistance(plane, mesh.position(v1));
        if (!((d0 > eps && d1 < -eps) || (d0 < -eps && d1 > eps))) {
            continue;
        }
        const Vec3 q = lerp(mesh.position(v0), mesh.position(v1), d0 / (d0 - d1));
        const float s = dot(q - a, ab) / abLen2;
        if (s < 0.0f || s > 1.0f) {
            continue;  // crossing outside the knife segment's extent
        }
        crossing.push_back(e);
    }
    std::unordered_set<Index> cutVerts;
    std::unordered_set<Index> touched;
    for (const EdgeId e : crossing) {
        if (!mesh.isAlive(e)) {
            continue;
        }
        const auto [v0, v1] = mesh.edgeVertices(e);
        const float d0 = signedDistance(plane, mesh.position(v0));
        const float d1 = signedDistance(plane, mesh.position(v1));
        for (const FaceId f : mesh.edgeFaces(e)) {
            touched.insert(f.value);
        }
        const VertexId nv = mesh.splitEdge(e, d0 / (d0 - d1));
        if (nv.valid()) {
            if (snap != nullptr && !snap->empty()) {
                mesh.setPosition(nv, snap->snapToSurface(mesh.position(nv)).point);
            }
            cutVerts.insert(nv.value);
            ++result.splitEdges;
        }
    }
    if (cutVerts.empty()) {
        return result;
    }

    // Pass 2: split every touched face carrying two non-adjacent cut
    // vertices between them (the new edge along the knife line).
    std::vector<FaceId> faces;
    faces.reserve(touched.size());
    for (const Index i : touched) {
        const FaceId f{i};
        if (mesh.isAlive(f)) {
            faces.push_back(f);
        }
    }
    std::vector<FaceId> halves;
    for (const FaceId f : faces) {
        if (!mesh.isAlive(f)) {
            continue;
        }
        const std::vector<VertexId> vs = mesh.faceVertices(f);
        std::vector<std::size_t> hits;
        for (std::size_t k = 0; k < vs.size(); ++k) {
            if (cutVerts.count(vs[k].value) != 0) {
                hits.push_back(k);
            }
        }
        if (hits.size() != 2) {
            continue;
        }
        const std::size_t gap = hits[1] - hits[0];
        if (gap <= 1 || gap + 1 >= vs.size()) {
            continue;  // adjacent corners: nothing to split between
        }
        const FaceId half = mesh.splitFace(f, vs[hits[0]], vs[hits[1]]);
        if (half.valid()) {
            halves.push_back(half);
            ++result.splitFaces;
        }
    }
    for (const FaceId f : halves) {
        touched.insert(f.value);
    }

    // Pass 3: auto-triangulate every touched face left with > 4 sides (a
    // quad with one split edge is a pentagon even when it was never split
    // between two cut vertices).
    if (triangulateNGons) {
        for (const Index i : touched) {
            const FaceId f{i};
            if (mesh.isAlive(f) && mesh.faceVertices(f).size() > 4) {
                mesh.triangulateFace(f);
                ++result.triangulatedFaces;
            }
        }
    }
    return result;
}

// LoopInfo: measure an ordered edge loop given as a vertex list. `closed` is set
// when the last and first vertices are joined by an existing edge.
struct LoopMetrics {
    std::size_t edgeCount = 0;
    float length = 0.0f;
    bool closed = false;
};

[[nodiscard]] inline LoopMetrics loopInfo(const Mesh& mesh, std::span<const VertexId> loop) {
    LoopMetrics m;
    if (loop.size() < 2) {
        return m;
    }
    for (std::size_t i = 0; i + 1 < loop.size(); ++i) {
        m.length += length(mesh.position(loop[i + 1]) - mesh.position(loop[i]));
        ++m.edgeCount;
    }
    if (loop.size() >= 3 && mesh.edgeBetween(loop.back(), loop.front()).valid()) {
        m.length += length(mesh.position(loop.front()) - mesh.position(loop.back()));
        ++m.edgeCount;
        m.closed = true;
    }
    return m;
}

}  // namespace cyber::retopo
