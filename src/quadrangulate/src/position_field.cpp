#include "cyber/quadrangulate/position_field.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <vector>

namespace cyber::remesh {

Vec3 PositionField::qPerp(std::size_t i) const { return cross(normal[i], q[i]); }

namespace {

// Rotate a tangent-plane vector 90 degrees about the normal (right-handed).
Vec3 rot90(Vec3 d, Vec3 n) { return cross(n, d); }

// The representative of direction `d` under 4-fold (90-degree) symmetry that
// best matches reference `ref`, both unit vectors in the plane normal to `n`.
Vec3 matchRoSy(Vec3 ref, Vec3 d, Vec3 n) {
    Vec3 best = d;
    float bestDot = dot(ref, d);
    for (int k = 0; k < 3; ++k) {
        d = rot90(d, n);
        const float dt = dot(ref, d);
        if (dt > bestDot) {
            bestDot = dt;
            best = d;
        }
    }
    return best;
}

// Project `v` into the plane normal to `n` and renormalise (zero-safe).
Vec3 projectUnit(Vec3 v, Vec3 n) {
    const Vec3 p = v - n * dot(n, v);
    const float len = length(p);
    return len > 1e-12f ? p / len : v;
}

struct Frames {
    std::vector<Vec3> normal, tangent;
    std::vector<bool> constrained;     // on a feature/boundary edge
    std::vector<Vec3> constraintDir;   // feature/boundary direction (tangent plane)
    std::vector<std::vector<Index>> neighbours;  // 1-ring vertex neighbours
    std::vector<bool> valid;
};

Frames buildFrames(const Mesh& mesh) {
    const std::size_t n = mesh.vertexCapacity();
    Frames fr;
    fr.normal.assign(n, Vec3{0, 0, 1});
    fr.tangent.assign(n, Vec3{1, 0, 0});
    fr.constrained.assign(n, false);
    fr.constraintDir.assign(n, Vec3{0, 0, 0});
    fr.neighbours.assign(n, {});
    fr.valid.assign(n, false);

    for (Index i = 0; i < n; ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v)) {
            continue;
        }
        const auto faces = mesh.vertexFaces(v);
        if (faces.empty()) {
            continue;
        }
        Vec3 nrm{};
        for (const FaceId f : faces) {
            nrm += mesh.faceNormal(f);
        }
        nrm = normalized(nrm);
        fr.normal[i] = nrm;
        fr.valid[i] = true;

        // Neighbours + a stable initial tangent (first incident edge).
        Vec3 firstEdge{};
        for (const EdgeId e : mesh.vertexEdges(v)) {
            const auto [a, b] = mesh.edgeVertices(e);
            const VertexId other = (a == v ? b : a);
            fr.neighbours[i].push_back(other.value);
            if (length(firstEdge) < 1e-12f) {
                firstEdge = mesh.position(other) - mesh.position(v);
            }
            // A feature/boundary edge constrains the vertex to align with it.
            if (mesh.isFeatureEdge(e) || mesh.isBoundaryEdge(e)) {
                fr.constrained[i] = true;
                fr.constraintDir[i] = projectUnit(mesh.position(other) - mesh.position(v), nrm);
            }
        }
        fr.tangent[i] = projectUnit(length(firstEdge) > 1e-12f ? firstEdge : Vec3{1, 0, 0}, nrm);
    }
    return fr;
}

// One Jacobi sweep of the 4-RoSy orientation field: each free vertex moves
// toward the RoSy-matched average of its neighbours' directions, reprojected
// into its tangent plane. Constrained vertices hold the feature direction.
void smoothOrientation(const Frames& fr, std::vector<Vec3>& q) {
    std::vector<Vec3> next(q.size());
    for (std::size_t i = 0; i < q.size(); ++i) {
        if (!fr.valid[i]) {
            next[i] = q[i];
            continue;
        }
        if (fr.constrained[i]) {
            next[i] = fr.constraintDir[i];
            continue;
        }
        Vec3 acc = q[i];
        for (const Index j : fr.neighbours[i]) {
            const Vec3 dj = projectUnit(q[static_cast<std::size_t>(j)], fr.normal[i]);
            acc += matchRoSy(q[i], dj, fr.normal[i]);
        }
        next[i] = projectUnit(acc, fr.normal[i]);
    }
    q.swap(next);
}

// One Jacobi sweep of the position field. For each neighbour, snap its lattice
// point into this vertex's cell (integer offset in the local q / q_perp frame)
// and average; then re-anchor the average to the lattice point nearest the
// vertex (position_round_4). Same-cell vertices thus converge to one shared
// lattice point (crisp cells for the collapse), while cross-cell neighbours,
// snapped back by a full step, reinforce the target spacing instead of merging.
void smoothPosition(const Frames& fr, const Mesh& mesh, const std::vector<Vec3>& q, float s,
                    std::vector<Vec3>& o) {
    const float invS = 1.0f / s;
    std::vector<Vec3> next(o.size());
    for (std::size_t i = 0; i < o.size(); ++i) {
        const VertexId vi{static_cast<Index>(i)};
        if (!fr.valid[i]) {
            next[i] = o[i];
            continue;
        }
        const Vec3 qi = q[i];
        const Vec3 ti = cross(fr.normal[i], qi);
        Vec3 sum = o[i];
        float w = 1.0f;
        for (const Index jn : fr.neighbours[i]) {
            const Vec3 diff = o[static_cast<std::size_t>(jn)] - o[i];
            const float a = std::round(dot(diff, qi) * invS);
            const float b = std::round(dot(diff, ti) * invS);
            const Vec3 aligned = o[static_cast<std::size_t>(jn)] - (qi * (a * s) + ti * (b * s));
            sum += aligned;
            w += 1.0f;
        }
        sum = sum / w;
        // position_round_4: shift `sum` by whole lattice steps to the cell of p,
        // then remove the normal drift so the representative stays on the surface
        // (pure averaging sinks o toward the centroid on curved regions).
        const Vec3 p = mesh.position(vi);
        const Vec3 d = p - sum;
        const Vec3 anchored = sum + qi * (std::round(dot(qi, d) * invS) * s) +
                              ti * (std::round(dot(ti, d) * invS) * s);
        Vec3 rel = anchored - p;
        rel = rel - fr.normal[i] * dot(fr.normal[i], rel);
        next[i] = p + rel;
    }
    o.swap(next);
}

}  // namespace

PositionField computePositionField(const Mesh& mesh, float spacing, int iterations) {
    const Frames fr = buildFrames(mesh);
    const std::size_t n = mesh.vertexCapacity();

    PositionField field;
    field.spacing = spacing;
    field.normal = fr.normal;
    field.valid.assign(n, false);
    field.q.assign(n, Vec3{1, 0, 0});
    field.o.assign(n, Vec3{0, 0, 0});
    for (std::size_t i = 0; i < n; ++i) {
        field.valid[i] = fr.valid[i];
        field.q[i] = fr.constrained[i] ? fr.constraintDir[i] : fr.tangent[i];
        field.o[i] = mesh.position(VertexId{static_cast<Index>(i)});
    }

    for (int it = 0; it < iterations; ++it) {
        smoothOrientation(fr, field.q);
    }
    for (int it = 0; it < iterations; ++it) {
        smoothPosition(fr, mesh, field.q, spacing, field.o);
    }
    return field;
}

}  // namespace cyber::remesh
