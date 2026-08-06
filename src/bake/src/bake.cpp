#include "cyber/bake/bake.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "cyber/accel/backend.hpp"
#include "cyber/accel/primitives.hpp"
#include "cyber/bake/curvature.hpp"
#include "cyber/bake/tangent.hpp"
#include "cyber/core/bvh.hpp"
#include "cyber/core/io.hpp"
#include "cyber/imageio/load.hpp"

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

// Interpolated per-vertex scalar at a ray hit (n-gon faces average the corners,
// mirroring hitColor()).
float hitScalar(const Mesh& mesh, const Bvh::RayHit& hit, const std::vector<float>& values) {
    const std::vector<VertexId> verts = mesh.faceVertices(hit.face);
    if (verts.size() != 3) {
        float sum = 0.0f;
        for (const VertexId v : verts) {
            sum += values[v.value];
        }
        return sum / static_cast<float>(verts.size());
    }
    const std::array<float, 3> bc = barycentric(hit.point, mesh.position(verts[0]),
                                                mesh.position(verts[1]), mesh.position(verts[2]));
    return values[verts[0].value] * bc[0] + values[verts[1].value] * bc[1] +
           values[verts[2].value] * bc[2];
}

// Interpolated Target UV at a ray hit, from the hit face's per-corner "uv"
// attribute (n-gon faces fall back to the corner-average UV, mirroring
// hitColor()).
Vec2 hitUv(const Mesh& mesh, const Bvh::RayHit& hit, const std::vector<Vec2>& uvs) {
    const std::vector<LoopId> loops = mesh.faceLoops(hit.face);
    const std::size_t n = loops.size();
    if (n < 3) {
        return uvs[loops[0].value];
    }
    // Fan-triangulate (loops[0], loops[i], loops[i+1]) as the BVH does and
    // interpolate from the sub-triangle that best contains the hit — averaging
    // an n-gon's corner UVs would flatten the mapping (constant UV per face).
    Vec2 best{};
    float bestScore = -1e30f;
    for (std::size_t i = 1; i + 1 < n; ++i) {
        const std::array<float, 3> bc = barycentric(
            hit.point, mesh.position(mesh.loopVertex(loops[0])),
            mesh.position(mesh.loopVertex(loops[i])), mesh.position(mesh.loopVertex(loops[i + 1])));
        const float score = std::min({bc[0], bc[1], bc[2]});  // least clamping = inside
        if (score > bestScore) {
            bestScore = score;
            best = uvs[loops[0].value] * bc[0] + uvs[loops[i].value] * bc[1] +
                   uvs[loops[i + 1].value] * bc[2];
        }
    }
    return best;
}

// Target color at a ray hit sampled from `texture` at the interpolated Target
// UV (bilinear). Returns the RGB channels of the RGBA sample.
Vec3 hitTextureColor(const Mesh& mesh, const Bvh::RayHit& hit, const std::vector<Vec2>& uvs,
                     const imageio::LoadedImage& texture) {
    const Vec2 uv = hitUv(mesh, hit, uvs);
    const std::array<float, 4> rgba = imageio::sampleBilinear(texture, uv.x, uv.y);
    return Vec3{rgba[0], rgba[1], rgba[2]};
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
            const int x0 =
                std::max(0, static_cast<int>(std::floor(minU * static_cast<float>(width))));
            const int x1 =
                std::min(width - 1, static_cast<int>(std::ceil(maxU * static_cast<float>(width))));
            const int y0 = std::max(
                0, static_cast<int>(std::floor((1.0f - maxV) * static_cast<float>(height))));
            const int y1 =
                std::min(height - 1,
                         static_cast<int>(std::ceil((1.0f - minV) * static_cast<float>(height))));

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

// An arbitrary unit tangent orthogonal to `n` (Duff et al. branchless frame).
// Used to build the hemisphere basis when AO fires around a high-poly hit
// normal that carries no UV-derived tangent of its own.
Vec3 anyTangent(Vec3 n) {
    const float sign = std::copysign(1.0f, n.z);
    const float a = -1.0f / (sign + n.z);
    const float bxy = n.x * n.y * a;
    return normalized(Vec3{1.0f + sign * n.x * n.x * a, sign * bxy, -sign * n.x});
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
        case BakeMap::Curvature:
        case BakeMap::Cavity:
            return 1;
        default:
            return 3;
    }
}

// Grayscale encoding of a signed curvature value. Curvature is centred on
// mid-gray with convex bright and concave dark; Cavity keeps concavity only, so
// flat and convex both read white and the map drops straight into a multiply
// slot. `range` <= 0 means there is nothing to normalize against (a flat
// Target), in which case every texel takes the neutral value.
float encodeCurvature(float curvature, float range, bool cavityOnly) {
    if (cavityOnly) {
        if (range <= 0.0f) {
            return 1.0f;
        }
        const float concavity = std::clamp(-curvature / range, 0.0f, 1.0f);
        return 1.0f - concavity;
    }
    if (range <= 0.0f) {
        return 0.5f;
    }
    return 0.5f + 0.5f * std::clamp(curvature / range, -1.0f, 1.0f);
}

// Which maps an attached field evaluator can serve. The other three
// (Displacement, Position, Color) describe the Target MESH — a height above the
// low-poly, a hit point, a vertex color — and have no field counterpart, so
// they keep requiring the high-poly whatever is attached.
bool fieldSupports(BakeMap map) {
    switch (map) {
        case BakeMap::Normal:
        case BakeMap::AmbientOcclusion:
        case BakeMap::Curvature:
        case BakeMap::Cavity:
            return true;
        default:
            return false;
    }
}

// AO at one texel on the raycast path: project onto the high-poly with the same
// cage ray normal/displacement use, then fire the hemisphere from the HIGH-poly
// hit so occlusion captures the high-poly's crevices, not the low-poly's smooth
// surface. Falls back to the low-poly frame when the projection misses the cage.
float aoOpennessFromMesh(const Bvh& bvh, const Mesh& highPoly, const std::vector<Vec3>& highNormals,
                         const Texel& tx, const BakeParams& params, accel::IBackend& backend) {
    const Vec3 projOrigin = tx.position + tx.normal * params.cageDistance;
    const std::optional<Bvh::RayHit> proj = bvh.raycast(projOrigin, tx.normal * -1.0f);
    const bool projValid = proj.has_value() && proj->t <= 2.0f * params.cageDistance;
    const Vec3 aoNormal = projValid ? hitNormal(highPoly, *proj, highNormals) : tx.normal;
    const Vec3 aoPosition = projValid ? proj->point : tx.position;
    const Vec3 aoTangent = anyTangent(aoNormal);
    const Vec3 aoBitangent = cross(aoNormal, aoTangent);

    accel::Buffer<Vec3> origins(static_cast<std::size_t>(params.aoSamples));
    accel::Buffer<Vec3> dirs(static_cast<std::size_t>(params.aoSamples));
    for (int k = 0; k < params.aoSamples; ++k) {
        origins[static_cast<std::size_t>(k)] = aoPosition + aoNormal * params.aoBias;
        dirs[static_cast<std::size_t>(k)] =
            hemisphereDir(static_cast<std::size_t>(k), static_cast<std::size_t>(params.aoSamples),
                          aoTangent, aoBitangent, aoNormal);
    }
    accel::Buffer<std::optional<Bvh::RayHit>> hits;
    accel::raycast(backend, bvh, origins, dirs, hits);
    int occluded = 0;
    for (std::size_t k = 0; k < hits.size(); ++k) {
        if (hits[k].has_value() && hits[k]->t <= params.aoRadius) {
            ++occluded;
        }
    }
    return 1.0f - static_cast<float>(occluded) / static_cast<float>(params.aoSamples);
}

// The raycast shading pass. Its arithmetic is deliberately untouched by the
// field-evaluator work: `params.field == nullptr` must reproduce the pre-bridge
// pixels bit for bit (pipeline-bridge spec, "No evaluator, no behavior change"),
// which tests/bake/test_field_bake.cpp pins against captured checksums.
void shadeFromMesh(BakeResult& result, const std::vector<Texel>& texels, const Mesh& highPoly,
                   BakeMap map, const BakeParams& params, const CancelToken* cancel) {
    const Bvh bvh(highPoly);
    const std::vector<Vec3> highNormals = vertexNormals(highPoly);
    const auto* colors = highPoly.vertexAttributes().find<Vec3>(io::kColorAttribute);
    const auto* highUvs = highPoly.cornerAttributes().find<Vec2>(io::kUvAttribute);
    // Texture color source is only usable when a texture and Target UVs both
    // exist; otherwise the Color bake falls back to Target vertex colors.
    const bool useTexture = params.colorSource.kind == ColorSource::Texture &&
                            params.colorSource.texture != nullptr && highUvs != nullptr;

    // Curvature/cavity read the Target's curvature field at the same cage hit
    // the normal bake uses, so the two maps register texel for texel.
    std::vector<float> highCurvature;
    float curvatureRange = 0.0f;
    if (map == BakeMap::Curvature || map == BakeMap::Cavity) {
        highCurvature = vertexMeanCurvature(highPoly);
        curvatureRange =
            params.curvatureRange > 0.0f ? params.curvatureRange : curvatureScale(highCurvature);
    }

    auto& backend = *accel::defaultBackend();

    for (std::size_t i = 0; i < texels.size(); ++i) {
        if (cancel != nullptr && i % kCancelStride == 0 && cancel->isCancelled()) {
            result.cancelled = true;
            return;
        }
        const Texel& tx = texels[i];

        if (map == BakeMap::AmbientOcclusion) {
            result.image.at(tx.px, tx.py, 0) =
                aoOpennessFromMesh(bvh, highPoly, highNormals, tx, params, backend);
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
                Vec3 c{1, 1, 1};
                if (valid) {
                    c = useTexture
                            ? hitTextureColor(highPoly, *hit, *highUvs, *params.colorSource.texture)
                            : hitColor(highPoly, *hit, colors);
                }
                result.image.at(tx.px, tx.py, 0) = c.x;
                result.image.at(tx.px, tx.py, 1) = c.y;
                result.image.at(tx.px, tx.py, 2) = c.z;
                break;
            }
            case BakeMap::Curvature:
            case BakeMap::Cavity: {
                // A missed cage ray contributes 0 curvature, which encodes to
                // the neutral value for both variants (mid-gray / white).
                const float h = valid ? hitScalar(highPoly, *hit, highCurvature) : 0.0f;
                result.image.at(tx.px, tx.py, 0) =
                    encodeCurvature(h, curvatureRange, map == BakeMap::Cavity);
                break;
            }
            case BakeMap::AmbientOcclusion:
                break;  // handled above
        }
    }
}

// One sphere-traced hit of the cage ray against the field. `t` is the distance
// travelled from the cage origin, so the caller applies exactly the same
// `t <= 2 * cageDistance` acceptance rule the BVH path uses.
struct FieldHit {
    bool valid = false;
    Vec3 position;
    Vec3 normal;
};

FieldHit traceField(const FieldEvaluator& field, const Texel& tx, const BakeParams& params) {
    constexpr int kMaxSteps = 96;
    const float maxT = 2.0f * params.cageDistance;
    // Surface tolerance scaled to the cage so the tracer's precision follows
    // the model's size rather than a fixed world epsilon.
    const float epsilon = std::fmax(1e-6f, params.cageDistance * 1e-4f);
    const Vec3 origin = tx.position + tx.normal * params.cageDistance;
    const Vec3 dir = tx.normal * -1.0f;

    float t = 0.0f;
    for (int step = 0; step < kMaxSteps && t <= maxT; ++step) {
        const Vec3 p = origin + dir * t;
        const float d = field.distance(p);
        if (std::fabs(d) <= epsilon) {
            return {true, p, normalized(field.gradient(p))};
        }
        // |d| because the cage origin can start inside the field (a bulge that
        // pokes through the low-poly); marching by the magnitude converges from
        // either side, and the Lipschitz-<=1 contract makes the step safe.
        t += std::fmax(std::fabs(d), epsilon);
    }
    return {};
}

// The field shading pass. Only the four maps fieldSupports() covers reach here.
void shadeFromField(BakeResult& result, const std::vector<Texel>& texels,
                    const FieldEvaluator& field, BakeMap map, const BakeParams& params,
                    const CancelToken* cancel) {
    const bool curvatureMap = map == BakeMap::Curvature || map == BakeMap::Cavity;
    // Central-difference step for the evaluator's curvature default, tied to the
    // cage the same way the trace tolerance is.
    const float curvatureStep = std::fmax(1e-5f, params.cageDistance * 0.01f);

    std::vector<FieldHit> hits(texels.size());
    std::vector<float> samples(curvatureMap ? texels.size() : 0, 0.0f);
    for (std::size_t i = 0; i < texels.size(); ++i) {
        if (cancel != nullptr && i % kCancelStride == 0 && cancel->isCancelled()) {
            result.cancelled = true;
            return;
        }
        hits[i] = traceField(field, texels[i], params);
        if (curvatureMap && hits[i].valid) {
            samples[i] = field.curvature(hits[i].position, curvatureStep);
        }
    }

    // Without a Target mesh there is no vertex curvature field to take the
    // auto range from, so it comes from the sampled texels through the same
    // percentile helper the mesh path uses.
    const float curvatureRange =
        params.curvatureRange > 0.0f ? params.curvatureRange : curvatureScale(samples);

    for (std::size_t i = 0; i < texels.size(); ++i) {
        const Texel& tx = texels[i];
        const FieldHit& hit = hits[i];
        switch (map) {
            case BakeMap::Normal: {
                Vec3 encoded{0.5f, 0.5f, 1.0f};
                if (hit.valid) {
                    const Vec3 tn{dot(hit.normal, tx.tangent), dot(hit.normal, tx.bitangent),
                                  dot(hit.normal, tx.normal)};
                    encoded = tn * 0.5f + Vec3{0.5f, 0.5f, 0.5f};
                }
                result.image.at(tx.px, tx.py, 0) = encoded.x;
                result.image.at(tx.px, tx.py, 1) = encoded.y;
                result.image.at(tx.px, tx.py, 2) = encoded.z;
                break;
            }
            case BakeMap::AmbientOcclusion: {
                const Vec3 n = hit.valid ? hit.normal : tx.normal;
                const Vec3 p = hit.valid ? hit.position : tx.position;
                result.image.at(tx.px, tx.py, 0) = std::clamp(
                    field.occlusion(p + n * params.aoBias, n, params.aoRadius), 0.0f, 1.0f);
                break;
            }
            default: {  // Curvature | Cavity — fieldSupports() gates the rest out
                result.image.at(tx.px, tx.py, 0) =
                    encodeCurvature(samples[i], curvatureRange, map == BakeMap::Cavity);
                break;
            }
        }
    }
}

}  // namespace

BakeResult bake(const Mesh& lowPoly, const Mesh& highPoly, BakeMap map, const BakeParams& params,
                ProgressSink* progress, const CancelToken* cancel) {
    BakeResult result;
    const auto* uvs = lowPoly.cornerAttributes().find<Vec2>(io::kUvAttribute);
    // An evaluator makes the Target mesh optional, but only for the maps the
    // field can actually answer.
    const bool useField = params.field != nullptr && fieldSupports(map);
    if (uvs == nullptr || params.width <= 0 || params.height <= 0 ||
        (highPoly.faceCount() == 0 && !useField)) {
        return result;  // empty image: nothing to bake
    }

    const std::vector<Vec3> lowNormals = vertexNormals(lowPoly);
    const std::vector<Texel> texels =
        rasterize(lowPoly, lowNormals, *uvs, params.width, params.height);
    result.texelsCovered = texels.size();
    result.image = makeImage(params.width, params.height, channelsFor(map));

    if (useField) {
        shadeFromField(result, texels, *params.field, map, params, cancel);
    } else {
        shadeFromMesh(result, texels, highPoly, map, params, cancel);
    }
    if (result.cancelled) {
        return result;
    }

    if (progress != nullptr) {
        progress->report(1.0f, "bake");
    }
    return result;
}

}  // namespace cyber::bake
