#include "topology_layout_build.hpp"

#include <algorithm>
#include <cstdint>

namespace cyber::remesh {
namespace {

Vec3 toVec3(const std::array<float, 3>& p) { return Vec3{p[0], p[1], p[2]}; }

LayoutNodeKind kindOf(bimdf::NodeGeomKind k) {
    switch (k) {
        case bimdf::NodeGeomKind::Cone:
            return LayoutNodeKind::Singularity;
        case bimdf::NodeGeomKind::Crease:
            return LayoutNodeKind::FeatureCorner;
        case bimdf::NodeGeomKind::Boundary:
            return LayoutNodeKind::BoundaryCorner;
        case bimdf::NodeGeomKind::TJunction:
            return LayoutNodeKind::TJunction;
        case bimdf::NodeGeomKind::Regular:
            return LayoutNodeKind::Regular;
    }
    return LayoutNodeKind::TJunction;
}

// Compact face index -> source FaceId. Falls back to the compact index when
// the caller did not supply the map.
class FaceMapper {
public:
    explicit FaceMapper(const bimdf::Charts& charts) : map_(charts.faceOfCompact) {}

    FaceId operator()(std::size_t compact) const {
        return FaceId{compact < map_.size() ? map_[compact] : static_cast<std::uint32_t>(compact)};
    }

private:
    const std::vector<std::uint32_t>& map_;
};

void convertNodes(const bimdf::TMesh& tm, const FaceMapper& face, TopologyLayout& layout) {
    layout.nodes.reserve(tm.nodeCount);
    for (std::size_t i = 0; i < tm.nodeCount; ++i) {
        LayoutNode n;
        n.id = static_cast<LayoutNodeId>(i);
        if (i >= tm.nodeGeom.size()) {
            n.face = FaceId{0};  // traced without geometry capture
            layout.nodes.push_back(n);
            continue;
        }
        const bimdf::NodeGeom& g = tm.nodeGeom[i];
        n.kind = kindOf(g.kind);
        n.position = toVec3(g.position);
        n.face = face(g.face);
        n.singularityIndex = n.kind == LayoutNodeKind::Singularity ? g.coneIndex : 0;
        if (g.meshVertex != 0xFFFFFFFFu) {
            n.vertex = VertexId{g.meshVertex};
        }
        // Cones and feature/boundary corners are the layout's fixed scaffolding
        // until a stage is explicitly allowed to move them.
        n.locked = n.kind != LayoutNodeKind::TJunction;
        layout.nodes.push_back(std::move(n));
    }
}

void convertArcs(const bimdf::TMesh& tm, const FaceMapper& face, TopologyLayout& layout) {
    layout.arcs.reserve(tm.arcs.size());
    for (std::size_t a = 0; a < tm.arcs.size(); ++a) {
        const bimdf::Arc& src = tm.arcs[a];
        LayoutArc arc;
        arc.id = static_cast<LayoutArcId>(a);
        arc.begin = static_cast<LayoutNodeId>(src.n0);
        arc.end = static_cast<LayoutNodeId>(src.n1);
        arc.kind = src.onFeature    ? LayoutArcKind::Feature
                   : src.onBoundary ? LayoutArcKind::Boundary
                                    : LayoutArcKind::Separatrix;
        // Relaxed arc lengths can go slightly negative on a folded map (the
        // parametric span of a separatrix piece locally reverses). The layout
        // reports a length, not a signed span, so clamp at zero.
        arc.desiredLength = std::max(0.0, src.len);
        arc.excluded = a < tm.arcExcluded.size() && tm.arcExcluded[a] != 0;
        arc.locked = src.onFeature;
        if (a < tm.arcGeom.size()) {
            const auto& pts = tm.arcGeom[a].points;
            arc.samples.reserve(pts.size());
            for (const bimdf::ArcPoint& p : pts) {
                arc.samples.push_back(LayoutSample{toVec3(p.position), face(p.face)});
            }
        }
        // Snap the polyline ends onto the node positions. The trail records the
        // walk's own interpolation, which agrees with the event position to
        // rounding; welding them exactly is what lets a viewer (and the layout
        // invariants) treat arcs as sharing their endpoints.
        if (!arc.samples.empty() && arc.begin < layout.nodes.size() &&
            arc.end < layout.nodes.size()) {
            arc.samples.front().position = layout.nodes[arc.begin].position;
            arc.samples.back().position = layout.nodes[arc.end].position;
        }
        layout.arcs.push_back(std::move(arc));
    }
}

void convertPatches(const bimdf::TMesh& tm, TopologyLayout& layout) {
    layout.patches.reserve(tm.patches.size());
    for (std::size_t p = 0; p < tm.patches.size(); ++p) {
        LayoutPatch patch;
        patch.id = static_cast<LayoutPatchId>(p);
        patch.sides.reserve(tm.patches[p].side.size());
        for (const auto& side : tm.patches[p].side) {
            std::vector<LayoutArcId> out;
            out.reserve(side.size());
            for (const std::size_t a : side) {
                out.push_back(static_cast<LayoutArcId>(a));
            }
            patch.sides.push_back(std::move(out));
        }
        layout.patches.push_back(std::move(patch));
    }
}

}  // namespace

TopologyLayout layoutFromTMesh(const bimdf::Charts& charts, const bimdf::TMesh& tm) {
    const FaceMapper face(charts);
    TopologyLayout layout;
    convertNodes(tm, face, layout);
    convertArcs(tm, face, layout);
    convertPatches(tm, layout);
    return layout;
}

void applyQuantization(TopologyLayout& layout, const bimdf::TMesh& tm,
                       const bimdf::BimdfResult& result) {
    if (!result.ok) {
        return;
    }
    const std::size_t n = std::min(layout.arcs.size(), result.arcLenHalf.size());
    for (std::size_t a = 0; a < n; ++a) {
        if (a < result.arcOutside.size() && result.arcOutside[a] != 0) {
            continue;  // never entered the flow network; no assignment to record
        }
        // Cones live on the half-integer lattice, so the solve works in
        // half-cells; the layout reports whole grid edges.
        layout.arcs[a].quantizedLength = static_cast<int>(result.arcLenHalf[a] / 2);
    }
    for (std::size_t p = 0; p < layout.patches.size() && p < tm.patches.size(); ++p) {
        LayoutPatch& patch = layout.patches[p];
        if (!patch.quadLike()) {
            continue;
        }
        const auto sideSum = [&](std::size_t k) {
            long long total = 0;
            for (const LayoutArcId a : patch.sides[k]) {
                if (a < result.arcLenHalf.size()) {
                    total += result.arcLenHalf[a];
                }
            }
            return static_cast<int>(total / 2);
        };
        patch.uCount = sideSum(0);
        patch.vCount = sideSum(1);
    }
}

}  // namespace cyber::remesh
