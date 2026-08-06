// C ABI harness (capi module, task 13.3 partial): drives the pure-C surface
// end to end from C++ — write a cube OBJ, load it through the C entry point,
// remesh with default parameters, and assert the status, the quad output and
// that the progress callback fired.
#include <doctest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

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
    const CyberStatus status = cyber_mesh_load_obj("/nonexistent/definitely/missing.obj", &mesh);
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

namespace {
// Two-triangle unit plane in z=0 with per-corner UVs (vt) — usable as both the
// low-poly (needs UVs) and the coincident high-poly for a bake smoke test.
std::filesystem::path writeUvPlaneObj() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "cyber_capi_uvplane.obj";
    std::ofstream out(path);
    out << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
           "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
           "f 1/1 2/2 3/3\nf 1/1 3/3 4/4\n";
    return path;
}
}  // namespace

TEST_CASE("capi bakes a normal map onto a UV plane") {
    const std::filesystem::path objPath = writeUvPlaneObj();
    CyberMesh* low = nullptr;
    CyberMesh* high = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &low) == CYBER_OK);
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &high) == CYBER_OK);

    CyberBakeParams params{};
    cyber_default_bake_params(&params);
    params.width = 16;
    params.height = 16;

    CyberImage* image = nullptr;
    REQUIRE(cyber_bake(low, high, CYBER_BAKE_NORMAL, &params, &image) == CYBER_OK);
    REQUIRE(image != nullptr);
    REQUIRE(cyber_image_width(image) == 16);
    REQUIRE(cyber_image_height(image) == 16);
    REQUIRE(cyber_image_channels(image) == 3);

    const size_t needed = cyber_image_copy_pixels(image, nullptr, 0);
    REQUIRE(needed == 16u * 16u * 3u);
    std::vector<float> pixels(needed);
    REQUIRE(cyber_image_copy_pixels(image, pixels.data(), pixels.size()) == needed);
    // Coincident flat planes -> tangent-space up -> centre texel ~ (0.5,0.5,1).
    const size_t centre = (8u * 16u + 8u) * 3u;
    REQUIRE(pixels[centre + 2] == doctest::Approx(1.0f).epsilon(0.05));

    const std::filesystem::path pngPath =
        std::filesystem::temp_directory_path() / "cyber_capi_bake.png";
    REQUIRE(cyber_image_save_png(image, pngPath.string().c_str()) == CYBER_OK);
    REQUIRE(std::filesystem::exists(pngPath));

    cyber_image_free(image);
    cyber_image_free(nullptr);  // tolerated
    cyber_mesh_free(low);
    cyber_mesh_free(high);

    std::error_code ec;
    std::filesystem::remove(objPath, ec);
    std::filesystem::remove(pngPath, ec);
}

// ---- soft selection over the C ABI ----------------------------------------

namespace {

// A cols x rows lattice in the plane z = `z`, spanning x in [0, cols-1] and
// y in [0, rows-1], laid out row-major for cyber_retopo_create_grid.
std::vector<float> gridLattice(int cols, int rows, float z) {
    std::vector<float> pts;
    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < cols; ++i) {
            pts.push_back(static_cast<float>(i));
            pts.push_back(static_cast<float>(j));
            pts.push_back(z);
        }
    }
    return pts;
}

CyberMesh* makeGridMesh(int cols, int rows, float z) {
    CyberMesh* mesh = cyber_mesh_create();
    REQUIRE(mesh != nullptr);
    const std::vector<float> pts = gridLattice(cols, rows, z);
    size_t faces = 0;
    REQUIRE(cyber_retopo_create_grid(mesh, pts.data(), static_cast<size_t>(rows - 1),
                                     static_cast<size_t>(cols - 1), nullptr, &faces) == CYBER_OK);
    REQUIRE(faces == static_cast<size_t>((rows - 1) * (cols - 1)));
    return mesh;
}

std::vector<float> positionsOf(const CyberMesh* mesh) {
    std::vector<float> out(cyber_mesh_copy_positions(mesh, nullptr, 0));
    cyber_mesh_copy_positions(mesh, out.data(), out.size());
    return out;
}

}  // namespace

TEST_CASE("capi soft selection: line select, smooth, weighted transform with glue") {
    CyberMesh* target = makeGridMesh(9, 9, 0.0f);
    CyberSnapper* snapper = nullptr;
    REQUIRE(cyber_snapper_create(target, &snapper) == CYBER_OK);
    REQUIRE(snapper != nullptr);

    // EditMesh floating above the Target: a stray re-snap would be obvious.
    CyberMesh* edit = makeGridMesh(9, 9, 0.3f);
    const std::vector<float> before = positionsOf(edit);

    const float anchor[3] = {4.0f, 0.0f, 0.0f};
    const float end[3] = {8.0f, 0.0f, 0.0f};
    const float viewDir[3] = {0.0f, 0.0f, 1.0f};
    REQUIRE(cyber_retopo_selection_line(edit, anchor, end, viewDir, 1, 15.0f,
                                        CYBER_FALLOFF_LINEAR) == CYBER_OK);

    const size_t weightCount = cyber_retopo_selection_copy_weights(edit, nullptr, 0);
    REQUIRE(weightCount == cyber_mesh_vertex_count(edit));
    std::vector<float> weights(weightCount);
    REQUIRE(cyber_retopo_selection_copy_weights(edit, weights.data(), weights.size()) ==
            weightCount);
    for (const float w : weights) {
        REQUIRE(w >= 0.0f);
        REQUIRE(w <= 1.0f);
    }

    // Saving and reloading the slot round-trips the field exactly.
    REQUIRE(cyber_retopo_selection_save(edit, "taper") == CYBER_OK);
    REQUIRE(cyber_retopo_selection_slot_count(edit) == 1u);
    REQUIRE(std::string(cyber_retopo_selection_slot_name(edit, 0)) == "taper");
    REQUIRE(cyber_retopo_selection_load(edit, "nope") == CYBER_ERR_INVALID_ARG);

    REQUIRE(cyber_retopo_selection_smooth(edit, 5) == CYBER_OK);
    std::vector<float> smoothed(weightCount);
    cyber_retopo_selection_copy_weights(edit, smoothed.data(), smoothed.size());
    bool softened = false;
    for (size_t i = 0; i < weightCount; ++i) {
        softened = softened || smoothed[i] != weights[i];
    }
    REQUIRE(softened);

    REQUIRE(cyber_retopo_selection_load(edit, "taper") == CYBER_OK);
    std::vector<float> restored(weightCount);
    cyber_retopo_selection_copy_weights(edit, restored.data(), restored.size());
    REQUIRE(restored == weights);

    // Weighted translate straight down onto the Target plane.
    const float xf[12] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, -0.3f};
    CyberSoftTransformReport report{};
    REQUIRE(cyber_retopo_selection_transform(edit, xf, snapper, 0.0f, &report) == CYBER_OK);
    REQUIRE(report.moved > 0u);

    const std::vector<float> after = positionsOf(edit);
    REQUIRE(after.size() == before.size());
    size_t movedSeen = 0;
    for (size_t v = 0; v < weightCount; ++v) {
        const bool same = after[v * 3 + 0] == before[v * 3 + 0] &&
                          after[v * 3 + 1] == before[v * 3 + 1] &&
                          after[v * 3 + 2] == before[v * 3 + 2];
        if (weights[v] <= 0.0f) {
            REQUIRE(same);  // zero weight -> bit-identical, never re-snapped
        } else {
            ++movedSeen;
            // Glued to the Target plane by the transform itself.
            REQUIRE(std::fabs(after[v * 3 + 2]) < 1e-5f);
        }
    }
    REQUIRE(movedSeen == report.moved);

    cyber_snapper_free(snapper);
    cyber_mesh_free(edit);
    cyber_mesh_free(target);
}

TEST_CASE("capi soft selection: paint stroke accumulates and clear zeroes the field") {
    CyberMesh* edit = makeGridMesh(9, 3, 0.0f);
    const float centre[3] = {4.0f, 1.0f, 0.0f};
    REQUIRE(cyber_retopo_selection_paint(edit, centre, 2.5f, 0.5f, 0, CYBER_FALLOFF_SMOOTH) ==
            CYBER_OK);
    const size_t n = cyber_retopo_selection_copy_weights(edit, nullptr, 0);
    std::vector<float> once(n);
    cyber_retopo_selection_copy_weights(edit, once.data(), once.size());

    const float stroke[8] = {3.0f, 1.0f, 0.0f, 0.5f, 5.0f, 1.0f, 0.0f, 0.5f};
    REQUIRE(cyber_retopo_selection_paint_stroke(edit, stroke, 2, 2.5f, 0, CYBER_FALLOFF_SMOOTH) ==
            CYBER_OK);
    std::vector<float> accumulated(n);
    cyber_retopo_selection_copy_weights(edit, accumulated.data(), accumulated.size());
    bool grew = false;
    for (size_t i = 0; i < n; ++i) {
        REQUIRE(accumulated[i] >= once[i]);
        REQUIRE(accumulated[i] <= 1.0f);
        grew = grew || accumulated[i] > once[i];
    }
    REQUIRE(grew);

    REQUIRE(cyber_retopo_selection_clear(edit) == CYBER_OK);
    std::vector<float> cleared(n);
    cyber_retopo_selection_copy_weights(edit, cleared.data(), cleared.size());
    for (const float w : cleared) {
        REQUIRE(w == 0.0f);
    }

    REQUIRE(cyber_retopo_selection_line(nullptr, centre, centre, centre, 0, 15.0f,
                                        CYBER_FALLOFF_LINEAR) == CYBER_ERR_INVALID_ARG);
    REQUIRE(cyber_retopo_selection_sphere(edit, centre, 0.0f, CYBER_FALLOFF_LINEAR) ==
            CYBER_ERR_INVALID_ARG);
    cyber_mesh_free(edit);
}

TEST_CASE("capi seam path: route, edit, commit, resume, drop") {
    // 7 x 5 flat lattice; vertex ids follow the row-major lattice order.
    constexpr int kCols = 7;
    constexpr int kRows = 5;
    const auto vid = [](int x, int y) { return static_cast<uint32_t>(y * kCols + x); };
    CyberMesh* mesh = makeGridMesh(kCols, kRows, 0.0f);

    CyberSeamSet* seams = nullptr;
    if (cyber_seam_set_create(&seams) != CYBER_OK) {
        // Engine built without the UV module: the symbols exist but do nothing.
        MESSAGE("seam path ABI unavailable (CYBER_BUILD_UV=OFF)");
        cyber_mesh_free(mesh);
        return;
    }
    REQUIRE(seams != nullptr);

    CyberSeamPathOptions options{};
    cyber_default_seam_path_options(&options);
    REQUIRE(options.flatWeight > options.concaveWeight);
    REQUIRE(options.minWeight > 0.0f);

    CyberSeamPath* path = nullptr;
    REQUIRE(cyber_seam_path_create(mesh, &options, &path) == CYBER_OK);
    REQUIRE(path != nullptr);

    // Three waypoints across the top row, then a drag of the middle one.
    REQUIRE(cyber_seam_path_add_waypoint(path, vid(0, 0)) == 1);
    REQUIRE(cyber_seam_path_add_waypoint(path, vid(3, 0)) == 1);
    REQUIRE(cyber_seam_path_add_waypoint(path, vid(6, 0)) == 1);
    REQUIRE(cyber_seam_path_add_waypoint(path, vid(6, 0)) == 0);  // repeat rejected
    REQUIRE(cyber_seam_path_waypoint_count(path) == 3u);
    REQUIRE(cyber_seam_path_segment_count(path) == 2u);
    REQUIRE(cyber_seam_path_is_routed(path) == 1);

    const uint64_t rev0 = cyber_seam_path_segment_revision(path, 0);
    const uint64_t rev1 = cyber_seam_path_segment_revision(path, 1);
    const float drag[3] = {3.0f, 2.05f, 0.0f};
    REQUIRE(cyber_seam_path_move_waypoint_to(path, 1, drag, 0.5f) == 1);
    REQUIRE(cyber_seam_path_segment_revision(path, 0) == rev0 + 1);
    REQUIRE(cyber_seam_path_segment_revision(path, 1) == rev1 + 1);

    std::vector<uint32_t> waypoints(cyber_seam_path_waypoint_count(path));
    REQUIRE(cyber_seam_path_waypoints(path, waypoints.data(), waypoints.size()) ==
            waypoints.size());
    REQUIRE(waypoints[1] == vid(3, 2));

    // Back to the straight run, then commit.
    REQUIRE(cyber_seam_path_remove_waypoint(path, 1) == 1);
    REQUIRE(cyber_seam_path_segment_count(path) == 1u);
    const size_t chainLen = cyber_seam_path_vertices(path, nullptr, 0);
    REQUIRE(chainLen == static_cast<size_t>(kCols));
    const size_t edgeCount = cyber_seam_path_edges(path, nullptr, 0);
    REQUIRE(edgeCount == chainLen - 1);

    std::vector<uint32_t> marked(edgeCount);
    REQUIRE(cyber_seam_path_commit(path, seams, marked.data(), marked.size()) == edgeCount);
    REQUIRE(cyber_seam_set_count(seams) == edgeCount);
    for (const uint32_t e : marked) {
        REQUIRE(cyber_seam_set_is_seam(seams, e) == 1);
    }
    REQUIRE(cyber_seam_path_waypoint_count(path) == 0u);

    // Resume: the marker is the path's last vertex and seeds the next route.
    REQUIRE(cyber_seam_path_resume_marker(path) == vid(6, 0));
    REQUIRE(cyber_seam_path_add_waypoint(path, vid(6, 4)) == 1);
    REQUIRE(cyber_seam_path_waypoint_count(path) == 2u);
    std::vector<uint32_t> resumed(cyber_seam_path_segment(path, 0, nullptr, 0));
    REQUIRE(cyber_seam_path_segment(path, 0, resumed.data(), resumed.size()) == resumed.size());
    REQUIRE(resumed.front() == vid(6, 0));

    // Dropping the marker discards nothing committed.
    const size_t committed = cyber_seam_set_count(seams);
    std::vector<uint32_t> before(committed);
    REQUIRE(cyber_seam_set_edges(seams, before.data(), before.size()) == committed);
    cyber_seam_path_clear(path);
    cyber_seam_path_drop_resume_marker(path);
    REQUIRE(cyber_seam_path_resume_marker(path) == CYBER_INVALID_ID);
    std::vector<uint32_t> after(cyber_seam_set_count(seams));
    REQUIRE(cyber_seam_set_edges(seams, after.data(), after.size()) == committed);
    REQUIRE(after == before);

    // Erasing the reported ids is the undo of the commit.
    for (const uint32_t e : marked) {
        REQUIRE(cyber_seam_set_erase(seams, e) == CYBER_OK);
    }
    REQUIRE(cyber_seam_set_count(seams) == 0u);

    cyber_seam_path_free(path);
    cyber_seam_set_free(seams);
    cyber_mesh_free(mesh);
}
