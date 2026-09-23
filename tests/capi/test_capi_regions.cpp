#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "cyber_capi.h"

// Regioned baking through the C ABI (engine-bindings, "Regioned baking is
// reachable from the bindings"; ABI 2.1), and the first real use of the promise
// ABI 2.0's sized structs make: a member APPENDED in a minor is invisible to a
// caller compiled against the older layout.
namespace {

// A tilted plane, so the normal and position maps vary down the image, UV-mapped
// onto a chart that leaves a border for the padded band to grow into.
std::filesystem::path writeLowObj() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "cyber_capi_regions_low.obj";
    std::ofstream out(path);
    out << "v 0 0 0\nv 1 0 0\nv 1 1 0.3\nv 0 1 0.3\n"
           "vt 0.1 0.1\nvt 0.9 0.1\nvt 0.9 0.9\nvt 0.1 0.9\n"
           "f 1/1 2/2 3/3\nf 1/1 3/3 4/4\n";
    return path;
}

std::filesystem::path writeHighObj() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "cyber_capi_regions_high.obj";
    std::ofstream out(path);
    out << "v -0.5 -0.5 0.02\nv 1.5 -0.5 0.02\nv 1.5 1.5 0.5\nv -0.5 1.5 0.5\nv 0.5 0.5 0.1\n"
           "f 1 2 5\nf 2 3 5\nf 3 4 5\nf 4 1 5\n";
    return path;
}

// The ABI 2.0 layout's end: the floor, frozen at 2.0 and never moved.
constexpr std::size_t kBake20Size = offsetof(CyberBakeParams, densityNormalization) + sizeof(int);
constexpr std::size_t kBundle20Size = offsetof(CyberBundleParams, udim) + sizeof(int);

struct Rows {
    std::vector<float> pixels;
    int nextRow = 0;
    int bands = 0;
    bool ordered = true;
    int stopAfter = -1;  // bands to accept before asking to stop; -1 = never
};

int collectRows(int rowBegin, int rowCount, int width, int channels, const float* rows,
                void* user) {
    auto& sink = *static_cast<Rows*>(user);
    if (sink.stopAfter >= 0 && sink.bands >= sink.stopAfter) {
        return 1;
    }
    sink.ordered = sink.ordered && rowBegin == sink.nextRow;
    sink.nextRow = rowBegin + rowCount;
    ++sink.bands;
    const auto count = static_cast<std::size_t>(rowCount) * static_cast<std::size_t>(width) *
                       static_cast<std::size_t>(channels);
    sink.pixels.insert(sink.pixels.end(), rows, rows + count);
    return 0;
}

struct Meshes {
    CyberMesh* low = nullptr;
    CyberMesh* high = nullptr;
    Meshes() {
        REQUIRE(cyber_mesh_load_obj(writeLowObj().string().c_str(), &low) == CYBER_OK);
        REQUIRE(cyber_mesh_load_obj(writeHighObj().string().c_str(), &high) == CYBER_OK);
    }
    ~Meshes() {
        cyber_mesh_free(low);
        cyber_mesh_free(high);
    }
    Meshes(const Meshes&) = delete;
    Meshes& operator=(const Meshes&) = delete;
};

CyberBakeParams defaults() {
    CyberBakeParams params{};
    params.structSize = sizeof params;
    REQUIRE(cyber_default_bake_params(&params) == CYBER_OK);
    params.width = 48;
    params.height = 48;
    params.cageDistance = 0.3f;
    params.aoSamples = 8;
    params.paddingRadius = 4;
    return params;
}

std::vector<float> wholePixels(CyberImage* image) {
    std::vector<float> pixels(cyber_image_copy_pixels(image, nullptr, 0));
    cyber_image_copy_pixels(image, pixels.data(), pixels.size());
    return pixels;
}

}  // namespace

TEST_CASE("capi ABI 2.1 appends the working-set bound to the sized parameter structs") {
    int major = 0;
    int minor = 0;
    cyber_abi_version(&major, &minor);
    CHECK(major == 2);
    CHECK(minor == 1);
    CHECK(cyber_abi_check(2, 0) == CYBER_OK);  // a 2.0 client is served
    CHECK(cyber_abi_check(2, 1) == CYBER_OK);

    CyberBakeParams bake{};
    bake.structSize = sizeof bake;
    REQUIRE(cyber_default_bake_params(&bake) == CYBER_OK);
    CHECK(bake.maxWorkingSetTexels == 0u);
    CyberBundleParams bundle{};
    bundle.structSize = sizeof bundle;
    REQUIRE(cyber_default_bundle_params(&bundle) == CYBER_OK);
    CHECK(bundle.maxWorkingSetTexels == 0u);
    // Appended, so past the 2.0 floor.
    CHECK(offsetof(CyberBakeParams, maxWorkingSetTexels) >= kBake20Size);
    CHECK(offsetof(CyberBundleParams, maxWorkingSetTexels) >= kBundle20Size);
}

TEST_CASE("a caller stating the 2.0 size gets the default bound and nothing past its size") {
    // The 2.0 caller's struct ends at the 2.0 floor. Whatever lies beyond it is
    // the caller's own memory: the defaults call must not write it, and the bake
    // must not read it -- here it holds a bound of ONE texel, which would split
    // the map into one-row regions if it were read.
    CyberBakeParams params;
    std::memset(&params, 0xEE, sizeof params);
    params.structSize = kBake20Size;
    REQUIRE(cyber_default_bake_params(&params) == CYBER_OK);
    const auto* bytes = reinterpret_cast<const unsigned char*>(&params);
    for (std::size_t i = kBake20Size; i < sizeof params; ++i) {
        CAPTURE(i);
        CHECK(bytes[i] == 0xEE);
    }
    params.maxWorkingSetTexels = 1;  // beyond the stated size: must be ignored
    params.width = 32;
    params.height = 32;

    Meshes meshes;
    Rows rows;
    CyberImage* report = nullptr;
    REQUIRE(cyber_bake_regions(meshes.low, meshes.high, CYBER_BAKE_NORMAL, &params, nullptr,
                               collectRows, nullptr, nullptr, &rows, nullptr, &report) == CYBER_OK);
    uint64_t regions = 0;
    REQUIRE(cyber_image_regions(report, &regions, nullptr, nullptr, nullptr) == CYBER_OK);
    CHECK(regions == 1u);  // the default (no bound), not the unread 1
    CHECK(rows.bands == 1);
    cyber_image_free(report);

    CyberBundleParams bundle;
    std::memset(&bundle, 0xEE, sizeof bundle);
    bundle.structSize = kBundle20Size;
    REQUIRE(cyber_default_bundle_params(&bundle) == CYBER_OK);
    const auto* bundleBytes = reinterpret_cast<const unsigned char*>(&bundle);
    for (std::size_t i = kBundle20Size; i < sizeof bundle; ++i) {
        CAPTURE(i);
        CHECK(bundleBytes[i] == 0xEE);
    }
    CHECK(bundle.udim == 0);  // the last 2.0 member WAS written
}

TEST_CASE("capi regioned bake equals cyber_bake, band by band") {
    Meshes meshes;
    for (const CyberBakeMap map :
         {CYBER_BAKE_NORMAL, CYBER_BAKE_AO, CYBER_BAKE_OBJECT_POSITION, CYBER_BAKE_UV_DENSITY,
          CYBER_BAKE_MATERIAL_ID, CYBER_BAKE_CURVATURE}) {
        CAPTURE(static_cast<int>(map));
        CyberBakeParams params = defaults();
        params.densityNormalization = CYBER_DENSITY_RELATIVE;
        CyberImage* whole = nullptr;
        REQUIRE(cyber_bake(meshes.low, meshes.high, map, &params, &whole) == CYBER_OK);

        params.maxWorkingSetTexels = 48u * (5u + 2u * 8u);  // 5-row regions, 8-row halo
        Rows rows;
        CyberImage* report = nullptr;
        REQUIRE(cyber_bake_regions(meshes.low, meshes.high, map, &params, nullptr, collectRows,
                                   nullptr, nullptr, &rows, nullptr, &report) == CYBER_OK);
        CHECK(rows.ordered);
        CHECK(rows.nextRow == 48);
        CHECK(rows.pixels == wholePixels(whole));

        uint64_t count = 0;
        int regionRows = 0;
        int halo = 0;
        uint64_t workingSet = 0;
        REQUIRE(cyber_image_regions(report, &count, &regionRows, &halo, &workingSet) == CYBER_OK);
        CHECK(count == 10u);
        CHECK(regionRows == 5);
        CHECK(halo == 8);
        CHECK(workingSet <= params.maxWorkingSetTexels);
        // The pixel-less report carries the metadata a whole image carries.
        CHECK(cyber_image_width(report) == 48);
        CHECK(cyber_image_channels(report) == cyber_image_channels(whole));
        CHECK(cyber_image_copy_pixels(report, nullptr, 0) == 0u);
        CyberImagePadding a{};
        CyberImagePadding b{};
        REQUIRE(cyber_image_padding(whole, &a) == CYBER_OK);
        REQUIRE(cyber_image_padding(report, &b) == CYBER_OK);
        CHECK(a.texelsFilled == b.texelsFilled);
        CyberImageDensity da{};
        CyberImageDensity db{};
        REQUIRE(cyber_image_density(whole, &da) == CYBER_OK);
        REQUIRE(cyber_image_density(report, &db) == CYBER_OK);
        CHECK(da.mean == db.mean);
        CHECK(cyber_image_id_color_count(whole) == cyber_image_id_color_count(report));

        // A whole-image bake reports one region of its full height.
        REQUIRE(cyber_image_regions(whole, &count, &regionRows, &halo, &workingSet) == CYBER_OK);
        CHECK(count == 1u);
        CHECK(regionRows == 48);
        CHECK(halo == 0);
        CHECK(workingSet == 48u * 48u);
        cyber_image_free(whole);
        cyber_image_free(report);
    }
}

TEST_CASE("capi regioned bake: a stopping callback, refusals and the ceiling") {
    Meshes meshes;
    CyberBakeParams params = defaults();
    params.maxWorkingSetTexels = 48u * 21u;

    Rows stopping;
    stopping.stopAfter = 2;
    CyberImage* report = nullptr;
    CHECK(cyber_bake_regions(meshes.low, meshes.high, CYBER_BAKE_NORMAL, &params, nullptr,
                             collectRows, nullptr, nullptr, &stopping, nullptr,
                             &report) == CYBER_ERR_IO);
    CHECK(report == nullptr);
    CHECK(stopping.bands == 2);

    Rows rows;
    CHECK(cyber_bake_regions(meshes.low, meshes.high, CYBER_BAKE_NORMAL, &params, nullptr, nullptr,
                             nullptr, nullptr, &rows, nullptr, &report) == CYBER_ERR_INVALID_ARG);
    CyberBakeParams negative = params;
    negative.paddingRadius = -1;
    CHECK(cyber_bake_regions(meshes.low, meshes.high, CYBER_BAKE_NORMAL, &negative, nullptr,
                             collectRows, nullptr, nullptr, &rows, nullptr,
                             &report) == CYBER_ERR_INVALID_ARG);

    // The texel ceiling bounds the OUTPUT; the working-set bound never refuses.
    REQUIRE(cyber_set_max_bake_pixels(48u * 47u) == CYBER_OK);
    CHECK(cyber_bake_regions(meshes.low, meshes.high, CYBER_BAKE_NORMAL, &params, nullptr,
                             collectRows, nullptr, nullptr, &rows, nullptr,
                             &report) == CYBER_ERR_RUNTIME);
    REQUIRE(cyber_set_max_bake_pixels(48u * 48u) == CYBER_OK);
    params.maxWorkingSetTexels = 1;
    Rows tiny;
    REQUIRE(cyber_bake_regions(meshes.low, meshes.high, CYBER_BAKE_NORMAL, &params, nullptr,
                               collectRows, nullptr, nullptr, &tiny, nullptr, &report) == CYBER_OK);
    CHECK(tiny.bands == 48);
    cyber_image_free(report);
    REQUIRE(cyber_set_max_bake_pixels(0) == CYBER_OK);
}

TEST_CASE("capi regioned bake honours cooperative cancellation") {
    Meshes meshes;
    CyberBakeParams params = defaults();
    params.maxWorkingSetTexels = 48u * 21u;
    Rows rows;
    CyberImage* report = nullptr;
    const auto always = [](void*) { return 1; };
    CHECK(cyber_bake_regions(meshes.low, meshes.high, CYBER_BAKE_AO, &params, nullptr, collectRows,
                             nullptr, always, &rows, nullptr, &report) == CYBER_ERR_CANCELLED);
    CHECK(report == nullptr);
    CHECK(rows.bands == 0);
}

#ifdef CYBER_TESTS_HAVE_EXPORTBUNDLE
TEST_CASE("capi bundle under a working-set bound reports its regions per file") {
    Meshes meshes;
    const std::filesystem::path outDir =
        std::filesystem::temp_directory_path() / "cyber_capi_regions_bundle";
    std::error_code ec;
    std::filesystem::remove_all(outDir, ec);
    std::filesystem::create_directories(outDir, ec);

    CyberExportPreset* preset = nullptr;
    REQUIRE(cyber_export_preset_resolve("blender", &preset) == CYBER_OK);
    REQUIRE(cyber_export_preset_set_resolution(preset, 64) == CYBER_OK);
    CyberBundleParams params{};
    params.structSize = sizeof params;
    REQUIRE(cyber_default_bundle_params(&params) == CYBER_OK);
    const std::string meshOut = (outDir / "plane.obj").string();
    params.meshPath = meshOut.c_str();
    params.aoSamples = 4;
    params.cageDistance = 0.3f;
    params.maxWorkingSetTexels = 64u * 40u;

    CyberBundleResult* result = nullptr;
    REQUIRE(cyber_export_bundle_write(meshes.low, meshes.high, preset, &params, nullptr, nullptr,
                                      nullptr, &result) == CYBER_OK);
    const size_t files = cyber_bundle_result_file_count(result);
    REQUIRE(files > 1u);
    for (size_t i = 0; i < files; ++i) {
        CyberBundleFile file{};
        REQUIRE(cyber_bundle_result_file(result, i, &file) == CYBER_OK);
        uint64_t count = 0;
        int rows = 0;
        int halo = 0;
        uint64_t workingSet = 0;
        REQUIRE(cyber_bundle_result_file_regions(result, i, &count, &rows, &halo, &workingSet) ==
                CYBER_OK);
        if (std::string(file.kind) == "mesh") {
            CHECK(count == 0u);
        } else {
            CHECK(count == 8u);
            CHECK(rows == 8);
            CHECK(halo == 16);
            CHECK(workingSet == 64u * 40u);
        }
    }
    CHECK(cyber_bundle_result_file_regions(result, files, nullptr, nullptr, nullptr, nullptr) ==
          CYBER_ERR_INVALID_ARG);
    cyber_bundle_result_free(result);
    cyber_export_preset_free(preset);
    std::filesystem::remove_all(outDir, ec);
}
#endif  // CYBER_TESTS_HAVE_EXPORTBUNDLE
