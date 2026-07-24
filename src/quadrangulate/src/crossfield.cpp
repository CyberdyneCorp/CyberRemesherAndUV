#include "cyber/quadrangulate/crossfield.hpp"

#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

#include "cyber/accel/primitives.hpp"
#include "cyber/quadrangulate/position_field.hpp"

namespace cyber::remesh {

namespace {

// Unit reference tangent of a face: its first edge, projected into the face
// plane and orthonormalised against the face normal.
Vec3 faceTangent(const Mesh& mesh, FaceId f, Vec3 normal) {
    const std::vector<VertexId> verts = mesh.faceVertices(f);
    Vec3 t = mesh.position(verts[1]) - mesh.position(verts[0]);
    t = t - normal * dot(normal, t);
    return normalized(t);
}

// Angle of world direction `d` in the (t, b) tangent frame.
float frameAngle(Vec3 d, Vec3 t, Vec3 b) { return std::atan2(dot(d, b), dot(d, t)); }

}  // namespace

Vec3 CrossField::direction(FaceId f) const {
    // Recover theta from the 4-symmetry representation u = e^{i*4*theta}.
    const float theta = std::atan2(imag[f.value], real[f.value]) / 4.0f;
    return tangent[f.value] * std::cos(theta) + bitangent[f.value] * std::sin(theta);
}

float CrossField::angle(FaceId f) const {
    float theta = std::atan2(imag[f.value], real[f.value]) / 4.0f;
    if (theta < 0.0f) {
        theta += kPi / 2.0f;
    }
    return theta;
}

CrossField computeCrossField(const Mesh& mesh, int iterations, accel::IBackend& backend,
                             float creaseAlignDegrees) {
    const std::size_t cap = mesh.faceCapacity();
    CrossField field;
    field.tangent.assign(cap, Vec3{1, 0, 0});
    field.bitangent.assign(cap, Vec3{0, 1, 0});
    field.real.assign(cap, 1.0f);
    field.imag.assign(cap, 0.0f);

    // Per-face frame; collect the live faces.
    std::vector<FaceId> faces;
    for (Index i = 0; i < cap; ++i) {
        const FaceId f{i};
        if (!mesh.isAlive(f) || mesh.faceSize(f) != 3) {
            continue;
        }
        const Vec3 n = mesh.faceNormal(f);
        const Vec3 t = faceTangent(mesh, f, n);
        field.tangent[i] = t;
        field.bitangent[i] = cross(n, t);
        faces.push_back(f);
    }
    const std::size_t nf = faces.size();
    if (nf == 0) {
        return field;
    }

    // Compact index per live face.
    std::vector<Index> compact(cap, kInvalidIndex);
    for (std::size_t c = 0; c < nf; ++c) {
        compact[faces[c].value] = static_cast<Index>(c);
    }

    // CYBER_QC_FIELD_CREASE_DEG (lever c2): widen the set of edges the field ALIGNS to without
    // widening the set that becomes a hard seam.
    // CYBER_QC_FIELD_CREASE_DEG overrides the caller's value for A/B runs.
    float alignDeg = creaseAlignDegrees;
    if (const char* fc = std::getenv("CYBER_QC_FIELD_CREASE_DEG"); fc != nullptr) {
        alignDeg = static_cast<float>(std::atof(fc));
    }
    const float alignCos = alignDeg > 0.0f ? std::cos(degreesToRadians(alignDeg)) : 2.0f;
    const auto creaseAligned = [&](const EdgeId e) {
        if (alignCos > 1.0f || mesh.edgeFaceCount(e) != 2) {
            return false;
        }
        const auto ef = mesh.edgeFaces(e);
        return dot(normalized(mesh.faceNormal(ef[0])), normalized(mesh.faceNormal(ef[1]))) <
               alignCos;
    };

    // Constrain faces touching a feature or boundary edge to align with it.
    std::vector<char> constrained(nf, 0);
    for (std::size_t c = 0; c < nf; ++c) {
        const FaceId f = faces[c];
        const std::vector<VertexId> fv = mesh.faceVertices(f);
        for (std::size_t k = 0; k < fv.size(); ++k) {
            const EdgeId e = mesh.edgeBetween(fv[k], fv[(k + 1) % fv.size()]);
            if (!e.valid() ||
                (!mesh.isFeatureEdge(e) && mesh.edgeFaceCount(e) == 2 && !creaseAligned(e))) {
                continue;  // interior non-feature, non-crease edge imposes no constraint
            }
            const auto [a, b] = mesh.edgeVertices(e);
            const Vec3 d = normalized(mesh.position(b) - mesh.position(a));
            const float alpha = frameAngle(d, field.tangent[f.value], field.bitangent[f.value]);
            field.real[f.value] = std::cos(4.0f * alpha);
            field.imag[f.value] = std::sin(4.0f * alpha);
            constrained[c] = 1;
            break;
        }
    }

    // Build the 2F x 2F transport-averaging operator as CSR: row 2c/2c+1 hold
    // the real/imag equations for face c. The diagonal is a small self-damping
    // term (kSelf); each interior non-feature neighbour contributes a 2x2 rotation
    // that transports its cross into this face's frame. Smaller self-damping =>
    // heavier neighbour averaging => the iteration converges to a smoother (fewer
    // spurious singularities) harmonic field; the renormalise step keeps it a unit
    // 4-RoSy. kSelf and the sweep count are env-tunable for calibration.
    const char* dampEnv = std::getenv("CYBER_QC_FIELD_DAMP");
    const float kSelf = dampEnv != nullptr ? static_cast<float>(std::atof(dampEnv)) : 0.15f;
    std::vector<std::vector<std::pair<std::size_t, float>>> rows(2 * nf);
    for (std::size_t c = 0; c < nf; ++c) {
        rows[2 * c].emplace_back(2 * c, kSelf);
        rows[2 * c + 1].emplace_back(2 * c + 1, kSelf);
    }
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (!mesh.isAlive(e) || mesh.isFeatureEdge(e) || mesh.edgeFaceCount(e) != 2) {
            continue;
        }
        const auto ef = mesh.edgeFaces(e);
        const Index cf = compact[ef[0].value];
        const Index cg = compact[ef[1].value];
        if (cf == kInvalidIndex || cg == kInvalidIndex) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(e);
        const Vec3 d = normalized(mesh.position(b) - mesh.position(a));
        const float af = frameAngle(d, field.tangent[ef[0].value], field.bitangent[ef[0].value]);
        const float ag = frameAngle(d, field.tangent[ef[1].value], field.bitangent[ef[1].value]);
        // Transport g -> f rotates by 4*(af-ag); f -> g by the negative.
        const auto addBlock = [&rows](Index row, Index col, float phi) {
            const float cphi = std::cos(phi), sphi = std::sin(phi);
            rows[2 * row].emplace_back(2 * col, cphi);
            rows[2 * row].emplace_back(2 * col + 1, -sphi);
            rows[2 * row + 1].emplace_back(2 * col, sphi);
            rows[2 * row + 1].emplace_back(2 * col + 1, cphi);
        };
        addBlock(cf, cg, 4.0f * (af - ag));
        addBlock(cg, cf, 4.0f * (ag - af));
    }

    accel::SparseMatrix mat;
    mat.rows = 2 * nf;
    mat.rowStart.reserve(2 * nf + 1);
    mat.rowStart.push_back(0);
    for (const auto& row : rows) {
        for (const auto& [col, val] : row) {
            mat.colIndex.push_back(col);
            mat.value.push_back(val);
        }
        mat.rowStart.push_back(mat.colIndex.size());
    }

    // Iterate: y = A * u (dispatched through the accel layer), renormalise each
    // face's cross, and re-pin the constrained faces.
    accel::Buffer<float> u(2 * nf), y;
    for (std::size_t c = 0; c < nf; ++c) {
        u[2 * c] = field.real[faces[c].value];
        u[2 * c + 1] = field.imag[faces[c].value];
    }
    const char* itersEnv = std::getenv("CYBER_QC_FIELD_ITERS");
    const int sweeps = itersEnv != nullptr ? std::atoi(itersEnv) : std::max(iterations, 120);
    for (int it = 0; it < sweeps; ++it) {
        accel::spmv(backend, mat, u, y);
        float maxDelta = 0.0f;
        for (std::size_t c = 0; c < nf; ++c) {
            if (constrained[c]) {
                continue;
            }
            const float re = y[2 * c];
            const float im = y[2 * c + 1];
            const float len = std::sqrt(re * re + im * im);
            if (len > 1e-12f) {
                const float nr = re / len;
                const float ni = im / len;
                const float d = std::abs(nr - u[2 * c]) + std::abs(ni - u[2 * c + 1]);
                maxDelta = std::max(maxDelta, d);
                u[2 * c] = nr;
                u[2 * c + 1] = ni;
            }
        }
        if (maxDelta < 1e-6f && it > 8) {
            break;  // converged: further sweeps do not move the field
        }
    }

    for (std::size_t c = 0; c < nf; ++c) {
        field.real[faces[c].value] = u[2 * c];
        field.imag[faces[c].value] = u[2 * c + 1];
    }
    return field;
}

namespace {

// Project v onto the plane with unit normal n and renormalise.
Vec3 projectUnitLocal(Vec3 v, Vec3 n) { return normalized(v - n * dot(n, v)); }

// The 4-RoSy representative of tangent vector `d` (in the plane of n) that best
// aligns with `ref`: pick the 90-degree rotation (d, n x d, -d, -(n x d)) with
// the largest dot against ref.
Vec3 matchRoSyLocal(Vec3 ref, Vec3 d, Vec3 n) {
    Vec3 best = d;
    float bestDot = dot(ref, d);
    Vec3 cur = d;
    for (int k = 0; k < 3; ++k) {
        cur = cross(n, cur);  // rotate 90 degrees about n
        const float dd = dot(ref, cur);
        if (dd > bestDot) {
            bestDot = dd;
            best = cur;
        }
    }
    return best;
}

}  // namespace

CrossField computeCrossFieldFromOrientation(const Mesh& mesh, int iterations) {
    const std::size_t cap = mesh.faceCapacity();
    CrossField field;
    field.tangent.assign(cap, Vec3{1, 0, 0});
    field.bitangent.assign(cap, Vec3{0, 1, 0});
    field.real.assign(cap, 1.0f);
    field.imag.assign(cap, 0.0f);

    // The multiresolution per-vertex 4-RoSy orientation. spacing only drives the
    // position field, which we do not consume here, so pass a unit spacing.
    const PositionField pf = computePositionField(mesh, 1.0f, iterations);

    for (Index i = 0; i < cap; ++i) {
        const FaceId f{i};
        if (!mesh.isAlive(f) || mesh.faceSize(f) != 3) {
            continue;
        }
        const Vec3 n = mesh.faceNormal(f);
        const Vec3 t = faceTangent(mesh, f, n);
        field.tangent[i] = t;
        field.bitangent[i] = cross(n, t);

        // Average the three vertex orientations, projected into the face plane and
        // brought into a common 4-RoSy representative, then encode e^{i4theta}.
        const std::vector<VertexId> fv = mesh.faceVertices(f);
        Vec3 acc{0, 0, 0};
        Vec3 ref{0, 0, 0};
        bool haveRef = false;
        for (const VertexId v : fv) {
            if (v.value >= pf.q.size() || (v.value < pf.valid.size() && !pf.valid[v.value])) {
                continue;
            }
            const Vec3 qv = projectUnitLocal(pf.q[v.value], n);
            if (lengthSquared(qv) < 1e-12f) {
                continue;
            }
            if (!haveRef) {
                ref = qv;
                acc = qv;
                haveRef = true;
            } else {
                acc += matchRoSyLocal(ref, qv, n);
            }
        }
        if (!haveRef || lengthSquared(acc) < 1e-12f) {
            continue;  // no usable orientation -> leave the identity cross (theta 0)
        }
        const Vec3 dFace = projectUnitLocal(acc, n);
        const float theta = frameAngle(dFace, t, field.bitangent[i]);
        field.real[i] = std::cos(4.0f * theta);
        field.imag[i] = std::sin(4.0f * theta);
    }

    // Re-pin faces touching a feature/boundary edge exactly to it, matching
    // computeCrossField so feature meshes stay feature-aligned and well-conditioned.
    for (Index i = 0; i < cap; ++i) {
        const FaceId f{i};
        if (!mesh.isAlive(f) || mesh.faceSize(f) != 3) {
            continue;
        }
        const std::vector<VertexId> fv = mesh.faceVertices(f);
        for (std::size_t k = 0; k < fv.size(); ++k) {
            const EdgeId e = mesh.edgeBetween(fv[k], fv[(k + 1) % fv.size()]);
            if (!e.valid() || (!mesh.isFeatureEdge(e) && mesh.edgeFaceCount(e) == 2)) {
                continue;
            }
            const auto [a, b] = mesh.edgeVertices(e);
            const Vec3 d = normalized(mesh.position(b) - mesh.position(a));
            const float alpha = frameAngle(d, field.tangent[i], field.bitangent[i]);
            field.real[i] = std::cos(4.0f * alpha);
            field.imag[i] = std::sin(4.0f * alpha);
            break;
        }
    }
    return field;
}

}  // namespace cyber::remesh
