#include <doctest.h>

#include "../../src/uv/src/rejection_memo.hpp"

using cyber::uv::detail::RejectionMemo;

// The merge fixpoint's accept predicate LSCM-unwraps the concatenation `a ++ b`,
// so it is a pure function of the ORDERED pair: swapping the arguments changes
// the linear system and can change the answer. A memo that folded its key to
// min/max would answer one order with the other's rejection and suppress a merge
// that is legal as requested, silently changing the chart partition. Measured on
// a noisy sphere at maxChartAngleDeg 15: folding the key drops one extra chart
// and moves 19 seam edges away from the un-memoised result.

TEST_CASE("a rejection recorded for one chart order does not answer the other") {
    RejectionMemo memo(8);
    memo.reject(5, 2);

    CHECK(memo.isKnownRejected(5, 2));
    CHECK_FALSE(memo.isKnownRejected(2, 5));
}

TEST_CASE("a rejection survives until one of its two charts absorbs another") {
    RejectionMemo memo(8);
    memo.reject(1, 3);
    REQUIRE(memo.isKnownRejected(1, 3));

    SUBCASE("an unrelated chart growing leaves it standing") {
        memo.bumpVersion(6);
        CHECK(memo.isKnownRejected(1, 3));
    }
    SUBCASE("either endpoint growing expires it") {
        SUBCASE("the first") {
            memo.bumpVersion(1);
            CHECK_FALSE(memo.isKnownRejected(1, 3));
        }
        SUBCASE("the second") {
            memo.bumpVersion(3);
            CHECK_FALSE(memo.isKnownRejected(1, 3));
        }
    }
}

TEST_CASE("a pair that was never rejected is not reported as known") {
    RejectionMemo memo(4);
    memo.reject(0, 1);

    CHECK_FALSE(memo.isKnownRejected(2, 3));
    CHECK_FALSE(memo.isKnownRejected(0, 2));
}
