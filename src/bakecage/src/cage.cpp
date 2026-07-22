#include "cyber/bakecage/cage.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cyber::bakecage {

namespace {

// Area-weighted (Newell face normals already carry area) vertex normal:
// the sum of incident face normals, normalised. Zero for isolated vertices.
Vec3 vertexNormal(const Mesh& mesh, VertexId v) {
    Vec3 sum{};
    for (const FaceId f : mesh.vertexFaces(v)) {
        sum += mesh.faceNormal(f);
    }
    return normalized(sum);
}

}  // namespace

float falloffWeight(Falloff curve, float t) {
    if (t <= 0.0f) {
        return 1.0f;
    }
    if (t >= 1.0f) {
        return 0.0f;
    }
    switch (curve) {
        case Falloff::Constant:
            return 1.0f;
        case Falloff::Linear:
            return 1.0f - t;
        case Falloff::Smooth: {
            const float s = 1.0f - t;
            return s * s * (3.0f - 2.0f * s);  // smoothstep on (1 - t)
        }
        case Falloff::Sharp: {
            const float s = 1.0f - t;
            return s * s;
        }
        case Falloff::Root:
            return std::sqrt(1.0f - t);
    }
    return 0.0f;
}

Cage::Cage(const Mesh& mesh, float defaultDistance)
    : m_mesh(&mesh),
      m_defaultDistance(defaultDistance),
      m_distance(mesh.vertexCapacity(), defaultDistance),
      m_overridden(mesh.vertexCapacity(), std::uint8_t{0}) {}

float Cage::distance(VertexId v) const {
    return v.value < m_distance.size() ? m_distance[v.value] : m_defaultDistance;
}

bool Cage::isOverridden(VertexId v) const {
    return v.value < m_overridden.size() && m_overridden[v.value] != 0;
}

std::size_t Cage::tweakDistance(std::span<const VertexId> region, float delta, const Brush& brush) {
    const float radius = std::max(brush.radius, 1e-6f);
    std::size_t modified = 0;
    for (const VertexId v : region) {
        if (v.value >= m_distance.size() || !m_mesh->isAlive(v)) {
            continue;
        }
        if (m_overridden[v.value] != 0) {
            continue;  // pinned vertices are immune to falloff tweaks
        }
        const float dist = length(m_mesh->position(v) - brush.center);
        const float weight = falloffWeight(brush.curve, dist / radius);
        if (weight <= 0.0f) {
            continue;
        }
        m_distance[v.value] = std::max(0.0f, m_distance[v.value] + delta * weight);
        ++modified;
    }
    return modified;
}

void Cage::setVertexDistance(VertexId v, float d) {
    if (v.value >= m_distance.size()) {
        return;
    }
    m_distance[v.value] = std::max(0.0f, d);
    m_overridden[v.value] = 1;
}

void Cage::clearOverride(VertexId v) {
    if (v.value < m_overridden.size()) {
        m_overridden[v.value] = 0;
    }
}

void Cage::relax(int iterations, float strength) {
    const float s = std::clamp(strength, 0.0f, 1.0f);
    if (s == 0.0f) {
        return;
    }
    std::vector<float> next = m_distance;
    for (int it = 0; it < iterations; ++it) {
        for (Index vi = 0; vi < static_cast<Index>(m_distance.size()); ++vi) {
            const VertexId v{vi};
            if (!m_mesh->isAlive(v) || m_overridden[vi] != 0) {
                continue;  // pinned / dead vertices are fixed constraints
            }
            float sum = 0.0f;
            std::size_t count = 0;
            for (const EdgeId e : m_mesh->vertexEdges(v)) {
                const auto [a, b] = m_mesh->edgeVertices(e);
                const VertexId other = (a == v) ? b : a;
                if (other.value < m_distance.size()) {
                    sum += m_distance[other.value];
                    ++count;
                }
            }
            if (count == 0) {
                continue;
            }
            const float average = sum / static_cast<float>(count);
            next[vi] = m_distance[vi] + (average - m_distance[vi]) * s;
        }
        m_distance = next;
    }
}

void Cage::reset(std::span<const VertexId> region) {
    for (const VertexId v : region) {
        if (v.value < m_distance.size()) {
            m_distance[v.value] = m_defaultDistance;
            m_overridden[v.value] = 0;
        }
    }
}

void Cage::reset() {
    std::fill(m_distance.begin(), m_distance.end(), m_defaultDistance);
    std::fill(m_overridden.begin(), m_overridden.end(), std::uint8_t{0});
}

std::vector<CageRay> Cage::projectionRays() const {
    std::vector<CageRay> rays;
    rays.reserve(m_mesh->vertexCount());
    for (Index vi = 0; vi < static_cast<Index>(m_distance.size()); ++vi) {
        const VertexId v{vi};
        if (!m_mesh->isAlive(v)) {
            continue;
        }
        const Vec3 normal = vertexNormal(*m_mesh, v);
        const float d = m_distance[vi];
        CageRay ray;
        ray.origin = m_mesh->position(v) + normal * d;
        ray.direction = normal * -1.0f;  // aim back through the surface
        ray.maxDistance = 2.0f * d;
        ray.vertex = v;
        rays.push_back(ray);
    }
    return rays;
}

CageState Cage::save() const { return CageState{m_defaultDistance, m_distance, m_overridden}; }

bool Cage::load(const CageState& state) {
    if (state.distances.size() != m_distance.size() ||
        state.overridden.size() != m_overridden.size()) {
        return false;
    }
    m_defaultDistance = state.defaultDistance;
    m_distance = state.distances;
    m_overridden = state.overridden;
    return true;
}

std::vector<std::byte> Cage::serialize() const {
    const std::uint64_t count = m_distance.size();
    std::vector<std::byte> out;
    out.resize(sizeof(float) + sizeof(std::uint64_t) + count * sizeof(float) +
               count * sizeof(std::uint8_t));
    std::size_t offset = 0;
    auto put = [&](const void* src, std::size_t n) {
        std::memcpy(out.data() + offset, src, n);
        offset += n;
    };
    put(&m_defaultDistance, sizeof(float));
    put(&count, sizeof(std::uint64_t));
    if (count != 0) {
        put(m_distance.data(), count * sizeof(float));
        put(m_overridden.data(), count * sizeof(std::uint8_t));
    }
    return out;
}

bool Cage::deserialize(std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    auto get = [&](void* dst, std::size_t n) {
        if (offset + n > bytes.size()) {
            return false;
        }
        std::memcpy(dst, bytes.data() + offset, n);
        offset += n;
        return true;
    };
    float defaultDistance = 0.0f;
    std::uint64_t count = 0;
    if (!get(&defaultDistance, sizeof(float)) || !get(&count, sizeof(std::uint64_t))) {
        return false;
    }
    if (count != m_distance.size()) {
        return false;  // blob was captured against a different topology
    }
    std::vector<float> distances(count);
    std::vector<std::uint8_t> overridden(count);
    if (count != 0) {
        if (!get(distances.data(), count * sizeof(float)) ||
            !get(overridden.data(), count * sizeof(std::uint8_t))) {
            return false;
        }
    }
    m_defaultDistance = defaultDistance;
    m_distance = std::move(distances);
    m_overridden = std::move(overridden);
    return true;
}

}  // namespace cyber::bakecage
