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
#include <iterator>
#include <limits>
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

TEST_CASE("capi backend selection reports what it actually selected") {
    // The C ABI used to expose no backend selection at all: a host embedding
    // libcyber_capi could only steer the engine with the CYBER_BACKEND
    // environment variable, which has to be set before the first accelerated
    // call and cannot be changed afterwards.
    const size_t count = cyber_available_backends(nullptr, 0);
    REQUIRE(count >= 1);  // the CPU backend is always present

    std::vector<CyberBackend> backends(count, CYBER_BACKEND_AUTO);
    REQUIRE(cyber_available_backends(backends.data(), backends.size()) == count);
    REQUIRE(backends.back() == CYBER_BACKEND_CPU);  // always last, always there
    for (const CyberBackend backend : backends) {
        REQUIRE(backend != CYBER_BACKEND_AUTO);  // AUTO is not a device
    }

    // A short buffer fills what fits and still reports the true total.
    CyberBackend first = CYBER_BACKEND_AUTO;
    REQUIRE(cyber_available_backends(&first, 1) == count);
    REQUIRE(first == backends.front());

    // Every reported backend is selectable, and the selection is observable.
    for (const CyberBackend backend : backends) {
        REQUIRE(cyber_set_backend(backend) == CYBER_OK);
        REQUIRE(cyber_active_backend() == backend);
        REQUIRE(std::string(cyber_active_backend_name()).size() > 0);
    }

    // An absent backend is REFUSED, never silently downgraded to the CPU:
    // selectBackend() falls back to CPU internally, so reporting CYBER_OK here
    // would tell a host it is on the GPU while it runs on the CPU.
    for (const CyberBackend absent : {CYBER_BACKEND_METAL, CYBER_BACKEND_CUDA,
                                      CYBER_BACKEND_OPENCL}) {
        if (std::find(backends.begin(), backends.end(), absent) != backends.end()) {
            continue;
        }
        REQUIRE(cyber_set_backend(CYBER_BACKEND_CPU) == CYBER_OK);
        REQUIRE(cyber_set_backend(absent) == CYBER_ERR_INVALID_ARG);
        // The message first: every successful call clears the error slot.
        REQUIRE(std::string(cyber_last_error()).find("not available") != std::string::npos);
        REQUIRE(cyber_active_backend() == CYBER_BACKEND_CPU);  // selection untouched
    }

    // A value with no enumerator (a C caller passes an int) is rejected rather
    // than mapped onto a device. 7 is the largest value the enum's underlying
    // range admits, so the cast itself stays well-defined.
    REQUIRE(cyber_set_backend(static_cast<CyberBackend>(7)) == CYBER_ERR_INVALID_ARG);

    // AUTO restores the automatic choice, which always resolves to one of the
    // available devices (which one depends on CYBER_BACKEND in the env).
    REQUIRE(cyber_set_backend(CYBER_BACKEND_AUTO) == CYBER_OK);
    REQUIRE(cyber_active_backend() != CYBER_BACKEND_AUTO);
    REQUIRE(std::find(backends.begin(), backends.end(), cyber_active_backend()) != backends.end());
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

// Regression: the C ABI built the default quad-cover extractor without the
// feature angle, so params.sharpEdgeDegrees never reached the parameterization
// and every binding ran at the factory's 40 degrees — the ABI's own documented
// default of 90 could not take effect, and two runs differing only in the knob
// returned bit-identical geometry while the CLI's diverged.
TEST_CASE("capi remesh honours sharpEdgeDegrees on the default quad-cover path") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* input = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &input) == CYBER_OK);

    const auto remeshAt = [input](float sharpDegrees) {
        CyberRemeshParams params{};
        cyber_default_params(&params);
        params.targetQuads = 200;  // keep the unit test fast
        params.sharpEdgeDegrees = sharpDegrees;
        CyberMesh* output = nullptr;
        REQUIRE(cyber_remesh(input, &params, nullptr, nullptr, nullptr, &output) == CYBER_OK);
        REQUIRE(output != nullptr);
        std::vector<float> positions(cyber_mesh_copy_positions(output, nullptr, 0));
        cyber_mesh_copy_positions(output, positions.data(), positions.size());
        cyber_mesh_free(output);
        return positions;
    };

    // A cube's creases are features at 40 degrees and not at 90, so the two
    // runs cannot agree unless the value is being dropped on the way in.
    CHECK(remeshAt(40.0f) != remeshAt(90.0f));
    // Same value twice is still deterministic (the knob, not run-to-run noise).
    CHECK(remeshAt(40.0f) == remeshAt(40.0f));

    cyber_mesh_free(input);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
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

// Regression: vertex ids are recycled from the mesh's free list, so the weight
// of an erased vertex was still in the field when the next create_face handed
// that id to a brand-new vertex — the weighted transform then dragged geometry
// the user had never selected, and the saved slot resurrected it after a load.
TEST_CASE("capi soft selection: an erased vertex cannot pass its weight to a new one") {
    CyberMesh* edit = cyber_mesh_create();
    REQUIRE(edit != nullptr);
    const float quad[12] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                            1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    uint32_t face = 0;
    REQUIRE(cyber_retopo_create_face(edit, quad, 4, nullptr, &face) == CYBER_OK);

    const float centre[3] = {0.5f, 0.5f, 0.0f};
    REQUIRE(cyber_retopo_selection_sphere(edit, centre, 3.0f, CYBER_FALLOFF_SMOOTH) == CYBER_OK);
    REQUIRE(cyber_retopo_selection_save(edit, "quad") == CYBER_OK);
    std::vector<float> selected(cyber_retopo_selection_copy_weights(edit, nullptr, 0));
    cyber_retopo_selection_copy_weights(edit, selected.data(), selected.size());
    REQUIRE(selected.size() == 4u);
    for (const float w : selected) {
        REQUIRE(w > 0.0f);
    }

    size_t removed = 0;
    REQUIRE(cyber_retopo_erase(edit, centre, 1.0f, 1.0f, &removed) == CYBER_OK);
    REQUIRE(removed == 1u);
    REQUIRE(cyber_mesh_vertex_count(edit) == 0u);

    // The four freed ids come straight back to this quad, a hundred units away.
    const float faraway[12] = {100.0f, 100.0f, 0.0f, 101.0f, 100.0f, 0.0f,
                               101.0f, 101.0f, 0.0f, 100.0f, 101.0f, 0.0f};
    REQUIRE(cyber_retopo_create_face(edit, faraway, 4, nullptr, &face) == CYBER_OK);
    std::vector<float> reused(cyber_retopo_selection_copy_weights(edit, nullptr, 0));
    cyber_retopo_selection_copy_weights(edit, reused.data(), reused.size());
    for (const float w : reused) {
        REQUIRE(w == 0.0f);
    }

    const std::vector<float> before = positionsOf(edit);
    const float xf[12] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 10.0f};
    CyberSoftTransformReport report{};
    REQUIRE(cyber_retopo_selection_transform(edit, xf, nullptr, 0.0f, &report) == CYBER_OK);
    REQUIRE(report.moved == 0u);
    REQUIRE(positionsOf(edit) == before);

    // The persisted slot must be swept too, or a load brings the ghost back.
    REQUIRE(cyber_retopo_selection_load(edit, "quad") == CYBER_OK);
    REQUIRE(cyber_retopo_selection_transform(edit, xf, nullptr, 0.0f, &report) == CYBER_OK);
    REQUIRE(report.moved == 0u);
    REQUIRE(positionsOf(edit) == before);

    cyber_mesh_free(edit);
}

// Regression: the weight sweep above covered only ONE of the handle's id-keyed
// tables. Face ids are recycled the same way, so the hidden set still held the
// deleted face's id when create_face handed it out again — the face the artist
// had just drawn was born hidden and never appeared, with CYBER_OK throughout.
TEST_CASE("capi overlays: a deleted face cannot pass its hidden flag to a new one") {
    CyberMesh* mesh = cyber_mesh_create();
    REQUIRE(mesh != nullptr);
    const float first[12] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                             1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const float second[12] = {2.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f,
                              3.0f, 1.0f, 0.0f, 2.0f, 1.0f, 0.0f};
    uint32_t a = 0, b = 0;
    REQUIRE(cyber_retopo_create_face(mesh, first, 4, nullptr, &a) == CYBER_OK);
    REQUIRE(cyber_retopo_create_face(mesh, second, 4, nullptr, &b) == CYBER_OK);

    size_t indices = 0;
    REQUIRE(cyber_mesh_triangle_indices_ptr(mesh, &indices) != nullptr);
    REQUIRE(indices == 12u);  // two quads, fan-triangulated

    REQUIRE(cyber_mesh_set_hidden_faces(mesh, &b, 1) == CYBER_OK);
    cyber_mesh_triangle_indices_ptr(mesh, &indices);
    REQUIRE(indices == 6u);

    size_t removed = 0;
    REQUIRE(cyber_retopo_delete_faces(mesh, &b, 1, &removed) == CYBER_OK);
    REQUIRE(removed == 1u);
    REQUIRE(cyber_mesh_hidden_face_count(mesh) == 0u);  // the dead id is swept

    // The freed face id comes straight back to a face somewhere else entirely.
    const float third[12] = {5.0f, 0.0f, 0.0f, 6.0f, 0.0f, 0.0f,
                             6.0f, 1.0f, 0.0f, 5.0f, 1.0f, 0.0f};
    uint32_t c = 0;
    REQUIRE(cyber_retopo_create_face(mesh, third, 4, nullptr, &c) == CYBER_OK);
    REQUIRE(c == b);  // the id really is recycled
    cyber_mesh_triangle_indices_ptr(mesh, &indices);
    REQUIRE(indices == 12u);
    REQUIRE(cyber_mesh_hidden_face_count(mesh) == 0u);

    cyber_mesh_free(mesh);
}

// Regression: same class on the edge-keyed table — a tag left on a deleted
// edge id resurfaced as a seam over the brand-new edge that inherited the id.
TEST_CASE("capi overlays: a deleted edge cannot pass its tag to a new one") {
    CyberMesh* mesh = cyber_mesh_create();
    REQUIRE(mesh != nullptr);
    const float quad[12] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                            1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    uint32_t face = 0;
    REQUIRE(cyber_retopo_create_face(mesh, quad, 4, nullptr, &face) == CYBER_OK);

    const uint32_t tagged[1] = {0u};
    REQUIRE(cyber_mesh_set_tagged_edges(mesh, tagged, 1) == CYBER_OK);
    size_t count = 0;
    REQUIRE(cyber_mesh_tagged_edge_indices_ptr(mesh, &count) != nullptr);
    REQUIRE(count == 2u);

    size_t removed = 0;
    REQUIRE(cyber_retopo_delete_faces(mesh, &face, 1, &removed) == CYBER_OK);
    REQUIRE(removed == 1u);
    REQUIRE(cyber_mesh_vertex_count(mesh) == 0u);

    // A quad drawn a hundred units away reuses the freed edge ids, id 0 among
    // them: nothing here was ever tagged, so nothing may render as a seam.
    const float faraway[12] = {100.0f, 100.0f, 0.0f, 101.0f, 100.0f, 0.0f,
                               101.0f, 101.0f, 0.0f, 100.0f, 101.0f, 0.0f};
    REQUIRE(cyber_retopo_create_face(mesh, faraway, 4, nullptr, &face) == CYBER_OK);
    count = 1;
    REQUIRE(cyber_mesh_tagged_edge_indices_ptr(mesh, &count) == nullptr);
    REQUIRE(count == 0u);

    cyber_mesh_free(mesh);
}

// Regression: cyber_retopo_selection_relax copied `strength` into RelaxParams
// unvalidated while its sibling cyber_retopo_relax rejects anything outside
// [0,1], so a NaN slider value NaN'd every selected vertex and returned
// CYBER_OK with a plausible moved count.
TEST_CASE("capi soft relax rejects an out-of-range strength like the plain relax does") {
    CyberMesh* edit = makeGridMesh(5, 5, 0.0f);
    const float centre[3] = {2.0f, 2.0f, 0.0f};
    REQUIRE(cyber_retopo_selection_sphere(edit, centre, 3.0f, CYBER_FALLOFF_SMOOTH) == CYBER_OK);
    const std::vector<float> before = positionsOf(edit);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float bad[] = {nan, 2.0f, -1.0f};
    CyberSoftTransformReport report{};
    for (const float strength : bad) {
        REQUIRE(cyber_retopo_selection_relax(edit, strength, 4, nullptr, 0, nullptr, 0.0f,
                                             &report) == CYBER_ERR_INVALID_PARAM);
        REQUIRE(positionsOf(edit) == before);  // bit-identical, like the sibling
    }

    // The accepted range still relaxes.
    REQUIRE(cyber_retopo_selection_relax(edit, 0.5f, 1, nullptr, 0, nullptr, 0.0f, &report) ==
            CYBER_OK);

    cyber_mesh_free(edit);
}

// Regression: the brush's coverage test was `d >= radius`, false for a NaN
// distance, so a dab with a non-finite center covered every vertex whatever the
// radius and the sanitized NaN zeroed the whole painted field — with CYBER_OK.
TEST_CASE("capi soft selection: a non-finite paint dab is refused, never applied") {
    CyberMesh* edit = makeGridMesh(9, 3, 0.0f);
    const float centre[3] = {4.0f, 1.0f, 0.0f};
    REQUIRE(cyber_retopo_selection_paint(edit, centre, 2.5f, 1.0f, 0, CYBER_FALLOFF_SMOOTH) ==
            CYBER_OK);
    const size_t n = cyber_retopo_selection_copy_weights(edit, nullptr, 0);
    std::vector<float> painted(n);
    cyber_retopo_selection_copy_weights(edit, painted.data(), painted.size());
    bool anyPainted = false;
    for (const float w : painted) {
        anyPainted = anyPainted || w > 0.0f;
    }
    REQUIRE(anyPainted);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float nowhere[3] = {nan, nan, nan};
    std::vector<float> after(n);
    // A 0.5 radius can touch at most one vertex of a unit grid, so a wiped
    // field could only have come from the NaN.
    REQUIRE(cyber_retopo_selection_paint(edit, nowhere, 0.5f, 1.0f, 0, CYBER_FALLOFF_SMOOTH) ==
            CYBER_ERR_INVALID_ARG);
    cyber_retopo_selection_copy_weights(edit, after.data(), after.size());
    REQUIRE(after == painted);
    REQUIRE(cyber_retopo_selection_paint(edit, centre, 0.5f, nan, 0, CYBER_FALLOFF_SMOOTH) ==
            CYBER_ERR_INVALID_ARG);
    cyber_retopo_selection_copy_weights(edit, after.data(), after.size());
    REQUIRE(after == painted);

    // One bad sample only drops itself: the rest of the stroke still paints.
    const float stroke[8] = {nan, nan, nan, 1.0f, 3.0f, 1.0f, 0.0f, 0.5f};
    REQUIRE(cyber_retopo_selection_paint_stroke(edit, stroke, 2, 2.5f, 0, CYBER_FALLOFF_SMOOTH) ==
            CYBER_OK);
    cyber_retopo_selection_copy_weights(edit, after.data(), after.size());
    bool grew = false;
    for (size_t i = 0; i < n; ++i) {
        REQUIRE(after[i] >= painted[i]);
        grew = grew || after[i] > painted[i];
    }
    REQUIRE(grew);

    cyber_mesh_free(edit);
}

// Regression: the paint dab got its non-finite gate, but the two region ops
// that REPLACE the selection did not — `radius <= 0` and the zero-length test
// are both false for NaN, so a ray-miss unprojection wiped the user's field and
// returned CYBER_OK.
TEST_CASE("capi soft selection: a non-finite line or sphere region is refused, never applied") {
    CyberMesh* edit = makeGridMesh(9, 9, 0.0f);
    const float centre[3] = {4.0f, 4.0f, 0.0f};
    REQUIRE(cyber_retopo_selection_sphere(edit, centre, 3.0f, CYBER_FALLOFF_SMOOTH) == CYBER_OK);
    const size_t n = cyber_retopo_selection_copy_weights(edit, nullptr, 0);
    std::vector<float> selected(n);
    cyber_retopo_selection_copy_weights(edit, selected.data(), selected.size());
    bool anySelected = false;
    for (const float w : selected) {
        anySelected = anySelected || w > 0.0f;
    }
    REQUIRE(anySelected);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float nowhere[3] = {nan, nan, nan};
    const float end[3] = {8.0f, 4.0f, 0.0f};
    const float viewDir[3] = {0.0f, 0.0f, 1.0f};
    std::vector<float> after(n);
    const auto weightsUnchanged = [&] {
        cyber_retopo_selection_copy_weights(edit, after.data(), after.size());
        return after == selected;
    };

    CHECK(cyber_retopo_selection_sphere(edit, nowhere, 3.0f, CYBER_FALLOFF_SMOOTH) ==
          CYBER_ERR_INVALID_ARG);
    CHECK(weightsUnchanged());
    CHECK(cyber_retopo_selection_sphere(edit, centre, nan, CYBER_FALLOFF_SMOOTH) ==
          CYBER_ERR_INVALID_ARG);
    CHECK(weightsUnchanged());
    CHECK(cyber_retopo_selection_line(edit, nowhere, end, viewDir, 0, 15.0f,
                                      CYBER_FALLOFF_SMOOTH) == CYBER_ERR_INVALID_ARG);
    CHECK(weightsUnchanged());
    CHECK(cyber_retopo_selection_line(edit, centre, nowhere, viewDir, 0, 15.0f,
                                      CYBER_FALLOFF_SMOOTH) == CYBER_ERR_INVALID_ARG);
    CHECK(weightsUnchanged());
    CHECK(cyber_retopo_selection_line(edit, centre, end, nowhere, 0, 15.0f,
                                      CYBER_FALLOFF_SMOOTH) == CYBER_ERR_INVALID_ARG);
    CHECK(weightsUnchanged());
    // snap_degrees only steers the line when snapping is on, so only then is a
    // non-finite value a refusal.
    CHECK(cyber_retopo_selection_line(edit, centre, end, viewDir, 1, nan, CYBER_FALLOFF_SMOOTH) ==
          CYBER_ERR_INVALID_ARG);
    CHECK(weightsUnchanged());
    CHECK(cyber_retopo_selection_line(edit, centre, end, viewDir, 0, nan, CYBER_FALLOFF_SMOOTH) ==
          CYBER_OK);

    cyber_mesh_free(edit);
}

// Regression: cyber_mesh_copy_positions keeps every live vertex (it is what
// cyber_mesh_set_positions writes back) while the render buffers drop vertices
// used only by hidden faces, so a renderer that took its positions from
// copy_positions indexed the shorter compaction and sheared. The filtered
// stream now has its own accessor.
TEST_CASE("capi render positions follow the visibility filter that copy_positions does not") {
    CyberMesh* mesh = makeGridMesh(3, 3, 0.0f);
    const size_t vertexFloats = cyber_mesh_copy_positions(mesh, nullptr, 0);
    REQUIRE(vertexFloats == 27u);  // 9 vertices
    REQUIRE(cyber_mesh_copy_render_positions(mesh, nullptr, 0) == vertexFloats);

    const uint32_t hidden[1] = {0u};
    REQUIRE(cyber_mesh_set_hidden_faces(mesh, hidden, 1) == CYBER_OK);

    // The corner vertex is used only by the hidden face: it leaves the render
    // order, and every render stream shortens together.
    const size_t renderFloats = cyber_mesh_copy_render_positions(mesh, nullptr, 0);
    CHECK(renderFloats == vertexFloats - 3u);
    CHECK(cyber_mesh_copy_normals(mesh, nullptr, 0) == renderFloats);
    CHECK(cyber_mesh_copy_positions(mesh, nullptr, 0) == vertexFloats);  // set_positions' order

    // ... and it is the very buffer the pointer view exposes.
    std::vector<float> copied(renderFloats);
    REQUIRE(cyber_mesh_copy_render_positions(mesh, copied.data(), copied.size()) == renderFloats);
    size_t viewCount = 0;
    const float* view = cyber_mesh_positions_ptr(mesh, &viewCount);
    REQUIRE(view != nullptr);
    REQUIRE(viewCount == renderFloats);
    CHECK(std::equal(copied.begin(), copied.end(), view));

    CHECK(cyber_mesh_copy_render_positions(nullptr, nullptr, 0) == 0u);

    cyber_mesh_free(mesh);
}

// Regression: cyber_mesh_loop_metrics was the one status-returning entry point
// that neither cleared the thread-local error on success nor set it on failure,
// so a shell logging cyber_last_error() after it reported an unrelated older
// failure — or nothing at all.
TEST_CASE("capi loop metrics keeps the error slot in step with its status") {
    CyberMesh* stale = nullptr;
    REQUIRE(cyber_mesh_load_obj("/nonexistent/definitely/missing.obj", &stale) == CYBER_ERR_IO);
    REQUIRE(std::string(cyber_last_error()).size() > 0);  // the unrelated message

    CyberMesh* mesh = makeGridMesh(4, 4, 0.0f);
    CyberLoopMetrics metrics{};
    REQUIRE(cyber_mesh_loop_metrics(mesh, 0u, nullptr, &metrics) == CYBER_OK);
    CHECK(std::string(cyber_last_error()).empty());

    CHECK(cyber_mesh_loop_metrics(nullptr, 0u, nullptr, &metrics) == CYBER_ERR_INVALID_ARG);
    CHECK(std::string(cyber_last_error()).find("loop_metrics") != std::string::npos);
    CHECK(cyber_mesh_loop_metrics(mesh, 0u, nullptr, nullptr) == CYBER_ERR_INVALID_ARG);
    CHECK(std::string(cyber_last_error()).find("loop_metrics") != std::string::npos);

    cyber_mesh_free(mesh);
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

namespace {

// Three triangles sharing the edge (v0, v1): an ordinary non-manifold fan,
// the shape the engine tags rather than rejects.
std::filesystem::path writeNonManifoldFanObj() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "cyber_capi_fan.obj";
    std::ofstream out(path);
    out << "v 0 0 0\n"
           "v 1 0 0\n"
           "v 0 1 0\n"
           "v 0 -1 0\n"
           "v 0 0 1\n"
           "f 1 2 3\n"
           "f 2 1 4\n"
           "f 1 2 5\n";
    return path;
}

}  // namespace

// Regression: the count reported here is a loop bound for the caller, and the
// prototype declares two-element arrays — so a non-manifold edge must not send
// that loop past the end of the buffers the signature asked for.
TEST_CASE("capi edge faces never reports more entries than its out arrays hold") {
    const std::filesystem::path objPath = writeNonManifoldFanObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);

    uint32_t nonManifold = CYBER_INVALID_ID;
    for (uint32_t edge = 0; edge < 32u; ++edge) {
        if (cyber_mesh_edge_face_count(mesh, edge) >= 3) {
            nonManifold = edge;
            break;
        }
    }
    REQUIRE(nonManifold != CYBER_INVALID_ID);
    CHECK(cyber_mesh_edge_face_count(mesh, nonManifold) == 3);

    // Sentinels one past the declared extents: nothing may be written there,
    // and the return must not invite the caller to read them.
    constexpr uint32_t kFaceGuard = 0xABCDEFu;
    constexpr size_t kSizeGuard = 12345u;
    uint32_t faces[3] = {kFaceGuard, kFaceGuard, kFaceGuard};
    size_t sizes[3] = {kSizeGuard, kSizeGuard, kSizeGuard};
    const int written = cyber_mesh_edge_faces(mesh, nonManifold, faces, sizes);
    CHECK(written == 2);
    CHECK(faces[2] == kFaceGuard);
    CHECK(sizes[2] == kSizeGuard);
    for (int i = 0; i < written; ++i) {
        CHECK(faces[static_cast<size_t>(i)] != kFaceGuard);
        CHECK(sizes[static_cast<size_t>(i)] == 3u);
    }

    // Manifold and boundary edges are unaffected: written count == true count.
    for (uint32_t edge = 0; edge < 32u; ++edge) {
        const int total = cyber_mesh_edge_face_count(mesh, edge);
        if (total < 0 || total > 2) {
            continue;
        }
        CHECK(cyber_mesh_edge_faces(mesh, edge, faces, sizes) == total);
    }

    CHECK(cyber_mesh_edge_face_count(mesh, 100000u) == -1);
    CHECK(cyber_mesh_edge_face_count(nullptr, 0u) == -1);
    CHECK(cyber_mesh_edge_faces(mesh, 100000u, faces, sizes) == -1);

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

// Regression: cyber_uv_atlas cleared the error slot and then returned a
// failure status, and the header defines an empty slot as "the last call
// succeeded" — a failure must always carry a message.
TEST_CASE("capi uv atlas failure always leaves a message in the error slot") {
    CyberMesh* empty = cyber_mesh_create();
    REQUIRE(empty != nullptr);
    CyberAtlasResult result{};
    const CyberStatus status = cyber_uv_atlas(empty, nullptr, &result);
    CHECK(status != CYBER_OK);
    CHECK(std::string(cyber_last_error()).find("cyber_uv_atlas") != std::string::npos);
    cyber_mesh_free(empty);
}

// Regression: the atlas's default chart-merge cap trial-unwraps candidate chart
// unions and runs for minutes on ordinary assets, and cyber_uv_atlas gave a host
// no way out of it. The cancellable entry point must actually stop, and must not
// leave a half-written atlas behind.
TEST_CASE("capi cancellable uv atlas aborts and leaves the mesh untouched") {
    const std::filesystem::path objPath = writeCubeObj();
    CyberMesh* mesh = nullptr;
    REQUIRE(cyber_mesh_load_obj(objPath.string().c_str(), &mesh) == CYBER_OK);
    REQUIRE(mesh != nullptr);

    int calls = 0;
    CyberAtlasResult result{};
    const CyberStatus status = cyber_uv_atlas_cancellable(
        mesh, nullptr, nullptr,
        [](void* user) {
            ++*static_cast<int*>(user);
            return 1;
        },
        &calls, &result);
    CHECK(status == CYBER_ERR_CANCELLED);
    CHECK(calls > 0);
    CHECK(std::string(cyber_last_error()).find("cancelled") != std::string::npos);
    // Nothing was written, so the mesh still has no UVs and the atlas that runs
    // afterwards on the same handle succeeds normally.
    CHECK(cyber_uv_atlas(mesh, nullptr, &result) == CYBER_OK);
    CHECK(result.chartCount > 0);

    cyber_mesh_free(mesh);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
}

#ifdef CYBER_CAPI_HEADER_PATH
// Regression: the header advertised "SNAPSHOT SEMANTICS, as for CyberSnapper"
// for a handle that BORROWS the mesh, so a binding author following the
// CyberSnapper analogy releases the mesh first and gets a use-after-free.
// CyberSnapper really does copy; CyberSeamPath must document the borrow.
TEST_CASE("capi header documents the seam path's borrowed mesh lifetime") {
    std::ifstream header(CYBER_CAPI_HEADER_PATH);
    REQUIRE(header.good());
    const std::string text((std::istreambuf_iterator<char>(header)),
                           std::istreambuf_iterator<char>());

    const size_t decl = text.find("typedef struct CyberSeamPath CyberSeamPath;");
    REQUIRE(decl != std::string::npos);
    const size_t blockStart = text.rfind("/*", decl);
    REQUIRE(blockStart != std::string::npos);
    const std::string block = text.substr(blockStart, decl - blockStart);

    CHECK(block.find("as for CyberSnapper") == std::string::npos);
    CHECK(block.find("SNAPSHOT SEMANTICS") == std::string::npos);
    CHECK(block.find("outlive") != std::string::npos);

    const size_t create = text.find("CyberStatus cyber_seam_path_create(");
    REQUIRE(create != std::string::npos);
    const size_t createDoc = text.rfind("/*", create);
    REQUIRE(createDoc != std::string::npos);
    CHECK(text.substr(createDoc, create - createDoc).find("BORROWS") != std::string::npos);
}
#endif  // CYBER_CAPI_HEADER_PATH

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

// Regression: toGuidance allocates straight from the caller's counts, so a
// count a binding marshalled from a signed -1 (SIZE_MAX here) threw
// std::length_error out of an extern "C" function and std::terminate took the
// host down. The ABI reports it as an argument error instead. Bounded by
// construction: the allocator refuses the request without reserving anything.
TEST_CASE("cyber_remesh_guided reports an impossible guidance count instead of aborting") {
    std::filesystem::path objPath;
    CyberMesh* input = makeCapiBox(objPath);
    REQUIRE(input != nullptr);
    CyberRemeshParams params{};
    cyber_default_params(&params);
    params.targetQuads = 200;

    const float density[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    CyberGuidance guidance{};
    guidance.vertex_density = density;
    guidance.vertex_density_count = std::numeric_limits<size_t>::max();

    CyberMesh* out = reinterpret_cast<CyberMesh*>(0x1);  // must be overwritten with null
    CHECK(cyber_remesh_guided(input, &params, &guidance, nullptr, nullptr, nullptr, nullptr,
                              &out) == CYBER_ERR_INVALID_ARG);
    CHECK(out == nullptr);
    CHECK(std::string(cyber_last_error()).find("cyber_remesh_guided") != std::string::npos);

    // Same trap one level down, in a flow guide's point count.
    const float points[6] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    CyberFlowGuide guide{};
    guide.points = points;
    guide.point_count = std::numeric_limits<size_t>::max();
    guide.strength = 1.0f;
    guide.radius = 0.5f;
    CyberGuidance pointy{};
    pointy.guides = &guide;
    pointy.guide_count = 1;
    out = reinterpret_cast<CyberMesh*>(0x1);
    CHECK(cyber_remesh_guided(input, &params, &pointy, nullptr, nullptr, nullptr, nullptr, &out) ==
          CYBER_ERR_INVALID_ARG);
    CHECK(out == nullptr);

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

// Regression: cyber_conform moves every live vertex but ran outside
// runMeshEdit, so it never invalidated the render cache. A renderer that
// correctly re-fetched the zero-copy views after the call still drew the
// un-conformed mesh, and never self-corrected until some unrelated
// cyber_retopo_* op happened to run.
TEST_CASE("capi conform invalidates the zero-copy render views") {
    CyberMesh* target = makeGridMesh(3, 3, 1.0f);
    CyberMesh* edit = makeGridMesh(3, 3, 0.0f);

    // Build the cache BEFORE the conform, the way a renderer would.
    size_t count = 0;
    const float* stale = cyber_mesh_positions_ptr(edit, &count);
    REQUIRE(stale != nullptr);
    REQUIRE(count == cyber_mesh_vertex_count(edit) * 3u);

    CyberConformReport report{};
    REQUIRE(cyber_conform(edit, target, /*threshold=*/0.1f, &report, nullptr, 0) == CYBER_OK);
    REQUIRE(report.movedVertices > 0u);

    const float* fresh = cyber_mesh_positions_ptr(edit, &count);
    REQUIRE(fresh != nullptr);
    const std::vector<float> rendered(fresh, fresh + count);
    REQUIRE(rendered == positionsOf(edit));  // the views agree with the geometry
    for (size_t i = 2; i < rendered.size(); i += 3) {
        CHECK(rendered[i] == doctest::Approx(1.0f));
    }

    // The null-argument contract is unchanged.
    REQUIRE(cyber_conform(nullptr, target, 0.1f, &report, nullptr, 0) == CYBER_ERR_INVALID_ARG);
    REQUIRE(cyber_conform(edit, nullptr, 0.1f, &report, nullptr, 0) == CYBER_ERR_INVALID_ARG);

    cyber_mesh_free(edit);
    cyber_mesh_free(target);
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

// Regression: presetMapFileName signals a name that would leave the output
// directory with an EMPTY string (tests/core/test_export_preset.cpp), and the C
// entry point forwarded it verbatim — a caller checking the documented NULL saw
// success and joined "" onto its output directory.
TEST_CASE("capi reports a refused export map name as NULL, not an empty string") {
    CyberExportPreset* preset = nullptr;
    REQUIRE(cyber_export_preset_resolve("blender", &preset) == CYBER_OK);

    const char* good = cyber_export_preset_map_file_name(preset, 0, "hero");
    REQUIRE(good != nullptr);
    CHECK(std::string(good) == "hero_normal.png");
    CHECK(std::string(cyber_last_error()).empty());

    for (const char* escaping : {"../../hero", "/tmp/hero"}) {
        CHECK(cyber_export_preset_map_file_name(preset, 0, escaping) == nullptr);
        CHECK(std::string(cyber_last_error()).find("outside the output directory") !=
              std::string::npos);
    }

    CHECK(cyber_export_preset_map_file_name(preset, 9999u, "hero") == nullptr);
    CHECK(cyber_export_preset_map_file_name(nullptr, 0, "hero") == nullptr);

    cyber_export_preset_free(preset);
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

// ---- pinned weighted transform ---------------------------------------------

// Regression (soft-selection honesty pass, item 2): cyber_capi.h promised "any
// pinned vertex is not moved" for the weighted transform, but the entry point
// hard-coded a null PinSet, so a HIGH-weight pinned vertex moved with the rest.
// Only _relax honoured pins. The fix is the additive
// cyber_retopo_selection_transform_pinned; _transform is that call with an
// empty list.
TEST_CASE("capi weighted transform honours a pin on a high-weight vertex") {
    CyberMesh* edit = makeGridMesh(5, 5, 0.0f);
    const size_t vertices = cyber_mesh_vertex_count(edit);
    REQUIRE(vertices == 25u);

    // Weight 1 everywhere: weight cannot be what holds the pinned vertex still.
    const std::vector<float> full(vertices, 1.0f);
    REQUIRE(cyber_retopo_selection_set_weights(edit, full.data(), full.size()) == CYBER_OK);
    const std::vector<float> before = positionsOf(edit);

    // A translation, so every vertex has somewhere to go.
    const float xf[12] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 3.0f, 0.0f, 0.0f};
    const uint32_t pinned[2] = {0u, 7u};
    CyberSoftTransformReport report{};
    REQUIRE(cyber_retopo_selection_transform_pinned(edit, xf, pinned, 2, nullptr, 0.0f, &report) ==
            CYBER_OK);

    // The pinned vertices are excluded from `moved` and are bit-identical.
    CHECK(report.moved == vertices - 2u);
    const std::vector<float> after = positionsOf(edit);
    REQUIRE(after.size() == before.size());
    for (uint32_t v : pinned) {
        for (size_t axis = 0; axis < 3; ++axis) {
            const size_t i = static_cast<size_t>(v) * 3u + axis;
            CHECK(after[i] == before[i]);
        }
    }
    // Everything else did move by the full translation.
    for (size_t v = 0; v < vertices; ++v) {
        if (v == 0u || v == 7u) {
            continue;
        }
        CHECK(after[v * 3u] == doctest::Approx(before[v * 3u] + 3.0f));
    }

    // The unpinned spelling is the same call with an empty list.
    CyberMesh* plain = makeGridMesh(5, 5, 0.0f);
    REQUIRE(cyber_retopo_selection_set_weights(plain, full.data(), full.size()) == CYBER_OK);
    CyberSoftTransformReport plainReport{};
    REQUIRE(cyber_retopo_selection_transform(plain, xf, nullptr, 0.0f, &plainReport) == CYBER_OK);
    CHECK(plainReport.moved == vertices);

    // Argument checks: a null pin list with a non-zero count is rejected and
    // leaves the mesh alone.
    const std::vector<float> untouched = positionsOf(plain);
    REQUIRE(cyber_retopo_selection_transform_pinned(plain, xf, nullptr, 4, nullptr, 0.0f,
                                                    nullptr) == CYBER_ERR_INVALID_ARG);
    REQUIRE(cyber_retopo_selection_transform_pinned(plain, nullptr, pinned, 2, nullptr, 0.0f,
                                                    nullptr) == CYBER_ERR_INVALID_ARG);
    CHECK(positionsOf(plain) == untouched);

    cyber_mesh_free(plain);
    cyber_mesh_free(edit);
}

// Regression: the three transform entry points took the 12-float affine on
// trust. A single NaN in it (a ray-miss unprojection, a gizmo divided by a
// zero-length drag) reached EVERY vertex it was applied to and the call still
// returned CYBER_OK — and a NaN position is terminal: no later edit restores
// it, cyber_mesh_validate still reports the mesh well-formed, and the mesh
// saves to disk that way. cyber_retopo_selection_relax already refused a NaN
// `strength` on exactly these grounds; the transforms now match it.
TEST_CASE("capi transforms refuse a non-finite affine instead of writing NaN positions") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    // Component 9 is the translation x; component 0 is a rotation term. Both
    // reach every vertex, so both must be refused.
    float xf[12] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 3.0f, 0.0f, 0.0f};

    for (const int component : {0, 9}) {
        for (const float poison : {nan, inf}) {
            CyberMesh* edit = makeGridMesh(4, 4, 0.0f);
            const size_t vertices = cyber_mesh_vertex_count(edit);
            const std::vector<float> full(vertices, 1.0f);
            REQUIRE(cyber_retopo_selection_set_weights(edit, full.data(), full.size()) == CYBER_OK);
            const std::vector<float> before = positionsOf(edit);

            float bad[12];
            std::copy(std::begin(xf), std::end(xf), std::begin(bad));
            bad[component] = poison;

            const uint32_t all[1] = {0u};
            CHECK(cyber_retopo_selection_transform(edit, bad, nullptr, 0.0f, nullptr) ==
                  CYBER_ERR_INVALID_PARAM);
            CHECK(cyber_retopo_selection_transform_pinned(edit, bad, nullptr, 0, nullptr, 0.0f,
                                                          nullptr) == CYBER_ERR_INVALID_PARAM);
            CHECK(cyber_retopo_transform_vertices(edit, all, 1, bad, nullptr, 0.0f, nullptr,
                                                  nullptr) == CYBER_ERR_INVALID_PARAM);
            // Mesh unchanged, bit for bit.
            CHECK(positionsOf(edit) == before);
            cyber_mesh_free(edit);
        }
    }

    // The same call with a finite affine still works, so the gate is not a
    // blanket refusal.
    CyberMesh* edit = makeGridMesh(4, 4, 0.0f);
    const size_t vertices = cyber_mesh_vertex_count(edit);
    const std::vector<float> full(vertices, 1.0f);
    REQUIRE(cyber_retopo_selection_set_weights(edit, full.data(), full.size()) == CYBER_OK);
    CyberSoftTransformReport report{};
    REQUIRE(cyber_retopo_selection_transform(edit, xf, nullptr, 0.0f, &report) == CYBER_OK);
    CHECK(report.moved == vertices);
    cyber_mesh_free(edit);
}

// Documents the `moved` vs `resnapped` gap the 16_soft_selection figure showed
// (193 moved, 180 re-snapped): `moved` counts WRITES, not displacements. A
// vertex whose weight is positive but negligible blends to a target that rounds
// to its current position, so the op writes the same value back and the Target
// re-projection has nothing to correct.
TEST_CASE("capi weighted transform counts a negligible-weight vertex as moved") {
    CyberMesh* target = makeGridMesh(5, 5, 0.0f);
    CyberSnapper* snapper = nullptr;
    REQUIRE(cyber_snapper_create(target, &snapper) == CYBER_OK);

    CyberMesh* edit = makeGridMesh(5, 5, 0.0f);  // already on the Target
    const size_t vertices = cyber_mesh_vertex_count(edit);
    std::vector<float> weights(vertices, 0.0f);
    weights[6] = 1.0f;    // a real edit
    weights[7] = 1e-34f;  // positive, but far below float resolution here
    REQUIRE(cyber_retopo_selection_set_weights(edit, weights.data(), weights.size()) == CYBER_OK);

    const std::vector<float> before = positionsOf(edit);
    const float xf[12] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    CyberSoftTransformReport report{};
    REQUIRE(cyber_retopo_selection_transform(edit, xf, snapper, 0.0f, &report) == CYBER_OK);

    // Both weighted vertices are in `moved`; only the real edit needed pulling
    // back onto the Target.
    CHECK(report.moved == 2u);
    CHECK(report.resnapped == 1u);

    // ...and the negligible-weight vertex is bit-identical anyway.
    const std::vector<float> after = positionsOf(edit);
    for (size_t axis = 0; axis < 3; ++axis) {
        CHECK(after[7u * 3u + axis] == before[7u * 3u + axis]);
    }

    cyber_snapper_free(snapper);
    cyber_mesh_free(edit);
    cyber_mesh_free(target);
}

#ifdef CYBER_TESTS_HAVE_APP
// ---- document persistence for named selection slots ------------------------

// Regression (soft-selection honesty pass, item 1): the mesh handle owned the
// slot map and cyber::app::Document owned a serializer for one, and NOTHING in
// the tree moved weights between them. A slot saved through the ABI was absent
// from the saved document and a slot in a loaded document was invisible to the
// ABI. This walks the whole public path: save a weighted selection through the
// C ABI, serialize the document, load it back, read the weights out again.
TEST_CASE("capi document round-trips named soft-selection slots end to end") {
    CyberMesh* edit = makeGridMesh(5, 4, 0.0f);
    const size_t vertices = cyber_mesh_vertex_count(edit);
    const float centre[3] = {2.0f, 1.5f, 0.0f};
    REQUIRE(cyber_retopo_selection_sphere(edit, centre, 2.0f, CYBER_FALLOFF_SMOOTH) == CYBER_OK);
    REQUIRE(cyber_retopo_selection_save(edit, "taper") == CYBER_OK);
    REQUIRE(cyber_retopo_selection_smooth(edit, 3) == CYBER_OK);
    REQUIRE(cyber_retopo_selection_save(edit, "soft-taper") == CYBER_OK);

    std::vector<float> saved(cyber_retopo_selection_copy_weights(edit, nullptr, 0));
    cyber_retopo_selection_copy_weights(edit, saved.data(), saved.size());
    REQUIRE(saved.size() >= vertices);
    const std::vector<float> editPositions = positionsOf(edit);

    CyberMesh* target = makeGridMesh(9, 9, 0.0f);
    CyberDocument* doc = cyber_document_create();
    REQUIRE(doc != nullptr);
    REQUIRE(cyber_document_set_target(doc, target) == CYBER_OK);
    REQUIRE(cyber_document_set_edit_mesh(doc, edit) == CYBER_OK);
    // The slots crossed the seam with the mesh.
    REQUIRE(cyber_document_slot_count(doc) == 2u);
    REQUIRE(std::string(cyber_document_slot_name(doc, 1)) == "taper");

    // Serialize, then drop every live handle: nothing but the bytes survives.
    std::vector<uint8_t> bytes(cyber_document_save(doc, nullptr, 0));
    REQUIRE(!bytes.empty());
    REQUIRE(cyber_document_save(doc, bytes.data(), bytes.size()) == bytes.size());
    cyber_document_free(doc);
    cyber_mesh_free(edit);
    cyber_mesh_free(target);

    CyberDocument* loaded = cyber_document_load(bytes.data(), bytes.size());
    REQUIRE(loaded != nullptr);
    REQUIRE(cyber_document_slot_count(loaded) == 2u);
    REQUIRE(std::string(cyber_document_slot_name(loaded, 0)) == "soft-taper");
    REQUIRE(std::string(cyber_document_slot_name(loaded, 1)) == "taper");

    // Read the weights straight off the document...
    std::vector<float> fromDoc(cyber_document_slot_weights(loaded, "soft-taper", nullptr, 0));
    REQUIRE(fromDoc.size() == saved.size());
    cyber_document_slot_weights(loaded, "soft-taper", fromDoc.data(), fromDoc.size());
    CHECK(fromDoc == saved);
    CHECK(cyber_document_slot_weights(loaded, "no-such-slot", nullptr, 0) == 0u);

    // ...and through a mesh handle, which is what an editing session does.
    CyberMesh* restored = cyber_document_edit_mesh(loaded);
    REQUIRE(restored != nullptr);
    CHECK(cyber_mesh_vertex_count(restored) == vertices);
    CHECK(positionsOf(restored) == editPositions);
    REQUIRE(cyber_retopo_selection_slot_count(restored) == 2u);
    REQUIRE(cyber_retopo_selection_load(restored, "soft-taper") == CYBER_OK);
    std::vector<float> reread(cyber_retopo_selection_copy_weights(restored, nullptr, 0));
    cyber_retopo_selection_copy_weights(restored, reread.data(), reread.size());
    CHECK(reread == saved);

    CyberMesh* restoredTarget = cyber_document_target(loaded);
    REQUIRE(restoredTarget != nullptr);
    CHECK(cyber_mesh_vertex_count(restoredTarget) == 81u);
    // Only the EditMesh carries slots: the weights are indexed against it.
    CHECK(cyber_retopo_selection_slot_count(restoredTarget) == 0u);

    cyber_mesh_free(restoredTarget);
    cyber_mesh_free(restored);
    cyber_document_free(loaded);
}

TEST_CASE("capi document survives a file round trip and rejects a foreign buffer") {
    CyberMesh* edit = makeGridMesh(4, 4, 0.0f);
    const float centre[3] = {1.5f, 1.5f, 0.0f};
    REQUIRE(cyber_retopo_selection_sphere(edit, centre, 1.5f, CYBER_FALLOFF_LINEAR) == CYBER_OK);
    REQUIRE(cyber_retopo_selection_save(edit, "cap") == CYBER_OK);
    std::vector<float> saved(cyber_retopo_selection_copy_weights(edit, nullptr, 0));
    cyber_retopo_selection_copy_weights(edit, saved.data(), saved.size());

    CyberDocument* doc = cyber_document_create();
    REQUIRE(doc != nullptr);
    REQUIRE(cyber_document_set_edit_mesh(doc, edit) == CYBER_OK);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "cyber_capi_document.cydc";
    REQUIRE(cyber_document_save_file(doc, path.string().c_str()) == CYBER_OK);
    cyber_document_free(doc);
    cyber_mesh_free(edit);

    CyberDocument* loaded = cyber_document_load_file(path.string().c_str());
    REQUIRE(loaded != nullptr);
    std::vector<float> fromFile(cyber_document_slot_weights(loaded, "cap", nullptr, 0));
    REQUIRE(fromFile.size() == saved.size());
    cyber_document_slot_weights(loaded, "cap", fromFile.data(), fromFile.size());
    CHECK(fromFile == saved);
    cyber_document_free(loaded);

    // Clearing the edit mesh drops the slots that were indexed against it.
    CyberDocument* cleared = cyber_document_load_file(path.string().c_str());
    REQUIRE(cleared != nullptr);
    REQUIRE(cyber_document_set_edit_mesh(cleared, nullptr) == CYBER_OK);
    CHECK(cyber_document_slot_count(cleared) == 0u);
    cyber_document_free(cleared);

    const uint8_t garbage[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(cyber_document_load(garbage, sizeof(garbage)) == nullptr);
    CHECK(cyber_document_load_file("/definitely/not/a/document.cydc") == nullptr);

    // The overflow case, driven through the ABI a host actually calls rather
    // than through Document::load directly (the engine-side case lives in
    // tests/app/test_app.cpp). `cursor + length` used to wrap to 0 and pass the
    // bounds check, so the reader walked off the end of the caller's buffer:
    // an ASan heap-buffer-overflow reachable from any host that opens an
    // untrusted .cydc.
    std::vector<uint8_t> forged;
    const auto appendU32 = [&forged](uint32_t v) {
        for (size_t i = 0; i < 4; ++i) {
            forged.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFFu));
        }
    };
    // kMagic ("CYDC"), kFormatVersion, one section, SectionId::EditMesh.
    for (const uint32_t word : {0x43594443u, 1u, 1u, 2u}) {
        appendU32(word);
    }
    appendU32(0xFFFFFFE8u);  // low half of 2^64 - 24: cursor is at byte 24
    appendU32(0xFFFFFFFFu);  // high half
    appendU32(1000u);        // the vertexCount an accepted span would have read
    REQUIRE(forged.size() == 28u);
    CHECK(cyber_document_load(forged.data(), forged.size()) == nullptr);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// Regression: the stream state was tested while the bytes were still in the
// filebuf, so a device that rejects the write at flush time (a full disk, an
// over-quota mount) reported CYBER_OK for a file that never landed — and the
// shell cleared its unsaved-changes indicator over lost work.
TEST_CASE("capi document save reports a write the device never accepted") {
    // /dev/full accepts the open and swallows small writes into the buffer;
    // the ENOSPC only surfaces when that buffer is flushed.
    const std::filesystem::path full("/dev/full");
    if (!std::filesystem::exists(full)) {
        MESSAGE("/dev/full unavailable on this platform");
        return;
    }
    CyberMesh* edit = makeGridMesh(4, 4, 0.0f);
    CyberDocument* doc = cyber_document_create();
    REQUIRE(doc != nullptr);
    REQUIRE(cyber_document_set_edit_mesh(doc, edit) == CYBER_OK);

    CHECK(cyber_document_save_file(doc, "/dev/full") == CYBER_ERR_IO);
    CHECK(std::string(cyber_last_error()).find("/dev/full") != std::string::npos);

    // A writable path still succeeds, byte for byte.
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "cyber_capi_full_probe.cydc";
    REQUIRE(cyber_document_save_file(doc, path.string().c_str()) == CYBER_OK);
    CHECK(std::filesystem::file_size(path) > 0u);

    cyber_document_free(doc);
    cyber_mesh_free(edit);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

#endif  // CYBER_TESTS_HAVE_APP
