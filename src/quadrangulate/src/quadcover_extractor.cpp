#include "cyber/quadrangulate/quadcover_extractor.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cyber/accel/backend.hpp"
#include "cyber/core/isotropic.hpp"
#include "cyber/core/math.hpp"
#include "cyber/core/reference_surface.hpp"
#include "cyber/quadrangulate/position_field.hpp"
#include "cyber/quadrangulate/seamless_solver.hpp"

#ifdef CYBER_HAVE_QUADCOVER
#include "autoremesher_solve.hpp"
#endif

// TASK F. The isoline extractor and the IQuadrangulator seam are still stubs
// (Milestones 2-3); Milestone 1 fills computeSeamlessUv by obtaining a seamless
// integer-grid UV out-of-process from AutoRemesher's Geogram quad_cover harness.
namespace cyber::remesh {

namespace {

#ifndef CYBER_HAVE_QUADCOVER
// The OBJ writer and UV-dump parser serve only the out-of-process subprocess path.
// With the in-process solver linked in they are unused, so compile them out to keep
// -Werror=unused-function happy.

// Parse the harness UV dump (autoremesher_harness.cpp writeUvDump): the isotropic
// mesh (V) plus per-triangle vertex indices and 3 corner UVs (T).
bool parseUvDump(const std::string& path, SeamlessUv& uv) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    char line[1024];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (line[0] == 'V') {
            long n = 0;
            std::sscanf(line + 1, "%ld", &n);
            for (long i = 0; i < n; ++i) {
                if (std::fgets(line, sizeof(line), f) == nullptr) {
                    std::fclose(f);
                    return false;
                }
                double x = 0, y = 0, z = 0;
                std::sscanf(line, "%lf %lf %lf", &x, &y, &z);
                uv.vertices.push_back(
                    Vec3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)});
            }
        } else if (line[0] == 'T') {
            long n = 0;
            std::sscanf(line + 1, "%ld", &n);
            for (long i = 0; i < n; ++i) {
                if (std::fgets(line, sizeof(line), f) == nullptr) {
                    std::fclose(f);
                    return false;
                }
                long a = 0, b = 0, c = 0;
                double u0 = 0, v0 = 0, u1 = 0, v1 = 0, u2 = 0, v2 = 0;
                std::sscanf(line, "%ld %ld %ld %lf %lf %lf %lf %lf %lf", &a, &b, &c, &u0, &v0, &u1,
                            &v1, &u2, &v2);
                uv.triangles.push_back(
                    {static_cast<Index>(a), static_cast<Index>(b), static_cast<Index>(c)});
                uv.triangleUv.push_back({Vec2{static_cast<float>(u0), static_cast<float>(v0)},
                                         Vec2{static_cast<float>(u1), static_cast<float>(v1)},
                                         Vec2{static_cast<float>(u2), static_cast<float>(v2)}});
            }
        }
    }
    std::fclose(f);
    return !uv.triangles.empty();
}

// Write a minimal Wavefront OBJ (triangles) for the harness to consume.
bool writeObjFor(const std::string& path, const Mesh& mesh, double& areaOut) {
    std::vector<Vec3> pos;
    std::vector<std::vector<Index>> faces;
    mesh.toIndexed(pos, faces);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    for (const Vec3& p : pos) {
        std::fprintf(f, "v %.9g %.9g %.9g\n", static_cast<double>(p.x), static_cast<double>(p.y),
                     static_cast<double>(p.z));
    }
    double area = 0.0;
    for (const auto& fc : faces) {
        std::fprintf(f, "f");
        for (const Index v : fc) {
            std::fprintf(f, " %u", static_cast<unsigned>(v) + 1U);
        }
        std::fprintf(f, "\n");
        for (std::size_t k = 1; k + 1 < fc.size(); ++k) {
            area += 0.5 * static_cast<double>(
                              length(cross(pos[fc[k]] - pos[fc[0]], pos[fc[k + 1]] - pos[fc[0]])));
        }
    }
    std::fclose(f);
    areaOut = area;
    return true;
}
#endif  // CYBER_HAVE_QUADCOVER

}  // namespace

namespace {

// Distance from point p to segment [a, b].
float pointSegmentDistance(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 ab = b - a;
    const float len2 = dot(ab, ab);
    if (len2 <= 1e-20f) {
        return length(p - a);
    }
    const float t = std::clamp(dot(p - a, ab) / len2, 0.0f, 1.0f);
    return length(p - (a + ab * t));
}

// Feature-pinning lever: the ORIGINAL mesh's sharp-edge polyline network, restricted to
// crease chains long enough to be RESOLVABLE at the output grid resolution. A genuine CAD
// crease (a cube edge, a cylinder rim) is a long chain the output grid should follow; an
// organic scan's over-threshold dihedrals are sub-resolution wrinkle fragments whose total
// chain length is far below one output cell — re-tagging them as hard seams after the
// coarse isotropic remesh is what blew nefertiti's singularities up under the lever (73
// lever-off -> 216: every coarse wrinkle became a seam + field pin). Chains shorter than
// `minChainLength` are dropped; what remains is the reference network the post-remesh
// feature re-tag is filtered against.
std::vector<std::array<Vec3, 2>> resolvableCreaseSegments(const Mesh& mesh, float minChainLength) {
    // Union-find over vertices joined by interior feature edges.
    std::unordered_map<Index, Index> parent;
    const std::function<Index(Index)> find = [&](Index x) {
        while (true) {
            auto it = parent.find(x);
            if (it == parent.end() || it->second == x) {
                return x;
            }
            x = it->second;
        }
    };
    struct Seg {
        Vec3 a, b;
        Index root;
        float len;
    };
    std::vector<Seg> segs;
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (!mesh.isAlive(e) || mesh.edgeFaceCount(e) != 2 || !mesh.isFeatureEdge(e)) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(e);
        const Index ra = find(a.value), rb = find(b.value);
        parent.emplace(a.value, a.value);
        parent.emplace(b.value, b.value);
        parent[ra] = rb;
        segs.push_back(
            {mesh.position(a), mesh.position(b), rb, length(mesh.position(b) - mesh.position(a))});
    }
    std::unordered_map<Index, float> chainLength;
    for (Seg& s : segs) {
        s.root = find(s.root);
        chainLength[s.root] += s.len;
    }
    std::vector<std::array<Vec3, 2>> out;
    for (const Seg& s : segs) {
        if (chainLength[s.root] >= minChainLength) {
            out.push_back({s.a, s.b});
        }
    }
    return out;
}

// Feature-pinning lever, post-remesh: keep an interior re-tagged feature edge if it either
// (a) traces the reference crease network (endpoints and midpoint all within `tol` — the
// isotropic stage keeps crease vertices ON the crease polyline, so genuine crease edges sit
// at distance ~0 while coarse-remesh dihedral noise does not), or (b) still qualifies at the
// HISTORICAL lever-off threshold (`fallbackDihedralDegrees`, knife edges) — so on organic
// meshes the lever-on feature set degrades exactly to the lever-off one instead of inventing
// (or dropping) seams the lever-off run never had. Boundary/non-manifold edges are never
// untagged (they are structural, not dihedral evidence).
void filterFeatureEdgesToReference(Mesh& mesh, const std::vector<std::array<Vec3, 2>>& network,
                                   float tol, float fallbackDihedralDegrees) {
    const float fallbackNormalAngle = degreesToRadians(180.0f - fallbackDihedralDegrees) - 1e-3f;
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (!mesh.isAlive(e) || mesh.edgeFaceCount(e) != 2 || !mesh.isFeatureEdge(e)) {
            continue;
        }
        const auto ef = mesh.edgeFaces(e);
        const float cosAngle =
            std::clamp(dot(normalized(mesh.faceNormal(ef[0])), normalized(mesh.faceNormal(ef[1]))),
                       -1.0f, 1.0f);
        if (std::acos(cosAngle) >= fallbackNormalAngle) {
            continue;  // knife edge: the lever-off tag would keep it too
        }
        const auto [a, b] = mesh.edgeVertices(e);
        const std::array<Vec3, 3> samples{mesh.position(a), mesh.position(b),
                                          (mesh.position(a) + mesh.position(b)) * 0.5f};
        bool onNetwork = !network.empty();
        for (const Vec3& p : samples) {
            if (!onNetwork) {
                break;
            }
            bool near = false;
            for (const std::array<Vec3, 2>& s : network) {
                if (pointSegmentDistance(p, s[0], s[1]) <= tol) {
                    near = true;
                    break;
                }
            }
            onNetwork = near;
        }
        if (!onNetwork) {
            mesh.setFeatureEdge(e, false);
        }
    }
}

// True when all three probe points of edge e (endpoints + midpoint) lie within `tol` of the
// segment network — the same tracing test filterFeatureEdgesToReference uses: the isotropic
// stage keeps crease vertices ON the source crease polyline, so genuine crease edges sit at
// distance ~0 while coarse-remesh dihedral noise does not.
bool edgeNearNetwork(const Mesh& mesh, EdgeId e, const std::vector<std::array<Vec3, 2>>& network,
                     float tol) {
    if (network.empty()) {
        return false;
    }
    const auto [a, b] = mesh.edgeVertices(e);
    const std::array<Vec3, 3> samples{mesh.position(a), mesh.position(b),
                                      (mesh.position(a) + mesh.position(b)) * 0.5f};
    for (const Vec3& p : samples) {
        bool near = false;
        for (const std::array<Vec3, 2>& s : network) {
            if (pointSegmentDistance(p, s[0], s[1]) <= tol) {
                near = true;
                break;
            }
        }
        if (!near) {
            return false;
        }
    }
    return true;
}

// CYBER_QC_FIELD_STATS: census of the post-remesh feature-edge web BEFORE demotion — the
// "pseudo-feature web" that saturated field-level dipole annihilation (gap #4: nefertiti's
// coarse remesh pinned a dense wrinkle web and 349 of 521 +cones never found a partner over
// non-pinned paths). Chains (connected components) of tagged interior feature edges are
// classified against the output resolution and the ORIGINAL mesh's resolvable sharp network:
//   resolvable — total length >= 2 output cells AND >= half its edges trace `refNetwork`;
//   longOffNet — resolvable-scale but NOT tracing the original network (remesh artifact ridge);
//   subres     — multi-edge chain shorter than ~2 output cells (sub-resolution wrinkle);
//   isolated   — single tagged edge.
void reportFeatureWebCensus(const Mesh& mesh, const std::vector<std::array<Vec3, 2>>& refNetwork,
                            float targetEdgeLength, float tol) {
    std::unordered_map<Index, Index> parent;
    const std::function<Index(Index)> find = [&](Index x) {
        while (true) {
            auto it = parent.find(x);
            if (it == parent.end() || it->second == x) {
                return x;
            }
            x = it->second;
        }
    };
    struct EdgeRec {
        EdgeId e;
        Index root;
    };
    std::vector<EdgeRec> tagged;
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (!mesh.isAlive(e) || mesh.edgeFaceCount(e) != 2 || !mesh.isFeatureEdge(e)) {
            continue;
        }
        const auto [a, b] = mesh.edgeVertices(e);
        const Index ra = find(a.value), rb = find(b.value);
        parent.emplace(a.value, a.value);
        parent.emplace(b.value, b.value);
        parent[ra] = rb;
        tagged.push_back({e, rb});
    }
    struct Chain {
        std::size_t edges = 0;
        std::size_t onNet = 0;
        float len = 0.0f;
    };
    std::unordered_map<Index, Chain> chains;
    for (EdgeRec& r : tagged) {
        r.root = find(r.root);
        Chain& c = chains[r.root];
        const auto [a, b] = mesh.edgeVertices(r.e);
        c.edges += 1;
        c.len += length(mesh.position(b) - mesh.position(a));
        c.onNet += edgeNearNetwork(mesh, r.e, refNetwork, tol) ? 1U : 0U;
    }
    const float resolvableLen = 2.0f * targetEdgeLength;
    std::size_t nRes = 0, eRes = 0, nOff = 0, eOff = 0, nSub = 0, eSub = 0, nIso = 0;
    for (const auto& [root, c] : chains) {
        if (c.edges == 1) {
            ++nIso;
        } else if (c.len < resolvableLen) {
            ++nSub;
            eSub += c.edges;
        } else if (2 * c.onNet >= c.edges) {
            ++nRes;
            eRes += c.edges;
        } else {
            ++nOff;
            eOff += c.edges;
        }
    }
    std::fprintf(stderr,
                 "[web] featureEdges=%zu chains=%zu | resolvable=%zu(%zu edges) "
                 "longOffNet=%zu(%zu edges) subres=%zu(%zu edges) isolated=%zu | refSegs=%zu "
                 "h=%.4g\n",
                 tagged.size(), chains.size(), nRes, eRes, nOff, eOff, nSub, eSub, nIso,
                 refNetwork.size(), static_cast<double>(targetEdgeLength));
}

// Second resolvability criterion: PERSISTENT BEND at output scale, measured on the ORIGINAL
// surface. The reference networks above are per-edge dihedrals on the original (fine) mesh,
// and they miss ridges that are smooth at original resolution but tighter than an output cell
// — the bunny ears: every original edge bends a few degrees, yet the coarse remesh
// legitimately reads the ridge as a crease, and demoting its pins measurably shreds the ears
// (ears 19 -> 64, sing 94 -> 208 with the network test alone). Discriminator: probe the
// original surface half a cell to each side of the work edge (perpendicular to it, in the
// tangent plane) and compare the hit faces' normals. A resolvable ridge stays bent across a
// cell (the ear flanks differ by ~60 degrees); sub-resolution relief re-aligns to the smooth
// carrier surface on both sides. Measured on the WORK mesh instead this test cannot work: the
// coarse remesh ALIASES sub-cell relief into cell-scale corrugation, so the work-mesh
// neighbourhood bend of a wrinkle looks exactly like a real ridge (nefertiti kept 3117 of
// 5214 wrinkle pins that way, cones 589 -> 1071).
bool bendPersistsAtScale(const Mesh& work, EdgeId e, const Mesh& original,
                         const ReferenceSurface& originalRef, float cellSize,
                         float minAngleDegrees) {
    const auto ef = work.edgeFaces(e);
    const auto [a, b] = work.edgeVertices(e);
    const Vec3 mid = (work.position(a) + work.position(b)) * 0.5f;
    const Vec3 tangent = normalized(work.position(b) - work.position(a));
    const Vec3 nAvg =
        normalized(normalized(work.faceNormal(ef[0])) + normalized(work.faceNormal(ef[1])));
    const Vec3 across = normalized(cross(tangent, nAvg));
    if (lengthSquared(across) < 0.5f) {
        return true;  // degenerate frame (opposed face normals): keep the pin, be conservative
    }
    // Three probe distances per side. A resolvable crease separates two COHERENT flanks (each
    // side's normals agree with each other out to a full cell) that differ across the crease;
    // cell-scale corrugation (nefertiti's carved stripes, armadillo's warts) fails the
    // within-side coherence — a single-distance probe cannot tell the two apart (a stripe
    // period ~= the cell puts opposite flanks under the probes and reads as a crease:
    // measured, it kept 2975 of nefertiti's 5214 wrinkle pins and cones went 589 -> 1021).
    constexpr std::array<float, 3> kProbeScales{0.3f, 0.6f, 1.0f};
    const float coherentCos = std::cos(degreesToRadians(20.0f));
    Vec3 sideMean[2];
    for (std::size_t s = 0; s < 2; ++s) {
        std::array<Vec3, kProbeScales.size()> n{};
        for (std::size_t k = 0; k < kProbeScales.size(); ++k) {
            const float sign = s == 0 ? 1.0f : -1.0f;
            const Vec3 q = mid + across * (sign * kProbeScales[k] * cellSize);
            const Bvh::ClosestHit hit = originalRef.closest(q);
            if (!hit.face.valid() || !original.isAlive(hit.face)) {
                return true;  // no reliable probe: keep the pin
            }
            n[k] = normalized(original.faceNormal(hit.face));
        }
        for (std::size_t i = 0; i < n.size(); ++i) {
            for (std::size_t j = i + 1; j < n.size(); ++j) {
                if (dot(n[i], n[j]) < coherentCos) {
                    return false;  // oscillating side: sub-resolution relief, demote
                }
            }
        }
        sideMean[s] = normalized(n[0] + n[1] + n[2]);
    }
    return dot(sideMean[0], sideMean[1]) < std::cos(degreesToRadians(minAngleDegrees));
}

// Resolution-aware feature DEMOTION, tag level (kill switch CYBER_QC_NO_FEATURE_DEMOTE, see
// prepareNativeSolve): untag interior feature edges that do NOT trace the original mesh's
// resolvable sharp network. Unlike filterFeatureEdgesToReference there is deliberately no
// knife-edge fallback — a knife-fold dihedral on the coarse remesh of a smooth surface is
// exactly the sub-resolution wrinkle the lever exists to demote (it cannot be resolved by the
// output grid, so it must not act as a hard seam, field pin or dipole barrier). Genuine CAD
// creases (box/cylinder/fandisk) sit ON the network at distance ~0 and are never demoted.
std::size_t demoteUnresolvableFeatureEdges(Mesh& mesh,
                                           const std::vector<std::array<Vec3, 2>>& network,
                                           float tol, const Mesh& original,
                                           const ReferenceSurface& originalRef,
                                           float sampleDistance, float persistDegrees) {
    std::size_t demoted = 0;
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (!mesh.isAlive(e) || mesh.edgeFaceCount(e) != 2 || !mesh.isFeatureEdge(e)) {
            continue;
        }
        if (!edgeNearNetwork(mesh, e, network, tol) &&
            !bendPersistsAtScale(mesh, e, original, originalRef, sampleDistance, persistDegrees)) {
            mesh.setFeatureEdge(e, false);
            ++demoted;
        }
    }
    return demoted;
}

// Resolution-aware feature DEMOTION, crease-alignment level: per-edge flags gating the field's
// 45-degree crease-ALIGNMENT pins (applyPins) and the dipole-annihilation barrier
// (fieldPinnedFaces) to edges tracing the original mesh's resolvable 45-degree network
// (`refAlign`). Only alignment CANDIDATES (face-normal angle above ~35 degrees, margin under
// the 45-degree default) are tested; everything else keeps flag 1 (irrelevant to the pin test,
// and permissive if CYBER_QC_FIELD_CREASE_DEG is lowered for an A/B). On nefertiti@4000 the
// wrinkle alignment froze 43% of the substrate faces — the measured cone factory.
std::vector<char> creaseAlignSupportFlags(const Mesh& mesh,
                                          const std::vector<std::array<Vec3, 2>>& network,
                                          float tol, const Mesh& original,
                                          const ReferenceSurface& originalRef, float sampleDistance,
                                          float persistDegrees) {
    std::vector<char> flags(mesh.edgeCapacity(), 1);
    const float candCos = std::cos(degreesToRadians(35.0f));
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (!mesh.isAlive(e) || mesh.edgeFaceCount(e) != 2) {
            continue;
        }
        const auto ef = mesh.edgeFaces(e);
        if (dot(normalized(mesh.faceNormal(ef[0])), normalized(mesh.faceNormal(ef[1]))) >=
            candCos) {
            continue;  // not an alignment candidate: flag value never consulted
        }
        flags[ei] = edgeNearNetwork(mesh, e, network, tol) ||
                            bendPersistsAtScale(mesh, e, original, originalRef, sampleDistance,
                                                persistDegrees)
                        ? 1
                        : 0;
    }
    return flags;
}

// Fold-tag filter. The isotropic remesh can leave FOLDED triangles in flat regions (a
// face lying in the plane of its neighbors but wound backwards — the valence-only flip
// pass has no geometric guard). A folded pair reads as a ~180-degree dihedral, so the
// post-remesh feature re-tag sees knife edges in the middle of flat CAD patches (the
// generated cylinder at ~1100 quads: 21 spurious feature seams across the caps,
// totalIndex 8 -> 5, angle_dev 28 degrees), and filterFeatureEdgesToReference's
// knife-edge exception deliberately keeps them. A fold is distinguishable from a real
// knife edge by ORIENTATION: exactly one side disagrees with the surrounding surface.
// Mark faces whose normal opposes the majority of their edge-neighbors as inverted and
// untag any feature edge with exactly one inverted side. Meshes without folds are
// untouched (tag-only, no geometry edits).
void untagFoldedFeatureEdges(Mesh& mesh) {
    std::vector<Vec3> normals(mesh.faceCapacity(), Vec3{0.0f, 0.0f, 0.0f});
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (mesh.isAlive(f)) {
            normals[fi] = normalized(mesh.faceNormal(f));
        }
    }
    std::vector<char> inverted(mesh.faceCapacity(), 0);
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f)) {
            continue;
        }
        int agree = 0;
        int disagree = 0;
        const std::vector<VertexId> verts = mesh.faceVertices(f);
        for (std::size_t k = 0; k < verts.size(); ++k) {
            const EdgeId e = mesh.edgeBetween(verts[k], verts[(k + 1) % verts.size()]);
            if (!e.valid()) {
                continue;
            }
            for (const FaceId nb : mesh.edgeFaces(e)) {
                if (nb == f || !mesh.isAlive(nb)) {
                    continue;
                }
                (dot(normals[fi], normals[nb.value]) < 0.0f ? disagree : agree) += 1;
            }
        }
        inverted[fi] = disagree >= 2 && disagree > agree ? 1 : 0;
    }
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (!mesh.isAlive(e) || !mesh.isFeatureEdge(e) || mesh.edgeFaceCount(e) != 2) {
            continue;
        }
        const auto ef = mesh.edgeFaces(e);
        if (inverted[ef[0].value] != inverted[ef[1].value]) {
            mesh.setFeatureEdge(e, false);
        }
    }
}

// CYBER_QC_DEBUG-gated per-call trace of whether the native seamless solve RAN or DECLINED,
// so the corpus harness can grep "native OK" / "native DECLINED <reason>" and confirm a model
// took the native path rather than silently falling back to the vendored / field-aligned path.
void logNative(bool ok, const char* reason) {
    if (std::getenv("CYBER_QC_DEBUG") == nullptr) {
        return;
    }
    if (ok) {
        std::fprintf(stderr, "[qc] native OK\n");
    } else {
        std::fprintf(stderr, "[qc] native DECLINED %s\n", reason);
    }
}

// Attempt-invariant native-solve preparation (see NativeSolveContext): isotropic pre-remesh,
// feature re-tag/filter, the feature-binding decision, and the cross-field/cut setup.
// Everything here depends only on (mesh, targetEdgeLength, adaptivity, featureDegrees) —
// never on the calibration scaling (only solveParameterization consumes the spacing) — so
// the quadrangulator's calibration loop computes it ONCE and re-solves at different
// spacings. Returns false with `reason` set when a stage declines; on success fills `ctx`
// and marks it ready. `isoMs`/`setupMs` report the stage timings for the [qc-time] line.
bool prepareNativeSolve(const Mesh& mesh, float targetEdgeLength, float adaptivity,
                        float featureDegrees, const CancelToken* cancel, NativeSolveContext& ctx,
                        const char*& reason, long& isoMs, long& setupMs) {
    // Isotropic pre-remesh. Tag features so sharp edges survive the resample, then project
    // onto a reference built from the raw island (flat projection: smoothNormalDegrees 0).
    // Feature edges are NO LONGER a decline gate: buildSeamlessSetup marks them as hard seams
    // (the grid breaks along creases, isolines run along them) and solveParameterization pins
    // each feature-bounded patch, so CAD / sharp-feature meshes now run the native solve
    // instead of falling back to the field-aligned path.
    const char* featEnv = std::getenv("CYBER_QC_FEATURE_DEG");
    const float kFeatureDihedralDegrees =
        featEnv != nullptr ? static_cast<float>(std::atof(featEnv)) : featureDegrees;
    constexpr int kFieldIterations = 40;
    // CYBER_QC_PRESERVE_CREASE_DEG (docs/ROADMAP.md Phase 3, lever c1): the threshold used to
    // PROTECT crease geometry through the isotropic pre-remesh, decoupled from the (narrower)
    // threshold that decides hard seams after it.
    //
    // isotropicRemesh already refuses to collapse feature vertices, flip feature edges or smooth
    // them (isotropic.cpp:210/254/318) — but it only sees what `tagFeatureEdges` marked, and this
    // tag takes an INCLUDED angle, so the shipped 40 means "face-normal angle >= 140", i.e. only
    // knife-edge folds. fandisk has ZERO such edges, so the remesher is told the part has no
    // features and freely remeshes across every crease. MEASURED on fandisk at ~3000 quads: the
    // crease network goes from 706 edges in ONE connected component (2 dangling ends) to 449 edges
    // in 55 components with 136 dangling ends — 36% of the creases destroyed outright and the rest
    // shattered. That is upstream of every constraint-layer lever tried so far (M2a-M2d), and it
    // is why pinning the field to creases injected singularities rather than removing them.
    //
    // 135 here means "protect anything with a face-normal angle above 45", matching
    // creaseEdgeFraction's routing test and examples/common.py's feature_error. It deliberately
    // does NOT change the post-remesh tag below: widening THAT turns every crease into a hard seam,
    // measured at deg 135 as feature 0.82 -> 0.77 but median 83.4 -> 77.5 and irregular 3.3 -> 7.1.
    const char* preserveEnv = std::getenv("CYBER_QC_PRESERVE_CREASE_DEG");
    const float kPreserveDihedralDegrees =
        preserveEnv != nullptr ? static_cast<float>(std::atof(preserveEnv)) : 135.0f;
    Mesh& work = ctx.work;
    work = mesh;
    work.triangulate();  // feature tagging + the solve both need a pure-triangle mesh
    // Feature-pinning lever: record the ORIGINAL geometry's resolvable crease network before
    // the isotropic remesh, so the post-remesh feature re-tag can be filtered against it
    // (see resolvableCreaseSegments). Only chains at least a couple of output cells long
    // count as creases the grid should follow.
    // Resolution-aware feature demotion (the wrinkle-web lever, docs/ROADMAP.md gap #4): both
    // reference networks below come from the ORIGINAL geometry, restricted to chains at least
    // ~2 output cells long, so "is this post-remesh dihedral a real feature at THIS density?"
    // has a ground truth. Kill switch CYBER_QC_NO_FEATURE_DEMOTE restores the historical
    // behavior (every tagged knife edge is a seam+pin, every 45-degree crease aligns).
    bool demoteFeatures = std::getenv("CYBER_QC_NO_FEATURE_DEMOTE") == nullptr;
    const bool fieldStats = std::getenv("CYBER_QC_FIELD_STATS") != nullptr;
    std::vector<std::array<Vec3, 2>> refCreases;
    if (featEnv != nullptr || demoteFeatures || fieldStats) {
        work.tagFeatureEdges(kFeatureDihedralDegrees);
        refCreases = resolvableCreaseSegments(work, 2.0f * targetEdgeLength);
    }
    work.tagFeatureEdges(kPreserveDihedralDegrees);
    // The preserve tag doubles as the crease-ALIGNMENT threshold (135 included == the field's
    // 45-degree normal-angle default), so the align-level reference network is read off the
    // original mesh right here, while its tags are live.
    std::vector<std::array<Vec3, 2>> refAlign;
    Mesh originalSnapshot;  // pre-isotropic copy: the persistence probes sample ITS normals
    float coarseness = 0.0f;
    if (demoteFeatures) {
        // COARSE-SUBSTRATE gate: demotion targets sub-cell structure, which only exists when
        // the output cell spans several input edges. On a substrate near the input's own
        // resolution the persistence probes sample scan noise instead of structure — measured
        // on the bunny default arm @3000 (ratio 3.2): any perturbation of its ear crease pins
        // costs ears 19 -> 25..64. The aliased regime the lever exists for sits at ratio ~6.6+
        // (nefertiti/armadillo pure @4000). Threshold 4 output-cell-to-input-edge; density-
        // relative by construction (nefertiti pure @16000 drops to 3.4 and disengages).
        // CYBER_QC_DEMOTE_MIN_RATIO overrides for A/Bs.
        double edgeSum = 0.0;
        std::size_t edgeCount = 0;
        for (Index ei = 0; ei < work.edgeCapacity(); ++ei) {
            const EdgeId e{ei};
            if (!work.isAlive(e)) {
                continue;
            }
            const auto [a, b] = work.edgeVertices(e);
            edgeSum += static_cast<double>(length(work.position(b) - work.position(a)));
            ++edgeCount;
        }
        const float meanInputEdge =
            edgeCount > 0 ? static_cast<float>(edgeSum / static_cast<double>(edgeCount)) : 0.0f;
        float minRatio = 4.0f;
        if (const char* r = std::getenv("CYBER_QC_DEMOTE_MIN_RATIO"); r != nullptr) {
            minRatio = static_cast<float>(std::atof(r));
        }
        coarseness = meanInputEdge > 0.0f ? targetEdgeLength / meanInputEdge : 0.0f;
        demoteFeatures = coarseness >= minRatio;
        if (fieldStats && !demoteFeatures) {
            std::fprintf(stderr, "[web] demotion disengaged: coarseness %.2f < %.2f\n",
                         static_cast<double>(coarseness), static_cast<double>(minRatio));
        }
    }
    if (demoteFeatures) {
        refAlign = resolvableCreaseSegments(work, 2.0f * targetEdgeLength);
        originalSnapshot = work;
    }

    if (cancel != nullptr && cancel->isCancelled()) {
        reason = "cancelled";
        return false;
    }

    const auto tick = []() { return std::chrono::steady_clock::now(); };
    const auto ms = [](auto a, auto b) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
    };
    auto t0 = tick();

    const ReferenceSurface reference(work, 0.0f);
    IsotropicOptions iso;
    iso.targetEdgeLength = targetEdgeLength;
    iso.adaptivity = adaptivity;
    // Painted density sizes the solve substrate: quad-cover skips the pipeline's
    // isotropic stage, so this is where density has to enter for this backend.
    iso.density = ctx.guidance;
    if (isotropicRemesh(work, reference, iso, nullptr, cancel) != IsotropicStatus::Success ||
        work.faceCount() == 0) {
        reason = "isotropic remesh failed (or cancelled)";
        return false;
    }
    const auto tIso = tick();
    // Re-tag features on the REMESHED mesh: the isotropic stage preserves the sharp geometry
    // (feature vertices are never smoothed off) but the newly-created edges do not inherit the
    // feature flag, so the seam logic in buildSeamlessSetup would see none. Re-detect them by
    // dihedral angle now that the crease geometry is intact.
    work.tagFeatureEdges(kFeatureDihedralDegrees);
    // Folded (inverted) remesh triangles masquerade as knife edges — untag them before
    // the reference filter, whose knife-edge exception would keep them (see
    // untagFoldedFeatureEdges).
    untagFoldedFeatureEdges(work);
    // Feature-pinning lever: drop re-tagged interior edges that do not trace the original
    // resolvable crease network — a coarse remesh of an organic surface has plenty of
    // over-threshold dihedrals that are sampling artifacts, not creases, and every spurious
    // hard seam costs singularities (nefertiti lever-on: 216 -> ~lever-off level with this
    // filter). Lever-off is untouched.
    const bool wideBinding = kFeatureDihedralDegrees > 41.0f;
    if (wideBinding) {
        filterFeatureEdgesToReference(work, refCreases, 0.25f * targetEdgeLength, featureDegrees);
    }
    // Web census + resolution-aware demotion (see the helpers above). The census reports the
    // web the filters LEFT (the state the field solve would consume); demotion then unTAGs
    // sub-resolution chains — removing their hard seams, field pins, planar-fill seeds and
    // dipole barriers in one stroke, since every consumer reads Mesh::isFeatureEdge — and
    // gates the 45-degree crease-alignment pins to the original mesh's resolvable network.
    // The isotropic-stage geometry PRESERVATION (the 135-degree preserve tag) already ran and
    // is untouched; the featureBinding decision below runs on the demoted tag set, so a mesh
    // whose entire web is sub-resolution takes the featureless solve path it should have
    // taken all along.
    if (fieldStats) {
        reportFeatureWebCensus(work, refCreases, targetEdgeLength, 0.25f * targetEdgeLength);
    }
    ctx.creaseAlignSupport.clear();
    if (demoteFeatures) {
        // Persistence threshold for the at-scale bend test (see bendPersistsAtScale). 45 ==
        // the crease-ALIGNMENT threshold itself: an edge pins because its dihedral crosses 45
        // degrees, so it keeps the pin only if that bend still measures 45 degrees at cell
        // scale on the original surface. Measured vs 30: nefertiti greedy 244 -> 220,
        // armadillo 150 -> 159 (noise-level), CAD unaffected (engagement-gated + on-network).
        // CYBER_QC_DEMOTE_PERSIST_DEG for A/Bs.
        float persistDeg = 45.0f;
        if (const char* p = std::getenv("CYBER_QC_DEMOTE_PERSIST_DEG"); p != nullptr) {
            persistDeg = static_cast<float>(std::atof(p));
        }
        const ReferenceSurface originalRef(originalSnapshot, 0.0f);
        const float sampleDist = targetEdgeLength;  // probe scales are relative to a cell
        const std::size_t demoted =
            demoteUnresolvableFeatureEdges(work, refCreases, 0.25f * targetEdgeLength,
                                           originalSnapshot, originalRef, sampleDist, persistDeg);
        ctx.creaseAlignSupport =
            creaseAlignSupportFlags(work, refAlign, 0.25f * targetEdgeLength, originalSnapshot,
                                    originalRef, sampleDist, persistDeg);
        if (fieldStats) {
            std::size_t candidates = 0, supported = 0;
            const float candCos = std::cos(degreesToRadians(45.0f));
            for (Index ei = 0; ei < work.edgeCapacity(); ++ei) {
                const EdgeId e{ei};
                if (!work.isAlive(e) || work.edgeFaceCount(e) != 2) {
                    continue;
                }
                const auto ef = work.edgeFaces(e);
                if (dot(normalized(work.faceNormal(ef[0])), normalized(work.faceNormal(ef[1]))) >=
                    candCos) {
                    continue;
                }
                ++candidates;
                supported += ctx.creaseAlignSupport[ei] != 0 ? 1U : 0U;
            }
            std::fprintf(stderr,
                         "[web] demoted=%zu tag edges | align candidates=%zu supported=%zu "
                         "(refAlign=%zu segs, coarseness=%.2f)\n",
                         demoted, candidates, supported, refAlign.size(),
                         static_cast<double>(coarseness));
        }
    }
    // Feature binding engages only when the (filtered) mesh actually carries
    // tagged interior feature edges AND the surface is closed: a featureless
    // mesh (sphere, torus, organic scans below the threshold) takes the
    // historical solve path bit-identically, and OPEN surfaces stay on it too
    // — they cannot run the ARAP polish (same boundary gate) and measured a
    // CV blow-up under feature seams (the open-surface cleanup suite).
    // Binding features on boundaried scans is future work.
    ctx.featureBinding = false;
    if (wideBinding) {
        std::size_t nBoundary = 0, nAlive = 0;
        bool anyInteriorFeature = false;
        for (Index ei = 0; ei < work.edgeCapacity(); ++ei) {
            const EdgeId e{ei};
            if (!work.isAlive(e)) {
                continue;
            }
            ++nAlive;
            const std::size_t nf = work.edgeFaceCount(e);
            if (nf == 1) {
                ++nBoundary;
            } else if (nf == 2 && work.isFeatureEdge(e)) {
                anyInteriorFeature = true;
            }
        }
        const bool closed =
            nAlive > 0 && static_cast<double>(nBoundary) < 0.01 * static_cast<double>(nAlive);
        ctx.featureBinding = anyInteriorFeature && closed;
    }

    if (cancel != nullptr && cancel->isCancelled()) {
        reason = "cancelled";
        return false;
    }

    // Debug dump: the post-isotropic work mesh with its (filtered) feature tags, for
    // offline analysis of density/tagging pathologies (this is how the sliver cascade and
    // the fold tags were found). CYBER_QC_DUMP_WORK=<path.obj>; feature edges as trailing
    // "l" lines.
    if (const char* dumpPath = std::getenv("CYBER_QC_DUMP_WORK"); dumpPath != nullptr) {
        std::vector<Vec3> dp;
        std::vector<std::vector<Index>> df;
        work.toIndexed(dp, df);
        // toIndexed compacts vertices; rebuild the compaction map for edge output.
        std::vector<Index> vmap(work.vertexCapacity(), 0);
        Index next = 0;
        for (Index vi = 0; vi < work.vertexCapacity(); ++vi) {
            if (work.isAlive(VertexId{vi})) {
                vmap[vi] = next++;
            }
        }
        if (std::FILE* f = std::fopen(dumpPath, "wb"); f != nullptr) {
            for (const Vec3& p : dp) {
                std::fprintf(f, "v %.9g %.9g %.9g\n", static_cast<double>(p.x),
                             static_cast<double>(p.y), static_cast<double>(p.z));
            }
            for (const auto& fc : df) {
                std::fprintf(f, "f");
                for (const Index v : fc) {
                    std::fprintf(f, " %u", static_cast<unsigned>(v) + 1U);
                }
                std::fprintf(f, "\n");
            }
            for (Index ei = 0; ei < work.edgeCapacity(); ++ei) {
                const EdgeId e{ei};
                if (work.isAlive(e) && work.isFeatureEdge(e)) {
                    const auto [a, b] = work.edgeVertices(e);
                    std::fprintf(f, "l %u %u\n", static_cast<unsigned>(vmap[a.value]) + 1U,
                                 static_cast<unsigned>(vmap[b.value]) + 1U);
                }
            }
            std::fclose(f);
        }
    }

    auto backend = accel::defaultBackend();
    ctx.setup = buildSeamlessSetup(
        work, kFieldIterations, *backend, ctx.featureBinding,
        ctx.creaseAlignSupport.empty() ? nullptr : &ctx.creaseAlignSupport, ctx.guidance);
    const SeamlessSetup& setup = ctx.setup;
    if (fieldStats) {
        // Cone census: total cones, how many sit off tagged feature edges (spurious flat-region
        // cones are a field-quality smell), and how many sit ON the crease-align web (an
        // incident interior edge past the 45-degree normal-angle alignment threshold) — the
        // cones-vs-web statistic for the wrinkle-web lever.
        std::size_t cones = 0, coneFlat = 0, coneOnAlign = 0;
        const float alignCos = std::cos(degreesToRadians(45.0f));
        for (Index vi = 0; vi < work.vertexCapacity(); ++vi) {
            const VertexId vv{vi};
            if (!work.isAlive(vv) || vi >= setup.singularityIndex.size() ||
                setup.singularityIndex[vi] == 0) {
                continue;
            }
            ++cones;
            bool onFeature = false;
            bool onAlign = false;
            for (const EdgeId e : work.vertexEdges(vv)) {
                if (work.isFeatureEdge(e)) {
                    onFeature = true;
                }
                if (work.isAlive(e) && work.edgeFaceCount(e) == 2) {
                    const auto ef = work.edgeFaces(e);
                    if (dot(normalized(work.faceNormal(ef[0])),
                            normalized(work.faceNormal(ef[1]))) < alignCos) {
                        onAlign = true;
                    }
                }
            }
            if (!onFeature) {
                ++coneFlat;
            }
            if (onAlign) {
                ++coneOnAlign;
            }
        }
        std::fprintf(stderr, "[native] work faces=%zu cones=%zu offFeature=%zu onAlignWeb=%zu\n",
                     work.faceCount(), cones, coneFlat, coneOnAlign);
    }
    if (!setup.valid) {
        reason = "setup invalid";
        return false;
    }
    const auto tSetup = tick();
    if (std::getenv("CYBER_QC_DEBUG") != nullptr) {
        std::size_t featureEdges = 0;
        for (Index ei = 0; ei < work.edgeCapacity(); ++ei) {
            if (work.isAlive(EdgeId{ei}) && work.edgeFaceCount(EdgeId{ei}) == 2 &&
                work.isFeatureEdge(EdgeId{ei})) {
                ++featureEdges;
            }
        }
        std::fprintf(stderr,
                     "[qc] native setup: faces=%zu featureEdges=%zu singular=%zu totalIndex=%d\n",
                     work.faceCount(), featureEdges, setup.singularityCount(), setup.totalIndex());
    }
    isoMs = static_cast<long>(ms(t0, tIso));
    setupMs = static_cast<long>(ms(tIso, tSetup));
    ctx.ready = true;
    return true;
}
}  // namespace

SeamlessUv computeSeamlessUvNative(const Mesh& mesh, float targetEdgeLength, float adaptivity,
                                   float spacingScale, const CancelToken* cancel,
                                   float featureDegrees, NativeSolveContext* ctx) {
    // M3 (docs/native-miq-plan.md): the fully native QuadCover-style seamless solve. No
    // Geogram, no subprocess. Pipeline:
    //   (pre) isotropic pre-remesh at `targetEdgeLength` so the solve runs on a clean,
    //         uniformly-sized triangulation (the role the harness's internal remesh played);
    //   (M1)  frame field + period jumps + singularities + cut graph (buildSeamlessSetup);
    //   (M2)  seamless integer-grid Poisson solve at grid cell == targetEdgeLength
    //         (solveParameterization) -> per-corner UV;
    //   assemble a SeamlessUv on the remeshed verts/tris from the per-corner UV.
    // Any degenerate stage returns an invalid UV so the caller falls through cleanly.
    // The (pre) + (M1) stages are attempt-invariant (prepareNativeSolve): a caller-provided
    // `ctx` carries them across the calibration loop's re-solves; a null ctx recomputes
    // them here, byte-identically, on every call.
    SeamlessUv uv;
    if (targetEdgeLength <= 0.0f || mesh.faceCapacity() == 0) {
        logNative(false, "empty/degenerate input");
        return uv;
    }

    const bool timing = std::getenv("CYBER_QC_TIME") != nullptr;
    const auto tick = []() { return std::chrono::steady_clock::now(); };
    const auto ms = [](auto a, auto b) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
    };

    NativeSolveContext local;
    NativeSolveContext& prep = ctx != nullptr ? *ctx : local;
    long isoMs = 0;
    long setupMs = 0;
    if (!prep.ready) {
        const char* reason = "";
        if (!prepareNativeSolve(mesh, targetEdgeLength, adaptivity, featureDegrees, cancel, prep,
                                reason, isoMs, setupMs)) {
            logNative(false, reason);
            return uv;
        }
    }
    const Mesh& work = prep.work;
    const SeamlessSetup& setup = prep.setup;
    const bool featureBinding = prep.featureBinding;

    if (cancel != nullptr && cancel->isCancelled()) {
        logNative(false, "cancelled");
        return uv;
    }

    auto backend = accel::defaultBackend();
    // Grid spacing == targetEdgeLength gives ~1 UV cell per triangle. The isoline extractor
    // then loses a large fraction of cells to short-edge collapse, so the quad count lands well
    // under target. `spacingScale` < 1 traces denser isolines (several per triangle) so the
    // extracted count tracks the request; the quadrangulator's calibration loop drives it, and
    // CYBER_QC_SPACING_MUL overrides for experiments.
    const char* spEnv = std::getenv("CYBER_QC_SPACING_MUL");
    const float spacingMul = spEnv != nullptr ? static_cast<float>(std::atof(spEnv)) : spacingScale;
    const auto tSolve0 = tick();
    const Parameterization param =
        solveParameterization(work, setup, targetEdgeLength * spacingMul, *backend, cancel,
                              &prep.solveCache, prep.guidance);
    if (!param.valid) {
        logNative(false, "parameterization invalid (or cancelled)");
        return uv;
    }
    if (timing) {
        std::fprintf(stderr,
                     "[qc-time] faces=%zu | isotropic=%ldms field+setup=%ldms solve=%ldms\n",
                     work.faceCount(), isoMs, setupMs, static_cast<long>(ms(tSolve0, tick())));
    }

    // Assemble the seamless UV on the remeshed triangles from the per-corner parameterization.
    uv.vertices.assign(work.vertexCapacity(), Vec3{0, 0, 0});
    for (Index vi = 0; vi < work.vertexCapacity(); ++vi) {
        if (work.isAlive(VertexId{vi})) {
            uv.vertices[vi] = work.position(VertexId{vi});
        }
    }
    float uMin = std::numeric_limits<float>::max();
    float uMax = std::numeric_limits<float>::lowest();
    float vMin = uMin, vMax = uMax;
    for (Index fi = 0; fi < work.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!work.isAlive(f) || work.faceSize(f) != 3) {
            continue;
        }
        const std::vector<VertexId> vs = work.faceVertices(f);
        uv.triangles.push_back({vs[0].value, vs[1].value, vs[2].value});
        // Snap near-integer coordinates exactly onto the lattice. The solve
        // pins feature-seam level sets to integers, but CG + reconstruction
        // return them as integer±1e-6 — and the extractor's edge-on-isoline
        // test (Double::isZero) is machine-epsilon exact, so without the snap
        // a pinned crease is never recognized as lying ON an isoline. Part of
        // the in-progress feature-pinning lever: OPT-IN via CYBER_QC_UV_SNAP
        // until the seam-shear fix ships (the snap alone shifts extraction on
        // meshes whose UVs coincidentally graze the lattice). Only under
        // feature binding — without pinned creases the snap merely perturbs
        // (a measured torus regression). Kill switch: CYBER_QC_NO_UV_SNAP.
        const bool kSnapUv = featureBinding && std::getenv("CYBER_QC_NO_UV_SNAP") == nullptr;
        std::array<Vec2, 3> corners = param.cornerUv[fi];
        if (kSnapUv) {
            for (Vec2& c : corners) {
                const float ru = std::round(c.x);
                const float rv = std::round(c.y);
                if (std::fabs(c.x - ru) < 1e-3f) {
                    c.x = ru;
                }
                if (std::fabs(c.y - rv) < 1e-3f) {
                    c.y = rv;
                }
            }
        }
        uv.triangleUv.push_back(corners);
        for (const Vec2& c : param.cornerUv[fi]) {
            uMin = std::min(uMin, c.x);
            uMax = std::max(uMax, c.x);
            vMin = std::min(vMin, c.y);
            vMax = std::max(vMax, c.y);
        }
    }
    if (uv.triangles.empty()) {
        logNative(false, "no triangles assembled");
        return uv;
    }

    // Divergence guard. The seam being locally integer-seamless (residual ~0) does NOT
    // guarantee a globally usable map: on organic, many-cone meshes the integer phase can
    // blow the map up while staying per-edge consistent (spot: whole UV spans ~1e9 cells).
    // Such a UV makes the isoline tracer enumerate an astronomical grid and effectively
    // hang. A sane integer-grid UV covers ~one cell per triangle, so its bounding box in
    // cells is O(triangleCount); reject anything wildly beyond that so the caller degrades
    // cleanly (falls through to the vendored / subprocess path, or the field-aligned
    // fallback) instead of feeding the extractor a divergent map.
    const double spanCellsU = static_cast<double>(uMax - uMin);
    const double spanCellsV = static_cast<double>(vMax - vMin);
    const double cellArea = spanCellsU * spanCellsV;
    const double sane = 100.0 * static_cast<double>(uv.triangles.size());
    if (std::getenv("CYBER_QC_DEBUG") != nullptr) {
        std::fprintf(stderr,
                     "[qc] native UV span: u=[%.2f,%.2f] v=[%.2f,%.2f] cellArea=%.1f sane=%.1f\n",
                     static_cast<double>(uMin), static_cast<double>(uMax),
                     static_cast<double>(vMin), static_cast<double>(vMax), cellArea, sane);
    }
    if (!(cellArea >= 0.0) || cellArea > sane) {
        logNative(false, "divergence guard (UV bbox >> triangle count)");
        return SeamlessUv{};  // diverged -> invalid, caller degrades cleanly
    }

    logNative(true, "");
    uv.valid = true;
    return uv;
}

double nativeRelaxedCellArea(const Mesh& mesh, float targetEdgeLength, float adaptivity,
                             float featureDegrees, const CancelToken* cancel,
                             NativeSolveContext& ctx) {
    if (targetEdgeLength <= 0.0f || mesh.faceCapacity() == 0) {
        return -1.0;
    }
    const auto tick = []() { return std::chrono::steady_clock::now(); };
    const auto ms = [](auto a, auto b) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
    };
    long isoMs = 0;
    long setupMs = 0;
    if (!ctx.ready) {
        const char* reason = "";
        if (!prepareNativeSolve(mesh, targetEdgeLength, adaptivity, featureDegrees, cancel, ctx,
                                reason, isoMs, setupMs)) {
            logNative(false, reason);
            return -1.0;
        }
    }
    auto backend = accel::defaultBackend();
    const auto tRelax0 = tick();
    const double cells = relaxedCellArea(ctx.work, ctx.setup, targetEdgeLength, *backend, cancel,
                                         &ctx.solveCache, ctx.guidance);
    if (std::getenv("CYBER_QC_TIME") != nullptr) {
        std::fprintf(stderr,
                     "[qc-time] probe: isotropic=%ldms field+setup=%ldms relaxed=%ldms "
                     "cells=%.1f\n",
                     isoMs, setupMs, static_cast<long>(ms(tRelax0, tick())), cells);
    }
    return cells;
}

namespace {

// Vendored seamless UV: Geogram quad_cover run in-process (built with CYBER_WITH_QUADCOVER)
// or via the CYBER_QUADCOVER_CLI harness subprocess. QuadriFlow parity where available.
// Returns an invalid UV when neither is present so computeSeamlessUv falls through to the
// native solver.
SeamlessUv computeSeamlessUvVendored(const Mesh& mesh, float targetEdgeLength, float harnessScaling,
                                     float harnessAdaptivity,
                                     [[maybe_unused]] const CancelToken* cancel) {
    SeamlessUv uv;
    if (targetEdgeLength <= 0.0f) {
        return uv;  // no target density -> caller degrades cleanly
    }

#ifdef CYBER_HAVE_QUADCOVER
    // In-process solve (M4c): no subprocess, no temp files, no CYBER_QUADCOVER_CLI.
    // Convert the mesh to indexed verts/tris, derive the target quad count from the
    // requested edge length exactly as the subprocess path did, and run AutoRemesher's
    // Geogram quad_cover directly via the isolated cyber_quadcover_solver library.
    std::vector<Vec3> pos;
    std::vector<std::vector<Index>> faces;
    mesh.toIndexed(pos, faces);
    if (pos.empty() || faces.empty()) {
        return uv;
    }
    std::vector<std::array<double, 3>> verts;
    verts.reserve(pos.size());
    for (const Vec3& p : pos) {
        verts.push_back(
            {static_cast<double>(p.x), static_cast<double>(p.y), static_cast<double>(p.z)});
    }
    std::vector<std::array<std::size_t, 3>> tris;
    double area = 0.0;
    for (const auto& fc : faces) {
        for (std::size_t k = 1; k + 1 < fc.size(); ++k) {  // fan-triangulate
            tris.push_back({static_cast<std::size_t>(fc[0]), static_cast<std::size_t>(fc[k]),
                            static_cast<std::size_t>(fc[k + 1])});
            area += 0.5 * static_cast<double>(
                              length(cross(pos[fc[k]] - pos[fc[0]], pos[fc[k + 1]] - pos[fc[0]])));
        }
    }
    if (tris.empty()) {
        return uv;
    }
    const double cell =
        static_cast<double>(targetEdgeLength) * static_cast<double>(targetEdgeLength);
    const long quads = cell > 0.0 ? std::max(64L, std::lround(area / cell)) : 3000L;
    // Scaling defaults to the passed harnessScaling (the QuadCoverQuadrangulator
    // calibration loop sweeps it); CYBER_QC_SCALING overrides for calibration sweeps.
    // Adaptivity defaults to a uniform field (0.0) matching the subprocess `-a 0.0`;
    // CYBER_QC_ADAPT overrides. Both env overrides honored in BOTH paths.
    const char* scalingEnv = std::getenv("CYBER_QC_SCALING");
    const double scaling =
        scalingEnv != nullptr ? std::atof(scalingEnv) : static_cast<double>(harnessScaling);
    const char* adaptEnv = std::getenv("CYBER_QC_ADAPT");
    const double adapt =
        adaptEnv != nullptr ? std::atof(adaptEnv) : static_cast<double>(harnessAdaptivity);
    const bool dbg = std::getenv("CYBER_QC_DEBUG") != nullptr;
    if (dbg) {
        std::fprintf(stderr, "[qc] in-process edgeLen=%g area=%g quads=%ld s=%g a=%g\n",
                     static_cast<double>(targetEdgeLength), area, quads, scaling, adapt);
    }
    const qcsolver::SeamlessSolveResult res =
        qcsolver::solveSeamlessUv(verts, tris, quads, scaling, adapt);
    if (dbg) {
        std::fprintf(stderr, "[qc] in-process ok=%d isoTris=%zu\n", static_cast<int>(res.ok),
                     res.triangles.size());
    }
    if (!res.ok) {
        return uv;
    }
    uv.vertices.reserve(res.vertices.size());
    for (const auto& v : res.vertices) {
        uv.vertices.push_back(
            Vec3{static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])});
    }
    uv.triangles.reserve(res.triangles.size());
    uv.triangleUv.reserve(res.triangleUvs.size());
    for (std::size_t t = 0; t < res.triangles.size(); ++t) {
        const auto& tri = res.triangles[t];
        const auto& q = res.triangleUvs[t];
        uv.triangles.push_back(
            {static_cast<Index>(tri[0]), static_cast<Index>(tri[1]), static_cast<Index>(tri[2])});
        uv.triangleUv.push_back({Vec2{static_cast<float>(q[0]), static_cast<float>(q[1])},
                                 Vec2{static_cast<float>(q[2]), static_cast<float>(q[3])},
                                 Vec2{static_cast<float>(q[4]), static_cast<float>(q[5])}});
    }
    uv.valid = !uv.triangles.empty();
    return uv;
#else
    const char* cli = std::getenv("CYBER_QUADCOVER_CLI");
    if (cli == nullptr) {
        return uv;  // no solver available -> caller degrades cleanly
    }
    static std::atomic<unsigned> counter{0};
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::string tag = "cyber_qc_" + std::to_string(counter.fetch_add(1));
    const std::string objPath = (dir / (tag + ".obj")).string();
    const std::string uvPath = (dir / (tag + ".uv")).string();

    double area = 0.0;
    if (!writeObjFor(objPath, mesh, area)) {
        return uv;
    }
    const double cell =
        static_cast<double>(targetEdgeLength) * static_cast<double>(targetEdgeLength);
    const long quads = cell > 0.0 ? std::max(64L, std::lround(area / cell)) : 3000L;
    // The harness scaling controls the isotropic-remesh density that in turn sets the
    // seamless-UV grid resolution (and thus our extracted quad count). Overridable via
    // CYBER_QC_SCALING for calibration sweeps; the default is tuned so the extracted quad
    // count tracks the requested target (see M4 calibration in docs/quadcover-plan.md).
    const char* scalingEnv = std::getenv("CYBER_QC_SCALING");
    const std::string scaling =
        scalingEnv != nullptr ? std::string(scalingEnv) : std::to_string(harnessScaling);
    // Frame-field gradient adaptivity (harness -a). AutoRemesher defaults to 1.0, whose
    // curvature-driven scaling gradients inject singularities; a UNIFORM field (0.0)
    // minimizes them and is the right choice here since quad-cover is a uniform-density
    // method (it bypasses the pipeline's adaptive isotropic stage). Measured at matched
    // density this drops irregular fraction — spot 3.6->2.6%, fandisk 5.2->1.6% — and
    // lifts quad-dominance to ~99.6% (M4b). CYBER_QC_ADAPT overrides for sweeps.
    const char* adaptEnv = std::getenv("CYBER_QC_ADAPT");
    const std::string adaptArg =
        std::string(" -a ") +
        (adaptEnv != nullptr ? std::string(adaptEnv) : std::to_string(harnessAdaptivity));
    const std::string cmd = std::string(cli) + " -i " + objPath + " -u " + uvPath + " -f " +
                            std::to_string(quads) + " -s " + scaling + adaptArg +
                            " >/dev/null 2>&1";
    const bool dbg = std::getenv("CYBER_QC_DEBUG") != nullptr;
    if (dbg) {
        std::fprintf(stderr, "[qc] edgeLen=%g area=%g quads=%ld cmd=%s\n",
                     static_cast<double>(targetEdgeLength), area, quads, cmd.c_str());
    }
#if defined(__APPLE__) && TARGET_OS_IPHONE
    const int rc = -1;  // out-of-process QuadCover CLI is unavailable on iOS
#else
    const int rc = std::system(cmd.c_str());
#endif
    if (rc == 0) {
        uv.valid = parseUvDump(uvPath, uv);
    }
    if (dbg) {
        std::fprintf(stderr, "[qc] rc=%d valid=%d isoTris=%zu\n", rc, static_cast<int>(uv.valid),
                     uv.triangles.size());
    }
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
    std::filesystem::remove(uvPath, ec);
    return uv;
#endif  // CYBER_HAVE_QUADCOVER
}

}  // namespace

// Fraction of interior (exactly-two-face) edges whose dihedral angle exceeds
// `dihedralDegrees` — a CAD discriminator: high on crease-heavy parts (fandisk
// ~4%), near zero on smooth organic meshes (spot/rocker/bunny < 0.2%). Const and
// non-mutating (unlike Mesh::tagFeatureEdges, which writes edge flags).
float creaseEdgeFraction(const Mesh& mesh, float dihedralDegrees) {
    const float cosThresh = std::cos(dihedralDegrees * 3.14159265358979324f / 180.0f);
    std::size_t interior = 0;
    std::size_t creases = 0;
    for (Index e = 0; e < mesh.edgeCapacity(); ++e) {
        const EdgeId edge{e};
        if (!mesh.isAlive(edge)) {
            continue;
        }
        const std::vector<FaceId> faces = mesh.edgeFaces(edge);
        if (faces.size() != 2) {
            continue;
        }
        ++interior;
        const Vec3 n0 = normalized(mesh.faceNormal(faces[0]));
        const Vec3 n1 = normalized(mesh.faceNormal(faces[1]));
        if (dot(n0, n1) < cosThresh) {
            ++creases;
        }
    }
    return interior > 0 ? static_cast<float>(creases) / static_cast<float>(interior) : 0.0f;
}

SeamlessUv computeSeamlessUv(const Mesh& mesh, float targetEdgeLength, float harnessScaling,
                             float harnessAdaptivity, const CancelToken* cancel,
                             float featureDegrees, NativeSolveContext* ctx) {
    if (targetEdgeLength <= 0.0f) {
        return SeamlessUv{};  // no target density -> caller degrades cleanly
    }

    const bool haveNative = std::getenv("CYBER_QC_NO_NATIVE") == nullptr;
    // CAD / crease-heavy meshes: the native feature-aware seamless solver marks
    // sharp edges as hard seams and pins the feature-bounded patches, so it makes
    // markedly squarer quads on such parts than the vendored Geogram extractor
    // (measured fandisk median 83.6 vs 80.7 at matched quad count, 0 defects
    // either way). Route them to native FIRST; smooth organic meshes keep the
    // vendored-first order (Geogram wins there). CYBER_QC_ROUTE_CREASE tunes the
    // interior-crease-fraction threshold (0 disables); CYBER_QC_NO_ROUTE forces
    // the original vendored-first order for A/B.
    const char* routeEnv = std::getenv("CYBER_QC_ROUTE_CREASE");
    const float routeThresh = routeEnv != nullptr ? static_cast<float>(std::atof(routeEnv)) : 0.02f;
    // User-drawn guidance FORCES the native route: the vendored Geogram
    // quad_cover has no hook for either flow guides or painted density (it
    // builds its own frame field from a single scalar density inside sources we
    // do not patch). Routing a guided island there would silently drop the
    // input, which is precisely the failure mode this feature exists to avoid.
    const bool guided = ctx != nullptr && ctx->guidance != nullptr;
    const bool routeNative =
        haveNative &&
        (guided || (std::getenv("CYBER_QC_NO_ROUTE") == nullptr && routeThresh > 0.0f &&
                    creaseEdgeFraction(mesh, 45.0f) >= routeThresh));

    if (routeNative) {
        SeamlessUv native = computeSeamlessUvNative(mesh, targetEdgeLength, harnessAdaptivity,
                                                    harnessScaling, cancel, featureDegrees, ctx);
        if (native.valid) {
            return native;
        }
        // Native declined -> fall back to the vendored path (valid or not). A
        // guided island loses its guidance here; say so instead of pretending.
        if (guided) {
            ctx->guidanceHonored = false;
        }
        return computeSeamlessUvVendored(mesh, targetEdgeLength, harnessScaling, harnessAdaptivity,
                                         cancel);
    }

    // Vendored (Geogram quad_cover) FIRST — QuadriFlow parity (1-4% irregular),
    // fast, and best on organic meshes.
    SeamlessUv uv = computeSeamlessUvVendored(mesh, targetEdgeLength, harnessScaling,
                                              harnessAdaptivity, cancel);
    if (uv.valid) {
        return uv;
    }
    // Native FALLBACK — the dependency-free standalone solver, and the stock
    // (no-Geogram) default: watertight, bounded, cancellable at ~4-5% irregular.
    if (haveNative) {
        SeamlessUv native = computeSeamlessUvNative(mesh, targetEdgeLength, harnessAdaptivity,
                                                    harnessScaling, cancel, featureDegrees, ctx);
        if (native.valid) {
            return native;
        }
    }
    return uv;  // invalid -> caller (quad-cover quadrangulator) degrades to field-aligned
}

// Max integer-jump residual across interior edges: for each shared edge the grid
// symmetry mapping one triangle's shared-vertex UVs to the other's must have an
// integer translation. 0 == seamless. (Mirrors the M1 python validator.)
double seamlessUvResidual(const SeamlessUv& uv) {
    if (!uv.valid || uv.triangles.empty()) {
        return 0.0;
    }
    std::map<std::pair<Index, Index>, std::vector<int>> edges;
    for (int t = 0; t < static_cast<int>(uv.triangles.size()); ++t) {
        const auto& tri = uv.triangles[static_cast<std::size_t>(t)];
        for (int i = 0; i < 3; ++i) {
            const Index a = tri[static_cast<std::size_t>(i)];
            const Index b = tri[static_cast<std::size_t>((i + 1) % 3)];
            edges[{std::min(a, b), std::max(a, b)}].push_back(t);
        }
    }
    const auto uvOf = [&](int t, Index vid) {
        const auto& tri = uv.triangles[static_cast<std::size_t>(t)];
        for (int c = 0; c < 3; ++c) {
            if (tri[static_cast<std::size_t>(c)] == vid) {
                return uv.triangleUv[static_cast<std::size_t>(t)][static_cast<std::size_t>(c)];
            }
        }
        return Vec2{0.0f, 0.0f};
    };
    const auto rot90 = [](Vec2 v, int k) {
        for (int i = 0; i < ((k % 4) + 4) % 4; ++i) {
            v = Vec2{-v.y, v.x};
        }
        return v;
    };
    double maxRes = 0.0;
    for (const auto& [e, inc] : edges) {
        if (inc.size() != 2) {
            continue;
        }
        const Vec2 a1 = uvOf(inc[0], e.first), a2 = uvOf(inc[0], e.second);
        const Vec2 b1 = uvOf(inc[1], e.first), b2 = uvOf(inc[1], e.second);
        const Vec2 da{a2.x - a1.x, a2.y - a1.y}, db{b2.x - b1.x, b2.y - b1.y};
        if (da.x == 0.0f && da.y == 0.0f) {
            continue;
        }
        int bestK = 0;
        float bestD = 1e18f;
        for (int k = 0; k < 4; ++k) {
            const Vec2 r = rot90(da, k);
            const float d = (r.x - db.x) * (r.x - db.x) + (r.y - db.y) * (r.y - db.y);
            if (d < bestD) {
                bestD = d;
                bestK = k;
            }
        }
        const Vec2 ra1 = rot90(a1, bestK);
        const float tx = b1.x - ra1.x, ty = b1.y - ra1.y;
        maxRes = std::max(maxRes, static_cast<double>(std::max(std::abs(tx - std::round(tx)),
                                                               std::abs(ty - std::round(ty)))));
    }
    return maxRes;
}

// -----------------------------------------------------------------------------
// Milestone 2: isoline quad extractor.
//
// Faithful port of AutoRemesher's MIT quadextractor.cpp (Copyright (c) 2020 Jeremy
// HU, MIT license — see examples/reference/autoremesher-src). The reference's
// double-precision Vector2/Vector3, PositionKey and MeshSeparator helpers are
// reproduced here as small local types so the port stays self-contained and adds
// no library dependency. Internal geometry stays in double (the integer-isoline
// tests and ratio interpolation need it); results are narrowed to float only when
// filling IsolineQuadMesh.vertices.
// -----------------------------------------------------------------------------
namespace {

// --- double-precision vectors (mirror reference Vector2/Vector3) -------------
struct DVec2 {
    double x = 0.0;
    double y = 0.0;
    double operator[](std::size_t i) const { return i == 0 ? x : y; }
};
struct DVec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double operator[](std::size_t i) const { return i == 0 ? x : (i == 1 ? y : z); }
    DVec3& operator+=(const DVec3& o) {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    DVec3& operator/=(double s) {
        x /= s;
        y /= s;
        z /= s;
        return *this;
    }
};

inline DVec2 operator*(const DVec2& v, double s) { return {v.x * s, v.y * s}; }
inline DVec2 operator+(const DVec2& a, const DVec2& b) { return {a.x + b.x, a.y + b.y}; }
inline DVec2 operator-(const DVec2& a, const DVec2& b) { return {a.x - b.x, a.y - b.y}; }
inline DVec3 operator*(const DVec3& v, double s) { return {v.x * s, v.y * s, v.z * s}; }
inline DVec3 operator+(const DVec3& a, const DVec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline DVec3 operator-(const DVec3& a, const DVec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

inline double dLength(const DVec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
inline DVec3 dNormalized(const DVec3& v) {
    const double len = dLength(v);
    if (len <= std::numeric_limits<double>::epsilon()) {
        return DVec3{};
    }
    return {v.x / len, v.y / len, v.z / len};
}
inline double dDot(const DVec3& a, const DVec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline DVec3 dCross(const DVec3& a, const DVec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
// Unit triangle normal (reference Vector3::normal), zero vector when degenerate.
inline DVec3 dNormal(const DVec3& a, const DVec3& b, const DVec3& c) {
    return dNormalized(dCross(b - a, c - a));
}

inline bool dIsZero(double n) { return std::abs(n) <= std::numeric_limits<double>::epsilon(); }

// Reference Vector2::isInTriangle via barycentric coordinates.
inline bool dInTriangle2(const DVec2& a, const DVec2& b, const DVec2& c, const DVec2& p) {
    const DVec2 v0 = c - a;
    const DVec2 v1 = b - a;
    const DVec2 v2 = p - a;
    const double dot00 = v0.x * v0.x + v0.y * v0.y;
    const double dot01 = v0.x * v1.x + v0.y * v1.y;
    const double dot02 = v0.x * v2.x + v0.y * v2.y;
    const double dot11 = v1.x * v1.x + v1.y * v1.y;
    const double dot12 = v1.x * v2.x + v1.y * v2.y;
    const double denom = dot00 * dot11 - dot01 * dot01;
    if (dIsZero(denom)) {
        return false;
    }
    const double invDenom = 1.0 / denom;
    const double alpha = (dot11 * dot02 - dot01 * dot12) * invDenom;
    const double beta = (dot00 * dot12 - dot01 * dot02) * invDenom;
    if (alpha < 0.0 || beta < 0.0 || (1.0 - (alpha + beta)) < 0.0) {
        return false;
    }
    return true;
}

// Project 3-D points to a local 2-D frame (reference Vector3::project overload).
void dProject(const std::vector<DVec3>& in3d, std::vector<DVec2>& out2d, const DVec3& normal,
              const DVec3& axis, const DVec3& origin) {
    const DVec3 perp = dCross(normal, axis);
    out2d.clear();
    out2d.reserve(in3d.size());
    for (const DVec3& p : in3d) {
        const DVec3 dir = p - origin;
        out2d.push_back({dDot(dir, axis), dDot(dir, perp)});
    }
}

// --- 1e-5-quantised position key (reference PositionKey) ---------------------
struct PositionKey {
    long ix = 0;
    long iy = 0;
    long iz = 0;
    explicit PositionKey(const DVec3& v)
        : ix(static_cast<long>(v.x * kFactor)),
          iy(static_cast<long>(v.y * kFactor)),
          iz(static_cast<long>(v.z * kFactor)) {}
    bool operator<(const PositionKey& r) const {
        if (ix != r.ix) {
            return ix < r.ix;
        }
        if (iy != r.iy) {
            return iy < r.iy;
        }
        return iz < r.iz;
    }
    static constexpr double kFactor = 100000.0;
};

// --- mesh-island helpers (reference MeshSeparator) ---------------------------
void buildEdgeToFaceMap(const std::vector<std::vector<std::size_t>>& faces,
                        std::map<std::pair<std::size_t, std::size_t>, std::size_t>& edgeToFaceMap) {
    edgeToFaceMap.clear();
    for (std::size_t index = 0; index < faces.size(); ++index) {
        const auto& face = faces[index];
        for (std::size_t i = 0; i < face.size(); ++i) {
            const std::size_t j = (i + 1) % face.size();
            edgeToFaceMap[{face[i], face[j]}] = index;
        }
    }
}

void splitToIslands(const std::vector<std::vector<std::size_t>>& faces,
                    std::vector<std::vector<std::vector<std::size_t>>>& islands) {
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> edgeToFaceMap;
    buildEdgeToFaceMap(faces, edgeToFaceMap);
    std::unordered_set<std::size_t> processed;
    std::queue<std::size_t> wait;
    for (std::size_t seed = 0; seed < faces.size(); ++seed) {
        if (processed.find(seed) != processed.end()) {
            continue;
        }
        wait.push(seed);
        std::vector<std::vector<std::size_t>> island;
        while (!wait.empty()) {
            const std::size_t index = wait.front();
            wait.pop();
            if (processed.find(index) != processed.end()) {
                continue;
            }
            const auto& face = faces[index];
            for (std::size_t i = 0; i < face.size(); ++i) {
                const std::size_t j = (i + 1) % face.size();
                const auto opp = edgeToFaceMap.find({face[j], face[i]});
                if (opp == edgeToFaceMap.end()) {
                    continue;
                }
                wait.push(opp->second);
            }
            island.push_back(faces[index]);
            processed.insert(index);
        }
        if (!island.empty()) {
            islands.push_back(island);
        }
    }
}

// --- the extractor -----------------------------------------------------------
// Mirrors AutoRemesher::QuadExtractor; member names and flow kept close to the
// reference to keep the port auditable. Cognitive complexity of extractMesh /
// extractConnections is high by nature (deeply nested orbit walk / isoline
// tracer) — this is an irreducible ported algorithm.
using Graph = std::unordered_map<std::size_t, std::unordered_set<std::size_t>>;
using EdgePair = std::pair<std::size_t, std::size_t>;

class IsolineExtractor {
public:
    IsolineExtractor(std::vector<DVec3> vertices, std::vector<std::array<std::size_t, 3>> triangles,
                     std::vector<std::array<DVec2, 3>> triangleUvs)
        : m_vertices(std::move(vertices)),
          m_triangles(std::move(triangles)),
          m_triangleUvs(std::move(triangleUvs)) {}

    void extract();

    // Enable the graph cleanup + hole fill. Quality depends on it for ANY input —
    // without it the redundant mid-isoline samples survive and every cell traces as
    // an n-gon (open paraboloid: 282 hexagons of 310 faces, median 50°) — but on an
    // open surface it is only safe together with setPreserveInputBoundary, and even
    // then incompletely; see the gating comment in extractIsolineQuads.
    void setRunClosedSurfaceCleanup(bool on) { m_runClosedSurfaceCleanup = on; }

    // Preserve the input surface's genuine boundary (M3). The cleanup's hole fill
    // cannot itself tell the outer boundary of an open surface from a hole the
    // extraction opened, so it fills the perimeter and closes the mesh. With this
    // on, fixHoles leaves loops that trace the INPUT boundary alone, which lets an
    // open surface get the cleanup's quality without losing its rim.
    void setPreserveInputBoundary(bool on) { m_preserveInputBoundary = on; }

    // Run's hole-fill policy (remeshing-parameters spec, "holeFillMaxBoundary"):
    // fill boundary loops of at most this many edges, leave longer ones open, and
    // fill nothing below 3. Replaces a hard-coded 65 — the same magic constant
    // AutoRemesher used, which the spec calls out by name.
    void setHoleFillMaxBoundary(int maxEdges) { m_holeFillMaxBoundary = maxEdges; }

    const std::vector<DVec3>& remeshedVertices() const { return m_remeshedVertices; }
    const std::vector<std::vector<std::size_t>>& remeshedQuads() const {
        return m_remeshedPolygons;
    }

private:
    std::vector<DVec3> m_vertices;
    std::vector<std::array<std::size_t, 3>> m_triangles;
    std::vector<std::array<DVec2, 3>> m_triangleUvs;
    std::vector<DVec3> m_remeshedVertices;
    std::vector<std::vector<std::size_t>> m_remeshedPolygons;
    std::set<EdgePair> m_halfEdges;
    // Graph cleanup + hole fill (faithfully ported below); the caller enables it.
    bool m_runClosedSurfaceCleanup = false;
    // M3 open-surface support: when set, fixHoles skips loops tracing the input's
    // genuine boundary. m_inputBoundary holds those segments and m_boundaryTol the
    // on-boundary distance threshold; both are built lazily by buildInputBoundary().
    bool m_preserveInputBoundary = false;
    std::vector<std::pair<DVec3, DVec3>> m_inputBoundary;
    double m_boundaryTol = 0.0;
    // Hole-fill policy; the RemeshParams default, overridden per run by the caller.
    int m_holeFillMaxBoundary = 64;

    void buildInputBoundary();
    bool tracesInputBoundary(const std::vector<std::size_t>& loop) const;

    void extractConnections(std::vector<DVec3>& crossPoints,
                            std::vector<std::size_t>& sourceTriangles,
                            std::set<EdgePair>& connections);
    void extractEdges(const std::set<EdgePair>& connections, Graph& edgeConnectMap);
    void simplifyGraph(Graph& graph);
    bool collapseShortEdges(std::vector<DVec3>& crossPoints, Graph& edgeConnectMap);
    bool collapseTriangles(std::vector<DVec3>& crossPoints, Graph& edgeConnectMap);
    bool removeSingleEndpoints(Graph& edgeConnectMap);
    void collapseEdge(std::vector<DVec3>& crossPoints, Graph& edgeConnectMap, const EdgePair& edge);
    void extractMesh(std::vector<DVec3>& points,
                     const std::vector<std::size_t>& pointSourceTriangles, Graph& edgeConnectMap);
    bool testPointInTriangle(const std::vector<DVec3>& points,
                             const std::array<std::size_t, 3>& triangle,
                             const std::vector<std::size_t>& testPoints);
    void searchBoundaries(const std::set<EdgePair>& halfEdges,
                          std::vector<std::vector<std::size_t>>& loops);
    void fixHoleWithQuads(std::vector<std::size_t>& hole, bool checkScore);
    void fixHoles();
    void rebuildHalfEdges();
    bool removeIsolatedFaces();
    bool removeNonManifoldFaces();
};

void IsolineExtractor::extractEdges(const std::set<EdgePair>& connections, Graph& edgeConnectMap) {
    for (const auto& c : connections) {
        edgeConnectMap[c.first].insert(c.second);
        edgeConnectMap[c.second].insert(c.first);
    }
    simplifyGraph(edgeConnectMap);
}

void IsolineExtractor::simplifyGraph(Graph& graph) {
    for (;;) {
        std::unordered_map<std::size_t, std::pair<std::size_t, std::size_t>> delayPairs;
        for (auto it = graph.begin(); it != graph.end();) {
            if (it->second.size() != 2) {
                ++it;
                continue;
            }
            auto neighborIt = it->second.begin();
            const std::size_t firstNeighbor = *neighborIt++;
            const std::size_t secondNeighbor = *neighborIt++;
            if (delayPairs.end() != delayPairs.find(firstNeighbor) ||
                delayPairs.end() != delayPairs.find(secondNeighbor)) {
                ++it;
                continue;
            }
            delayPairs.insert({it->first, {firstNeighbor, secondNeighbor}});
            it = graph.erase(it);
        }
        if (delayPairs.empty()) {
            break;
        }
        for (const auto& dp : delayPairs) {
            graph[dp.second.first].erase(dp.first);
            graph[dp.second.first].insert(dp.second.second);
            graph[dp.second.second].erase(dp.first);
            graph[dp.second.second].insert(dp.second.first);
        }
    }
}

bool IsolineExtractor::removeSingleEndpoints(Graph& edgeConnectMap) {
    bool removed = false;
    Graph& graph = edgeConnectMap;
    std::vector<std::size_t> endpoints;
    for (const auto& node : graph) {
        if (node.second.size() == 1) {
            endpoints.push_back(node.first);
        }
    }
    for (const std::size_t endpoint : endpoints) {
        std::size_t loopIndex = endpoint;
        for (;;) {
            const auto findEndpoint = graph.find(loopIndex);
            if (findEndpoint == graph.end()) {
                break;
            }
            if (findEndpoint->second.size() != 1) {
                break;
            }
            const std::size_t neighbor = *findEndpoint->second.begin();
            graph.erase(loopIndex);
            removed = true;
            const auto findNeighbor = graph.find(neighbor);
            if (findNeighbor == graph.end()) {
                break;
            }
            findNeighbor->second.erase(loopIndex);
            loopIndex = neighbor;
        }
    }
    return removed;
}

bool IsolineExtractor::collapseTriangles(std::vector<DVec3>& crossPoints, Graph& edgeConnectMap) {
    Graph triangleEdges;
    for (const auto& level0It : edgeConnectMap) {
        const std::size_t level0 = level0It.first;
        for (const std::size_t level1 : level0It.second) {
            const auto findLevel2 = edgeConnectMap.find(level1);
            if (findLevel2 == edgeConnectMap.end()) {
                continue;
            }
            for (const std::size_t level2 : findLevel2->second) {
                if (level0 == level2) {
                    continue;
                }
                const auto findLevel3 = edgeConnectMap.find(level2);
                if (findLevel3 == edgeConnectMap.end()) {
                    continue;
                }
                if (findLevel3->second.find(level0) != findLevel3->second.end()) {
                    triangleEdges[level0].insert(level1);
                    triangleEdges[level0].insert(level2);
                    triangleEdges[level1].insert(level0);
                    triangleEdges[level1].insert(level2);
                    triangleEdges[level2].insert(level0);
                    triangleEdges[level2].insert(level1);
                }
            }
        }
    }

    if (triangleEdges.empty()) {
        return false;
    }

    std::vector<std::vector<std::size_t>> clusters;
    std::unordered_set<std::size_t> visited;
    for (const auto& edge : triangleEdges) {
        std::queue<std::size_t> q;
        q.push(edge.first);
        std::vector<std::size_t> group;
        while (!q.empty()) {
            const std::size_t v = q.front();
            q.pop();
            if (visited.find(v) != visited.end()) {
                continue;
            }
            visited.insert(v);
            group.push_back(v);
            const auto findNeighbor = triangleEdges.find(v);
            if (findNeighbor == triangleEdges.end()) {
                continue;
            }
            for (const std::size_t neighbor : findNeighbor->second) {
                if (visited.find(neighbor) == visited.end()) {
                    q.push(neighbor);
                }
            }
        }
        if (!group.empty()) {
            clusters.push_back(group);
        }
    }

    // Size gate: the 3-cycles this pass exists for are SAMPLING artifacts — clusters of
    // near-coincident crossing samples — so their spatial extent is a tiny fraction of a
    // grid cell. Genuine small cells also read as 3-cycles (a CAD corner cone's cell,
    // one grid cell wide) and collapsing those to their centroid deletes real geometry:
    // at ~100 quads on the generated box the eight cube corners were chopped up to 0.17
    // (2/3 of a cell) off the surface. Only collapse clusters smaller than a quarter of
    // the average traced edge; bigger cycles are real cells and stay.
    double totalLength = 0.0;
    std::size_t edgeCount = 0;
    for (const auto& node : edgeConnectMap) {
        for (const std::size_t neighbor : node.second) {
            totalLength += dLength(crossPoints[node.first] - crossPoints[neighbor]);
            ++edgeCount;
        }
    }
    const double artifactExtent =
        edgeCount > 0 ? 0.25 * totalLength / static_cast<double>(edgeCount) : 0.0;

    for (const auto& group : clusters) {
        double extent = 0.0;
        for (std::size_t i = 0; i < group.size(); ++i) {
            for (std::size_t j = i + 1; j < group.size(); ++j) {
                extent = std::max(extent, dLength(crossPoints[group[i]] - crossPoints[group[j]]));
            }
        }
        if (extent > artifactExtent) {
            continue;  // a real (cone/corner) cell, not a sampling artifact
        }
        DVec3 center;
        for (const std::size_t v : group) {
            center += crossPoints[v];
        }
        center /= static_cast<double>(group.size());
        crossPoints[group[0]] = center;

        std::unordered_set<std::size_t> moveNeighbors;
        for (std::size_t i = 1; i < group.size(); ++i) {
            for (const std::size_t neighbor : edgeConnectMap[group[i]]) {
                moveNeighbors.insert(neighbor);
            }
        }
        moveNeighbors.erase(group[0]);

        for (const std::size_t neighbor : moveNeighbors) {
            edgeConnectMap[group[0]].insert(neighbor);
            edgeConnectMap[neighbor].insert(group[0]);
        }
        for (const std::size_t neighbor : moveNeighbors) {
            for (std::size_t i = 1; i < group.size(); ++i) {
                edgeConnectMap[neighbor].erase(group[i]);
                if (edgeConnectMap[neighbor].empty()) {
                    edgeConnectMap.erase(neighbor);
                }
            }
        }
        for (std::size_t i = 1; i < group.size(); ++i) {
            edgeConnectMap.erase(group[i]);
            edgeConnectMap[group[0]].erase(group[i]);
        }
        if (edgeConnectMap[group[0]].empty()) {
            edgeConnectMap.erase(group[0]);
        }
    }

    return true;
}

bool IsolineExtractor::collapseShortEdges(std::vector<DVec3>& crossPoints, Graph& edgeConnectMap) {
    double totalLength = 0.0;
    std::size_t edgeCount = 0;
    std::map<EdgePair, double> edgeLengths;
    for (const auto& node : edgeConnectMap) {
        for (const std::size_t neighbor : node.second) {
            if (edgeLengths.end() != edgeLengths.find({neighbor, node.first})) {
                continue;
            }
            const double edgeLength = dLength(crossPoints[node.first] - crossPoints[neighbor]);
            totalLength += edgeLength;
            edgeLengths.insert({{node.first, neighbor}, edgeLength});
            ++edgeCount;
        }
    }
    if (edgeCount == 0) {
        return false;
    }
    const double averageEdgeLength = totalLength / static_cast<double>(edgeCount);
    const double collapsedLength = averageEdgeLength * 0.01;
    bool collapsed = false;
    for (const auto& el : edgeLengths) {
        if (el.second > collapsedLength) {
            continue;
        }
        collapseEdge(crossPoints, edgeConnectMap, el.first);
        collapsed = true;
    }
    return collapsed;
}

void IsolineExtractor::collapseEdge(std::vector<DVec3>& crossPoints, Graph& edgeConnectMap,
                                    const EdgePair& edge) {
    const auto findSecondNeighbors = edgeConnectMap.find(edge.second);
    if (findSecondNeighbors == edgeConnectMap.end()) {
        return;
    }
    const auto findFirstNeighbors = edgeConnectMap.find(edge.first);
    if (findFirstNeighbors == edgeConnectMap.end()) {
        return;
    }
    if (findSecondNeighbors->second.end() == findSecondNeighbors->second.find(edge.first)) {
        return;
    }
    if (findFirstNeighbors->second.end() == findFirstNeighbors->second.find(edge.second)) {
        return;
    }
    const auto firstNeighbors = findFirstNeighbors->second;
    crossPoints[edge.second] = (crossPoints[edge.first] + crossPoints[edge.second]) * 0.5;
    for (const std::size_t neighbor : firstNeighbors) {
        if (neighbor == edge.second) {
            continue;
        }
        edgeConnectMap[edge.second].insert(neighbor);
        edgeConnectMap[neighbor].insert(edge.second);
        edgeConnectMap[neighbor].erase(edge.first);
    }
    edgeConnectMap.erase(edge.first);
    edgeConnectMap[edge.second].erase(edge.first);
    if (edgeConnectMap[edge.second].empty()) {
        edgeConnectMap.erase(edge.second);
    }
}

void IsolineExtractor::extractMesh(std::vector<DVec3>& points,
                                   const std::vector<std::size_t>& pointSourceTriangles,
                                   Graph& edgeConnectMap) {
    std::unordered_map<std::size_t, DVec3> triangleNormals;
    for (std::size_t pointIndex = 0; pointIndex < pointSourceTriangles.size(); ++pointIndex) {
        const auto& tri = m_triangles[pointSourceTriangles[pointIndex]];
        triangleNormals.insert(
            {pointIndex, dNormal(m_vertices[tri[0]], m_vertices[tri[1]], m_vertices[tri[2]])});
    }

    auto calculateFaceNormal = [&](const std::vector<std::size_t>& corners) {
        DVec3 center;
        for (const std::size_t c : corners) {
            center += points[c];
        }
        center /= static_cast<double>(corners.size());
        DVec3 normals;
        for (std::size_t i = 0; i < corners.size(); ++i) {
            normals += dNormal(points[corners[(i + 0) % corners.size()]],
                               points[corners[(i + 1) % corners.size()]], center);
        }
        return dNormalized(normals);
    };

    auto calculateSide = [&](const std::vector<std::size_t>& corners) {
        const DVec3 ringNormal = calculateFaceNormal(corners);
        DVec3 originalNormal;
        for (const std::size_t c : corners) {
            originalNormal += triangleNormals[c];
        }
        const double d = dDot(ringNormal, dNormalized(originalNormal));
        const double dotThreshold = 0.259;  // > 75 or < 105 degrees
        if (d > dotThreshold) {
            return 1;
        }
        if (d < -dotThreshold) {
            return -1;
        }
        return 0;
    };

    std::set<std::tuple<std::size_t, std::size_t, std::size_t>> corners;
    auto& halfEdges = m_halfEdges;
    auto isCornerUsed = [&](std::size_t previous, std::size_t current, std::size_t next) {
        if (corners.end() != corners.find(std::make_tuple(previous, current, next))) {
            return true;
        }
        return corners.end() != corners.find(std::make_tuple(next, current, previous));
    };
    auto isFaceCornerExist = [&](const std::vector<std::size_t>& vertices) {
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            const std::size_t j = (i + 1) % vertices.size();
            const std::size_t k = (i + 2) % vertices.size();
            if (isCornerUsed(vertices[i], vertices[j], vertices[k])) {
                return true;
            }
        }
        return false;
    };
    auto addFaceCorners = [&](const std::vector<std::size_t>& vertices) {
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            const std::size_t j = (i + 1) % vertices.size();
            const std::size_t k = (i + 2) % vertices.size();
            corners.insert(std::make_tuple(vertices[i], vertices[j], vertices[k]));
            corners.insert(std::make_tuple(vertices[k], vertices[j], vertices[i]));
        }
    };
    auto isFaceHalfEdgeExist = [&](const std::vector<std::size_t>& vertices) {
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            const std::size_t j = (i + 1) % vertices.size();
            if (halfEdges.end() != halfEdges.find({vertices[i], vertices[j]})) {
                return true;
            }
        }
        return false;
    };
    auto addFaceHalfEdges = [&](const std::vector<std::size_t>& vertices) {
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            const std::size_t j = (i + 1) % vertices.size();
            halfEdges.insert({vertices[i], vertices[j]});
        }
    };
    auto emitFace = [&](std::vector<std::size_t> forward) {
        if (isFaceCornerExist(forward)) {
            return;
        }
        const int side = calculateSide(forward);
        if (side > 0) {
            if (!isFaceHalfEdgeExist(forward)) {
                addFaceCorners(forward);
                addFaceHalfEdges(forward);
                m_remeshedPolygons.push_back(std::move(forward));
            }
        } else if (side < 0) {
            std::vector<std::size_t> reversed(forward.rbegin(), forward.rend());
            if (!isFaceHalfEdgeExist(reversed)) {
                addFaceCorners(reversed);
                addFaceHalfEdges(reversed);
                m_remeshedPolygons.push_back(std::move(reversed));
            }
        }
    };

    const std::size_t triangleRound = 4;
    for (std::size_t round = 0; round < 5; ++round) {
        for (const auto& level0It : edgeConnectMap) {
            const std::size_t level0 = level0It.first;
            const auto findLevel1 = edgeConnectMap.find(level0);
            if (findLevel1 == edgeConnectMap.end()) {
                continue;
            }
            for (const std::size_t level1 : findLevel1->second) {
                const auto findLevel2 = edgeConnectMap.find(level1);
                if (findLevel2 == edgeConnectMap.end()) {
                    continue;
                }
                if (halfEdges.find({level0, level1}) != halfEdges.end() &&
                    halfEdges.find({level1, level0}) != halfEdges.end()) {
                    continue;
                }
                for (const std::size_t level2 : findLevel2->second) {
                    if (level0 == level2) {
                        continue;
                    }
                    const auto findLevel3 = edgeConnectMap.find(level2);
                    if (findLevel3 == edgeConnectMap.end()) {
                        continue;
                    }
                    if (halfEdges.find({level1, level2}) != halfEdges.end() &&
                        halfEdges.find({level2, level1}) != halfEdges.end()) {
                        continue;
                    }
                    for (const std::size_t level3 : findLevel3->second) {
                        if (level0 == level3) {
                            if (triangleRound == round) {
                                emitFace({level0, level1, level2});
                                break;
                            }
                        } else {
                            if (triangleRound == round) {
                                break;
                            }
                        }
                        if (level1 == level3 || level0 == level3) {
                            continue;
                        }
                        const auto findLevel4 = edgeConnectMap.find(level3);
                        if (findLevel4 == edgeConnectMap.end()) {
                            continue;
                        }
                        if (halfEdges.find({level2, level3}) != halfEdges.end() &&
                            halfEdges.find({level3, level2}) != halfEdges.end()) {
                            continue;
                        }
                        for (const std::size_t level4 : findLevel4->second) {
                            if (level0 != level4) {
                                if (level2 == level4 || level1 == level4) {
                                    continue;
                                }
                                if (round < 1) {
                                    continue;
                                }
                                const auto findLevel5 = edgeConnectMap.find(level4);
                                if (findLevel5 == edgeConnectMap.end()) {
                                    continue;
                                }
                                if (halfEdges.find({level3, level4}) != halfEdges.end() &&
                                    halfEdges.find({level4, level3}) != halfEdges.end()) {
                                    continue;
                                }
                                for (const std::size_t level5 : findLevel5->second) {
                                    if (level0 != level5) {
                                        if (level3 == level5 || level2 == level5 ||
                                            level1 == level5) {
                                            continue;
                                        }
                                        if (round < 2) {
                                            continue;
                                        }
                                        const auto findLevel6 = edgeConnectMap.find(level5);
                                        if (findLevel6 == edgeConnectMap.end()) {
                                            continue;
                                        }
                                        if (halfEdges.find({level4, level5}) != halfEdges.end() &&
                                            halfEdges.find({level5, level4}) != halfEdges.end()) {
                                            continue;
                                        }
                                        for (const std::size_t level6 : findLevel6->second) {
                                            if (level0 != level6) {
                                                if (level4 == level6 || level3 == level6 ||
                                                    level2 == level6 || level1 == level6) {
                                                    continue;
                                                }
                                                if (round < 3) {
                                                    continue;
                                                }
                                                const auto findLevel7 = edgeConnectMap.find(level6);
                                                if (findLevel7 == edgeConnectMap.end()) {
                                                    continue;
                                                }
                                                if (halfEdges.find({level5, level6}) !=
                                                        halfEdges.end() &&
                                                    halfEdges.find({level6, level5}) !=
                                                        halfEdges.end()) {
                                                    continue;
                                                }
                                                for (const std::size_t level7 :
                                                     findLevel7->second) {
                                                    if (level0 != level7) {
                                                        continue;
                                                    }
                                                    if (3 != round) {
                                                        break;
                                                    }
                                                    emitFace({level0, level1, level2, level3,
                                                              level4, level5, level6});
                                                    break;
                                                }
                                                continue;
                                            }
                                            if (2 != round) {
                                                break;
                                            }
                                            emitFace(
                                                {level0, level1, level2, level3, level4, level5});
                                            break;
                                        }
                                        continue;
                                    }
                                    if (1 != round) {
                                        break;
                                    }
                                    emitFace({level0, level1, level2, level3, level4});
                                    break;
                                }
                                continue;
                            }
                            if (0 != round) {
                                break;
                            }
                            emitFace({level0, level1, level2, level3});
                            break;
                        }
                    }
                }
            }
        }
    }

    m_remeshedVertices = points;
}

void IsolineExtractor::extractConnections(std::vector<DVec3>& crossPoints,
                                          std::vector<std::size_t>& sourceTriangles,
                                          std::set<EdgePair>& connections) {
    std::map<PositionKey, std::size_t> crossPointMap;

    auto addCrossPoint = [&](const DVec3& position3, std::size_t triangleIndex) {
        const auto insertResult =
            crossPointMap.insert({PositionKey(position3), crossPoints.size()});
        if (insertResult.second) {
            crossPoints.push_back(position3);
            sourceTriangles.push_back(triangleIndex);
        }
        return insertResult.first->second;
    };
    auto addConnection = [&](std::size_t fromPointIndex, std::size_t toPointIndex) {
        if (fromPointIndex != toPointIndex) {
            connections.insert({fromPointIndex, toPointIndex});
        }
    };

    struct CrossPoint {
        DVec3 position3;
        DVec2 position2;
        int integer = 0;
    };

    for (std::size_t triangleIndex = 0; triangleIndex < m_triangles.size(); ++triangleIndex) {
        const auto& cornerUvs = m_triangleUvs[triangleIndex];
        const auto& cornerIndices = m_triangles[triangleIndex];

        // Extract intersections of isolines with edges.
        std::map<int, std::vector<std::vector<CrossPoint>>> lines[2];
        bool edgeCollapsed[2][3] = {{false, false, false}, {false, false, false}};
        for (std::size_t i = 0; i < 2; ++i) {
            for (std::size_t j = 0; j < 3; ++j) {
                const std::size_t k = (j + 1) % 3;
                const DVec2& current = cornerUvs[j];
                const DVec2& next = cornerUvs[k];
                if (dIsZero(static_cast<double>(static_cast<int>(current[i])) - current[i]) &&
                    dIsZero(current[i] - next[i])) {
                    const int integer = static_cast<int>(current[i]);
                    edgeCollapsed[i][j] = true;
                    CrossPoint fromPoint;
                    fromPoint.position3 = m_vertices[cornerIndices[j]];
                    fromPoint.position2 = cornerUvs[j];
                    fromPoint.integer = integer;
                    CrossPoint toPoint;
                    toPoint.position3 = m_vertices[cornerIndices[k]];
                    toPoint.position2 = cornerUvs[k];
                    toPoint.integer = integer;
                    lines[i][integer].push_back({fromPoint, toPoint});
                }
            }
            std::map<int, std::vector<CrossPoint>> points;
            for (std::size_t j = 0; j < 3; ++j) {
                const std::size_t k = (j + 1) % 3;
                const DVec2& current = cornerUvs[j];
                const DVec2& next = cornerUvs[k];
                const double distance = std::abs(current[i] - next[i]);
                if (static_cast<int>(current[i]) != static_cast<int>(next[i]) ||
                    (current[i] > 0) != (next[i] > 0)) {
                    int lowInteger = 0;
                    int highInteger = 0;
                    double fromPosition = 0.0;
                    std::size_t fromIndex = 0;
                    std::size_t toIndex = 0;
                    if (current[i] < next[i]) {
                        lowInteger = static_cast<int>(current[i]);
                        highInteger = static_cast<int>(next[i]);
                        fromPosition = current[i];
                        fromIndex = j;
                        toIndex = k;
                    } else {
                        lowInteger = static_cast<int>(next[i]);
                        highInteger = static_cast<int>(current[i]);
                        fromPosition = next[i];
                        fromIndex = k;
                        toIndex = j;
                    }
                    for (int integer = lowInteger; integer <= highInteger; ++integer) {
                        const double ratio =
                            (static_cast<double>(integer) - fromPosition) / distance;
                        if (ratio < 0 || ratio > 1) {
                            continue;
                        }
                        if (dIsZero(ratio) || dIsZero(ratio - 1.0)) {
                            if (edgeCollapsed[i][j]) {
                                continue;
                            }
                        }
                        CrossPoint point;
                        point.position3 = m_vertices[cornerIndices[fromIndex]] * (1 - ratio) +
                                          m_vertices[cornerIndices[toIndex]] * ratio;
                        point.position2 =
                            cornerUvs[fromIndex] * (1 - ratio) + cornerUvs[toIndex] * ratio;
                        point.integer = integer;
                        points[integer].push_back(point);
                    }
                }
            }
            for (const auto& pt : points) {
                for (std::size_t pointIndex = 0; pointIndex < pt.second.size(); ++pointIndex) {
                    const std::size_t nextPointIndex = (pointIndex + 1) % pt.second.size();
                    lines[i][pt.first].push_back(
                        {pt.second[pointIndex], pt.second[nextPointIndex]});
                }
            }
        }

        // Segment lines by the perpendicular isolines so each crossing splits a segment.
        for (std::size_t i = 0; i < 2; ++i) {
            const std::size_t j = (i + 1) % 2;
            for (const auto& targetIt : lines[i]) {
                for (const auto& target : targetIt.second) {
                    std::vector<std::vector<CrossPoint>> segments = {target};
                    for (const auto& splitIt : lines[j]) {
                        const std::size_t coordIndex = j;
                        const double segmentPosition =
                            splitIt.second.front().front().position2[coordIndex];
                        for (int segmentIndex = static_cast<int>(segments.size()) - 1;
                             segmentIndex >= 0; --segmentIndex) {
                            auto& segment = segments[static_cast<std::size_t>(segmentIndex)];
                            const DVec2& uv0 = segment[0].position2;
                            const DVec2& uv1 = segment[1].position2;
                            const double distance = std::abs(uv0[coordIndex] - uv1[coordIndex]);
                            if (dIsZero(distance)) {
                                continue;
                            }
                            double fromPosition = 0.0;
                            double toPosition = 0.0;
                            std::size_t fromIndex = 0;
                            std::size_t toIndex = 0;
                            if (uv0[coordIndex] < uv1[coordIndex]) {
                                fromPosition = uv0[coordIndex];
                                toPosition = uv1[coordIndex];
                                fromIndex = 0;
                                toIndex = 1;
                            } else {
                                fromPosition = uv1[coordIndex];
                                toPosition = uv0[coordIndex];
                                fromIndex = 1;
                                toIndex = 0;
                            }
                            if (segmentPosition < fromPosition || segmentPosition > toPosition) {
                                continue;
                            }
                            const double ratio = (segmentPosition - fromPosition) / distance;
                            const DVec3 position3 = segment[fromIndex].position3 * (1 - ratio) +
                                                    segment[toIndex].position3 * ratio;
                            const DVec2 position2 = segment[fromIndex].position2 * (1 - ratio) +
                                                    segment[toIndex].position2 * ratio;
                            const int integer = segment[toIndex].integer;
                            CrossPoint newFromPoint;
                            newFromPoint.position3 = position3;
                            newFromPoint.position2 = position2;
                            newFromPoint.integer = integer;
                            const CrossPoint newToPoint = segment[toIndex];
                            segment[toIndex] = newFromPoint;
                            segments.push_back({newFromPoint, newToPoint});
                        }
                    }
                    for (const auto& segment : segments) {
                        addConnection(addCrossPoint(segment[0].position3, triangleIndex),
                                      addCrossPoint(segment[1].position3, triangleIndex));
                    }
                }
            }
        }
    }
}

bool IsolineExtractor::testPointInTriangle(const std::vector<DVec3>& points,
                                           const std::array<std::size_t, 3>& triangle,
                                           const std::vector<std::size_t>& testPoints) {
    const DVec3 triangleNormal =
        dNormal(points[triangle[0]], points[triangle[1]], points[triangle[2]]);
    std::vector<DVec3> pointsIn3d;
    for (const std::size_t it : triangle) {
        pointsIn3d.push_back(points[it]);
    }
    for (const std::size_t it : testPoints) {
        pointsIn3d.push_back(points[it]);
    }
    std::vector<DVec2> pointsIn2d;
    const DVec3 origin =
        (points[triangle[0]] + points[triangle[1]] + points[triangle[2]]) * (1.0 / 3.0);
    const DVec3 axis = dNormalized(points[triangle[0]] - origin);
    dProject(pointsIn3d, pointsIn2d, triangleNormal, axis, origin);
    const DVec2& a = pointsIn2d[0];
    const DVec2& b = pointsIn2d[1];
    const DVec2& c = pointsIn2d[2];
    for (std::size_t i = 3; i < pointsIn2d.size(); ++i) {
        if (dInTriangle2(a, b, c, pointsIn2d[i])) {
            return true;
        }
    }
    return false;
}

void IsolineExtractor::rebuildHalfEdges() {
    m_halfEdges.clear();
    for (const auto& face : m_remeshedPolygons) {
        for (std::size_t i = 0; i < face.size(); ++i) {
            const std::size_t j = (i + 1) % face.size();
            m_halfEdges.insert({face[i], face[j]});
        }
    }
}

// Squared distance from `p` to segment [a,b].
static double pointSegmentDistanceSq(const DVec3& p, const DVec3& a, const DVec3& b) {
    const DVec3 ab{b.x - a.x, b.y - a.y, b.z - a.z};
    const DVec3 ap{p.x - a.x, p.y - a.y, p.z - a.z};
    const double len2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
    double t = 0.0;
    if (len2 > 0.0) {
        t = (ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / len2;
        t = std::max(0.0, std::min(1.0, t));
    }
    const DVec3 d{ap.x - t * ab.x, ap.y - t * ab.y, ap.z - t * ab.z};
    return d.x * d.x + d.y * d.y + d.z * d.z;
}

// Collect the INPUT surface's genuine boundary: triangle edges used by exactly one
// triangle. Isolines terminate on those edges, so an output loop tracing the real
// rim sits on them, while a loop around a hole the extraction opened does not.
void IsolineExtractor::buildInputBoundary() {
    std::map<EdgePair, int> edgeUse;
    for (const auto& tri : m_triangles) {
        for (std::size_t k = 0; k < 3; ++k) {
            const std::size_t a = tri[k];
            const std::size_t b = tri[(k + 1) % 3];
            ++edgeUse[{std::min(a, b), std::max(a, b)}];
        }
    }
    double lenSum = 0.0;
    for (const auto& [edge, uses] : edgeUse) {
        if (uses != 1) {
            continue;
        }
        const DVec3& a = m_vertices[edge.first];
        const DVec3& b = m_vertices[edge.second];
        m_inputBoundary.emplace_back(a, b);
        const double dx = a.x - b.x;
        const double dy = a.y - b.y;
        const double dz = a.z - b.z;
        lenSum += std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    // "On the boundary" means within a quarter of a mean boundary edge. The
    // crossing points start exactly on those edges; the quarter-edge slack absorbs
    // the drift collapseShortEdges introduces by averaging merged samples.
    m_boundaryTol =
        m_inputBoundary.empty() ? 0.0 : 0.25 * lenSum / static_cast<double>(m_inputBoundary.size());
}

// True if most of `loop` sits on the input boundary. A majority vote (not "all")
// because the cleanup may pull an occasional loop vertex slightly inboard; a hole
// in the surface interior scores ~0 either way, so the two cases separate cleanly.
bool IsolineExtractor::tracesInputBoundary(const std::vector<std::size_t>& loop) const {
    if (m_inputBoundary.empty() || loop.empty()) {
        return false;
    }
    const double tolSq = m_boundaryTol * m_boundaryTol;
    std::size_t onBoundary = 0;
    for (const std::size_t vi : loop) {
        const DVec3& p = m_remeshedVertices[vi];
        for (const auto& [a, b] : m_inputBoundary) {
            if (pointSegmentDistanceSq(p, a, b) <= tolSq) {
                ++onBoundary;
                break;
            }
        }
    }
    return onBoundary * 2 > loop.size();
}

void IsolineExtractor::fixHoles() {
    // Policy check first: below 3 the caller has disabled filling outright, so
    // there is nothing to do and no reason to build the boundary index.
    if (m_holeFillMaxBoundary < 3) {
        return;
    }
    if (m_preserveInputBoundary && m_inputBoundary.empty()) {
        buildInputBoundary();
    }
    std::vector<std::vector<std::size_t>> loops;
    searchBoundaries(m_halfEdges, loops);
    for (auto& loop : loops) {
        // Longer than the configured limit -> left open, as the spec requires.
        if (loop.size() > static_cast<std::size_t>(m_holeFillMaxBoundary)) {
            continue;
        }
        // The surface's own rim is not a hole — filling it would close the mesh.
        if (m_preserveInputBoundary && tracesInputBoundary(loop)) {
            continue;
        }
        fixHoleWithQuads(loop, true);
        if (loop.size() >= 4) {
            fixHoleWithQuads(loop, false);
        }
    }
}

void IsolineExtractor::fixHoleWithQuads(std::vector<std::size_t>& hole, bool checkScore) {
    auto recordHalfEdgesOfLastPolygon = [&]() {
        const auto& face = m_remeshedPolygons.back();
        for (std::size_t i = 0; i < face.size(); ++i) {
            const std::size_t j = (i + 1) % face.size();
            m_halfEdges.insert({face[i], face[j]});
        }
    };

    // Emit the terminal 3-/4-gon that closes a hole, unless one of its directed half-edges is
    // already taken (the same guard the >4 path applies to every candidate quad).
    //
    // CLEARING `hole` IS LOAD-BEARING, not tidiness. `hole` is an in/out parameter and `fixHoles`
    // calls this twice per loop — once score-checked, once not — relying on the first pass to
    // consume what it filled. The terminal cases used to `return` with `hole` still full, so a
    // loop that reduced to exactly 4 edges was closed by the first call and closed AGAIN,
    // identically, by the second. (The 3-gon terminal cannot double-fill on its own: `fixHoles`
    // only issues the second call when `loop.size() >= 4`. It is routed through the same helper
    // so the invariant holds for any future caller.)
    //
    // Two coincident faces are edge-count-manifold on their own, so nothing downstream rejected
    // them; the pure-quad subdivision then gave each its own face point and turned the shared rim
    // into a genuine non-manifold edge. MEASURED on the corpus at 5 densities (2600-4200):
    // stanford-bunny carried 40 non-manifold edges across 4 of its 5 densities, and 0 after this.
    auto closeHole = [&](std::vector<std::size_t> face) {
        for (std::size_t i = 0; i < face.size(); ++i) {
            if (m_halfEdges.find({face[i], face[(i + 1) % face.size()]}) != m_halfEdges.end()) {
                hole.clear();
                return;
            }
        }
        m_remeshedPolygons.push_back(std::move(face));
        recordHalfEdgesOfLastPolygon();
        hole.clear();
    };

    for (;;) {
        if (hole.size() <= 2) {
            return;
        }
        if (3 == hole.size()) {
            closeHole({hole[2], hole[1], hole[0]});
            return;
        }
        if (4 == hole.size()) {
            closeHole({hole[3], hole[2], hole[1], hole[0]});
            return;
        }

        const int n = static_cast<int>(hole.size());
        std::vector<std::pair<int, double>> edgeScores;
        edgeScores.reserve(hole.size());
        for (int i = 0; i < n; ++i) {
            const int h = (i + n - 1) % n;
            const int j = (i + 1) % n;
            const int k = (j + 1) % n;
            const DVec3 left = dNormalized(m_remeshedVertices[hole[static_cast<std::size_t>(h)]] -
                                           m_remeshedVertices[hole[static_cast<std::size_t>(i)]]);
            const DVec3 right = dNormalized(m_remeshedVertices[hole[static_cast<std::size_t>(k)]] -
                                            m_remeshedVertices[hole[static_cast<std::size_t>(j)]]);
            edgeScores.push_back({i, dDot(left, right)});
        }
        std::sort(edgeScores.begin(), edgeScores.end(),
                  [](const std::pair<int, double>& a, const std::pair<int, double>& b) {
                      return a.second < b.second;
                  });
        bool holeChanged = false;
        for (int edgeIndex = static_cast<int>(edgeScores.size()) - 1; edgeIndex >= 0; --edgeIndex) {
            const auto& score = edgeScores[static_cast<std::size_t>(edgeIndex)];
            if (checkScore && score.second <= 0) {
                return;
            }
            const int i = score.first;
            const int h = (i + n - 1) % n;
            const int j = (i + 1) % n;
            const int k = (j + 1) % n;
            std::vector<std::size_t> candidate = {
                hole[static_cast<std::size_t>(k)], hole[static_cast<std::size_t>(j)],
                hole[static_cast<std::size_t>(i)], hole[static_cast<std::size_t>(h)]};
            if (m_halfEdges.end() != m_halfEdges.find({candidate[0], candidate[1]}) ||
                m_halfEdges.end() != m_halfEdges.find({candidate[1], candidate[2]}) ||
                m_halfEdges.end() != m_halfEdges.find({candidate[2], candidate[3]}) ||
                m_halfEdges.end() != m_halfEdges.find({candidate[3], candidate[0]})) {
                continue;
            }
            std::vector<std::size_t> remainPoints;
            for (int w = 0; w < n; ++w) {
                if (w == i || w == j || w == h || w == k) {
                    continue;
                }
                remainPoints.push_back(hole[static_cast<std::size_t>(w)]);
            }
            if (testPointInTriangle(m_remeshedVertices, {candidate[0], candidate[1], candidate[2]},
                                    remainPoints) ||
                testPointInTriangle(m_remeshedVertices, {candidate[2], candidate[3], candidate[0]},
                                    remainPoints)) {
                continue;
            }
            m_remeshedPolygons.push_back(candidate);
            recordHalfEdgesOfLastPolygon();
            std::vector<std::size_t> newHole;
            for (int w = 0; w < n; ++w) {
                if (w == i || w == j) {
                    continue;
                }
                newHole.push_back(hole[static_cast<std::size_t>(w)]);
            }
            hole = newHole;
            holeChanged = true;
            break;
        }
        if (!holeChanged) {
            break;
        }
    }
}

void IsolineExtractor::searchBoundaries(const std::set<EdgePair>& halfEdges,
                                        std::vector<std::vector<std::size_t>>& loops) {
    Graph nextMap;
    for (const auto& he : halfEdges) {
        if (halfEdges.end() != halfEdges.find({he.second, he.first})) {
            continue;
        }
        nextMap[he.first].insert(he.second);
    }

    while (!nextMap.empty()) {
        auto it = nextMap.begin();
        std::vector<std::size_t> loop;
        const std::size_t startVertex = it->first;
        bool valid = false;
        while (it != nextMap.end()) {
            if (startVertex == it->first && loop.size() >= 3) {
                valid = true;
                break;
            }
            loop.push_back(it->first);
            if (it->second.size() != 1) {
                break;
            }
            it = nextMap.find(*it->second.begin());
        }
        for (const std::size_t v : loop) {
            nextMap.erase(v);
        }
        if (valid) {
            loops.push_back(loop);
        }
    }
}

bool IsolineExtractor::removeIsolatedFaces() {
    std::vector<std::vector<std::vector<std::size_t>>> quadsIslands;
    splitToIslands(m_remeshedPolygons, quadsIslands);
    if (quadsIslands.empty()) {
        return false;
    }
    // The reference kept ONLY the largest island, silently discarding every other one.
    // That is correct for its purpose — shedding incoherent junk fragments — but on a
    // CLOSED surface it is silent geometry loss when a genuine patch traced
    // disconnected (a feature-bounded CAD face whose seam misaligned): box_sharp
    // dropped BOTH cube caps this way and still reported success. On closed surfaces
    // keep every island that is clearly not junk (at least 10% of the largest island,
    // and at least 4 faces); the quadrangulator's area-coverage gate downstream reports
    // honestly if significant geometry is still missing. OPEN surfaces keep the
    // largest-only policy: their raw trace legitimately fragments into many mid-size
    // patches (the under-trace fixHoles repairs), and retaining those regressed the
    // open-paraboloid uniformity guard (edge CV 0.31 -> 1.67).
    if (m_preserveInputBoundary) {
        m_remeshedPolygons = *std::max_element(
            quadsIslands.begin(), quadsIslands.end(),
            [](const std::vector<std::vector<std::size_t>>& a,
               const std::vector<std::vector<std::size_t>>& b) { return a.size() < b.size(); });
        return true;
    }
    std::size_t largest = 0;
    for (const auto& island : quadsIslands) {
        largest = std::max(largest, island.size());
    }
    const std::size_t keepThreshold = std::max<std::size_t>(4, largest / 10);
    std::vector<std::vector<std::size_t>> kept;
    for (auto& island : quadsIslands) {
        if (island.size() >= keepThreshold || island.size() == largest) {
            for (auto& face : island) {
                kept.push_back(std::move(face));
            }
        }
    }
    m_remeshedPolygons = std::move(kept);
    // Historical control flow: the reference returned true whenever any island existed
    // (the caller then re-runs rebuildHalfEdges + fixHoles); keep that exact behavior.
    return true;
}

bool IsolineExtractor::removeNonManifoldFaces() {
    bool changed = false;
    std::map<EdgePair, std::size_t> edgeToFaceMap;
    buildEdgeToFaceMap(m_remeshedPolygons, edgeToFaceMap);
    std::unordered_map<std::size_t, std::size_t> vertexOpenBoundaryCountMap;
    for (const auto& e : edgeToFaceMap) {
        if (edgeToFaceMap.end() != edgeToFaceMap.find({e.first.second, e.first.first})) {
            continue;
        }
        vertexOpenBoundaryCountMap[e.first.first]++;
        vertexOpenBoundaryCountMap[e.first.second]++;
    }
    std::vector<std::vector<std::size_t>> manifoldFaces;
    for (const auto& face : m_remeshedPolygons) {
        bool isNonManifold = false;
        for (const std::size_t v : face) {
            const auto findCount = vertexOpenBoundaryCountMap.find(v);
            if (findCount == vertexOpenBoundaryCountMap.end()) {
                continue;
            }
            if (findCount->second > 2) {
                isNonManifold = true;
                break;
            }
        }
        if (isNonManifold) {
            changed = true;
            continue;
        }
        manifoldFaces.push_back(face);
    }
    m_remeshedPolygons = manifoldFaces;
    return changed;
}

void IsolineExtractor::extract() {
    std::vector<DVec3> crossPoints;
    std::vector<std::size_t> crossPointSourceTriangles;
    std::set<EdgePair> connections;
    extractConnections(crossPoints, crossPointSourceTriangles, connections);

    // M2 tracer core: raw connection adjacency -> orbit-walk into oriented quads.
    // For a seamless integer grid this is exact — every crossing is a transversal
    // (valence-4) node — so the raw graph already yields a clean quad mesh.
    //
    // The reference additionally runs a closed-surface graph-cleanup stage here
    //   extractEdges (simplifyGraph) -> collapseShortEdges -> collapseTriangles
    //   -> removeSingleEndpoints  [and, after extractMesh]  fixHoles ->
    //   removeIsolatedFaces / removeNonManifoldFaces.
    // Those steps merge redundant mid-isoline sample points and patch small holes
    // on a WATERTIGHT input. They are faithfully ported below but intentionally NOT
    // wired into this path yet: on an OPEN surface they corrupt the boundary
    // (simplifyGraph deletes genuine valence-2 boundary corners; fixHoles fills the
    // outer boundary loop as if it were a hole). Enabling them for dense/curved
    // seamless UVs is Milestone 3 and needs boundary-aware gating (e.g. a turn-angle
    // guard on simplifyGraph and outer-boundary detection in fixHoles).
    // TODO(M3): wire the cleanup stage with open-boundary safety.
    Graph edgeConnectMap;
    // The graph-cleanup collapses all funnel into `simplifyGraph`, which dissolves EVERY valence-2
    // node. On a closed surface that safely merges redundant mid-isoline samples. On an OPEN
    // surface it is destructive: most isoline samples are legitimately valence-2, so dissolving
    // them merges cells into long uneven quads. Bisected on an open paraboloid at request 1200:
    // running the collapses takes edge CV from 0.312 to 1.696 and halves the face count
    // (1920 -> 954), while `fixHoles` alone -- which fills the under-traced gaps -- is what
    // actually makes the open trace work. So open islands take the raw graph and skip straight to
    // fixHoles below; only closed islands run the collapse pipeline. `m_preserveInputBoundary` is
    // the open-island flag (set !closed alongside m_runClosedSurfaceCleanup).
    const bool runCollapsePipeline = m_runClosedSurfaceCleanup && !m_preserveInputBoundary;
    if (runCollapsePipeline) {
        extractEdges(connections, edgeConnectMap);
        if (collapseShortEdges(crossPoints, edgeConnectMap)) {
            simplifyGraph(edgeConnectMap);
        }
        collapseTriangles(crossPoints, edgeConnectMap);
        if (removeSingleEndpoints(edgeConnectMap)) {
            simplifyGraph(edgeConnectMap);
        }
    } else {
        for (const auto& c : connections) {
            edgeConnectMap[c.first].insert(c.second);
            edgeConnectMap[c.second].insert(c.first);
        }
    }

    extractMesh(crossPoints, crossPointSourceTriangles, edgeConnectMap);

    if (m_runClosedSurfaceCleanup) {
        fixHoles();
        bool changed = false;
        if (removeIsolatedFaces()) {
            changed = true;
        }
        while (removeNonManifoldFaces()) {
            changed = true;
            removeIsolatedFaces();
        }
        if (changed) {
            rebuildHalfEdges();
            fixHoles();
        }
    }

    // Compact: keep only referenced vertices, in ascending original order.
    std::set<std::size_t> usedVertices;
    for (const auto& face : m_remeshedPolygons) {
        for (const std::size_t v : face) {
            usedVertices.insert(v);
        }
    }
    if (usedVertices.size() < m_remeshedVertices.size()) {
        std::vector<DVec3> compactedVertices;
        std::unordered_map<std::size_t, std::size_t> oldToNew;
        compactedVertices.reserve(usedVertices.size());
        for (const std::size_t oldIndex : usedVertices) {
            oldToNew[oldIndex] = compactedVertices.size();
            compactedVertices.push_back(m_remeshedVertices[oldIndex]);
        }
        for (auto& face : m_remeshedPolygons) {
            for (auto& v : face) {
                v = oldToNew[v];
            }
        }
        m_remeshedVertices = std::move(compactedVertices);
    }
}

}  // namespace

namespace {

// Cap elimination (PRIMARY LEVER for native irregular %). The isoline tracer
// leaves a few percent of NON-QUAD "cap" faces (triangles / pentagons / hexagons)
// at cone and boundary regions. The pipeline's pure-quad path then linear-subdivides
// (Catmull-Clark split), turning EVERY non-quad n-gon into a valence-n fan centre — a
// permanent irregular vertex. Eliminating the non-quad caps in the COARSE mesh, before
// that subdivision, removes exactly those fan-centre irregulars.
//
// Two operations, both provably non-regressing:
//   (1) pair two edge-adjacent ODD faces (tri-tri, tri-pent, pent-pent) into an even
//       polygon and split that into quads;
//   (2) split a leftover even n-gon (hexagon/octagon) into quads by a chord.
// A candidate is applied only when it does not INCREASE the final irregular count.
// After a uniform Catmull-Clark split the final irregular set is exactly
//   {coarse vertices with edge-valence != 4}  union  {centroids of non-quad faces},
// so we can score each op on the coarse mesh: it is accepted iff
//   delta(non-quad centroids) + delta(vertices whose valence crosses 4) <= 0.
// This keeps the mesh watertight (we only re-partition faces over the SAME vertex set,
// never touching outer boundaries) and never regresses c.irregular_pct.
class QuadCapCleaner {
public:
    using Poly = std::vector<std::size_t>;

    QuadCapCleaner(const std::vector<Vec3>& verts, std::vector<Poly>& faces)
        : m_verts(verts), m_faces(faces) {}

    void run() {
        build();
        // Phase 1: merge edge-adjacent odd faces, then split the even union into quads.
        for (std::size_t i = 0; i < m_originalCount; ++i) {
            if (isMergeCandidate(i)) {
                tryMergeWithNeighbour(i);
            }
        }
        // Phase 2: split any surviving even non-quad face (hexagons, octagons).
        for (std::size_t i = 0; i < m_originalCount; ++i) {
            if (!m_alive[i]) {
                continue;
            }
            const std::size_t n = m_faces[i].size();
            if (n > 4 && (n % 2) == 0) {
                trySplitEven(i);
            }
        }
        compact();
    }

private:
    static std::uint64_t undirectedKey(std::size_t a, std::size_t b) {
        if (a > b) {
            std::swap(a, b);
        }
        return (static_cast<std::uint64_t>(a) << 32) | static_cast<std::uint64_t>(b);
    }
    static std::uint64_t directedKey(std::size_t a, std::size_t b) {
        return (static_cast<std::uint64_t>(a) << 32) | static_cast<std::uint64_t>(b);
    }
    static bool isRegular(int valence) { return valence == 4; }

    // Add (sign +1) or remove (sign -1) a face's undirected edges from the live edge
    // multiset, tracking per-vertex edge valence as edges appear (0->1) / vanish (1->0).
    void applyFaceEdges(const Poly& f, int sign) {
        const std::size_t n = f.size();
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t a = f[i];
            const std::size_t b = f[(i + 1) % n];
            const std::uint64_t key = undirectedKey(a, b);
            int& c = m_edgeCount[key];
            if (sign > 0) {
                if (c == 0) {
                    ++m_deg[a];
                    ++m_deg[b];
                }
                ++c;
            } else {
                --c;
                if (c == 0) {
                    --m_deg[a];
                    --m_deg[b];
                }
            }
        }
    }

    void build() {
        m_originalCount = m_faces.size();
        m_alive.assign(m_originalCount, true);
        m_deg.assign(m_verts.size(), 0);
        m_edgeCount.clear();
        m_dirToFace.clear();
        for (std::size_t fi = 0; fi < m_originalCount; ++fi) {
            const Poly& f = m_faces[fi];
            applyFaceEdges(f, +1);
            for (std::size_t i = 0; i < f.size(); ++i) {
                m_dirToFace[directedKey(f[i], f[(i + 1) % f.size()])] = fi;
            }
        }
    }

    bool isMergeCandidate(std::size_t i) const {
        if (!m_alive[i]) {
            return false;
        }
        const std::size_t n = m_faces[i].size();
        return n != 4 && (n % 2) == 1;  // triangle / pentagon / heptagon
    }

    // Merge two polygons across shared directed edge a->b (in F1) / b->a (in F2).
    // Returns the CCW union polygon, or empty on a degenerate share (repeated vertex).
    static Poly mergePolys(const Poly& f1, const Poly& f2, std::size_t a, std::size_t b) {
        Poly out;
        out.reserve(f1.size() + f2.size() - 2);
        // Walk f1 starting at b around to a (this is f1 minus the a->b edge).
        std::size_t start1 = f1.size();
        for (std::size_t i = 0; i < f1.size(); ++i) {
            if (f1[i] == b) {
                start1 = i;
                break;
            }
        }
        if (start1 == f1.size()) {
            return {};
        }
        for (std::size_t k = 0; k < f1.size(); ++k) {
            out.push_back(f1[(start1 + k) % f1.size()]);  // b ... a
        }
        // Insert f2's interior (from a to b, excluding both endpoints).
        std::size_t startA = f2.size();
        for (std::size_t i = 0; i < f2.size(); ++i) {
            if (f2[i] == a) {
                startA = i;
                break;
            }
        }
        if (startA == f2.size()) {
            return {};
        }
        for (std::size_t k = 1; k + 1 < f2.size(); ++k) {
            out.push_back(f2[(startA + k) % f2.size()]);
        }
        // Reject if any vertex repeats (the two faces shared more than one edge).
        std::set<std::size_t> seen(out.begin(), out.end());
        if (seen.size() != out.size()) {
            return {};
        }
        return out;
    }

    // Fan-split an even polygon into quads from vertex index `start`.
    static std::vector<Poly> fanSplit(const Poly& p, std::size_t start) {
        std::vector<Poly> quads;
        const std::size_t n = p.size();
        for (std::size_t i = 1; i + 1 < n / 2 + 1; ++i) {
            // quad: (start, start+2i-1, start+2i, start+2i+1)
            quads.push_back({p[start], p[(start + 2 * i - 1) % n], p[(start + 2 * i) % n],
                             p[(start + 2 * i + 1) % n]});
        }
        return quads;
    }

    double quadArea(const Poly& q) const {
        const Vec3 a = m_verts[q[0]];
        const Vec3 b = m_verts[q[1]];
        const Vec3 c = m_verts[q[2]];
        const Vec3 d = m_verts[q[3]];
        const double t1 = 0.5 * static_cast<double>(length(cross(b - a, c - a)));
        const double t2 = 0.5 * static_cast<double>(length(cross(c - a, d - a)));
        return t1 + t2;
    }

    // Score replacing `oldIdx` faces by `newPolys`: feasibility (manifold, non-degenerate)
    // plus the resulting change in irregular count. Returns {feasible, deltaIrregular}.
    std::pair<bool, int> score(const std::vector<std::size_t>& oldIdx,
                               const std::vector<Poly>& newPolys) const {
        std::map<std::uint64_t, int> mult;  // net change in edge multiplicity
        for (const std::size_t fi : oldIdx) {
            const Poly& f = m_faces[fi];
            for (std::size_t i = 0; i < f.size(); ++i) {
                --mult[undirectedKey(f[i], f[(i + 1) % f.size()])];
            }
        }
        int newNonQuad = 0;
        for (const Poly& p : newPolys) {
            if (p.size() != 4) {
                ++newNonQuad;
            }
            if (p.size() == 4 && quadArea(p) <= 1e-12) {
                return {false, 0};  // degenerate quad
            }
            for (std::size_t i = 0; i < p.size(); ++i) {
                ++mult[undirectedKey(p[i], p[(i + 1) % p.size()])];
            }
        }
        int deltaVert = 0;
        for (const auto& [key, delta] : mult) {
            if (delta == 0) {
                continue;
            }
            const auto it = m_edgeCount.find(key);
            const int before = (it == m_edgeCount.end()) ? 0 : it->second;
            const int after = before + delta;
            if (after < 0 || after > 2) {
                return {false, 0};  // would create a non-manifold edge
            }
            const std::size_t a = static_cast<std::size_t>(key >> 32);
            const std::size_t b = static_cast<std::size_t>(key & 0xffffffffU);
            if (before == 0 && after > 0) {
                deltaVert += valenceCross(a, +1) + valenceCross(b, +1);
            } else if (before > 0 && after == 0) {
                deltaVert += valenceCross(a, -1) + valenceCross(b, -1);
            }
        }
        int oldNonQuad = 0;
        for (const std::size_t fi : oldIdx) {
            if (m_faces[fi].size() != 4) {
                ++oldNonQuad;
            }
        }
        return {true, deltaVert + (newNonQuad - oldNonQuad)};
    }

    // Irregular-count change if vertex v's edge valence shifts by `d` (+1/-1).
    int valenceCross(std::size_t v, int d) const {
        const int before = m_deg[v];
        const int after = before + d;
        return (isRegular(after) ? 0 : 1) - (isRegular(before) ? 0 : 1);
    }

    void commit(const std::vector<std::size_t>& oldIdx, std::vector<Poly>& newPolys) {
        for (const std::size_t fi : oldIdx) {
            applyFaceEdges(m_faces[fi], -1);
            m_alive[fi] = false;
        }
        for (Poly& p : newPolys) {
            applyFaceEdges(p, +1);
            m_alive.push_back(true);
            m_faces.push_back(std::move(p));
        }
    }

    // Of every fan-split of `poly`, pick the feasible one with the lowest resulting
    // irregular delta (ties -> largest minimum quad area). Returns {} if none feasible.
    // `oldIdx` is the face set being replaced (so edge deltas are scored in context).
    std::vector<Poly> chooseBestSplit(const Poly& poly, const std::vector<std::size_t>& oldIdx,
                                      int& outDelta) const {
        std::vector<Poly> best;
        int bestDelta = std::numeric_limits<int>::max();
        double bestMinArea = -1.0;
        for (std::size_t s = 0; s < poly.size(); ++s) {
            std::vector<Poly> quads = fanSplit(poly, s);
            if (quads.empty()) {
                continue;
            }
            const auto [feasible, delta] = score(oldIdx, quads);
            if (!feasible) {
                continue;
            }
            double minArea = std::numeric_limits<double>::max();
            for (const Poly& q : quads) {
                minArea = std::min(minArea, quadArea(q));
            }
            if (delta < bestDelta || (delta == bestDelta && minArea > bestMinArea)) {
                bestDelta = delta;
                bestMinArea = minArea;
                best = std::move(quads);
            }
        }
        outDelta = bestDelta;
        return best;
    }

    void tryMergeWithNeighbour(std::size_t i) {
        const Poly& fi = m_faces[i];
        std::vector<std::size_t> bestOld;
        std::vector<Poly> bestNew;
        int bestDelta = 1;  // accept only non-regressing merges (delta <= 0)
        for (std::size_t e = 0; e < fi.size(); ++e) {
            const std::size_t a = fi[e];
            const std::size_t b = fi[(e + 1) % fi.size()];
            const auto it = m_dirToFace.find(directedKey(b, a));
            if (it == m_dirToFace.end()) {
                continue;
            }
            const std::size_t j = it->second;
            if (j == i || !m_alive[j] || !isMergeCandidate(j)) {
                continue;
            }
            const Poly merged = mergePolys(fi, m_faces[j], a, b);
            if (merged.empty() || (merged.size() % 2) != 0) {
                continue;
            }
            int delta = 0;
            std::vector<Poly> quads = chooseBestSplit(merged, {i, j}, delta);
            if (!quads.empty() && delta <= bestDelta) {
                bestDelta = delta;
                bestOld = {i, j};
                bestNew = std::move(quads);
            }
        }
        if (!bestNew.empty() && bestDelta <= 0) {
            commit(bestOld, bestNew);
        }
    }

    void trySplitEven(std::size_t i) {
        int delta = 0;
        std::vector<Poly> quads = chooseBestSplit(m_faces[i], {i}, delta);
        if (!quads.empty() && delta <= 0) {
            std::vector<std::size_t> old = {i};
            commit(old, quads);
        }
    }

    void compact() {
        std::vector<Poly> kept;
        kept.reserve(m_faces.size());
        for (std::size_t i = 0; i < m_faces.size(); ++i) {
            if (m_alive[i]) {
                kept.push_back(std::move(m_faces[i]));
            }
        }
        m_faces = std::move(kept);
    }

    const std::vector<Vec3>& m_verts;
    std::vector<Poly>& m_faces;
    std::size_t m_originalCount = 0;
    std::vector<bool> m_alive;
    std::vector<int> m_deg;
    std::unordered_map<std::uint64_t, int> m_edgeCount;
    std::unordered_map<std::uint64_t, std::size_t> m_dirToFace;
};

}  // namespace

// Milestone 2 entry point: trace the seamless UV's integer isolines into an
// oriented quad mesh. Returns empty for an invalid/empty UV.
// Fraction of undirected edges that are on a boundary (incident to a single
// triangle). ~0 for a closed surface (or a closed surface with a few numerical
// cracks); large for a genuine open disk, whose whole perimeter is boundary.
double boundaryEdgeFraction(const std::vector<std::array<Index, 3>>& triangles) {
    if (triangles.empty()) {
        return 1.0;
    }
    std::unordered_map<std::uint64_t, int> edgeCount;
    edgeCount.reserve(triangles.size() * 3);
    const auto key = [](Index a, Index b) {
        const std::uint64_t lo = static_cast<std::uint64_t>(std::min(a, b));
        const std::uint64_t hi = static_cast<std::uint64_t>(std::max(a, b));
        return (hi << 32) | lo;
    };
    for (const auto& t : triangles) {
        ++edgeCount[key(t[0], t[1])];
        ++edgeCount[key(t[1], t[2])];
        ++edgeCount[key(t[2], t[0])];
    }
    std::size_t boundary = 0;
    for (const auto& [e, count] : edgeCount) {
        (void)e;
        if (count == 1) {
            ++boundary;
        }
    }
    return static_cast<double>(boundary) / static_cast<double>(edgeCount.size());
}

IsolineQuadMesh extractIsolineQuads(const Mesh& /*mesh*/, const SeamlessUv& uv,
                                    int holeFillMaxBoundary) {
    if (!uv.valid || uv.triangles.empty()) {
        return IsolineQuadMesh{};
    }

    std::vector<DVec3> vertices;
    vertices.reserve(uv.vertices.size());
    for (const Vec3& p : uv.vertices) {
        vertices.push_back(
            {static_cast<double>(p.x), static_cast<double>(p.y), static_cast<double>(p.z)});
    }
    std::vector<std::array<std::size_t, 3>> triangles;
    triangles.reserve(uv.triangles.size());
    for (const auto& t : uv.triangles) {
        triangles.push_back({static_cast<std::size_t>(t[0]), static_cast<std::size_t>(t[1]),
                             static_cast<std::size_t>(t[2])});
    }
    std::vector<std::array<DVec2, 3>> triangleUvs;
    triangleUvs.reserve(uv.triangleUv.size());
    for (const auto& t : uv.triangleUv) {
        triangleUvs.push_back({DVec2{static_cast<double>(t[0].x), static_cast<double>(t[0].y)},
                               DVec2{static_cast<double>(t[1].x), static_cast<double>(t[1].y)},
                               DVec2{static_cast<double>(t[2].x), static_cast<double>(t[2].y)}});
    }

    // Boundary-aware gate: the closed-surface graph cleanup (simplifyGraph / fixHoles /
    // collapse*) merges the raw isoline oversampling into clean quad cells and is REQUIRED
    // on a closed (or nearly-closed) surface — a sphere goes from 357 non-quad n-gons to
    // 1.1% irregular with it on — but CORRUPTS a genuine open disk (it fills the perimeter
    // as a hole and deletes real boundary corners). A binary closed test is too brittle:
    // the harness re-remeshes and leaves a few numerical crack edges, so a genuinely closed
    // model (e.g. spot) reads as "open" and gets the wrong branch. Decide on the boundary
    // FRACTION instead: ~0 for a closed/cracked surface, large (whole perimeter) for a real
    // open disk. Below 10% we treat the surface as closed and run the cleanup.
    const double boundaryFrac = boundaryEdgeFraction(uv.triangles);
    const bool closed = boundaryFrac < 0.10;

    // M3 open-surface cleanup — now the DEFAULT (opt out with CYBER_QC_NO_OPEN_CLEANUP).
    //
    // Running the cleanup on an OPEN surface is a large win: `fixHoles` fills the under-traced
    // gaps, without which every cell traces as an n-gon that cap elimination can only fan-split.
    // Measured on an open paraboloid at request 900, pure-quads: 92 quads / median 27° WITHOUT it,
    // versus ~1744 uniform quads / median 78° / edge CV 0.27 with it.
    //
    // What kept this opt-in was an edge-CV blowup (0.31 -> 1.70), and the recorded fix — a
    // turn-angle guard on simplifyGraph — was measured a no-op. Bisection found the real cause:
    // `simplifyGraph` itself dissolves every valence-2 node, which on an open surface merges
    // legitimately-valence-2 isoline samples into long uneven quads. The graph-building block above
    // now skips simplifyGraph (and the collapse steps that re-trigger it) on OPEN islands, keeping
    // only fixHoles, which is what removed the blowup. Closed islands are byte-identical.
    // `setPreserveInputBoundary(!closed)` already keeps fixHoles from filling the surface's own
    // rim. Open-surface cleanup now runs by DEFAULT (opt out with CYBER_QC_NO_OPEN_CLEANUP).
    // Without it an open island under-traces catastrophically at low density -- an open paraboloid
    // at request 900 gives 92 faces at median 27 degrees -- because `fixHoles` never runs to fill
    // the under-traced gaps. With it (and the simplifyGraph skip above, which is what made it safe)
    // the same request gives ~1744 uniform quads at median 78, edge CV 0.27. Closed islands are
    // unaffected either way (they already ran the cleanup and are byte-identical).
    const bool openCleanup = std::getenv("CYBER_QC_NO_OPEN_CLEANUP") == nullptr;
    IsolineExtractor extractor(std::move(vertices), std::move(triangles), std::move(triangleUvs));
    extractor.setRunClosedSurfaceCleanup(closed || openCleanup);
    extractor.setPreserveInputBoundary(!closed);
    extractor.setHoleFillMaxBoundary(holeFillMaxBoundary);
    extractor.extract();
    if (std::getenv("CYBER_QC_DEBUG") != nullptr) {
        std::map<std::size_t, std::size_t> hist;
        for (const auto& f : extractor.remeshedQuads()) {
            ++hist[f.size()];
        }
        std::string h;
        for (const auto& [sz, cnt] : hist) {
            h += " " + std::to_string(sz) + ":" + std::to_string(cnt);
        }
        std::fprintf(
            stderr,
            "[qc] extract closed=%d bndFrac=%.3f uvTris=%zu -> quads=%zu verts=%zu hist{%s}\n",
            static_cast<int>(closed), boundaryFrac, uv.triangles.size(),
            extractor.remeshedQuads().size(), extractor.remeshedVertices().size(), h.c_str());
    }

    IsolineQuadMesh out;
    out.vertices.reserve(extractor.remeshedVertices().size());
    for (const DVec3& v : extractor.remeshedVertices()) {
        out.vertices.push_back(
            {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)});
    }
    out.quads = extractor.remeshedQuads();
    return out;
}

// Cap elimination (see QuadCapCleaner): convert the tracer's non-quad "cap" faces into
// quads so the pipeline's Catmull-Clark pure-quad subdivision does not turn each into a
// valence-3/5 fan-centre irregular. Watertight- and irregular-count-non-regressing.
void eliminateNonQuadCaps(std::vector<Vec3>& vertices,
                          std::vector<std::vector<std::size_t>>& faces) {
    QuadCapCleaner(vertices, faces).run();
}

namespace {

// Total surface area (faces fan-triangulated). Serves the scaling calibration and the
// extraction area-coverage gate.
double meshSurfaceArea(const Mesh& mesh) {
    std::vector<Vec3> pos;
    std::vector<std::vector<Index>> faces;
    mesh.toIndexed(pos, faces);
    double area = 0.0;
    for (const auto& fc : faces) {
        for (std::size_t k = 1; k + 1 < fc.size(); ++k) {
            area += 0.5 * static_cast<double>(
                              length(cross(pos[fc[k]] - pos[fc[0]], pos[fc[k + 1]] - pos[fc[0]])));
        }
    }
    return area;
}

// Quad count implied by a target edge length: surface area / edge^2. Used to
// calibrate the harness scaling so the extracted count tracks the request.
double meshTargetQuads(const Mesh& mesh, float targetEdgeLength) {
    if (targetEdgeLength <= 0.0f) {
        return 0.0;
    }
    const double cell =
        static_cast<double>(targetEdgeLength) * static_cast<double>(targetEdgeLength);
    return meshSurfaceArea(mesh) / cell;
}

// Whether the vendored seamless solver (in-process Geogram quad_cover, or the
// CYBER_QUADCOVER_CLI harness subprocess) is configured. computeSeamlessUv may route
// to it, and there `scaling` is the harness -s with different semantics than the
// native spacing multiplier — so the calibration probe below only applies when the
// native solver is the only one available.
bool vendoredSeamlessAvailable() {
#ifdef CYBER_HAVE_QUADCOVER
    return true;
#else
    const char* cli = std::getenv("CYBER_QUADCOVER_CLI");
    return cli != nullptr && cli[0] != '\0';
#endif
}

// Valence-3/5 dipole cancellation on the extracted quad mesh — the integer
// extractor's FixValence (quadMeshValenceCleanup, quad_extract.cpp) wired into the
// quad-cover path. Flow loops break at irregular vertices, so cancelling 3/5 dipoles
// directly lengthens quad flow loops; the operator is strictly count-monotone (the
// irregular count never rises). Only the pure-quad subset is edited: every non-quad
// face is frozen (its vertices pinned, its edges reserved) so the seam between the
// two subsets stays manifold. Vertices on crease lines — any edge whose face-pair
// dihedral exceeds `featureDegrees` — are pinned too: a rotation deletes the shared
// edge, and deleting a crease-tracing edge would rotate quads off the feature line
// (feature recall is gated). Returns the number of applied operations; on change,
// `faces` is rewritten and unreferenced (doublet-freed) vertices are compacted out.
std::size_t cancelValenceDipoles(std::vector<Vec3>& vertices,
                                 std::vector<std::vector<std::size_t>>& faces,
                                 float featureDegrees) {
    // Partition into the editable pure-quad subset and the frozen remainder
    // (non-destructive: on a no-op `faces` is returned exactly as given).
    std::vector<std::array<int, 4>> quads;
    std::vector<char> isQuad(faces.size(), 0);
    std::vector<char> pinned(vertices.size(), 0);
    std::set<std::pair<int, int>> reservedEdges;
    for (std::size_t i = 0; i < faces.size(); ++i) {
        const auto& f = faces[i];
        bool pureQuad = f.size() == 4;
        for (std::size_t a = 0; pureQuad && a < 4; ++a) {
            for (std::size_t b = a + 1; b < 4; ++b) {
                if (f[a] == f[b]) {
                    pureQuad = false;
                    break;
                }
            }
        }
        if (pureQuad) {
            isQuad[i] = 1;
            quads.push_back({static_cast<int>(f[0]), static_cast<int>(f[1]), static_cast<int>(f[2]),
                             static_cast<int>(f[3])});
            continue;
        }
        for (std::size_t k = 0; k < f.size(); ++k) {
            const int a = static_cast<int>(f[k]);
            const int b = static_cast<int>(f[(k + 1) % f.size()]);
            pinned[static_cast<std::size_t>(a)] = 1;
            if (a != b) {
                reservedEdges.insert(std::minmax(a, b));
            }
        }
    }
    if (quads.empty()) {
        return 0;
    }

    // Crease pinning by face-pair dihedral (Newell normals over ALL faces, so a
    // crease between a quad and a frozen cap still pins the quad's vertices).
    const auto faceNormal = [&](const std::vector<std::size_t>& f) {
        Vec3 n{0.0f, 0.0f, 0.0f};
        for (std::size_t k = 0; k < f.size(); ++k) {
            const Vec3& p = vertices[f[k]];
            const Vec3& q = vertices[f[(k + 1) % f.size()]];
            n.x += (p.y - q.y) * (p.z + q.z);
            n.y += (p.z - q.z) * (p.x + q.x);
            n.z += (p.x - q.x) * (p.y + q.y);
        }
        const float len = length(n);
        return len > 0.0f ? n * (1.0f / len) : n;
    };
    const float cosThreshold = std::cos(featureDegrees * 3.14159265358979323846f / 180.0f);
    std::map<std::pair<int, int>, std::pair<int, Vec3>> edgeFaces;  // count + first normal
    const auto scanFace = [&](const std::vector<std::size_t>& f) {
        if (f.size() < 3) {
            return;
        }
        const Vec3 n = faceNormal(f);
        for (std::size_t k = 0; k < f.size(); ++k) {
            const int a = static_cast<int>(f[k]);
            const int b = static_cast<int>(f[(k + 1) % f.size()]);
            if (a == b) {
                continue;
            }
            auto [it, inserted] = edgeFaces.try_emplace(std::minmax(a, b), 1, n);
            if (!inserted) {
                ++it->second.first;
                if (it->second.first == 2 && dot(it->second.second, n) < cosThreshold) {
                    pinned[static_cast<std::size_t>(a)] = 1;
                    pinned[static_cast<std::size_t>(b)] = 1;
                }
            }
        }
    };
    for (const auto& f : faces) {
        scanFace(f);
    }

    const std::size_t ops =
        quadMeshValenceCleanup(quads, static_cast<int>(vertices.size()), pinned, reservedEdges);
    if (ops == 0) {
        return 0;  // `faces` untouched: the kill-switch A/B stays byte-identical
    }
    // Write back: the cleaned quads, then the frozen remainder in original order.
    std::vector<std::vector<std::size_t>> merged;
    merged.reserve(quads.size() + faces.size());
    for (const auto& q : quads) {
        merged.push_back({static_cast<std::size_t>(q[0]), static_cast<std::size_t>(q[1]),
                          static_cast<std::size_t>(q[2]), static_cast<std::size_t>(q[3])});
    }
    for (std::size_t i = 0; i < faces.size(); ++i) {
        if (isQuad[i] == 0) {
            merged.push_back(std::move(faces[i]));
        }
    }
    faces.swap(merged);
    // Compact out vertices freed by doublet dissolution.
    std::vector<int> remap(vertices.size(), -1);
    for (const auto& f : faces) {
        for (const std::size_t v : f) {
            remap[v] = 0;
        }
    }
    std::size_t top = 0;
    for (std::size_t v = 0; v < vertices.size(); ++v) {
        if (remap[v] == 0) {
            remap[v] = static_cast<int>(top);
            vertices[top++] = vertices[v];
        }
    }
    vertices.resize(top);
    for (auto& f : faces) {
        for (std::size_t& v : f) {
            v = static_cast<std::size_t>(remap[v]);
        }
    }
    return ops;
}

// IQuadrangulator implementation (Milestone 3). Runs the full QuadCover pipeline:
// seamless integer-grid UV (M1, out-of-process via CYBER_QUADCOVER_CLI) -> isoline
// trace + boundary-aware cleanup (M2) -> replace `mesh` in place with the quad mesh.
// Every failure path leaves `mesh` untouched, so the pipeline degrades cleanly (e.g.
// when the harness binary is absent, computeSeamlessUv returns invalid and we report
// a reason without corrupting the input triangle island).
class QuadCoverQuadrangulator final : public IQuadrangulator {
public:
    QuadCoverQuadrangulator(int fieldIterations, float adaptivity, int holeFillMaxBoundary,
                            float featureDegrees)
        : m_fieldIterations(fieldIterations),
          m_adaptivity(adaptivity),
          m_holeFillMaxBoundary(holeFillMaxBoundary),
          m_featureDegrees(featureDegrees) {}

    Outcome quadrangulate(Mesh& mesh, float targetEdgeLength, ProgressSink* progress,
                          const CancelToken* cancel) override {
        if (cancel != nullptr && cancel->isCancelled()) {
            return {.success = false, .cancelled = true, .failureReason = {}};
        }
        if (progress != nullptr) {
            progress->report(0.0f, "quadrangulate (quad-cover: seamless UV)");
        }
        (void)m_fieldIterations;

        // Target quad count implied by the requested edge length. The harness scaling only
        // loosely maps to the extracted count, so hit the target with a short closed loop:
        // extract, measure, correct the scaling by sqrt(actual/target) (quads ~ 1/scaling^2),
        // and re-solve once if we are off by more than 25%.
        const double targetQuads = meshTargetQuads(mesh, targetEdgeLength);
        IsolineQuadMesh out;
        float scaling = 0.5f;
        // The native solve's isotropic remesh + cross field + cut setup depend only on the
        // mesh / edge length / adaptivity / feature threshold — never on `scaling` — so the
        // context computes them once and each attempt re-runs only the (spacing-dependent)
        // parameterization + extraction.
        NativeSolveContext nativeCtx;
        nativeCtx.guidance = m_guidance;  // non-null forces the native route (see the header)
        // Probe-predicted initial scaling (native route only). The hardcoded 0.5 start
        // overshoots the extracted count 1.5-3x on every corpus mesh, so the loop below
        // always pays a second full solve. The relaxed Poisson phase already fixes the UV
        // cell area up to a per-mesh-stable factor, so one relaxed-only probe at spacing ==
        // targetEdgeLength predicts the mesh-specific scaling:
        //   quads(s) ~ eta * cellsRelaxed(1)/s^2  =>  s0 = sqrt(eta * cellsRelaxed(1)/target)
        // eta folds two corpus-measured, per-mesh-stable (<2% drift across spacings) ratios:
        // extraction efficiency quads/cellsFinal (0.95-1.00 on all 7 corpus meshes) and the
        // ARAP-polish area growth cellsFinal/cellsRelaxed (0.85-1.04 on 5/7 including every
        // expensive mesh; sphere/torus are ~1.65 outliers that fall back to the correction
        // attempt, exactly like today's constant start). The corpus-calibrated default is
        // 1.0: the product sits at 0.92-1.04 on the coherent meshes, and on CAD-flat parts
        // the relaxed cells EQUAL area/spacing^2, so eta=1 puts the grid cell exactly at
        // targetEdgeLength and preserves the perfect box grid (8 corner cones, 0-deg angle
        // dev — eta=0.95 measurably broke it). Note eta is calibrated for THIS measure
        // (relaxed |UV triangle area| sum); the historical 0.62 figure was quads per
        // full-solve bbox cell and does not apply. The 2-attempt loop stays unchanged as
        // the safety net (a probe miss costs one correction solve). Kill switch:
        // CYBER_QC_NO_PROBE restores the 0.5 start; CYBER_QC_EXTRACT_EFF overrides eta.
        // Skipped when the vendored solver is configured (there `scaling` is the harness
        // -s) or when a scaling/spacing experiment env pins the solve.
        const bool probeEnabled = std::getenv("CYBER_QC_NO_PROBE") == nullptr &&
                                  std::getenv("CYBER_QC_NO_NATIVE") == nullptr &&
                                  std::getenv("CYBER_QC_SCALING") == nullptr &&
                                  std::getenv("CYBER_QC_SPACING_MUL") == nullptr &&
                                  (!vendoredSeamlessAvailable() || m_guidance != nullptr) &&
                                  targetQuads > 0.0;
        if (probeEnabled) {
            const double cells = nativeRelaxedCellArea(mesh, targetEdgeLength, m_adaptivity,
                                                       m_featureDegrees, cancel, nativeCtx);
            if (cells > 0.0) {
                const char* effEnv = std::getenv("CYBER_QC_EXTRACT_EFF");
                const double kExtractEff = effEnv != nullptr ? std::atof(effEnv) : 1.0;
                scaling = std::clamp(
                    static_cast<float>(std::sqrt(kExtractEff * cells / targetQuads)), 0.2f, 1.5f);
                if (std::getenv("CYBER_QC_DEBUG") != nullptr) {
                    std::fprintf(stderr, "[qc] probe cells=%.1f eff=%.2f target=%.1f -> s0=%.3f\n",
                                 cells, kExtractEff, targetQuads, static_cast<double>(scaling));
                }
            }
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            const SeamlessUv uv = computeSeamlessUv(mesh, targetEdgeLength, scaling, m_adaptivity,
                                                    cancel, m_featureDegrees, &nativeCtx);
            if (!uv.valid) {
                return {.success = false,
                        .cancelled = false,
                        .failureReason =
                            "quad-cover seamless UV unavailable "
                            "(set CYBER_QUADCOVER_CLI to the autoremesher_cli build)"};
            }
            if (cancel != nullptr && cancel->isCancelled()) {
                return {.success = false, .cancelled = true, .failureReason = {}};
            }
            // Debug dump: the per-attempt seamless UV (positions + per-corner UVs), for
            // offline seam-integrality checks (this is how the fractional crease offsets
            // were measured). CYBER_QC_DUMP_UV=<prefix>; writes <prefix><attempt>.txt.
            if (const char* uvPath = std::getenv("CYBER_QC_DUMP_UV"); uvPath != nullptr) {
                const std::string path = std::string(uvPath) + std::to_string(attempt) + ".txt";
                if (std::FILE* f = std::fopen(path.c_str(), "wb"); f != nullptr) {
                    std::fprintf(f, "V %zu\n", uv.vertices.size());
                    for (const Vec3& p : uv.vertices) {
                        std::fprintf(f, "%.9g %.9g %.9g\n", static_cast<double>(p.x),
                                     static_cast<double>(p.y), static_cast<double>(p.z));
                    }
                    std::fprintf(f, "T %zu\n", uv.triangles.size());
                    for (std::size_t ti = 0; ti < uv.triangles.size(); ++ti) {
                        const auto& t = uv.triangles[ti];
                        const auto& tuv = uv.triangleUv[ti];
                        std::fprintf(f, "%u %u %u %.9g %.9g %.9g %.9g %.9g %.9g\n",
                                     static_cast<unsigned>(t[0]), static_cast<unsigned>(t[1]),
                                     static_cast<unsigned>(t[2]), static_cast<double>(tuv[0].x),
                                     static_cast<double>(tuv[0].y), static_cast<double>(tuv[1].x),
                                     static_cast<double>(tuv[1].y), static_cast<double>(tuv[2].x),
                                     static_cast<double>(tuv[2].y));
                    }
                    std::fclose(f);
                }
            }
            out = extractIsolineQuads(mesh, uv, m_holeFillMaxBoundary);
            const double got = static_cast<double>(out.quads.size());
            if (std::getenv("CYBER_QC_DEBUG") != nullptr) {
                std::fprintf(stderr,
                             "[qc] calibrate attempt=%d scaling=%.4f got=%.0f target=%.1f\n",
                             attempt, static_cast<double>(scaling), got, targetQuads);
            }
            if (std::getenv("CYBER_QC_SCALING") != nullptr || targetQuads <= 0.0 || got <= 0.0) {
                break;  // fixed scaling (experiment) or nothing to calibrate against
            }
            const double ratio = got / targetQuads;
            // Tiny targets (the --pure-quads quarter-density bases) cannot afford the
            // regular 25% band: a curved crease needs every cell it can get to stay
            // within the feature tolerance after subdivision (a 30%-undershot 100-quad
            // cylinder base leaves its rims 8 segments wide — recall-fatal), so accept
            // only within 12% there. Larger solves keep the historical band unchanged.
            const bool small = targetQuads < 200.0;
            if (ratio > (small ? 0.88 : 0.75) && ratio < (small ? 1.14 : 1.33)) {
                break;  // within band -> accept
            }
            scaling = std::clamp(scaling * static_cast<float>(std::sqrt(ratio)), 0.2f, 1.5f);
        }
        if (m_guidance != nullptr && !nativeCtx.guidanceHonored) {
            // The native solve declined and the vendored Geogram path ran
            // instead. It cannot see either input, so say which and why rather
            // than shipping a silently unguided island.
            m_unhonored.push_back(
                "flow guides and painted density: the native seamless solve declined this "
                "island and the vendored Geogram quad_cover solve has no guide/density hook");
        }
        if (progress != nullptr) {
            progress->report(0.5f, "quadrangulate (quad-cover: isoline extract)");
        }
        if (out.quads.empty()) {
            return {.success = false,
                    .cancelled = false,
                    .failureReason = "quad-cover isoline extraction produced no faces"};
        }
        // Convert non-quad caps to quads before the pipeline's pure-quad subdivision so
        // they do not become fan-centre irregulars. Runs AFTER the count calibration so it
        // never perturbs the scaling feedback. Opt out via CYBER_QC_NO_CAPFIX for A/B.
        if (std::getenv("CYBER_QC_NO_CAPFIX") == nullptr) {
            eliminateNonQuadCaps(out.vertices, out.quads);
            if (std::getenv("CYBER_QC_DEBUG") != nullptr) {
                std::map<std::size_t, std::size_t> hist;
                for (const auto& f : out.quads) {
                    ++hist[f.size()];
                }
                std::string h;
                for (const auto& [sz, cnt] : hist) {
                    h += " " + std::to_string(sz) + ":" + std::to_string(cnt);
                }
                std::fprintf(stderr, "[qc] capfix -> quads=%zu hist{%s}\n", out.quads.size(),
                             h.c_str());
            }
        }
        // Cancel valence-3/5 dipoles on the extracted quads (see cancelValenceDipoles):
        // strictly count-monotone, so it can only remove irregular vertices — the lever
        // for longer quad flow loops. Runs AFTER the count calibration and the cap fix
        // so it never perturbs the scaling feedback and sees the final quad set. Crease
        // detection reuses the solve's feature threshold (CYBER_QC_FEATURE_DEG override,
        // as in computeSeamlessUv). Kill switch CYBER_QC_NO_DIPOLE for A/B.
        if (std::getenv("CYBER_QC_NO_DIPOLE") == nullptr) {
            const char* featEnv = std::getenv("CYBER_QC_FEATURE_DEG");
            const float featureDeg =
                featEnv != nullptr ? static_cast<float>(std::atof(featEnv)) : m_featureDegrees;
            const std::size_t ops = cancelValenceDipoles(out.vertices, out.quads, featureDeg);
            if (std::getenv("CYBER_QC_DEBUG") != nullptr) {
                std::fprintf(stderr, "[qc] dipole cleanup ops=%zu -> faces=%zu verts=%zu\n", ops,
                             out.quads.size(), out.vertices.size());
            }
        }

        std::vector<std::vector<Index>> faces;
        faces.reserve(out.quads.size());
        for (const auto& q : out.quads) {
            std::vector<Index> f;
            f.reserve(q.size());
            for (const std::size_t v : q) {
                f.push_back(static_cast<Index>(v));
            }
            faces.push_back(std::move(f));
        }
        Mesh quads = Mesh::fromIndexed(out.vertices, faces);
        if (quads.faceCount() == 0) {
            return {.success = false,
                    .cancelled = false,
                    .failureReason = "quad-cover extraction yielded a degenerate mesh"};
        }
        // Area-coverage gate (remeshing-pipeline spec: no silent geometry loss). When
        // extraction drops or collapses a genuine patch (a disconnected feature-bounded
        // island, an untraced region), the surviving quads can still form a "clean" mesh
        // — box_sharp shipped a flawless zero-singularity grid missing BOTH cube caps.
        // Compare output surface area against the input island: on a CLOSED island a
        // healthy extraction covers nearly all of it (coarse-density corner rounding
        // costs a few percent at most), so a shortfall beyond 15% means real geometry
        // went missing. Decline honestly; the pipeline recovers via the fallback
        // quadrangulator or reports a per-island failure (exit 5), never a "successful"
        // wrong answer. OPEN islands are exempt: their traces legitimately stop short of
        // the boundary rim (the accepted open-surface undershoot contract — see the
        // open-surface cleanup suite).
        std::size_t nBoundary = 0;
        std::size_t nAliveEdges = 0;
        for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
            const EdgeId e{ei};
            if (!mesh.isAlive(e)) {
                continue;
            }
            ++nAliveEdges;
            if (mesh.edgeFaceCount(e) == 1) {
                ++nBoundary;
            }
        }
        const bool closedInput = nAliveEdges > 0 && static_cast<double>(nBoundary) <
                                                        0.01 * static_cast<double>(nAliveEdges);
        const double inputArea = meshSurfaceArea(mesh);
        const double outputArea = meshSurfaceArea(quads);
        if (closedInput && inputArea > 0.0 && outputArea < 0.85 * inputArea) {
            char reason[128];
            std::snprintf(reason, sizeof(reason),
                          "quad-cover extraction dropped geometry (area coverage %.0f%%)",
                          100.0 * outputArea / inputArea);
            return {.success = false, .cancelled = false, .failureReason = reason};
        }
        mesh = std::move(quads);
        if (progress != nullptr) {
            progress->report(1.0f, "quadrangulate");
        }
        return {.success = true, .cancelled = false, .failureReason = {}};
    }

    // Guides reach the native cross field; painted density reaches both the
    // native solve's isotropic pre-remesh AND its per-face grid spacing. Both
    // are native-only, so accepting here also forces the native route.
    bool acceptGuidance(const GuidanceField& field, std::string& reason) override {
        (void)reason;
        m_guidance = &field;
        m_unhonored.clear();
        return true;
    }

    [[nodiscard]] std::vector<std::string> unhonoredGuidance() const override {
        return m_unhonored;
    }

    [[nodiscard]] std::string name() const override { return "quad-cover"; }

private:
    int m_fieldIterations;
    float m_adaptivity;
    int m_holeFillMaxBoundary;
    float m_featureDegrees = 40.0f;
    const GuidanceField* m_guidance = nullptr;
    std::vector<std::string> m_unhonored;
};

}  // namespace

std::unique_ptr<IQuadrangulator> makeQuadCoverQuadrangulator(int fieldIterations, float adaptivity,
                                                             int holeFillMaxBoundary,
                                                             float featureDegrees) {
    return std::make_unique<QuadCoverQuadrangulator>(fieldIterations, adaptivity,
                                                     holeFillMaxBoundary, featureDegrees);
}

bool quadCoverAvailable() {
    // M4: the native QuadCover-style seamless solver (computeSeamlessUvNative) is compiled
    // into cyber_quadrangulate unconditionally, so a seamless-UV solver is ALWAYS available
    // — quad-cover no longer needs Geogram (CYBER_HAVE_QUADCOVER) or the CYBER_QUADCOVER_CLI
    // harness. The vendored / subprocess paths remain as faster/reference fallbacks.
    return true;
}

}  // namespace cyber::remesh
