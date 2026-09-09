// The C ABI's own version contract.
//
// openspec/specs/engine-bindings/spec.md has required this since the bootstrap
// change -- "The ABI SHALL carry a runtime-queryable semantic version; minor
// releases SHALL be additive only", with a "Scenario: ABI version query" for a
// 1.x client loading a 1.y library. Until CYBER_ABI_VERSION_* existed, that
// scenario could not be written down: there was no ABI version to compile
// against, only the engine's project version, which moves for reasons that
// leave the linkable surface untouched.
#include <doctest.h>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>

#include "cyber_capi.h"

TEST_CASE("the ABI version is reported and is independent of the engine version") {
    int abiMajor = -1;
    int abiMinor = -1;
    cyber_abi_version(&abiMajor, &abiMinor);
    CHECK(abiMajor == CYBER_ABI_VERSION_MAJOR);
    CHECK(abiMinor == CYBER_ABI_VERSION_MINOR);

    int major = -1;
    int minor = -1;
    int patch = -1;
    cyber_version(&major, &minor, &patch);

    // The point of two numbers: the ABI is 1.x while the engine is 0.x, so a
    // test that accidentally compared them would fail. If these ever coincide,
    // this assertion is not what breaks -- but the independence is the contract,
    // and cyber_version must keep meaning the engine.
    CHECK(abiMajor != major);

    // Either pointer may be NULL, like cyber_version's.
    cyber_abi_version(nullptr, nullptr);
    int onlyMinor = -1;
    cyber_abi_version(nullptr, &onlyMinor);
    CHECK(onlyMinor == CYBER_ABI_VERSION_MINOR);
}

TEST_CASE("a client compiled against an earlier ABI minor is served, a later one is refused") {
    // THIS IS THE SPEC'S "Scenario: ABI version query", now expressible: a
    // client compiled against 1.x loading a 1.y (y > x) library keeps working.
    // The library is the y side; the argument is the x the client compiled with.
    CHECK(cyber_abi_check(CYBER_ABI_VERSION_MAJOR, CYBER_ABI_VERSION_MINOR) == CYBER_OK);
    for (int older = 0; older <= CYBER_ABI_VERSION_MINOR; ++older) {
        CHECK(cyber_abi_check(CYBER_ABI_VERSION_MAJOR, older) == CYBER_OK);
    }

    // A client compiled against a LATER minor asked for entry points that
    // genuinely are not here, so serving it would be a lie.
    CHECK(cyber_abi_check(CYBER_ABI_VERSION_MAJOR, CYBER_ABI_VERSION_MINOR + 1) ==
          CYBER_ERR_INCOMPATIBLE_VERSION);

    // A different major is incompatible in BOTH directions -- there is no
    // "newer is fine" for a breaking change.
    CHECK(cyber_abi_check(CYBER_ABI_VERSION_MAJOR + 1, 0) == CYBER_ERR_INCOMPATIBLE_VERSION);
    CHECK(cyber_abi_check(CYBER_ABI_VERSION_MAJOR - 1, 0) == CYBER_ERR_INCOMPATIBLE_VERSION);
}

TEST_CASE("a refused ABI check names both versions and leaves a message") {
    REQUIRE(cyber_abi_check(CYBER_ABI_VERSION_MAJOR + 1, 7) == CYBER_ERR_INCOMPATIBLE_VERSION);
    const std::string message = cyber_last_error();
    // Both sides, because "incompatible" without the numbers sends the host
    // reading our source to find out which way round it was.
    CHECK(message.find(std::to_string(CYBER_ABI_VERSION_MAJOR) + "." +
                       std::to_string(CYBER_ABI_VERSION_MINOR)) != std::string::npos);
    CHECK(message.find(std::to_string(CYBER_ABI_VERSION_MAJOR + 1) + ".7") != std::string::npos);

    // A pass clears it, so a host polling last_error does not see a stale refusal.
    REQUIRE(cyber_abi_check(CYBER_ABI_VERSION_MAJOR, CYBER_ABI_VERSION_MINOR) == CYBER_OK);
    CHECK(std::string(cyber_last_error()).empty());
}

// The sizeof-stride convention is what makes "appending a field is MAJOR" true
// rather than merely stated: callers pass arrays of these and stride by sizeof,
// so a field appended in a minor silently misreads every element after the
// first. Pinned to literals here, deliberately -- an assertion derived from the
// struct could not fail.
//
// This is a PARTIAL guard. It covers the structs where the stride convention is
// load-bearing, not the whole surface; the adversarial review of this change
// killed two designs for a full layout manifest (one recorded only size and
// offset, so a float* -> double* swap was invisible; the other was blind to a
// field dropped into existing trailing padding, which 4 of the 26 structs have).
// A manifest that pins type identity and struct-level padding is follow-up work,
// and until it exists the additive-only-minor promise is enforced by review
// everywhere except right here.
static_assert(sizeof(CyberRemeshParams) == 32, "CyberRemeshParams layout is ABI-frozen");
static_assert(sizeof(CyberFlowGuide) == 24, "CyberFlowGuide layout is ABI-frozen");
static_assert(sizeof(CyberFlowGuideEx) == 32, "CyberFlowGuideEx layout is ABI-frozen");
static_assert(sizeof(CyberGuidance) == 48, "CyberGuidance layout is ABI-frozen");
static_assert(sizeof(CyberZRemesherParams) == 20, "CyberZRemesherParams layout is ABI-frozen");

#ifdef CYBER_CAPI_HEADER_PATH
TEST_CASE("the header states the ABI contract where a consumer reads it") {
    std::ifstream header(CYBER_CAPI_HEADER_PATH);
    REQUIRE(header.good());
    const std::string text((std::istreambuf_iterator<char>(header)),
                           std::istreambuf_iterator<char>());

    // The policy has to live in the header, not only in the spec: an embedder
    // vendoring this repo reads cyber_capi.h and may never open openspec/.
    CHECK(text.find("#define CYBER_ABI_VERSION_MAJOR") != std::string::npos);
    CHECK(text.find("#define CYBER_ABI_VERSION_MINOR") != std::string::npos);
    CHECK(text.find("additive only") != std::string::npos);

    // The trap this block exists to close: a host that reads a matching ABI as
    // a promise of identical output. Said in capitals in the header for that
    // reason, so it must stay said.
    CHECK(text.find("A MATCHING ABI DOES NOT PROMISE THE SAME MESH") != std::string::npos);

    // Appending an enumerator is NOT additive here, and the reason is the same
    // out-of-range enum load that enumCode() exists for. The review of this
    // change refuted an earlier table that listed it as a safe minor.
    const size_t enumNote = text.find("Appending an ENUMERATOR is deliberately NOT");
    CHECK(enumNote != std::string::npos);
}
#endif
