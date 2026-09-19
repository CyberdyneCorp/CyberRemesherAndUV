#include "cyber/bake/bake.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>

#include "border_padding.hpp"
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

bool isFinite(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }

bool isFinite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

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
            // A non-finite corner (a glTF TEXCOORD_0 accessor holding NaN, say)
            // poisons the whole sub-triangle: fmin/fmax silently ignore NaN, so
            // the bbox below collapses to the finite corners, and the inside
            // test is a `>` comparison that a NaN passes — every texel in that
            // bbox would be accepted and written as NaN.
            if (!isFinite(uv[0]) || !isFinite(uv[1]) || !isFinite(uv[2]) || !isFinite(pos[0]) ||
                !isFinite(pos[1]) || !isFinite(pos[2])) {
                continue;
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

// Integer avalanche (Wang-style), the hash behind the per-texel sample
// rotation. A hash, not an RNG: the same texel always yields the same offsets,
// so a bake is still reproducible bit for bit.
std::uint32_t mixBits(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Per-texel Cranley-Patterson offsets in [0,1) for the (radius, azimuth) pair.
// Every texel used to fire the IDENTICAL Hammersley set, so openness could only
// land on the k/aoSamples lattice AND whole neighbourhoods snapped to the same
// rung — visible banding, not noise. Shifting the sequence toroidally per texel
// keeps its low-discrepancy stratification while breaking that lock-step, which
// turns the residual estimator error into fine dither.
Vec2 texelRotation(int px, int py) {
    constexpr float kInv32 = 2.3283064e-10f;  // 1 / 2^32
    const std::uint32_t h1 = mixBits(static_cast<std::uint32_t>(px) * 0x9e3779b9u ^
                                     mixBits(static_cast<std::uint32_t>(py)));
    const std::uint32_t h2 = mixBits(h1 ^ 0x68bc21ebu);
    return Vec2{static_cast<float>(h1) * kInv32, static_cast<float>(h2) * kInv32};
}

// Cosine-weighted hemisphere direction from a Hammersley pair rotated by
// `rot`, in tangent space (deterministic — no RNG, so AO bakes are
// reproducible).
Vec3 hemisphereDir(std::size_t i, std::size_t n, Vec2 rot, Vec3 t, Vec3 b, Vec3 nrm) {
    float bits = 0.0f;
    float inv = 0.5f;
    for (std::size_t v = i; v != 0; v >>= 1) {
        if (v & 1u) {
            bits += inv;
        }
        inv *= 0.5f;
    }
    const auto wrap = [](float v) { return v - std::floor(v); };
    const float u1 = wrap((static_cast<float>(i) + 0.5f) / static_cast<float>(n) + rot.x);
    const float r = std::sqrt(u1);
    const float phi = 2.0f * kPi * wrap(bits + rot.y);
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
        case BakeMap::Thickness:
            return 1;
        default:
            return 3;
    }
}

// The three maps whose cost is a hemisphere of rays per texel, as opposed to the
// single cage projection ray every other map casts. They share one sampling
// pass, so they also share its parallelism, progress and cancellation.
bool isRayTraced(BakeMap map) {
    return map == BakeMap::AmbientOcclusion || map == BakeMap::BentNormal ||
           map == BakeMap::Thickness;
}

// The two maps whose texels are exact KEYS rather than measurements.
bool isIdMap(BakeMap map) { return map == BakeMap::MaterialId || map == BakeMap::ObjectId; }

// Re-expresses an engine-space (y-up) vector in the requested convention. The
// z-up form is the standard -90 degree rotation about X: y-up's up (0,1,0)
// becomes (0,0,1). Its own inverse is (x, z, -y).
Vec3 toUpAxis(Vec3 v, UpAxis axis) { return axis == UpAxis::ZUp ? Vec3{v.x, -v.z, v.y} : v; }

// Encodes a unit direction into [0,1] the way every normal map here does.
Vec3 encodeDirection(Vec3 v) { return v * 0.5f + Vec3{0.5f, 0.5f, 0.5f}; }

// The box an ObjectPosition bake rescales over, accumulated over the LIVE
// vertices of both meshes and IN the requested axis convention. Both meshes,
// because the map writes the high-poly hit where the cage ray lands and the
// low-poly's own point where it misses, so a box around either one alone would
// clamp real texels. `valid` is false when neither mesh has a vertex.
struct BakeBounds {
    Vec3 min{};
    Vec3 max{};
    bool valid = false;

    void add(Vec3 p) {
        if (!valid) {
            min = p;
            max = p;
            valid = true;
            return;
        }
        min = Vec3{std::fmin(min.x, p.x), std::fmin(min.y, p.y), std::fmin(min.z, p.z)};
        max = Vec3{std::fmax(max.x, p.x), std::fmax(max.y, p.y), std::fmax(max.z, p.z)};
    }
};

void accumulateBounds(const Mesh& mesh, UpAxis axis, BakeBounds& bounds) {
    for (Index vi = 0; vi < mesh.vertexCapacity(); ++vi) {
        const VertexId v{vi};
        if (!mesh.isAlive(v)) {
            continue;
        }
        const Vec3 p = toUpAxis(mesh.position(v), axis);
        if (isFinite(p)) {
            bounds.add(p);
        }
    }
}

// Rescales one object-space coordinate into [0,1] over `bounds`. An axis of zero
// extent -- a flat plate, or a Target with a single vertex -- takes the midpoint
// instead of dividing by zero, which is the only value on that axis anyway.
//
// The clamp is belt and braces and NO test can reach it: `bounds` is the union
// of both meshes' live vertices, and every value written is either a point on a
// Target triangle or a point interpolated across an EditMesh triangle, so it is
// inside by construction. It stands against float rounding at the boundary
// (an exact-corner texel landing at 1.0000001), not against a real out-of-box
// coordinate -- do not read its presence as coverage of one.
Vec3 encodePosition(Vec3 p, const BakeBounds& bounds) {
    const auto axis = [](float value, float lo, float hi) {
        const float extent = hi - lo;
        return extent > 0.0f ? std::clamp((value - lo) / extent, 0.0f, 1.0f) : 0.5f;
    };
    return Vec3{axis(p.x, bounds.min.x, bounds.max.x), axis(p.y, bounds.min.y, bounds.max.y),
                axis(p.z, bounds.min.z, bounds.max.z)};
}

// The Target's per-face id column for an id map, what it was read from, and the
// table the bake reports. Empty for every other map.
struct IdField {
    std::string source;
    std::vector<std::int32_t> byFace;  // indexed by FaceId::value
    std::vector<IdColorEntry> table;   // ascending by id
};

// Face-connected components as ids. Deterministic by construction: islands()
// seeds in ascending face order, sorts each island and touches no unordered
// container, so the numbering cannot differ between standard libraries the way
// a hash-ordered traversal would.
std::vector<std::int32_t> componentIds(const Mesh& mesh) {
    std::vector<std::int32_t> ids(mesh.faceCapacity(), 0);
    const std::vector<std::vector<FaceId>> islands = mesh.islands();
    for (std::size_t i = 0; i < islands.size(); ++i) {
        for (const FaceId f : islands[i]) {
            ids[f.value] = static_cast<std::int32_t>(i);
        }
    }
    return ids;
}

// Every distinct id on the Target's LIVE faces, ascending, with the colour each
// one is written as. Every id, not only the ones the UV layout happens to show:
// which ids reach a texel is a property of the layout, and a consumer resolving
// a picked colour needs the whole key. Sorted with std::sort rather than
// gathered from a set, so the order is the ids' own and not a container's.
std::vector<IdColorEntry> idTable(const Mesh& mesh, const std::vector<std::int32_t>& byFace) {
    std::vector<std::int32_t> ids;
    for (Index fi = 0; fi < mesh.faceCapacity(); ++fi) {
        if (mesh.isAlive(FaceId{fi})) {
            ids.push_back(byFace[fi]);
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::vector<IdColorEntry> table;
    table.reserve(ids.size());
    for (const std::int32_t id : ids) {
        table.push_back(IdColorEntry{id, idColor(id)});
    }
    return table;
}

// The documented resolution order: `material_id` for the material map, then
// `object_id` and `group_id` for the object map. `group_id` is kept because it
// is the name the rest of this tree already reads; `object_id` comes first
// because it is the one a host reaches for.
std::array<const char*, 2> idColumnNames(BakeMap map) {
    if (map == BakeMap::MaterialId) {
        return {"material_id", nullptr};
    }
    return {"object_id", "group_id"};
}

IdField gatherIdField(const Mesh& highPoly, BakeMap map) {
    IdField field;
    if (!isIdMap(map)) {
        return field;
    }
    for (const char* name : idColumnNames(map)) {
        const auto* values =
            name == nullptr ? nullptr : highPoly.faceAttributes().find<std::int32_t>(name);
        if (values != nullptr) {
            field.source = name;
            field.byFace = *values;
            break;
        }
    }
    if (field.source.empty()) {
        // Nothing declares an id. An object map still has an answer: a
        // multi-part asset merged into one mesh carries its parts as
        // disconnected components and nowhere else, and no loader in this tree
        // writes an id column, so without this the map would be one flat colour
        // on every real asset. A MATERIAL map has no such fallback -- a
        // component is an object, not a material -- so every face reads 0 and
        // the report says so.
        const bool component = map == BakeMap::ObjectId;
        field.source = component ? "component" : "none";
        field.byFace = component ? componentIds(highPoly)
                                 : std::vector<std::int32_t>(highPoly.faceCapacity(), 0);
    }
    field.byFace.resize(highPoly.faceCapacity(), 0);
    field.table = idTable(highPoly, field.byFace);
    return field;
}

// An id's colour as the float triple the image holds. Exactly `byte / 255`, so
// the 8-bit writer's round trip is the identity and an exact comparison at zero
// tolerance survives the file.
Vec3 idColorValue(std::int32_t id) {
    const std::array<std::uint8_t, 3> c = idColor(id);
    return Vec3{static_cast<float>(c[0]) / 255.0f, static_cast<float>(c[1]) / 255.0f,
                static_cast<float>(c[2]) / 255.0f};
}

// What the pixels of `map` mean, recorded with the image so a consumer never has
// to infer it from the map's name (surface-baking spec, "Baked maps record their
// encoding basis").
BakeEncoding encodingFor(BakeMap map, const BakeParams& params, const BakeBounds& bounds,
                         const IdField& ids) {
    BakeEncoding encoding;
    encoding.upAxis = params.upAxis;
    // The range the map's own encoding guarantees. Declared here, beside the
    // basis, so a map's channel semantics travel with it -- border padding
    // reads this record rather than switching on the map's name, and a map type
    // added later is bounded correctly as long as it declares honestly. The
    // default (infinite) means "this encoding guarantees no range", which is the
    // truth for a position in model units, a signed displacement and a colour
    // copied verbatim off the Target.
    const auto unitRange = [&encoding]() {
        encoding.valueMin = 0.0f;
        encoding.valueMax = 1.0f;
    };
    switch (map) {
        case BakeMap::Normal:
            encoding.basis = EncodingBasis::TangentNormal;
            unitRange();  // n * 0.5 + 0.5 of a unit direction
            break;
        case BakeMap::BentNormal:
            encoding.basis = params.bentNormalSpace == NormalSpace::Object
                                 ? EncodingBasis::ObjectNormal
                                 : EncodingBasis::TangentNormal;
            unitRange();
            break;
        case BakeMap::ObjectNormal:
            encoding.basis = EncodingBasis::ObjectNormal;
            unitRange();
            break;
        case BakeMap::ObjectPosition:
            encoding.basis = EncodingBasis::ObjectBounds;
            encoding.boundsMin = bounds.min;
            encoding.boundsMax = bounds.max;
            // (p - min) / (max - min): the contract of the map is that every
            // texel is inside the box it records.
            unitRange();
            break;
        case BakeMap::Displacement:
            encoding.basis = EncodingBasis::Distance;
            break;  // signed, in model units: no range to guarantee
        case BakeMap::Thickness:
            encoding.basis = EncodingBasis::Distance;
            encoding.scale = params.thicknessScale;
            // A ray that hits nothing, or that hits a front face, contributes
            // zero -- it never entered material. A negative thickness is not a
            // thinner solid, it is a distance that ran backwards.
            encoding.valueMin = 0.0f;
            break;
        case BakeMap::MaterialId:
        case BakeMap::ObjectId:
            encoding.basis = EncodingBasis::IdColor;
            encoding.idSource = ids.source;
            encoding.idColors = ids.table;
            unitRange();  // channel / 255
            break;
        case BakeMap::AmbientOcclusion:
            unitRange();  // a fraction of the hemisphere that is open
            break;
        case BakeMap::Curvature:
        case BakeMap::Cavity:
            unitRange();  // encodeCurvature() clamps into [0,1]
            break;
        case BakeMap::Position:
        case BakeMap::Color:
            break;  // model units / the Target's own colour: unbounded
    }
    return encoding;
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

// The value an UNCOVERED texel carries, per map. Zero is not neutral for most
// maps: it bleeds into the surface under dilation or mip generation as a
// phantom crease (curvature/cavity) or an occlusion ring (AO), and for Normal a
// DirectX preset's green flip would turn (0,0,0) into pure green, so one bake
// shipped two different paddings depending on the target app. Each value is
// what a missed cage ray already writes for that map, which is what makes the
// padding continuous with the covered texels at a chart border.
std::array<float, 3> neutralPadding(BakeMap map, const BakeParams& params) {
    switch (map) {
        case BakeMap::Normal:
            return {0.5f, 0.5f, 1.0f};  // flat tangent normal, invariant under the green flip
        case BakeMap::BentNormal:
            // A bent normal is a normal, so it pads the way its own frame's flat
            // normal encodes: (0,0,1) in tangent space, and the zero vector in
            // object space, where no direction is neutral.
            return params.bentNormalSpace == NormalSpace::Object
                       ? std::array<float, 3>{0.5f, 0.5f, 0.5f}
                       : std::array<float, 3>{0.5f, 0.5f, 1.0f};
        case BakeMap::ObjectNormal:
            return {0.5f, 0.5f, 0.5f};  // the zero vector: object space has no flat normal
        case BakeMap::ObjectPosition:
            return {0.5f, 0.5f, 0.5f};  // the centre of the bake bounds, not a corner
        case BakeMap::Curvature:
            return {0.5f, 0.0f, 0.0f};   // encodeCurvature(0, range, false), range-independent
        case BakeMap::Cavity:            // no concavity
        case BakeMap::AmbientOcclusion:  // fully open
            return {1.0f, 0.0f, 0.0f};
        case BakeMap::MaterialId:
        case BakeMap::ObjectId:
            // The reserved "no id". idColor() lifts every channel into
            // [64,255], so black is a value no assigned id can take and it is
            // safe to mean "nothing here".
            return {};
        default:
            // Displacement, Position, Color, Thickness: zero already IS the
            // neutral value (no height, no material behind an uncovered texel).
            return {};
    }
}

// A finite, non-negative distance or factor: the shape every length-like bake
// parameter has to have before it seeds a ray or scales a texel.
bool usableDistance(float value) { return std::isfinite(value) && value >= 0.0f; }

// The image the bake would allocate: positive, and inside both the arithmetic
// limit and the host's texel ceiling.
bool imageUsable(const BakeParams& params) {
    if (params.width <= 0 || params.height <= 0) {
        return false;
    }
    const std::size_t width = static_cast<std::size_t>(params.width);
    const std::size_t height = static_cast<std::size_t>(params.height);
    return width <= std::numeric_limits<std::size_t>::max() / height &&
           (params.maxPixels == 0 || width <= params.maxPixels / height);
}

// The hemisphere budget the ray-traced maps spend. Bent normal and thickness
// fire the SAME hemisphere AO does, so they read the same budget, radius and
// bias and take the same rejection. Only the raycast path spends a ray budget;
// the field path asks the evaluator for occlusion directly and never divides
// by it.
bool hemisphereUsable(const BakeParams& params, bool useField) {
    return usableDistance(params.aoRadius) && std::isfinite(params.aoBias) &&
           (useField || params.aoSamples > 0);
}

// Every numeric parameter the requested bake actually reads must be in range
// before a single ray is fired. aoSamples is the one that bites: openness is
// `1 - occluded / aoSamples`, so a zero budget is 0/0 at EVERY covered texel and
// the map ships as NaN — which the PNG writer's clamp flattens to solid black,
// the exact inverse of AO's neutral white, with no diagnostic anywhere. The
// distances are the same class: a non-finite cage or radius seeds rays that can
// never hit anything, so the "bake" is a blank map dressed as a success.
// Substituting a default instead would hide the caller's bug just as well as the
// NaN did, so a degenerate value takes the same empty-image rejection the
// width/height checks already take. Parameters the requested map never reads
// stay unchecked: a bake that worked before still works.
bool paramsUsable(BakeMap map, const BakeParams& params, bool useField) {
    if (!imageUsable(params) || !usableDistance(params.cageDistance)) {
        return false;
    }
    if (isRayTraced(map) && !hemisphereUsable(params, useField)) {
        return false;
    }
    // A non-finite scale turns every covered texel into NaN, which the PNG
    // writer's clamp flattens to solid black with no diagnostic anywhere -- the
    // same failure the aoSamples check above exists for.
    if (map == BakeMap::Thickness && !usableDistance(params.thicknessScale)) {
        return false;
    }
    // A negative padding radius is a caller bug, not a request for the
    // default: substituting one would hide it exactly as a substituted
    // aoSamples would. Zero is the documented way to turn padding off.
    if (params.paddingRadius < 0) {
        return false;
    }
    const bool curvatureMap = map == BakeMap::Curvature || map == BakeMap::Cavity;
    return !curvatureMap || std::isfinite(params.curvatureRange);
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

// Where a hemisphere bake anchors its rays: the cage projection onto the
// high-poly, the same cage ray normal/displacement use, so occlusion captures
// the high-poly's crevices rather than the low-poly's smooth surface. Falls back
// to the low-poly frame when the projection misses the cage.
struct Anchor {
    Vec3 position;
    Vec3 normal;
};

Anchor projectToTarget(const Bvh& bvh, const Mesh& highPoly, const std::vector<Vec3>& highNormals,
                       const Texel& tx, const BakeParams& params) {
    const Vec3 origin = tx.position + tx.normal * params.cageDistance;
    const std::optional<Bvh::RayHit> proj = bvh.raycast(origin, tx.normal * -1.0f);
    const bool valid = proj.has_value() && proj->t <= 2.0f * params.cageDistance;
    return {valid ? proj->point : tx.position,
            valid ? hitNormal(highPoly, *proj, highNormals) : tx.normal};
}

// Distance from a hemisphere ray's origin to where it LEAVES the solid: the
// first BACK-FACING hit within the radius. A ray that hits nothing, or that hits
// a FRONT face, never was inside material and contributes 0 -- which is what
// makes a thin double-sided Target read near zero instead of solid white
// (surface-baking spec, "A thin double-sided surface reads near zero"). Reading
// a miss as "maximally thick" would paint every open sheet solid.
float backFacingDepth(const Mesh& mesh, const std::optional<Bvh::RayHit>& hit, Vec3 dir,
                      float radius) {
    if (!hit.has_value() || hit->t > radius) {
        return 0.0f;
    }
    return dot(dir, mesh.faceNormal(hit->face)) > 0.0f ? hit->t : 0.0f;
}

// What one texel's hemisphere of cosine-weighted rays saw. AO reads `occluded`,
// the bent normal reads `openSum` and thickness reads `depthSum`: one sampling
// pass, so the three maps cannot drift apart in sampling, rotation or cage.
struct Hemisphere {
    int occluded = 0;       // hits within aoRadius
    Vec3 openSum;           // sum of the directions that hit nothing
    float depthSum = 0.0f;  // sum of backFacingDepth over the batch
};

Hemisphere gatherHemisphere(const FlatBvh& flat, const Mesh& highPoly, const Anchor& anchor,
                            Vec3 axis, bool wantDepth, const Texel& tx, const BakeParams& params,
                            accel::IBackend& backend) {
    const Vec3 tangent = anyTangent(axis);
    const Vec3 bitangent = cross(axis, tangent);
    const Vec2 rot = texelRotation(tx.px, tx.py);
    const auto count = static_cast<std::size_t>(params.aoSamples);

    accel::Buffer<Vec3> origins(count);
    accel::Buffer<Vec3> dirs(count);
    for (std::size_t k = 0; k < count; ++k) {
        origins[k] = anchor.position + axis * params.aoBias;
        dirs[k] = hemisphereDir(k, count, rot, tangent, bitangent, axis);
    }
    accel::Buffer<std::optional<Bvh::RayHit>> hits;
    accel::raycast(backend, flat, origins, dirs, hits);

    Hemisphere gathered;
    for (std::size_t k = 0; k < hits.size(); ++k) {
        if (hits[k].has_value() && hits[k]->t <= params.aoRadius) {
            ++gathered.occluded;
        } else {
            gathered.openSum += dirs[k];
        }
        if (wantDepth) {
            gathered.depthSum += backFacingDepth(highPoly, hits[k], dirs[k], params.aoRadius);
        }
    }
    return gathered;
}

// One texel's value on the ray-traced path. AO and thickness put their scalar in
// channel 0; the bent normal fills all three.
Vec3 shadeRayTracedTexel(const Hemisphere& gathered, const Anchor& anchor, const Texel& tx,
                         BakeMap map, const BakeParams& params) {
    const auto budget = static_cast<float>(params.aoSamples);
    if (map == BakeMap::AmbientOcclusion) {
        return Vec3{1.0f - static_cast<float>(gathered.occluded) / budget, 0.0f, 0.0f};
    }
    if (map == BakeMap::Thickness) {
        return Vec3{gathered.depthSum / budget * params.thicknessScale, 0.0f, 0.0f};
    }
    // Bent normal. A texel with no unoccluded direction at all is fully
    // enclosed; the surface normal is the honest answer there, not a zero vector
    // normalized into whatever the float arithmetic happens to produce.
    const Vec3 bent =
        length(gathered.openSum) > 0.0f ? normalized(gathered.openSum) : anchor.normal;
    if (params.bentNormalSpace == NormalSpace::Object) {
        return encodeDirection(toUpAxis(bent, params.upAxis));
    }
    return encodeDirection(
        Vec3{dot(bent, tx.tangent), dot(bent, tx.bitangent), dot(bent, tx.normal)});
}

// Serialises progress out of the texel loop. ProgressSink merges values
// monotonically, but it hands the HOST callback straight through and a host
// callback carries no thread-safety contract of its own; the texel loop runs on
// every worker. Reported on a 1% step so a 4096-square bake does not spend its
// time in the host's callback.
class TexelProgress {
public:
    TexelProgress(ProgressSink* sink, std::size_t total)
        : m_sink(sink), m_total(total), m_stride(std::max<std::size_t>(1, total / 100)) {}

    void step() {
        if (m_sink == nullptr || m_total == 0) {
            return;
        }
        const std::size_t done = m_done.fetch_add(1, std::memory_order_relaxed) + 1;
        if (done % m_stride != 0 && done != m_total) {
            return;
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_sink->report(static_cast<float>(done) / static_cast<float>(m_total), "bake");
    }

private:
    ProgressSink* m_sink;
    std::size_t m_total;
    std::size_t m_stride;
    std::atomic<std::size_t> m_done{0};
    std::mutex m_mutex;
};

// The ray-traced shading pass (AO, bent normal, thickness). A hemisphere per
// texel is the only per-texel work in the bake heavy enough to be worth threads,
// so the parallelism lives HERE, over texels: a texel's own ray batch is a few
// dozen items, far too short to pay for a fan-out of its own (it used to spawn
// and join one set of workers per texel). Results go to a scratch vector and
// reach the image afterwards in texel order, so overlapping texels keep
// last-write-wins and the map is bit-identical to a serial bake.
void shadeRayTraced(BakeResult& result, const std::vector<Texel>& texels, const Mesh& highPoly,
                    const Bvh& bvh, const std::vector<Vec3>& highNormals, BakeMap map,
                    const BakeParams& params, ProgressSink* progress, const CancelToken* cancel) {
    // Flattened once for the whole pass: FlatBvh is a full copy of every node
    // and triangle, so rebuilding it per texel made the bake cost linear in the
    // Target's triangle count — the one axis a bake should be flat in.
    const FlatBvh flat = bvh.flatten();
    auto& backend = *accel::defaultBackend();
    const bool wantDepth = map == BakeMap::Thickness;
    const bool rgb = channelsFor(map) == 3;
    std::vector<Vec3> shaded(texels.size(), Vec3{});
    std::atomic<bool> cancelled{false};
    TexelProgress reporter(progress, texels.size());
    backend.parallelFor(0, texels.size(), [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            // Offset by `lo` so every chunk polls its first texel: a chunk can
            // be shorter than the stride.
            if (cancel != nullptr && (i - lo) % kCancelStride == 0 && cancel->isCancelled()) {
                cancelled.store(true, std::memory_order_relaxed);
                return;
            }
            const Anchor anchor = projectToTarget(bvh, highPoly, highNormals, texels[i], params);
            // Thickness looks the other way: INTO the material, not away from it.
            const Vec3 axis = wantDepth ? anchor.normal * -1.0f : anchor.normal;
            const Hemisphere gathered = gatherHemisphere(flat, highPoly, anchor, axis, wantDepth,
                                                         texels[i], params, backend);
            shaded[i] = shadeRayTracedTexel(gathered, anchor, texels[i], map, params);
            reporter.step();
        }
    });
    if (cancelled.load(std::memory_order_relaxed)) {
        result.cancelled = true;
        return;
    }
    for (std::size_t i = 0; i < texels.size(); ++i) {
        result.image.at(texels[i].px, texels[i].py, 0) = shaded[i].x;
        if (rgb) {
            result.image.at(texels[i].px, texels[i].py, 1) = shaded[i].y;
            result.image.at(texels[i].px, texels[i].py, 2) = shaded[i].z;
        }
    }
}

// The raycast shading pass. Its arithmetic is deliberately untouched by the
// field-evaluator work: `params.field == nullptr` must reproduce the pre-bridge
// pixels bit for bit (pipeline-bridge spec, "No evaluator, no behavior change"),
// which tests/bake/test_field_bake.cpp pins against captured checksums.
// Everything the rasterized maps read besides the texel and its own cage hit.
// Gathered once, outside the texel loop: a Target's curvature field and color
// source do not change from texel to texel.
struct RasterSources {
    const Mesh& highPoly;
    const std::vector<Vec3>& highNormals;
    const BakeBounds& bounds;
    // The Target's per-face ids for an id map; `byFace` is empty for every
    // other map, which never reads it.
    const IdField& ids;
    const std::vector<Vec3>* colors = nullptr;
    const std::vector<Vec2>* highUvs = nullptr;
    // Usable only when a texture and Target UVs both exist; otherwise the Color
    // bake falls back to Target vertex colors.
    bool useTexture = false;
    std::vector<float> curvature;
    float curvatureRange = 0.0f;
};

RasterSources gatherRasterSources(const Mesh& highPoly, const std::vector<Vec3>& highNormals,
                                  const BakeBounds& bounds, const IdField& ids, BakeMap map,
                                  const BakeParams& params) {
    const std::vector<Vec3>* colors = highPoly.vertexAttributes().find<Vec3>(io::kColorAttribute);
    const std::vector<Vec2>* highUvs = highPoly.cornerAttributes().find<Vec2>(io::kUvAttribute);
    // Every member is named: a designated-initializer list that stops early is
    // -Wmissing-field-initializers under GCC even where the skipped members
    // carry default initializers, and this tree builds with -Werror.
    RasterSources sources{.highPoly = highPoly,
                          .highNormals = highNormals,
                          .bounds = bounds,
                          .ids = ids,
                          .colors = colors,
                          .highUvs = highUvs,
                          .useTexture = params.colorSource.kind == ColorSource::Texture &&
                                        params.colorSource.texture != nullptr && highUvs != nullptr,
                          .curvature = {},
                          .curvatureRange = 0.0f};
    // Curvature/cavity read the Target's curvature field at the same cage hit
    // the normal bake uses, so the two maps register texel for texel.
    if (map == BakeMap::Curvature || map == BakeMap::Cavity) {
        // The auto range is a percentile over SURFACE, not over vertices: a
        // dense sliver fan (a UV sphere's poles) must not set the range for the
        // whole model just because it owns a lot of vertices.
        std::vector<float> highAreas;
        sources.curvature = vertexMeanCurvature(highPoly, &highAreas);
        sources.curvatureRange = params.curvatureRange > 0.0f
                                     ? params.curvatureRange
                                     : curvatureScale(sources.curvature, highAreas);
    }
    return sources;
}

// The Target's color at a cage hit, from the Target texture when one is usable
// and from Target vertex colors otherwise.
Vec3 targetColor(const RasterSources& src, const Bvh::RayHit& hit, const BakeParams& params) {
    if (src.useTexture) {
        return hitTextureColor(src.highPoly, hit, *src.highUvs, *params.colorSource.texture);
    }
    return hitColor(src.highPoly, hit, src.colors);
}

// One texel's value on the rasterized path, the counterpart of
// shadeRayTracedTexel(): the single-channel maps put their scalar in channel 0
// and the rest fill all three. `hit` is the primary cage projection and `valid`
// says whether it landed inside the cage; every map's MISS value lives here too,
// beside the value it misses.
Vec3 shadeRasterTexel(const RasterSources& src, const Texel& tx,
                      const std::optional<Bvh::RayHit>& hit, bool valid, BakeMap map,
                      const BakeParams& params) {
    // The hit point is two floats' worth of work; the SMOOTH normal is a
    // barycentric blend, so the two maps that read it pay for it and the rest
    // do not.
    const Vec3 point = valid ? hit->point : tx.position;
    switch (map) {
        case BakeMap::Normal: {
            if (!valid) {
                return Vec3{0.5f, 0.5f, 1.0f};
            }
            const Vec3 hn = hitNormal(src.highPoly, *hit, src.highNormals);
            return encodeDirection(
                Vec3{dot(hn, tx.tangent), dot(hn, tx.bitangent), dot(hn, tx.normal)});
        }
        case BakeMap::Displacement:
            // Height of the hit above the low-poly surface along its normal.
            return Vec3{valid ? params.cageDistance - hit->t : 0.0f, 0.0f, 0.0f};
        case BakeMap::Position:
            return point;
        case BakeMap::ObjectPosition:
            // The SAME sample BakeMap::Position writes in model units, only
            // re-expressed and rescaled. Position keeps its meaning; this is a
            // second map, not a redefinition.
            return encodePosition(toUpAxis(point, params.upAxis), src.bounds);
        case BakeMap::ObjectNormal: {
            const Vec3 n = valid ? hitNormal(src.highPoly, *hit, src.highNormals) : tx.normal;
            return encodeDirection(toUpAxis(n, params.upAxis));
        }
        case BakeMap::Color:
            return valid ? targetColor(src, *hit, params) : Vec3{1, 1, 1};
        case BakeMap::MaterialId:
        case BakeMap::ObjectId:
            // The hit FACE's id, flat: no barycentric blend, no anti-aliasing.
            // A blended key is a colour that belongs to neither surface, and it
            // is exactly what makes an exact selection fail on every boundary
            // texel. A missed cage ray writes the reserved "no id" rather than
            // inventing one from the EditMesh, which carries no id column.
            return valid ? idColorValue(src.ids.byFace[hit->face.value]) : Vec3{};
        case BakeMap::Curvature:
        case BakeMap::Cavity: {
            // A missed cage ray contributes 0 curvature, which encodes to the
            // neutral value for both variants (mid-gray / white).
            const float h = valid ? hitScalar(src.highPoly, *hit, src.curvature) : 0.0f;
            return Vec3{encodeCurvature(h, src.curvatureRange, map == BakeMap::Cavity), 0.0f, 0.0f};
        }
        case BakeMap::AmbientOcclusion:
        case BakeMap::BentNormal:
        case BakeMap::Thickness:
            break;  // handled on the ray-traced path
    }
    return Vec3{};
}

void shadeFromMesh(BakeResult& result, const std::vector<Texel>& texels, const Mesh& highPoly,
                   BakeMap map, const BakeParams& params, const BakeBounds& bounds,
                   const IdField& ids, ProgressSink* progress, const CancelToken* cancel) {
    const Bvh bvh(highPoly);
    const std::vector<Vec3> highNormals = vertexNormals(highPoly);
    if (isRayTraced(map)) {
        shadeRayTraced(result, texels, highPoly, bvh, highNormals, map, params, progress, cancel);
        return;
    }
    const RasterSources sources =
        gatherRasterSources(highPoly, highNormals, bounds, ids, map, params);
    const bool rgb = channelsFor(map) == 3;

    for (std::size_t i = 0; i < texels.size(); ++i) {
        if (cancel != nullptr && i % kCancelStride == 0 && cancel->isCancelled()) {
            result.cancelled = true;
            return;
        }
        const Texel& tx = texels[i];

        // Primary projection ray from the cage inward along the surface normal.
        const Vec3 origin = tx.position + tx.normal * params.cageDistance;
        const Vec3 dir = tx.normal * -1.0f;
        const std::optional<Bvh::RayHit> hit = bvh.raycast(origin, dir);
        const bool valid = hit.has_value() && hit->t <= 2.0f * params.cageDistance;

        const Vec3 shaded = shadeRasterTexel(sources, tx, hit, valid, map, params);
        result.image.at(tx.px, tx.py, 0) = shaded.x;
        if (rgb) {
            result.image.at(tx.px, tx.py, 1) = shaded.y;
            result.image.at(tx.px, tx.py, 2) = shaded.z;
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

// ---- the host-callback boundary ------------------------------------------
//
// A FieldEvaluator is HOST code, and its return values are not ours to trust.
// The split below is by "can a CORRECT field return this?", not by convenience.
//
// TIER 1 -- CONTRACT VIOLATION, which no correct field can produce: a NaN
// distance, a non-finite gradient, a non-finite curvature, an openness outside
// [0,1] beyond float slack. The WHOLE BAKE fails, naming the callback and the
// point. Sanitizing these quietly is the fails-open pattern: it conceals the
// host's bug and hands back an image that looks like a real map. paramsUsable()
// already takes this position for a non-finite BakeParams member, and its
// comment is the precedent -- substituting a default "would hide the caller's
// bug just as well as the NaN did".
//
// TIER 2 -- LEGITIMATELY UNDEFINED, which a correct field does hit: an INFINITE
// distance, the ordinary "nothing here" answer from a grid field asked outside
// its domain; and a ZERO-LENGTH gradient, because a signed distance field has
// no gradient on its medial axis and curvature() probes six points OFF the
// surface where that skeleton is reachable. These end the march for one texel,
// which then takes the same neutral padding an un-hit cage ray already writes,
// and are COUNTED rather than hidden.
//
// Why NaN is Tier 1 while infinity is Tier 2, which looks arbitrary and is not:
// std::fmax(NaN, epsilon) returns epsilon, so a NaN distance never even stopped
// the march. It burned all 96 steps for every texel and arrived at a laundered
// miss indistinguishable from empty space. Infinity, by contrast, exits the
// march honestly through `t <= maxT`.
class GuardedField {
public:
    explicit GuardedField(const FieldEvaluator& field) : m_field(field) {}

    [[nodiscard]] bool violated() const { return m_violated; }
    [[nodiscard]] const std::string& message() const { return m_message; }
    [[nodiscard]] std::size_t undefinedSamples() const { return m_undefined; }

    // Finite distance, or nullopt when the march must end for this texel.
    [[nodiscard]] std::optional<float> distance(Vec3 p) {
        const float d = m_field.distance(p);
        if (std::isnan(d)) {
            violate("distance", p, "returned NaN");
            return std::nullopt;
        }
        if (std::isinf(d)) {
            ++m_undefined;
            return std::nullopt;
        }
        return d;
    }

    // Unit normal, or nullopt when the field has no gradient there.
    [[nodiscard]] std::optional<Vec3> unitGradient(Vec3 p) {
        const Vec3 g = m_field.gradient(p);
        if (!isFinite(g)) {
            violate("gradient", p, "returned a non-finite vector");
            return std::nullopt;
        }
        if (!(length(g) > 0.0f)) {
            ++m_undefined;  // the medial axis: undefined, not wrong
            return std::nullopt;
        }
        return normalized(g);
    }

    [[nodiscard]] float openness(Vec3 p, Vec3 n, float radius) {
        const float o = m_field.openness(p, n, radius);
        if (!std::isfinite(o)) {
            violate("openness", p, "returned a non-finite value");
            return 0.0f;
        }
        // Was a silent clamp. A value far outside [0,1] is not float slack, it
        // is a host that inverted the convention or returned a distance by
        // mistake -- and openness IS the inverted one, so the plausible-looking
        // failure is exactly the one to refuse. Slack still clamps.
        constexpr float kSlack = 1e-3f;
        if (o < -kSlack || o > 1.0f + kSlack) {
            violate("openness", p, "returned a value outside [0,1]");
            return 0.0f;
        }
        return std::clamp(o, 0.0f, 1.0f);
    }

    [[nodiscard]] float curvature(Vec3 p, float h) {
        const float c = m_field.curvature(p, h);
        if (!std::isfinite(c)) {
            // Reachable through the interface default, which probes gradient()
            // six times OFF the surface and now propagates a non-finite probe
            // instead of letting normalized() launder it to {0,0,0}.
            violate("curvature", p, "returned a non-finite value");
            return 0.0f;
        }
        return c;
    }

private:
    void violate(const char* callback, Vec3 p, const char* what) {
        if (m_violated) {
            return;  // first offence wins; it is the cause, the rest are echoes
        }
        m_violated = true;
        m_message = std::string("field evaluator contract violated: ") + callback + " " + what +
                    " at (" + std::to_string(p.x) + ", " + std::to_string(p.y) + ", " +
                    std::to_string(p.z) + ")";
    }

    const FieldEvaluator& m_field;
    bool m_violated = false;
    std::string m_message;
    std::size_t m_undefined = 0;
};

FieldHit traceField(GuardedField& field, const Texel& tx, const BakeParams& params) {
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
        const std::optional<float> sample = field.distance(p);
        if (!sample) {
            return {};  // violation, or the field says there is nothing here
        }
        const float d = *sample;
        if (std::fabs(d) <= epsilon) {
            const std::optional<Vec3> n = field.unitGradient(p);
            if (!n) {
                return {};  // no gradient here: a miss, never a fabricated normal
            }
            return {true, p, *n};
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
                    const FieldEvaluator& rawField, BakeMap map, const BakeParams& params,
                    const CancelToken* cancel) {
    GuardedField field(rawField);
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
        // Checked per texel rather than once at the end: a violated contract
        // means every later sample is being taken from a field we have already
        // caught misbehaving, and there is no point paying for them.
        if (field.violated()) {
            result.fieldContractViolated = true;
            result.fieldContractMessage = field.message();
            result.image = Image{};
            result.texelsCovered = 0;
            return;
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
                result.image.at(tx.px, tx.py, 0) =
                    field.openness(p + n * params.aoBias, n, params.aoRadius);
                if (field.violated()) {
                    result.fieldContractViolated = true;
                    result.fieldContractMessage = field.message();
                    result.image = Image{};
                    result.texelsCovered = 0;
                    return;
                }
                break;
            }
            default: {  // Curvature | Cavity — fieldSupports() gates the rest out
                result.image.at(tx.px, tx.py, 0) =
                    encodeCurvature(samples[i], curvatureRange, map == BakeMap::Cavity);
                break;
            }
        }
    }
    result.fieldUndefinedSamples = field.undefinedSamples();
}

}  // namespace

std::array<std::uint8_t, 3> idColor(std::int32_t id) {
    // INTEGER arithmetic end to end. ArmorPaint derives its id colours from
    // frac(sin(dot(id, ...)) * 43758.5453); sin is not correctly rounded and
    // implementations disagree in the last ulp, which that multiply amplifies
    // into a different colour -- exactly the wrong tool for a value that has to
    // be bit-identical on every machine. mixBits() is the Wang-style avalanche
    // already used for the per-texel AO rotation: defined bit for bit by C++'s
    // unsigned arithmetic, and it scatters consecutive ids (0, 1, 2, ...) into
    // unrelated colours because half the output bits flip per input bit.
    //
    // The salt keeps id 0 off the avalanche's fixed point (mixBits(0) == 0).
    const std::uint32_t h = mixBits(static_cast<std::uint32_t>(id) ^ 0x9e3779b9u);
    std::array<std::uint8_t, 3> color{};
    for (std::size_t c = 0; c < 3; ++c) {
        const std::uint32_t byte = (h >> (c * 8u)) & 0xffu;
        // Lifted into [64, 255]: every assigned colour stays legible against a
        // dark background, and (0,0,0) becomes unreachable, which is what makes
        // it safe to reserve for "no id". A sentinel the generator can still
        // emit is a bug that shows up only for particular ids.
        color[c] = static_cast<std::uint8_t>(64u + (byte * 3u) / 4u);
    }
    return color;
}

BakeResult bake(const Mesh& lowPoly, const Mesh& highPoly, BakeMap map, const BakeParams& params,
                ProgressSink* progress, const CancelToken* cancel) {
    BakeResult result;
    const auto* uvs = lowPoly.cornerAttributes().find<Vec2>(io::kUvAttribute);
    // An evaluator makes the Target mesh optional, but only for the maps the
    // field can actually answer.
    const bool useField = params.field != nullptr && fieldSupports(map);
    if (uvs == nullptr || !paramsUsable(map, params, useField) ||
        (highPoly.faceCount() == 0 && !useField)) {
        return result;  // empty image: nothing to bake
    }

    // The box ObjectPosition rescales over, and the basis every map records.
    // Both meshes, because the map writes the high-poly hit where the cage ray
    // lands and the low-poly's own point where it misses.
    BakeBounds bounds;
    if (map == BakeMap::ObjectPosition) {
        accumulateBounds(lowPoly, params.upAxis, bounds);
        accumulateBounds(highPoly, params.upAxis, bounds);
    }
    // The Target's id column and the table an id map reports. Empty, and free,
    // for every other map.
    const IdField ids = gatherIdField(highPoly, map);
    result.encoding = encodingFor(map, params, bounds, ids);

    const std::vector<Vec3> lowNormals = vertexNormals(lowPoly);
    const std::vector<Texel> texels =
        rasterize(lowPoly, lowNormals, *uvs, params.width, params.height);
    result.texelsCovered = texels.size();
    result.image = makeImage(params.width, params.height, channelsFor(map));
    const std::array<float, 3> padding = neutralPadding(map, params);
    for (int y = 0; y < result.image.height; ++y) {
        for (int x = 0; x < result.image.width; ++x) {
            for (int c = 0; c < result.image.channels; ++c) {
                result.image.at(x, y, c) = padding[static_cast<std::size_t>(c)];
            }
        }
    }

    if (useField) {
        shadeFromField(result, texels, *params.field, map, params, cancel);
    } else {
        shadeFromMesh(result, texels, highPoly, map, params, bounds, ids, progress, cancel);
    }
    if (result.cancelled) {
        return result;
    }

    // Padding runs LAST, on the finished texels of either path, so both get the
    // same band from the same code. An abandoned bake (a violated field
    // contract) has an empty image and padBorders() leaves it alone.
    std::vector<detail::PadCoord> covered;
    covered.reserve(texels.size());
    for (const Texel& tx : texels) {
        covered.push_back(detail::PadCoord{tx.px, tx.py});
    }
    const detail::PadOutcome padded =
        detail::padBorders(result.image, covered, result.encoding, params.paddingRadius, cancel);
    result.padding = padded.padding;
    if (padded.cancelled) {
        result.cancelled = true;
        return result;
    }

    if (progress != nullptr) {
        progress->report(1.0f, "bake");
    }
    return result;
}

}  // namespace cyber::bake
