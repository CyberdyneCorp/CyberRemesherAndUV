#include "cyber/bake/bake.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "cyber/accel/backend.hpp"
#include "cyber/accel/primitives.hpp"
#include "cyber/bake/tangent.hpp"
#include "cyber/core/bvh.hpp"
#include "cyber/core/io.hpp"

namespace cyber::bake {

namespace {

constexpr std::size_t kCancelStride = 2048;

// Per-vertex normals: area/angle-agnostic sum of incident face normals.
std::vector<Vec3> vertexNormals(const Mesh& mesh) {
    std::vector<Vec3> normals(mesh.vertexCapacity(), Vec3{});
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f)) {
            continue;
        }
        const Vec3 fn = mesh.faceNormal(f);
        for (const VertexId v : mesh.faceVertices(f)) {
            normals[v.value] += fn;
        }
    }
    for (Vec3& n : normals) {
        n = normalized(n);
    }
    return normals;
}

// Barycentric weights of `p` on triangle (a, b, c), clamped to the triangle.
std::array<float, 3> barycentric(Vec3 p, Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 v0 = b - a, v1 = c - a, v2 = p - a;
    const float d00 = dot(v0, v0), d01 = dot(v0, v1), d11 = dot(v1, v1);
    const float d20 = dot(v2, v0), d21 = dot(v2, v1);
    const float denom = d00 * d11 - d01 * d01;
    if (std::fabs(denom) < 1e-20f) {
        return {1.0f, 0.0f, 0.0f};
    }
    float v = (d11 * d20 - d01 * d21) / denom;
    float w = (d00 * d21 - d01 * d20) / denom;
    float u = 1.0f - v - w;
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    w = std::clamp(w, 0.0f, 1.0f);
    const float s = u + v + w;
    return {u / s, v / s, w / s};
}

// Smooth Target normal at a ray hit: barycentric blend of the hit triangle's
// vertex normals (falls back to the flat face normal on n-gon faces).
Vec3 hitNormal(const Mesh& mesh, const Bvh::RayHit& hit, const std::vector<Vec3>& vnormals) {
    const std::vector<VertexId> verts = mesh.faceVertices(hit.face);
    if (verts.size() != 3) {
        return mesh.faceNormal(hit.face);
    }
    const std::array<float, 3> b = barycentric(hit.point, mesh.position(verts[0]),
                                               mesh.position(verts[1]), mesh.position(verts[2]));
    Vec3 n = vnormals[verts[0].value] * b[0] + vnormals[verts[1].value] * b[1] +
             vnormals[verts[2].value] * b[2];
    return normalized(n);
}

Vec3 hitColor(const Mesh& mesh, const Bvh::RayHit& hit, const std::vector<Vec3>* colors) {
    if (colors == nullptr) {
        return Vec3{1, 1, 1};
    }
    const std::vector<VertexId> verts = mesh.faceVertices(hit.face);
    if (verts.size() != 3) {
        Vec3 sum{};
        for (const VertexId v : verts) {
            sum += (*colors)[v.value];
        }
        return sum / static_cast<float>(verts.size());
    }
    const std::array<float, 3> bc = barycentric(hit.point, mesh.position(verts[0]),
                                                mesh.position(verts[1]), mesh.position(verts[2]));
    return (*colors)[verts[0].value] * bc[0] + (*colors)[verts[1].value] * bc[1] +
           (*colors)[verts[2].value] * bc[2];
}

// Tangent from a triangle's position/UV gradient — delegates to the shared
// tangentFrame() so the basis used for baking is byte-for-byte the one exported
// with the mesh (spec: tangent-basis consistency; task 11.4).
Vec3 faceTangent(Vec3 p0, Vec3 p1, Vec3 p2, Vec2 uv0, Vec2 uv1, Vec2 uv2, Vec3 n) {
    return tangentFrame(p0, p1, p2, uv0, uv1, uv2, n).tangent;
}

// One texel to shade: pixel coord plus the interpolated low-poly frame.
struct Texel {
    int px = 0;
    int py = 0;
    Vec3 position;
    Vec3 normal;
    Vec3 tangent;
    Vec3 bitangent;
};

// Rasterizes the low-poly UV layout into covered texels carrying their
// interpolated 3D frame. Faces are fan-triangulated on the corners.
std::vector<Texel> rasterize(const Mesh& mesh, const std::vector<Vec3>& vnormals,
                             const std::vector<Vec2>& uvByLoop, int width, int height) {
    std::vector<Texel> texels;
    const auto sampleUv = [width, height](int px, int py) {
        return Vec2{(static_cast<float>(px) + 0.5f) / static_cast<float>(width),
                    1.0f - (static_cast<float>(py) + 0.5f) / static_cast<float>(height)};
    };

    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        const FaceId f{fi};
        if (!mesh.isAlive(f)) {
            continue;
        }
        const std::vector<LoopId> loops = mesh.faceLoops(f);
        for (std::size_t i = 2; i < loops.size(); ++i) {
            const std::array<LoopId, 3> tri{loops[0], loops[i - 1], loops[i]};
            std::array<Vec3, 3> pos;
            std::array<Vec3, 3> nrm;
            std::array<Vec2, 3> uv;
            for (int k = 0; k < 3; ++k) {
                const VertexId v = mesh.loopVertex(tri[static_cast<std::size_t>(k)]);
                pos[static_cast<std::size_t>(k)] = mesh.position(v);
                nrm[static_cast<std::size_t>(k)] = vnormals[v.value];
                uv[static_cast<std::size_t>(k)] = uvByLoop[tri[static_cast<std::size_t>(k)].value];
            }
            const Vec3 tangent = faceTangent(pos[0], pos[1], pos[2], uv[0], uv[1], uv[2],
                                             normalized(nrm[0] + nrm[1] + nrm[2]));

            // Pixel bounding box from the triangle's UVs (V flipped).
            float minU = 1e30f, maxU = -1e30f, minV = 1e30f, maxV = -1e30f;
            for (const Vec2& t : uv) {
                minU = std::fmin(minU, t.x);
                maxU = std::fmax(maxU, t.x);
                minV = std::fmin(minV, t.y);
                maxV = std::fmax(maxV, t.y);
            }
            const int x0 = std::max(0, static_cast<int>(std::floor(minU * static_cast<float>(width))));
            const int x1 = std::min(width - 1, static_cast<int>(std::ceil(maxU * static_cast<float>(width))));
            const int y0 = std::max(0, static_cast<int>(std::floor((1.0f - maxV) * static_cast<float>(height))));
            const int y1 = std::min(height - 1, static_cast<int>(std::ceil((1.0f - minV) * static_cast<float>(height))));

            for (int py = y0; py <= y1; ++py) {
                for (int px = x0; px <= x1; ++px) {
                    const Vec2 s = sampleUv(px, py);
                    const std::array<float, 3> bc =
                        barycentric({s.x, s.y, 0}, {uv[0].x, uv[0].y, 0}, {uv[1].x, uv[1].y, 0},
                                    {uv[2].x, uv[2].y, 0});
                    // Inside test: the clamped barycentric must reproduce the point.
                    const Vec2 rebuilt = uv[0] * bc[0] + uv[1] * bc[1] + uv[2] * bc[2];
                    if (std::fabs(rebuilt.x - s.x) > 1e-4f || std::fabs(rebuilt.y - s.y) > 1e-4f) {
                        continue;
                    }
                    Texel texel;
                    texel.px = px;
                    texel.py = py;
                    texel.position = pos[0] * bc[0] + pos[1] * bc[1] + pos[2] * bc[2];
                    texel.normal = normalized(nrm[0] * bc[0] + nrm[1] * bc[1] + nrm[2] * bc[2]);
                    texel.tangent = normalized(tangent - texel.normal * dot(texel.normal, tangent));
                    texel.bitangent = cross(texel.normal, texel.tangent);
                    texels.push_back(texel);
                }
            }
        }
    }
    return texels;
}

// Cosine-weighted hemisphere direction from a Hammersley pair, in tangent
// space (deterministic — no RNG, so AO bakes are reproducible).
Vec3 hemisphereDir(std::size_t i, std::size_t n, Vec3 t, Vec3 b, Vec3 nrm) {
    float bits = 0.0f;
    float inv = 0.5f;
    for (std::size_t v = i; v != 0; v >>= 1) {
        if (v & 1u) {
            bits += inv;
        }
        inv *= 0.5f;
    }
    const float u1 = (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
    const float r = std::sqrt(u1);
    const float phi = 2.0f * kPi * bits;
    const float x = r * std::cos(phi);
    const float y = r * std::sin(phi);
    const float z = std::sqrt(std::fmax(0.0f, 1.0f - u1));
    return normalized(t * x + b * y + nrm * z);
}

Image makeImage(int w, int h, int channels) {
    Image img;
    img.width = w;
    img.height = h;
    img.channels = channels;
    img.pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) *
                          static_cast<std::size_t>(channels),
                      0.0f);
    return img;
}

int channelsFor(BakeMap map) {
    switch (map) {
        case BakeMap::AmbientOcclusion:
        case BakeMap::Displacement:
            return 1;
        default:
            return 3;
    }
}

}  // namespace

BakeResult bake(const Mesh& lowPoly, const Mesh& highPoly, BakeMap map, const BakeParams& params,
                ProgressSink* progress, const CancelToken* cancel) {
    BakeResult result;
    const auto* uvs = lowPoly.cornerAttributes().find<Vec2>(io::kUvAttribute);
    if (uvs == nullptr || params.width <= 0 || params.height <= 0 || highPoly.faceCount() == 0) {
        return result;  // empty image: nothing to bake
    }

    const Bvh bvh(highPoly);
    const std::vector<Vec3> lowNormals = vertexNormals(lowPoly);
    const std::vector<Vec3> highNormals = vertexNormals(highPoly);
    const auto* colors = highPoly.vertexAttributes().find<Vec3>(io::kColorAttribute);

    const std::vector<Texel> texels = rasterize(lowPoly, lowNormals, *uvs, params.width, params.height);
    result.texelsCovered = texels.size();
    result.image = makeImage(params.width, params.height, channelsFor(map));

    auto& backend = *accel::defaultBackend();

    for (std::size_t i = 0; i < texels.size(); ++i) {
        if (cancel != nullptr && i % kCancelStride == 0 && cancel->isCancelled()) {
            result.cancelled = true;
            return result;
        }
        const Texel& tx = texels[i];

        if (map == BakeMap::AmbientOcclusion) {
            accel::Buffer<Vec3> origins(static_cast<std::size_t>(params.aoSamples));
            accel::Buffer<Vec3> dirs(static_cast<std::size_t>(params.aoSamples));
            for (int k = 0; k < params.aoSamples; ++k) {
                origins[static_cast<std::size_t>(k)] = tx.position + tx.normal * params.aoBias;
                dirs[static_cast<std::size_t>(k)] = hemisphereDir(static_cast<std::size_t>(k),
                                                                  static_cast<std::size_t>(params.aoSamples),
                                                                  tx.tangent, tx.bitangent, tx.normal);
            }
            accel::Buffer<std::optional<Bvh::RayHit>> hits;
            accel::raycast(backend, bvh, origins, dirs, hits);
            int occluded = 0;
            for (std::size_t k = 0; k < hits.size(); ++k) {
                if (hits[k].has_value() && hits[k]->t <= params.aoRadius) {
                    ++occluded;
                }
            }
            const float openness =
                1.0f - static_cast<float>(occluded) / static_cast<float>(params.aoSamples);
            result.image.at(tx.px, tx.py, 0) = openness;
            continue;
        }

        // Primary projection ray from the cage inward along the surface normal.
        const Vec3 origin = tx.position + tx.normal * params.cageDistance;
        const Vec3 dir = tx.normal * -1.0f;
        const std::optional<Bvh::RayHit> hit = bvh.raycast(origin, dir);
        const bool valid = hit.has_value() && hit->t <= 2.0f * params.cageDistance;

        switch (map) {
            case BakeMap::Normal: {
                Vec3 encoded{0.5f, 0.5f, 1.0f};
                if (valid) {
                    const Vec3 hn = hitNormal(highPoly, *hit, highNormals);
                    const Vec3 tn{dot(hn, tx.tangent), dot(hn, tx.bitangent), dot(hn, tx.normal)};
                    encoded = tn * 0.5f + Vec3{0.5f, 0.5f, 0.5f};
                }
                result.image.at(tx.px, tx.py, 0) = encoded.x;
                result.image.at(tx.px, tx.py, 1) = encoded.y;
                result.image.at(tx.px, tx.py, 2) = encoded.z;
                break;
            }
            case BakeMap::Displacement: {
                // Height of the hit above the low-poly surface along its normal.
                result.image.at(tx.px, tx.py, 0) = valid ? params.cageDistance - hit->t : 0.0f;
                break;
            }
            case BakeMap::Position: {
                const Vec3 p = valid ? hit->point : tx.position;
                result.image.at(tx.px, tx.py, 0) = p.x;
                result.image.at(tx.px, tx.py, 1) = p.y;
                result.image.at(tx.px, tx.py, 2) = p.z;
                break;
            }
            case BakeMap::Color: {
                const Vec3 c = valid ? hitColor(highPoly, *hit, colors) : Vec3{1, 1, 1};
                result.image.at(tx.px, tx.py, 0) = c.x;
                result.image.at(tx.px, tx.py, 1) = c.y;
                result.image.at(tx.px, tx.py, 2) = c.z;
                break;
            }
            case BakeMap::AmbientOcclusion:
                break;  // handled above
        }
    }

    if (progress != nullptr) {
        progress->report(1.0f, "bake");
    }
    return result;
}

}  // namespace cyber::bake
