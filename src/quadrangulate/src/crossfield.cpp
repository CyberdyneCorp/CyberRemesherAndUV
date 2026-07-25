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

// One dual-graph edge of the per-face connection Laplacian: transports face `cg`'s cross into
// face `cf`'s frame by R(phi) and carries weight `w`. Retained (not only baked into the CSR) so
// the inverse iteration can monitor the TRUE Dirichlet smoothness energy E = sum_e w||u_cf -
// R(phi) u_cg||^2 -- the quantity KC minimises -- separately from the penalty/shift terms that
// dominate the raw Rayleigh quotient of M (a rigor fix: "lambda stabilised" on M certifies the
// penalised solve, not the interior smoothness).
struct KcEdge {
    std::size_t cf;
    std::size_t cg;
    float phi;
    float w;
};

// Dirichlet smoothness energy of the field `u` over the dual connection graph: the sum the
// globally-optimal field minimises. With u B-normalised (u^T B u = 1) this IS the Dirichlet
// Rayleigh quotient, uncontaminated by the eps*B shift and P=1e6 penalty pins baked into M.
double dirichletEnergy(const std::vector<KcEdge>& edges, const std::vector<float>& u) {
    double e = 0.0;
    for (const KcEdge& ed : edges) {
        const float c = std::cos(ed.phi), s = std::sin(ed.phi);
        const float gr = u[2 * ed.cg], gi = u[2 * ed.cg + 1];
        const float tr = c * gr - s * gi;  // R(phi) u_cg
        const float ti = s * gr + c * gi;
        const float dr = u[2 * ed.cf] - tr;
        const float di = u[2 * ed.cf + 1] - ti;
        e += static_cast<double>(ed.w) * (static_cast<double>(dr) * dr + static_cast<double>(di) * di);
    }
    return e;
}

// Inverse power iteration for the smallest generalized eigenpair of (M, B): the smoothest
// 4-RoSy field is the eigenvector of the smallest generalized eigenvalue of the connection
// Laplacian, and inverse iteration finds it by repeatedly solving M x = B u then B-normalising.
// M is the shifted connection Laplacian (SPD via the eps*B shift + penalty pins); B is the
// diagonal lumped mass (per DOF in `Bmass2`); `pinRhs` carries the penalty RHS for pinned DOFs;
// `edges` re-evaluates the penalty-free Dirichlet energy for the convergence monitor.
// `u` is seeded (warm start) and overwritten with the converged eigenvector (un-renormalised
// per face). Returns false on divergence (non-finite / indefinite CG) so the caller can fall
// back to the iterative field -- KC can only help or no-op, never regress.
bool solveSmallestField(accel::IBackend& backend, const accel::SparseMatrix& M,
                        const std::vector<double>& Bmass2, const std::vector<float>& pinRhs,
                        const std::vector<KcEdge>& edges, std::vector<float>& u, int outerIters,
                        float cgTol) {
    const std::size_t n = M.rows;
    std::vector<float> x(u.begin(), u.end());
    std::vector<float> uPrev(u.begin(), u.end());
    std::vector<float> rhs(n);
    const bool stats = std::getenv("CYBER_QC_FIELD_STATS") != nullptr;
    double energy = 0.0;
    double deltaU = 0.0;
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
        uPrev.assign(u.begin(), u.end());
        for (std::size_t i = 0; i < n; ++i) {
            u[i] = static_cast<float>(static_cast<double>(x[i]) / s);
        }
        // Converge on the CHANGE IN THE FIELD, not the raw Rayleigh quotient of M (whose value is
        // dominated by the P=1e6 penalty and so never certifies the interior). deltaU is the
        // B-norm distance between successive normalised iterates, sign-disambiguated (inverse
        // iteration can flip the eigenvector's sign). energy is the honest smoothness KC minimises.
        double dPlus = 0.0, dMinus = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double a = static_cast<double>(u[i]) - static_cast<double>(uPrev[i]);
            const double b = static_cast<double>(u[i]) + static_cast<double>(uPrev[i]);
            dPlus += Bmass2[i] * a * a;
            dMinus += Bmass2[i] * b * b;
        }
        deltaU = std::sqrt(std::min(dPlus, dMinus));
        energy = dirichletEnergy(edges, u);
        if (!std::isfinite(energy)) {
            return false;
        }
        if (k >= 2 && deltaU < 1e-3) {
            break;  // field stabilised: the eigenvector stopped moving
        }
    }
    if (stats) {
        std::fprintf(stderr,
                     "[kc] inverse-iteration outer=%d innerCG=%d dirichletE=%.6g deltaU=%.3g\n",
                     outerDone, totalInner, energy, deltaU);
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

    // Per-face barycenter, for the dual-graph (finite-volume) edge weights below.
    std::vector<Vec3> centroid(nf);
    for (std::size_t c = 0; c < nf; ++c) {
        const std::vector<VertexId> fv = mesh.faceVertices(faces[c]);
        centroid[c] = (mesh.position(fv[0]) + mesh.position(fv[1]) + mesh.position(fv[2])) *
                      (1.0f / 3.0f);
    }

    // Connection Laplacian L = D - W(transport): off-diagonal block (c,g) = -w * R(4(af-ag)),
    // degree diagonal D_c = sum_g w. The 2x2 interleaved block form IS the real-symmetric
    // [[Re,-Im],[Im,Re]] embedding, so M is genuinely symmetric (block(g,c) = block(c,g)^T) and
    // x^T L x = sum_edges w ||u_c - R u_g||^2 >= 0 (PSD).
    //
    // Edge weight w. DEFAULT is uniform w=1. CYBER_QC_KC_DUAL_COTAN opts into the DEC-consistent
    // dual-graph finite-volume weight w = |shared edge| / dist(centroid_f, centroid_g) -- the
    // cotan-analogue for a per-FACE field (the Hodge star ⋆1 on the DUAL mesh), which discretizes
    // the smooth Dirichlet integral rather than counting faces. This is lever (i) of
    // docs/ROADMAP.md c7. MEASURED: on the near-uniform remeshing corpus the ratio |e|/|dual e| is
    // nearly constant (~1.8, equilateral triangles give 1.73), so it barely differs from uniform
    // and on the count-matched spot row it REGRESSES (irregular 3.81 -> 4.18, median 82.23 ->
    // 80.51, field cones 73 -> 75) -- a documented refutation, kept opt-in for reproducibility. The
    // ~2x spurious-singularity gap vs Geogram is dominated by the per-FACE discretization, not the
    // edge weighting; the paper-faithful per-VERTEX cotan build (lever ii) is the remaining lever.
    const bool uniformW = std::getenv("CYBER_QC_KC_DUAL_COTAN") == nullptr;
    std::vector<std::vector<std::pair<std::size_t, float>>> rows(2 * nf);
    std::vector<double> deg(nf, 0.0);
    std::vector<KcEdge> edges;
    const auto addBlockNeg = [&rows](Index r, Index c, float phi, float w) {
        const float cphi = std::cos(phi) * w;
        const float sphi = std::sin(phi) * w;
        rows[2 * r].emplace_back(2 * c, -cphi);
        rows[2 * r].emplace_back(2 * c + 1, sphi);
        rows[2 * r + 1].emplace_back(2 * c, -sphi);
        rows[2 * r + 1].emplace_back(2 * c + 1, -cphi);
    };
    double wSum = 0.0;
    std::size_t wCount = 0;
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
        const Vec3 ev = mesh.position(b) - mesh.position(a);
        const Vec3 d = normalized(ev);
        const float af = frameAngle(d, field.tangent[ef[0].value], field.bitangent[ef[0].value]);
        const float ag = frameAngle(d, field.tangent[ef[1].value], field.bitangent[ef[1].value]);
        float w = 1.0f;
        if (!uniformW) {
            const float edgeLen = length(ev);
            const float dualLen = length(centroid[cf] - centroid[cg]);
            w = dualLen > 1e-9f ? edgeLen / dualLen : 1.0f;
            if (!(w > 0.0f) || !std::isfinite(w)) {
                w = 1.0f;
            }
        }
        wSum += w;
        ++wCount;
        const float phiFG = 4.0f * (af - ag);
        addBlockNeg(cf, cg, phiFG, w);
        addBlockNeg(cg, cf, -phiFG, w);
        deg[cf] += w;
        deg[cg] += w;
        edges.push_back(KcEdge{static_cast<std::size_t>(cf), static_cast<std::size_t>(cg), phiFG, w});
    }
    if (std::getenv("CYBER_QC_FIELD_STATS") != nullptr) {
        std::fprintf(stderr, "[kc] weighting=%s dualEdges=%zu meanW=%.4g\n",
                     uniformW ? "uniform" : "dual-cotan", wCount,
                     wCount > 0 ? wSum / static_cast<double>(wCount) : 0.0);
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
    if (!solveSmallestField(backend, mat, Bmass2, pinRhs, edges, u, outerIters, 1e-6f)) {
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

namespace {

// An arbitrary unit tangent of the plane normal to `n` (matches position_field.cpp's anyTangent).
Vec3 anyTangentLocal(Vec3 n) {
    const Vec3 seed = std::fabs(n.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    return projectUnitLocal(seed, n);
}

// Per-vertex frames, lumped mass and feature/boundary pins for the paper-faithful per-VERTEX
// connection Laplacian. Mirrors buildFrameAndConstraints but on vertices: the field lives at
// vertices (as the paper prescribes), the mass is the barycentric vertex area, and a vertex on a
// feature/boundary edge is pinned to that edge's direction so features stay sharp.
struct VertexSetup {
    std::vector<VertexId> verts;    // live vertices, in order
    std::vector<Index> compact;     // vertex.value -> compact index (kInvalidIndex if not live)
    std::vector<Vec3> tangent;      // per-vertex tangent frame
    std::vector<Vec3> bitangent;    // normal x tangent
    std::vector<Vec3> normal;       // area-summed vertex normal
    std::vector<double> mass;       // barycentric vertex area -> diagonal generalized mass B
    std::vector<char> constrained;  // pinned to a feature/boundary edge?
    std::vector<float> pinReal;     // pin value cos(4a)
    std::vector<float> pinImag;     // pin value sin(4a)
    std::size_t nv = 0;
};

VertexSetup buildVertexSetup(const Mesh& mesh) {
    const std::size_t cap = mesh.vertexCapacity();
    VertexSetup s;
    s.compact.assign(cap, kInvalidIndex);
    for (Index i = 0; i < cap; ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v) || mesh.vertexFaces(v).empty()) {
            continue;
        }
        Vec3 n{0, 0, 0};
        for (const FaceId f : mesh.vertexFaces(v)) {
            if (mesh.isAlive(f) && mesh.faceSize(f) == 3) {
                n += mesh.faceNormal(f);
            }
        }
        n = normalized(n);
        const Vec3 t = anyTangentLocal(n);
        s.compact[i] = static_cast<Index>(s.verts.size());
        s.verts.push_back(v);
        s.normal.push_back(n);
        s.tangent.push_back(t);
        s.bitangent.push_back(cross(n, t));
    }
    s.nv = s.verts.size();
    s.mass.assign(s.nv, 1e-9);
    s.constrained.assign(s.nv, 0);
    s.pinReal.assign(s.nv, 1.0f);
    s.pinImag.assign(s.nv, 0.0f);

    for (std::size_t k = 0; k < s.nv; ++k) {
        const VertexId v = s.verts[k];
        double m = 0.0;
        for (const FaceId f : mesh.vertexFaces(v)) {
            if (!mesh.isAlive(f) || mesh.faceSize(f) != 3) {
                continue;
            }
            const std::vector<VertexId> fv = mesh.faceVertices(f);
            const Vec3 e1 = mesh.position(fv[1]) - mesh.position(fv[0]);
            const Vec3 e2 = mesh.position(fv[2]) - mesh.position(fv[0]);
            m += static_cast<double>(0.5f * length(cross(e1, e2))) / 3.0;
        }
        if (m > 0.0) {
            s.mass[k] = m;
        }
        // Pin a vertex on a feature/boundary edge to that edge's direction (c1: feature keep).
        for (const EdgeId e : mesh.vertexEdges(v)) {
            if (!mesh.isAlive(e) || !(mesh.isFeatureEdge(e) || mesh.isBoundaryEdge(e))) {
                continue;
            }
            const auto [a, b] = mesh.edgeVertices(e);
            const VertexId other = (a == v ? b : a);
            const Vec3 d = projectUnitLocal(mesh.position(other) - mesh.position(v), s.normal[k]);
            if (lengthSquared(d) < 1e-12f) {
                continue;
            }
            const float alpha = frameAngle(d, s.tangent[k], s.bitangent[k]);
            s.pinReal[k] = std::cos(4.0f * alpha);
            s.pinImag[k] = std::sin(4.0f * alpha);
            s.constrained[k] = 1;
            break;
        }
    }
    return s;
}

// Cotan edge weight (cot a + cot b)/2 over the (up to two) triangles incident to edge (va, vb).
// Negative weights (obtuse opposite angles) are clamped to 0 so the connection Laplacian stays
// PSD and CG's pAp>0 precondition holds (a standard cotan-Laplacian robustness fix).
float cotanEdgeWeight(const Mesh& mesh, EdgeId e, VertexId va, VertexId vb) {
    double cot = 0.0;
    for (const FaceId f : mesh.edgeFaces(e)) {
        if (!mesh.isAlive(f) || mesh.faceSize(f) != 3) {
            continue;
        }
        VertexId vc{kInvalidIndex};
        for (const VertexId x : mesh.faceVertices(f)) {
            if (x.value != va.value && x.value != vb.value) {
                vc = x;
            }
        }
        if (vc.value == kInvalidIndex) {
            continue;
        }
        const Vec3 u1 = mesh.position(va) - mesh.position(vc);
        const Vec3 u2 = mesh.position(vb) - mesh.position(vc);
        const float crossLen = length(cross(u1, u2));
        if (crossLen > 1e-12f) {
            cot += static_cast<double>(dot(u1, u2)) / static_cast<double>(crossLen);
        }
    }
    float w = static_cast<float>(0.5 * cot);
    if (!std::isfinite(w) || w < 0.0f) {
        w = 0.0f;
    }
    return w;
}

// Warm-start the per-vertex eigenvector `u` from the iterative face field: average each vertex's
// incident-face directions into its tangent frame (constrained vertices seed to their pin).
void seedVertexField(const Mesh& mesh, const VertexSetup& vs, const CrossField& seed,
                     std::vector<float>& u) {
    for (std::size_t k = 0; k < vs.nv; ++k) {
        if (vs.constrained[k]) {
            u[2 * k] = vs.pinReal[k];
            u[2 * k + 1] = vs.pinImag[k];
            continue;
        }
        Vec3 acc{0, 0, 0};
        Vec3 ref{0, 0, 0};
        bool have = false;
        for (const FaceId f : mesh.vertexFaces(vs.verts[k])) {
            if (!mesh.isAlive(f) || mesh.faceSize(f) != 3) {
                continue;
            }
            const Vec3 dir = projectUnitLocal(seed.direction(f), vs.normal[k]);
            if (lengthSquared(dir) < 1e-12f) {
                continue;
            }
            if (!have) {
                ref = dir;
                acc = dir;
                have = true;
            } else {
                acc += matchRoSyLocal(ref, dir, vs.normal[k]);
            }
        }
        if (have && lengthSquared(acc) > 1e-12f) {
            const Vec3 dv = projectUnitLocal(acc, vs.normal[k]);
            const float th = frameAngle(dv, vs.tangent[k], vs.bitangent[k]);
            u[2 * k] = std::cos(4.0f * th);
            u[2 * k + 1] = std::sin(4.0f * th);
        } else {
            u[2 * k] = 1.0f;
            u[2 * k + 1] = 0.0f;
        }
    }
}

// Project the converged per-vertex eigenvector `u` onto the faces the extractor reads: for each
// unconstrained face, average its three vertices' directions (brought into a common 4-RoSy
// representative) into the face frame; constrained faces keep the per-face pin already in `field`.
void projectVertexToFaces(const Mesh& mesh, const VertexSetup& vs, const FieldSetup& fsetup,
                          const std::vector<float>& u, const CrossField& seed, CrossField& field) {
    for (std::size_t c = 0; c < fsetup.nf; ++c) {
        const FaceId f = fsetup.faces[c];
        if (fsetup.constrained[c]) {
            continue;  // per-face feature/crease pin already written by buildFrameAndConstraints
        }
        const Vec3 n = normalized(mesh.faceNormal(f));
        Vec3 acc{0, 0, 0};
        Vec3 ref{0, 0, 0};
        bool have = false;
        for (const VertexId v : mesh.faceVertices(f)) {
            const Index vk = vs.compact[v.value];
            if (vk == kInvalidIndex) {
                continue;
            }
            const float th = std::atan2(u[2 * vk + 1], u[2 * vk]) / 4.0f;
            const Vec3 wdir = vs.tangent[vk] * std::cos(th) + vs.bitangent[vk] * std::sin(th);
            const Vec3 dir = projectUnitLocal(wdir, n);
            if (lengthSquared(dir) < 1e-12f) {
                continue;
            }
            if (!have) {
                ref = dir;
                acc = dir;
                have = true;
            } else {
                acc += matchRoSyLocal(ref, dir, n);
            }
        }
        if (!have || lengthSquared(acc) < 1e-12f) {
            field.real[f.value] = seed.real[f.value];  // guard: keep the iterative seed here
            field.imag[f.value] = seed.imag[f.value];
            continue;
        }
        const Vec3 df = projectUnitLocal(acc, n);
        const float th = frameAngle(df, field.tangent[f.value], field.bitangent[f.value]);
        field.real[f.value] = std::cos(4.0f * th);
        field.imag[f.value] = std::sin(4.0f * th);
    }
}

}  // namespace

CrossField computeCrossFieldKnoppelCraneVertex(const Mesh& mesh, int iterations,
                                               accel::IBackend& backend, float creaseAlignDegrees) {
    // Seed / warm-start / fallback = the iterative field (returned unchanged on any divergence).
    CrossField seed = computeCrossField(mesh, iterations, backend, creaseAlignDegrees);

    // Per-face frames + feature/crease pins for the final projection and output (c1/c2 unchanged).
    CrossField field;
    const FieldSetup fsetup = buildFrameAndConstraints(mesh, creaseAlignDegrees, field);
    if (fsetup.nf == 0) {
        return field;
    }

    const VertexSetup vs = buildVertexSetup(mesh);
    const std::size_t nv = vs.nv;
    if (nv == 0) {
        return seed;
    }
    double meanB = 0.0;
    for (const double m : vs.mass) {
        meanB += m;
    }
    meanB /= static_cast<double>(nv);
    if (!(meanB > 0.0)) {
        return seed;
    }

    // Connection Laplacian L = D - W(transport) over the primal mesh with cotan weights.
    // CYBER_QC_KC_VERTEX_UNIFORM forces w=1 to isolate cotan-weighting bugs from the algorithm.
    const bool uniformW = std::getenv("CYBER_QC_KC_VERTEX_UNIFORM") != nullptr;
    std::vector<std::vector<std::pair<std::size_t, float>>> rows(2 * nv);
    std::vector<double> deg(nv, 0.0);
    std::vector<KcEdge> edges;
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
        if (!mesh.isAlive(e) || mesh.isFeatureEdge(e)) {
            continue;  // feature edges are hard seams: no smoothing across them
        }
        const auto [va, vb] = mesh.edgeVertices(e);
        const Index ci = vs.compact[va.value];
        const Index cj = vs.compact[vb.value];
        if (ci == kInvalidIndex || cj == kInvalidIndex) {
            continue;
        }
        const float w = uniformW ? 1.0f : cotanEdgeWeight(mesh, e, va, vb);
        if (w <= 0.0f) {
            continue;
        }
        const Vec3 d = normalized(mesh.position(vb) - mesh.position(va));
        const float ai =
            frameAngle(projectUnitLocal(d, vs.normal[ci]), vs.tangent[ci], vs.bitangent[ci]);
        const float aj =
            frameAngle(projectUnitLocal(d, vs.normal[cj]), vs.tangent[cj], vs.bitangent[cj]);
        const float phi = 4.0f * (ai - aj);
        addBlockNeg(ci, cj, phi, w);
        addBlockNeg(cj, ci, -phi, w);
        deg[ci] += w;
        deg[cj] += w;
        edges.push_back(KcEdge{static_cast<std::size_t>(ci), static_cast<std::size_t>(cj), phi, w});
    }

    // eps*B Tikhonov shift (SPD) + penalty Dirichlet pins for the constrained vertices.
    const double eps = 1e-4 * meanB;
    double meanDiag = 0.0;
    for (std::size_t k = 0; k < nv; ++k) {
        meanDiag += deg[k] + eps * vs.mass[k];
    }
    meanDiag /= static_cast<double>(nv);
    const double penalty = 1e6 * std::max(meanDiag, 1e-12);
    for (std::size_t k = 0; k < nv; ++k) {
        const double diag = deg[k] + eps * vs.mass[k] + (vs.constrained[k] ? penalty : 0.0);
        rows[2 * k].emplace_back(2 * k, static_cast<float>(diag));
        rows[2 * k + 1].emplace_back(2 * k + 1, static_cast<float>(diag));
    }

    accel::SparseMatrix mat;
    mat.rows = 2 * nv;
    mat.rowStart.reserve(2 * nv + 1);
    mat.rowStart.push_back(0);
    for (const auto& row : rows) {
        for (const auto& [col, val] : row) {
            mat.colIndex.push_back(col);
            mat.value.push_back(val);
        }
        mat.rowStart.push_back(mat.colIndex.size());
    }

    std::vector<double> Bmass2(2 * nv);
    std::vector<float> pinRhs(2 * nv, 0.0f);
    for (std::size_t k = 0; k < nv; ++k) {
        Bmass2[2 * k] = vs.mass[k];
        Bmass2[2 * k + 1] = vs.mass[k];
        if (vs.constrained[k]) {
            pinRhs[2 * k] = static_cast<float>(penalty * static_cast<double>(vs.pinReal[k]));
            pinRhs[2 * k + 1] = static_cast<float>(penalty * static_cast<double>(vs.pinImag[k]));
        }
    }

    std::vector<float> u(2 * nv);
    seedVertexField(mesh, vs, seed, u);

    if (std::getenv("CYBER_QC_FIELD_STATS") != nullptr) {
        std::size_t nPinned = 0;
        for (const char c : vs.constrained) {
            nPinned += c != 0 ? 1u : 0u;
        }
        std::fprintf(stderr, "[kc] per-VERTEX build: verts=%zu edges=%zu weighting=%s pinned=%zu\n",
                     nv, edges.size(), uniformW ? "uniform" : "cotan", nPinned);
    }

    const char* outerEnv = std::getenv("CYBER_QC_KC_OUTER");
    const int outerIters = outerEnv != nullptr ? std::max(1, std::atoi(outerEnv)) : 20;
    if (!solveSmallestField(backend, mat, Bmass2, pinRhs, edges, u, outerIters, 1e-6f)) {
        return seed;  // divergence guard: never regress below the iterative field
    }

    projectVertexToFaces(mesh, vs, fsetup, u, seed, field);
    return field;
}

}  // namespace cyber::remesh
