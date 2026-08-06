// C ABI harness (capi module, task 13.3 partial): drives the pure-C surface
// end to end from C++ — write a cube OBJ, load it through the C entry point,
// remesh with default parameters, and assert the status, the quad output and
// that the progress callback fired.
#include <doctest.h>

#include <algorithm>
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

// Regression (binding-gap report, item 1): CyberSoftTransformReport::moved used
// to be a CUMULATIVE per-iteration write count for the weighted relax while
// _transform reported DISTINCT vertices, so a multi-iteration relax could
// report more "moved" vertices than the mesh has. Both must report distinct
// vertices, and resnapped must never exceed moved.
TEST_CASE("capi soft relax reports distinct vertices, not per-iteration writes") {
    CyberMesh* target = makeGridMesh(9, 9, 0.0f);
    CyberSnapper* snapper = nullptr;
    REQUIRE(cyber_snapper_create(target, &snapper) == CYBER_OK);

    CyberMesh* edit = makeGridMesh(9, 9, 0.0f);
    const size_t vertices = cyber_mesh_vertex_count(edit);
    const float centre[3] = {4.0f, 4.0f, 0.0f};
    REQUIRE(cyber_retopo_selection_sphere(edit, centre, 3.0f, CYBER_FALLOFF_SMOOTH) == CYBER_OK);

    std::vector<float> weights(vertices);
    cyber_retopo_selection_copy_weights(edit, weights.data(), weights.size());
    size_t selected = 0;
    for (const float w : weights) {
        selected += w > 0.0f ? 1u : 0u;
    }
    REQUIRE(selected > 0u);

    CyberSoftTransformReport once{};
    REQUIRE(cyber_retopo_selection_relax(edit, 0.5f, 1, nullptr, 0, snapper, 0.0f, &once) ==
            CYBER_OK);
    REQUIRE(once.moved > 0u);

    // Twelve sweeps over the same region: a cumulative count would report
    // 12 * once.moved, which here exceeds the mesh's vertex count outright.
    CyberSoftTransformReport many{};
    REQUIRE(cyber_retopo_selection_relax(edit, 0.5f, 12, nullptr, 0, snapper, 0.0f, &many) ==
            CYBER_OK);
    REQUIRE(many.moved == once.moved);
    REQUIRE(many.moved <= vertices);
    REQUIRE(many.moved <= selected);
    REQUIRE(many.resnapped <= many.moved);
    REQUIRE(12u * once.moved > vertices);  // the old count would have been out of range

    cyber_snapper_free(snapper);
    cyber_mesh_free(edit);
    cyber_mesh_free(target);
}

// Regression (binding-gap report, item 2): there was no in-memory way to
// duplicate a mesh or restore a previous state, so callers round-tripped
// through save_obj/load_obj and picked up float narrowing.
TEST_CASE("capi clones a mesh handle and writes positions back") {
    CyberMesh* mesh = makeGridMesh(5, 4, 0.0f);
    const float centre[3] = {2.0f, 1.5f, 0.0f};
    REQUIRE(cyber_retopo_selection_sphere(mesh, centre, 2.0f, CYBER_FALLOFF_SMOOTH) == CYBER_OK);
    REQUIRE(cyber_retopo_selection_save(mesh, "cap") == CYBER_OK);
    const uint32_t tagged[2] = {0u, 1u};
    REQUIRE(cyber_mesh_set_tagged_edges(mesh, tagged, 2) == CYBER_OK);

    CyberMesh* copy = nullptr;
    REQUIRE(cyber_mesh_clone(mesh, &copy) == CYBER_OK);
    REQUIRE(copy != nullptr);
    REQUIRE(copy != mesh);
    REQUIRE(cyber_mesh_vertex_count(copy) == cyber_mesh_vertex_count(mesh));
    REQUIRE(cyber_mesh_face_count(copy) == cyber_mesh_face_count(mesh));
    REQUIRE(positionsOf(copy) == positionsOf(mesh));  // exact, no float narrowing

    // The whole handle rides along: overlays, weight field, saved slots.
    const size_t n = cyber_mesh_vertex_count(mesh);
    std::vector<float> original(n);
    std::vector<float> copied(n);
    cyber_retopo_selection_copy_weights(mesh, original.data(), original.size());
    cyber_retopo_selection_copy_weights(copy, copied.data(), copied.size());
    REQUIRE(copied == original);
    REQUIRE(cyber_retopo_selection_slot_count(copy) == 1u);
    REQUIRE(std::string(cyber_retopo_selection_slot_name(copy, 0)) == "cap");
    size_t taggedCount = 0;
    REQUIRE(cyber_mesh_tagged_edge_indices_ptr(copy, &taggedCount) != nullptr);

    // Editing the copy leaves the original alone.
    const std::vector<float> snapshot = positionsOf(mesh);
    std::vector<float> lifted = snapshot;
    for (size_t i = 2; i < lifted.size(); i += 3) {
        lifted[i] += 1.0f;
    }
    REQUIRE(cyber_mesh_set_positions(copy, lifted.data(), lifted.size()) == CYBER_OK);
    REQUIRE(positionsOf(copy) == lifted);
    REQUIRE(positionsOf(mesh) == snapshot);

    // ... and the snapshot restores bit-exactly through the same door.
    REQUIRE(cyber_mesh_set_positions(copy, snapshot.data(), snapshot.size()) == CYBER_OK);
    REQUIRE(positionsOf(copy) == snapshot);

    // A count mismatch is rejected and changes nothing.
    REQUIRE(cyber_mesh_set_positions(copy, snapshot.data(), snapshot.size() - 3) ==
            CYBER_ERR_INVALID_ARG);
    REQUIRE(positionsOf(copy) == snapshot);
    REQUIRE(cyber_mesh_set_positions(copy, nullptr, snapshot.size()) == CYBER_ERR_INVALID_ARG);
    REQUIRE(cyber_mesh_set_positions(nullptr, snapshot.data(), snapshot.size()) ==
            CYBER_ERR_INVALID_ARG);
    REQUIRE(cyber_mesh_clone(nullptr, &copy) == CYBER_ERR_INVALID_ARG);
    REQUIRE(cyber_mesh_clone(mesh, nullptr) == CYBER_ERR_INVALID_ARG);

    cyber_mesh_free(copy);
    cyber_mesh_free(mesh);
}

// Regression (binding-gap report, item 3): a committed seam is a set of edge
// ids, so it is undrawable without an edge -> vertex-pair accessor.
TEST_CASE("capi resolves an edge id to its endpoint positions") {
    CyberMesh* mesh = makeGridMesh(4, 3, 0.0f);
    uint32_t endpoints[2] = {0u, 0u};
    REQUIRE(cyber_mesh_edge_endpoints(mesh, 0u, endpoints) == 1);
    REQUIRE(endpoints[0] != endpoints[1]);

    float a[3] = {0.0f, 0.0f, 0.0f};
    float b[3] = {0.0f, 0.0f, 0.0f};
    REQUIRE(cyber_mesh_vertex_position(mesh, endpoints[0], a) == 1);
    REQUIRE(cyber_mesh_vertex_position(mesh, endpoints[1], b) == 1);
    const float dx = a[0] - b[0];
    const float dy = a[1] - b[1];
    const float dz = a[2] - b[2];
    REQUIRE(std::sqrt(dx * dx + dy * dy + dz * dz) > 0.0f);

    REQUIRE(cyber_mesh_edge_endpoints(mesh, 100000u, endpoints) == 0);
    REQUIRE(cyber_mesh_edge_endpoints(nullptr, 0u, endpoints) == 0);
    REQUIRE(cyber_mesh_vertex_position(mesh, 100000u, a) == 0);
    cyber_mesh_free(mesh);
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
    REQUIRE(options.flatWeight > options.convexWeight);
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

namespace {

void collectWarning(const char* message, void* user) {
    auto* sink = static_cast<std::vector<std::string>*>(user);
    sink->emplace_back(message != nullptr ? message : "");
}

// Loads the shared unit-cube OBJ through the C entry point; the file is left
// on disk for the duration of the case and removed by the caller.
CyberMesh* makeCapiBox(std::filesystem::path& objPath) {
    objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    if (cyber_mesh_load_obj(objPath.string().c_str(), &mesh) != CYBER_OK) {
        return nullptr;
    }
    return mesh;
}

}  // namespace

TEST_CASE("cyber_remesh_guided with null guidance matches cyber_remesh exactly") {
    std::filesystem::path objPath;
    CyberMesh* input = makeCapiBox(objPath);
    REQUIRE(input != nullptr);
    CyberRemeshParams params{};
    cyber_default_params(&params);
    params.targetQuads = 200;
    params.quadMethod = CYBER_QUAD_FIELD_ALIGNED;  // deterministic across builds

    CyberMesh* plain = nullptr;
    CyberMesh* guided = nullptr;
    REQUIRE(cyber_remesh(input, &params, nullptr, nullptr, nullptr, &plain) == CYBER_OK);
    REQUIRE(cyber_remesh_guided(input, &params, nullptr, nullptr, nullptr, nullptr, nullptr,
                                &guided) == CYBER_OK);
    REQUIRE(plain != nullptr);
    REQUIRE(guided != nullptr);

    CyberStats a{}, b{};
    REQUIRE(cyber_mesh_stats(plain, &a) == CYBER_OK);
    REQUIRE(cyber_mesh_stats(guided, &b) == CYBER_OK);
    CHECK(a.vertices == b.vertices);
    CHECK(a.quads == b.quads);
    CHECK(a.triangles == b.triangles);

    std::vector<float> pa(a.vertices * 3), pb(b.vertices * 3);
    cyber_mesh_copy_positions(plain, pa.data(), pa.size());
    cyber_mesh_copy_positions(guided, pb.data(), pb.size());
    CHECK(pa == pb);  // byte-for-byte: same floats, not "close enough"

    cyber_mesh_free(plain);
    cyber_mesh_free(guided);
    cyber_mesh_free(input);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

TEST_CASE("cyber_remesh_guided rejects an unusable guide and allocates nothing") {
    std::filesystem::path objPath;
    CyberMesh* input = makeCapiBox(objPath);
    REQUIRE(input != nullptr);
    CyberRemeshParams params{};
    cyber_default_params(&params);
    params.targetQuads = 200;

    const float points[] = {0.0f, 0.0f, 0.0f};  // one point: not a polyline
    CyberFlowGuide guide{};
    guide.points = points;
    guide.point_count = 1;
    guide.strength = 1.0f;
    guide.radius = 0.5f;
    CyberGuidance guidance{};
    guidance.guides = &guide;
    guidance.guide_count = 1;

    std::vector<std::string> warnings;
    CyberMesh* out = reinterpret_cast<CyberMesh*>(0x1);  // must be overwritten with null
    const CyberStatus status = cyber_remesh_guided(input, &params, &guidance, nullptr, nullptr,
                                                   collectWarning, &warnings, &out);
    CHECK(status == CYBER_ERR_INVALID_PARAM);
    CHECK(out == nullptr);
    CHECK_FALSE(warnings.empty());  // the rejection is LOUD
    cyber_mesh_free(input);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

TEST_CASE("cyber_remesh_guided reports unhonored guidance through the warning callback") {
    std::filesystem::path objPath;
    CyberMesh* input = makeCapiBox(objPath);
    REQUIRE(input != nullptr);
    CyberRemeshParams params{};
    cyber_default_params(&params);
    params.targetQuads = 200;
    // The Instant-Meshes extractor takes IQuadrangulator's default
    // acceptGuidance, which declines — the island must say so.
    params.quadMethod = CYBER_QUAD_INSTANT_MESHES;

    const float points[] = {0.1f, 0.1f, 0.0f, 0.9f, 0.9f, 0.0f};
    CyberFlowGuide guide{};
    guide.points = points;
    guide.point_count = 2;
    guide.strength = 1.0f;
    guide.radius = 0.5f;
    CyberGuidance guidance{};
    guidance.guides = &guide;
    guidance.guide_count = 1;

    std::vector<std::string> warnings;
    CyberMesh* out = nullptr;
    const CyberStatus status = cyber_remesh_guided(input, &params, &guidance, nullptr, nullptr,
                                                   collectWarning, &warnings, &out);
    REQUIRE(status == CYBER_OK);
    REQUIRE(out != nullptr);
    bool named = false;
    for (const std::string& w : warnings) {
        if (w.find("NOT honored") != std::string::npos) {
            named = true;
        }
    }
    CHECK(named);
    cyber_mesh_free(out);
    cyber_mesh_free(input);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

// ---- sculpt handoff bridge over the C ABI ---------------------------------

namespace {

// The synthetic producer, C-ABI side: the ASCII PLY profile from
// docs/sculpt-handoff-format.md, with the version left as a parameter so a
// test can emit one this engine must refuse.
std::filesystem::path writeHandoffPly(const std::string& name, int major, int minor) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path);
    out << "ply\nformat ascii 1.0\n";
    out << "comment cyber_sculpt_handoff " << major << " " << minor << "\n";
    out << "comment cyber_handoff_producer capi-test\n";
    out << "element vertex 4\n"
           "property float x\nproperty float y\nproperty float z\n"
           "property float nx\nproperty float ny\nproperty float nz\n"
           "property uchar red\nproperty uchar green\nproperty uchar blue\n"
           "property float material_mix\n"
           "element face 2\n"
           "property list uchar int vertex_indices\n"
           "end_header\n";
    out << "0 0 0 0 0 1 255 0 0 0.1\n"
           "1 0 0 0 0 1 255 0 0 0.2\n"
           "1 1 0 0 0 1 255 0 0 0.3\n"
           "0 1 0 0 0 1 255 0 0 0.4\n"
           "3 0 1 2\n3 0 2 3\n";
    return path;
}

// An analytic plane field: distance is the height above z=0.
float planeDistance(void*, const float p[3]) { return p[2]; }
void planeGradient(void*, const float[3], float out[3]) {
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 1.0f;
}
float planeOcclusion(void* user, const float[3], const float[3], float) {
    return *static_cast<const float*>(user);
}

}  // namespace

TEST_CASE("capi opens a sculpt handoff as a Target") {
    const std::filesystem::path path = writeHandoffPly("cyber_capi_handoff.ply", 1, 0);
    CyberMesh* mesh = nullptr;
    CyberHandoffInfo info{};
    REQUIRE(cyber_handoff_open(path.string().c_str(), &mesh, &info) == CYBER_OK);
    REQUIRE(mesh != nullptr);
    CHECK(info.versionMajor == CYBER_HANDOFF_VERSION_MAJOR);
    CHECK(info.versionMinor == CYBER_HANDOFF_VERSION_MINOR);
    CHECK(std::string(info.producer) == "capi-test");
    CHECK(info.vertexCount == 4u);
    CHECK(info.faceCount == 2u);
    CHECK(info.hasVertexColors == 1);
    CHECK(info.hasVertexNormals == 1);
    CHECK(info.hasMaterialMix == 1);
    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("capi rejects an unsupported handoff version and allocates nothing") {
    const std::filesystem::path path = writeHandoffPly("cyber_capi_handoff_future.ply", 2, 0);
    CyberMesh* mesh = reinterpret_cast<CyberMesh*>(0x1);  // must be overwritten with NULL
    REQUIRE(cyber_handoff_open(path.string().c_str(), &mesh, nullptr) ==
            CYBER_ERR_INCOMPATIBLE_VERSION);
    CHECK(mesh == nullptr);
    const std::string message = cyber_last_error();
    CHECK(message.find("2.0") != std::string::npos);
    CHECK(message.find("1.0") != std::string::npos);
    CHECK(std::string(cyber_status_string(CYBER_ERR_INCOMPATIBLE_VERSION)) ==
          "incompatible version");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("capi opens a handoff from buffers under the same version gate") {
    const float positions[] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    const float colors[] = {1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0};
    const float mix[] = {0.1f, 0.2f, 0.3f, 0.4f};
    const uint32_t indices[] = {0, 1, 2, 0, 2, 3};

    CyberHandoffBuffers buffers{};
    buffers.positions = positions;
    buffers.colors = colors;
    buffers.material_mix = mix;
    buffers.vertex_count = 4;
    buffers.indices = indices;
    buffers.index_count = 6;
    buffers.version_major = CYBER_HANDOFF_VERSION_MAJOR;
    buffers.version_minor = CYBER_HANDOFF_VERSION_MINOR;
    buffers.producer = "buffers";

    CyberMesh* mesh = nullptr;
    CyberHandoffInfo info{};
    REQUIRE(cyber_handoff_open_buffers(&buffers, &mesh, &info) == CYBER_OK);
    CHECK(info.vertexCount == 4u);
    CHECK(info.hasMaterialMix == 1);
    CHECK(info.hasVertexNormals == 0);  // none supplied
    CHECK(std::string(info.producer) == "buffers");
    cyber_mesh_free(mesh);

    buffers.version_major = CYBER_HANDOFF_VERSION_MAJOR + 1;
    CyberMesh* rejected = nullptr;
    CHECK(cyber_handoff_open_buffers(&buffers, &rejected, nullptr) ==
          CYBER_ERR_INCOMPATIBLE_VERSION);
    CHECK(rejected == nullptr);
}

TEST_CASE("capi bakes through a C field evaluator") {
    const std::filesystem::path objPath = writeUvPlaneObj();
    CyberMesh* low = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &low) == CYBER_OK);

    CyberBakeParams params{};
    cyber_default_bake_params(&params);
    params.width = 16;
    params.height = 16;

    float openness = 0.25f;
    CyberFieldEvaluator field{};
    field.distance = &planeDistance;
    field.gradient = &planeGradient;
    field.occlusion = &planeOcclusion;
    field.user = &openness;

    // No Target mesh at all: the field answers the normal bake on its own.
    CyberImage* normal = nullptr;
    REQUIRE(cyber_bake_field(low, nullptr, CYBER_BAKE_NORMAL, &params, &field, &normal) ==
            CYBER_OK);
    std::vector<float> pixels(cyber_image_copy_pixels(normal, nullptr, 0));
    REQUIRE(cyber_image_copy_pixels(normal, pixels.data(), pixels.size()) == pixels.size());
    const size_t centre = (8u * 16u + 8u) * 3u;
    CHECK(pixels[centre + 2] == doctest::Approx(1.0f).epsilon(0.05));
    cyber_image_free(normal);

    // AO comes straight from the evaluator's occlusion callback.
    CyberImage* ao = nullptr;
    REQUIRE(cyber_bake_field(low, nullptr, CYBER_BAKE_AO, &params, &field, &ao) == CYBER_OK);
    std::vector<float> aoPixels(cyber_image_copy_pixels(ao, nullptr, 0));
    REQUIRE(cyber_image_copy_pixels(ao, aoPixels.data(), aoPixels.size()) == aoPixels.size());
    CHECK(aoPixels[8u * 16u + 8u] == doctest::Approx(0.25f).epsilon(0.01));
    cyber_image_free(ao);

    // A map a field cannot answer still needs the Target.
    CyberImage* colorMap = nullptr;
    CHECK(cyber_bake_field(low, nullptr, CYBER_BAKE_COLOR, &params, &field, &colorMap) ==
          CYBER_ERR_INVALID_ARG);
    CHECK(colorMap == nullptr);

    cyber_mesh_free(low);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

TEST_CASE("capi conform re-snaps onto a new Target and reports max/RMS deviation") {
    // The EditMesh sits at z = 0; the new Target is the same plane lifted 0.25.
    const std::filesystem::path editPath = writeUvPlaneObj();
    const std::filesystem::path targetPath =
        std::filesystem::temp_directory_path() / "cyber_capi_conform_target.obj";
    {
        std::ofstream out(targetPath);
        out << "v 0 0 0.25\nv 1 0 0.25\nv 1 1 0.25\nv 0 1 0.25\n"
               "f 1 2 3\nf 1 3 4\n";
    }

    CyberMesh* edit = nullptr;
    CyberMesh* target = nullptr;
    REQUIRE(cyber_mesh_load_obj(editPath.string().c_str(), &edit) == CYBER_OK);
    REQUIRE(cyber_mesh_load_obj(targetPath.string().c_str(), &target) == CYBER_OK);

    const CyberStats before = [&] {
        CyberStats s{};
        REQUIRE(cyber_mesh_stats(edit, &s) == CYBER_OK);
        return s;
    }();

    CyberConformReport report{};
    std::vector<uint32_t> flagged(8, 0xffffffffu);
    REQUIRE(cyber_conform(edit, target, /*threshold=*/0.1f, &report, flagged.data(),
                          flagged.size()) == CYBER_OK);
    CHECK(report.movedVertices == 4u);
    CHECK(report.maxDeviation == doctest::Approx(0.25f).epsilon(0.02));
    CHECK(report.rmsDeviation == doctest::Approx(0.25f).epsilon(0.02));
    CHECK(report.flaggedCount == 4u);  // every vertex is past the 0.1 threshold

    // Topology preserved: the conform only ever moved positions.
    CyberStats after{};
    REQUIRE(cyber_mesh_stats(edit, &after) == CYBER_OK);
    CHECK(after.vertices == before.vertices);
    CHECK(after.triangles == before.triangles);
    CHECK(after.quads == before.quads);

    cyber_mesh_free(edit);
    cyber_mesh_free(target);
    std::error_code ec;
    std::filesystem::remove(editPath, ec);
    std::filesystem::remove(targetPath, ec);
}

// ---- named export presets ---------------------------------------------------
//
// The preset DATA half of the C ABI is available in EVERY configuration (it is
// pure core), so these cases are unconditional; only the bundle writer below is
// gated on the export-bundle module.

TEST_CASE("capi lists and resolves the built-in export presets") {
    const size_t count = cyber_export_preset_builtin_count();
    REQUIRE(count >= 4u);
    CHECK(cyber_export_preset_builtin_name(count) == nullptr);  // out of range

    bool sawBlender = false;
    bool sawUnreal = false;
    for (size_t i = 0; i < count; ++i) {
        const char* name = cyber_export_preset_builtin_name(i);
        REQUIRE(name != nullptr);

        CyberExportPreset* preset = nullptr;
        REQUIRE(cyber_export_preset_resolve(name, &preset) == CYBER_OK);
        REQUIRE(preset != nullptr);

        CyberExportPresetInfo info{};
        REQUIRE(cyber_export_preset_info(preset, &info) == CYBER_OK);
        CHECK(std::string(info.name) == name);
        CHECK(info.schemaVersion == CYBER_PRESET_SCHEMA_VERSION);
        CHECK(std::string(info.meshFormat).find('.') == std::string::npos);
        CHECK(info.resolution > 0);
        CHECK(info.mapCount > 0u);
        sawBlender = sawBlender || std::string(name) == "blender";
        sawUnreal = sawUnreal || std::string(name) == "unreal";
        // Only Unreal reads DirectX-style normals; the rest are OpenGL-style.
        CHECK(info.normalGreenPlusY == (std::string(name) == "unreal" ? 0 : 1));

        // Every map expands to a distinct file name, or one would overwrite
        // the previous one.
        std::vector<std::string> names;
        for (size_t m = 0; m < info.mapCount; ++m) {
            CyberExportPresetMap entry{};
            REQUIRE(cyber_export_preset_map(preset, m, &entry) == CYBER_OK);
            CHECK(std::string(entry.map).empty() == false);
            CHECK((std::string(entry.colorSpace) == "linear" ||
                   std::string(entry.colorSpace) == "srgb"));
            CHECK(std::string(entry.suffix).empty() == false);
            const char* fileName = cyber_export_preset_map_file_name(preset, m, "spot");
            REQUIRE(fileName != nullptr);
            const std::string expanded(fileName);
            CHECK(expanded.find("spot") != std::string::npos);
            CHECK(expanded.find('{') == std::string::npos);  // every token expanded
            CHECK(std::find(names.begin(), names.end(), expanded) == names.end());
            names.push_back(expanded);
        }
        CHECK(cyber_export_preset_map(preset, info.mapCount, nullptr) == CYBER_ERR_INVALID_ARG);
        CHECK(cyber_export_preset_map_file_name(preset, info.mapCount, "spot") == nullptr);

        // --texture-size override, and its guard.
        REQUIRE(cyber_export_preset_set_resolution(preset, 64) == CYBER_OK);
        CyberExportPresetInfo resized{};
        REQUIRE(cyber_export_preset_info(preset, &resized) == CYBER_OK);
        CHECK(resized.resolution == 64);
        CHECK(cyber_export_preset_set_resolution(preset, 0) == CYBER_ERR_INVALID_ARG);

        cyber_export_preset_free(preset);
    }
    CHECK(sawBlender);
    CHECK(sawUnreal);
    cyber_export_preset_free(nullptr);  // free(NULL) is a no-op
}

TEST_CASE("capi rejects an unknown preset name and an unsupported preset schema") {
    CyberExportPreset* preset = reinterpret_cast<CyberExportPreset*>(0x1);
    CHECK(cyber_export_preset_resolve("definitely-not-a-preset", &preset) == CYBER_ERR_INVALID_ARG);
    CHECK(preset == nullptr);
    // The message names the alternatives rather than just failing.
    CHECK(std::string(cyber_last_error()).find("blender") != std::string::npos);

    // A well-formed preset from a FUTURE schema is a contract mismatch, not a
    // parse error: it must be rejected loudly, never partially honored.
    const std::filesystem::path future =
        std::filesystem::temp_directory_path() / "cyber_capi_future_preset.json";
    {
        std::ofstream out(future);
        out << R"({"schemaVersion": 99, "name": "future", "maps": [{"map": "normal"}]})";
    }
    preset = reinterpret_cast<CyberExportPreset*>(0x1);
    CHECK(cyber_export_preset_resolve(future.string().c_str(), &preset) ==
          CYBER_ERR_INCOMPATIBLE_VERSION);
    CHECK(preset == nullptr);
    const std::string message(cyber_last_error());
    CHECK(message.find("99") != std::string::npos);
    CHECK(message.find("1") != std::string::npos);

    // A malformed preset file is an I/O-class failure, distinct from the above.
    const std::filesystem::path broken =
        std::filesystem::temp_directory_path() / "cyber_capi_broken_preset.json";
    {
        std::ofstream out(broken);
        out << R"({"schemaVersion": 1, "name": "broken"})";  // no "maps"
    }
    preset = nullptr;
    CHECK(cyber_export_preset_resolve(broken.string().c_str(), &preset) == CYBER_ERR_IO);
    CHECK(preset == nullptr);

    CHECK(cyber_export_preset_resolve(nullptr, &preset) == CYBER_ERR_INVALID_ARG);
    CHECK(cyber_export_preset_info(nullptr, nullptr) == CYBER_ERR_INVALID_ARG);

    std::error_code ec;
    std::filesystem::remove(future, ec);
    std::filesystem::remove(broken, ec);
}

#ifdef CYBER_TESTS_HAVE_EXPORTBUNDLE
TEST_CASE("capi writes an export bundle for a mesh pair") {
    // A UV'd plane as the low-poly (so the bundle does NOT need to unwrap) and
    // the same plane lifted as the projection source.
    const std::filesystem::path lowPath = writeUvPlaneObj();
    const std::filesystem::path highPath =
        std::filesystem::temp_directory_path() / "cyber_capi_bundle_high.obj";
    {
        std::ofstream out(highPath);
        out << "v 0 0 0.05\nv 1 0 0.05\nv 1 1 0.05\nv 0 1 0.05\nf 1 2 3\nf 1 3 4\n";
    }
    const std::filesystem::path outDir =
        std::filesystem::temp_directory_path() / "cyber_capi_bundle_out";
    std::error_code ec;
    std::filesystem::remove_all(outDir, ec);
    std::filesystem::create_directories(outDir, ec);

    CyberMesh* low = nullptr;
    CyberMesh* high = nullptr;
    REQUIRE(cyber_mesh_load_obj(lowPath.string().c_str(), &low) == CYBER_OK);
    REQUIRE(cyber_mesh_load_obj(highPath.string().c_str(), &high) == CYBER_OK);

    CyberExportPreset* preset = nullptr;
    REQUIRE(cyber_export_preset_resolve("blender", &preset) == CYBER_OK);
    REQUIRE(cyber_export_preset_set_resolution(preset, 16) == CYBER_OK);

    CyberBundleParams params{};
    cyber_default_bundle_params(&params);
    CHECK(params.aoSamples > 0);
    const std::string meshOut = (outDir / "plane.obj").string();
    params.meshPath = meshOut.c_str();
    params.aoSamples = 4;
    params.cageDistance = 0.2f;

    int progressCalls = 0;
    auto onProgress = [](float, const char*, void* user) { ++*static_cast<int*>(user); };

    CyberBundleResult* result = nullptr;
    REQUIRE(cyber_export_bundle_write(low, high, preset, &params, onProgress, nullptr,
                                      &progressCalls, &result) == CYBER_OK);
    REQUIRE(result != nullptr);
    CHECK(progressCalls > 0);

    // The low-poly already had UVs, so nothing was unwrapped.
    CHECK(cyber_bundle_result_unwrapped(result) == 0);
    CHECK(cyber_bundle_result_chart_count(result) == 0);

    CyberExportPresetInfo info{};
    REQUIRE(cyber_export_preset_info(preset, &info) == CYBER_OK);
    const size_t fileCount = cyber_bundle_result_file_count(result);
    CHECK(fileCount == info.mapCount + 1u);  // one mesh plus one file per map

    bool sawMesh = false;
    for (size_t i = 0; i < fileCount; ++i) {
        CyberBundleFile file{};
        REQUIRE(cyber_bundle_result_file(result, i, &file) == CYBER_OK);
        CHECK(std::filesystem::exists(file.path));
        if (std::string(file.kind) == "mesh") {
            sawMesh = true;
            CHECK(file.width == 0);
        } else {
            // Every map honours the resolution override.
            CHECK(file.width == 16);
            CHECK(file.height == 16);
            CHECK((std::string(file.colorSpace) == "linear" ||
                   std::string(file.colorSpace) == "srgb"));
        }
    }
    CHECK(sawMesh);
    CHECK(cyber_bundle_result_file(result, fileCount, nullptr) == CYBER_ERR_INVALID_ARG);
    CHECK(cyber_bundle_result_warning(result, cyber_bundle_result_warning_count(result)) ==
          nullptr);

    cyber_bundle_result_free(result);

    // Null-argument contract: the output pointer is always cleared first.
    result = reinterpret_cast<CyberBundleResult*>(0x1);
    CHECK(cyber_export_bundle_write(low, nullptr, preset, &params, nullptr, nullptr, nullptr,
                                    &result) == CYBER_ERR_INVALID_ARG);
    CHECK(result == nullptr);
    CHECK(cyber_export_bundle_write(nullptr, high, preset, &params, nullptr, nullptr, nullptr,
                                    &result) == CYBER_ERR_INVALID_ARG);

    cyber_export_preset_free(preset);
    cyber_mesh_free(low);
    cyber_mesh_free(high);
    std::filesystem::remove(lowPath, ec);
    std::filesystem::remove(highPath, ec);
    std::filesystem::remove_all(outDir, ec);
}

TEST_CASE("capi export bundle unwraps a low-poly that carries no UVs") {
    // No vt in the low-poly: baking is impossible without UVs, so the bundle
    // unwraps it IN PLACE rather than refusing.
    const std::filesystem::path lowPath =
        std::filesystem::temp_directory_path() / "cyber_capi_bundle_nouv.obj";
    {
        std::ofstream out(lowPath);
        out << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3\nf 1 3 4\n";
    }
    const std::filesystem::path outDir =
        std::filesystem::temp_directory_path() / "cyber_capi_bundle_nouv_out";
    std::error_code ec;
    std::filesystem::remove_all(outDir, ec);
    std::filesystem::create_directories(outDir, ec);

    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(lowPath.string().c_str(), &mesh) == CYBER_OK);
    CyberExportPreset* preset = nullptr;
    REQUIRE(cyber_export_preset_resolve("gltf-generic", &preset) == CYBER_OK);
    REQUIRE(cyber_export_preset_set_resolution(preset, 16) == CYBER_OK);

    CyberBundleParams params{};
    cyber_default_bundle_params(&params);
    const std::string meshOut = (outDir / "plane.ply").string();
    params.meshPath = meshOut.c_str();
    params.basename = "custom";
    params.aoSamples = 4;
    params.cageDistance = 0.2f;

    CyberBundleResult* result = nullptr;
    REQUIRE(cyber_export_bundle_write(mesh, mesh, preset, &params, nullptr, nullptr, nullptr,
                                      &result) == CYBER_OK);
    REQUIRE(result != nullptr);
    CHECK(cyber_bundle_result_unwrapped(result) == 1);
    CHECK(cyber_bundle_result_chart_count(result) > 0);

    // The preset declares glb but the output path says ply: the explicit path
    // wins and the mismatch is reported, never silently resolved.
    bool sawMismatch = false;
    for (size_t i = 0; i < cyber_bundle_result_warning_count(result); ++i) {
        const char* warning = cyber_bundle_result_warning(result, i);
        REQUIRE(warning != nullptr);
        sawMismatch = sawMismatch || std::string(warning).find("ply") != std::string::npos;
    }
    CHECK(sawMismatch);

    // `basename` drove the map file names.
    for (size_t i = 0; i < cyber_bundle_result_file_count(result); ++i) {
        CyberBundleFile file{};
        REQUIRE(cyber_bundle_result_file(result, i, &file) == CYBER_OK);
        if (std::string(file.kind) != "mesh") {
            CHECK(std::filesystem::path(file.path).filename().string().rfind("custom", 0) == 0u);
        }
    }

    cyber_bundle_result_free(result);
    cyber_export_preset_free(preset);
    cyber_mesh_free(mesh);
    std::filesystem::remove(lowPath, ec);
    std::filesystem::remove_all(outDir, ec);
}
#endif  // CYBER_TESTS_HAVE_EXPORTBUNDLE
