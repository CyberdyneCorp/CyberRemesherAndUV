#include "cyber/quadrangulate/position_field.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <string>
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

// An arbitrary unit tangent of the plane normal to `n`.
Vec3 anyTangent(Vec3 n) {
    const Vec3 seed = std::fabs(n.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    return projectUnit(seed, n);
}

// A generic field graph: positions, normals, adjacency and per-node fields.
// Used both for the mesh (finest level) and every coarsened level of the
// multiresolution hierarchy, so the same smoothing code runs at all levels.
struct FieldGraph {
    std::vector<Vec3> pos;
    std::vector<Vec3> normal;
    std::vector<std::vector<int>> nbr;
    std::vector<bool> constrained;  // feature/boundary
    std::vector<Vec3> constraintDir;
    std::vector<Vec3> q;       // orientation field
    std::vector<Vec3> o;       // position field
    std::vector<float> scale;  // per-node lattice-spacing multiplier (1 = uniform)
    [[nodiscard]] std::size_t size() const { return pos.size(); }
};

// Builds the finest-level graph from the mesh; `baseToVertex` maps each graph
// node back to its mesh vertex index.
FieldGraph buildBaseGraph(const Mesh& mesh, std::vector<Index>& baseToVertex) {
    const std::size_t cap = mesh.vertexCapacity();
    std::vector<int> toBase(cap, -1);
    FieldGraph g;
    for (Index i = 0; i < cap; ++i) {
        const VertexId v{i};
        if (!mesh.isAlive(v) || mesh.vertexFaces(v).empty()) {
            continue;
        }
        toBase[i] = static_cast<int>(g.pos.size());
        baseToVertex.push_back(i);
        g.pos.push_back(mesh.position(v));
        Vec3 n{};
        for (const FaceId f : mesh.vertexFaces(v)) {
            n += mesh.faceNormal(f);
        }
        g.normal.push_back(normalized(n));
        g.constrained.push_back(false);
        g.constraintDir.push_back(Vec3{0, 0, 0});
    }
    g.nbr.assign(g.pos.size(), {});
    for (Index i = 0; i < cap; ++i) {
        if (toBase[i] < 0) {
            continue;
        }
        const VertexId v{i};
        const auto base = static_cast<std::size_t>(toBase[i]);
        for (const EdgeId e : mesh.vertexEdges(v)) {
            const auto [a, b] = mesh.edgeVertices(e);
            const VertexId other = (a == v ? b : a);
            if (toBase[other.value] >= 0) {
                g.nbr[base].push_back(toBase[other.value]);
            }
            if (mesh.isFeatureEdge(e) || mesh.isBoundaryEdge(e)) {
                g.constrained[base] = true;
                g.constraintDir[base] =
                    projectUnit(mesh.position(other) - mesh.position(v), g.normal[base]);
            }
        }
    }
    g.q.assign(g.size(), Vec3{1, 0, 0});
    g.o = g.pos;

    // Per-node lattice-spacing multiplier from LOCAL mesh density: the mean
    // incident edge length, normalized by the global mean so a uniform mesh maps
    // to ~1 everywhere (extractor behaviour then matches the fixed-spacing path),
    // while an adaptive mesh — fine near curvature, coarse on flats — carries
    // that density into the lattice, so cells track the local edge length instead
    // of over-merging the coarse regions. Smoothed so remesh noise doesn't
    // fragment the field.
    std::vector<float> localLen(g.size(), 0.0f);
    for (std::size_t i = 0; i < g.size(); ++i) {
        const auto& nb = g.nbr[i];
        if (nb.empty()) {
            localLen[i] = 1.0f;
            continue;
        }
        float sum = 0.0f;
        for (const int j : nb) {
            sum += length(g.pos[i] - g.pos[static_cast<std::size_t>(j)]);
        }
        localLen[i] = sum / static_cast<float>(nb.size());
    }
    double meanLen = 0.0;
    for (const float l : localLen) {
        meanLen += l;
    }
    meanLen = g.size() ? meanLen / static_cast<double>(g.size()) : 1.0;
    const float invMean = meanLen > 1e-12 ? static_cast<float>(1.0 / meanLen) : 1.0f;
    g.scale.assign(g.size(), 1.0f);
    for (std::size_t i = 0; i < g.size(); ++i) {
        g.scale[i] = localLen[i] * invMean;
    }
    // Heavy Laplacian smoothing: isotropic-remesh edge noise (~±20% even on a
    // "uniform" mesh) would otherwise fragment the lattice; many passes flatten
    // that noise toward 1 while the large-scale adaptive density gradient (smooth
    // over many vertices) survives. Uniform input -> scale ~1, so the extractor
    // matches the fixed-spacing path; adaptive input keeps its gradient.
    for (int pass = 0; pass < 16; ++pass) {
        std::vector<float> next = g.scale;
        for (std::size_t i = 0; i < g.size(); ++i) {
            float sum = g.scale[i];
            for (const int j : g.nbr[i]) {
                sum += g.scale[static_cast<std::size_t>(j)];
            }
            next[i] = sum / static_cast<float>(g.nbr[i].size() + 1);
        }
        g.scale.swap(next);
    }
    return g;
}

// Coarsens a graph by greedy edge matching: adjacent nodes are paired, each
// pair (or lone node) becomes one coarse node. `parent[fine] = coarse index`.
//
// Tube-aware matching: pair each node with its most surface-coherent unmatched
// neighbour (largest normal agreement) and REFUSE to pair across a sharp fold
// (normals more than acos(kMinNormalDot) apart). Plain first-neighbour matching
// bridges thin high-curvature necks — e.g. the two sides of the Stanford-bunny's
// ear — collapsing front and back of the tube into one coarse node whose averaged
// normal points nowhere; the multiresolution orientation smoothing on that node is
// then meaningless and plants extra singularities on the ear. Requiring normal
// coherence keeps every coarse node inside one smooth patch. On a smooth model all
// neighbours agree, so this reduces to ordinary matching and preserves its output.
FieldGraph coarsen(const FieldGraph& fine, std::vector<int>& parent) {
    const int n = static_cast<int>(fine.size());
    std::vector<int> match(fine.size(), -1);
    const char* dotEnv = std::getenv("CYBER_QC_COARSEN_MINDOT");
    const float kMinNormalDot = dotEnv != nullptr ? static_cast<float>(std::atof(dotEnv)) : 0.5f;
    for (int i = 0; i < n; ++i) {
        if (match[static_cast<std::size_t>(i)] >= 0) {
            continue;
        }
        int bestJ = -1;
        float bestDot = kMinNormalDot;  // a match must beat the fold threshold
        for (const int j : fine.nbr[static_cast<std::size_t>(i)]) {
            if (j == i || match[static_cast<std::size_t>(j)] >= 0) {
                continue;
            }
            const float d = dot(fine.normal[static_cast<std::size_t>(i)],
                                fine.normal[static_cast<std::size_t>(j)]);
            if (d > bestDot) {
                bestDot = d;
                bestJ = j;
            }
        }
        if (bestJ >= 0) {
            match[static_cast<std::size_t>(i)] = bestJ;
            match[static_cast<std::size_t>(bestJ)] = i;
        }
    }
    parent.assign(fine.size(), -1);
    int nc = 0;
    for (int i = 0; i < n; ++i) {
        if (parent[static_cast<std::size_t>(i)] >= 0) {
            continue;
        }
        parent[static_cast<std::size_t>(i)] = nc;
        if (match[static_cast<std::size_t>(i)] >= 0) {
            parent[static_cast<std::size_t>(match[static_cast<std::size_t>(i)])] = nc;
        }
        ++nc;
    }

    FieldGraph c;
    const auto ncs = static_cast<std::size_t>(nc);
    c.pos.assign(ncs, Vec3{0, 0, 0});
    c.normal.assign(ncs, Vec3{0, 0, 0});
    c.constrained.assign(ncs, false);
    c.constraintDir.assign(ncs, Vec3{0, 0, 0});
    c.nbr.assign(ncs, {});
    c.scale.assign(ncs, 0.0f);
    std::vector<float> count(ncs, 0.0f);
    for (int i = 0; i < n; ++i) {
        const auto ci = static_cast<std::size_t>(parent[static_cast<std::size_t>(i)]);
        c.pos[ci] += fine.pos[static_cast<std::size_t>(i)];
        c.normal[ci] += fine.normal[static_cast<std::size_t>(i)];
        c.scale[ci] += fine.scale[static_cast<std::size_t>(i)];
        count[ci] += 1.0f;
        if (fine.constrained[static_cast<std::size_t>(i)]) {
            c.constrained[ci] = true;
            c.constraintDir[ci] = fine.constraintDir[static_cast<std::size_t>(i)];
        }
    }
    for (std::size_t ci = 0; ci < ncs; ++ci) {
        if (count[ci] > 0.0f) {
            c.pos[ci] = c.pos[ci] / count[ci];
            c.scale[ci] = c.scale[ci] / count[ci];
        }
        c.normal[ci] = normalized(c.normal[ci]);
        if (c.constrained[ci]) {
            c.constraintDir[ci] = projectUnit(c.constraintDir[ci], c.normal[ci]);
        }
    }
    // Coarse adjacency = resolved fine edges between distinct coarse nodes.
    for (int i = 0; i < n; ++i) {
        const int ci = parent[static_cast<std::size_t>(i)];
        for (const int j : fine.nbr[static_cast<std::size_t>(i)]) {
            const int cj = parent[static_cast<std::size_t>(j)];
            if (ci != cj) {
                c.nbr[static_cast<std::size_t>(ci)].push_back(cj);
            }
        }
    }
    for (auto& list : c.nbr) {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    }
    c.q.assign(ncs, Vec3{1, 0, 0});
    c.o = c.pos;
    return c;
}

// One Jacobi sweep of the 4-RoSy orientation field on a graph.
void smoothOrientation(FieldGraph& g, int iterations) {
    std::vector<Vec3> next(g.size());
    for (int it = 0; it < iterations; ++it) {
        for (std::size_t i = 0; i < g.size(); ++i) {
            if (g.constrained[i]) {
                next[i] = g.constraintDir[i];
                continue;
            }
            Vec3 acc = g.q[i];
            for (const int j : g.nbr[i]) {
                const Vec3 dj = projectUnit(g.q[static_cast<std::size_t>(j)], g.normal[i]);
                acc += matchRoSy(g.q[i], dj, g.normal[i]);
            }
            next[i] = projectUnit(acc, g.normal[i]);
        }
        g.q.swap(next);
    }
}

// One Jacobi sweep of the position field on a graph (position_round_4 anchoring
// plus normal-drift removal, so representatives form crisp on-surface cells).
void smoothPosition(FieldGraph& g, float s, int iterations) {
    std::vector<Vec3> next(g.size());
    for (int it = 0; it < iterations; ++it) {
        for (std::size_t i = 0; i < g.size(); ++i) {
            // Local lattice spacing at this node (s for a uniform mesh; scaled by
            // the density multiplier on an adaptive one).
            const float si = s * (g.scale.empty() ? 1.0f : g.scale[i]);
            const float invS = 1.0f / si;
            const Vec3 qi = g.q[i];
            const Vec3 ti = cross(g.normal[i], qi);
            Vec3 sum = g.o[i];
            float w = 1.0f;
            for (const int jn : g.nbr[i]) {
                const auto j = static_cast<std::size_t>(jn);
                const Vec3 diff = g.o[j] - g.o[i];
                const float a = std::round(dot(diff, qi) * invS);
                const float b = std::round(dot(diff, ti) * invS);
                sum += g.o[j] - (qi * (a * si) + ti * (b * si));
                w += 1.0f;
            }
            sum = sum / w;
            const Vec3 d = g.pos[i] - sum;
            const Vec3 anchored = sum + qi * (std::round(dot(qi, d) * invS) * si) +
                                  ti * (std::round(dot(ti, d) * invS) * si);
            Vec3 rel = anchored - g.pos[i];
            rel = rel - g.normal[i] * dot(g.normal[i], rel);
            next[i] = g.pos[i] + rel;
        }
        g.o.swap(next);
    }
}

// CYBER_QC_MR_DUMP=<prefix>: write each hierarchy level's orientation field after its
// smoothing pass to <prefix>.lvl<k>.txt (one node per line: pos, normal, q). Diagnosis
// instrumentation only; the solve is unchanged.
void dumpLevel(const FieldGraph& g, int lvl) {
    const char* prefix = std::getenv("CYBER_QC_MR_DUMP");
    if (prefix == nullptr) {
        return;
    }
    const std::string path = std::string(prefix) + ".lvl" + std::to_string(lvl) + ".txt";
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < g.size(); ++i) {
        std::fprintf(f, "%g %g %g %g %g %g %g %g %g\n", g.pos[i].x, g.pos[i].y, g.pos[i].z,
                     g.normal[i].x, g.normal[i].y, g.normal[i].z, g.q[i].x, g.q[i].y, g.q[i].z);
    }
    std::fclose(f);
}

// Historical per-node orientation init: the constraint direction where anchored, an
// arbitrary tangent elsewhere.
void initNodeQ(FieldGraph& g, std::size_t i) {
    g.q[i] = g.constrained[i] ? g.constraintDir[i] : anyTangent(g.normal[i]);
}

// Labels the graph's connected components by BFS; returns per-node labels and sets `nComp`.
std::vector<int> labelComponents(const FieldGraph& g, int& nComp) {
    std::vector<int> comp(g.size(), -1);
    std::vector<std::size_t> queue;
    nComp = 0;
    for (std::size_t s = 0; s < g.size(); ++s) {
        if (comp[s] >= 0) {
            continue;
        }
        comp[s] = nComp;
        queue.assign(1, s);
        std::size_t head = 0;
        while (head < queue.size()) {
            const std::size_t i = queue[head++];
            for (const int jn : g.nbr[i]) {
                const auto j = static_cast<std::size_t>(jn);
                if (comp[j] < 0) {
                    comp[j] = nComp;
                    queue.push_back(j);
                }
            }
        }
        ++nComp;
    }
    return comp;
}

// Per-node "a winding invariant exists here" flag for the coherent seed: label the base
// graph's connected components and compute each component's Euler characteristic on the
// mesh (V - E + F over the component's live vertices/edges/faces). chi = 2 is a topological
// sphere — H1 = 0, nothing to trap; chi < 2 carries independent cycles a seed can wind.
std::vector<char> windingEligibility(const Mesh& mesh, const FieldGraph& base,
                                     const std::vector<Index>& baseToVertex) {
    int nComp = 0;
    const std::vector<int> comp = labelComponents(base, nComp);
    std::vector<int> vertexToBase(mesh.vertexCapacity(), -1);
    for (std::size_t bn = 0; bn < baseToVertex.size(); ++bn) {
        vertexToBase[baseToVertex[bn]] = static_cast<int>(bn);
    }
    const auto compOfVertex = [&](Index v) {
        const int bn = vertexToBase[v];
        return bn >= 0 ? comp[static_cast<std::size_t>(bn)] : -1;
    };
    std::vector<long> chi(static_cast<std::size_t>(nComp), 0);
    for (std::size_t bn = 0; bn < base.size(); ++bn) {
        chi[static_cast<std::size_t>(comp[bn])] += 1;  // V
    }
    for (Index ei = 0; ei < mesh.edgeCapacity(); ++ei) {
        const EdgeId e{ei};
        if (!mesh.isAlive(e)) {
            continue;
        }
        const int c = compOfVertex(mesh.edgeVertices(e).first.value);
        if (c >= 0) {
            chi[static_cast<std::size_t>(c)] -= 1;  // E
        }
    }
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f)) {
            continue;
        }
        const std::vector<VertexId> fv = mesh.faceVertices(f);
        const int c = fv.empty() ? -1 : compOfVertex(fv[0].value);
        if (c >= 0) {
            chi[static_cast<std::size_t>(c)] += 1;  // F
        }
    }
    std::vector<char> eligible(base.size(), 0);
    for (std::size_t bn = 0; bn < base.size(); ++bn) {
        eligible[bn] = chi[static_cast<std::size_t>(comp[bn])] < 2 ? 1 : 0;
    }
    return eligible;
}

// Coherent seeding of the UNANCHORED, NON-SIMPLY-CONNECTED components (per `eligible`).
// Per-node independent `anyTangent` seeds land the coarse solve in a random holonomy
// basin; on a closed handle with no constraints (torus) that basin can carry a trapped
// twist winding around a generator that downstream smoothing provably cannot unwind — it
// would have to move cones across the generator — and the wound field extracts as
// sheared, incoherent cells (torus hausdorff +123%). BFS-transporting ONE seed direction
// across the component picks the near-zero-winding basin instead. The rule engages
// exactly where a winding invariant exists:
//  - components WITH constraints keep the per-node init — their gauge is anchored, and
//    measured, a transported global gauge on top of the anchors is worse (nefertiti
//    cyber-pure 204 vs 176 cones);
//  - simply-connected components (Euler characteristic 2) keep it too — with H1 = 0
//    there is no winding to trap, every basin is smoothing-reachable, and measured, the
//    transported hedgehog seed is worse (sphere hausdorff 0.0070 -> 0.0218 at an
//    identical 8-cone census).
void seedCoherentUnanchored(FieldGraph& g, const std::vector<char>& eligible) {
    int nComp = 0;
    const std::vector<int> comp = labelComponents(g, nComp);
    std::vector<char> anchored(static_cast<std::size_t>(nComp), 0);
    for (std::size_t i = 0; i < g.size(); ++i) {
        if (g.constrained[i]) {
            anchored[static_cast<std::size_t>(comp[i])] = 1;
        }
    }
    std::vector<char> seeded(g.size(), 0);
    std::vector<std::size_t> queue;
    for (std::size_t s = 0; s < g.size(); ++s) {
        if (seeded[s] != 0) {
            continue;
        }
        if (anchored[static_cast<std::size_t>(comp[s])] != 0 ||
            (s < eligible.size() && eligible[s] == 0)) {
            initNodeQ(g, s);
            seeded[s] = 1;
            continue;
        }
        // Unanchored, winding-capable component: one arbitrary source, transported
        // everywhere.
        g.q[s] = anyTangent(g.normal[s]);
        seeded[s] = 1;
        queue.assign(1, s);
        std::size_t head = 0;
        while (head < queue.size()) {
            const std::size_t i = queue[head++];
            for (const int jn : g.nbr[i]) {
                const auto j = static_cast<std::size_t>(jn);
                if (seeded[j] != 0) {
                    continue;
                }
                seeded[j] = 1;
                const Vec3 t = g.q[i] - g.normal[j] * dot(g.normal[j], g.q[i]);
                g.q[j] = lengthSquared(t) > 1e-12f ? normalized(t) : anyTangent(g.normal[j]);
                queue.push_back(j);
            }
        }
    }
}

}  // namespace

PositionField computePositionField(const Mesh& mesh, float spacing, int iterations,
                                   bool coherentSeedUnanchored) {
    std::vector<Index> baseToVertex;
    FieldGraph base = buildBaseGraph(mesh, baseToVertex);

    // Build the multiresolution hierarchy (finest first) by greedy coarsening.
    const char* floorEnv = std::getenv("CYBER_QC_MR_FLOOR");
    const std::size_t floorN =
        floorEnv != nullptr ? static_cast<std::size_t>(std::atoi(floorEnv)) : 16;
    std::vector<FieldGraph> levels;
    std::vector<std::vector<int>> parents;
    levels.push_back(std::move(base));
    while (levels.back().size() > floorN) {
        std::vector<int> parent;
        FieldGraph c = coarsen(levels.back(), parent);
        if (c.size() >= levels.back().size()) {
            break;  // no progress (e.g. disconnected singletons)
        }
        parents.push_back(std::move(parent));
        levels.push_back(std::move(c));
    }
    if (std::getenv("CYBER_QC_MR_DUMP") != nullptr) {
        for (std::size_t l = 0; l < levels.size(); ++l) {
            const FieldGraph& g = levels[l];
            double dotSum = 0.0;
            std::size_t dotCount = 0;
            for (std::size_t i = 0; i < g.size(); ++i) {
                for (const int j : g.nbr[i]) {
                    dotSum += dot(g.normal[i], g.normal[static_cast<std::size_t>(j)]);
                    ++dotCount;
                }
            }
            std::fprintf(stderr, "[mr] lvl%zu nodes=%zu meanNbrNormalDot=%.3f\n", l, g.size(),
                         dotCount ? dotSum / static_cast<double>(dotCount) : 1.0);
        }
    }

    // Winding eligibility for the coherent seed (see seedCoherentUnanchored): chi < 2
    // flags computed on the base mesh components and pushed down the hierarchy through
    // `parents` (pairing never crosses components, so a coarse node inherits its
    // children's component).
    std::vector<char> eligible;
    if (coherentSeedUnanchored) {
        eligible = windingEligibility(mesh, levels.front(), baseToVertex);
        for (std::size_t lvl = 1; lvl < levels.size(); ++lvl) {
            std::vector<char> next(levels[lvl].size(), 0);
            const std::vector<int>& parent = parents[lvl - 1];
            for (std::size_t i = 0; i < parent.size(); ++i) {
                next[static_cast<std::size_t>(parent[i])] = eligible[i];
            }
            eligible.swap(next);
        }
    }

    // Orientation: seed + smooth the coarsest level, then prolong + smooth each
    // finer level. The coarse solve fixes the global cross-field topology
    // (singularity placement) that single-resolution smoothing gets stuck on.
    // CYBER_QC_MR_SEED=legacy: diagnostic override forcing the historical per-node init
    // even when the caller asked for coherent seeding (A/B isolation of the seed basin).
    const char* seedEnv = std::getenv("CYBER_QC_MR_SEED");
    const bool legacySeed = seedEnv != nullptr && std::string(seedEnv) == "legacy";
    if (coherentSeedUnanchored && !legacySeed) {
        seedCoherentUnanchored(levels.back(), eligible);
    } else {
        for (std::size_t i = 0; i < levels.back().size(); ++i) {
            initNodeQ(levels.back(), i);
        }
    }
    smoothOrientation(levels.back(), iterations);
    dumpLevel(levels.back(), static_cast<int>(levels.size()) - 1);
    for (int lvl = static_cast<int>(levels.size()) - 2; lvl >= 0; --lvl) {
        FieldGraph& fine = levels[static_cast<std::size_t>(lvl)];
        const FieldGraph& coarse = levels[static_cast<std::size_t>(lvl) + 1];
        const std::vector<int>& parent = parents[static_cast<std::size_t>(lvl)];
        for (std::size_t i = 0; i < fine.size(); ++i) {
            fine.q[i] =
                fine.constrained[i]
                    ? fine.constraintDir[i]
                    : projectUnit(coarse.q[static_cast<std::size_t>(parent[i])], fine.normal[i]);
        }
        smoothOrientation(fine, iterations);
        dumpLevel(fine, lvl);
    }

    // Position: same coarse-to-fine schedule, carrying the coarse lattice
    // through the prolongation (parent's o shifted by this node's offset).
    for (std::size_t i = 0; i < levels.back().size(); ++i) {
        levels.back().o[i] = levels.back().pos[i];
    }
    smoothPosition(levels.back(), spacing, iterations);
    for (int lvl = static_cast<int>(levels.size()) - 2; lvl >= 0; --lvl) {
        FieldGraph& fine = levels[static_cast<std::size_t>(lvl)];
        const FieldGraph& coarse = levels[static_cast<std::size_t>(lvl) + 1];
        const std::vector<int>& parent = parents[static_cast<std::size_t>(lvl)];
        for (std::size_t i = 0; i < fine.size(); ++i) {
            const auto p = static_cast<std::size_t>(parent[i]);
            fine.o[i] = coarse.o[p] + (fine.pos[i] - coarse.pos[p]);
        }
        smoothPosition(fine, spacing, iterations);
    }

    // Scatter the finest level back onto the mesh's vertex indexing.
    const FieldGraph& fine = levels.front();
    const std::size_t cap = mesh.vertexCapacity();
    PositionField field;
    field.spacing = spacing;
    field.normal.assign(cap, Vec3{0, 0, 1});
    field.q.assign(cap, Vec3{1, 0, 0});
    field.o.assign(cap, Vec3{0, 0, 0});
    field.scale.assign(cap, 1.0f);
    field.valid.assign(cap, false);
    for (std::size_t b = 0; b < baseToVertex.size(); ++b) {
        const std::size_t v = baseToVertex[b];
        field.valid[v] = true;
        field.normal[v] = fine.normal[b];
        field.q[v] = fine.q[b];
        field.o[v] = fine.o[b];
        field.scale[v] = fine.scale.empty() ? 1.0f : fine.scale[b];
    }
    return field;
}

}  // namespace cyber::remesh
