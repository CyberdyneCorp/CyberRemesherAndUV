// C ABI harness (capi module, task 13.3 partial): drives the pure-C surface
// end to end from C++ — write a cube OBJ, load it through the C entry point,
// remesh with default parameters, and assert the status, the quad output and
// that the progress callback fired.
#include <doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "cyber_capi.h"

namespace {

// Unit cube as a Wavefront OBJ with six quad faces (1-indexed).
std::filesystem::path writeCubeObj() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "cyber_capi_cube.obj";
    std::ofstream out(path);
    out << "v -0.5 -0.5 -0.5\n"
           "v  0.5 -0.5 -0.5\n"
           "v  0.5  0.5 -0.5\n"
           "v -0.5  0.5 -0.5\n"
           "v -0.5 -0.5  0.5\n"
           "v  0.5 -0.5  0.5\n"
           "v  0.5  0.5  0.5\n"
           "v -0.5  0.5  0.5\n"
           "f 1 4 3 2\n"   // -Z
           "f 5 6 7 8\n"   // +Z
           "f 1 2 6 5\n"   // -Y
           "f 3 4 8 7\n"   // +Y
           "f 2 3 7 6\n"   // +X
           "f 1 5 8 4\n";  // -X
    return path;
}

void onProgress(float fraction, const char* stage, void* user) {
    REQUIRE(fraction >= 0.0f);
    REQUIRE(fraction <= 1.0001f);
    REQUIRE(stage != nullptr);
    ++*static_cast<int*>(user);
}

}  // namespace

TEST_CASE("capi version and status strings are well-formed") {
    int major = -1, minor = -1, patch = -1;
    cyber_version(&major, &minor, &patch);
    REQUIRE(major >= 0);
    REQUIRE(minor >= 0);
    REQUIRE(patch >= 0);

    REQUIRE(std::string(cyber_status_string(CYBER_OK)) == "ok");
    REQUIRE(std::string(cyber_status_string(CYBER_ERR_IO)).size() > 0);
    REQUIRE(std::string(cyber_status_string(CYBER_ERR_INVALID_PARAM)).size() > 0);
}

TEST_CASE("capi rejects null arguments") {
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(nullptr, &mesh) == CYBER_ERR_INVALID_ARG);
    REQUIRE(cyber_mesh_load_obj("x.obj", nullptr) == CYBER_ERR_INVALID_ARG);
    REQUIRE(cyber_mesh_stats(nullptr, nullptr) == CYBER_ERR_INVALID_ARG);
    REQUIRE(std::string(cyber_last_error()).size() > 0);
}

TEST_CASE("capi reports a missing OBJ as an I/O error") {
    CyberMesh* mesh = nullptr;
    const CyberStatus status =
        cyber_mesh_load_obj("/nonexistent/definitely/missing.obj", &mesh);
    REQUIRE(status == CYBER_ERR_IO);
    REQUIRE(mesh == nullptr);
    REQUIRE(std::string(cyber_last_error()).size() > 0);
}

TEST_CASE("capi loads, remeshes and reports stats for a cube") {
    const std::filesystem::path objPath = writeCubeObj();

    CyberMesh* input = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &input) == CYBER_OK);
    REQUIRE(input != nullptr);

    CyberStats inputStats{};
    REQUIRE(cyber_mesh_stats(input, &inputStats) == CYBER_OK);
    REQUIRE(inputStats.vertices == 8);
    REQUIRE(inputStats.quads == 6);
    REQUIRE(inputStats.islands == 1);

    CyberRemeshParams params{};
    cyber_default_params(&params);
    REQUIRE(params.targetQuads > 0);
    params.targetQuads = 200;  // keep the unit test fast (default 50k is a stress load)

    int progressCalls = 0;
    CyberMesh* output = nullptr;
    const CyberStatus status =
        cyber_remesh(input, &params, onProgress, nullptr, &progressCalls, &output);
    REQUIRE(status == CYBER_OK);
    REQUIRE(output != nullptr);
    REQUIRE(progressCalls > 0);

    CyberStats outStats{};
    REQUIRE(cyber_mesh_stats(output, &outStats) == CYBER_OK);
    REQUIRE(outStats.quads > 0);
    REQUIRE(outStats.vertices > 0);

    // Round-trip the remeshed result back out through the C save path.
    const std::filesystem::path savePath =
        std::filesystem::temp_directory_path() / "cyber_capi_out.obj";
    REQUIRE(cyber_mesh_save_obj(output, savePath.string().c_str()) == CYBER_OK);
    REQUIRE(std::filesystem::exists(savePath));

    cyber_mesh_free(output);
    cyber_mesh_free(input);
    cyber_mesh_free(nullptr);  // tolerated

    std::error_code ec;
    std::filesystem::remove(objPath, ec);
    std::filesystem::remove(savePath, ec);
}
