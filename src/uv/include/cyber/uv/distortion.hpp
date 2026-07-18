#pragma once

#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include "cyber/core/math.hpp"
#include "cyber/core/mesh.hpp"
#include "cyber/uv/common.hpp"

// UV distortion analysis (uv-editing spec, "Distortion visualization"):
// per-face angular (conformal) and area distortion plus flipped-island
// detection, feeding the stretch/shear overlay and orientation indicators.
namespace cyber::uv {

struct FaceDistortion {
    FaceId face;
    // Conformal error in [0, 1): |s1 - s2| / (s1 + s2) of the map Jacobian,
    // averaged over the face's fan triangles. 0 means angle-preserving.
    float angle = 0.0f;
    // Area ratio uvArea / surfaceArea (unnormalized). 0 for a collapsed face.
    float area = 0.0f;
    // True when the face's UV winding is reversed (signed UV area < 0).
    bool flipped = false;
};

struct IslandDistortion {
    std::vector<FaceDistortion> faces;
    float maxAngle = 0.0f;
    float rmsAngle = 0.0f;
    float maxArea = 0.0f;   // largest area ratio
    float minArea = 0.0f;   // smallest area ratio
    // The island as a whole is flipped when its net signed UV area is
    // negative (mirrored layout — spec scenario "Flipped island is visible").
    bool flipped = false;
};

namespace detail {

// Singular values of a 2x2 matrix [[a,b],[c,d]] (closed form), s1 >= s2 >= 0.
inline void singularValues2x2(float a, float b, float c, float d, float& s1, float& s2) {
    const float e = (a + d) * 0.5f;
    const float f = (a - d) * 0.5f;
    const float g = (c + b) * 0.5f;
    const float h = (c - b) * 0.5f;
    const float q = std::sqrt(e * e + h * h);
    const float r = std::sqrt(f * f + g * g);
    s1 = q + r;
    s2 = std::fabs(q - r);
}

}  // namespace detail

// Measures distortion of an island whose faces carry a per-loop "uv"
// attribute. Faces without UVs (or a missing column) yield an empty result.
[[nodiscard]] inline IslandDistortion measureDistortion(const Mesh& mesh,
                                                        std::span<const FaceId> island) {
    IslandDistortion out;
    const std::vector<Vec2>* uv = uvColumn(mesh);
    if (uv == nullptr) {
        return out;
    }

    float angleSumSq = 0.0f;
    std::size_t triCount = 0;
    float signedUvAreaTotal = 0.0f;
    bool haveArea = false;

    for (const FaceId face : island) {
        const std::vector<LoopId> loops = mesh.faceLoops(face);
        if (loops.size() < 3) {
            continue;
        }

        FaceDistortion fd;
        fd.face = face;
        float faceAngle = 0.0f;
        float faceUvArea = 0.0f;    // signed
        float faceSurfArea = 0.0f;  // positive
        std::size_t faceTris = 0;

        const Vec3 p0 = mesh.position(mesh.loopVertex(loops[0]));
        const Vec2 q0 = (*uv)[static_cast<std::size_t>(loops[0].value)];
        for (std::size_t i = 1; i + 1 < loops.size(); ++i) {
            const Vec3 p1 = mesh.position(mesh.loopVertex(loops[i]));
            const Vec3 p2 = mesh.position(mesh.loopVertex(loops[i + 1]));
            const Vec2 q1 = (*uv)[static_cast<std::size_t>(loops[i].value)];
            const Vec2 q2 = (*uv)[static_cast<std::size_t>(loops[i + 1].value)];

            // Embed the surface triangle in a local 2D frame.
            const Vec3 e1 = p1 - p0;
            const Vec3 e2 = p2 - p0;
            const float baseLen = length(e1);
            const Vec3 xAxis = baseLen > 0.0f ? e1 * (1.0f / baseLen) : Vec3{1, 0, 0};
            const Vec3 normal = normalized(cross(e1, e2));
            const Vec3 yAxis = cross(normal, xAxis);
            const Vec2 s1v = {baseLen, 0.0f};
            const Vec2 s2v = {dot(e2, xAxis), dot(e2, yAxis)};

            const float surfArea = 0.5f * (s1v.x * s2v.y - s2v.x * s1v.y);
            const Vec2 du = q1 - q0;
            const Vec2 dv = q2 - q0;
            const float uvArea = 0.5f * (du.x * dv.y - dv.x * du.y);
            faceUvArea += uvArea;
            faceSurfArea += std::fabs(surfArea);

            // Jacobian J = Dq * Dp^{-1}, Dp = [s1v s2v], Dq = [du dv].
            const float detP = s1v.x * s2v.y - s2v.x * s1v.y;
            if (std::fabs(detP) > 1e-20f) {
                const float invDet = 1.0f / detP;
                // Dp^{-1}
                const float ia = s2v.y * invDet, ib = -s2v.x * invDet;
                const float ic = -s1v.y * invDet, id = s1v.x * invDet;
                // J = Dq * Dp^{-1}
                const float ja = du.x * ia + dv.x * ic;
                const float jb = du.x * ib + dv.x * id;
                const float jc = du.y * ia + dv.y * ic;
                const float jd = du.y * ib + dv.y * id;
                float sv1 = 0.0f, sv2 = 0.0f;
                detail::singularValues2x2(ja, jb, jc, jd, sv1, sv2);
                const float denom = sv1 + sv2;
                faceAngle += denom > 0.0f ? (sv1 - sv2) / denom : 0.0f;
                ++faceTris;
            }
        }

        (void)q0;  // silence unused when a face has only degenerate tris
        fd.angle = faceTris > 0 ? faceAngle / static_cast<float>(faceTris) : 0.0f;
        fd.area = faceSurfArea > 0.0f ? std::fabs(faceUvArea) / faceSurfArea : 0.0f;
        fd.flipped = faceUvArea < 0.0f;

        signedUvAreaTotal += faceUvArea;
        angleSumSq += fd.angle * fd.angle;
        triCount += 1;
        haveArea = true;

        out.maxAngle = std::fmax(out.maxAngle, fd.angle);
        if (triCount == 1) {
            out.maxArea = out.minArea = fd.area;
        } else {
            out.maxArea = std::fmax(out.maxArea, fd.area);
            out.minArea = std::fmin(out.minArea, fd.area);
        }
        out.faces.push_back(fd);
    }

    if (triCount > 0) {
        out.rmsAngle = std::sqrt(angleSumSq / static_cast<float>(triCount));
    }
    out.flipped = haveArea && signedUvAreaTotal < 0.0f;
    return out;
}

}  // namespace cyber::uv
