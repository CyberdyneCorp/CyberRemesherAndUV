#pragma once

// Toolchain-stable angle arithmetic.
//
// libm's sinf/cosf are NOT correctly rounded, and the implementations disagree by
// one ULP between toolchains -- measured on ~1.2% of inputs for Apple Clang vs GCC
// on the same machine. atan2, sqrt and hypot agree exactly.
//
// That single ULP does not stay small. The cross field feeds discrete decisions
// (singularity placement, isoline seeding) that flip on it, and the same source
// then produces a visibly different mesh per toolchain -- an open paraboloid came
// out 144 quads under GCC and 224 under Clang from identical input.
//
// So angle round-trips use normalisation and the double- and half-angle
// identities, which need only multiplication and sqrt, and threshold cosines are
// evaluated in double. Both are bit-identical across toolchains, and the former is
// cheaper than the transcendentals it replaces. Keep it that way: reintroducing
// sinf/cosf on these paths reintroduces the divergence.

#include <cmath>

namespace cyber::remesh::stable {

inline constexpr double kPiOver180 = 3.14159265358979323846 / 180.0;

// cos of an angle in degrees. Threshold cosines gate comparisons against dot
// products, where a one-ULP shift can flip a decision; cos(double) agrees across
// toolchains where cosf does not.
inline float cosDegrees(double degrees) {
    return static_cast<float>(std::cos(degrees * kPiOver180));
}

// The inverse, in radians, for code that compares an angle rather than its cosine.
inline float acosRadians(double cosine) { return static_cast<float>(std::acos(cosine)); }

struct CosSin {
    float c;
    float s;
};

// (cos 2t, sin 2t) from a unit (cos t, sin t).
inline CosSin doubleAngle(CosSin u) { return {u.c * u.c - u.s * u.s, 2.0f * u.c * u.s}; }

// (cos t/2, sin t/2) from a unit (cos t, sin t). atan2's principal branch puts t
// in (-pi, pi], so t/2 lands in (-pi/2, pi/2] and cos(t/2) is never negative.
inline CosSin halfAngle(CosSin u) {
    const float c2 = std::sqrt(std::fmax(0.0f, (1.0f + u.c) * 0.5f));
    if (c2 > 0.5f) {
        // t away from +-pi: recovering sin from the chord keeps the small-angle
        // precision that sqrt((1 - c)/2) loses to cancellation when c is near 1.
        return {c2, u.s / (2.0f * c2)};
    }
    const float s2 = std::sqrt(std::fmax(0.0f, (1.0f - u.c) * 0.5f));
    return {c2, u.s < 0.0f ? -s2 : s2};
}

// (cos 4t, sin 4t) where t is the angle of (x, y) -- replaces cos/sin(4*atan2(y, x)).
// A degenerate vector yields the identity cross, matching atan2(0, 0) == 0.
inline CosSin cross4(float x, float y) {
    const float r = std::hypot(x, y);
    if (!(r > 0.0f)) {
        return {1.0f, 0.0f};
    }
    return doubleAngle(doubleAngle({x / r, y / r}));
}

// (cos 4(a-b), sin 4(a-b)) for the angles of (xa, ya) and (xb, yb): the quotient of
// the two unit complex numbers, before the fourth power.
inline CosSin cross4Delta(float xa, float ya, float xb, float yb) {
    return cross4(xa * xb + ya * yb, ya * xb - xa * yb);
}

// (cos t/4, sin t/4) where t is the angle of (x, y) -- replaces
// cos/sin(atan2(y, x) / 4), the inverse of cross4().
inline CosSin quarter(float x, float y) {
    const float r = std::hypot(x, y);
    if (!(r > 0.0f)) {
        return {1.0f, 0.0f};
    }
    return halfAngle(halfAngle({x / r, y / r}));
}

}  // namespace cyber::remesh::stable
