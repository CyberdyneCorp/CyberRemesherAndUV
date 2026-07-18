#include "cyber/retopo/snapping.hpp"

namespace cyber::retopo {

SurfaceSnapper::SurfaceSnapper(const Mesh& target) : m_bvh(target) {
    m_vertices.reserve(target.vertexCount());
    for (Index i = 0; i < target.vertexCapacity(); ++i) {
        const VertexId v{i};
        if (!target.isAlive(v)) {
            continue;
        }
        m_vertices.push_back({v, target.position(v)});
    }
}

SurfaceHit SurfaceSnapper::snapToSurface(Vec3 query) const {
    const Bvh::ClosestHit hit = m_bvh.closestPoint(query);
    return {hit.point, hit.face, hit.distanceSquared};
}

std::optional<VertexHit> SurfaceSnapper::snapToVertex(Vec3 query, float radius) const {
    const float radiusSquared = radius * radius;
    std::optional<VertexHit> best;
    for (const VertexRecord& rec : m_vertices) {
        const float d2 = lengthSquared(rec.position - query);
        if (d2 > radiusSquared) {
            continue;
        }
        if (!best || d2 < best->distanceSquared) {
            best = VertexHit{rec.id, rec.position, d2};
        }
    }
    return best;
}

}  // namespace cyber::retopo
