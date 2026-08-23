#include <doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../../src/quadrangulate/src/stable_angle.hpp"

using cyber::remesh::stable::acosRadians;
using cyber::remesh::stable::cosDegrees;
using cyber::remesh::stable::CosSin;
using cyber::remesh::stable::cross4;
using cyber::remesh::stable::cross4Delta;
using cyber::remesh::stable::quarter;

namespace {

// M_PI is a POSIX extension rather than standard C++, and MinGW does not declare
// it under -std=c++20, so spell the constant out.
constexpr double kPi = 3.14159265358979323846;

std::uint32_t bits(float v) {
    std::uint32_t b = 0;
    std::memcpy(&b, &v, sizeof b);
    return b;
}

float fromBits(std::uint32_t b) {
    float v = 0.0f;
    std::memcpy(&v, &b, sizeof v);
    return v;
}

}  // namespace

// These helpers exist so the field and the extractor do not depend on libm's float
// trig, whose implementations differ by one ULP between toolchains (Apple Clang vs
// GCC disagree on ~1.2% of sinf/cosf inputs). The pipeline amplifies one ULP into a
// different mesh, so the values below are pinned as EXACT bit patterns rather than
// compared with a tolerance: an approximate check would pass on exactly the drift
// this guards against. They were verified identical under both toolchains.
//
// A failure here means the helpers stopped being bit-reproducible -- most likely
// because a transcendental crept back onto the path. Do not "fix" it by relaxing to
// a tolerance; find what reintroduced the libm dependency.
TEST_CASE("stable angle helpers are bit-reproducible") {
    struct Row {
        std::uint32_t x, y, c4c, c4s, qc, qs;
    };
    // {x, y, cross4.c, cross4.s, quarter.c, quarter.s}
    const Row rows[] = {
        {0x3f800000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u, 0x00000000u},
        {0xbf000000u, 0x3f400000u, 0xbf3442a6u, 0x3f35c672u, 0x3f5b9ce9u, 0x3f038d89u},
        {0x3e800000u, 0xc0200000u, 0x3f6bec6du, 0x3ec6c1cbu, 0x3f6ee16cu, 0xbeb8167cu},
        {0xc0700000u, 0x3f900000u, 0x3ec9b91eu, 0xbf6b4b18u, 0x3f41b799u, 0x3f275c61u},
        {0x00000000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u, 0x00000000u},
        {0x322bcc77u, 0xb22bcc77u, 0xbf7ffffeu, 0x80000000u, 0x3f7b14bfu, 0xbe47c5c1u},
        {0x41480000u, 0xc0e80000u, 0xbf01c19cu, 0xbf5cade0u, 0x3f7dcb14u, 0xbe0629a1u},
    };
    for (const Row& r : rows) {
        const float x = fromBits(r.x), y = fromBits(r.y);
        const CosSin a = cross4(x, y);
        CHECK(bits(a.c) == r.c4c);
        CHECK(bits(a.s) == r.c4s);
        const CosSin q = quarter(x, y);
        CHECK(bits(q.c) == r.qc);
        CHECK(bits(q.s) == r.qs);
    }

    const double degrees[] = {0.0, 1.0, 20.0, 35.0, 45.0, 90.0, 179.0};
    const std::uint32_t cosExpected[] = {0x3f800000u, 0x3f7ff605u, 0x3f708fb2u, 0x3f51b3f3u,
                                         0x3f3504f3u, 0x248d3132u, 0xbf7ff605u};
    for (std::size_t i = 0; i < 7; ++i) {
        CHECK(bits(cosDegrees(degrees[i])) == cosExpected[i]);
    }

    const double cosines[] = {-1.0, -0.5, 0.0, 0.25, 1.0};
    const std::uint32_t acosExpected[] = {0x40490fdbu, 0x40060a92u, 0x3fc90fdbu, 0x3fa8b807u,
                                          0x00000000u};
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(bits(acosRadians(cosines[i])) == acosExpected[i]);
    }
}

// The pinned bits above are only worth anything if they are the RIGHT numbers, so
// check the helpers against the transcendental forms they replaced. Tolerance here
// is deliberate: this asserts the maths, the test above asserts the bits.
TEST_CASE("stable angle helpers agree with the trig they replaced") {
    for (int i = -60; i <= 60; ++i) {
        for (int j = -60; j <= 60; ++j) {
            const float x = static_cast<float>(i) * 0.37f;
            const float y = static_cast<float>(j) * 0.29f;
            if (x == 0.0f && y == 0.0f) {
                continue;
            }
            const double theta = std::atan2(static_cast<double>(y), static_cast<double>(x));

            const CosSin a = cross4(x, y);
            CHECK(a.c == doctest::Approx(std::cos(4.0 * theta)).epsilon(1e-5));
            CHECK(a.s == doctest::Approx(std::sin(4.0 * theta)).epsilon(1e-5));

            const CosSin q = quarter(x, y);
            CHECK(q.c == doctest::Approx(std::cos(theta / 4.0)).epsilon(1e-5));
            CHECK(q.s == doctest::Approx(std::sin(theta / 4.0)).epsilon(1e-5));
        }
    }
}

TEST_CASE("cross4Delta is the fourth power of the angle difference") {
    for (int i = -24; i <= 24; ++i) {
        for (int j = -24; j <= 24; ++j) {
            const float xa = static_cast<float>(i) * 0.41f + 0.13f;
            const float ya = static_cast<float>(j) * 0.23f - 0.07f;
            const float xb = static_cast<float>(j) * 0.31f - 0.19f;
            const float yb = static_cast<float>(i) * 0.17f + 0.11f;
            const double a = std::atan2(static_cast<double>(ya), static_cast<double>(xa));
            const double b = std::atan2(static_cast<double>(yb), static_cast<double>(xb));
            const CosSin d = cross4Delta(xa, ya, xb, yb);
            CHECK(d.c == doctest::Approx(std::cos(4.0 * (a - b))).epsilon(1e-5));
            CHECK(d.s == doctest::Approx(std::sin(4.0 * (a - b))).epsilon(1e-5));
        }
    }
}

// quarter() undoes cross4() up to the 4-fold symmetry the representation encodes.
TEST_CASE("quarter inverts cross4 up to the cross's 4-fold symmetry") {
    for (int i = 1; i <= 400; ++i) {
        const double theta = (static_cast<double>(i) / 400.0) * (kPi / 2.0) - (kPi / 4.0);
        const CosSin round = quarter(static_cast<float>(std::cos(4.0 * theta)),
                                     static_cast<float>(std::sin(4.0 * theta)));
        CHECK(round.c == doctest::Approx(std::cos(theta)).epsilon(1e-4));
        CHECK(round.s == doctest::Approx(std::sin(theta)).epsilon(1e-4));
    }
}
