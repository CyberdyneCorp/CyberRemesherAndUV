#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/core/plane.hpp"
#include "cyber/retopo/build_tools.hpp"  // detail::snapAdd — the shared "add a vertex on the Target" helper
#include "cyber/retopo/snapping.hpp"

// Contours (manual-retopology spec, "Advanced build tools"): sample the Target
// with cross-section strokes and loft the resulting rings into a quad tube.
//
// This is the opposite construction to DrawStrip, and the difference is the
// whole reason it needs its own file. In DrawStrip the stroke IS the ribbon's
// spine and the rails are built around it. Here the stroke is a SAMPLING
// GESTURE: it names a cutting plane, and the ring comes from the Target's
// intersection with that plane — which is how the far side of a limb, that the
// artist never drew on and cannot see, ends up in the ring.
//
// Header-only and inline, matching build_tools.hpp.
namespace cyber::retopo {

// One cross-section: a closed ring of points on the Target, in traversal order.
struct ContourRing {
    std::vector<Vec3> points;
    Vec3 centroid{};
    Vec3 normal{};  // ring plane normal, oriented by the traversal direction
};

// What a contour run produced, and what it refused.
struct ContourResult {
    std::vector<FaceId> faces;
    std::vector<VertexId> vertices;  // ring-major: ring i occupies [i*spans, (i+1)*spans)
    std::size_t ringCount = 0;
    // Index of the first stroke that named no usable plane or no Target
    // cross-section; npos when every stroke produced a ring. Reported rather
    // than silently skipped, because a missing ring in the middle of a tube
    // lofts the two rings either side of it into a band that spans the gap and
    // looks deliberate.
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    std::size_t failedStroke = npos;
};

namespace detail {

// Least-squares plane through the stroke samples, by the covariance matrix's
// smallest eigenvector. A stroke drawn across a tube is close to planar by
// construction; FITTING rather than constructing the plane from the endpoints
// and a view direction means a wobbly hand-drawn arc still lands on the
// section the artist meant.
//
// Returns false when the samples span no plane: fewer than three points, or a
// spread so degenerate (a dot, a perfectly straight line) that the normal is
// arbitrary. An arbitrary normal cuts the Target somewhere unrelated to the
// gesture, so this is refused rather than guessed.
inline bool fitStrokePlane(std::span<const Vec3> stroke, Plane& out) {
    if (stroke.size() < 3) {
        return false;
    }
    Vec3 centroid{0.0f, 0.0f, 0.0f};
    for (const Vec3 p : stroke) {
        centroid = centroid + p;
    }
    const float inv = 1.0f / static_cast<float>(stroke.size());
    centroid = centroid * inv;

    // Symmetric covariance, upper triangle.
    float xx = 0.0f, xy = 0.0f, xz = 0.0f, yy = 0.0f, yz = 0.0f, zz = 0.0f;
    for (const Vec3 p : stroke) {
        const Vec3 d = p - centroid;
        xx += d.x * d.x;
        xy += d.x * d.y;
        xz += d.x * d.z;
        yy += d.y * d.y;
        yz += d.y * d.z;
        zz += d.z * d.z;
    }

    // The smallest-eigenvector direction, without an eigensolver: each axis'
    // cofactor row gives a candidate normal, and the one with the largest
    // determinant is the numerically best conditioned. This is the standard
    // robust plane fit and it degrades gracefully on a near-linear stroke,
    // which the caller then rejects on the spread test below.
    const float detX = yy * zz - yz * yz;
    const float detY = xx * zz - xz * xz;
    const float detZ = xx * yy - xy * xy;
    const float detMax = std::max({detX, detY, detZ});
    if (!(detMax > 0.0f)) {
        return false;
    }
    Vec3 n;
    if (detMax == detX) {
        n = Vec3{detX, xz * yz - xy * zz, xy * yz - xz * yy};
    } else if (detMax == detY) {
        n = Vec3{xz * yz - xy * zz, detY, xy * xz - yz * xx};
    } else {
        n = Vec3{xy * yz - xz * yy, xy * xz - yz * xx, detZ};
    }
    const float len2 = lengthSquared(n);
    if (!(len2 > 0.0f) || !std::isfinite(len2)) {
        return false;
    }

    // A straight stroke fits every plane through its line equally well, so the
    // fit above returns one of them arbitrarily. Reject when the in-plane
    // spread is not genuinely two-dimensional: compare the smallest principal
    // spread against the largest. trace - (spread along n) is the in-plane
    // spread; if the out-of-plane residual is a large fraction of it the
    // samples are not planar, and if the samples are collinear the cross
    // products above are near zero and the residual test does not catch it.
    const Vec3 nn = normalized(n);
    float residual = 0.0f;
    float spread = 0.0f;
    Vec3 axis{0.0f, 0.0f, 0.0f};
    for (const Vec3 p : stroke) {
        const Vec3 d = p - centroid;
        const float off = dot(d, nn);
        residual += off * off;
        spread += lengthSquared(d);
        if (lengthSquared(d) > lengthSquared(axis)) {
            axis = d;
        }
    }
    if (!(spread > 0.0f)) {
        return false;  // every sample at one point
    }
    // Collinearity: the component of the spread orthogonal to the dominant
    // direction, within the fitted plane.
    if (lengthSquared(axis) > 0.0f) {
        const Vec3 a = normalized(axis);
        float ortho = 0.0f;
        for (const Vec3 p : stroke) {
            const Vec3 d = p - centroid;
            const Vec3 rej = d - a * dot(d, a) - nn * dot(d, nn);
            ortho += lengthSquared(rej);
        }
        if (ortho <= spread * 1e-6f) {
            return false;  // a straight line names no unique plane
        }
    }
    if (residual > spread * 0.5f) {
        return false;  // not a planar gesture at all
    }
    out.point = centroid;
    out.normal = nn;
    return true;
}

// Quantised endpoint key, so two triangles that meet on an edge produce
// segment ends that chain. Plane-cut points are computed per triangle from the
// same two vertex positions and the same interpolant, so they agree to the
// last bit in the common case; the quantisation covers the case where they do
// not (a vertex exactly on the plane, reached from two different triangles).
inline std::uint64_t pointKey(Vec3 p, float scale) {
    const auto q = [scale](float v) {
        return static_cast<std::int64_t>(std::llround(static_cast<double>(v) * scale));
    };
    const std::uint64_t a = static_cast<std::uint64_t>(q(p.x)) & 0x1FFFFFULL;
    const std::uint64_t b = static_cast<std::uint64_t>(q(p.y)) & 0x1FFFFFULL;
    const std::uint64_t c = static_cast<std::uint64_t>(q(p.z)) & 0x1FFFFFULL;
    return a | (b << 21) | (c << 42);
}

// Intersects `target` with `plane`, returning every closed polyline component.
//
// Classifies each face's vertices by signed distance and emits one segment per
// straddling face, then chains segments through shared endpoints. Faces are
// triangulated as a fan for the crossing test, which is exact for the
// triangles a Target actually carries and correct-enough for an n-gon whose
// vertices straddle.
inline std::vector<std::vector<Vec3>> crossSections(const Mesh& target, const Plane& plane) {
    std::vector<std::vector<Vec3>> components;
    if (target.faceCount() == 0) {
        return components;
    }

    // Quantisation scale from the Target's own extent: ~1e-5 of the diagonal,
    // fine enough never to weld two genuinely different crossings and coarse
    // enough to close a seam that differs in the last float bit.
    Vec3 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max()};
    Vec3 hi{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest()};
    for (Index v = 0; v < target.vertexCapacity(); ++v) {
        const VertexId id{v};
        if (!target.isAlive(id)) {
            continue;
        }
        lo = min(lo, target.position(id));
        hi = max(hi, target.position(id));
    }
    const float diag = length(hi - lo);
    if (!(diag > 0.0f)) {
        return components;
    }
    const float scale = 1.0f / (diag * 1e-5f);

    struct Segment {
        Vec3 a, b;
    };
    std::vector<Segment> segments;
    const Vec3 n = normalized(plane.normal);

    for (Index f = 0; f < target.faceCapacity(); ++f) {
        const FaceId face{f};
        if (!target.isAlive(face)) {
            continue;
        }
        const std::vector<VertexId> ring = target.faceVertices(face);
        if (ring.size() < 3) {
            continue;
        }
        // Fan-triangulate and cut each triangle.
        for (std::size_t t = 1; t + 1 < ring.size(); ++t) {
            const std::array<Vec3, 3> p{target.position(ring[0]), target.position(ring[t]),
                                        target.position(ring[t + 1])};
            std::array<float, 3> d{};
            for (int i = 0; i < 3; ++i) {
                d[static_cast<std::size_t>(i)] =
                    dot(p[static_cast<std::size_t>(i)] - plane.point, n);
            }
            Vec3 ends[2];
            int found = 0;
            for (int i = 0; i < 3 && found < 2; ++i) {
                const std::size_t i0 = static_cast<std::size_t>(i);
                const std::size_t i1 = static_cast<std::size_t>((i + 1) % 3);
                const float d0 = d[i0];
                const float d1 = d[i1];
                // A crossing, counting an exactly-on-plane first endpoint once:
                // taking (d0 == 0) as a crossing on THIS edge only and not on
                // the previous one is what keeps a vertex lying in the plane
                // from emitting the same point twice.
                if ((d0 < 0.0f && d1 >= 0.0f) || (d0 >= 0.0f && d1 < 0.0f)) {
                    const float denom = d0 - d1;
                    const float u = denom != 0.0f ? d0 / denom : 0.0f;
                    ends[found++] = lerp(p[i0], p[i1], u);
                }
            }
            if (found == 2) {
                segments.push_back(Segment{ends[0], ends[1]});
            }
        }
    }
    if (segments.empty()) {
        return components;
    }

    // Chain segments through shared endpoints.
    std::unordered_multimap<std::uint64_t, std::size_t> byKey;
    byKey.reserve(segments.size() * 2);
    for (std::size_t i = 0; i < segments.size(); ++i) {
        byKey.emplace(pointKey(segments[i].a, scale), i);
        byKey.emplace(pointKey(segments[i].b, scale), i);
    }
    std::vector<char> used(segments.size(), 0);
    for (std::size_t seed = 0; seed < segments.size(); ++seed) {
        if (used[seed] != 0) {
            continue;
        }
        used[seed] = 1;
        std::vector<Vec3> poly{segments[seed].a, segments[seed].b};
        // Walk forward from the growing end until nothing continues it.
        for (;;) {
            const std::uint64_t key = pointKey(poly.back(), scale);
            const auto range = byKey.equal_range(key);
            std::size_t next = segments.size();
            for (auto it = range.first; it != range.second; ++it) {
                if (used[it->second] == 0) {
                    next = it->second;
                    break;
                }
            }
            if (next == segments.size()) {
                break;
            }
            used[next] = 1;
            const Segment& s = segments[next];
            poly.push_back(pointKey(s.a, scale) == key ? s.b : s.a);
        }
        if (poly.size() >= 3) {
            // Drop a duplicated closing point: the ring is implicitly closed.
            if (pointKey(poly.front(), scale) == pointKey(poly.back(), scale)) {
                poly.pop_back();
            }
            if (poly.size() >= 3) {
                components.push_back(std::move(poly));
            }
        }
    }
    return components;
}

// Resamples a closed polyline to exactly `spans` points at even arc length,
// beginning at arc-length offset `startT` in [0,1).
inline std::vector<Vec3> resampleClosed(std::span<const Vec3> poly, std::size_t spans,
                                        float startT) {
    std::vector<Vec3> out;
    if (poly.size() < 3 || spans < 3) {
        return out;
    }
    std::vector<float> cum(poly.size() + 1, 0.0f);
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const Vec3 a = poly[i];
        const Vec3 b = poly[(i + 1) % poly.size()];
        cum[i + 1] = cum[i] + length(b - a);
    }
    const float total = cum.back();
    if (!(total > 0.0f)) {
        return out;
    }
    out.reserve(spans);
    for (std::size_t k = 0; k < spans; ++k) {
        float s = (startT + static_cast<float>(k) / static_cast<float>(spans)) * total;
        s = std::fmod(s, total);
        if (s < 0.0f) {
            s += total;
        }
        // Segment containing arc length s.
        const auto it = std::upper_bound(cum.begin(), cum.end(), s);
        std::size_t seg = static_cast<std::size_t>(std::distance(cum.begin(), it));
        seg = seg == 0 ? 0 : seg - 1;
        seg = std::min(seg, poly.size() - 1);
        const float segLen = cum[seg + 1] - cum[seg];
        const float u = segLen > 0.0f ? (s - cum[seg]) / segLen : 0.0f;
        out.push_back(lerp(poly[seg], poly[(seg + 1) % poly.size()], u));
    }
    return out;
}

// Newell normal of a closed ring, which carries its traversal direction.
//
// Named contourNormal/contourCentroid, not ringNormal/ringCentroid: adjacency.hpp
// already owns those names for the mesh-topology sense (a vertex's one-ring), and
// a same-named helper in this nested `detail` scope HIDES them from every
// unqualified call in the enclosing namespace rather than overloading with them —
// which is exactly what it did, breaking relax.hpp by include order alone.
inline Vec3 contourNormal(std::span<const Vec3> ring) {
    Vec3 n{0.0f, 0.0f, 0.0f};
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const Vec3 a = ring[i];
        const Vec3 b = ring[(i + 1) % ring.size()];
        n = n +
            Vec3{(a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)};
    }
    return n;
}

inline Vec3 contourCentroid(std::span<const Vec3> ring) {
    Vec3 c{0.0f, 0.0f, 0.0f};
    for (const Vec3 p : ring) {
        c = c + p;
    }
    return ring.empty() ? c : c * (1.0f / static_cast<float>(ring.size()));
}

}  // namespace detail

// Builds ONE contour ring: fits a plane to `stroke`, cuts `target` with it, and
// takes the cross-section component nearest the stroke.
//
// A plane through a limb usually cuts the body somewhere else too, so the cut
// returns several components. The one whose centroid is nearest the stroke's
// own centroid wins — the artist pointed at it. Components are ranked, never
// merged: merging would loft a tube between an arm and a thigh.
//
// Returns false when the stroke names no usable plane or the plane misses the
// Target.
[[nodiscard]] inline bool contourRing(const Mesh& target, std::span<const Vec3> stroke,
                                      std::size_t spans, ContourRing& out) {
    if (spans < 3) {
        return false;
    }
    Plane plane;
    if (!detail::fitStrokePlane(stroke, plane)) {
        return false;
    }
    const std::vector<std::vector<Vec3>> components = detail::crossSections(target, plane);
    if (components.empty()) {
        return false;
    }
    const Vec3 strokeCentroid = detail::contourCentroid(stroke);
    std::size_t best = 0;
    float bestDist = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < components.size(); ++i) {
        const float d = lengthSquared(detail::contourCentroid(components[i]) - strokeCentroid);
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    std::vector<Vec3> resampled = detail::resampleClosed(components[best], spans, 0.0f);
    if (resampled.size() != spans) {
        return false;
    }
    out.centroid = detail::contourCentroid(resampled);
    out.normal = detail::contourNormal(resampled);
    out.points = std::move(resampled);
    return true;
}

// Contours: sample `target` with cross-section `strokes` and loft the rings
// into a quad tube on `mesh`.
//
// `strokes` are world-space stroke samples, one span per stroke, in the order
// the artist drew them — lofting follows that order deliberately. Sorting by
// position along a fitted axis was considered and rejected: it guesses at
// intent, and an artist who draws a ring out of order to close a gap gets a
// tube that reorders itself under them.
//
// Every ring is resampled to the SAME `spans` count, which is what a loft needs
// and also what makes the quads even. Two consistency problems remain, and both
// produce output that looks plausible and is wrong:
//
//   - seam drift: each resampling has an arbitrary start, so ring i+1 starts at
//     the sample nearest ring i's start and the seam runs straight instead of
//     spiralling;
//   - winding flip: a cut can come back in either orientation, so ring i+1 is
//     reversed when its normal opposes ring i's, which keeps one band in the
//     middle of a tube from inverting against its neighbours.
//
// Ring vertices snap to the Target when `snap` is given. `closed` wraps the
// last ring back to the first (a torus). A stroke that produces no ring stops
// the loft and is reported in `failedStroke`, rather than being skipped so the
// rings either side of it loft across the gap.
[[nodiscard]] inline ContourResult contours(Mesh& mesh, const Mesh& target,
                                            std::span<const std::span<const Vec3>> strokes,
                                            std::size_t spans, const SurfaceSnapper* snap = nullptr,
                                            bool closed = false) {
    ContourResult out;
    if (strokes.size() < 2 || spans < 3) {
        return out;
    }

    std::vector<ContourRing> rings;
    rings.reserve(strokes.size());
    for (std::size_t i = 0; i < strokes.size(); ++i) {
        ContourRing ring;
        if (!contourRing(target, strokes[i], spans, ring)) {
            out.failedStroke = i;
            return out;
        }
        rings.push_back(std::move(ring));
    }

    // Align every ring to its predecessor: winding first, then seam. Winding
    // first matters — reversing a ring also reverses which sample sits nearest
    // the predecessor's start, so choosing the seam before the winding picks
    // the wrong one.
    for (std::size_t i = 1; i < rings.size(); ++i) {
        ContourRing& cur = rings[i];
        const ContourRing& prev = rings[i - 1];
        if (dot(cur.normal, prev.normal) < 0.0f) {
            std::reverse(cur.points.begin(), cur.points.end());
            cur.normal = detail::contourNormal(cur.points);
        }
        std::size_t bestOffset = 0;
        float bestDist = std::numeric_limits<float>::max();
        for (std::size_t k = 0; k < cur.points.size(); ++k) {
            const float d = lengthSquared(cur.points[k] - prev.points[0]);
            if (d < bestDist) {
                bestDist = d;
                bestOffset = k;
            }
        }
        if (bestOffset != 0) {
            std::rotate(cur.points.begin(),
                        cur.points.begin() + static_cast<std::ptrdiff_t>(bestOffset),
                        cur.points.end());
        }
    }

    // Materialise ring vertices, ring-major.
    out.vertices.reserve(rings.size() * spans);
    for (const ContourRing& ring : rings) {
        for (const Vec3 p : ring.points) {
            out.vertices.push_back(detail::snapAdd(mesh, p, snap));
        }
    }
    out.ringCount = rings.size();

    // Loft consecutive rings.
    const std::size_t bands = closed ? rings.size() : rings.size() - 1;
    for (std::size_t b = 0; b < bands; ++b) {
        const std::size_t r0 = b * spans;
        const std::size_t r1 = ((b + 1) % rings.size()) * spans;
        for (std::size_t k = 0; k < spans; ++k) {
            const std::size_t k1 = (k + 1) % spans;
            const std::array<VertexId, 4> quad{out.vertices[r0 + k], out.vertices[r0 + k1],
                                               out.vertices[r1 + k1], out.vertices[r1 + k]};
            const FaceId f = mesh.addFace(quad);
            if (f.valid()) {
                out.faces.push_back(f);
            }
        }
    }
    return out;
}

}  // namespace cyber::retopo
