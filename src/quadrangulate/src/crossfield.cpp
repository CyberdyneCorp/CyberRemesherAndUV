#include "cyber/quadrangulate/crossfield.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>
#include <vector>

#include "cyber/accel/primitives.hpp"
#include "cyber/quadrangulate/position_field.hpp"

namespace cyber::remesh {

namespace {

// How far a neighbouring face's normal may tilt before the neighbourhood counts as curved rather
// than a flat panel. Deliberately tiny: a cube's same-panel neighbours are exactly coplanar, so
// anything with real curvature is excluded by a fraction of a degree.
constexpr float kPlanarNeighbourDegrees = 1.0f;

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

namespace {

// Result of the shared prologue every cross-field solver in this file runs before it
// builds its own smoothing operator: per-face frames written into `field`, feature/crease
// pins applied to field.real/imag, and the face list + pin mask that index the operator.
struct FieldSetup {
    std::vector<FaceId> faces;      // live triangle faces, in order
    std::vector<Index> compact;     // face.value -> compact index (kInvalidIndex if not live)
    std::vector<char> constrained;  // per compact face: pinned to a feature/crease edge?
    std::size_t nf = 0;
};

// Builds the per-face tangent frame into `field`, then pins every face incident to a
// feature/boundary/crease edge to that edge's direction (honouring the planarity gate of
// lever c2). computeCrossField and computeCrossFieldKnoppelCrane share this verbatim so KC
// pins EXACTLY the same faces to the same values as the iterative field (c1/c2 unchanged).
FieldSetup buildFrameAndConstraints(const Mesh& mesh, float creaseAlignDegrees, CrossField& field) {
    const std::size_t cap = mesh.faceCapacity();
    field.tangent.assign(cap, Vec3{1, 0, 0});
    field.bitangent.assign(cap, Vec3{0, 1, 0});
    field.real.assign(cap, 1.0f);
    field.imag.assign(cap, 0.0f);

    FieldSetup setup;

    // Per-face frame; collect the live faces.
    std::vector<FaceId>& faces = setup.faces;
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
    setup.nf = nf;
    if (nf == 0) {
        return setup;
    }

    // Compact index per live face.
    std::vector<Index>& compact = setup.compact;
    compact.assign(cap, kInvalidIndex);
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

    // PLANARITY GATE for crease alignment. Crease pins help a CURVED surface and hurt a FLAT one:
    // measured, they freeze 52.0% of fandisk's faces and improve it, but only 14.7% of a
    // subdivided cube's and take its edge CV 0.201 -> 0.397. The reason is not the amount of
    // constraint (fandisk freezes 3.5x more) — it is that a planar panel's cross field is
    // DEGENERATE. Every orientation is equally smooth there, so pinning a border band imposes
    // arbitrary structure that the free interior cannot reconcile, and the cells go uneven. On a
    // curved surface the field already has a curvature-driven preference that the pins reinforce.
    //
    // So ask about the SURFACE, not the constraint: look across this face's non-crease edges only
    // — deliberately not across the crease, whose whole point is to be a fold — and skip the pin
    // when every own-side neighbour is coplanar. The separation is clean rather than delicate: a
    // cube's same-panel neighbours are EXACTLY coplanar (0 degrees), while any curvature at all
    // puts fandisk past a fraction of a degree.
    const float planarCos = std::cos(degreesToRadians(kPlanarNeighbourDegrees));
    const float sameSideCos = std::cos(degreesToRadians(45.0f));
    const auto planarNeighbourhood = [&](const FaceId f, const std::vector<VertexId>& fv) {
        const Vec3 n = normalized(mesh.faceNormal(f));
        // Use the VERTEX ring, not the edge ring. A crease-adjacent triangle's edge-neighbours can
        // all lie in the same row of the fold, and a developable surface is exactly coplanar along
        // that row — so an edge-only test reads a curved cylinder section as planar and gates the
        // pin off wherever it is actually wanted. The vertex ring reaches the faces on the other
        // side of that row, which is where the curvature shows up.
        //
        // Faces past `sameSideCos` are on the far side of the fold; the fold is the crease itself,
        // not evidence about this face's own panel, so they are excluded rather than counted as
        // curvature.
        for (const VertexId v : fv) {
            for (const FaceId g : mesh.vertexFaces(v)) {
                if (g.value == f.value || !mesh.isAlive(g) || mesh.faceSize(g) != 3) {
                    continue;
                }
                const float d = dot(n, normalized(mesh.faceNormal(g)));
                if (d < sameSideCos) {
                    continue;  // across the fold: not own-side evidence
                }
                if (d < planarCos) {
                    return false;  // an own-side neighbour bends away: genuinely curved
                }
            }
        }
        return true;
    };

    std::size_t dbgCrease = 0, dbgGated = 0, dbgPinned = 0;
    // Constrain faces touching a feature or boundary edge to align with it.
    setup.constrained.assign(nf, 0);
    std::vector<char>& constrained = setup.constrained;
    for (std::size_t c = 0; c < nf; ++c) {
        const FaceId f = faces[c];
        const std::vector<VertexId> fv = mesh.faceVertices(f);
        for (std::size_t k = 0; k < fv.size(); ++k) {
            const EdgeId e = mesh.edgeBetween(fv[k], fv[(k + 1) % fv.size()]);
            const bool isCrease = e.valid() && !mesh.isFeatureEdge(e) &&
                                  mesh.edgeFaceCount(e) == 2 && creaseAligned(e);
            if (!e.valid() || (!mesh.isFeatureEdge(e) && mesh.edgeFaceCount(e) == 2 && !isCrease)) {
                continue;  // interior non-feature, non-crease edge imposes no constraint
            }
            if (isCrease) {
                ++dbgCrease;
                if (planarNeighbourhood(f, fv)) {
                    ++dbgGated;
                    continue;  // flat panel: field degenerate here, a pin would be arbitrary
                }
                ++dbgPinned;
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

    // CYBER_QC_FIELD_STATS: what fraction of the field the constraints freeze. This is what
    // diagnosed why crease alignment (lever c2) helps curved CAD but hurts flat CAD -- it is NOT
    // over-constraining. fandisk freezes 52% of its faces and improves; a subdivided cube freezes
    // only 14.7% and degrades (edge CV 0.201 -> 0.397). A flat panel's field is degenerate (every
    // orientation is equally smooth), so pinning a border band imposes arbitrary structure the
    // interior cannot reconcile, while a curved surface has a curvature-driven preference that
    // crease pins reinforce. See docs/ROADMAP.md Phase 3 lever c2.
    if (std::getenv("CYBER_QC_FIELD_STATS") != nullptr) {
        std::size_t nPinned = 0;
        for (std::size_t c = 0; c < nf; ++c) {
            nPinned += constrained[c] != 0 ? std::size_t{1} : std::size_t{0};
        }
        std::fprintf(stderr,
                     "[field] creaseEdgesSeen=%zu gatedOff=%zu creasePinned=%zu | "
                     "faces=%zu constrained=%zu (%.1f%%) alignDeg=%.0f\n",
                     dbgCrease, dbgGated, dbgPinned, nf, nPinned,
                     100.0 * static_cast<double>(nPinned) / static_cast<double>(nf),
                     static_cast<double>(alignDeg));
    }

    return setup;
}

}  // namespace

CrossField computeCrossField(const Mesh& mesh, int iterations, accel::IBackend& backend,
                             float creaseAlignDegrees) {
    CrossField field;
    const FieldSetup setup = buildFrameAndConstraints(mesh, creaseAlignDegrees, field);
    const std::size_t nf = setup.nf;
    if (nf == 0) {
        return field;
    }
    const std::vector<FaceId>& faces = setup.faces;
    const std::vector<Index>& compact = setup.compact;
    const std::vector<char>& constrained = setup.constrained;

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

// Conjugate Gradient for an SPD CSR system A x = b via accel spmv (private copy of the
// seamless solver's CG so crossfield.cpp needs no cross-TU dependency; identical math).
// Returns the iteration count and the final relative residual in *outResidual; x is seeded
// with its incoming value.
int conjugateGradientLocal(accel::IBackend& backend, const accel::SparseMatrix& A,
                           const std::vector<float>& b, std::vector<float>& x, int maxIters,
                           float tol, double* outResidual) {
    const std::size_t n = A.rows;
    accel::Buffer<float> xb(std::vector<float>(x.begin(), x.end()));
    accel::Buffer<float> ax;
    accel::spmv(backend, A, xb, ax);
    std::vector<float> r(n), p(n);
    double rs = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        r[i] = b[i] - ax[i];
        p[i] = r[i];
        rs += static_cast<double>(r[i]) * r[i];
    }
    const double rs0 = rs;
    if (outResidual != nullptr) {
        *outResidual = rs0 > 0.0 ? 1.0 : 0.0;
    }
    if (rs0 <= 0.0) {
        return 0;
    }
    int it = 0;
    accel::Buffer<float> pb, apb;
    for (; it < maxIters; ++it) {
        pb.upload(p);
        accel::spmv(backend, A, pb, apb);
        double pAp = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            pAp += static_cast<double>(p[i]) * apb[i];
        }
        if (pAp <= 0.0) {
            break;  // indefinite: CG cannot proceed (caller treats as divergence)
        }
        const double alpha = rs / pAp;
        double rsNew = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            x[i] += static_cast<float>(alpha) * p[i];
            r[i] -= static_cast<float>(alpha * apb[i]);
            rsNew += static_cast<double>(r[i]) * r[i];
        }
        if (rsNew <= static_cast<double>(tol) * static_cast<double>(tol) * rs0) {
            rs = rsNew;
            ++it;
            break;
        }
        const double beta = rsNew / rs;
        for (std::size_t i = 0; i < n; ++i) {
            p[i] = r[i] + static_cast<float>(beta) * p[i];
        }
        rs = rsNew;
    }
    if (outResidual != nullptr) {
        *outResidual = std::sqrt(rs / rs0);
    }
    return it;
}

// Inverse power iteration for the smallest generalized eigenpair of (M, B): the smoothest
// 4-RoSy field is the eigenvector of the smallest generalized eigenvalue of the connection
// Laplacian, and inverse iteration finds it by repeatedly solving M x = B u then B-normalising.
// M is the shifted connection Laplacian (SPD via the eps*B shift + penalty pins); B is the
// diagonal lumped mass (per DOF in `Bmass2`); `pinRhs` carries the penalty RHS for pinned DOFs.
// `u` is seeded (warm start) and overwritten with the converged eigenvector (un-renormalised
// per face). Returns false on divergence (non-finite / indefinite CG) so the caller can fall
// back to the iterative field -- KC can only help or no-op, never regress.
bool solveSmallestField(accel::IBackend& backend, const accel::SparseMatrix& M,
                        const std::vector<double>& Bmass2, const std::vector<float>& pinRhs,
                        std::vector<float>& u, int outerIters, float cgTol) {
    const std::size_t n = M.rows;
    std::vector<float> x(u.begin(), u.end());
    std::vector<float> rhs(n);
    const bool stats = std::getenv("CYBER_QC_FIELD_STATS") != nullptr;
    double prevLambda = std::numeric_limits<double>::infinity();
    double lambda = 0.0;
    int totalInner = 0;
    int outerDone = 0;
    for (int k = 0; k < outerIters; ++k) {
        for (std::size_t i = 0; i < n; ++i) {
            rhs[i] = static_cast<float>(Bmass2[i] * static_cast<double>(u[i])) + pinRhs[i];
        }
        // Loosen the CG tolerance on early outer steps (the eigenvector is still coarse) so
        // inner iteration counts collapse; tighten as the outer iteration settles.
        const float innerTol = k < 3 ? std::max(cgTol, 1e-3f) : cgTol;
        double res = 0.0;
        const int inner = conjugateGradientLocal(backend, M, rhs, x, 800, innerTol, &res);
        totalInner += inner;
        ++outerDone;
        if (!std::isfinite(res) || res > 0.5) {
            if (stats) {
                std::fprintf(stderr, "[kc] CG diverged at outer=%d residual=%g -> fallback\n", k,
                             res);
            }
            return false;
        }
        double s2 = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            s2 += Bmass2[i] * static_cast<double>(x[i]) * static_cast<double>(x[i]);
        }
        if (!(s2 > 0.0) || !std::isfinite(s2)) {
            if (stats) {
                std::fprintf(stderr, "[kc] B-norm collapsed at outer=%d -> fallback\n", k);
            }
            return false;
        }
        const double s = std::sqrt(s2);
        for (std::size_t i = 0; i < n; ++i) {
            u[i] = static_cast<float>(static_cast<double>(x[i]) / s);
        }
        // Rayleigh quotient lambda = u^T M u / u^T B u (== 1 after B-normalisation).
        accel::Buffer<float> ub(std::vector<float>(u.begin(), u.end())), mub;
        accel::spmv(backend, M, ub, mub);
        double num = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            num += static_cast<double>(u[i]) * static_cast<double>(mub[i]);
        }
        lambda = num;  // denominator is 1 after normalisation
        if (!std::isfinite(lambda)) {
            return false;
        }
        if (k >= 2 && std::abs(lambda - prevLambda) < 1e-4 * (1.0 + std::abs(lambda))) {
            prevLambda = lambda;
            break;  // Rayleigh quotient stabilised: converged
        }
        prevLambda = lambda;
    }
    if (stats) {
        std::fprintf(stderr, "[kc] inverse-iteration outer=%d innerCG=%d lambda=%.6g\n", outerDone,
                     totalInner, lambda);
    }
    return true;
}

}  // namespace

// Knoppel-Crane globally-optimal 4-RoSy field on the PER-FACE (dual) connection Laplacian
// (docs/ROADMAP.md Phase 3 lever c7). Builds the true connection Laplacian L = D - W(transport)
// over the same per-face frames, transport phases and feature/crease pins the iterative field
// uses, then takes its smallest generalized eigenvector via inverse iteration on the existing
// spmv+CG -- the globally-optimal smoothest field, which is known to place fewer, better-located
// cones than local relaxation. Gated behind CYBER_QC_KC_FIELD; falls back to computeCrossField if
// the eigensolver diverges, so it can only help or no-op.
CrossField computeCrossFieldKnoppelCrane(const Mesh& mesh, int iterations, accel::IBackend& backend,
                                         float creaseAlignDegrees) {
    // Seed / warm-start from the iterative field, and reuse it verbatim as the divergence fallback.
    CrossField seed = computeCrossField(mesh, iterations, backend, creaseAlignDegrees);

    CrossField field;
    const FieldSetup setup = buildFrameAndConstraints(mesh, creaseAlignDegrees, field);
    const std::size_t nf = setup.nf;
    if (nf == 0) {
        return field;
    }
    const std::vector<FaceId>& faces = setup.faces;
    const std::vector<Index>& compact = setup.compact;
    const std::vector<char>& constrained = setup.constrained;

    // Per-face lumped mass (triangle area) -> the diagonal generalized-mass B.
    std::vector<double> Bmass(nf, 0.0);
    double meanB = 0.0;
    for (std::size_t c = 0; c < nf; ++c) {
        const std::vector<VertexId> fv = mesh.faceVertices(faces[c]);
        const Vec3 e1 = mesh.position(fv[1]) - mesh.position(fv[0]);
        const Vec3 e2 = mesh.position(fv[2]) - mesh.position(fv[0]);
        Bmass[c] = 0.5 * static_cast<double>(length(cross(e1, e2)));
        meanB += Bmass[c];
    }
    meanB /= static_cast<double>(nf);
    if (!(meanB > 0.0)) {
        return seed;  // degenerate geometry: keep the iterative field
    }

    // Connection Laplacian L = D - W(transport): off-diagonal block (c,g) = -w * R(4(af-ag)),
    // degree diagonal D_c = sum_g w. The 2x2 interleaved block form IS the real-symmetric
    // [[Re,-Im],[Im,Re]] embedding, so M is genuinely symmetric (block(g,c) = block(c,g)^T) and
    // x^T L x = sum_edges w ||u_c - R u_g||^2 >= 0 (PSD). CYBER_QC_KC_UNIFORM_W keeps w=1 to
    // isolate the algorithm from weighting during bring-up.
    std::vector<std::vector<std::pair<std::size_t, float>>> rows(2 * nf);
    std::vector<double> deg(nf, 0.0);
    const auto addBlockNeg = [&rows](Index r, Index c, float phi, float w) {
        const float cphi = std::cos(phi) * w;
        const float sphi = std::sin(phi) * w;
        rows[2 * r].emplace_back(2 * c, -cphi);
        rows[2 * r].emplace_back(2 * c + 1, sphi);
        rows[2 * r + 1].emplace_back(2 * c, -sphi);
        rows[2 * r + 1].emplace_back(2 * c + 1, -cphi);
    };
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
        const float w = 1.0f;  // uniform weights (dual-cotan is the documented upgrade)
        addBlockNeg(cf, cg, 4.0f * (af - ag), w);
        addBlockNeg(cg, cf, 4.0f * (ag - af), w);
        deg[cf] += w;
        deg[cg] += w;
    }

    // eps*B Tikhonov shift makes M strictly SPD (L alone is singular at the target field so
    // CG's pAp>0 precondition would fail); penalty pins impose the feature/crease constraints
    // as hard Dirichlet BCs while keeping M SPD.
    const double eps = 1e-4 * meanB;
    double meanDiag = 0.0;
    for (std::size_t c = 0; c < nf; ++c) {
        meanDiag += deg[c] + eps * Bmass[c];
    }
    meanDiag /= static_cast<double>(nf);
    const double penalty = 1e6 * std::max(meanDiag, 1e-12);
    for (std::size_t c = 0; c < nf; ++c) {
        const double diag = deg[c] + eps * Bmass[c] + (constrained[c] ? penalty : 0.0);
        rows[2 * c].emplace_back(2 * c, static_cast<float>(diag));
        rows[2 * c + 1].emplace_back(2 * c + 1, static_cast<float>(diag));
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

    // Per-DOF mass and penalty RHS.
    std::vector<double> Bmass2(2 * nf);
    std::vector<float> pinRhs(2 * nf, 0.0f);
    for (std::size_t c = 0; c < nf; ++c) {
        Bmass2[2 * c] = Bmass[c];
        Bmass2[2 * c + 1] = Bmass[c];
        if (constrained[c]) {
            pinRhs[2 * c] = static_cast<float>(penalty * field.real[faces[c].value]);
            pinRhs[2 * c + 1] = static_cast<float>(penalty * field.imag[faces[c].value]);
        }
    }

    // Warm-start the eigenvector from the iterative field.
    std::vector<float> u(2 * nf);
    for (std::size_t c = 0; c < nf; ++c) {
        u[2 * c] = seed.real[faces[c].value];
        u[2 * c + 1] = seed.imag[faces[c].value];
    }

    const char* outerEnv = std::getenv("CYBER_QC_KC_OUTER");
    const int outerIters = outerEnv != nullptr ? std::max(1, std::atoi(outerEnv)) : 20;
    if (!solveSmallestField(backend, mat, Bmass2, pinRhs, u, outerIters, 1e-6f)) {
        return seed;  // divergence guard: never regress below the iterative field
    }

    // Fill the per-face field, renormalising each face's cross to unit. GUARD near-zero faces
    // (a ~0 (real,imag) yields a garbage atan2 angle and a spurious period jump): keep the
    // iterative seed's value there.
    for (std::size_t c = 0; c < nf; ++c) {
        const float re = u[2 * c];
        const float im = u[2 * c + 1];
        const float len = std::sqrt(re * re + im * im);
        if (len > 1e-9f) {
            field.real[faces[c].value] = re / len;
            field.imag[faces[c].value] = im / len;
        } else {
            field.real[faces[c].value] = seed.real[faces[c].value];
            field.imag[faces[c].value] = seed.imag[faces[c].value];
        }
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
