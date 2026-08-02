#include "cyber/quadrangulate/crossfield.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

// Per-face tangent frames for the live triangles; returns the live faces and fills `compact`
// with each face's dense index.
std::vector<FaceId> initFrames(const Mesh& mesh, CrossField& field, std::vector<Index>& compact) {
    const std::size_t cap = mesh.faceCapacity();
    std::vector<FaceId> faces;
    compact.assign(cap, kInvalidIndex);
    for (Index i = 0; i < cap; ++i) {
        const FaceId f{i};
        if (!mesh.isAlive(f) || mesh.faceSize(f) != 3) {
            continue;
        }
        const Vec3 n = mesh.faceNormal(f);
        const Vec3 t = faceTangent(mesh, f, n);
        field.tangent[i] = t;
        field.bitangent[i] = cross(n, t);
        compact[f.value] = static_cast<Index>(faces.size());
        faces.push_back(f);
    }
    return faces;
}

// Constrains faces touching a feature/boundary edge (and, with the planarity gate, a crease-
// aligned interior edge) to align exactly with it, then flood-fills crease pins across flat
// panels. Overwrites the pinned faces' cross values and returns the constrained flags (dense,
// per `faces` index). Shared verbatim by computeCrossField (its historical body) and by the
// multires hand-off so both paths pin identically.
std::vector<char> applyPins(const Mesh& mesh, const std::vector<FaceId>& faces,
                            const std::vector<Index>& compact, CrossField& field,
                            float creaseAlignDegrees);

// The converged damped transport-averaging solve shared by computeCrossField (its historical
// sweep loop, byte-identical) and the multires hand-off relax: builds the 2F x 2F CSR
// neighbour-averaging operator from the CURRENT field frames and iterates from the field's
// current real/imag until maxDelta < 1e-6 (min 9 sweeps, cap `sweepCap`), re-pinning the
// constrained faces every sweep. CYBER_QC_FIELD_ITERS overrides the cap for calibration.
void transportSmooth(const Mesh& mesh, const std::vector<FaceId>& faces,
                     const std::vector<Index>& compact, const std::vector<char>& constrained,
                     CrossField& field, int sweepCap, accel::IBackend& backend);

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

    std::vector<Index> compact;
    const std::vector<FaceId> faces = initFrames(mesh, field, compact);
    if (faces.empty()) {
        return field;
    }
    const std::vector<char> constrained =
        applyPins(mesh, faces, compact, field, creaseAlignDegrees);
    transportSmooth(mesh, faces, compact, constrained, field, std::max(iterations, 120), backend);
    return field;
}

namespace {

std::vector<char> applyPins(const Mesh& mesh, const std::vector<FaceId>& faces,
                            const std::vector<Index>& compact, CrossField& field,
                            float creaseAlignDegrees) {
    const std::size_t nf = faces.size();

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
    std::vector<char> constrained(nf, 0);
    // Faces whose pin came from an interior FEATURE edge (a crease): only
    // these seed the planar flood fill below. Boundary pins stay local —
    // spreading them across open flat scans measurably regresses the
    // open-surface cleanup suite (CV blow-up).
    std::vector<char> fillSeed(nf, 0);
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
            if (mesh.isFeatureEdge(e) && mesh.edgeFaceCount(e) == 2) {
                fillSeed[c] = 1;
            }
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

    // Planar flood fill: propagate feature/boundary pins across FLAT regions.
    // On a plane every constant field is equally smooth, so the discrete
    // smoothing has no geometric preference — on diagonally-triangulated flat
    // grids the discretization pulls the interior ~27-45 degrees off-axis
    // (native-miq-plan "still open"), which is also why a pinned border band
    // alone REGRESSED flat CAD (ROADMAP lever c2: cube CV 0.201->0.397): the
    // pinned ring and the diagonal interior fight, and the blend shears the
    // grid. Extending the pin as a CONSTANT field across the whole coplanar
    // patch removes the conflict instead of surrendering the alignment (the
    // planarity gate's answer). Curved regions are untouched: the fill stops
    // at any non-planar edge. Kill switch: CYBER_QC_NO_PLANAR_FILL.
    if (std::getenv("CYBER_QC_NO_PLANAR_FILL") == nullptr) {
        std::vector<std::size_t> queue;
        for (std::size_t c = 0; c < nf; ++c) {
            if (fillSeed[c] != 0) {
                queue.push_back(c);
            }
        }
        std::size_t head = 0;
        while (head < queue.size()) {
            const std::size_t c = queue[head++];
            const FaceId f = faces[c];
            const Vec3 nF = normalized(mesh.faceNormal(f));
            const float alpha = std::atan2(field.imag[f.value], field.real[f.value]) * 0.25f;
            const Vec3 dir3d = field.tangent[f.value] * std::cos(alpha) +
                               field.bitangent[f.value] * std::sin(alpha);
            const std::vector<VertexId> fv = mesh.faceVertices(f);
            for (std::size_t k = 0; k < fv.size(); ++k) {
                const EdgeId e = mesh.edgeBetween(fv[k], fv[(k + 1) % fv.size()]);
                if (!e.valid() || mesh.isFeatureEdge(e) || mesh.edgeFaceCount(e) != 2) {
                    continue;  // never spread across a crease or boundary
                }
                for (const FaceId g : mesh.edgeFaces(e)) {
                    if (g == f) {
                        continue;
                    }
                    const Index gc = compact[g.value];
                    if (gc == kInvalidIndex || constrained[gc] != 0) {
                        continue;
                    }
                    if (dot(nF, normalized(mesh.faceNormal(g))) < planarCos) {
                        continue;  // genuinely curved: leave to the smoother
                    }
                    const float beta =
                        frameAngle(dir3d, field.tangent[g.value], field.bitangent[g.value]);
                    field.real[g.value] = std::cos(4.0f * beta);
                    field.imag[g.value] = std::sin(4.0f * beta);
                    constrained[gc] = 1;
                    queue.push_back(gc);
                }
            }
        }
        if (std::getenv("CYBER_QC_FIELD_STATS") != nullptr) {
            std::size_t nPinned = 0;
            for (std::size_t c = 0; c < nf; ++c) {
                nPinned += constrained[c] != 0 ? std::size_t{1} : std::size_t{0};
            }
            std::fprintf(stderr, "[field] planar fill -> constrained=%zu (%.1f%%)\n", nPinned,
                         100.0 * static_cast<double>(nPinned) / static_cast<double>(nf));
        }
    }
    return constrained;
}

void transportSmooth(const Mesh& mesh, const std::vector<FaceId>& faces,
                     const std::vector<Index>& compact, const std::vector<char>& constrained,
                     CrossField& field, int sweepCap, accel::IBackend& backend) {
    const std::size_t nf = faces.size();

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
    const int sweeps = itersEnv != nullptr ? std::atoi(itersEnv) : sweepCap;
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
}

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

CrossField computeCrossFieldFromOrientation(const Mesh& mesh, int iterations,
                                            accel::IBackend& backend, float creaseAlignDegrees) {
    const std::size_t cap = mesh.faceCapacity();
    CrossField field;
    field.tangent.assign(cap, Vec3{1, 0, 0});
    field.bitangent.assign(cap, Vec3{0, 1, 0});
    field.real.assign(cap, 1.0f);
    field.imag.assign(cap, 0.0f);

    std::vector<Index> compact;
    const std::vector<FaceId> faces = initFrames(mesh, field, compact);
    if (faces.empty()) {
        return field;
    }

    // The multiresolution per-vertex 4-RoSy orientation. spacing only drives the
    // position field, which we do not consume here, so pass a unit spacing.
    const PositionField pf = computePositionField(mesh, 1.0f, iterations);

    for (const FaceId f : faces) {
        const Index i = f.value;
        const Vec3 n = mesh.faceNormal(f);

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
        const float theta = frameAngle(dFace, field.tangent[i], field.bitangent[i]);
        field.real[i] = std::cos(4.0f * theta);
        field.imag[i] = std::sin(4.0f * theta);
    }

    // Pin exactly as computeCrossField does (feature/boundary alignment, crease pins, planar
    // flood fill), then RELAX the vertex-to-face hand-off with the same converged transport
    // smoothing the stock path runs — seeded from the hierarchy instead of theta=0. The
    // hierarchy places the singularities globally (coarse levels annihilate stranded cone
    // pairs); the naive per-face vertex average it used to return, however, is unconverged at
    // the fine scale and measurably regressed edge CV / normal error (bunny: CV 0.29 -> 0.39,
    // Nerr 12.8 -> 16.5). Local averaging is a contraction, so the converged result inherits
    // the seed's cone placement while restoring stock-level fine-scale smoothness.
    const std::vector<char> constrained =
        applyPins(mesh, faces, compact, field, creaseAlignDegrees);
    transportSmooth(mesh, faces, compact, constrained, field, std::max(iterations, 120), backend);
    return field;
}

}  // namespace cyber::remesh
