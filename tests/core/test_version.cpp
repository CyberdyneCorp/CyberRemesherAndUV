#include <doctest.h>

#include "cyber/core/version.hpp"

TEST_CASE("version is a non-empty semantic version") {
    const auto v = cyber::version();
    REQUIRE(!v.empty());
    // major.minor.patch
    int dots = 0;
    for (const char c : v) {
        if (c == '.') {
            ++dots;
        } else {
            REQUIRE(c >= '0');
            REQUIRE(c <= '9');
        }
    }
    REQUIRE(dots == 2);
}
