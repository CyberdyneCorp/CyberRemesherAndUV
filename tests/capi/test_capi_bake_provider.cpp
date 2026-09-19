// The bake provider surface: what an external map consumer (CyberTexel) drives.
//
// engine-bindings spec, "Bake provider surface for an external map consumer";
// pipeline-bridge spec, "The bake provider is a seam, not a dependency".
//
// The cases here are written from the acceptance list rather than from the
// implementation: enumerate, request, size, refuse, cancel, and read the
// metadata back. Two of them matter more than the rest — a map this build cannot
// produce must be NAMED and not substituted, and a cancelled request must hand
// back nothing — so both are checked against a poisoned buffer rather than
// against a status code alone.
#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "cyber_capi.h"

namespace {

// Two-triangle unit plane in z=0 with per-corner UVs — the low-poly (needs UVs)
// and the coincident Target.
std::filesystem::path writeProviderPlaneObj() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "cyber_provider_plane.obj";
    std::ofstream out(path);
    out << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
           "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
           "f 1/1 2/2 3/3\nf 1/1 3/3 4/4\n";
    return path;
}

// The same plane pushed DOWN by `depth`, so a cage ray from the low-poly has to
// travel `cageDistance + depth` to reach it. The projection is accepted only
// while that is within 2 * cageDistance, which is what makes the cage visible in
// the output rather than merely marshalled.
std::filesystem::path writeSunkPlaneObj(float depth) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "cyber_provider_sunk_plane.obj";
    std::ofstream out(path);
    out << "v 0 0 " << -depth << "\nv 1 0 " << -depth << "\nv 1 1 " << -depth << "\nv 0 1 "
        << -depth
        << "\n"
           "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
           "f 1/1 2/2 3/3\nf 1/1 3/3 4/4\n";
    return path;
}

// Two faces carrying distinct material ids, through the bulk path -- the only
// way a host declares a face-domain id column over the ABI. Ids 4 and 9, so the
// table has two rows and a short buffer has something to truncate.
CyberMesh* twoMaterialPlane() {
    static const float positions[] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    static const size_t offsets[] = {0, 3, 6};
    static const uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    static const int32_t materials[] = {4, 9};
    const CyberAttributeColumn columns[] = {
        {"material_id", CYBER_ATTRIBUTE_FACE, CYBER_ATTRIBUTE_INT32, materials, 2},
    };
    const CyberIndexedMesh source{positions, 4, offsets, 2, indices, 6, columns, 1};
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_from_indexed(&source, &mesh) == CYBER_OK);
    return mesh;
}

// The pair every case starts from, freed by the caller.
struct PlanePair {
    std::filesystem::path path;
    CyberMesh* low = nullptr;
    CyberMesh* high = nullptr;

    PlanePair() {
        path = writeProviderPlaneObj();
        REQUIRE(cyber_mesh_load_obj(path.string().c_str(), &low) == CYBER_OK);
        REQUIRE(cyber_mesh_load_obj(path.string().c_str(), &high) == CYBER_OK);
    }
    PlanePair(const PlanePair&) = delete;
    PlanePair& operator=(const PlanePair&) = delete;
    ~PlanePair() {
        cyber_mesh_free(low);
        cyber_mesh_free(high);
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

CyberBakeParams smallParams(int size = 8) {
    CyberBakeParams params{};
    cyber_default_bake_params(&params);
    params.width = size;
    params.height = size;
    return params;
}

CyberBakeProviderRequest baseRequest(const PlanePair& pair, int map,
                                     const CyberBakeParams* params) {
    CyberBakeProviderRequest request{};
    request.structSize = sizeof(CyberBakeProviderRequest);
    request.low = pair.low;
    request.high = pair.high;
    request.map = map;
    request.params = params;
    return request;
}

CyberBakeProviderResult emptyResult() {
    CyberBakeProviderResult result{};
    result.structSize = sizeof(CyberBakeProviderResult);
    return result;
}

// A field evaluator over the plane at z = 0, so the field path has something
// well-formed to sample.
float planeFieldDistance(void*, const float p[3]) { return p[2]; }
void planeFieldGradient(void*, const float[3], float out[3]) {
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 1.0f;
}
float planeFieldOcclusion(void*, const float[3], const float[3], float) { return 1.0f; }

int alwaysCancel(void*) { return 1; }

struct ProgressCounter {
    int calls = 0;
    float last = -1.0f;
};
void countProgress(float fraction, const char*, void* user) {
    auto* counter = static_cast<ProgressCounter*>(user);
    counter->calls += 1;
    counter->last = fraction;
}

}  // namespace

TEST_CASE("the provider advertises a non-empty, self-consistent map set") {
    const size_t count = cyber_bake_provider_map_count();
    REQUIRE(count > 0u);

    std::set<std::string> names;
    std::set<int> codes;
    for (size_t index = 0; index < count; ++index) {
        CyberBakeProviderMap info{};
        info.structSize = sizeof(CyberBakeProviderMap);
        REQUIRE(cyber_bake_provider_map_at(index, &info) == CYBER_OK);
        REQUIRE(info.name != nullptr);
        CHECK(names.insert(info.name).second);
        CHECK(codes.insert(info.map).second);
        CHECK((info.channels == 1 || info.channels == 3));
        CHECK(std::string(info.colorSpace) != "");

        // Reachable by name as well as by index, and the same record either way.
        CyberBakeProviderMap byName{};
        byName.structSize = sizeof(CyberBakeProviderMap);
        REQUIRE(cyber_bake_provider_find_map(info.name, &byName) == CYBER_OK);
        CHECK(byName.map == info.map);
        CHECK(byName.channels == info.channels);
        CHECK(byName.fieldCapable == info.fieldCapable);

        // The name list a diagnostic quotes has to contain every advertised map,
        // or a refusal sends a consumer looking for a map it was never offered.
        CHECK(std::string(cyber_bake_provider_map_list(0)).find(info.name) != std::string::npos);
    }

    CyberBakeProviderMap unknown{};
    unknown.structSize = sizeof(CyberBakeProviderMap);
    CHECK(cyber_bake_provider_find_map("no-such-map", &unknown) == CYBER_ERR_INVALID_ARG);
    const std::string message = cyber_last_error();
    CHECK(message.find("no-such-map") != std::string::npos);
    CHECK(message.find("normal") != std::string::npos);

    CyberBakeProviderMap past{};
    past.structSize = sizeof(CyberBakeProviderMap);
    CHECK(cyber_bake_provider_map_at(count, &past) == CYBER_ERR_INVALID_ARG);
}

TEST_CASE("a consumer enumerates every advertised map and bakes each of them") {
    const PlanePair pair;
    const CyberBakeParams params = smallParams();

    for (size_t index = 0; index < cyber_bake_provider_map_count(); ++index) {
        CyberBakeProviderMap info{};
        info.structSize = sizeof(CyberBakeProviderMap);
        REQUIRE(cyber_bake_provider_map_at(index, &info) == CYBER_OK);
        CAPTURE(std::string(info.name));

        CyberBakeProviderRequest request = baseRequest(pair, info.map, &params);
        CyberBakeProviderResult sized = emptyResult();
        REQUIRE(cyber_bake_provider_bake(&request, &sized) == CYBER_OK);
        CHECK(sized.channels == info.channels);
        CHECK(sized.pixelCount == static_cast<size_t>(params.width) *
                                      static_cast<size_t>(params.height) *
                                      static_cast<size_t>(info.channels));

        std::vector<float> pixels(sized.pixelCount, -7.0f);
        std::vector<CyberIdColor> ids(64);
        request.pixels = pixels.data();
        request.pixelCapacity = pixels.size();
        request.idColors = ids.data();
        request.idColorCapacity = ids.size();

        CyberBakeProviderResult result = emptyResult();
        REQUIRE(cyber_bake_provider_bake(&request, &result) == CYBER_OK);
        CHECK(result.width == params.width);
        CHECK(result.height == params.height);
        CHECK(result.channels == info.channels);
        CHECK(result.texelsCovered > 0u);
        // +Y (OpenGL) is what this engine bakes, and the point of reporting it
        // is that a consumer never has to assume it.
        CHECK(result.normalGreenPlusY == 1);
        CHECK(result.idSource != nullptr);
        // Every texel was written: the poison value must be gone everywhere.
        for (const float value : pixels) {
            CHECK(value != -7.0f);
        }
        // The padding record travels with the map, for every map.
        CHECK(result.padding.radius == params.paddingRadius);
    }
}

TEST_CASE("the provider and cyber_bake produce the same map, texel for texel") {
    // The provider must take the SHARED bake path rather than a second one: a
    // consumer driving the seam and a host calling cyber_bake are entitled to
    // the same pixels.
    const PlanePair pair;
    const CyberBakeParams params = smallParams(16);

    CyberImage* image = nullptr;
    REQUIRE(cyber_bake(pair.low, pair.high, CYBER_BAKE_OBJECT_NORMAL, &params, &image) == CYBER_OK);
    const size_t needed = cyber_image_copy_pixels(image, nullptr, 0);
    std::vector<float> reference(needed);
    REQUIRE(cyber_image_copy_pixels(image, reference.data(), reference.size()) == needed);
    CyberImageEncoding referenceEncoding{};
    REQUIRE(cyber_image_encoding(image, &referenceEncoding) == CYBER_OK);
    cyber_image_free(image);

    std::vector<float> pixels(needed, 0.0f);
    CyberBakeProviderRequest request = baseRequest(pair, CYBER_BAKE_OBJECT_NORMAL, &params);
    request.pixels = pixels.data();
    request.pixelCapacity = pixels.size();
    CyberBakeProviderResult result = emptyResult();
    REQUIRE(cyber_bake_provider_bake(&request, &result) == CYBER_OK);

    REQUIRE(pixels.size() == reference.size());
    for (size_t i = 0; i < pixels.size(); ++i) {
        CHECK(pixels[i] == reference[i]);
    }
    CHECK(result.encoding.basis == referenceEncoding.basis);
    CHECK(result.encoding.upAxis == referenceEncoding.upAxis);
    CHECK(result.encoding.boundsMin[0] == referenceEncoding.boundsMin[0]);
    CHECK(result.encoding.boundsMax[0] == referenceEncoding.boundsMax[0]);
}

TEST_CASE("the sizing call validates the request and writes no pixel") {
    const PlanePair pair;
    const CyberBakeParams params = smallParams(32);

    CyberBakeProviderRequest request = baseRequest(pair, CYBER_BAKE_AO, &params);
    CyberBakeProviderResult result = emptyResult();
    REQUIRE(cyber_bake_provider_bake(&request, &result) == CYBER_OK);
    CHECK(result.width == 32);
    CHECK(result.height == 32);
    CHECK(result.channels == 1);
    CHECK(result.pixelCount == 32u * 32u);
    // No bake ran, so nothing that only a bake can fill is filled.
    CHECK(result.texelsCovered == 0u);
    CHECK(result.padding.radius == 0);

    // And an invalid request is refused by the sizing call too, so a consumer
    // learns it cannot have the map before it allocates for it.
    CyberBakeParams bad = params;
    bad.paddingRadius = -1;
    CyberBakeProviderRequest badRequest = baseRequest(pair, CYBER_BAKE_AO, &bad);
    CyberBakeProviderResult badResult = emptyResult();
    CHECK(cyber_bake_provider_bake(&badRequest, &badResult) == CYBER_ERR_INVALID_ARG);
    CHECK(std::string(cyber_last_error()).find("paddingRadius") != std::string::npos);
}

TEST_CASE("a short pixel buffer is refused with both capacities named") {
    const PlanePair pair;
    const CyberBakeParams params = smallParams();

    std::vector<float> pixels(10, 3.5f);
    CyberBakeProviderRequest request = baseRequest(pair, CYBER_BAKE_NORMAL, &params);
    request.pixels = pixels.data();
    request.pixelCapacity = pixels.size();
    CyberBakeProviderResult result = emptyResult();
    CHECK(cyber_bake_provider_bake(&request, &result) == CYBER_ERR_INVALID_ARG);

    const std::string message = cyber_last_error();
    CHECK(message.find("10") != std::string::npos);
    CHECK(message.find(std::to_string(8 * 8 * 3)) != std::string::npos);
    // Refused rather than filled partway: truncation here is a consumer
    // uploading the top of its texture and garbage below.
    for (const float value : pixels) {
        CHECK(value == 3.5f);
    }
}

TEST_CASE("a map this build cannot produce is named, never substituted") {
    const PlanePair pair;
    const CyberBakeParams params = smallParams();

    std::vector<float> pixels(8 * 8 * 3, 0.25f);
    CyberBakeProviderRequest request = baseRequest(pair, 4242, &params);
    request.pixels = pixels.data();
    request.pixelCapacity = pixels.size();
    CyberBakeProviderResult result = emptyResult();
    CHECK(cyber_bake_provider_bake(&request, &result) == CYBER_ERR_INVALID_ARG);

    const std::string message = cyber_last_error();
    CHECK(message.find("4242") != std::string::npos);
    // The advertised set, so the consumer can put the refusal in front of a user.
    CHECK(message.find(cyber_bake_provider_map_list(0)) != std::string::npos);
    // NOT a neutral image: a consumer silently handed flat grey where it asked
    // for curvature ships work that is subtly wrong instead of visibly broken.
    for (const float value : pixels) {
        CHECK(value == 0.25f);
    }
}

TEST_CASE("a field evaluator narrows the producible set, and the refusal says so") {
    const PlanePair pair;
    const CyberBakeParams params = smallParams(4);
    const CyberFieldEvaluator field{planeFieldDistance, planeFieldGradient, planeFieldOcclusion,
                                    nullptr};

    const std::string fieldList = cyber_bake_provider_map_list(1);
    CHECK(fieldList.find("normal") != std::string::npos);
    CHECK(fieldList.find("thickness") == std::string::npos);

    // A field-capable map, with no Target at all.
    std::vector<float> pixels(4 * 4 * 3, 0.0f);
    CyberBakeProviderRequest request = baseRequest(pair, CYBER_BAKE_NORMAL, &params);
    request.high = nullptr;
    request.field = &field;
    request.pixels = pixels.data();
    request.pixelCapacity = pixels.size();
    CyberBakeProviderResult result = emptyResult();
    CHECK(cyber_bake_provider_bake(&request, &result) == CYBER_OK);
    CHECK(result.encoding.basis == CYBER_ENCODING_TANGENT_NORMAL);

    // A Target-only map through the same evaluator: refused by name, listing
    // what a field alone CAN produce rather than the whole set.
    std::vector<float> untouched(4 * 4, 9.0f);
    CyberBakeProviderRequest refused = baseRequest(pair, CYBER_BAKE_THICKNESS, &params);
    refused.high = nullptr;
    refused.field = &field;
    refused.pixels = untouched.data();
    refused.pixelCapacity = untouched.size();
    CyberBakeProviderResult refusedResult = emptyResult();
    CHECK(cyber_bake_provider_bake(&refused, &refusedResult) == CYBER_ERR_INVALID_ARG);
    const std::string message = cyber_last_error();
    CHECK(message.find("thickness") != std::string::npos);
    CHECK(message.find(fieldList) != std::string::npos);
    for (const float value : untouched) {
        CHECK(value == 9.0f);
    }

    // The capability query said so in advance, which is the point of having one.
    CyberBakeProviderMap thickness{};
    thickness.structSize = sizeof(CyberBakeProviderMap);
    REQUIRE(cyber_bake_provider_find_map("thickness", &thickness) == CYBER_OK);
    CHECK(thickness.fieldCapable == 0);
}

TEST_CASE("cancellation returns the cancelled code and hands back no pixels") {
    const PlanePair pair;
    const CyberBakeParams params = smallParams(32);

    std::vector<float> pixels(32u * 32u * 3u, 1.25f);
    std::vector<CyberIdColor> ids(4);
    std::memset(ids.data(), 0x5a, ids.size() * sizeof(CyberIdColor));
    const std::vector<CyberIdColor> idsBefore = ids;

    CyberBakeProviderRequest request = baseRequest(pair, CYBER_BAKE_NORMAL, &params);
    request.cancel = alwaysCancel;
    request.pixels = pixels.data();
    request.pixelCapacity = pixels.size();
    request.idColors = ids.data();
    request.idColorCapacity = ids.size();
    CyberBakeProviderResult result = emptyResult();

    CHECK(cyber_bake_provider_bake(&request, &result) == CYBER_ERR_CANCELLED);
    for (const float value : pixels) {
        CHECK(value == 1.25f);
    }
    CHECK(std::memcmp(ids.data(), idsBefore.data(), ids.size() * sizeof(CyberIdColor)) == 0);
    // Nothing about the map was reported either — there is no map.
    CHECK(result.width == 0);
    CHECK(result.pixelCount == 0u);
}

TEST_CASE("progress is reported while a ray-traced map accumulates") {
    const PlanePair pair;
    CyberBakeParams params = smallParams(32);
    params.aoSamples = 8;

    ProgressCounter counter;
    std::vector<float> pixels(32u * 32u, 0.0f);
    CyberBakeProviderRequest request = baseRequest(pair, CYBER_BAKE_AO, &params);
    request.progress = countProgress;
    request.user = &counter;
    request.pixels = pixels.data();
    request.pixelCapacity = pixels.size();
    CyberBakeProviderResult result = emptyResult();

    REQUIRE(cyber_bake_provider_bake(&request, &result) == CYBER_OK);
    // More than once: a bar that jumps from nothing to done is not progress.
    CHECK(counter.calls > 1);
    CHECK(counter.last == doctest::Approx(1.0f));
}

TEST_CASE("an id map's table arrives with its pixels and resolves at zero tolerance") {
    const std::filesystem::path objPath = writeProviderPlaneObj();
    CyberMesh* low = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &low) == CYBER_OK);

    CyberMesh* high = twoMaterialPlane();

    const CyberBakeParams params = smallParams(16);
    std::vector<float> pixels(16u * 16u * 3u, 0.0f);
    std::vector<CyberIdColor> ids(8);

    CyberBakeProviderRequest request{};
    request.structSize = sizeof(CyberBakeProviderRequest);
    request.low = low;
    request.high = high;
    request.map = CYBER_BAKE_MATERIAL_ID;
    request.params = &params;
    request.pixels = pixels.data();
    request.pixelCapacity = pixels.size();
    request.idColors = ids.data();
    request.idColorCapacity = ids.size();
    CyberBakeProviderResult result = emptyResult();
    REQUIRE(cyber_bake_provider_bake(&request, &result) == CYBER_OK);

    CHECK(result.encoding.basis == CYBER_ENCODING_ID_COLOR);
    CHECK(std::string(result.idSource) == "material_id");
    REQUIRE(result.idColorCount == 2u);
    CHECK(ids[0].id == 4);
    CHECK(ids[1].id == 9);  // ascending by id, never a container's order

    // A colour picked out of the map resolves to exactly one row, byte-exact.
    const size_t centre = (8u * 16u + 4u) * 3u;
    int matches = 0;
    for (size_t row = 0; row < result.idColorCount; ++row) {
        const bool same =
            static_cast<int>(pixels[centre + 0] * 255.0f + 0.5f) == ids[row].color[0] &&
            static_cast<int>(pixels[centre + 1] * 255.0f + 0.5f) == ids[row].color[1] &&
            static_cast<int>(pixels[centre + 2] * 255.0f + 0.5f) == ids[row].color[2];
        matches += same ? 1 : 0;
    }
    CHECK(matches == 1);

    // A NULL table buffer still reports the total, so a consumer can allocate
    // and ask again — the two-call convention the rest of this ABI uses.
    request.idColors = nullptr;
    request.idColorCapacity = 0;
    CyberBakeProviderResult counted = emptyResult();
    REQUIRE(cyber_bake_provider_bake(&request, &counted) == CYBER_OK);
    CHECK(counted.idColorCount == 2u);

    cyber_mesh_free(low);
    cyber_mesh_free(high);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

TEST_CASE("the descriptor size mechanism serves a newer caller and refuses a truncated one") {
    const PlanePair pair;
    const CyberBakeParams params = smallParams();

    // A caller compiled against a LATER header states a bigger descriptor. This
    // build reads only what it knows and ignores the surplus, which is the whole
    // promise of putting structSize first.
    std::vector<float> pixels(8u * 8u * 3u, 0.0f);
    CyberBakeProviderRequest larger = baseRequest(pair, CYBER_BAKE_NORMAL, &params);
    larger.structSize = sizeof(CyberBakeProviderRequest) + 64;
    larger.pixels = pixels.data();
    larger.pixelCapacity = pixels.size();
    CyberBakeProviderResult result = emptyResult();
    result.structSize = sizeof(CyberBakeProviderResult) + 64;
    CHECK(cyber_bake_provider_bake(&larger, &result) == CYBER_OK);
    CHECK(result.width == 8);

    // Below the first published layout there is no way to tell which members the
    // caller has, so it is refused naming both numbers rather than guessed at.
    CyberBakeProviderRequest tiny = baseRequest(pair, CYBER_BAKE_NORMAL, &params);
    tiny.structSize = sizeof(size_t);
    CyberBakeProviderResult tinyResult = emptyResult();
    CHECK(cyber_bake_provider_bake(&tiny, &tinyResult) == CYBER_ERR_INVALID_ARG);
    std::string message = cyber_last_error();
    CHECK(message.find(std::to_string(sizeof(size_t))) != std::string::npos);
    CHECK(message.find(std::to_string(sizeof(CyberBakeProviderRequest))) != std::string::npos);

    CyberBakeProviderRequest good = baseRequest(pair, CYBER_BAKE_NORMAL, &params);
    CyberBakeProviderResult shortResult = emptyResult();
    shortResult.structSize = sizeof(size_t);
    CHECK(cyber_bake_provider_bake(&good, &shortResult) == CYBER_ERR_INVALID_ARG);
    CHECK(std::string(cyber_last_error()).find("result") != std::string::npos);

    CyberBakeProviderMap shortMap{};
    shortMap.structSize = 4;
    CHECK(cyber_bake_provider_map_at(0, &shortMap) == CYBER_ERR_INVALID_ARG);
}

TEST_CASE("a newer caller's extra descriptor bytes are left alone") {
    // The out-param half of the mechanism, tested from the only direction that
    // exists in ABI 1.22: this build is the FIRST published layout, so a smaller
    // descriptor is refused (above) and the case that can occur is a caller
    // compiled against a later header, whose result struct is LONGER than ours.
    // Writing our whole layout into it is fine; writing past it is not, and the
    // members it has that we do not must keep the value it set them to.
    const PlanePair pair;
    const CyberBakeParams params = smallParams();

    struct Extended {
        CyberBakeProviderResult result;
        unsigned char appended[64];
    };
    Extended extended{};
    std::memset(&extended, 0xcd, sizeof(extended));
    extended.result.structSize = sizeof(Extended);

    std::vector<float> pixels(8u * 8u * 3u, 0.0f);
    CyberBakeProviderRequest request = baseRequest(pair, CYBER_BAKE_NORMAL, &params);
    request.pixels = pixels.data();
    request.pixelCapacity = pixels.size();
    REQUIRE(cyber_bake_provider_bake(&request, &extended.result) == CYBER_OK);

    CHECK(extended.result.width == 8);
    CHECK(extended.result.pixelCount == 8u * 8u * 3u);
    // structSize comes back as the caller stated it, not as this build's size:
    // a caller that read it back and used it to stride would otherwise walk off.
    CHECK(extended.result.structSize == sizeof(Extended));
    for (const unsigned char byte : extended.appended) {
        CHECK(byte == 0xcd);
    }
}

TEST_CASE("a caller compiled against the ABI 1.22 layout is still served") {
    // ABI 1.23 appended `density` and `placement` to CyberBakeProviderResult.
    // The accepted FLOOR must not have moved with them: a caller whose struct
    // stops at `idSource` is exactly the caller the descriptor-size mechanism
    // exists for, and refusing it would make "appending to these three is
    // additive" false the first time it was used.
    const PlanePair pair;
    const CyberBakeParams params = smallParams();

    CyberBakeProviderResult result{};
    std::memset(&result, 0xcd, sizeof(result));
    // The end of the last 1.22 member, which is at or below what a 1.22 caller
    // would state (its own sizeof includes that layout's trailing padding).
    result.structSize = offsetof(CyberBakeProviderResult, density);

    std::vector<float> pixels(8u * 8u * 3u, 0.0f);
    CyberBakeProviderRequest request = baseRequest(pair, CYBER_BAKE_UV_DENSITY, &params);
    request.pixels = pixels.data();
    request.pixelCapacity = 8u * 8u;  // one channel
    REQUIRE(cyber_bake_provider_bake(&request, &result) == CYBER_OK);
    CHECK(result.width == 8);
    CHECK(result.channels == 1);

    // Nothing was written into the members that caller does not have, even for
    // the very map whose metadata lives in them.
    const auto* bytes = reinterpret_cast<const unsigned char*>(&result);
    for (std::size_t i = offsetof(CyberBakeProviderResult, density); i < sizeof(result); ++i) {
        CHECK(bytes[i] == 0xcd);
    }
}

TEST_CASE("the provider reports the 1.23 density and placement metadata") {
    const PlanePair pair;
    CyberBakeParams params = smallParams();

    std::vector<float> density(8u * 8u, 0.0f);
    CyberBakeProviderRequest request = baseRequest(pair, CYBER_BAKE_UV_DENSITY, &params);
    request.pixels = density.data();
    request.pixelCapacity = density.size();
    CyberBakeProviderResult result = emptyResult();
    REQUIRE(cyber_bake_provider_bake(&request, &result) == CYBER_OK);
    CHECK(result.encoding.basis == CYBER_ENCODING_UV_DENSITY);
    CHECK(result.density.normalization == CYBER_DENSITY_ABSOLUTE);
    CHECK(result.density.mean > 0.0f);

    // A map that reads no placement still reports the identity, so a consumer
    // never has to know which maps read one.
    for (int i = 0; i < 16; ++i) {
        CHECK(result.placement[i] == ((i % 5 == 0) ? 1.0f : 0.0f));
    }

    // The placement a world-direction request was given comes back with it.
    params.placement[5] = 0.0f;
    params.placement[6] = -1.0f;
    params.placement[9] = 1.0f;
    params.placement[10] = 0.0f;
    std::vector<float> direction(8u * 8u * 3u, 0.0f);
    CyberBakeProviderRequest placed = baseRequest(pair, CYBER_BAKE_WORLD_DIRECTION, &params);
    placed.pixels = direction.data();
    placed.pixelCapacity = direction.size();
    CyberBakeProviderResult placedResult = emptyResult();
    REQUIRE(cyber_bake_provider_bake(&placed, &placedResult) == CYBER_OK);
    CHECK(placedResult.encoding.basis == CYBER_ENCODING_WORLD_DIRECTION);
    for (int i = 0; i < 16; ++i) {
        CHECK(placedResult.placement[i] == params.placement[i]);
    }

    // And the same refusal cyber_bake applies: a singular placement is not
    // folded to the identity here either.
    params.placement[5] = 0.0f;
    params.placement[6] = 0.0f;
    params.placement[9] = 0.0f;
    params.placement[10] = 0.0f;
    CyberBakeProviderRequest singular = baseRequest(pair, CYBER_BAKE_WORLD_DIRECTION, &params);
    singular.pixels = direction.data();
    singular.pixelCapacity = direction.size();
    CyberBakeProviderResult refused = emptyResult();
    CHECK(cyber_bake_provider_bake(&singular, &refused) == CYBER_ERR_INVALID_ARG);
}

TEST_CASE("the provider honours the host's bake texel ceiling") {
    const PlanePair pair;
    const CyberBakeParams params = smallParams(16);

    REQUIRE(cyber_set_max_bake_pixels(255) == CYBER_OK);
    std::vector<float> pixels(16u * 16u * 3u, 2.0f);
    CyberBakeProviderRequest request = baseRequest(pair, CYBER_BAKE_NORMAL, &params);
    request.pixels = pixels.data();
    request.pixelCapacity = pixels.size();
    CyberBakeProviderResult result = emptyResult();
    CHECK(cyber_bake_provider_bake(&request, &result) == CYBER_ERR_RUNTIME);
    CHECK(std::string(cyber_last_error()).find("bake ceiling") != std::string::npos);
    for (const float value : pixels) {
        CHECK(value == 2.0f);
    }
    REQUIRE(cyber_set_max_bake_pixels(0) == CYBER_OK);
}

TEST_CASE("the provider refuses null arguments and a missing Target") {
    const PlanePair pair;
    const CyberBakeParams params = smallParams();

    CyberBakeProviderResult result = emptyResult();
    CHECK(cyber_bake_provider_bake(nullptr, &result) == CYBER_ERR_INVALID_ARG);
    CyberBakeProviderRequest request = baseRequest(pair, CYBER_BAKE_NORMAL, &params);
    CHECK(cyber_bake_provider_bake(&request, nullptr) == CYBER_ERR_INVALID_ARG);

    CyberBakeProviderRequest noLow = baseRequest(pair, CYBER_BAKE_NORMAL, &params);
    noLow.low = nullptr;
    CHECK(cyber_bake_provider_bake(&noLow, &result) == CYBER_ERR_INVALID_ARG);

    CyberBakeProviderRequest noHigh = baseRequest(pair, CYBER_BAKE_NORMAL, &params);
    noHigh.high = nullptr;
    CHECK(cyber_bake_provider_bake(&noHigh, &result) == CYBER_ERR_INVALID_ARG);
    CHECK(std::string(cyber_last_error()).find("Target") != std::string::npos);

    // A half-filled evaluator is a host bug, not a field: refused rather than
    // called through a null pointer.
    CyberFieldEvaluator broken{planeFieldDistance, nullptr, planeFieldOcclusion, nullptr};
    CyberBakeProviderRequest partial = baseRequest(pair, CYBER_BAKE_NORMAL, &params);
    partial.field = &broken;
    CHECK(cyber_bake_provider_bake(&partial, &result) == CYBER_ERR_INVALID_ARG);
}

TEST_CASE("a low-poly without UVs is empty, not a blank map") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "cyber_provider_nouv.obj";
    {
        std::ofstream out(path);
        out << "v 0 0 0\nv 1 0 0\nv 1 1 0\nf 1 2 3\n";
    }
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(path.string().c_str(), &mesh) == CYBER_OK);

    const CyberBakeParams params = smallParams();
    std::vector<float> pixels(8u * 8u * 3u, 0.75f);
    CyberBakeProviderRequest request{};
    request.structSize = sizeof(CyberBakeProviderRequest);
    request.low = mesh;
    request.high = mesh;
    request.map = CYBER_BAKE_NORMAL;
    request.params = &params;
    request.pixels = pixels.data();
    request.pixelCapacity = pixels.size();
    CyberBakeProviderResult result = emptyResult();
    CHECK(cyber_bake_provider_bake(&request, &result) == CYBER_ERR_EMPTY);
    for (const float value : pixels) {
        CHECK(value == 0.75f);
    }

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("a short id table is filled to its stated capacity and never past it") {
    // The one asymmetry in this surface: a short PIXEL buffer is refused (the
    // consumer could have computed the exact count from the capability query
    // before it called), while a short ID buffer follows the two-call
    // convention, because the number of ids is not knowable until the bake has
    // read the Target. Filling what fits is therefore correct here -- and the
    // whole contract rests on one clamp, so the check that matters is the one
    // on the rows BEYOND the stated capacity: writing there is an out-of-bounds
    // write into a consumer's heap, silent in every other gate.
    const std::filesystem::path objPath = writeProviderPlaneObj();
    CyberMesh* low = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &low) == CYBER_OK);
    CyberMesh* high = twoMaterialPlane();

    const CyberBakeParams params = smallParams(16);
    std::vector<float> pixels(16u * 16u * 3u, 0.0f);

    // Four rows allocated and poisoned; ONE row declared. Rows 1..3 exist so an
    // overrun lands in memory this case owns and can inspect, instead of in
    // whatever the allocator put next.
    constexpr int32_t kPoisonId = -424242;
    constexpr unsigned char kPoisonByte = 0x7e;
    std::vector<CyberIdColor> ids(4);
    for (CyberIdColor& row : ids) {
        row.id = kPoisonId;
        row.color[0] = kPoisonByte;
        row.color[1] = kPoisonByte;
        row.color[2] = kPoisonByte;
    }

    CyberBakeProviderRequest request{};
    request.structSize = sizeof(CyberBakeProviderRequest);
    request.low = low;
    request.high = high;
    request.map = CYBER_BAKE_MATERIAL_ID;
    request.params = &params;
    request.pixels = pixels.data();
    request.pixelCapacity = pixels.size();
    request.idColors = ids.data();
    request.idColorCapacity = 1;
    CyberBakeProviderResult result = emptyResult();
    REQUIRE(cyber_bake_provider_bake(&request, &result) == CYBER_OK);

    // The TOTAL, not what fit: that number is how the consumer learns it must
    // allocate again, and reporting the truncated count would make the second
    // call as short as the first.
    CHECK(result.idColorCount == 2u);
    // The row that fit is the first row of the table, not an arbitrary one.
    CHECK(ids[0].id == 4);
    for (size_t row = 1; row < ids.size(); ++row) {
        CAPTURE(row);
        CHECK(ids[row].id == kPoisonId);
        CHECK(ids[row].color[0] == kPoisonByte);
        CHECK(ids[row].color[1] == kPoisonByte);
        CHECK(ids[row].color[2] == kPoisonByte);
    }

    // Capacity 0 with a non-NULL buffer is the same contract with nothing to
    // fill, and must not write row 0 either.
    for (CyberIdColor& row : ids) {
        row.id = kPoisonId;
        row.color[0] = kPoisonByte;
    }
    request.idColorCapacity = 0;
    CyberBakeProviderResult zeroed = emptyResult();
    REQUIRE(cyber_bake_provider_bake(&request, &zeroed) == CYBER_OK);
    CHECK(zeroed.idColorCount == 2u);
    CHECK(ids[0].id == kPoisonId);

    // And asking again with the reported capacity gets the whole table, which
    // is what makes the truncation a two-call convention rather than a loss.
    std::vector<CyberIdColor> full(result.idColorCount);
    request.idColors = full.data();
    request.idColorCapacity = full.size();
    CyberBakeProviderResult second = emptyResult();
    REQUIRE(cyber_bake_provider_bake(&request, &second) == CYBER_OK);
    REQUIRE(second.idColorCount == full.size());
    CHECK(full[0].id == 4);
    CHECK(full[1].id == 9);

    cyber_mesh_free(low);
    cyber_mesh_free(high);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

TEST_CASE("every advertised map states a colour space, and only colour is sRGB") {
    // A consumer reads this to decide whether to put a transfer curve on the
    // texels. "Linear" on colour ships a washed-out base map; "srgb" on any
    // other map gamma-encodes DATA -- a normal, a distance, an id key -- which
    // is wrong in every target app. Checking only that the string is non-empty
    // would let either drift through.
    bool sawSrgb = false;
    for (size_t index = 0; index < cyber_bake_provider_map_count(); ++index) {
        CyberBakeProviderMap info{};
        info.structSize = sizeof(CyberBakeProviderMap);
        REQUIRE(cyber_bake_provider_map_at(index, &info) == CYBER_OK);
        REQUIRE(info.colorSpace != nullptr);
        const std::string space = info.colorSpace;
        CAPTURE(std::string(info.name));
        CHECK((space == "linear" || space == "srgb"));
        CHECK(space == (std::string(info.name) == "color" ? "srgb" : "linear"));
        sawSrgb = sawSrgb || space == "srgb";
    }
    // The rule has a positive half too: if every map went linear the case above
    // would still pass on a build that had simply dropped the colour map.
    CHECK(sawSrgb);
}

TEST_CASE("the projection cage reaches the bake through the provider") {
    // cageDistance is marshalled by the same helper cyber_bake and
    // cyber_bake_field use, and nothing anywhere asserted that it arrives. It is
    // not a cosmetic parameter: rays start at surface + normal * cageDistance
    // and a projection is accepted only within 2 * cageDistance, so the cage
    // decides which Target a texel sees at all.
    //
    // Target sunk 0.3 below the EditMesh. A 0.1 cage searches 0.2 and misses it;
    // a 0.5 cage searches 1.0 and finds it. CYBER_BAKE_POSITION writes the hit
    // point on a hit and the EditMesh's own point on a miss, so the two cages
    // are a plain difference in the Z channel rather than a shade.
    constexpr float kDepth = 0.3f;
    const std::filesystem::path lowPath = writeProviderPlaneObj();
    const std::filesystem::path highPath = writeSunkPlaneObj(kDepth);
    CyberMesh* low = nullptr;
    CyberMesh* high = nullptr;
    REQUIRE(cyber_mesh_load_obj(lowPath.string().c_str(), &low) == CYBER_OK);
    REQUIRE(cyber_mesh_load_obj(highPath.string().c_str(), &high) == CYBER_OK);

    const auto bakeWithCage = [&](float cage) {
        CyberBakeParams params = smallParams(16);
        params.cageDistance = cage;
        std::vector<float> pixels(16u * 16u * 3u, 0.0f);
        CyberBakeProviderRequest request{};
        request.structSize = sizeof(CyberBakeProviderRequest);
        request.low = low;
        request.high = high;
        request.map = CYBER_BAKE_POSITION;
        request.params = &params;
        request.pixels = pixels.data();
        request.pixelCapacity = pixels.size();
        CyberBakeProviderResult result = emptyResult();
        REQUIRE(cyber_bake_provider_bake(&request, &result) == CYBER_OK);
        return pixels;
    };

    const std::vector<float> tooShort = bakeWithCage(0.1f);
    const std::vector<float> longEnough = bakeWithCage(0.5f);

    // Centre texel, Z channel.
    const size_t centre = (8u * 16u + 8u) * 3u + 2u;
    CHECK(tooShort[centre] == doctest::Approx(0.0f).epsilon(1e-3));
    CHECK(longEnough[centre] == doctest::Approx(-kDepth).epsilon(1e-3));

    // A cage that cannot reach must not reach: no covered texel may report the
    // Target's depth under the short cage.
    for (size_t texel = 0; texel < 16u * 16u; ++texel) {
        CHECK(tooShort[texel * 3u + 2u] > -kDepth * 0.5f);
    }

    cyber_mesh_free(low);
    cyber_mesh_free(high);
    std::error_code ec;
    std::filesystem::remove(lowPath, ec);
    std::filesystem::remove(highPath, ec);
}
