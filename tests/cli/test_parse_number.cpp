#include <doctest.h>

#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "parse_number.hpp"

// The CLI's numeric flag parser. It used std::from_chars for every type, and
// the FLOATING-POINT overloads of std::from_chars do not exist in libc++ — the
// standard library of both mobile targets — so --edge-scale (and every other
// float flag) failed to link the moment the CLI was built for iOS or Android.
//
// The fix keeps std::from_chars where it exists and reimposes its grammar over
// strtof/strtod where it does not. That fallback is the branch no desktop build
// takes, so it is exactly the code that rots silently. These cases compile it
// everywhere and hold it to the reference's verdict, value for value.
namespace {

using cyber::cli::parseNumber;

// The inputs the two paths must agree on: the accepted forms, and each way a
// string can be rejected (empty, junk, trailing junk, leading space, leading
// '+', hex — which from_chars' `general` format has no notion of — and a bare
// sign or point).
const std::vector<std::string> kInputs = {
    "0",
    "1",
    "-1",
    "1.5",
    "-1.5",
    "0.25",
    ".5",
    "1.",
    "1e3",
    "1E3",
    "1e-3",
    "-2e2",
    "0.0",
    "-0.0",
    "123456",
    "3.4e38",
    "1e40",
    "1e-50",
    "",
    " ",
    " 1",
    "1 ",
    "+1",
    "abc",
    "1abc",
    "--1",
    "-",
    ".",
    "0x10",
    "-0x1",
    "0X1p3",
    "1,5",
    "1.2.3",
    "e5",
    "1e",
    "\t2",
    "1e+3",
    "-1e+3",
    "00.5",
    "007",
    "1.0000000000000000001",
};

template <typename T>
std::optional<T> reference(const std::string& text) {
    return cyber::cli::detail::parseFloatingViaFromChars<T>(text);
}

template <typename T>
void checkFallbackMatchesFromChars() {
    for (const std::string& text : kInputs) {
        CAPTURE(text);
        const std::optional<T> expected = reference<T>(text);
        const std::optional<T> actual = cyber::cli::detail::parseFloatingViaStrtod<T>(text);
        CHECK(actual.has_value() == expected.has_value());
        if (expected.has_value() && actual.has_value()) {
            // Bitwise: a flag parsed one ulp differently is a different remesh.
            CHECK(*actual == *expected);
        }
    }
}

}  // namespace

TEST_CASE("the strtod fallback reproduces std::from_chars exactly") {
#if CYBER_CLI_STRTOD_FLOATS
    // This build has no floating-point std::from_chars to compare against, so
    // the reference side would answer nullopt for everything and the case would
    // pass vacuously. The comparison runs on libstdc++ lanes, which is where it
    // has to be right.
    WARN_MESSAGE(false, "no floating-point std::from_chars here; comparison skipped");
#else
    checkFallbackMatchesFromChars<float>();
    checkFallbackMatchesFromChars<double>();
#endif
}

TEST_CASE("parseNumber accepts the CLI's real flag values on every platform") {
    // Whichever path this build takes, these are the values the flags carry.
    REQUIRE(parseNumber<float>("1.0") == doctest::Approx(1.0f));
    REQUIRE(parseNumber<float>("0.5") == doctest::Approx(0.5f));
    REQUIRE(parseNumber<float>("-1.25") == doctest::Approx(-1.25f));
    REQUIRE(parseNumber<float>("2e2") == doctest::Approx(200.0f));
    REQUIRE(parseNumber<double>("45.5") == doctest::Approx(45.5));
    REQUIRE(parseNumber<int>("2000") == 2000);
    REQUIRE(parseNumber<int>("-3") == -3);
}

TEST_CASE("parseNumber rejects rather than silently zeroing") {
    // AutoRemesher turned "abc" into 0. The spec forbids it, for floats too.
    REQUIRE(!parseNumber<float>("abc").has_value());
    REQUIRE(!parseNumber<float>("").has_value());
    REQUIRE(!parseNumber<float>("1abc").has_value());
    REQUIRE(!parseNumber<float>(" 1").has_value());
    REQUIRE(!parseNumber<float>("+1").has_value());
    REQUIRE(!parseNumber<float>("0x10").has_value());
    REQUIRE(!parseNumber<int>("1.5").has_value());
    REQUIRE(!parseNumber<int>("abc").has_value());
}

TEST_CASE("out-of-range values are rejected, not saturated") {
    // std::from_chars reports result_out_of_range instead of handing back
    // HUGE_VALF; the fallback has to match, or --edge-scale 1e40 would silently
    // become infinity on mobile.
    REQUIRE(!parseNumber<float>("1e40").has_value());
    REQUIRE(!parseNumber<int>("99999999999999999999").has_value());
}
